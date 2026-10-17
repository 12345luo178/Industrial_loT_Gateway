#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""CI 检查：RTOS 落地的两处"接线"（都属于"漏了不报错、运行时才炸"）

============================================================================
 ① 被"弱符号占位"的钩子，RTOS 层必须给出强定义
============================================================================

  `port/gw_ota_port.c` 每擦一页都会调 `gw_ota_port_yield()`：
  裸机侧用**弱符号**给了空实现，RTOS 侧由 `port/gw_rtos.c` 给**强定义**
  （`vTaskDelay(1)`）覆盖它。

  这条链一旦断掉，**不会报任何错**：

      OTA 擦除 136 KB（1.4~2.7 秒）期间不让出 CPU
      -> 空闲任务跑不到 -> vApplicationIdleHook() 里的喂狗被跳过
      -> IWDG（1.07~2.13 秒）咬 -> **升级擦到一半被复位**

  而它**只在真做一次 OTA 时**才暴露：平时串口正常、自检全绿、CI 全绿。

  ⚠️ 2026-10-11 之前的状态就是"断的"：那边写的是
     `#if defined(GW_OTA_PORT_USE_RTOS)`，而**全工程没有任何地方定义这个宏**、
     也没有任何强定义 —— 于是钩子静默退化成空函数。
     现在改成弱符号（RTOS 侧自动覆盖，不需要谁记得开开关）。

============================================================================
 这是"廉价预警"，不是编译器
============================================================================

  它做的是**源文本**检查（真正的一致性由链接器保证：强定义覆盖弱定义）。
  它防的是"有人把强定义删了、或把弱符号改回宏开关"这类**沉默的**回归 ——
  那种改动在编译和链接上都不报错。

  检查四条：
    1) gw_ota_port.c 里 gw_ota_port_yield 是**弱**定义
    2) gw_ota_port.c 里**不再出现** GW_OTA_PORT_USE_RTOS 宏开关
       （它一旦回来，"忘了定义宏"的静默路径就回来了）
    3) gw_rtos.c 里 gw_ota_port_yield 是**强**定义（不带 weak）
    4) 调用点还在（gw_ota_port.c 里确实在擦页循环里调用它）

============================================================================
 ② 三个中断向量重定向必须在 FreeRTOSConfig.h 里
============================================================================

  `port.c` 定义的是 `vPortSVCHandler` / `xPortPendSVHandler` /
  `xPortSysTickHandler`，而启动文件的向量表里是 `SVC_Handler` /
  `PendSV_Handler` / `SysTick_Handler`（**弱符号**）。重定向宏必须让
  **port.c 编译时看得到** —— 而 port.c 只包含 `FreeRTOS.h` → `FreeRTOSConfig.h`。

  ⚠️ 写在 main.c 里等于没写（迁移指南原来就是这么教的，2026-10-11 更正），
     而且**链接不报错**（那几个名字只是没被向量表引用）：
     后果是**第一次任务切换就 HardFault**，现场与任务毫无关系。

  检查：`#define vPortSVCHandler SVC_Handler` 等三条必须在
        `port/FreeRTOSConfig.h` 里。
============================================================================
"""
import os
import re
import sys

# ⚠️ 编码守卫：本文件的 print 里有 ⚠️（U+26A0 U+FE0F），而中文控制台是 GBK。
#    没有守卫的话，**在裸控制台里跑会自己 UnicodeEncodeError 崩掉** ——
#    而 CI 设了 PYTHONIOENCODING=utf-8，所以 CI 里完全看不出来。
#    ⚠️ 这个守卫本身就是被 tools/check_console_encoding.py 抓出来的
#       （2026-10-11 加堆预算检查时把 ⚠️ 写进了 print，CI 立刻报了这个文件）。
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(errors="replace")
    except (AttributeError, ValueError):
        pass

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

OTA_PORT = os.path.join("port", "gw_ota_port.c")
RTOS_C   = os.path.join("port", "gw_rtos.c")

HOOK = "gw_ota_port_yield"


def read(rel):
    path = os.path.join(REPO, rel)
    if not os.path.exists(path):
        return None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def strip_comments(text):
    """剥掉块注释与行注释。

    ⚠️ 必须有这一步：本次改动的注释里**故意**写了被删掉的那个宏名
       （解释"为什么删它"），不剥注释就会把自己的说明文档当成违规代码。
       ——第一次跑这个检查就是这么误报的。
    """
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def def_is_weak(text):
    """找到 HOOK 的定义行，判断它前面有没有 __attribute__((weak))

    ⚠️ 必须先剥注释：**注释里提到函数名**（"每擦一页都调 gw_ota_port_yield()"）
       也会匹配到"以 ) 结尾的行"，于是把注释当成定义 ——
       本工具第一次自证时就是这么漏判的（强定义删掉了它还说 OK）。
    """
    code = strip_comments(text)
    for m in re.finditer(r"^[^\n]*\b" + HOOK + r"\s*\([^;]*\)\s*$", code, re.M):
        line = m.group(0)
        return ("weak" in line)
    return None


def main():
    fail = 0
    print("  定义检查：%s（弱占位） + %s（强定义）" % (OTA_PORT, RTOS_C))

    ota = read(OTA_PORT)
    rtos = read(RTOS_C)
    if ota is None:
        print("  [FAIL] 找不到 %s" % OTA_PORT)
        return 1
    if rtos is None:
        print("  [FAIL] 找不到 %s" % RTOS_C)
        return 1

    # ---- 1) 裸机侧必须是弱定义 ----
    w = def_is_weak(ota)
    if w is None:
        print("  [FAIL] %s 里找不到 %s 的定义" % (OTA_PORT, HOOK))
        fail += 1
    elif w is False:
        print("  [FAIL] %s 里的 %s **不是弱定义** —— RTOS 侧就没法覆盖它"
              % (OTA_PORT, HOOK))
        print("         修法：写成 `__attribute__((weak)) void %s(void) { }`" % HOOK)
        fail += 1
    else:
        print("    [ OK ] %s：%s 是弱定义（空实现）" % (OTA_PORT, HOOK))

    # ---- 2) 旧的宏开关不许回来（只看代码，注释里的历史说明不算）----
    ota_code = strip_comments(ota)
    if "GW_OTA_PORT_USE_RTOS" in ota_code:
        print("  [FAIL] %s 里又出现了 GW_OTA_PORT_USE_RTOS（代码里，不是注释）"
              % OTA_PORT)
        print("         那个宏**全工程没人定义** -> 钩子会静默退化成空函数")
        print("         -> RTOS 下 OTA 擦除不让出 CPU -> 被看门狗咬在中途")
        fail += 1
    else:
        print("    [ OK ] 旧的 GW_OTA_PORT_USE_RTOS 宏开关已不存在（代码里）")

    # ---- 3) RTOS 侧必须是强定义 ----
    r = def_is_weak(rtos)
    if r is None:
        print("  [FAIL] %s 里没有 %s 的强定义" % (RTOS_C, HOOK))
        print("         -> 链接器会用裸机那个空弱定义 -> OTA 擦除期间不让出 CPU")
        fail += 1
    elif r is True:
        print("  [FAIL] %s 里的 %s 是**弱**定义（两个都弱等于没有强定义）"
              % (RTOS_C, HOOK))
        fail += 1
    else:
        print("    [ OK ] %s：%s 是强定义（会覆盖弱定义）" % (RTOS_C, HOOK))

    # ---- 4) 调用点还在（否则钩子挂空了，弱/强都无所谓，但设计意图丢了）----
    if re.search(r"^\s*" + HOOK + r"\s*\(\s*\)\s*;", ota, re.M):
        print("    [ OK ] %s：擦页循环里确实调用了它" % OTA_PORT)
    else:
        print("  [FAIL] %s 里找不到对 %s 的调用 —— 钩子挂空了"
              % (OTA_PORT, HOOK))
        fail += 1

    # ---- 5) 三个中断向量重定向必须在 FreeRTOSConfig.h 里 ----
    #
    # ⚠️ 这一条与上面那条是**同一类**：接线漏了，链接**不报错**，运行时才炸。
    #    port.c 定义的是 vPortSVCHandler/xPortPendSVHandler/xPortSysTickHandler，
    #    启动文件的向量表里是 SVC_Handler/PendSV_Handler/SysTick_Handler（弱符号）。
    #    重定向宏必须让 **port.c 编译时能看到** —— 而 port.c 只包含
    #    FreeRTOS.h -> FreeRTOSConfig.h。写在 main.c 里等于没写（本指南原来
    #    就是这么写的，2026-10-11 更正）。
    cfg = read(os.path.join("port", "FreeRTOSConfig.h"))
    if cfg is None:
        print("  [FAIL] 找不到 port/FreeRTOSConfig.h")
        fail += 1
    else:
        cfg_code = strip_comments(cfg)
        for name, cmsis in (("vPortSVCHandler", "SVC_Handler"),
                            ("xPortPendSVHandler", "PendSV_Handler"),
                            ("xPortSysTickHandler", "SysTick_Handler")):
            if re.search(r"#define\s+" + name + r"\s+" + cmsis, cfg_code):
                print("    [ OK ] FreeRTOSConfig.h: %s -> %s" % (name, cmsis))
            else:
                print("  [FAIL] port/FreeRTOSConfig.h 里缺 `#define %s %s`"
                      % (name, cmsis))
                print("         后果：向量表仍指向启动文件的**弱定义** -> 内核的"
                      "SVC/PendSV/SysTick 从没被执行")
                print("         -> **第一次任务切换就 HardFault**（链接不报错！）")
                fail += 1

    # ---- 6) FreeRTOS 堆够不够放下所有任务栈 + 内核对象 ----
    #
    # ⚠️ 这一条是 2026-10-11 补的，因为**差点带着一个必然失败的上板计划去烧板**：
    #      `gw_rtos.h` 写"6 个任务栈合计 ≈ 8.2 KB"（384+384+256+512+256+256 字 × 4）
    #      而 `FreeRTOSConfig.h` 写"6 个任务栈 ≈ 5.5 KB"，据此把堆定成 **8 KB**。
    #      8 KB 堆连**任务栈本身**都放不下，更别说空闲任务栈、7 个 TCB、
    #      heap_4 的每块开销 —— `xTaskCreate` 会失败，运行时走到
    #      `[FATAL] task create failed`。而这条**只在真上板启动 RTOS 时**才暴露。
    #
    # ⚠️ 两个文件各自"估"同一个预算，就一定会互相矛盾 ——
    #    所以这里把它变成**一处机械计算**：改任何一处，这条检查立刻报出来。
    rtos_h = read(os.path.join("port", "gw_rtos.h"))
    if rtos_h is None:
        print("  [FAIL] 找不到 port/gw_rtos.h（读任务栈尺寸）")
        fail += 1
    elif cfg is None:
        pass                       # 上面已经报过
    else:
        stacks = {}
        for m in re.finditer(r"#define\s+(GW_STACK_\w+)\s+(\d+)",
                             strip_comments(rtos_h)):
            stacks[m.group(1)] = int(m.group(2))

        m = re.search(r"#define\s+configTOTAL_HEAP_SIZE\s+"
                      r"\(\(size_t\)\s*\(\s*(\d+)\s*\*\s*(\d+)\s*\)\s*\)",
                      cfg_code)
        if m:
            heap = int(m.group(1)) * int(m.group(2))
        else:
            m2 = re.search(r"#define\s+configTOTAL_HEAP_SIZE\s+"
                           r"\(\(size_t\)\s*\(\s*(\d+)\s*\)\s*\)", cfg_code)
            heap = int(m2.group(1)) if m2 else None

        m = re.search(r"#define\s+configMINIMAL_STACK_SIZE\s+"
                      r"\(\(unsigned short\)\s*(\d+)\s*\)", cfg_code)
        idle_words = int(m.group(1)) if m else None

        timers = 1 if re.search(r"#define\s+configUSE_TIMERS\s+1", cfg_code) else 0

        if not stacks or heap is None or idle_words is None:
            print("  [FAIL] 堆预算检查：解析失败"
                  "（stacks=%d heap=%s idle=%s）" % (len(stacks), heap, idle_words))
            fail += 1
        else:
            TCB_BYTES, BLOCK_OVERHEAD, RESERVE = 128, 16, 1024
            MUTEX_BYTES = 192         # Flash 互斥 + 日志互斥（缺陷 X / Y）+ 块头
            n = len(stacks)
            n_obj = n + 1 + timers            # 业务任务 + 空闲任务(+ 定时器任务)
            task_b = sum(stacks.values()) * 4
            idle_b = idle_words * 4
            tcb_b = n_obj * TCB_BYTES
            ovh_b = n_obj * BLOCK_OVERHEAD + MUTEX_BYTES
            need = task_b + idle_b + tcb_b + ovh_b + RESERVE
            print("    堆预算：任务栈 %d B (%d 个) + 空闲栈 %d B + TCB %d B "
                  "+ 块开销/互斥量 %d B + 余量 %d B = 需要 %d B；堆 = %d B"
                  % (task_b, n, idle_b, tcb_b, ovh_b, RESERVE, need, heap))
            if heap < need:
                print("  [FAIL] configTOTAL_HEAP_SIZE 不够：差 %d B"
                      % (need - heap))
                print("         -> xTaskCreate 会失败 -> 启动即 [FATAL] task create failed")
                print("         ⚠️ 只在**真上板启动 RTOS** 时才暴露，链接与 CI 都不会报")
                fail += 1
            else:
                print("    [ OK ] 堆够用（余量 %d B）" % (heap - need))

        # ---- 7) 栈尺寸 / 优先级 / 任务数三者的自洽性 ----
        #
        # 这三条都是 FreeRTOS 的**硬性前置条件**，违反了同样只在启动时炸：
        #   · xTaskCreate 的栈深度 < configMINIMAL_STACK_SIZE -> 断言/失败
        #     （内核明确要求：任务栈不得小于空闲任务的栈）
        #   · 任务优先级 >= configMAX_PRIORITIES -> 断言/越界
        #   · 栈宏个数 != GW_TASK_COUNT -> 建任务表与枚举对不上
        prios = {}
        for m in re.finditer(r"#define\s+(GW_PRIO_\w+)\s+(\d+)", strip_comments(rtos_h or "")):
            prios[m.group(1)] = int(m.group(2))

        sched_h = read(os.path.join("inc", "gw_sched.h")) or ""
        m = re.search(r"GW_TASK_COUNT\s*=\s*(\d+)", sched_h)
        task_count = int(m.group(1)) if m else None

        m = re.search(r"#define\s+configMAX_PRIORITIES\s+\((\d+)\)", cfg_code)
        max_prios = int(m.group(1)) if m else None

        # (a) 任务数
        if task_count is None:
            print("  [FAIL] 读不到 GW_TASK_COUNT（inc/gw_sched.h）")
            fail += 1
        else:
            for what, d in (("栈尺寸", stacks), ("优先级", prios)):
                if len(d) != task_count:
                    print("  [FAIL] %s宏有 %d 个，而 GW_TASK_COUNT = %d —— 对不上"
                          % (what, len(d), task_count))
                    fail += 1
                else:
                    print("    [ OK ] %s宏 %d 个，与 GW_TASK_COUNT 一致" % (what, len(d)))

        # (b) 每个任务栈都不小于 configMINIMAL_STACK_SIZE（同为"字"）
        if stacks and idle_words is not None:
            small = {k: v for k, v in stacks.items() if v < idle_words}
            if small:
                print("  [FAIL] 这些任务栈小于 configMINIMAL_STACK_SIZE(%d 字)：%s"
                      % (idle_words, ", ".join("%s=%d" % kv for kv in small.items())))
                print("         FreeRTOS 要求任务栈不得小于空闲任务栈，否则 xTaskCreate 失败")
                fail += 1
            else:
                print("    [ OK ] 6 个任务栈都 >= configMINIMAL_STACK_SIZE(%d 字)"
                      % idle_words)

        # (c) 最高任务优先级必须小于 configMAX_PRIORITIES
        if prios and max_prios is not None:
            top = max(prios.values())
            if top >= max_prios:
                print("  [FAIL] 最高任务优先级 %d >= configMAX_PRIORITIES(%d)"
                      % (top, max_prios))
                print("         -> xTaskCreate 断言/越界。要么降优先级，要么加大 configMAX_PRIORITIES")
                fail += 1
            else:
                print("    [ OK ] 最高任务优先级 %d < configMAX_PRIORITIES(%d)"
                      % (top, max_prios))

    # ---- 8) RTOS 喂狗判据的三处接线（★ 缺陷 U；删掉任何一处都会**静默**失效）----
    #
    # ⚠️ 缺陷 U：`gw_rtos_create_tasks()` 故意不调 `gw_app_enable_sched()`
    #    （怕 gw_app_poll 又去分派任务），于是 `app->sched == NULL`，
    #    喂狗判据走了"无调度器 -> 无条件允许"那条路 -> **等于没养狗**，
    #    而三处文档都说它在检查"6 个任务有没有跑过一轮"。
    #
    # 判据要真的生效，缺一不可：
    #   ① gw_sched_set_clock_ms()      —— 心跳判据要毫秒时间源
    #   ② gw_app_enable_watchdog_sched —— 把判据挂到应用层
    #   ③ gw_sched_note_beat()         —— 任务体每轮登记心跳
    rtos_c = read(RTOS_C)
    if rtos_c is None:
        print("  [FAIL] 找不到 port/gw_rtos.c")
        fail += 1
    else:
        rtos_code = strip_comments(rtos_c)
        for what, pat, why in (
            ("gw_sched_set_clock_ms", r"gw_sched_set_clock_ms\s*\(",
             "心跳判据没有毫秒时间源 -> 退回 progress 判据 -> RTOS 下永远拒喂"),
            ("gw_app_enable_watchdog_sched", r"gw_app_enable_watchdog_sched\s*\(",
             "判据没挂到应用层 -> 回到'无调度器 -> 无条件允许' -> 等于没养狗"),
            ("gw_sched_note_beat", r"gw_sched_note_beat\s*\(",
             "任务体不登记心跳 -> 所有任务都被判成死了 -> 无脑复位"),
        ):
            if re.search(pat, rtos_code):
                print("    [ OK ] gw_rtos.c：调了 %s" % what)
            else:
                print("  [FAIL] port/gw_rtos.c 里没有调用 %s()" % what)
                print("         后果：%s" % why)
                fail += 1

    # ---- 9) 内务钩子必须成对（★ 缺陷 W：只有弱定义 = 什么都不做，且完全静默）----
    #
    # ⚠️ 缺陷 W：main 拆成"裸机主循环 / RTOS 分支"之后，**循环里剩下的内务**
    #    （故障/日志节拍、OTA 传输状态机、**OTA 确认**、上报节拍、波特率切换、
    #      重启挂起）没人调了 -> RTOS 一落地，OTA 与上报整条链路就死，而且
    #    编译/链接/CI 全都不报（因为弱符号默认是空实现）。
    #
    # 修法：gw_rtos.c 给**弱定义**并由 HOUSE 任务调用，
    #       templates/main_template.c 给**强定义**覆盖它。
    #       ⚠️ 少任何一边都不会报错 —— 所以必须在这里断言成对存在。
    tpl = read(os.path.join("templates", "main_template.c"))
    if rtos_c is None:
        pass                                  # 上面已经报过
    elif tpl is None:
        print("  [FAIL] 找不到 templates/main_template.c")
        fail += 1
    else:
        calls = re.search(r"gw_rtos_housekeeping\s*\(\s*dt_ms\s*\)", rtos_code)
        weakd = re.search(r"__attribute__\(\(weak\)\)\s*void\s+gw_rtos_housekeeping",
                          rtos_code)
        strong = re.search(r"^void\s+gw_rtos_housekeeping\s*\(\s*uint32_t",
                           strip_comments(tpl), re.M)
        if weakd:
            print("    [ OK ] gw_rtos.c：内务钩子是弱定义")
        else:
            print("  [FAIL] gw_rtos.c 里 gw_rtos_housekeeping 不是弱定义"
                  "（弱定义是「模板可以覆盖」的前提）")
            fail += 1
        if calls:
            print("    [ OK ] gw_rtos.c：HOUSE 任务每轮调用内务钩子")
        else:
            print("  [FAIL] gw_rtos.c 里没有调用 gw_rtos_housekeeping(dt_ms)")
            print("         后果：RTOS 下 OTA 传输/确认、上报节拍、波特率切换全都不执行")
            fail += 1
        if strong:
            print("    [ OK ] main_template.c：给出了强定义（覆盖弱符号）")
        else:
            print("  [FAIL] templates/main_template.c 里没有 gw_rtos_housekeeping 的强定义")
            print("         后果：**静默什么都不做** -> 缺陷 W 重现"
                  "（OTA 永不确认 -> 升级必被回滚）")
            fail += 1

    # ---- 10) 裸机主循环必须只"委托"，不许自己攒内务（★ 缺陷 W 结构性守卫）----
    #
    # ⚠️ 缺陷 W 的根因是**不对称**：裸机主循环里有一堆有副作用的调用，
    #    而 RTOS 分支没有那个循环 -> 那些调用整批消失，且完全静默。
    #    修法是把内务抽成共用函数 `app_housekeeping()`。
    #
    #    ⚠️ 但"以后有人往裸机循环里再加一句"这条老路还在 ——
    #       那时 RTOS 分支又会悄悄少做一件事。
    #       所以这里断言：**裸机主循环体里不许出现任何 gw_*/HAL_* 调用**，
    #       除了白名单里的"分派 + 共用内务 + 延时/取时"。
    #       （够用就好：真需要新的周期动作，就加进 app_housekeeping，
    #         两条路径自然都有。）
    if tpl is None:
        pass
    else:
        # ⚠️ 定位必须在**原文**里做：那段锚点本身就是注释，
        #    先 strip_comments 会把它删掉（第一版就是这么匹配失败的）。
        m = re.search(r"#else\s*/\*[^\n]*裸机主循环[^\n]*\n(.*?)\n#endif",
                      tpl, re.S)
        if m is None:
            print("  [FAIL] 在 main_template.c 里找不到「裸机主循环」那一段"
                  "（结构变了就同步本检查）")
            fail += 1
        else:
            body = strip_comments(m.group(1))
            allowed = {"gw_app_poll", "app_housekeeping",
                       "HAL_GetTick", "HAL_Delay"}
            # ⚠️ 别把 app_ 前缀漏掉 —— 共用内务函数就叫 app_housekeeping，
            #    第一版只匹配 gw_/HAL_，于是"没调用 app_housekeeping"这条误报。
            found = set(re.findall(r"\b((?:gw|HAL|app)_\w+)\s*\(", body))
            stray = sorted(found - allowed)
            if stray:
                print("  [FAIL] 裸机主循环里出现了没走共用内务的调用：%s"
                      % ", ".join(stray))
                print("         后果：RTOS 分支没有这个循环 -> 那些调用在 RTOS 下**整批消失**")
                print("               （缺陷 W 就是这么发生的：OTA 传输/确认全丢）")
                print("         修法：把它加进 app_housekeeping()，让两条路径都走到")
                fail += 1
            elif "app_housekeeping" not in found:
                print("  [FAIL] 裸机主循环没有调用 app_housekeeping()")
                print("         后果：裸机侧连日志节拍/OTA 传输确认都不做了")
                fail += 1
            else:
                print("    [ OK ] 裸机主循环只委托（分派 + 共用内务 + 延时），"
                      "没有自己攒的内务")

    # ---- 11) Flash 访问互斥（★ 缺陷 X；RTOS 引入的并发风险）----
    #
    # ⚠️ 缺陷 X：Flash 写入/擦除分散在两个模块（参数/日志 与 OTA），
    #    裸机下被单主循环隐式串行化；**RTOS 下 HOUSE 擦 OTA 槽（1.4~2.7 秒）
    #    与从站任务保存参数会并发碰同一个 Flash 控制器** ——
    #    F1 的擦/编靠 FLASH->CR/AR 下命令，交错执行会**擦错页**
    #    （擦到运行中的页 = 当场变砖）。
    #
    # 修法：`gw_flash_lock()/gw_flash_unlock()` 包住原有的解锁/上锁
    #      （互斥的获取/释放跟着原本就配对的 HAL 调用走 -> 按构造保证成对）。
    #      裸机侧弱定义空实现；RTOS 侧 gw_rtos.c 给强定义（FreeRTOS 互斥量）。
    guard_c = read(os.path.join("port", "gw_flash_guard.c"))
    if guard_c is None:
        print("  [FAIL] 找不到 port/gw_flash_guard.c")
        fail += 1
    else:
        gc = strip_comments(guard_c)
        for fn in ("gw_flash_guard_enter", "gw_flash_guard_exit"):
            if re.search(r"__attribute__\(\(weak\)\)\s*void\s+" + fn, gc):
                print("    [ OK ] gw_flash_guard.c：%s 是弱定义" % fn)
            else:
                print("  [FAIL] gw_flash_guard.c 里 %s 不是弱定义" % fn)
                fail += 1
        if re.search(r"HAL_FLASH_Unlock\s*\(", gc) and \
           re.search(r"HAL_FLASH_Lock\s*\(", gc):
            print("    [ OK ] gw_flash_guard.c：真正调用 HAL 解锁/上锁")
        else:
            print("  [FAIL] gw_flash_guard.c 没有调用 HAL_FLASH_Unlock/Lock")
            fail += 1

    # ---- 11b) 日志互斥（★ 缺陷 Y）：多任务并发写日志会丢记录/串输出 ----
    #
    # ⚠️ 缺陷 Y：`gw_log_write()` 直接改环形 head/count/stat，而 RTOS 下
    #    任何任务都可能写日志；两边的 sink 还会同时调 `HAL_UART_Transmit`
    #    （不可重入）。丢的往往正是故障前后那几条。
    #    修法：`src/gw_log.c` 弱定义 + `gw_rtos.c` 强定义（普通互斥量，
    #    因为"取一次放一次、不嵌套"—— flush 用不加锁的内部读函数）。
    log_c = read(os.path.join("src", "gw_log.c"))
    if log_c is None:
        print("  [FAIL] 找不到 src/gw_log.c")
        fail += 1
    else:
        lc = strip_comments(log_c)
        for fn in ("gw_log_guard_enter", "gw_log_guard_exit"):
            if re.search(r"__attribute__\(\(weak\)\)\s*void\s+" + fn, lc):
                print("    [ OK ] gw_log.c：%s 是弱定义" % fn)
            else:
                print("  [FAIL] src/gw_log.c 里 %s 不是弱定义" % fn)
                fail += 1
        if len(re.findall(r"\bgw_log_guard_enter\s*\(\s*\)", lc)) >= 2:
            print("    [ OK ] gw_log.c：写路径与读路径各自取放锁（flush 走公开读，不整圈持锁）")
        else:
            print("  [FAIL] gw_log.c 里 gw_log_guard_enter() 调用少于 2 处"
                  "（写路径与读路径都要）")
            fail += 1
        # ⚠️ flush **不许整圈持锁**（缺陷 Y 的第二版）：一次 flush 最多吐一整个
        #    环形 × 每条一次串口发送（超时 20 ms）-> 持锁可达数百毫秒，
        #    而 gw_log_write 要取同一把锁 -> 别的任务打条日志被卡住几百毫秒
        #    （从站任务正好要打日志的话，Modbus 响应会被拖过 5 ms 硬超时）。
        #    正解：逐条走公开的 gw_log_read()（每条内部取放一次，锁内只有几微秒），
        #    接受"flush 的串口输出可能与另一条日志交错"这个观感问题。
        m_flush = re.search(r"uint16_t gw_log_flush\(.*?\n\}", lc, re.S)
        if m_flush is None:
            print("  [FAIL] 在 src/gw_log.c 里找不到 gw_log_flush()")
            fail += 1
        else:
            fb = m_flush.group(0)
            if re.search(r"gw_log_guard_enter\s*\(", fb):
                print("  [FAIL] gw_log_flush() 里整圈持了锁")
                print("         后果：flush 最多吐一整个环形 × 每条一次串口发送（超时 20ms）")
                print("               -> 持锁可数百毫秒，别的任务打日志被卡住")
                print("               -> 从站任务正好要打日志时，Modbus 响应拖过 5ms 硬超时")
                fail += 1
            elif re.search(r"while\s*\(\s*gw_log_read\s*\(", fb):
                print("    [ OK ] gw_log_flush：逐条走公开读（锁内只有几微秒），不整圈持锁")
            else:
                print("  [FAIL] gw_log_flush() 既没整圈持锁、也没用 gw_log_read()"
                      "（结构变了就同步本检查）")
                fail += 1

    if rtos_code:
        strongs = re.findall(r"^void\s+(gw_flash_guard_(?:enter|exit))\s*\(",
                             rtos_code, re.M)
        if len(strongs) == 2:
            print("    [ OK ] gw_rtos.c：给出 %s 的强定义" % "/".join(strongs))
        else:
            print("  [FAIL] gw_rtos.c 缺少 Flash 守卫的强定义（找到 %d 个）"
                  % len(strongs))
            print("         后果：守卫静默变成空实现 -> RTOS 下两个任务照样并发写 Flash")
            fail += 1
        if len(re.findall(r"^void\s+(gw_log_guard_(?:enter|exit))\s*\(",
                          rtos_code, re.M)) == 2:
            print("    [ OK ] gw_rtos.c：给出日志守卫的强定义")
        else:
            print("  [FAIL] gw_rtos.c 缺少日志守卫的强定义")
            fail += 1
        n_mtx = len(re.findall(r"xSemaphoreCreateMutex\s*\(", rtos_code))
        if n_mtx >= 2:
            print("    [ OK ] gw_rtos.c：创建了 %d 个互斥量（Flash + 日志）" % n_mtx)
        else:
            print("  [FAIL] gw_rtos.c 只创建了 %d 个互斥量（Flash 与日志各要一个）"
                  % n_mtx)
            fail += 1

    # 两个 Flash 模块：必须走守卫，且 lock/unlock 必须成对（漏放锁 = RTOS 下死等）
    for mod in ("gw_flash_stm32.c", "gw_ota_port.c"):
        src = read(os.path.join("port", mod))
        if src is None:
            continue
        code = strip_comments(src)
        if re.search(r"HAL_FLASH_(?:Unlock|Lock)\s*\(", code):
            print("  [FAIL] port/%s 里还有直接的 HAL_FLASH_Unlock/Lock"
                  "（绕过守卫就没互斥）" % mod)
            fail += 1
        n_lock = len(re.findall(r"\bgw_flash_lock\s*\(", code))
        n_unl = len(re.findall(r"\bgw_flash_unlock\s*\(", code))
        if n_lock == 0:
            print("  [FAIL] port/%s 没有调用 gw_flash_lock()（Flash 操作没互斥）" % mod)
            fail += 1
        elif n_unl < n_lock:
            print("  [FAIL] port/%s：gw_flash_lock %d 次、gw_flash_unlock 只 %d 次"
                  " -> 有路径**拿了锁不放**（RTOS 下会死等）" % (mod, n_lock, n_unl))
            fail += 1
        else:
            print("    [ OK ] %s：Flash 全走守卫（lock %d 次 / unlock %d 次）"
                  % (mod, n_lock, n_unl))

    # ---- 11c) RS485 仲裁原子性（★ 缺陷 Z）：检查->置 owner 必须原子 ----
    #
    # ⚠️ 缺陷 Z：`rs485_arb_acquire()` 是"判定 owner -> 置 owner"两步。
    #    裸机下主站/从站在同一主循环里顺序执行；RTOS 抢占下会**两个角色
    #    都以为拿到总线 -> 总线上两个发送者**（半双工总线撞车）。
    #    修法：四个改状态的函数里用临界区包"决策+改状态"（不包收发过程，
    #    因为"从站抢占主站等待窗口"是设计语义）。
    arb_c = read(os.path.join("src", "rs485_arbiter.c"))
    if arb_c is None:
        print("  [FAIL] 找不到 src/rs485_arbiter.c")
        fail += 1
    else:
        ac = strip_comments(arb_c)
        for fn in ("rs485_arb_guard_enter", "rs485_arb_guard_exit"):
            if re.search(r"__attribute__\(\(weak\)\)\s*void\s+" + fn, ac):
                print("    [ OK ] rs485_arbiter.c：%s 是弱定义" % fn)
            else:
                print("  [FAIL] src/rs485_arbiter.c 里 %s 不是弱定义" % fn)
                fail += 1
        # 四个改状态的函数都要包住（acquire / release / enter_wait / exit_wait）
        n_guard = len(re.findall(r"\brs485_arb_guard_enter\s*\(\s*\)", ac))
        if n_guard >= 4:
            print("    [ OK ] rs485_arbiter.c：%d 处取临界区（四个改状态函数都包了）"
                  % n_guard)
        else:
            print("  [FAIL] rs485_arbiter.c 只有 %d 处 rs485_arb_guard_enter()"
                  "（acquire/release/enter_wait/exit_wait 四个都要）" % n_guard)
            fail += 1

    if rtos_code:
        if len(re.findall(r"^void\s+(rs485_arb_guard_(?:enter|exit))\s*\(",
                          rtos_code, re.M)) == 2:
            print("    [ OK ] gw_rtos.c：给出仲裁原子性钩子的强定义")
        else:
            print("  [FAIL] gw_rtos.c 缺少 rs485_arb_guard_* 的强定义")
            fail += 1
        if re.search(r"taskENTER_CRITICAL\s*\(", rtos_code) and \
           re.search(r"taskEXIT_CRITICAL\s*\(", rtos_code):
            print("    [ OK ] gw_rtos.c：用临界区实现（不是互斥量 —— 见缺陷 Z 说明）")
        else:
            print("  [FAIL] gw_rtos.c 没有用 taskENTER/EXIT_CRITICAL 实现仲裁钩子")
            fail += 1

    # ---- 11d) OTA 擦除的互斥粒度必须是"按页"（★ 缺陷 X 的第二版）----
    #
    # ⚠️ 第一版把互斥包在**整个擦除**外面（1.4~2.7 秒），后果是
    #    "从站任务一旦要保存参数就被阻塞 2.7 秒" —— 而**逐页让出 CPU**
    #    这条设计的初衷正是"擦除期间 Modbus 从站照常响应"，等于自己废掉它。
    #    按页取放后：别的任务最多等一页（20~40 ms）。
    ota_c = read(os.path.join("port", "gw_ota_port.c"))
    if ota_c is None:
        pass
    else:
        oc = strip_comments(ota_c)
        m = re.search(r"static int ota_erase\(void \*ctx.*?\n\}", oc, re.S)
        if m is None:
            print("  [FAIL] 在 gw_ota_port.c 里找不到 ota_erase()（结构变了就同步本检查）")
            fail += 1
        else:
            body = m.group(0)
            i_loop = body.find("for (page")
            prologue = body[:i_loop] if i_loop >= 0 else body
            loop = body[i_loop:] if i_loop >= 0 else ""
            if i_loop < 0:
                print("  [FAIL] ota_erase() 里找不到逐页循环（结构变了就同步本检查）")
                fail += 1
            elif re.search(r"gw_flash_lock\s*\(", strip_comments(prologue)):
                print("  [FAIL] ota_erase() 在**循环之外**取了 Flash 互斥")
                print("         后果：整段擦除（1.4~2.7 秒）期间别的任务拿不到锁")
                print("               -> 从站存参数被阻塞 2.7 秒，'逐页让出'白做了")
                fail += 1
            elif not re.search(r"gw_flash_lock\s*\(", loop):
                print("  [FAIL] ota_erase() 的逐页循环里没有取 Flash 互斥")
                print("         后果：擦除没有互斥保护（缺陷 X 重现）")
                fail += 1
            else:
                print("    [ OK ] ota_erase：互斥按页取放（别的任务最多等一页）")

    # ---- 10b) ★ app_housekeeping 里必须**确实还有**那些内务调用 ----
    #
    # ⚠️ 上面 10) 只保证了"裸机主循环只委托、不自己攒内务"。
    #    但**共用内务函数本身被掏空**是另一条路：两条路径会**同时**丢掉内务，
    #    而且同样完全静默 —— 那就是缺陷 W 换个位置复活。
    #
    #  ⚠️ 下面每一项都写清"丢了会怎样"，报错时直接说后果（不是只报一个名字）。
    HK_NEEDS = [
        ("gw_fault_port_tick", "运行时长不推进 -> **OTA 确认永远等不到 30 秒** -> "
                              "健康固件被回滚（缺陷 W 的典型症状）"),
        ("gw_ota_bridge_tick", "OTA 传输状态机不走 -> **根本收不下固件**"),
        ("gw_ota_confirm_running", "**升级永不确认** -> 3 次启动后必然回滚"),
        ("gw_ota_bridge_begin_pending", "待处理擦除不执行"),
        ("gw_ota_bridge_reboot_pending", "升级完成后不重启"),
        ("gw_log_tick", "日志时标停摆"),
        ("flush_log", "日志永远不输出"),
        ("gw_app_submit_telemetry", "周期上报停"),
        ("gw_baud_tick", "波特率运行时切换失效"),
    ]
    if tpl is None:
        pass
    else:
        m_hk = re.search(r"static void app_housekeeping\(uint32_t dt\)\s*\{(.*?)\n\}",
                         strip_comments(tpl), re.S)
        if m_hk is None:
            print("  [FAIL] 在 main_template.c 里找不到 app_housekeeping()")
            fail += 1
        else:
            hk = m_hk.group(1)
            missing = [(c, why) for c, why in HK_NEEDS
                       if not re.search(r"\b" + c + r"\s*\(", hk)]
            if missing:
                print("  [FAIL] app_housekeeping() 里少了 %d 个内务调用："
                      % len(missing))
                for c, why in missing:
                    print("           %s()  —— 丢了会：%s" % (c, why))
                print("         ⚠️ 两条路径（裸机主循环 / RTOS 的 HOUSE 任务）**同时**失效，"
                      "而且编译/链接都看不出来")
                fail += 1
            else:
                print("    [ OK ] app_housekeeping()：%d 项内务调用齐全"
                      "（OTA 传输/确认、运行时长、日志、上报、波特率、重启挂起）"
                      % len(HK_NEEDS))

    if fail:
        print("  ==> RTOS 让出钩子检查未通过（%d 处）" % fail)
        return 1
    print("  ==> RTOS 让出钩子链路完整（裸机弱占位 + RTOS 强覆盖 + 调用点）")
    print("  ==> 三个中断向量重定向都在 FreeRTOSConfig.h 里（port.c 能看到）")
    print("  ==> 喂狗判据三处接线都在（时钟 / 挂载 / 心跳）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
