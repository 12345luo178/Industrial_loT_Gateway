#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""逐任务心跳诊断（**只读 SWD**，可在浸泡运行中安全使用）。

⚠️ 2026-10-12 重写要点：结构偏移**不再硬编码**。
   原来把 gw_sched_t 的偏移写死在源码里（docstring 里甚至写着"改了结构要同步"），
   而 GW_TASK_COUNT 从 6 改成 7 之后数组偏移全部平移 -> 本工具读出一堆垃圾，
   还误报"任务全 IDLE"，把排查带偏过。现在改为**运行时**用 arm-none-eabi-gdb
   从 ELF 的 DWARF 现取（`ptype /o`），取不到就**明确报错退出**，
   绝不静默使用旧数字。想只看偏移、不连板子：`--offsets`。

判据（来自 inc/gw_sched.h）：某个已启动任务若满足
   `now - last_beat_ms[i] > tasks[i].period_ms + GW_SCHED_WD_SLACK_MS`
   则该次喂狗被拒；SLACK = 1000ms。
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys

try:
    sys.stdout.reconfigure(errors="replace")
except Exception:
    pass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_can_dual import swd_read, nm_symbol, DEFAULT_ELF      # noqa: E402

SLACK_MS = 1000
STATE_NAME = {0: "IDLE", 1: "READY", 2: "RUNNING", 3: "SUSPEND", 4: "FAULT"}

GDB_CANDIDATES = [
    r"D:\stm32cubeIDE\STM32CubeIDE_1.19.0\STM32CubeIDE\plugins"
    r"\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.win32_1.0.0.202411081344"
    r"\tools\bin\arm-none-eabi-gdb.exe",
]


def find_gdb():
    g = shutil.which("arm-none-eabi-gdb")
    if g:
        return g
    for c in GDB_CANDIDATES:
        if os.path.exists(c):
            return c
    return None


def _parse_ptype(text):
    """解析 gdb `ptype /o` 的输出 -> ({字段: (偏移, 大小)}, 结构总大小)"""
    fields = {}
    for m in re.finditer(r"/\*\s*(\d+)\s*\|\s*(\d+)\s*\*/\s*(.+?);", text):
        off, size, decl = int(m.group(1)), int(m.group(2)), m.group(3).strip()
        mm = re.search(r"([A-Za-z_]\w*)\s*(\[\s*\d*\s*\])?\s*$", decl)
        if mm:
            fields[mm.group(1)] = (off, size)
    total = None
    m = re.search(r"total size \(bytes\):\s*(\d+)", text)
    if m:
        total = int(m.group(1))
    return fields, total


def load_offsets(elf):
    gdb = find_gdb()
    if not gdb:
        print("  [FAIL] 找不到 arm-none-eabi-gdb —— 本工具**拒绝**使用硬编码偏移"
              "（那正是它以前给出假结论的原因）。")
        sys.exit(3)
    out = {}
    for t in ("gw_sched_t", "gw_task_def_t", "gw_task_stat_t"):
        r = subprocess.run([gdb, "-batch", "-ex", "ptype /o %s" % t, elf],
                           capture_output=True, text=True, errors="replace")
        fields, total = _parse_ptype(r.stdout)
        if not fields:
            print("  [FAIL] gdb 没能给出 %s 的布局（ELF 缺调试信息？）" % t)
            sys.exit(3)
        out[t] = (fields, total)
    return out


def main():
    ap = argparse.ArgumentParser(description="逐任务心跳诊断（只读；偏移由 gdb 现取）")
    ap.add_argument("--elf", default=DEFAULT_ELF)
    ap.add_argument("--gap", type=float, default=0.0, help="两次采样间隔秒")
    ap.add_argument("--offsets", action="store_true",
                    help="只打印结构偏移后退出（不连板子，可离线核对）")
    a = ap.parse_args()

    lay = load_offsets(a.elf)
    sch_f, sch_sz = lay["gw_sched_t"]
    def_f, def_sz = lay["gw_task_def_t"]
    sta_f, sta_sz = lay["gw_task_stat_t"]

    if a.offsets:
        print("  来自 ELF 的结构布局（gdb ptype /o）：")
        print("    gw_sched_t      总大小 %s" % sch_sz)
        for k in ("tasks", "stat", "state", "due_ms", "last_beat_ms",
                  "beat_seen", "wd_ref_ms", "loops", "inited"):
            if k in sch_f:
                print("      %-16s 偏移 %4d  大小 %d" % (k, sch_f[k][0], sch_f[k][1]))
            else:
                print("      %-16s **缺字段（结构改了？）**" % k)
        print("    gw_task_def_t   总大小 %s ：period_ms@%s"
              % (def_sz, def_f.get("period_ms", ("?",))[0]))
        print("    gw_task_stat_t  总大小 %s ：runs@%s max_us@%s over_budget@%s deadline_miss@%s"
              % (sta_sz, sta_f.get("runs", ("?",))[0], sta_f.get("max_us", ("?",))[0],
                 sta_f.get("over_budget", ("?",))[0],
                 sta_f.get("deadline_miss", ("?",))[0]))
        return 0

    sym = nm_symbol(a.elf, "s_sched")
    tick_sym = nm_symbol(a.elf, "uwTick")
    if not sym or not tick_sym:
        print("  [FAIL] ELF 里找不到 s_sched 或 uwTick")
        return 2
    w = swd_read(sym, sch_sz)
    if not w:
        print("  [FAIL] 读不到 s_sched（目标不可达？）")
        return 2
    b = b"".join(x.to_bytes(4, "little") for x in w)
    tick = swd_read(tick_sym, 4)
    now = tick[0] if tick else None

    def u32(off):
        return struct.unpack_from("<I", b, off)[0]

    def cstr(addr, n=16):
        ww = swd_read(addr, n)
        if not ww:
            return "?"
        raw = b"".join(x.to_bytes(4, "little") for x in ww)
        z = raw.find(b"\x00")
        return raw[:z if z >= 0 else n].decode("latin1", "replace")

    print("=" * 78)
    print(" 逐任务心跳诊断（只读）  s_sched@0x%08X 大小 %d  偏移=gdb 现取 ✓  now=%s"
          % (sym, sch_sz, now))
    print(" 判据：now - last_beat > period_ms + %d" % SLACK_MS)
    print("-" * 78)
    print(" %-7s %-8s %6s %8s %7s %7s %9s %8s  %s"
          % ("任务", "状态", "周期ms", "runs", "超期", "超预算", "max_us", "age_ms", "判定"))

    offenders = []
    n_task = sch_f["tasks"][1] // def_sz if def_sz else 0
    for i in range(n_task):
        d = sch_f["tasks"][0] + i * def_sz
        st = sch_f["stat"][0] + i * sta_sz
        name = cstr(u32(d))
        if not name or name == "?":
            break
        period = struct.unpack_from("<H", b, d + def_f["period_ms"][0])[0]
        runs = u32(st + sta_f["runs"][0])
        maxus = u32(st + sta_f["max_us"][0])
        overb = u32(st + sta_f["over_budget"][0])
        miss = u32(st + sta_f["deadline_miss"][0])
        state = b[sch_f["state"][0] + i]
        last = u32(sch_f["last_beat_ms"][0] + i * 4)
        age = (now - last) & 0xFFFFFFFF if now is not None else None
        bad = (state != 3) and (age is not None) and (age > period + SLACK_MS)
        if bad:
            offenders.append((name, age, period + SLACK_MS, miss))
        print(" %-7s %-8s %6d %8d %7d %7d %9d %8s  %s"
              % (name, STATE_NAME.get(state, "?"), period, runs, miss, overb, maxus,
                 age if age is not None else "?", "**超期**" if bad else "ok"))

    print("-" * 78)
    if offenders:
        for name, age, limit, miss in offenders:
            print("  [FAIL] %s 超期：age=%d > limit=%d（历史 deadline_miss=%d）"
                  % (name, age, limit, miss))
        print("  结论：**未通过** —— 上面点名的任务就是 s_feed_reject 的来源")
        return 1
    print("  [ OK ] 当前没有任务处于超期状态")
    return 0


if __name__ == "__main__":
    sys.exit(main())
