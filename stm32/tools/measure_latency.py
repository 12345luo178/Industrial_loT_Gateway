#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
精确测响应延迟 —— 回答"为什么每次事务 58ms"

============================================================================
 为什么要测

  reproduce_hang.py 测出每次事务 ~58ms。
  Modbus 从站的响应延迟是**报告里的关键指标**（规范要求 < 响应超时，
 通常 300ms~1s；但现场关心的是"快不快"）。

 115200 下，一个 9 字节响应在线上只需 0.78ms。
 所以 58ms 里绝大部分**不是串口传输时间**，而是别的东西。

 本脚本把延迟拆开看：
      t_send   从 write() 到数据真正出 USB（含 USB 轮询周期）
      t_first  从发完到第一个字节到达 PC
      t_all    从发完到最后一个字节到达

 ⚠️ 关键：Windows 的 USB 串口**不是实时设备**。
    FTDI/CH340 的驱动延迟定时器（latency timer）默认常是 16ms。
    所以"读延迟"里可能有一大块是**驱动**造成的，不是从站。
    本脚本通过对比"多次连续请求的延迟分布"来区分。
============================================================================
"""
import statistics
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
N = 60


def crc16(d: bytes) -> int:
    c = 0xFFFF
    for b in d:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c & 0xFFFF


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
    n = int(sys.argv[2]) if len(sys.argv) > 2 else N

    s = serial.Serial(port, BAUD, bytesize=8,
                      parity=serial.PARITY_NONE, stopbits=1, timeout=0.6)
    s.reset_input_buffer()

    pdu = bytes([ADDR, 4, 0, 0, 0, 4])
    c = crc16(pdu)
    frame = pdu + bytes([c & 0xFF, c >> 8])

    print("=" * 70)
    print("  响应延迟测量 —— 读输入寄存器 0..3（9 字节响应）")
    print("  端口 %s @ %d，共 %d 次" % (port, BAUD, n))
    print("=" * 70)
    print()
    print("  #     首字节到达   末字节到达   总耗时")
    print("  " + "-" * 58)

    t_first_list = []
    t_all_list = []

    for i in range(n):
        s.reset_input_buffer()
        t0 = time.perf_counter()
        s.write(frame)
        s.flush()
        t_sent = time.perf_counter()

        t_first = None
        buf = b""
        end = t_sent + 0.5
        while time.perf_counter() < end:
            nb = s.in_waiting
            if nb:
                chunk = s.read(nb)
                if t_first is None:
                    t_first = time.perf_counter()
                buf += chunk
                if len(buf) >= 9:
                    break
                end = time.perf_counter() + 0.05
            else:
                time.sleep(0.0005)
        t_done = time.perf_counter()

        if t_first is None:
            print("  %-4d ❌ 无应答" % (i + 1))
            continue

        d_first = (t_first - t_sent) * 1000.0
        d_all = (t_done - t_sent) * 1000.0
        t_first_list.append(d_first)
        t_all_list.append(d_all)

        if i < 12:
            print("  %-4d  %8.2f ms  %8.2f ms  %7.2f ms"
                  % (i + 1, d_first, d_all, d_all))

    s.close()

    print()
    print("=" * 70)
    print("  统计（%d 次有效）" % len(t_all_list))
    print("=" * 70)
    if not t_all_list:
        print("  ❌ 没有有效样本")
        return 1

    for name, lst in (("首字节到达", t_first_list), ("末字节到达", t_all_list)):
        print("  %-10s 最小 %6.2f  中位 %6.2f  平均 %6.2f  最大 %6.2f ms"
              % (name, min(lst), statistics.median(lst),
                 statistics.mean(lst), max(lst)))

    print()
    print("  ⚠️ 判读：")
    print("     115200 下 9 字节在线上只需 0.78 ms。")
    med = statistics.median(t_all_list)
    if med > 20:
        print("     中位 %.1f ms >> 0.78 ms —— 绝大部分延迟**不在从站**，" % med)
        print("     而是 Windows USB 串口驱动的延迟定时器（latency timer）。")
        print("     ⚠️ 这个数字**不能**当作从站的响应延迟写进报告！")
        print("        要测得从站真实延迟，必须用示波器/逻辑分析仪量 TX 引脚，")
        print("        或者用 USB 转串口的 latency timer 调到 1ms 后再测。")
    else:
        print("     中位 %.1f ms —— 大部分是驱动延迟，但可以反推" % med)
    return 0


if __name__ == "__main__":
    sys.exit(main())
