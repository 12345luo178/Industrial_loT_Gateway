#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
谨慎复现"从站卡死"——找出精确的触发条件

============================================================================
 已知事实（上板实测）

   1) 500 次连续快速请求（每次仅等 4ms）之后，从站完全无应答
   2) **按复位键无效** —— 连 [BOOT] 横幅都不打印
   3) **拔插 USB（断电）后恢复**，且 [13] 收帧计数归 0
      -> 证明是断电，不是软复位

   结论：外设（USART2/DMA1）被卡在了**软复位清不掉的硬件状态**。

============================================================================
 本脚本的做法（关键：不要一次打 500 次又失去现场）

   - 分组做，**每组之间停下来问一次**："还活着吗？"
   - 一发现无应答就**立刻停在那个请求号**并打印完整现场
   - 同时读 [12] 运行秒数 与 [13] 收帧计数
        [13] 能区分两种失败：
            收帧计数在涨但不应答 -> 从站收到了但发不出去（发送/方向问题）
            收帧计数不涨       -> 从站根本没收到（接收路径死了）
   - 默认只跑 50 次，确认安全再往上加

 用法：
     python reproduce_hang.py COM8 50           # 快速连发 50 次
     python reproduce_hang.py COM8 200 10       # 200 次，每次间隔 10ms
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
IREG_UPTIME_LO = 12
IREG_MB_RX_LO = 13


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc & 0xFFFF


def crc_ok(fr: bytes) -> bool:
    return (len(fr) >= 4
            and crc16(fr[:-2]) == (fr[-2] | (fr[-1] << 8)))


class Drv:
    def __init__(self, port):
        self.s = serial.Serial(port, BAUD, bytesize=8,
                               parity=serial.PARITY_NONE, stopbits=1,
                               timeout=0.35)
        self.s.reset_input_buffer()

    def close(self):
        self.s.close()

    def xact(self, pdu: bytes, settle_ms: float = 4.0):
        """发一帧，收一帧。返回 (发出去的, 收到的, 耗时ms)"""
        c = crc16(pdu)
        frame = pdu + bytes([c & 0xFF, c >> 8])
        self.s.reset_input_buffer()
        t0 = time.time()
        self.s.write(frame)
        self.s.flush()
        time.sleep(settle_ms / 1000.0)

        end = time.time() + self.s.timeout
        buf = b""
        while time.time() < end:
            n = self.s.in_waiting
            if n:
                buf += self.s.read(n)
                end = time.time() + 0.05
            else:
                time.sleep(0.002)
        return frame, buf, (time.time() - t0) * 1000.0


def read(m, func, start, qty):
    fr, resp, ms = m.xact(bytes([ADDR, func])
                          + start.to_bytes(2, "big")
                          + qty.to_bytes(2, "big"))
    if not resp or not crc_ok(resp):
        return None, resp, ms
    n = resp[2] // 2
    return [int.from_bytes(resp[3 + i*2:5 + i*2], "big")
            for i in range(n)], resp, ms


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
    N = int(sys.argv[2]) if len(sys.argv) > 2 else 50
    GAP = int(sys.argv[3]) if len(sys.argv) > 3 else 0

    print("=" * 70)
    print("  谨慎复现：连续请求 %d 次，请求间隔 %d ms" % (N, GAP))
    print("  端口 %s @ %d" % (port, BAUD))
    print("=" * 70)

    m = Drv(port)

    # ---- 基线 ----
    base, _, _ = read(m, 4, IREG_MB_RX_LO, 1)
    if base is None:
        print("  ❌ 一开始就没应答 —— 板子已经是卡死状态，先断电重启")
        m.close()
        return 1
    b0 = base[0]
    up0, _, _ = read(m, 4, IREG_UPTIME_LO, 1)
    print("  基线：运行 %s 秒，收帧计数 %d" % (up0, b0))
    print()

    ok = 0
    fail_at = None
    last_stats = None

    t_start = time.time()
    for i in range(1, N + 1):
        r, resp, ms = read(m, 4, 0, 4)      # 读 4 个输入寄存器
        if r is not None:
            ok += 1
            if i % 25 == 0 or i == 1:
                st, _, _ = read(m, 4, IREG_MB_RX_LO, 1)
                print("  [%4d/%d] OK   %6.1f ms   收帧计数=%s"
                      % (i, N, ms, st[0] if st else "?"))
        else:
            fail_at = i
            print()
            print("  " + "!" * 62)
            print("  ❌ 第 %d 次请求失败！" % i)
            print("     发出: %s" % m.xact.__self__ and "")
            print("     收 len=%d 内容=%s" % (len(resp), resp.hex().upper() or "(空)"))
            print("     本次耗时 %.1f ms" % ms)
            print("  " + "!" * 62)

            # 立刻多试几次，看是"偶发"还是"永久死"
            print("     立刻重试 3 次：")
            for k in range(3):
                r2, resp2, ms2 = read(m, 4, 0, 4)
                print("       重试%d: %s  (%.1f ms)"
                      % (k + 1, "OK" if r2 else "无应答", ms2))
                if r2:
                    print("       -> 是**偶发**丢帧，不是永久卡死")
                    break
            else:
                print("       -> **永久无应答**")

            # 读统计：这是区分"收不到"和"发不出"的关键
            st, _, _ = read(m, 4, IREG_MB_RX_LO, 1)
            up, _, _ = read(m, 4, IREG_UPTIME_LO, 1)
            last_stats = (up[0] if up else None, st[0] if st else None)
            print("     读统计: 运行秒数=%s  收帧计数=%s"
                  % (last_stats[0], last_stats[1]))
            if last_stats[1] is not None:
                print("     ⚠️ 若收帧计数**没有**从 %d 继续增长，"
                      "说明从站根本没收到请求" % b0)
                print("     ⚠️ 若收帧计数**还在涨**但不应答，"
                      "说明从站收到了、但发不回去")
            break

        if GAP:
            time.sleep(GAP / 1000.0)

    dt = time.time() - t_start
    print()
    print("=" * 70)
    print("  结果")
    print("=" * 70)
    print("  成功 %d / %d   总耗时 %.2f s" % (ok, N, dt))
    if N:
        print("  平均 %.2f ms/次" % (dt * 1000.0 / N))
    if fail_at:
        print("  ❌ 在第 %d 次失败" % fail_at)
        print("  最终统计: 运行秒数=%s  收帧计数=%s" % last_stats)
    else:
        print("  ✅ %d 次全部成功 —— 未复现卡死" % N)
        print("     可以加大次数或减小间隔继续试")
    m.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
