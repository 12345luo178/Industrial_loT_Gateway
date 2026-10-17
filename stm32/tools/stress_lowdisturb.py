#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
低扰动压测 —— 复现"高速连续请求后从站永久停止响应"

============================================================================
 为什么需要低扰动版本

   diag_monitor.py 每次业务请求前都读一遍诊断块（2 个额外请求），
   结果 300 次全过、一次状态变化都没有 —— **没复现**。

   怀疑：那 2 个额外请求给了从站"喘息时间"，
         而故障只在**持续背靠背**时才出现。

   本脚本反过来：**只打业务请求，不穿插诊断**，
   每 N 次才检查一次健康度。这样最接近原来的触发条件。

============================================================================
 用法
     python stress_lowdisturb.py COM8 2000 200
                                      ^次数 ^每多少次检查一次
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
DIAG_STATE, DIAG_SR, DIAG_RXLEN, DIAG_CNDTR = 28, 29, 30, 31
IREG_MB_RX_LO = 13

ST_NAMES = {0: "UNINIT", 1: "IDLE", 2: "RX", 3: "RX_READY", 4: "TX", 5: "ERROR"}


def crc16(d: bytes) -> int:
    c = 0xFFFF
    for b in d:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c & 0xFFFF


def crc_ok(fr: bytes) -> bool:
    return len(fr) >= 4 and crc16(fr[:-2]) == (fr[-2] | (fr[-1] << 8))


def decode_state(v: int) -> str:
    st = v & 0x07
    parts = [ST_NAMES.get(st, "?%d" % st)]
    for bit, name in ((3, "RX_OVF"), (4, "RXERR"), (5, "EVERR"), (6, "TXOK"),
                      (7, "ARMED"), (8, "IDLEIE"), (9, "DMAR"), (10, "RE"),
                      (11, "HALRDY")):
        parts.append(("+" if (v >> bit) & 1 else "-") + name)
    return " ".join(parts)


def decode_sr(v: int) -> str:
    p = [n for b, n in ((0, "PE"), (1, "FE"), (2, "NE"), (3, "ORE"),
                        (4, "IDLE"), (5, "RXNE"), (6, "TC"), (7, "TXE"))
         if (v >> b) & 1]
    return "|".join(p) if p else "-"


class Drv:
    def __init__(self, port):
        self.s = serial.Serial(port, BAUD, bytesize=8,
                               parity=serial.PARITY_NONE, stopbits=1,
                               timeout=1.5)
        self.s.reset_input_buffer()

    def close(self):
        self.s.close()

    def xact(self, pdu, want_len, settle_ms=4.0):
        c = crc16(pdu)
        fr = pdu + bytes([c & 0xFF, c >> 8])
        self.s.reset_input_buffer()
        self.s.write(fr)
        self.s.flush()
        time.sleep(settle_ms / 1000.0)
        deadline = time.time() + self.s.timeout
        buf = b""
        while time.time() < deadline:
            n = self.s.in_waiting
            if n:
                buf += self.s.read(n)
                if want_len is not None and len(buf) >= want_len:
                    break
            else:
                time.sleep(0.0005)
        return buf

    def rd(self, func, start, qty):
        r = self.xact(bytes([ADDR, func]) + start.to_bytes(2, "big")
                      + qty.to_bytes(2, "big"), 5 + 2 * qty)
        if not r or not crc_ok(r) or (r[1] & 0x80):
            return None
        return [int.from_bytes(r[3+i*2:5+i*2], "big")
                for i in range(r[2] // 2)]

    def health(self):
        d = self.rd(4, DIAG_STATE, 4)
        if d is None:
            return None
        rx = self.rd(4, IREG_MB_RX_LO, 1)
        return {"state": d[0], "sr": d[1], "rxlen": d[2], "cndtr": d[3],
                "mbrx": rx[0] if rx else -1}


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
    N = int(sys.argv[2]) if len(sys.argv) > 2 else 2000
    CHECK = int(sys.argv[3]) if len(sys.argv) > 3 else 200
    # ⚠️ GAP：每次请求之间的空闲间隔（毫秒）。
    #
    #   加这个参数是因为一个反常对比：
    #       原故障场景 = **慢速**（~2 次/秒，中间夹着 500ms 超时空档）-> 卡死
    #       20 分钟压测 = **高速**（215 次/秒，持续不断）        -> 4 万帧无错
    #   也就是说问题不在"高速"，反而在"慢速 + 空闲间隔"。
    #   所以必须能复现那种节奏。
    GAP = int(sys.argv[4]) if len(sys.argv) > 4 else 0

    print("=" * 78)
    print("  低扰动压测：连续 %d 次业务请求，每 %d 次检查一次健康度，间隔 %d ms" % (N, CHECK, GAP))
    print("  端口 %s @ %d" % (port, BAUD))
    print("=" * 78)

    m = Drv(port)
    h0 = m.health()
    if h0 is None:
        print("  ❌ 读不到诊断块 —— 固件不对或已卡死，先断电重启")
        m.close()
        return 1
    print("\n  基线: %s  SR=%s  CNDTR=%d  mbrx=%d"
          % (decode_state(h0["state"]), decode_sr(h0["sr"]),
             h0["cndtr"], h0["mbrx"]))
    print()

    ok = 0
    fail_at = None
    last = h0
    t0 = time.time()

    for i in range(1, N + 1):
        r = m.rd(4, 0, 4)
        if r is None:
            fail_at = i
            print("\n  " + "!" * 74)
            print("  ❌ 第 %d 次请求无应答" % i)
            print("  " + "!" * 74)
            print("     连续重试 6 次，每次读一遍状态：")
            for k in range(6):
                time.sleep(0.08)
                h = m.health()
                rr = m.rd(4, 0, 4)
                if h is None:
                    print("       重试%d: 请求=%s  **诊断块也读不到**"
                          % (k + 1, "OK" if rr else "无应答"))
                else:
                    print("       重试%d: 请求=%-6s 状态=%s"
                          % (k + 1, "OK" if rr else "无应答",
                             decode_state(h["state"])))
            break
        ok += 1

        if GAP:
            time.sleep(GAP / 1000.0)

        if i % CHECK == 0:
            h = m.health()
            if h is None:
                print("  ⚠️ [%4d] 业务请求 OK，但**诊断块读不到**了" % i)
            else:
                chg = ""
                if (h["state"], h["cndtr"]) != (last["state"], last["cndtr"]):
                    chg = "   <== 状态变了！"
                print("  [%4d/%d] OK  %s  CNDTR=%d mbrx=%d%s"
                      % (i, N, decode_state(h["state"]), h["cndtr"],
                         h["mbrx"], chg))
                last = h

    dt = time.time() - t0
    m.close()
    print()
    print("=" * 78)
    print("  成功 %d / %d   耗时 %.2f s" % (ok, N, dt))
    if fail_at:
        print("  ❌ 在第 %d 次失败（复现了！）" % fail_at)
    else:
        print("  ✅ 全部成功，未复现")
    return 0


if __name__ == "__main__":
    sys.exit(main())
