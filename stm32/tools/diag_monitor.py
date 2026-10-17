#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
诊断固件专用：边压测边监控 RS485 接收路径状态

============================================================================
 它回答的问题

   从站在几百次高速请求之后"永久停止响应"，但我看不到内部状态。
   诊断固件把状态映射到了输入寄存器 28~31（见 inc/modbus_regs.h）：

      28  IREG_DIAG_STATE    位段
      29  IREG_DIAG_SR       USART2->SR
      30  IREG_DIAG_RX_LEN   s_rx_len
      31  IREG_DIAG_CNDTR    DMA 剩余传输数

   本脚本在**每次请求之前**读一遍这 4 个寄存器，于是能看出：
       - 状态机卡在哪个具体状态（1=IDLE 2=RX 3=RX_READY 4=TX 5=ERROR）
       - DMA 是否还被武装（bit7 RX_ARMED）
       - USART 的 RE / DMAR / IDLEIE 是否还在（bit8/9/10）
       - huart2.RxState 是否 READY（bit11）
       - 有没有出过接收错误（bit4 HAS_RX_ERR）
       - SR 里的 ORE/NE/FE/PE（0x08 / 0x04 / 0x02 / 0x01）
       - s_rx_len 和 CNDTR 的具体值

============================================================================
 关键设计：**把每次读到的状态都留下来**

   一旦从站开始"间歇性不应答"，脚本会把**故障前后的状态序列**打出来。
   这比"卡死后读一次"有用得多 —— 卡死后已经什么都读不到了。
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

DIAG_STATE = 28
DIAG_SR = 29
DIAG_RX_LEN = 30
DIAG_CNDTR = 31

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
    flags = [
        (3, "RX_OVF"), (4, "RXERR"), (5, "EVERR"), (6, "TXOK"),
        (7, "ARMED"), (8, "IDLEIE"), (9, "DMAR"), (10, "RE"), (11, "HALRDY"),
    ]
    for bit, name in flags:
        parts.append(("+" if (v >> bit) & 1 else "-") + name)
    return " ".join(parts)


def decode_sr(v: int) -> str:
    parts = []
    for bit, name in ((0, "PE"), (1, "FE"), (2, "NE"), (3, "ORE"),
                      (4, "IDLE"), (5, "RXNE"), (6, "TC"), (7, "TXE")):
        if (v >> bit) & 1:
            parts.append(name)
    return "|".join(parts) if parts else "-"


class Drv:
    def __init__(self, port):
        self.s = serial.Serial(port, BAUD, bytesize=8,
                               parity=serial.PARITY_NONE, stopbits=1,
                               timeout=1.5)
        self.s.reset_input_buffer()

    def close(self):
        self.s.close()

    def xact(self, pdu: bytes, want_len=5, settle_ms=4.0):
        c = crc16(pdu)
        frame = pdu + bytes([c & 0xFF, c >> 8])
        self.s.reset_input_buffer()
        self.s.write(frame)
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
                time.sleep(0.001)
        return buf

    def rd(self, func, start, qty):
        r = self.xact(bytes([ADDR, func])
                      + start.to_bytes(2, "big")
                      + qty.to_bytes(2, "big"), want_len=5 + 2 * qty)
        if not r or not crc_ok(r) or (r[1] & 0x80):
            return None
        n = r[2] // 2
        return [int.from_bytes(r[3 + i*2:5 + i*2], "big") for i in range(n)]

    def diag(self):
        """读诊断块 + 收帧计数。返回 dict 或 None"""
        d = self.rd(4, DIAG_STATE, 4)
        if d is None:
            return None
        rx = self.rd(4, 13, 1)
        return {
            "state": d[0], "sr": d[1], "rxlen": d[2], "cndtr": d[3],
            "mbrx": rx[0] if rx else -1,
        }


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
    N = int(sys.argv[2]) if len(sys.argv) > 2 else 400

    print("=" * 78)
    print("  诊断监控：%d 次高速请求，每次请求前读一次内部状态" % N)
    print("  端口 %s @ %d" % (port, BAUD))
    print("=" * 78)

    m = Drv(port)

    d0 = m.diag()
    if d0 is None:
        print("  ❌ 读不到诊断块。可能原因：")
        print("     - 板上还是**旧固件**（没有诊断寄存器）—— 需要先烧新固件")
        print("     - 从站已卡死 —— 需要先断电重启")
        m.close()
        return 1

    print()
    print("  基线状态：")
    print("    状态   %s" % decode_state(d0["state"]))
    print("    SR     %s (0x%04X)" % (decode_sr(d0["sr"]), d0["sr"]))
    print("    rx_len %d    CNDTR %d    收帧计数 %d"
          % (d0["rxlen"], d0["cndtr"], d0["mbrx"]))
    print()

    history = []
    prev = d0
    ok = 0
    fail_at = None
    t0 = time.time()

    for i in range(1, N + 1):
        # ---- 请求前先看状态 ----
        pre = m.diag()
        if pre is None:
            fail_at = i
            print("  ❌ 第 %d 次：**连诊断块都读不到了**（接收路径已死）" % i)
            break

        # 状态有变化就记下来（这正是要找的东西）
        #
        # ⚠️ 只比较**诊断字段**（state/sr/rxlen/cndtr），
        #    **不能**把 mbrx 纳进来 —— 收帧计数每轮都在涨，
        #    带上它就会每次迭代都打印一行，把真正的状态变化淹没掉。
        #    （第一版就这么错了，100 次刷了 100 行"状态变化"。）
        key = (pre["state"], pre["sr"], pre["rxlen"], pre["cndtr"])
        pkey = (prev["state"], prev["sr"], prev["rxlen"], prev["cndtr"])
        if key != pkey:
            print("  [%4d] 状态变化: %s | CNDTR=%d rxlen=%d mbrx=%d"
                  % (i, decode_state(pre["state"]), pre["cndtr"],
                     pre["rxlen"], pre["mbrx"]))
            history.append((i, pre))
        prev = pre

        # 明显的坏信号
        st = pre["state"]
        if (st & 0x07) == 5:
            print("  ⚠️ [%4d] 状态 = ERROR！" % i)
        if not (st >> 7) & 1:
            print("  ⚠️ [%4d] DMA 不再武装（RX_ARMED 丢失）" % i)
        if (pre["sr"] & 0x0F) and not (pre["sr"] & 0x01):
            print("  ⚠️ [%4d] SR 有接收错误位: %s"
                  % (i, decode_sr(pre["sr"])))

        # ---- 真正的业务请求 ----
        r = m.rd(4, 0, 4)
        if r is None:
            fail_at = i
            print()
            print("  " + "!" * 74)
            print("  ❌ 第 %d 次请求**无应答**" % i)
            print("     请求前读到的状态: %s" % decode_state(pre["state"]))
            print("     SR=%s (0x%04X)  CNDTR=%d  rx_len=%d  收帧计数=%d"
                  % (decode_sr(pre["sr"]), pre["sr"], pre["cndtr"],
                     pre["rxlen"], pre["mbrx"]))
            print("  " + "!" * 74)

            # 再试几次，看是否恢复
            print("     连续重试 5 次：")
            for k in range(5):
                time.sleep(0.05)
                d = m.diag()
                rr = m.rd(4, 0, 4)
                print("       重试%d: 请求=%s  状态=%s"
                      % (k + 1, "OK" if rr else "无应答",
                         decode_state(d["state"]) if d else "读不到诊断块"))
            break
        ok += 1

        if i % 50 == 0:
            print("  [%4d/%d] OK   状态=%s  CNDTR=%d  mbrx=%d"
                  % (i, N, decode_state(pre["state"]), pre["cndtr"],
                     pre["mbrx"]))

    dt = time.time() - t0
    m.close()

    print()
    print("=" * 78)
    print("  结果")
    print("=" * 78)
    print("  成功 %d / %d   耗时 %.1f s" % (ok, N, dt))
    if fail_at:
        print("  ❌ 在第 %d 次失败" % fail_at)
        print()
        print("  状态变化历史（共 %d 次变化）：" % len(history))
        for idx, h in history[-15:]:
            print("    [%4d] %s" % (idx, decode_state(h["state"])))
    else:
        print("  ✅ %d 次全部成功，未复现" % N)
    return 0


if __name__ == "__main__":
    sys.exit(main())
