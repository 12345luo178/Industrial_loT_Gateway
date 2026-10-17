#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
读并解码 OTA 元数据的两个页（SWD），顺带判断引导程序会怎么决策

============================================================================
 为什么需要它

  槽位化之后，"板上验证 OTA"变成反复做同一件事：
      复位 -> 读两页元数据 -> 看 seq / boot_tries / pending 怎么变
  手工拼 hex 又慢又容易错（CLI 打印的是**字的值**，内存字节序要反过来）。
  这个工具把"读 + 解码 + 判读"一次做完，并**顺带检查不变量**。

============================================================================
 用法

    python tools/verify_ota_slots.py                 # 读一次
    python tools/verify_ota_slots.py --reset         # 先复位再读
    python tools/verify_ota_slots.py --reset --wait 6
    python tools/verify_ota_slots.py --steps 4        # 复位+读，重复 4 轮
                                                     # （观察试用计数/回滚/两页交替）

 判读要点：
    - **两页都有效**（一新一旧）是双页 ping-pong 的预期状态
    - 每轮复位后 seq 会 +1、boot_tries 会 +1（只要 pending=1 且没试满）
    - 试满 GW_OTA_MAX_BOOT_TRIES(3) 次后应看到 active 切换、pending 清零（回滚）
    - pending=0 之后再复位应当是"零写入"（seq 不变）
"""

import argparse
import os
import sys
import time

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(errors="replace")
    except Exception:
        pass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from verify_can_dual import CLI, swd_read          # noqa: E402
import make_ota_meta as META                        # noqa: E402

META_ADDR = 0x08008000
PAGE_SIZE = 0x800
PAGES = 2
MAX_TRIES = 3           # GW_OTA_MAX_BOOT_TRIES
RAM_LO, RAM_HI = 0x20000000, 0x20010000


def read_page(page):
    """读一页的前 40 字节（记录长度）"""
    words = swd_read(META_ADDR + page * PAGE_SIZE, META.LEN if hasattr(META, "LEN") else 40)
    if not words:
        return None
    return b"".join(w.to_bytes(4, "little") for w in words)[:40]


def parse(raw):
    """返回 dict；无效记录返回 None"""
    import struct
    vals = {}
    for name, off, fmt in META.layout():
        if fmt == "2s":
            vals[name] = raw[off:off + 2]
        else:
            vals[name] = struct.unpack_from("<" + fmt, raw, off)[0]
    import zlib
    chk = bytearray(raw[:40])
    chk[36:40] = b"\x00\x00\x00\x00"
    vals["crc_ok"] = (zlib.crc32(bytes(chk)) & 0xFFFFFFFF) == vals["meta_crc"]
    vals["valid"] = (vals["magic"] == META.MAGIC
                     and vals["version"] == META.VERSION
                     and vals["state"] == META.STATE_VALID
                     and vals["crc_ok"])
    return vals


def pick(recs):
    """按固件同样的规则选"当前有效"的那份（问题 3 定下的策略）"""
    ok = [i for i, r in enumerate(recs) if r and r["valid"]]
    if not ok:
        return None
    best = ok[0]
    for i in ok[1:]:
        if recs[i]["seq"] > recs[best]["seq"]:
            best = i
        elif (recs[i]["seq"] == recs[best]["seq"]
              and recs[i]["pending"] == 0 and recs[best]["pending"] != 0):
            best = i
    return best


def show(tag=""):
    """读两页 + 解码 + 判读（每页只读一次 —— SWD 每次往返要 1~2 秒）"""
    raw = [read_page(p) for p in range(PAGES)]
    recs = [parse(r) if r else None for r in raw]

    print("  %-12s %-6s %-5s %-6s %-7s %-8s %s"
          % ("页", "有效", "seq", "active", "pending", "tries", "备注"))
    for p in range(PAGES):
        r = recs[p]
        if r is None:
            print("  页%-10d %-6s  (读取失败)" % (p, "-"))
            continue
        note = ""
        if not r["valid"]:
            note = "全FF(空)" if raw[p] == b"\xff" * 40 else "无效(magic/CRC/版本不符)"
        print("  页%-10d %-6s %-5s %-6s %-7s %-8s %s"
              % (p, "是" if r["valid"] else "否", r["seq"],
                 META.SLOT_NAME.get(r["active"], r["active"]), r["pending"],
                 r["boot_tries"], note))

    cur = pick(recs)
    if cur is None:
        print("  => 两页都无效 -> 引导程序**回退启动槽 A**（不写 Flash）")
    else:
        r = recs[cur]
        if r["pending"] == 0:
            print("  => 当前记录在页 %d：pending=0 -> 启动槽 %s，**不写 Flash**"
                  % (cur, META.SLOT_NAME.get(r["active"], r["active"])))
        elif r["boot_tries"] < MAX_TRIES:
            print("  => 当前记录在页 %d：试用中 tries=%d/%d -> 试启动槽 %s，"
                  "计数 +1 并**写另一页**"
                  % (cur, r["boot_tries"], MAX_TRIES,
                     META.SLOT_NAME.get(r["active"], r["active"])))
        else:
            print("  => 当前记录在页 %d：tries=%d 已试满 -> **回滚**到槽 %s、pending 清零"
                  % (cur, r["boot_tries"],
                     META.SLOT_NAME.get(META.SLOT[("B" if r["active"] == 0 else "A")], "?")))
    if tag:
        print("  [%s]" % tag)
    return recs


def reset():
    import subprocess
    subprocess.run([CLI, "-c", "port=SWD", "-rst"],
                   capture_output=True, text=True, errors="replace")


def main():
    ap = argparse.ArgumentParser(description="读/解码 OTA 元数据两页并判读引导决策")
    ap.add_argument("--reset", action="store_true", help="读之前先复位一次")
    ap.add_argument("--wait", type=float, default=5.0, help="复位后等待秒数")
    ap.add_argument("--steps", type=int, default=1, help="重复轮数（每轮 复位+读）")
    args = ap.parse_args()

    if not CLI:
        print("[FAIL] 找不到 STM32_Programmer_CLI")
        return 2

    for k in range(max(1, args.steps)):
        if args.reset:
            reset()
            time.sleep(args.wait)
        print("=" * 72)
        print("  第 %d/%d 轮%s" % (k + 1, max(1, args.steps),
                                 "（已复位，等 %.0fs）" % args.wait if args.reset else ""))
        show()
    return 0


if __name__ == "__main__":
    sys.exit(main())
