# -*- coding: utf-8 -*-
"""
检查纯逻辑模块的公开 API 是否都被测试引用过

============================================================================
 为什么需要这个检查

这个项目的主打特性是"协议逻辑与硬件解耦，可在 PC 上跑单元测试"，
测试数量（2200+ 项）经常被当成质量的证据。

但**测试数量多不等于覆盖全**：本轮用它扫出 6 个公开 API
一个测试都没有引用过，其中：

  - `mb_master_current_index`  有文档意图（"轮询进度，用于上报告警"），
                               但既没调用方也没测试
  - `gw_pipeline_sync_from_regs` 刻意预留的扩展点，没人验证它现在是空操作
  - `gw_boot_err_str`          被 Bootloader 用它打印失败原因，没有测试
  - `mb_master_slave_count` / `slave_at` / `abort_pending`
                               被 gw_app.c 用（集成路径），仅缺单元测试

这类问题**单看测试数字发现不了**。所以加这个检查，
让"新增一个公开 API 却忘了测"在 CI 里直接暴露。

============================================================================
 判定方式与边界（诚实说明）

做法：从 `inc/*.h` 里抽出函数声明，在 `test/*.c` 里找同名标识符。

  - 这是**文本匹配**，不是真正的覆盖率统计（那需要 gcov 与可执行文件）。
  - 它只能证明"测试源码里提到了这个名字"，
    **不能证明那个测试真的断言了它的行为**。
  - 换句话说：它能发现"完全没测"，但发现不了"测得很差"。

对"防止漏测"这个目的来说，这个强度够用；对"证明质量"来说不够。
所以 README 里描述它时要如实说清这一点。
============================================================================
"""
import collections
import glob
import os
import re
import sys

# 纯逻辑模块（src/ 下，能在 PC 上编译测试）
PURE_MODULES = set(os.path.basename(p)[:-2] for p in glob.glob("src/*.c"))

# 有意的豁免：这些模块的 API 属于"仅上板可测"，不要求 PC 测试
EXEMPT = set()

DECL_RE = re.compile(
    r"^[A-Za-z_][\w \t\*]*?\b(\w+)\s*\([^;{]*\)\s*;", re.M
)
KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "defined",
            "else", "do", "case", "break", "continue"}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def collect_apis():
    apis = collections.defaultdict(list)
    for h in sorted(glob.glob("inc/*.h")):
        mod = os.path.basename(h)[:-2]
        if mod not in PURE_MODULES or mod in EXEMPT:
            continue
        src = strip_comments(open(h, encoding="utf-8", errors="replace").read())
        for m in DECL_RE.finditer(src):
            n = m.group(1)
            if n in KEYWORDS:
                continue
            apis[mod].append(n)
    return apis


def collect_test_text():
    """收集测试源码里出现的标识符（**先剥离注释**）

    ⚠️ 必须先剥注释 —— 否则"注释里提到过函数名"会被当成"测过了"。
       这个漏洞是故障注入发现的：我把测试里的 `gw_boot_err_str(` 改名后
       检查器仍然报通过，因为文件顶部的说明注释里还有这个名字。

       注释不是测试。剥离之后才算真正的"代码里引用了它"。
    """
    text = ""
    for t in glob.glob("test/*.c") + glob.glob("esp32/test/*.c"):
        text += strip_comments(open(t, encoding="utf-8", errors="replace").read())
    return text


def main():
    apis = collect_apis()
    testtext = collect_test_text()

    total = 0
    uncovered = []
    for mod in sorted(apis):
        for f in apis[mod]:
            total += 1
            # 用单词边界匹配，避免把 foo_bar 当成 foo 的引用
            if not re.search(r"\b" + re.escape(f) + r"\b", testtext):
                uncovered.append((mod, f))

    if not uncovered:
        print("  [通过] %d 个公开 API 都有测试引用（%d 个纯逻辑模块）"
              % (total, len(apis)))
        return 0

    print("  [失败] %d / %d 个公开 API 没有任何测试引用：" % (len(uncovered), total))
    for mod, f in uncovered:
        print("        %-18s %s" % (mod, f))
    print("        -> 要么补测试，要么删掉（死代码更该删）")
    return 1


if __name__ == "__main__":
    sys.exit(main())
