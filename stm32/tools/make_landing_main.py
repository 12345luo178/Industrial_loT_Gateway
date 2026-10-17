"""把「生产模板」+「工程自己的 CubeMX 尾巴」拼成一份可直接替换的工程 main.c。

============================================================================
 为什么需要它（2026-10-11）
============================================================================
本仓库有两个 main 模板：

    templates/main_template.c      生产版（OTA 桥 + 上报节拍 + RTOS 分支）
    templates/main_L0_bringup.c    L0 上板验证版（自检 / CAN 回环 / L0 诊断开关）

实测确认：**工程的 Core/Src/main.c 是 L0 版衍生物**（前 1070 行与
main_L0_bringup.c 逐行相同 1068/1070），而它下方 ~90 行是 CubeMX 的尾巴：

    SystemClock_Config              HSE + PLLx9 -> 72MHz
    HAL_TIM_PeriodElapsedCallback   TIM4 时基（替代 SysTick）  <- 少了它 HAL_GetTick 会停
    Error_Handler / assert_failed   出错落点

所以"换成生产版"**不能直接把模板覆盖过去**（会缺这四段 -> 链接期报错），
也不能"合并 app_housekeeping"（那是生产版内部的一处重构，与 L0 版结构不匹配）。

正确做法 = **生产模板（整体）+ 工程那份 CubeMX 尾巴**，也就是本脚本干的事。

============================================================================
 安全性设计
============================================================================
1. ⚠️ 只做**拼接**，不改任何一行内容：两块都是从文件里**逐字取**的；
2. ⚠️ **前置检查**：工程 main.c 必须确实"带 L0 标记 + 带 CubeMX 尾巴"，
   否则**拒绝生成**（宁可不出，也不出一份来源不明的 main.c）；
3. ⚠️ 尾巴边界**按内容定位**（"SystemClock_Config 前那段注释块的起始"），
   不写死行号 —— 用户哪怕在尾巴里加了几行注释也不会错位；
4. ⚠️ 生成的文件头写明来源（模板 commit / 工程路径 / 生成时间），便于追责。

用法：
    python tools/make_landing_main.py --out build/landing_main.c
    python tools/make_landing_main.py --check        # 只做前置检查（CI 用）
"""
import argparse
import io
import os
import re
import subprocess
import sys
import time

# ⚠️ 编码守卫（GBK 控制台下中文 print 会 UnicodeEncodeError）
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(errors="replace")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 工程 main.c 应当具备的 L0 特征（用于前置检查）
L0_MARKS = ["GW_L0_IWDG_ENABLE", "GW_L0_CAN_LOOPBACK_PERIOD_S",
            "can_port_loopback_test"]
# 尾巴必须提供的四段（缺一不可，否则生产模板链接不过）
TAIL_NEEDS = ["void SystemClock_Config(void)",
              "void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef",
              "void Error_Handler(void)",
              "void assert_failed(uint8_t"]


def read_lines(path):
    return io.open(path, encoding='utf-8').read().splitlines()


def drop_main(lines):
    """从骨架尾巴里去掉 CubeMX 生成的 `int main(void) { ... }` 整块。

    ⚠️ 落地当天实测（真编译当场报 redefinition of 'main'）：
       L0 版的 CubeMX 尾巴在 main() **之后**，搬尾巴不会撞；
       而 CubeMX **骨架**是 main() 在前、SystemClock_Config/Error_Handler 在后，
       而尾巴定位点在 main() **之前** -> 直接搬就多出一个 main()。
    """
    out = []
    i = 0
    n = len(lines)
    while i < n:
        if lines[i].startswith('int main(void)'):
            depth = 0
            started = False
            while i < n:
                depth += lines[i].count('{') - lines[i].count('}')
                if '{' in lines[i]:
                    started = True
                i += 1
                if started and depth <= 0:
                    break
            continue
        out.append(lines[i])
        i += 1
    return out


def find_include_block(lines):
    """取出工程 main.c 的 #include 块（**骨架路径**用它代替 extern 声明块）

    为什么骨架路径搬这个就够了：
        本工程是"CubeMX 按外设生成 .c/.h"的 —— 句柄的 extern 声明在
        usart.h / can.h / iwdg.h 里，MX_*_Init() 原型在 gpio.h / dma.h /
        usart.h / can.h / iwdg.h 里。骨架 main.c **不写**这些声明，
        但它 #include 了上述所有头 -> 把头带过去，声明就全有了。
    ⚠️ 这不是"少检查"：最终仍由 --verify-compile 真编译把关（-Wall 0 诊断）。
    """
    out = []
    for l in lines:
        s = l.strip()
        if s.startswith('#include'):
            # ⚠️ 跳过 CMSIS 包装层的头（2026-10-11）：我们只调原生 FreeRTOS API，
            #    cmsis_os.h 纯属多余，而且是可烧镜像构建（只带 kernel include）里
            #    唯一解析不到的 include —— 搬它会让 app 编译失败。
            if 'cmsis_os' in s:
                continue
            out.append(l)
    return out


def find_extern_block(lines):
    """取出工程 main.c 里"CubeMX 句柄 + 初始化函数"那整块声明。

    ⚠️ 为什么必须带上它（2026-10-11 编译预演当场发现的缺漏）：
       本工程 CubeMX 是"**按外设生成 .c/.h**"的：
         · 句柄**定义**在 usart.c / can.c / iwdg.c / spi.c 里，声明在
           usart.h / can.h / ... 里；
         · `MX_*_Init()` 声明在 gpio.h / dma.h / usart.h / can.h / iwdg.h 里。
       **但生产模板既没有这些 extern，也没有包含那些外设头**（模板注释里
       以为 `main.h` 带出来了，实际这个工程的 `main.h` 里一个 MX_ 都没有）。
       所以直接拼出来的 main.c 会报一堆
       `implicit declaration of function 'MX_GPIO_Init'`（**编译期**就炸）。
       工程 main.c 自己写了这块声明，这里**整块逐字搬过去**，不新写代码。

    按内容定位：从带"CubeMX 生成的全局句柄"的注释开始，往后连续取
    `extern ...;` 与空行，直到出现别的东西为止。
    """
    i0 = None
    for i, l in enumerate(lines):
        if "CubeMX 生成的全局句柄" in l or "CubeMX 生成的初始化函数" in l:
            i0 = i
            break
    if i0 is None:
        return []
    # 注释块可能以 /* 开头，往前收一行
    if i0 > 0 and lines[i0 - 1].lstrip().startswith("/*"):
        i0 -= 1
    out = []
    for l in lines[i0:]:
        s = l.strip()
        if s == "" or s.startswith("extern ") or s.startswith("/*") \
           or s.startswith("*") or s.endswith("*/"):
            out.append(l)
        else:
            break
    # 去掉尾部多余空行
    while out and out[-1].strip() == "":
        out.pop()
    return out


def find_tail_start(lines):
    """定位 CubeMX 尾巴的起点：SystemClock_Config 之前那段注释块的 /* 行。

    ⚠️ 按内容而不是行号定位 —— 行号会因为用户在尾巴里加注释而失效。
    """
    i_fn = None
    for i, l in enumerate(lines):
        if l.startswith("void SystemClock_Config(void)"):
            i_fn = i
            break
    if i_fn is None:
        return None
    # 往上找最近的、以 /* 开头的注释块起始行
    for j in range(i_fn, -1, -1):
        if lines[j].lstrip().startswith("/*"):
            return j
    return None


def find_gcc():
    """找 arm-none-eabi-gcc：优先 $GW_GCC，其次 CubeIDE 自带的。"""
    env = os.environ.get("GW_GCC", "")
    if env and os.path.isfile(env):
        return env
    base = r"D:\stm32cubeIDE"
    if os.path.isdir(base):
        for root, dirs, files in os.walk(base):
            if "arm-none-eabi-gcc.exe" in files and "gnu-tools-for-stm32" in root:
                return os.path.join(root, "arm-none-eabi-gcc.exe")
    return None


def include_dirs(project):
    """拼出编译工程 main.c 需要的所有 -I。

    ⚠️ 这份清单是**实测**得出的（2026-10-11 预演时逐个补的）：
       · CubeMX 工程的 Core/Inc + Drivers（HAL/CMSIS，按实际存在的目录挑）
       · 仓库的 inc / port / app / drivers / esp32/include
    """
    dirs = []
    for rel in ("Core\\Inc",):
        p = os.path.join(project, rel)
        if os.path.isdir(p):
            dirs.append(p)
    drv = os.path.join(project, "Drivers")
    for rel in ("STM32F1xx_HAL_Driver\\Inc",
                "STM32F1xx_HAL_Driver\\Inc\\Legacy",
                "CMSIS\\Device\\ST\\STM32F1xx\\Include",
                "CMSIS\\Include"):
        p = os.path.join(drv, rel)
        if os.path.isdir(p):
            dirs.append(p)
    for rel in ("inc", "port", "app", "drivers", "esp32\\include"):
        p = os.path.join(REPO, rel)
        if os.path.isdir(p):
            dirs.append(p)
    return dirs


def kernel_include_dirs(project):
    """FreeRTOS 内核头文件路径（优先级：工程内 Middlewares -> FW 包）

    ⚠️ 2026-10-11 实测：verify_compile 原来**不定义 GW_USE_FREERTOS**，
       于是生产模板里的 RTOS 分支根本没被编译 —— "编译通过"是假的。
       补上宏之后就需要内核头（FreeRTOS.h / task.h / cmsis_os.h），
       否则会以 `fatal error: FreeRTOS.h: No such file` 失败。
    """
    out = []
    base = os.path.join(project, "Middlewares", "Third_Party", "FreeRTOS", "Source")
    cands = [os.path.join(base, "include"),
             os.path.join(base, "portable", "GCC", "ARM_CM3"),
             os.path.join(base, "CMSIS_RTOS")]
    for d in cands:
        if os.path.isdir(d):
            out.append(d)
    if out:
        return out
    roots = []
    if os.environ.get("GW_FW_ROOT"):
        roots.append(os.environ["GW_FW_ROOT"])
    roots.append(os.path.join(os.environ.get("USERPROFILE", ""),
                              "STM32Cube", "Repository"))
    for r in roots:
        if not os.path.isdir(r):
            continue
        for d in sorted(os.listdir(r), reverse=True):
            if d.startswith("STM32Cube_FW_F1"):
                s = os.path.join(r, d, "Middlewares", "Third_Party", "FreeRTOS", "Source")
                for sub in ("include", os.path.join("portable", "GCC", "ARM_CM3")):
                    if os.path.isdir(os.path.join(s, sub)):
                        out.append(os.path.join(s, sub))
                if out:
                    return out
    return out


def verify_compile(project, src_path):
    """对生成出来的 main.c 做一次真实语法检查（-Wall，要求 0 诊断）。

    ⚠️ 为什么非要有这一步（2026-10-11 教训）：
       "拼好了"不等于"编得过"。上一轮手工预演时，拼出来的文件第一版直接报
       `'huart1' undeclared` 与 `implicit declaration of MX_GPIO_Init` ——
       两处缺漏都是**读文档永远看不出来、只有真编才暴露**的。
       把它做成工具后，CI 里就能天天跑，模板或工程任何一侧漂移都会当场暴露。
    """
    gcc = find_gcc()
    if gcc is None:
        print("  [FAIL] 找不到 arm-none-eabi-gcc（可设 GW_GCC 指定）")
        return 1

    args = [gcc, "-fsyntax-only", "-std=gnu11", "-mcpu=cortex-m3", "-mthumb",
            "-Wall", "-DUSE_HAL_DRIVER", "-DSTM32F103xE",
            # ★ 落地目标就是 RTOS 版：不定义它，模板里 #if GW_USE_FREERTOS
            #   那段**根本不会被编译**，"编译通过"就是假的（2026-10-11 修）
            "-DGW_USE_FREERTOS=1"]
    for d in include_dirs(project) + kernel_include_dirs(project):
        args += ["-I" + d]
    args.append(src_path)

    try:
        p = subprocess.run(args, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT)
    except Exception as e:                      # noqa: BLE001
        print("  [FAIL] 调用编译器失败：%s" % e)
        return 1

    out = p.stdout.decode("utf-8", "replace")
    # ⚠️ 原来只认 ": error:" / ": warning:" —— 于是 `fatal error:`（例如
    #    找不到 FreeRTOS.h）**被漏掉**，"编译不过"却显示"0 条诊断"。
    lines = [l for l in out.splitlines()
             if (": warning:" in l or ": error:" in l
                 or l.lower().startswith(("fatal error", "error:")))]
    if p.returncode != 0 or lines:
        print("  [FAIL] 生成的 main.c **编译不过**（%d 条诊断）：" % len(lines))
        for l in lines[:10]:
            print("         %s" % l.strip()[:150])
        print("         多半是「生产模板」与「工程 CubeMX 尾巴/声明」之间漂移了 ——")
        print("         检查上面报的符号，把对应声明块加进本工具的携带列表。")
        if not lines:
            print("         （编译器没有输出诊断行，原始输出尾部：）")
            for l in out.splitlines()[-6:]:
                print("         %s" % l.strip()[:150])
        return 1
    print("  [ OK ] 生成的 main.c 编译通过（-Wall，0 诊断）")
    return 0


def _assemble(tpl, externs, tail, out_path, prj_main):
    """把三段拼起来写到 out_path。

    ⚠️ 只做**逐字拼接**：三块内容全部来自文件，不新写任何 C 语句。
    """
    try:
        commit = subprocess.check_output(
            ["git", "-C", REPO, "rev-parse", "--short", "HEAD"],
            stderr=subprocess.DEVNULL).decode().strip()
    except Exception:                               # noqa: BLE001
        commit = "unknown"

    header = [
        "/* ============================================================================",
        " *  ⚠️ 本文件由 tools/make_landing_main.py **拼接生成**，不要手工改它 ——",
        " *     改模板（templates/main_template.c）或工程 CubeMX 尾巴，然后重新生成。",
        " *",
        " *  来源：",
        " *    第 1 段  templates/main_template.c（生产版：OTA 桥 + 上报节拍 + RTOS 分支）",
        " *    第 2 段  工程 main.c 自己的句柄/MX_*_Init 声明块（逐字搬）",
        " *    第 3 段  %s 的 CubeMX 尾巴" % prj_main,
        " *             （SystemClock_Config / HAL_TIM_PeriodElapsedCallback(TIM4) /",
        " *               Error_Handler / assert_failed）",
        " *  仓库 commit : %s" % commit,
        " *  生成时间     : %s" % time.strftime("%Y-%m-%d %H:%M:%S"),
        " *",
        " *  ⚠️ 生成后**必须**编译验证一次 —— 直接用：",
        " *       python tools/make_landing_main.py --out build/landing_main.c --verify-compile",
        " * ============================================================================ */",
        "",
    ]

    # ⚠️ 声明块必须插在**第一次使用之前**（第一版插到文件末尾 ->
    #    "huart1 undeclared"：C 要求先声明后使用，这跟"链接期才报错"不同，
    #    是**编译期**就炸。）
    i_last_inc = max(i for i, l in enumerate(tpl)
                     if l.lstrip().startswith("#include"))

    out_lines = (header
                 + tpl[:i_last_inc + 1]
                 + ["",
                    "/* ---- 句柄 / MX_*_Init 声明块：从工程 main.c 逐字搬来 ----",
                    " *  ⚠️ 本工程 CubeMX 是\"按外设生成 .c/.h\"：句柄与 MX_*_Init 的**定义**",
                    " *     在 usart.c / can.c / gpio.c / iwdg.c / spi.c / dma.c 里，声明在",
                    " *     对应的 .h 里；而这个工程的 main.h **一个 MX_ 都没有**。生产模板",
                    " *     没有这块 -> 不搬会报 'huart1 undeclared' 与一堆",
                    " *     'implicit declaration of MX_GPIO_Init'（**编译期**，不是链接期）。",
                    " *     （2026-10-11 手工预演当场发现；现在 CI 会跑 --check --verify-compile）*/"]
                 + externs
                 + tpl[i_last_inc + 1:]
                 + [""] + tail)

    d = os.path.dirname(out_path)
    if d and not os.path.isdir(d):
        os.makedirs(d, exist_ok=True)
    io.open(out_path, 'w', encoding='utf-8', newline='\n').write(
        "\n".join(out_lines) + "\n")
    print("  [ OK ] 已生成 %s（%d 行）" % (out_path, len(out_lines)))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--project",
                    default=os.environ.get("GW_CUBE_PROJECT",
                                           r"D:\stm32cubeIDE\gateway2"))
    ap.add_argument("--out", default="")
    ap.add_argument("--check", action="store_true",
                    help="只做前置检查，不写文件")
    ap.add_argument("--verify-compile", action="store_true",
                    help="装配后立刻做一次真实语法检查（-Wall，要求 0 诊断）")
    a = ap.parse_args()

    prj_main = os.path.join(a.project, "Core", "Src", "main.c")
    if not os.path.isfile(prj_main):
        print("  [FAIL] 找不到工程的 main.c：%s" % prj_main)
        return 1

    tpl_path = os.path.join(REPO, "templates", "main_template.c")
    tpl = read_lines(tpl_path)
    prj = read_lines(prj_main)

    # ---- 前置检查 1：工程那份是**哪一种变体**（都接受）----
    #
    # 2026-10-11 落地当天实测：CubeMX 一重新生成，工程 main.c 就从"L0 版衍生物"
    # 变成**标准骨架**（218 行，main() 里只有 HAL_Init / SystemClock_Config /
    # MX_*_Init / MX_FREERTOS_Init）。原门禁要求"必须带 L0 标记"，于是**误伤了
    # 正常流程**（第一次跑就撞上）。
    # 机制上放宽是安全的：本工具**丢弃源文件的 main()**，只用它的 extern 声明块
    # 与 CubeMX 尾部 —— 而这两样下面都有独立检查，拼不出来会当场报错。
    prj_txt = "\n".join(prj)           # 供下面的变体判断用
    miss_l0 = [m for m in L0_MARKS if m not in prj_txt]
    already_prod = ("app_housekeeping" in prj_txt) or ("gw_rtos_create_tasks" in prj_txt)
    is_skeleton = (("/* USER CODE BEGIN" in prj_txt) and ("HAL_Init();" in prj_txt)
                   and ("SystemClock_Config" in prj_txt))

    if already_prod:
        if a.check:
            # ⚠️ 落地完成后**本来就是**生产版 —— 检查模式下这是期望结果（幂等），
            #    否则 CI 的"落地预演"步骤会一直红。装配模式仍然拒绝。
            print("  [ OK ] 工程 main.c 已是**生产版**（落地已完成；"
                  "检查模式视为通过）")
            return 0
        print("  [FAIL] 工程 main.c 看起来**已经是生产版**"
              "（含 app_housekeeping / gw_rtos_create_tasks）。")
        print("         本工具是单向的（骨架/L0 -> 生产版），重复装配会破坏它；")
        print("         要重拼请先从备份恢复骨架或 L0 版。")
        return 2
    if not miss_l0:
        print("  [ OK ] 工程 main.c = **L0 版衍生物**（L0 标记 %d/%d）"
              % (len(L0_MARKS), len(L0_MARKS)))
    elif is_skeleton:
        print("  [ OK ] 工程 main.c = **CubeMX 骨架**（落地当天的正常输入；"
              "声明块与 CubeMX 尾部照常提取）")
    else:
        print("  [FAIL] 工程 main.c 既不是 L0 版衍生物，也不是 CubeMX 骨架。")
        print("         缺 L0 标记：%s" % ", ".join(miss_l0))
        print("         也找不到 /* USER CODE BEGIN + HAL_Init(); + SystemClock_Config。")
        print("         本脚本拒绝猜着拼 —— 请先确认工程 main.c 的来源。")
        return 2

    # ---- 前置检查 2：能定位尾巴，且尾巴里四段都在 ----
    i_tail = find_tail_start(prj)
    if i_tail is None:
        print("  [FAIL] 在工程 main.c 里找不到 SystemClock_Config() 前的注释块"
              "（尾巴边界定位失败）")
        return 1
    tail = prj[i_tail:]
    tail_txt = "\n".join(tail)
    miss_tail = [t for t in TAIL_NEEDS if t not in tail_txt]
    if miss_tail:
        print("  [FAIL] 工程 main.c 的尾巴里缺这些（生产模板会链接不过）：")
        for t in miss_tail:
            print("           %s" % t)
        return 1
    print("  [ OK ] CubeMX 尾巴定位成功：第 %d 行起，共 %d 行，四段齐全"
          % (i_tail + 1, len(tail)))

    if is_skeleton:
        _before = len(tail)
        tail = drop_main(tail)          # 骨架尾巴含自己的 main() -> 必须去掉
        print('  [ OK ] 骨架路径：尾巴里去掉自带的 main()（%d -> %d 行）'
              % (_before, len(tail)))

    # ---- 前置检查 3：生产模板的复制区标记在 ----
    if not any('复制开始' in l for l in tpl) or \
       not any('复制结束' in l for l in tpl):
        print("  [FAIL] templates/main_template.c 里没有复制区标记（结构变了）")
        return 1
    print("  [ OK ] 生产模板：%d 行（含复制区标记）" % len(tpl))

    # ---- 前置检查 4：按变体取"声明来源" ----
    #   L0 版：工程 main.c 自己写了 extern 句柄 + MX_*_Init 原型 -> 搬那一块
    #   骨架  ：那些声明都在外设头里（usart.h/can.h/gpio.h/dma.h/iwdg.h），
    #           骨架 main.c #include 了它们 -> 搬 #include 块
    if is_skeleton:
        # 骨架 main() 之前的声明行（如 void SystemClock_Config(void);）必须提到
        # 模板之前，否则模板里的调用是隐式声明（-Wall 会报，而预演要求 0 诊断）
        i_main = next((x for x, l in enumerate(prj)
                       if l.startswith('int main(void)')), len(prj))
        predecls = [l for l in prj[:i_main]
                    if l.strip() and not l.strip().startswith(('#', '/*', '*', '//'))]
        externs = find_include_block(prj) + predecls
        if not externs:
            print("  [FAIL] 骨架 main.c 里找不到 #include 块")
            return 1
        print("  [ OK ] 骨架路径：搬 %d 行 #include（声明由 usart.h/can.h/gpio.h "
              "等提供；最终由 --verify-compile 兜底）" % len(externs))
    else:
        externs = find_extern_block(prj)
        if not externs:
            print("  [FAIL] 工程 main.c 里找不到 `extern ...HandleTypeDef ...;` 声明")
            print("         生产模板没有这些声明，拼出来会报 'huartN undeclared'")
            return 1
        print("  [ OK ] L0 路径：搬 %d 行 extern 句柄声明（逐字搬过去）"
              % len(externs))

    if a.check:
        if a.verify_compile:
            # 只检查 + 编译验证：装配到临时文件，编完删掉
            tmp = os.path.join(REPO, "build", "_landing_main_syntaxcheck.c")
            os.makedirs(os.path.dirname(tmp), exist_ok=True)
            rc = _assemble(tpl, externs, tail, tmp, prj_main)
            if rc != 0:
                return rc
            v = verify_compile(a.project, tmp)
            try:
                os.remove(tmp)
            except OSError:
                pass
            if v != 0:
                return v
            print("  ==> 前置条件满足 + 编译通过（--check --verify-compile）")
            return 0
        print("  ==> 拼接前置条件全部满足（--check 模式，未写文件）")
        return 0

    if not a.out:
        print("  [FAIL] 需要 --out 指定输出文件（或加 --check 只检查）")
        return 1

    out_path = a.out if os.path.isabs(a.out) else os.path.join(REPO, a.out)
    if _assemble(tpl, externs, tail, out_path, prj_main) != 0:
        return 1

    if a.verify_compile:
        if verify_compile(a.project, out_path) != 0:
            return 1
        print("  ==> 生成 + 编译验证都通过；下一步把它覆盖成 "
              "<工程>/Core/Src/main.c")
    else:
        print("      ⚠️ 未做编译验证（建议加 --verify-compile）")
    return 0


if __name__ == '__main__':
    sys.exit(main())
