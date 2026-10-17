#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
读取片内耗时测量（输入寄存器 36~39）

============================================================================
 这些数字和 PC 端测出来的**不是一回事**

   PC 端（measure_latency.py）：中位 4.46 ms
       —— 里含 USB 串口驱动 + Python + OS 调度，**不能当从站性能**

   片内（本脚本）：DWT 微秒计数器，**不含任何 PC/UBS/驱动开销**
       —— 这才是能写进报告的真值

============================================================================
 ⚠️ 统计是从**开机**累起的

   要测"某个区间内"的表现，需要先清零（`gw_timing_reset()`）。
   本脚本先用"读两次取差值"的思路给出**区间增量**，
   这样至少能看出一段负载下的变化量。

   映射（inc/modbus_regs.h）：
       36  IREG_TM_SLAVE_MAX    从站处理耗时最大值 us
       37  IREG_TM_SLAVE_AVG    从站处理耗时平均值 us
       38  IREG_TM_LOOP_MAX     一轮主循环最大耗时 us
       39  IREG_TM_LOAD_PERMIL  CPU 负载 千分比
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
TM_START = 36
TM_QTY = 4

NAMES = ["从站处理 Max(us)", "从站处理 Avg(us)", "主循环 Max(us)", "CPU 负载(‰)"]

# ⚠️ 哨兵值：它们是"状态"而不是"测量值"。
#    未绑定时若写 0，读出来全是 0 ——
#    而那与"性能极好"在寄存器上**完全一样**。
SENTINEL_UNBOUND = 0xFFFE      # 模块未绑定（没接线）
SENTINEL_NOCALL  = 0xFFFD      # 已绑定但一次都没打点
SENTINEL_SAT     = 0xFFFF      # 超出 uint16 范围被钳位


def crc16(d: bytes) -> int:
    c = 0xFFFF
    for b in d:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c & 0xFFFF


def crc_ok(fr: bytes) -> bool:
    return len(fr) >= 4 and crc16(fr[:-2]) == (fr[-2] | (fr[-1] << 8))


class Drv:
    def __init__(self, port):
        self.s = serial.Serial(port, BAUD, bytesize=8,
                               parity=serial.PARITY_NONE, stopbits=1,
                               timeout=1.5)

    def close(self):
        self.s.close()

    def rd(self, start, qty):
        p = bytes([ADDR, 4]) + start.to_bytes(2, "big") + qty.to_bytes(2, "big")
        c = crc16(p)
        self.s.reset_input_buffer()
        self.s.write(p + bytes([c & 0xFF, c >> 8]))
        self.s.flush()
        want = 5 + 2 * qty
        deadline = time.time() + self.s.timeout
        buf = b""
        while time.time() < deadline:
            n = self.s.in_waiting
            if n:
                buf += self.s.read(n)
                if len(buf) >= want:
                    break
            else:
                time.sleep(0.001)
        if not buf or not crc_ok(buf) or (buf[1] & 0x80) or len(buf) < want:
            return None
        return [int.from_bytes(buf[3+i*2:5+i*2], "big") for i in range(qty)]


def decode(x):
    """把哨兵值译成人话；真实测量值原样返回"""
    if x == SENTINEL_UNBOUND:
        return "??"
    if x == SENTINEL_NOCALL:
        return "--"
    if x == SENTINEL_SAT:
        return "MAX"
    return str(x)


def show(v, tag):
    print("  %-24s" % tag, end="")
    for x in v:
        print(" %9s" % decode(x), end="")
    print()


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
    load_n = int(sys.argv[2]) if len(sys.argv) > 2 else 500

    print("=" * 78)
    print("  片内耗时测量（DWT，输入寄存器 36~39）")
    print("  端口 %s @ %d" % (port, BAUD))
    print("=" * 78)

    d = Drv(port)

    print()
    print("  %-28s %8s %8s %8s %8s" % ("", *NAMES))
    print("  " + "-" * 70)

    b = d.rd(TM_START, TM_QTY)
    if b is None:
        print("  ❌ 读不到。可能：固件还是旧的（没有 36~39），或已卡死。")
        d.close()
        return 1
    show(b, "基线（开机累起）")

    # ---- 制造一段已知负载：连续读 500 次 ----
    print()
    print("  制造负载：连续读 %d 次输入寄存器 0..3 ..." % load_n)
    t0 = time.time()
    okc = 0
    for _ in range(load_n):
        if d.rd(0, 4) is not None:
            okc += 1
    dt = time.time() - t0
    print("  完成：成功 %d / %d，耗时 %.2f s（%.0f 次/秒）"
          % (okc, load_n, dt, load_n / dt if dt else 0))

    a = d.rd(TM_START, TM_QTY)
    if a is None:
        print("  ❌ 负载后读不到 —— 可能卡死了！")
        d.close()
        return 1
    show(a, "负载后")

    print()
    print("=" * 78)
    print("  结果")
    print("=" * 78)

    if SENTINEL_UNBOUND in (b[0], a[0]):
        print("  ❌ 读到 0xFFFE：**本模块没接线**（gw_timing_port_bind 没调到）")
        print("     这不是测量结果 —— 不要当成“耗时为 0”。")
        d.close()
        return 1
    if SENTINEL_NOCALL in (b[0], a[0]):
        print("  ⚠️ 读到 0xFFFD：已绑定但**一次都没打点**（应用层没调 gw_timing_begin）")
        d.close()
        return 1

    for i, nm in enumerate(NAMES):
        print("  %-22s 基线 %-8s 负载后 %-8s 增量 %s"
              % (nm, decode(b[i]), decode(a[i]),
                 ("+" + str(a[i] - b[i])) if isinstance(a[i], int)
                 and isinstance(b[i], int) and a[i] >= b[i] else "?"))

    print()
    print("  合格判据：")
    if isinstance(a[2], int) and a[2] < 10000:
        print("    ✅ 主循环 Max %d us < 10000 us（10ms 节拍）" % a[2])
    else:
        print("    ⚠️ 主循环 Max %s —— 超节拍！" % decode(a[2]))
    if isinstance(a[3], int) and a[3] < 500:
        print("    ✅ CPU 负载 %d‰ < 500‰" % a[3])
    else:
        print("    ⚠️ CPU 负载 %s —— 超一半" % decode(a[3]))

    print()
    print("  " + "=" * 72)
    print("  ⚠️ 关于“从站处理 Max”这个数字 —— 写进报告前必须读完这段")
    print("  " + "=" * 72)
    print("   统计是**从开机累起**的，所以 Max 里包含")
    print("   **启动阶段的离群点**（初始化、自检、Flash 擦写等）。")
    print()
    if isinstance(a[1], int) and isinstance(a[0], int) and a[0] > 10 * max(a[1], 1):
        print("   本次数据就是这种情况：Max %d us 而 Avg %d us" % (a[0], a[1]))
        print("   —— Max 是启动离群点，**不代表稳态响应能力**。")
        print()
        print("   报告里应当这样写：")
        print("     • 稳态处理耗时（Avg） = %d us" % a[1])
        print("     • 上电过程最大 = %d us（含初始化与自检，属启动过程）" % a[0])
        print("     • 不要把 %d us 写成“从站响应最大耗时”" % a[0])
        print()
        print("   ⚠️ 要拿到**稳态的 Max**，需要在跑完一段后**清零统计**再测。")
        print("      本项目尚未提供清零接口（gw_timing_reset 已有，")
        print("      但还没有从 Modbus 旁路出来）—— 已记入待办。")
    else:
        print("   本次 Max 与 Avg 量级接近，说明没有明显离群点。")
    print("  " + "=" * 72)
    d.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
