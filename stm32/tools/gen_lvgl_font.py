#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从界面源码自动抓取中文字符，重新生成 LVGL 子集字库 port/lv_font_cn_16.c。

为什么要有这个脚本（2026-10-14）
--------------------------------
首次生成子集时，字符表是**手写**的汉字串，结果漏了「网」「关」
（标题栏 "网关 / 总览" 画成空白方块）和「无」（"CAN 无总线"）。
手写表 = 必然漏字，而且改文案时没人记得回头补。

本脚本把**界面源码当唯一真相源**：凡是出现在字符串字面量里的汉字，
一律收进子集。以后改文案只要重跑一次，不会再漏。

用法
----
    python tools/gen_lvgl_font.py                  # 生成（默认扫 app/gw_lvgl_ui.c）
    python tools/gen_lvgl_font.py --check          # 只检查缺字，不改文件（缺字返回码 1）
    python tools/gen_lvgl_font.py --src a.c b.c    # 指定扫描文件
    python tools/gen_lvgl_font.py --extra 备份恢复  # 额外并入一些汉字（预留）
    python tools/gen_lvgl_font.py --dry-run        # 只打印命令，不执行

依赖
----
    lv_font_conv（npm 包，`npm i lv_font_conv`）。
    默认从 DEFAULT_NODE / DEFAULT_LV_FONT_CONV 找，可用 --node / --lv-font-conv
    或环境变量 NODE / LV_FONT_CONV 覆盖。
"""

import argparse
import os
import re
import subprocess
import sys

# ---------------------------------------------------------------- 默认路径
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DEFAULT_SRC = ["app/gw_lvgl_ui.c"]
OUT_REL = "port/lv_font_cn_16.c"
FONT = r"C:\Windows\Fonts\simhei.ttf"

# 预留字表：这些字当前界面还没用上，但 CFG / DIAG 页迟早要用
# （配置/保存/确定/取消/诊断/标定/背光/看门狗/复位/电压/电流/频率…）。
# 并入它们只多花约 240 B flash，却能免掉"加个页面就要重烧一次板"的麻烦。
# 用 --no-reserved 可关掉，只按代码里实际出现的字生成（最省 flash）。
RESERVED = (
    "温度湿度设备状态时间配置保存确定取消通道总览诊断设置在线离线正常异常"
    "失败成功连接断开上云缓存版本重启触摸标定数据采样实时地址端口使能背光"
    "看门狗复位次数错误通道值电压电流功率频率"
)

DEFAULT_NODE = r"C:\Users\acer\.workbuddy-ai\binaries\node\versions\22.22.2-6\node.exe"
DEFAULT_LV_FONT_CONV = (
    r"C:\Users\acer\.workbuddy-ai\binaries\node\workspace"
    r"\node_modules\lv_font_conv\lv_font_conv.js"
)

# 生成文件的外壳（保证关掉 -Lvgl 时该文件是空文件，对基线零影响）
SHELL_HEAD = """#if defined(GW_USE_LVGL)
/* 本文件只在打开 -Lvgl 构建开关时参与编译（见 tools/build_ota_images.ps1）。
   关掉开关时它是空文件 —— 保证现有构建与浸泡基线一个字节都不变。 */

/* ⚠️ 本文件由 tools/gen_lvgl_font.py 自动生成，不要手工编辑。
   字符集来自界面源码里的字符串字面量（默认 app/gw_lvgl_ui.c）。
   改完界面文案后重跑：python tools/gen_lvgl_font.py */

"""
SHELL_TAIL = """
#endif /* GW_USE_LVGL */
"""

# lv_font_conv 默认写成两分支 include；构建时 -I 指向 LVGL 根目录，
# `#include "lvgl/lvgl.h"` 那个分支找不到头文件，统一成 "lvgl.h"。
CONV_INCLUDE = """#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif"""
OUR_INCLUDE = """/* -I 指向 LVGL 根目录（见 build_ota_images.ps1），直接用 "lvgl.h"。 */
#include "lvgl.h\""""


def strip_comments(src: str) -> str:
    """去掉 /* */ 与 // 注释，避免把注释里的汉字也收进字库。"""
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    return src


def cjk_of_file(path: str) -> set:
    """取出一个源文件里所有字符串字面量中的汉字。"""
    with open(path, encoding="utf-8", errors="replace") as f:
        src = strip_comments(f.read())
    out = set()
    for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', src):
        for ch in lit:
            if "\u4e00" <= ch <= "\u9fff":
                out.add(ch)
    return out


def old_symbols(out_path: str) -> str:
    """从已存在的字库文件头的 `Opts:` 行里读回旧符号表（可能为空）。"""
    if not os.path.isfile(out_path):
        return ""
    with open(out_path, encoding="utf-8", errors="replace") as f:
        head = f.read(4096)
    m = re.search(r"--symbols\s+(\S+)", head)
    return m.group(1) if m else ""


def build_cmd(node, conv_js, chars, out_path):
    """⚠️ `-o` 必须给**最终路径**：lv_font_conv 用输出文件名的 basename
    推导 C 标识符（`lv_font_cn_16.c` -> `lv_font_cn_16`）。
    给 `xxx.c.tmp` 会生成 `lv_font_cn_16.c` 这种非法标识符。"""
    cmd = [
        node, conv_js,
        "--font", FONT,
        "--range", "0x20-0x7E",
        "--symbols", chars,
        "--size", "16",
        "--bpp", "2",
        "--format", "lvgl",
        "--no-compress",
        "--force-fast-kern-format",
        "-o", out_path,
    ]
    return cmd


def main() -> int:
    ap = argparse.ArgumentParser(description="自动生成 LVGL 子集字库")
    ap.add_argument("--src", nargs="+", default=DEFAULT_SRC,
                    help=f"要扫描的源文件（默认 {DEFAULT_SRC}）")
    ap.add_argument("--out", default=OUT_REL, help=f"输出文件（默认 {OUT_REL}）")
    ap.add_argument("--extra", default="", help="再额外并入的汉字串")
    ap.add_argument("--no-reserved", action="store_true",
                    help="不并入预留字表，只按代码实际出现的字生成（最省 flash）")
    ap.add_argument("--check", action="store_true", help="只检查缺字，不生成")
    ap.add_argument("--dry-run", action="store_true", help="只打印命令")
    ap.add_argument("--node", default=os.environ.get("NODE") or DEFAULT_NODE)
    ap.add_argument("--lv-font-conv",
                    default=os.environ.get("LV_FONT_CONV") or DEFAULT_LV_FONT_CONV)
    args = ap.parse_args()

    out_path = os.path.join(REPO, args.out)

    # ---- 1. 抓取界面里真正用到的汉字 ----
    ui_need = set()
    for rel in args.src:
        p = os.path.join(REPO, rel)
        if not os.path.isfile(p):
            print(f"[!] 源文件不存在：{p}", file=sys.stderr)
            return 2
        got = cjk_of_file(p)
        print(f"    {rel:28s} -> {len(got):3d} 个汉字")
        ui_need |= got

    need = set(ui_need)
    if not args.no_reserved:
        need |= set(RESERVED)
    need |= set(args.extra)

    chars = "".join(sorted(need))
    n_res = 0 if args.no_reserved else len(set(RESERVED) - ui_need)
    print(f"\n[*] 字库共 {len(chars)} 个汉字"
          f"（界面实际用 {len(ui_need)} 个 + 预留 {n_res} 个）")
    print(f"    {chars}")

    # ---- 2. 与旧字库对比 ----
    old = old_symbols(out_path)
    old_cjk = {c for c in old if "\u4e00" <= c <= "\u9fff"}
    missing = sorted(c for c in need if c not in old_cjk)
    dropped = sorted(c for c in old_cjk if c not in need)
    print(f"[*] 旧字库含 {len(old_cjk)} 个汉字")
    if missing:
        print(f"[!] 旧字库**缺失**（会画空白）：{''.join(missing)}")
    if dropped:
        print(f"[*] 本次将**移除**（界面已不用）：{''.join(dropped)}")
    if not missing and not dropped:
        print("[*] 无差异 —— 字库已是最新")

    if args.check:
        return 1 if missing else 0

    # ---- 3. 调 lv_font_conv 生成 ----
    node = args.node if os.path.isfile(args.node) else "node"
    conv = args.lv_font_conv
    if not os.path.isfile(conv):
        print(f"[!] 找不到 lv_font_conv：{conv}\n"
              f"    装一个：npm i lv_font_conv   然后 --lv-font-conv <路径>", file=sys.stderr)
        return 2
    if not os.path.isfile(FONT):
        print(f"[!] 找不到字体：{FONT}", file=sys.stderr)
        return 2

    cmd = build_cmd(node, conv, chars, out_path)
    if args.dry_run:
        print("\n[cmd] " + " ".join(cmd))
        return 0

    # 生成前备份旧文件：lv_font_conv 直接写目标路径，失败时要把旧内容还原
    backup = None
    if os.path.isfile(out_path):
        with open(out_path, "rb") as f:
            backup = f.read()

    print(f"\n[*] 生成中 ...（{os.path.basename(FONT)}, 16px/2bpp）")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.isfile(out_path):
        if backup is not None:
            with open(out_path, "wb") as f:
                f.write(backup)
            print("[*] 已还原旧字库", file=sys.stderr)
        print("[!] lv_font_conv 失败：\n" + (r.stdout or "") + (r.stderr or ""),
              file=sys.stderr)
        return 2

    # ---- 4. 修 include + 包上 GW_USE_LVGL 外壳（关开关时是空文件）----
    with open(out_path, encoding="utf-8", errors="replace", newline="") as f:
        body = f.read()
    body = body.replace("\r\n", "\n")
    if CONV_INCLUDE not in body:
        print("[!] 未找到预期的 include 段（lv_font_conv 版本变了？）—— "
              "请检查生成文件里的 #include 是否指向 lvgl.h", file=sys.stderr)
    body = body.replace(CONV_INCLUDE, OUR_INCLUDE)

    text = SHELL_HEAD + body.rstrip("\n") + "\n" + SHELL_TAIL
    with open(out_path, "w", encoding="utf-8", newline="\r\n") as f:
        f.write(text)

    sz = os.path.getsize(out_path)
    print(f"[OK] 已写出 {args.out}  ({sz:,} B, {len(chars)} 汉字 + 95 ASCII)")
    if sz > 80_000:
        print(f"[!] 文件偏大，注意 app 槽余量", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
