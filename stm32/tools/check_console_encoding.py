#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
检查 tools/*.py 在**默认中文 Windows 控制台**里是否会自己崩掉

============================================================================
 为什么需要这一步

  Windows 上 Python 按**控制台代码页**编码 stdout（中文环境是 cp936/GBK），
  而 errors 是默认的 'strict'。于是只要 print 里出现 GBK 编不出来的字符
  （⚠️ U+26A0 / ✅ U+2705 / ❌ U+274C …），脚本就会抛

      UnicodeEncodeError: 'gbk' codec can't encode character ...

  这个错误发生在**打印阶段**，通常远早于干活的阶段 ——
  脚本看起来"跑了"，实际一行有用的事都没做。

============================================================================
 ⚠️ 这个检查是被一次真实事故逼出来的（2026-10-10）

  上板验证"主从共存"时，两个关键脚本就是这么死的：

      pc_slave_sim.py       在第 154 行 print("⚠️ 按 Ctrl-C 停止...") 死掉
      verify_master_dual.py 在第 185 行 print("⚠️ 主站轮询周期...") 死掉

  结果是"PC 从机没启动 / 主站修复没验证成功"，白查了一天。

  ⚠️ 而 tools/ci.ps1 里设了 `$env:PYTHONIOENCODING = "utf-8"`，
      **CI 里永远不会复现** —— 全绿。这类"只在用户手敲时才出现"的缺陷
      CI 抓不到，只能靠源文本检查。

============================================================================
 判定方式

  对每个 tools/*.py：
    1) 找出 print(...) 调用里 GBK 编不出来的字符
    2) 若有，则要求该文件带编码守卫（`.reconfigure(errors="replace")`）
       —— 放过注释/docstring 里的字符：它们不经过 print，不会崩

  守卫的标准写法（放在 import sys 之后）：

      for _stream in (sys.stdout, sys.stderr):
          try:
              _stream.reconfigure(errors="replace")
          except (AttributeError, ValueError):
              pass

  ⚠️ 用 errors="replace" 而不是 encoding="utf-8"：
     中文控制台下强制 UTF-8 会把已成习惯的中文输出变成乱码，
     而 replace 只把编不出来的那几个符号降级成 '?'，中文照常。
============================================================================
"""
import glob
import io
import os
import re
import sys

GUARD = 'reconfigure(errors="replace")'

PRINT_RE = re.compile(r"print\s*\((?:[^()]|\([^()]*\))*\)", re.S)


def unencodable(text):
    """返回 text 里 GBK 编不出来的字符（去重后按码位排序）"""
    bad = set()
    for ch in text:
        if ch in "\n\r\t":
            continue
        try:
            ch.encode("gbk")
        except UnicodeEncodeError:
            bad.add(ch)
    return sorted(bad)


def main():
    bad = []

    for path in sorted(glob.glob(os.path.join("tools", "*.py"))):
        src = io.open(path, encoding="utf-8").read()
        # 只统计 print(...) 里的字符
        in_print = "".join(m.group(0) for m in PRINT_RE.finditer(src))
        chars = unencodable(in_print)
        if not chars:
            continue
        if GUARD in src:
            continue
        bad.append((path.replace("\\", "/"), chars))

    if not bad:
        n = len(glob.glob(os.path.join("tools", "*.py")))
        print("  [通过] %d 个工具脚本：会在 GBK 控制台崩的 print 都有编码守卫" % n)
        return 0

    print("  [问题] 以下脚本在默认中文控制台（GBK）会自己崩掉：")
    for path, chars in bad:
        detail = " ".join("%s(U+%04X)" % (c, ord(c)) for c in chars)
        print("    %s" % path)
        print("        print 里的非 GBK 字符: %s" % detail)
    print()
    print("  处理：在该文件的 import sys 之后加编码守卫 ——")
    print("      for _stream in (sys.stdout, sys.stderr):")
    print("          try:")
    print('              _stream.reconfigure(errors="replace")')
    print("          except (AttributeError, ValueError):")
    print("              pass")
    print()
    print("  ⚠️ 不要靠 PYTHONIOENCODING：文档里让人手敲的命令")
    print("     是在**裸控制台**里跑的，CI 设了那个变量所以永远看不出来。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
