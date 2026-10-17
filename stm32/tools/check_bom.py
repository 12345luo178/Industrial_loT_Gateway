# -*- coding: utf-8 -*-
"""
检查源文件里有没有混进 UTF-8 BOM

============================================================================
 为什么需要这个检查

UTF-8 BOM（`EF BB BF`）在源码里是**有害**的：

  1) **GCC 会警告**（`-Wall` 下不一定报，但严格模式会）：
     某些编译器/工具链把它当成文件内容的一部分。

  2) **它会被当成一个字符计入源码位置** ——
     报错的行列号会偏移，与编辑器显示的不一致，排查时对不上。

  3) **它可能破坏 shebang 或宏** ——
     如果 BOM 出现在 `#include` 或 `#define` 之前，
     预处理器的行为取决于实现。

  4) 本项目还有一层特殊原因：
     **Windows PowerShell 的 `Set-Content -Encoding UTF8` 会加 BOM**，
     而 `.ps1` 文件**需要** BOM（否则 PS 5.1 按 ANSI 解码，中文乱码）。
     所以"要不要 BOM"取决于文件类型 —— 这种不对称很容易出错：
     我用 PowerShell 恢复一个 `.c` 文件时，就给它带进了 BOM。

============================================================================
 规则

  `.c` / `.h` / `.py` / `.md` / `.ld`  -> **不要** BOM
  `.ps1`                              -> **要** BOM（PS 5.1 需要）

  这个区分是实测得出的，不是猜的。
============================================================================
"""
import glob
import os
import sys

# 需要 BOM 的文件类型（PowerShell 脚本：PS 5.1 无 BOM 时按 ANSI 解码）
NEED_BOM = (".ps1",)

# 不允许 BOM 的文件类型
NO_BOM = (".c", ".h", ".py", ".md", ".ld", ".yml", ".yaml", ".txt", ".s")

BOM = b"\xef\xbb\xbf"

SCAN = [
    "src/*.c", "src/*.h",
    "app/*.c", "app/*.h",
    "port/*.c", "port/*.h",
    "drivers/*.c", "drivers/*.h",
    "inc/*.h",
    "test/*.c", "test/*.h",
    "esp32/src/*.c", "esp32/include/*.h", "esp32/test/*.c",
    "templates/*.c",
    "tools/*.py", "tools/*.ps1",
    "tools/*/*.h",
    "doc/*.md", "*.md",
    ".github/workflows/*.yml",
]


def main():
    no_bom_bad = []      # 不该有 BOM 却有
    need_bom_bad = []    # 该有 BOM 却没有
    checked = 0

    files = []
    for pat in SCAN:
        files += glob.glob(pat)

    for f in sorted(set(files)):
        if not os.path.isfile(f):
            continue
        ext = os.path.splitext(f)[1].lower()
        raw = open(f, "rb").read()
        has = raw.startswith(BOM)
        checked += 1

        if ext in NO_BOM and has:
            no_bom_bad.append(f)
        elif ext in NEED_BOM and not has:
            need_bom_bad.append(f)

    for f in no_bom_bad:
        print("  [问题] %s 带 UTF-8 BOM（%s 类型不该有）" % (f.replace("\\", "/"),
                                                        os.path.splitext(f)[1]))
        print("         -> GCC 会把 BOM 当成内容，报错的行列号会偏移；")
        print("         -> 诱因通常是 PowerShell 的 Set-Content -Encoding UTF8")
    for f in need_bom_bad:
        print("  [问题] %s 缺 UTF-8 BOM（PowerShell 脚本需要）" % f.replace("\\", "/"))
        print("         -> PS 5.1 会按 ANSI 解码，中文注释与字符串里的引号会失配")

    if not no_bom_bad and not need_bom_bad:
        print("  [通过] BOM 检查正常（扫描 %d 个文件）" % checked)
        return 0

    print("  [失败] %d 个文件 BOM 不正确" % (len(no_bom_bad) + len(need_bom_bad)))
    return 1


if __name__ == "__main__":
    sys.exit(main())
