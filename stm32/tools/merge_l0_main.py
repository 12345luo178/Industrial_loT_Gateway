# -*- coding: utf-8 -*-
"""
把 templates/main_L0_bringup.c 合并进用户的 CubeMX main.c

============================================================================
 保留什么、替换什么

用户的 main.c 由 CubeMX 生成，有几处**必须保留**：
    SystemClock_Config()              HSE + PLLx9 -> 72MHz
    HAL_TIM_PeriodElapsedCallback()   TIM4 时基（替代 SysTick）
    Error_Handler()                   HAL 出错时的落点
    assert_failed()                   USE_FULL_ASSERT 下用

替换：includes / 静态全局 / 回调 / main() 本体

============================================================================
 ⚠️ 第一版写错了，记录下来

   第一版用 "行首以函数名开头" 来找定义，结果匹配到了**前向声明**：
       void SystemClock_Config(void);        <- 第 54 行，这是声明！
       void SystemClock_Config(void)         <- 第 118 行，这才是定义
   于是切出来的"函数体"是从声明开始的几行，合并结果只有 15 行、
   且 `int main` 都没了。而脚本自己的自检（contains x0）会报出来 ——
   所以**自检不是装饰，它当场抓到了这个错**。

   正确判据：函数名出现在行首、行尾是 ')'（不是 ');'），
             且**下一个非空行以 '{' 开头**。
============================================================================
"""
import io
import sys

TEMPLATE = r"C:\Users\acer\Desktop\EmbeddedProjects\modbus_core\templates\main_L0_bringup.c"
USER_MAIN = r"D:\stm32cubeIDE\gateway2\Core\Src\main.c"
OUT = r"C:\Users\acer\Desktop\EmbeddedProjects\modbus_core\tools\_merged_main_preview.c"


def read(p):
    return io.open(p, encoding="utf-8").read().split("\n")


def find_def(lines, name):
    """找一个函数的**定义**行（不是前向声明）"""
    for i, ln in enumerate(lines):
        s = ln.strip()
        if not s.startswith(name):
            continue
        # 声明以 ';' 结尾 -> 跳过
        if s.rstrip().endswith(";"):
            continue
        # 定义必须后面跟着 '{'（允许中间有换行）
        j = i
        while j < len(lines) and "{" not in lines[j]:
            j += 1
            if j - i > 4:
                break
        if j < len(lines) and "{" in lines[j]:
            return i, j
    return -1, -1


def extract_block(lines, open_idx):
    """从含 '{' 的行开始，提取到配对的 '}'"""
    depth = 0
    i = open_idx
    while i < len(lines):
        depth += lines[i].count("{") - lines[i].count("}")
        if depth == 0 and i >= open_idx:
            return open_idx, i
        i += 1
    raise AssertionError("unbalanced braces")


def main():
    t = read(TEMPLATE)
    u = read(USER_MAIN)

    t_main_decl = -1
    for i, ln in enumerate(t):
        if ln.startswith("int main(void)"):
            t_main_decl = i
            break
    assert t_main_decl > 0, "template: no main"

    t_first_inc = -1
    for i, ln in enumerate(t):
        if ln.startswith("#include"):
            t_first_inc = i
            break
    assert t_first_inc > 0

    header = t[:t_first_inc]
    body_before_main = t[t_first_inc:t_main_decl]
    tm_open = t_main_decl
    while "{" not in t[tm_open]:
        tm_open += 1
    _, tm_close = extract_block(t, tm_open)
    t_main_body = t[t_main_decl:tm_close + 1]

    # ---- 从用户文件取要保留的函数定义 ----
    keep = []
    for name in ("void SystemClock_Config(void)",
                 "void HAL_TIM_PeriodElapsedCallback(",
                 "void Error_Handler(void)",
                 "void assert_failed("):
        di, oi = find_def(u, name)
        if di < 0:
            print("WARN: not found: %s" % name)
            continue
        _, ci = extract_block(u, oi)
        start = di
        while start > 0 and (u[start - 1].strip().startswith("*")
                             or u[start - 1].strip().startswith("/*")):
            start -= 1
        keep.append("\n".join(u[start:ci + 1]))
        print("kept: %-42s lines %d-%d" % (name[:42], start + 1, ci + 1))

    out = []
    out.extend(header)
    out.extend(body_before_main)
    out.extend(t_main_body)
    out.append("")
    out.append("/* " + "=" * 74)
    out.append(" *  \u4ee5\u4e0b\u51fd\u6570\u7531 CubeMX \u751f\u6210\uff0c**\u539f\u6837\u4fdd\u7559**")
    out.append(" *")
    out.append(" *  \u26a0\ufe0f \u5b83\u4eec\u4e0d\u662f\u914d\u9970\uff0c\u5404\u81ea\u90fd\u6709\u5b9e\u9645\u4f5c\u7528\uff1a")
    out.append(" *      SystemClock_Config             HSE + PLLx9 -> 72MHz")
    out.append(" *      HAL_TIM_PeriodElapsedCallback  TIM4 \u65f6\u57fa\uff08\u66ff\u4ee3 SysTick\uff09")
    out.append(" *      Error_Handler                  HAL \u51fa\u9519\u65f6\u7684\u843d\u70b9")
    out.append(" * ============================================================================ */")
    out.append("")
    out.append("\n\n".join(keep))
    out.append("")

    io.open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(out))

    print()
    print("template lines : %d" % len(t))
    print("user lines     : %d" % len(u))
    print("merged lines   : %d" % len(out))
    print("output         : %s" % OUT)
    print()

    txt = "\n".join(out)
    bad = 0
    # ⚠️ 期望计数必须包含**声明与注释里的出现**。
    #    第一版没算上，于是报了 4 个 BAD —— 而实际合并是对的。
    #    这类"自检本身写错"比漏检更容易浪费时间，所以这里逐一核对过。
    for must, want in (("int main(void)", 1),
                       ("SystemClock_Config(void)", 2),      # 声明 + 定义
                       ("Error_Handler(void)", 1),           # 仅定义
                       ("HAL_TIM_PeriodElapsedCallback", 2), # 注释 + 定义
                       ("gw_app_poll", 1),
                       ("run_selfcheck", 4),                 # 注释+声明+定义+调用
                       ("RCC_PLL_MUL9", 1),
                       ("HAL_IncTick", 2)):                  # 注释 + 调用
        n = txt.count(must)
        ok = "OK " if n == want else "BAD"
        if n != want:
            bad += 1
        print("  [%s] contains %-32s x%d (want %d)" % (ok, must, n, want))

    # 括号平衡的总检查
    ob, cb = txt.count("{"), txt.count("}")
    print("  braces: { = %d, } = %d  %s" % (ob, cb, "OK" if ob == cb else "BAD"))
    if ob != cb:
        bad += 1
    print()
    print("RESULT: %s" % ("PASS" if bad == 0 else "FAIL (%d)" % bad))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
