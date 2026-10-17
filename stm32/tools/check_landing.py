#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""FreeRTOS 落地预检：把"启用 FreeRTOS 那天要确认的事"变成一条可执行的检查。

============================================================================
 为什么要有它
============================================================================

  落地条件分散在**三个地方**，而它们**互相之间的一致性**没人管：

      ① 仓库代码        （port/FreeRTOSConfig.h、gw_rtos.c、gw_sched.c、模板 …）
      ② CubeMX 工程     （.ioc 设置、Core/Src/*.c、有没有内核源码）
      ③ IDE 预处理宏    （.cproject：GW_USE_FREERTOS / GW_HAL_HAS_UARTEX_IDLE）

  ⚠️ 本阶段已经在这些交界处踩到 7 个问题（O~V），而且**大多数只在链接或上板
     那一刻才暴露**。这个脚本把能静态核对的都核一遍。

============================================================================
 三种状态（只有"矛盾"会让脚本非零退出）
============================================================================

    [ OK ]   已经对了
    [待办]   还没做 —— 这是**当前预期状态**（FreeRTOS 还没启用），不算失败
    [矛盾]   两处状态互相冲突 —— **唯一**会非零退出的情况
             例如："工程里已经有内核源码，但 it.c 里三个 handler 还在"
                   -> 必然 multiple definition（缺陷 V）
                   "定义了 GW_USE_FREERTOS 却没有内核源码"
                   -> 必然缺 FreeRTOS.h / xTaskCreate

  ⚠️ 这样设计是因为：预检要能**随时跑**（CI 里也跑），
     而不能要求"你必须已经落地完成"。

============================================================================
 用法
============================================================================

    python tools/check_landing.py
    python tools/check_landing.py --project D:\\stm32cubeIDE\\gateway2

  工程目录默认取环境变量 GW_CUBE_PROJECT，否则 D:\\stm32cubeIDE\\gateway2。
  找不到工程就打印"跳过"并返回 0（CI 在别的机器上也能跑）。
============================================================================
"""
import argparse
import io
import os
import re
import sys

# ⚠️ 编码守卫（本文件的 print 里有 ⚠️/✅；裸 GBK 控制台不加守卫会 UnicodeEncodeError）
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(errors="replace")
    except (AttributeError, ValueError):
        pass

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

HANDLERS = ("SVC_Handler", "PendSV_Handler", "SysTick_Handler")

OK, TODO, BAD = "OK", "待办", "矛盾"


def read(path):
    if not os.path.isfile(path):
        return None
    return io.open(path, encoding="utf-8", errors="replace").read()


def find(prj, *rel):
    for r in rel:
        p = os.path.join(prj, r)
        if os.path.isfile(p):
            return p
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--project",
                    default=os.environ.get("GW_CUBE_PROJECT",
                                           r"D:\stm32cubeIDE\gateway2"),
                    help="CubeMX 工程目录")
    a = ap.parse_args()
    prj = a.project

    print("=" * 70)
    print(" FreeRTOS 落地预检")
    print("   仓库 : %s" % REPO)
    print("   工程 : %s" % prj)
    print("=" * 70)

    if not os.path.isfile(os.path.join(prj, "Core", "Src", "main.c")):
        print("  [跳过] 找不到 CubeMX 工程（Core/Src/main.c 不存在）")
        print("         —— 这是允许的：预检要能在没有工程的机器上跑。")
        return 0

    rows = []          # (状态, 项目, 说明)

    ioc = read(find(prj, "gateway2.ioc", "*.ioc")) or ""
    cproj = read(find(prj, ".cproject")) or ""
    it_c = read(os.path.join(prj, "Core", "Src", "stm32f1xx_it.c")) or ""
    main_c = read(os.path.join(prj, "Core", "Src", "main.c")) or ""

    # ---- 1) 时基不能是 SysTick（判据，不是具体值）----
    mtb = re.search(r"^NVIC\.TimeBaseIP=(\S+)", ioc, re.M)
    if mtb is None:
        rows.append((TODO, "HAL 时基", "在 .ioc 里读不到 NVIC.TimeBaseIP（CubeMX 里确认一下）"))
    elif "SYSTICK" in mtb.group(1).upper():
        rows.append((BAD, "HAL 时基",
                     " .ioc 里是 %s —— 必须是某个 TIM（SysTick 要留给内核）" % mtb.group(1)))
    else:
        rows.append((OK, "HAL 时基", " .ioc: NVIC.TimeBaseIP=%s（不是 SysTick ✓）" % mtb.group(1)))

    # 时基回调必须与 .ioc 的一致（两份不一致 -> uwTick 不增长，静默）
    for tim in ("TIM4", "TIM1", "TIM2", "TIM3"):
        if "if (htim->Instance == %s)" % tim in main_c:
            if mtb is not None and tim != mtb.group(1):
                rows.append((BAD, "时基回调",
                             " main.c 里是 %s 而 .ioc 是 %s —— 两边不一致，uwTick 不会增长"
                             % (tim, mtb.group(1))))
            else:
                rows.append((OK, "时基回调", " main.c: HAL_TIM_PeriodElapsedCallback 用 %s ✓" % tim))
            break

    # ---- 2) FreeRTOS 是否已在工程里启用 ----
    kern = os.path.join(prj, "Middlewares", "Third_Party", "FreeRTOS", "Source", "tasks.c")
    kern_on = os.path.isfile(kern)
    ioc_rtos = bool(re.search(r"^FREERTOS\.", ioc, re.M))
    if kern_on or ioc_rtos:
        rows.append((OK, "内核源码", " 已加入工程（%s）" % ("Middlewares/..." if kern_on else ".ioc 里有 FREERTOS.")))
    else:
        rows.append((TODO, "内核源码", " 还没加入 —— 按迁移指南第 3 步（CubeMX 勾 FREERTOS / CMSIS_V1）"))

    # ---- 3) 三个 handler 不能与内核重名（★ 缺陷 V：只在链接期暴露）----
    present = [h for h in HANDLERS if re.search(r"^\s*void\s+%s\s*\(" % h, it_c, re.M)]
    if present and (kern_on or ioc_rtos):
        rows.append((BAD, "三个 handler",
                     " it.c 里还有 %s —— 与内核重名 -> **multiple definition**（缺陷 V）"
                     % "/".join(present)))
    elif present:
        rows.append((TODO, "三个 handler",
                     " it.c 里还有 %s（现在是裸机，正常）—— 启用 FreeRTOS 时 CubeMX 会自动删掉；"
                     "手工改的工程要自己删（只删这三个）" % "/".join(present)))
    else:
        rows.append((OK, "三个 handler", " it.c 里已经没有 SVC/PendSV/SysTick ✓"))

    # ---- 4) GW_USE_FREERTOS（IDE 宏或 gw_config.h）----
    use_rtos = bool(re.search(r'value="GW_USE_FREERTOS=1"', cproj)) or \
               bool(re.search(r"#define\s+GW_USE_FREERTOS\s+1", cproj))
    cfg = read(os.path.join(prj, "Core", "Inc", "gw_config.h")) or ""
    if re.search(r"#define\s+GW_USE_FREERTOS\s+1", cfg):
        use_rtos = True
    if use_rtos and not kern_on:
        rows.append((BAD, "GW_USE_FREERTOS",
                     " 已定义 =1，但工程里**没有内核源码** -> 链接会缺 FreeRTOS.h / xTaskCreate"))
    elif use_rtos:
        rows.append((OK, "GW_USE_FREERTOS", " 已定义 =1（模板会走 RTOS 分支）"))
    elif kern_on or ioc_rtos:
        rows.append((BAD, "GW_USE_FREERTOS",
                     " 内核已加进来，但**没有定义 GW_USE_FREERTOS=1** -> 仍会跑裸机主循环，"
                     "加了内核等于白加"))
    else:
        rows.append((TODO, "GW_USE_FREERTOS",
                     " 还没定义 —— 落地时在 .cproject 的预处理器里加 GW_USE_FREERTOS=1"))

    # ---- 5) main() 是否已切到 RTOS 分支 ----
    landed = "gw_rtos_create_tasks" in main_c
    if landed:
        if "gw_rtos_start" not in main_c:
            rows.append((BAD, "main() 落地", " 调了 create_tasks 但没调 gw_rtos_start"))
        else:
            rows.append((OK, "main() 落地", " main.c 已调 gw_rtos_create_tasks + gw_rtos_start ✓"))
    else:
        rows.append((TODO, "main() 落地",
                     " main.c 还没切到 RTOS 分支（按迁移指南第 6 步：加宏 + 合并模板那段）"))

    # ---- 5b) ★ 内务与"只委托"结构（缺陷 W 的落地版检查，2026-10-11 加）----
    #
    # ⚠️ 为什么要查工程这份 main.c：**仓库模板与工程 main.c 会各自漂移** ——
    #    实测（2026-10-11）工程那份还是"W 修复之前"的版本（没有 app_housekeeping），
    #    也就是说"在工程里定义 GW_USE_FREERTOS=1"**并不够**：
    #    内务（日志节拍、OTA 传输/确认、上报节拍、波特率切换、重启挂起）
    #    会整批不执行 —— 正是缺陷 W 在工程侧的翻版。
    #
    # ⚠️ 工程 main.c 里**不要**照抄模板整个文件：模板不含 CubeMX 生成的
    #    `SystemClock_Config()` / `Error_Handler()` / `assert_failed()`
    #    （它们在工程 main.c 里），照抄会缺定义。
    #    正确做法是**合并**：把 `app_housekeeping()` 与 RTOS 分支搬进去。
    has_hk = "app_housekeeping" in main_c
    has_hk_hook = "gw_rtos_housekeeping" in main_c
    if use_rtos and not (has_hk and has_hk_hook):
        missing = []
        if not has_hk:
            missing.append("app_housekeeping()")
        if not has_hk_hook:
            missing.append("gw_rtos_housekeeping() 强定义")
        rows.append((BAD, "内务落地",
                     " 定义了 GW_USE_FREERTOS=1，但 main.c 里缺 %s"
                     " -> **日志节拍/OTA 传输与确认/上报节拍/波特率切换全都不执行**"
                     "（缺陷 W 的翻版）" % "、".join(missing)))
    elif has_hk and has_hk_hook:
        rows.append((OK, "内务落地", " main.c 里有 app_housekeeping() 与 RTOS 侧强定义 ✓"))
    else:
        rows.append((TODO, "内务落地",
                     " main.c 里还没有 app_housekeeping()（裸机主循环那部分内务）"
                     "—— 落地时从 templates/main_template.c 合并过去"))

    # 裸机主循环必须"只委托"（与仓库侧 CI 检查**同一条规则、同一个锚点**）
    #
    # ⚠️ 锚点必须用 `#else /* ---- 14) 裸机主循环` 那一段，不能简单 find("for (;;)")：
    #    2026-10-11 在合成工程上做落地预演时，那个写法把**循环之前的初始化调用**
    #    （gw_rtos_create_tasks / gw_sched_init / gw_timing_init ...）也算成了
    #    "循环里的杂事"，报了一次**假矛盾**。假警报比没有检查更坏：会让人开始
    #    忽略这条检查。
    if "app_housekeeping" in main_c:
        m_loop = re.search(r"#else\s*/\*[^\n]*裸机主循环[^\n]*\n(.*?)\n#endif",
                           main_c, re.S)
        if m_loop is None:
            rows.append((TODO, "主循环结构",
                         " 找不到裸机主循环那一段（结构变了就同步本检查）"))
        else:
            seg = m_loop.group(1)
            # 去掉注释，避免注释里提到的函数名被当成调用
            seg = re.sub(r"/\*.*?\*/", " ", seg, flags=re.S)
            calls = set(re.findall(r"\b((?:gw|HAL|app)_\w+)\s*\(", seg))
            allowed = {"gw_app_poll", "app_housekeeping", "HAL_GetTick", "HAL_Delay"}
            stray = sorted(calls - allowed)
            if stray:
                rows.append((BAD, "主循环结构",
                             " 裸机主循环里出现了没走共用内务的调用：%s"
                             " -> RTOS 分支没有这个循环，它们在 RTOS 下会整批消失（缺陷 W）"
                             % ", ".join(stray)))
            elif "app_housekeeping" not in calls:
                rows.append((BAD, "主循环结构", " 裸机主循环没有调用 app_housekeeping()"))
            else:
                rows.append((OK, "主循环结构", " 裸机主循环只委托（分派 + 共用内务 + 延时）✓"))

    # ---- 5c) ★ 工程 main.c 是哪个"变体"（2026-10-11 实测发现的重要事实）----
    #
    # ⚠️ 本仓库有**两个** main 模板：
    #      templates/main_template.c      生产版（有 OTA 桥、上报节拍、RTOS 分支）
    #      templates/main_L0_bringup.c    L0 上板验证版（有 L0 自检/回环/诊断寄存器）
    #
    # ⚠️ 实测（2026-10-11）：**工程的 main.c 是 L0 版衍生物**，不是生产版 ——
    #    证据：① 与两个模板逐行比对，相同行 L0 416 / 生产 111；
    #          ② L0 标记（GW_L0_IWDG_ENABLE / GW_L0_CAN_LOOPBACK_PERIOD_S /
    #             can_port_loopback_test）都在，生产版的标记（gw_ota_bridge_t g_ota /
    #             app_housekeeping / gw_rtos_create_tasks）一个都没有。
    #
    # ⚠️ 这件事**必须在上 RTOS 之前决定**，否则落地说明会指错文件：
    #      · 走**生产版**：把 main.c 换成 templates/main_template.c 那套
    #        （生产设计：OTA 桥 + 上报节拍 + RTOS 分支都齐），L0 脚手架退场；
    #      · 走 **L0 版**：那"合并 app_housekeeping"这套说明**不适用**
    #        （L0 版结构不同，且它根本没有生产版的 OTA 桥那一段）。
    L0_MARKS = ["GW_L0_IWDG_ENABLE", "GW_L0_CAN_LOOPBACK_PERIOD_S",
                "can_port_loopback_test"]
    PROD_MARKS = ["gw_ota_bridge_t g_ota", "app_housekeeping",
                  "gw_rtos_create_tasks"]
    n_l0 = sum(1 for m in L0_MARKS if m in main_c)
    n_prod = sum(1 for m in PROD_MARKS if m in main_c)
    is_l0 = (n_l0 >= 2) and (n_l0 > n_prod)
    is_prod = n_prod >= 2

    if is_l0:
        if use_rtos:
            rows.append((BAD, "main 变体",
                         " 工程 main.c 是 **L0 上板验证版**（L0 标记 %d/%d，生产标记 %d/%d），"
                         "却已经定义了 GW_USE_FREERTOS=1 —— 落地说明里"
                         "\"合并 app_housekeeping + OTA 桥\"那套**按 L0 版的结构根本不适用**。"
                         "**先决定**：换成生产版 main（推荐）还是在 L0 版上另做一套"
                         % (n_l0, len(L0_MARKS), n_prod, len(PROD_MARKS))))
        else:
            rows.append((TODO, "main 变体",
                         " 工程 main.c 是 **L0 上板验证版**（L0 标记 %d/%d，生产标记 %d/%d）"
                         "—— 与仓库 templates/main_template.c（生产版：OTA 桥 + 上报节拍 + "
                         "RTOS 分支）**不是同一支**。落地前先决定换哪支（推荐换生产版）"
                         % (n_l0, len(L0_MARKS), n_prod, len(PROD_MARKS))))
    elif is_prod:
        rows.append((OK, "main 变体",
                     " 工程 main.c 与生产版模板同支（生产标记 %d/%d）✓"
                     % (n_prod, len(PROD_MARKS))))
    else:
        # ★ 三态里的第三种：**既不是 L0、也不是生产版**（2026-10-11 第 25 轮补）
        #
        # ⚠️ 为什么必须单列：CubeMX「生成代码」会**重写 main.c**，只保留 `USER CODE` 区域。
        #    而本工程的 main.c 在 `main()` 里**一个 USER CODE 标记都没有**（最早的在 1127 行，
        #    `int main(void)` 在 515 行）—— 也就是说重新生成会把 `main()` 整段换成
        #    CubeMX 的骨架：**编译能过、烧进去什么都不做**。
        #
        # ⚠️ 第一版检查在这里会误报 `[OK] 与生产版同支`（因为"L0 标记 > 生产标记"两边都是 0）
        #    —— 恰恰把该抓的最坏状态报成了正常。所以改成三态，并给出明确的补救动作。
        msg = (" 工程 main.c **既不是 L0 版、也不是生产版**（L0 标记 %d/%d，生产标记 %d/%d）"
               "—— 典型是 CubeMX「生成代码」把 `main()` 整段重写成了骨架。"
               "补救：`python tools\\make_landing_main.py --out build\\landing_main.c "
               "--verify-compile` 生成后覆盖回 `Core/Src/main.c`"
               % (n_l0, len(L0_MARKS), n_prod, len(PROD_MARKS)))
        rows.append((BAD if use_rtos else TODO, "main 变体", msg))

    # ---- 6) 工程的 FreeRTOSConfig.h 与仓库那份的差异（"逐项对比"机械化）----
    #
    # ⚠️ 这一项**不能一律报"待办"**（2026-10-11 改进）：
    #    CubeMX 生成/覆盖 FreeRTOSConfig.h 时会用它自己的默认值，而其中有几项
    #    一旦不一致就是**已经修过的缺陷回归**，且都只在运行时才炸：
    #      · 三个向量重定向宏（缺陷 R）：没有它们 -> 内核的 SVC/PendSV/SysTick
    #        接不上 -> **第一次任务切换就 HardFault**
    #      · 堆大小（缺陷 S）：默认远小于实测需要 -> **任务创建失败**
    #      · 空闲钩子（喂狗）：关了 -> **看门狗没人喂 -> 反复复位**
    #      · 互斥量支持：关了 -> Flash/日志互斥（缺陷 X/Y）失效
    #      · 抢占/时间片：改了 -> 前面那套并发审查的前提全变
    #    所以这几项**不一致 = 矛盾**；其余项不一致只是"需人工确认"。
    CRITICAL = [
        ("configUSE_PREEMPTION", "并发审查的前提（抢占式）"),
        ("configUSE_TIME_SLICING", "同优先级轮转"),
        ("configUSE_IDLE_HOOK", "★ 空闲钩子喂狗 —— 关了狗没人喂"),
        ("configTICK_RATE_HZ", "调度时基（1000Hz）"),
        ("configMAX_PRIORITIES", "任务优先级上限"),
        ("configMINIMAL_STACK_SIZE", "空闲任务栈（堆预算里算过）"),
        ("configUSE_MUTEXES", "★ Flash/日志互斥要用（缺陷 X/Y）"),
        ("configTOTAL_HEAP_SIZE", "★ 堆大小（缺陷 S：实测需要 10.7KB+）"),
        ("configCHECK_FOR_STACK_OVERFLOW", "栈溢出检查"),
        ("configUSE_MALLOC_FAILED_HOOK", "分配失败钩子"),
        ("configPRIO_BITS", "NVIC 分组（Group 4）"),
        # ---- 2026-10-11 第 30 轮补：这几项是**静默**失效，比崩溃更难查 ----
        ("configUSE_16_BIT_TICKS",
         "★ 必须是 0：=1 时 tick 只有 16 位，**每 65.5 秒回绕** "
         "-> 心跳判据、OTA 的 1 秒确认累加器全乱（不报错）"),
        ("configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY",
         "★ ISR 里调 ...FromISR 的边界：优先级比它低的中断**不许**用 FreeRTOS API，"
         "一致才安全（不一致是静默损坏）"),
        ("configLIBRARY_LOWEST_INTERRUPT_PRIORITY",
         "★ 与上一项配套（NVIC 分组 4 下是 15）"),
        ("configUSE_TIMERS",
         "★ 必须是 0：=1 会多出定时器任务+队列（约 1.2KB）"
         "-> 12KB 堆（余量 1.5KB）会被挤爆，任务创建失败"),
        ("configSUPPORT_DYNAMIC_ALLOCATION",
         "★ 必须是 1：我们是动态建任务（xTaskCreate）"),
        ("configUSE_TASK_NOTIFICATIONS",
         "任务通知（内核对每个 TCB 的开销随此项变化）"),
    ]
    REMAP = ["vPortSVCHandler", "xPortPendSVHandler", "xPortSysTickHandler"]

    gen = read(os.path.join(prj, "Core", "Inc", "FreeRTOSConfig.h"))
    if gen is None:
        rows.append((TODO, "FreeRTOSConfig.h",
                     " 工程里还没有（CubeMX 生成后**会覆盖/新建**它 —— 生成后必须"
                     "把 ★ 项照仓库那份改回来，否则缺陷 R/S 静默回归）"))
    else:
        want = read(os.path.join(REPO, "port", "FreeRTOSConfig.h")) or ""

        def defs(txt):
            d = {}
            for m in re.finditer(r"^#define\s+(config\w+)\s+(.+?)\s*$", txt, re.M):
                d[m.group(1)] = m.group(2)
            return d

        w, g = defs(want), defs(gen)

        # ★ 关键项：不一致就是缺陷回归
        crit_bad = [(k, why) for k, why in CRITICAL if w.get(k) != g.get(k)]
        # ★ 向量重定向：必须三条都在且正确
        remap_bad = []
        for r in REMAP:
            in_want = re.search(r"^#define\s+%s\s+(\w+)" % r, want, re.M)
            in_gen = re.search(r"^#define\s+%s\s+(\w+)" % r, gen, re.M)
            if in_want and (not in_gen or
                            in_gen.group(1) != in_want.group(1)):
                remap_bad.append(r)

        if crit_bad or remap_bad:
            detail = []
            if remap_bad:
                detail.append("缺/错**向量重定向宏**（缺陷 R）: %s" % ", ".join(remap_bad))
            if crit_bad:
                detail.append("★ 关键项不一致: "
                              + ", ".join("%s(%s)" % (k, why) for k, why in crit_bad[:5]))
            rows.append((BAD, "FreeRTOSConfig.h",
                         " 工程那份与仓库不一致 -> " + "；".join(detail)))
        else:
            diff = [k for k in sorted(set(w) | set(g)) if w.get(k) != g.get(k)]
            if not diff:
                rows.append((OK, "FreeRTOSConfig.h",
                             " 与仓库那份的 config* 项完全一致（含 ★ 关键项与向量重定向）✓"))
            else:
                rows.append((TODO, "FreeRTOSConfig.h",
                             " ★ 关键项都一致 ✓，另有 %d 项非关键差异需人工确认：%s"
                             % (len(diff), ", ".join(diff[:8])
                                + (" ..." if len(diff) > 8 else ""))))

    # ---- 6b) ★ CubeMX 生成的 freertos.c 会与我们抢钩子（2026-10-11 第 26 轮加）----
    #
    # ⚠️ CubeMX 启用 FreeRTOS 后会**新建 `Core/Src/freertos.c`**，并按配置开关在里面
    #    生成钩子函数：
    #       configUSE_IDLE_HOOK == 1              -> vApplicationIdleHook
    #       configCHECK_FOR_STACK_OVERFLOW        -> vApplicationStackOverflowHook
    #       configUSE_MALLOC_FAILED_HOOK == 1     -> vApplicationMallocFailedHook
    #    而这三项**我们的 port/FreeRTOSConfig.h 全开着**，这三个钩子**仓库自己都定义了**
    #    （在 port/gw_rtos.c 里，其中 IdleHook 就是喂狗的那条链）。
    #    -> 链接期 `multiple definition`（同缺陷 V 一类：生成器和我们抢符号）。
    #
    # ⚠️ 为什么上一轮的"落地预演"没抓到：预演是**手工拷贝内核源码**进去的，
    #    没有跑 CubeMX、也就没有 freertos.c。**这类坑只有真按 CubeMX 流程走才会出现。**
    #
    # ⚠️ 补救原则：**必须保留仓库版本**（尤其 IdleHook 与看门狗联动），
    #    把 freertos.c 里那几个钩子体删掉（或整个 freertos.c 不加入构建）。
    HOOKS = ["vApplicationIdleHook", "vApplicationStackOverflowHook",
             "vApplicationMallocFailedHook"]
    frt = os.path.join(prj, "Core", "Src", "freertos.c")
    if not os.path.isfile(frt):
        rows.append((TODO, "freertos.c 钩子",
                     " 工程里还没有 Core/Src/freertos.c（CubeMX 生成 FreeRTOS 代码时会建）"
                     "—— 到时它会按配置生成 %s，而仓库在 port/gw_rtos.c 里**已经定义了同样三个**"
                     " -> `multiple definition`。生成后把 freertos.c 里那几个钩子体删掉"
                     "（**保留仓库版本**：IdleHook 就是喂狗那条链）" % "/".join(HOOKS)))
    else:
        txt = read(frt) or ""
        dup = [h for h in HOOKS if re.search(r"\bvoid\s+" + h + r"\s*\(", txt)]
        if dup:
            rows.append((BAD, "freertos.c 钩子",
                         " Core/Src/freertos.c 里定义了 %s —— 与 port/gw_rtos.c **重复定义**"
                         " -> 链接期 multiple definition。**删掉 freertos.c 里这几个钩子体**"
                         "（必须保留仓库版本：vApplicationIdleHook 就是喂狗那条链，"
                         "删了它就是缺陷 O/U 的复活）" % ", ".join(dup)))
        else:
            rows.append((OK, "freertos.c 钩子",
                         " 有 freertos.c 但没有与仓库重复的钩子 ✓"))
        # ⚠️ 只能看**调用**，不能看子串：CubeMX 骨架会把 `void MX_FREERTOS_Init(void);`
        #    的原型搬进 main.c，用子串判断会误报成"调了它"（2026-10-11 实测）。
        if re.search(r"^\s*MX_FREERTOS_Init\s*\(\s*\)\s*;", main_c, re.M):
            rows.append((TODO, "freertos.c 调用",
                         " main.c **调了** MX_FREERTOS_Init() —— 那是 CubeMX 的默认任务创建函数，"
                         "本项目自己建 6 个任务（gw_rtos_create_tasks），**不要**调它"))

    # ---- 7) IWDG 得在工程里配置好（RTOS 下要开狗）----
    # ⚠️ 两种启动方式都算满足（2026-10-11 修正）：CubeMX 生成的 MX_IWDG_Init()，
            # ⚠️ 必须要求 **MX_IWDG_Init()**（2026-10-11 板上实测的 HardFault）：
            #    `gw_rtos_iwdg_enable()` 调用 HAL 的 `HAL_IWDG_Init(&hiwdg)`，而
            #    `hiwdg.Instance` 只有 `MX_IWDG_Init()` 会赋值 —— 漏掉就是 Instance==0，
            #    HAL 往 0x00000008 写寄存器 -> HardFault -> 安全模式。
            #    （上一轮我错误地"放宽"成接受寄存器级使能，恰好掩盖了这个真缺陷。）
            if re.search(r"^IWDG\.", ioc, re.M) and "MX_IWDG_Init" in main_c:
                rows.append((OK, "IWDG", " .ioc 已配置 + main.c 调了 MX_IWDG_Init ✓（它同时初始化句柄并启动看门狗）"))
            else:
                rows.append((TODO, "IWDG", " 工程里没配 IWDG，或 main.c **没调 MX_IWDG_Init()** —— 只调 gw_rtos_iwdg_enable() 会因句柄未初始化而 HardFault"))

    # ---- 8) HAL_Init 必须被调用（NVIC 分组 = Group 4 由它设）----
    if "HAL_Init()" in main_c:
        rows.append((OK, "NVIC 分组", " main.c 调了 HAL_Init() -> Group 4（板上实测 AIRCR=0xFA050300 ✓）"))
    else:
        rows.append((BAD, "NVIC 分组", " main.c 里没有 HAL_Init() -> NVIC 分组可能不是 Group 4（configPRIO_BITS=4 要求）"))

    # ---- 9) 镜像构建路径（2026-10-11 第 29 轮加）----
    #
    # ⚠️ 工程自带的 `STM32F103ZETX_FLASH.ld` 是**单镜像布局**
    #    （`FLASH ORIGIN = 0x8000000, LENGTH = 512K`）。而本项目的可烧镜像
    #    **必须**由 `tools/build_ota_images.ps1` 用**仓库的槽位脚本**
    #    （`ld/app_slot_a.ld` 等，`VECT_TAB_OFFSET=0x00009000`）产出。
    #
    #    如果直接用 IDE 构建并烧录：
    #      · 镜像落在 **0x08000000** -> **覆盖引导程序**；
    #      · 槽位 A/B/golden 布局被毁 -> OTA/回滚/降级整条机制没了；
    #      · 而板子看起来**还能跑**（单镜像模式），最容易事后才发现。
    ld = None
    for name in ("STM32F103ZETX_FLASH.ld", "STM32F103ZETx_FLASH.ld"):
        p = os.path.join(prj, name)
        if os.path.isfile(p):
            ld = name
            break
    if ld:
        rows.append((OK, "镜像构建路径",
                     " 工程自带 `%s` 是**单镜像布局**（0x08000000）—— 可烧镜像"
                     "**一律**用 `powershell -File tools\\build_ota_images.ps1`（它套仓库的"
                     "`ld/app_slot_a.ld` 等槽位脚本）；**别**用 IDE 直接构建后烧录，"
                     "那会覆盖引导程序、毁掉槽位与 golden 布局 ✓（已确认构建脚本走槽位脚本）" % ld))
    else:
        rows.append((TODO, "镜像构建路径",
                     " 工程里没找到常见的单镜像链接脚本 —— 确认构建路径：可烧镜像必须由 "
                     "`tools/build_ota_images.ps1`（槽位脚本）产出"))

    # ---- 输出 ----
    width = max(len(r[1]) for r in rows)
    print("")
    for st, name, why in rows:
        tag = {"OK": "[ OK ]", TODO: "[待办]", BAD: "[矛盾]"}[st]
        print("  %s %-*s %s" % (tag, width, name, why))

    n_ok = sum(1 for r in rows if r[0] == OK)
    n_todo = sum(1 for r in rows if r[0] == TODO)
    n_bad = sum(1 for r in rows if r[0] == BAD)
    print("")
    print("  小结：%d 项已就绪 / %d 项待办 / **%d 项矛盾**" % (n_ok, n_todo, n_bad))
    if n_bad:
        print("  ==> 有互相冲突的状态，落地前必须先解决（上面 [矛盾] 那几行）")
        return 1
    if n_todo:
        print("  ==> 没有冲突；待办项就是落地那天要做的事（见上面 [待办]）")
    else:
        print("  ==> 全部就绪，可以按迁移指南落地")
    return 0


if __name__ == "__main__":
    sys.exit(main())
