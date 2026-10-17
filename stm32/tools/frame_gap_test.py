#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
帧中间隙实验 —— 把 "IDLE(1T) 是否等于 Modbus 的 T3.5" 变成实测数据

============================================================================
 要回答的问题

   RM0008 p518：空闲符号 = "完全由 '1' 组成的一个完整的数据帧"
                -> IDLE 只需约 **1 个字符时间（1T）** 就置位

   Modbus RTU：帧**内部**允许的字节间隙是 1.5T；
               两帧之间要靠 **3.5T** 静默分界

   115200 8N1：1T ≈ 86.8 us   T1.5 ≈ 130 us   T3.5 ≈ 304 us

   我的从站用 IDLE 判帧尾。如果 IDLE 只在 1T 就置位，
   那么 1T ~ 3.5T 之间的字节间隙就会**把一帧切成两帧** ——
   而按 Modbus，那仍属同一帧。

============================================================================
 判据（关键：不用 s_rx_len）

   ⚠️ s_rx_len **不能**用作判据。原因有两条：
      1) 读它的那次请求本身就是新帧，会覆盖它
      2) 帧被 take_frame() 取走后 s_rx_len 会被清 0
     （第一版就是栽在这里，永远读到 0，得出"帧已取走"的假结论。）

   可靠的判据是**收帧计数（输入寄存器 13）的增量**：
        +1  -> 从站把整帧当成**一帧**收下（按 Modbus 语义，正确）
        +2  -> 从站把它**切成了两帧**（两段 CRC 都错）-> 证明了不一致窗口

   同时记录错误帧计数（15/16）。切帧时两段 CRC 都错，错误计数也会 +2。

============================================================================
 用法
     python frame_gap_test.py COM8               # 默认扫描 0~600us
     python frame_gap_test.py COM8 0 600 16      # 起止 us 与取样点数
============================================================================
"""
import sys

# ⚠️ 中文 Windows 的控制台是 GBK，而 Python 按控制台代码页编码 stdout：
#    print 里出现 ⚠️/✅/❌ 就会 UnicodeEncodeError，**脚本在干活之前就死了**。
#    （ci.ps1 里设了 PYTHONIOENCODING=utf-8，所以 CI 全绿、完全看不出来。）
#    这里把编不出来的字符降级成 '?'，而不是让整个脚本崩掉。
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(errors="replace")
    except (AttributeError, ValueError):
        pass
import time

try:
    import serial
except ImportError:
    print("需要 pyserial")
    sys.exit(2)

ADDR = 1
BAUD = 115200

CHAR_US = 10.0 / 115200 * 1e6      # 86.8 us
T15 = 1.5 * CHAR_US                # 130 us
T35 = 3.5 * CHAR_US                # 304 us

PDU = bytes([ADDR, 0x04, 0x00, 0x00, 0x00, 0x04])   # 读 4 个输入寄存器


def crc16(d: bytes) -> int:
    c = 0xFFFF
    for b in d:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c & 0xFFFF


def crc_ok(fr: bytes) -> bool:
    return len(fr) >= 4 and crc16(fr[:-2]) == (fr[-2] | (fr[-1] << 8))


def build() -> bytes:
    c = crc16(PDU)
    return PDU + bytes([c & 0xFF, c >> 8])


class Drv:
    def __init__(self, port):
        self.s = serial.Serial(port, BAUD, bytesize=8,
                               parity=serial.PARITY_NONE, stopbits=1,
                               timeout=1.2, write_timeout=1.2)

    def close(self):
        self.s.close()

    def _rx(self, want):
        deadline = time.time() + self.s.timeout
        buf = b""
        while time.time() < deadline:
            n = self.s.in_waiting
            if n:
                buf += self.s.read(n)
                if len(buf) >= want:
                    break
                deadline = time.time() + 0.03
            else:
                time.sleep(0.001)
        return buf

    def raw(self, data: bytes, want: int, end_gap_us=0.0):
        """发一段原始字节，收响应。end_gap_us 用于'整帧但末尾留空隙'的场景"""
        self.s.reset_input_buffer()
        self.s.write(data)
        self.s.flush()
        if end_gap_us > 0:
            time.sleep(end_gap_us / 1e6)
        return self._rx(want)

    def split_send(self, frame: bytes, split_at: int, gap_us: float, want: int):
        """拆成两段发，中间留 gap_us"""
        self.s.reset_input_buffer()
        self.s.write(frame[:split_at])
        self.s.flush()
        time.sleep(gap_us / 1e6)
        self.s.write(frame[split_at:])
        self.s.flush()
        return self._rx(want)

    def stats(self):
        """读回统计量。

        ⚠️ 不能一次读 13..32（共 20 个）—— 地址 **32 不存在**，
           从站会正确地回异常 0x02（非法数据地址），
           而我的脚本会把它误认成「读不到统计」。
           这又是我自己的错（可用地址是 0~31）。
           改成两次读：计数器 13~16，诊断块 28~31。
        """
        start, qty = 13, 4
        p = bytes([ADDR, 0x04]) + start.to_bytes(2, "big") + qty.to_bytes(2, "big")
        c = crc16(p)
        r = self.raw(p + bytes([c & 0xFF, c >> 8]), 5 + 2 * qty)
        if not r or not crc_ok(r) or (r[1] & 0x80) or len(r) < 5 + 2 * qty:
            return None
        v = [int.from_bytes(r[3 + i*2:5 + i*2], "big") for i in range(qty)]

        start2 = 28
        p2 = bytes([ADDR, 0x04]) + start2.to_bytes(2, "big") + (4).to_bytes(2, "big")
        c2 = crc16(p2)
        r2 = self.raw(p2 + bytes([c2 & 0xFF, c2 >> 8]), 13)
        if not r2 or not crc_ok(r2) or (r2[1] & 0x80) or len(r2) < 13:
            return None
        w = [int.from_bytes(r2[3 + i*2:5 + i*2], "big") for i in range(4)]

        return {
            "mbrx": v[0],                    # 13 收帧计数 低
            "mbrx_hi": v[1],                 # 14 高
            "mberr": (v[3] << 16) | v[2],    # 16/15 错误帧计数（高字在前）
            "state": w[0],                   # 28
            "sr": w[1],                      # 29
            "rxlen": w[2],                   # 30
            "cndtr": w[3],                   # 31
        }


def classify(resp: bytes, sr: int = 0) -> str:
    """⚠️ 判据只能看**响应是否有效**，不能看收帧计数增量。

    原因：切帧时前后两半都是 CRC 错，它们会占用两个"帧槽"，
    但"增量"这个指标混入了读统计的 2 个请求，分辨不出 3 和 4。
    而"有没有合法应答"是直接且无歧义的。

    另外把 SR 的 IDLE 位（bit4）也打出来：它是 IDLE 确实触发过的硬件证据。
    """
    idle = "IDLE!" if (sr >> 4) & 1 else ""
    if not resp:
        return "无应答" + ("/" + idle if idle else "")
    if crc_ok(resp) and len(resp) == 13 and not (resp[1] & 0x80):
        return "OK 完整应答"
    if crc_ok(resp) and len(resp) >= 3 and (resp[1] & 0x80):
        return "异常 0x%02X" % resp[2]
    return "无效(%s)" % resp.hex().upper()[:16]


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
    lo = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    hi = int(sys.argv[3]) if len(sys.argv) > 3 else 600
    npts = int(sys.argv[4]) if len(sys.argv) > 4 else 16

    frame = build()
    split = 4
    want = 13

    print("=" * 78)
    print("  帧中间隙实验：IDLE(1T) vs Modbus T3.5")
    print("=" * 78)
    print("  端口 %s @ %d 8N1" % (port, BAUD))
    print("  请求帧 %s（%d 字节），切成 %d + %d"
          % (frame.hex().upper(), len(frame), split, len(frame) - split))
    print("  1T = %.1f us   1.5T = %.0f us   3.5T = %.0f us"
          % (CHAR_US, T15, T35))
    print("  判据：收帧计数增量（已扣除读统计的 2 个请求）")
    print()

    d = Drv(port)

    # ---------- 第 0 步：能力自证 ----------
    print("  ---- 第 0 步：工具能力自证 ----")
    s0 = d.stats()
    if s0 is None:
        print("  ❌ 读不到统计块。固件不对或已卡死。")
        d.close()
        return 1
    print("  基线：收帧计数=%d  错误计数=%d  CNDTR=%d" % (s0["mbrx"], s0["mberr"], s0["cndtr"]))

    # 0-a：整帧一次发
    b = d.stats()["mbrx"]
    r = d.raw(frame, want)
    a = d.stats()["mbrx"]
    print("  [对照] 整帧一次发     : 响应=%-14s 收帧计数 %d -> %d (+%d)"
          % (classify(r), b, a, a - b))
    if a - b != 3:
        print("  ⚠️ 整帧发送时增量应为 3（1 被测帧 + 2 读统计），实际 %d" % (a - b))

    # 0-b：整帧但在**末尾**留一个长间隙（这一帧本身必须完整收到）
    print()
    print("  ---- 第 0 步附：验证'拆两次 write'确实能在线上造成间隙 ----")
    print("  做法：把整帧拆成 4+4 两次 write，中间 sleep。")
    print("        若线上无间隙 -> 增量仍为 1；若有间隙 -> 变成 2。")
    print()

    # ---------- 主扫描 ----------
    if npts == 1:
        gaps = [lo]
    else:
        step = (hi - lo) / float(npts - 1)
        gaps = [int(round(lo + i * step)) for i in range(npts)]

    print("  %-9s %-7s %-20s %-7s %-7s %s"
          % ("间隙us", "折合T", "响应", "收帧+", "错误+", "判读"))
    print("  " + "-" * 72)

    floor_us = None      # 最小的"切帧"间隙
    ceil_us = None       # 最大的"仍算一帧"间隙

    for g in gaps:
        b = d.stats()
        if b is None:
            print("  %-9d %-7.2f ❌ 读不到统计（可能已卡死）" % (g, g / CHAR_US))
            break
        r = d.split_send(frame, split, g, want)
        a = d.stats()
        if a is None:
            print("  %-9d %-7.2f ❌ 读不到统计" % (g, g / CHAR_US))
            break

        dm = a["mbrx"] - b["mbrx"]
        de = a["mberr"] - b["mberr"]
        # ⚠️ 增量基准要算清楚：stats() 每次发 **2 个**请求（13 起 4 个 + 28 起 4 个），
        #    所以一轮 "b=stats(); 发被测帧; a=stats()" 的收帧增量是：
        #        整帧被当成一帧 -> 1(被测) + 2(读统计) = 3
        #        被切成两帧     -> 2(被测) + 2(读统计) = 4
        ok_resp = bool(r) and crc_ok(r) and len(r) == 13 and not (r[1] & 0x80)
        if ok_resp:
            verdict = "当成一帧 ✅"
            ceil_us = g
        else:
            verdict = "**被切断** ❌"
            if floor_us is None:
                floor_us = g

        print("  %-9d %-7.2f %-20s %-7d %-7d %s"
              % (g, g / CHAR_US, classify(r, a["sr"]), dm, de, verdict))

    d.close()

    # ---------- 结论 ----------
    print()
    print("=" * 78)
    print("  结论")
    print("=" * 78)
    if floor_us is not None and ceil_us is not None:
        print("  间隙 <= %d us (%.2fT) 时仍算一帧" % (ceil_us, ceil_us / CHAR_US))
        print("  间隙 >= %d us (%.2fT) 时被切成两帧" % (floor_us, floor_us / CHAR_US))
        print()
        print("  ⚠️ 而 Modbus 规定：帧内允许到 **1.5T = %.0f us**，" % T15)
        print("     帧边界要到 **3.5T = %.0f us**。" % T35)
        print()
        if floor_us < T35:
            print("  ✅ **实测证实不一致窗口存在**：")
            print("     从站在 %.2fT 就把帧切断了，比规范的 3.5T 早。" % (floor_us / CHAR_US))
            print("     落在 %.0f~%.0f us 之间的字节间隙会被误切成两帧 ——" % (floor_us, T35))
            print("     两段 CRC 都错、都不应答，主站表现为超时重试。")
        else:
            print("  ✅ 从站的行为与规范一致（在 3.5T 之后才切帧）。")
    elif floor_us is None and ceil_us is not None:
        print("  ⚠️ **所有间隙都未触发切帧**（最大试到 %d us）——" % max(gaps))
        print("     可能：USB 驱动把两次 write 合并了，线上根本没有间隙。")
        print("     需要用逻辑分析仪看线上波形才能确认。")
    elif floor_us is not None and ceil_us is None:
        print("  ⚠️ **所有间隙都触发切帧**（最小试到 %d us，%.2fT）——" % (floor_us, floor_us / CHAR_US))
        print("     说明 IDLE 触发得非常快。")
    else:
        print("  数据不足。")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
