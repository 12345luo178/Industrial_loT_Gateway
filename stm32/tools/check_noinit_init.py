# -*- coding: utf-8 -*-
"""
检查 `.noinit` 段的变量是否带非零初始化器

============================================================================
 为什么需要这个检查

ARM GCC 会对 `.noinit` 里的非零初始化器直接报错：

    error: only zero initializers are allowed in section '.noinit'

但**主机 gcc 不报这个错**。本项目的 PC 测试全部用主机 gcc，
所以这个错误只会在交叉编译时才暴露 —— 而交叉编译是 CI 的最后一步，
发现问题时已经离改动很远了。

更糟的情况是：如果 CI 里没跑交叉编译，这个错误会一直藏到上板。

本脚本用**源码扫描**的方式提前发现它：找 `section(".noinit")` 的声明，
检查紧随其后的变量有没有 `= 非零值`。

============================================================================
 判定规则的边界（诚实说明）

这是**文本扫描**，不是编译器。它可能：

  漏报：
    - `= {0}` 之外的聚合初始化写成 `= {1, 2}`（会被抓到，因为含非零）
    - 变量与属性不在相邻行（用了宏包装）
    - 属性写在变量名之后（`int x __attribute__((section(...)));`）

  误报：
    - `= 0` / `= {0}` / `= NULL` 是**合法**的（零初始化），不报
    - 注释里出现的 `section(".noinit")` 会被跳过

所以它的定位是"**廉价的早期预警**"，不是"替代编译器"。
真正的保证来自 CI 里的交叉编译那一步。
============================================================================
"""
import re
import sys
import glob
import os

# 合法：零初始化的各种写法
ZERO_INIT = re.compile(r'=\s*(0[uUlL]*|NULL|\{\s*0\s*\}|"")?\s*;')

def strip_comments(text):
    """去掉 /* */ 与 // 注释，避免把注释里的示例当成代码"""
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")          # 保留一个空格，避免拼接出新的 token
        elif text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            out.append(" ")
        elif text[i] == '"':         # 字符串字面量：整段跳过
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:min(j + 1, n)])
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def check_file(path):
    raw = open(path, encoding="utf-8", errors="replace").read()
    src = strip_comments(raw)

    problems = []

    for m in re.finditer(r'section\s*\(\s*"\.noinit[^"]*"\s*\)', src):
        # 属性之后到分号之间的内容 = 被声明的变量（可能跨行）
        tail = src[m.end(): m.end() + 400]
        semi = tail.find(";")
        if semi < 0:
            continue
        decl = tail[:semi]

        # 提取"= 初始值"部分
        eq = decl.find("=")
        if eq < 0:
            continue                      # 无初始化器：合法
        init = decl[eq:]

        # 零初始化是合法的
        if ZERO_INIT.match(init.strip()) or re.match(r'^=\s*0[uUlL]*\s*$', init.strip()):
            continue
        if re.match(r'^=\s*\{\s*0\s*\}\s*$', init.strip()):
            continue

        # 找到行号
        line = raw[:m.start()].count("\n") + 1
        problems.append((line, decl.strip().replace("\n", " ")[:90]))

    return problems


def main():
    roots = sys.argv[1:] or ["src", "app", "port", "drivers"]
    files = []
    for r in roots:
        if os.path.isfile(r):
            files.append(r)
        else:
            files += glob.glob(os.path.join(r, "**", "*.c"), recursive=True)
            files += glob.glob(os.path.join(r, "**", "*.h"), recursive=True)

    total = 0
    for f in sorted(files):
        for line, decl in check_file(f):
            print("  [问题] %s:%d" % (f.replace("\\", "/"), line))
            print("         %s" % decl)
            print("         -> .noinit 里只允许零初始化器（ARM GCC 会拒绝非零初值）")
            print("         -> 改成不写初值，在运行期填（见 gw_fault_boot_init）")
            total += 1

    if total == 0:
        print("  [通过] 未发现 .noinit 的非零初始化器（扫描 %d 个文件）" % len(files))
        return 0

    print("  [失败] 发现 %d 处" % total)
    return 1


if __name__ == "__main__":
    sys.exit(main())
