#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
被动监听 RS485 总线 —— 不发任何东西，只看总线上有没有帧

============================================================================
 为什么需要它（它是"链路通没通"最快的指示器）

  排查"收不到"时，最难的是把"软件没发"和"线没接好"分开：

     主动探测（发一帧、等应答）失败 → 可能是软件没发 / 线断了 / 对面没答，
                                      三种原因现象完全一样
     被动监听（只听）            → 有帧 = 总线上确实有人在发

  ⚠️ 本项目的 STM32 主站启用后**每秒都会发轮询请求**，
    所以"被动监听"变成了一条免费的判据：

        总线上看得到 02 04 00 00 00 04 xx xx  ->  发送方向通了
        一直安静                              ->  板子没发 / 模块没上总线 / 不在同一条线上

    排查接线时可以一边动线一边看它，比"发一帧等应答"直接得多。

============================================================================
 用法

    python tools/listen_bus.py COM8            # 听 6 秒
    python tools/listen_bus.py COM8 30         # 听 30 秒（动线时用这个）

    输出示例：

        [  1.204 s] 收到 8 字节: 0204000000 04C1 3B
          -> 从机地址 2 FC04 起始 0 数量 4   CRC OK

  ⚠️ 监听期间**不要**同时跑其它要占用同一个 COM 口的脚本
     （Windows 串口是独占的）。
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

BAUD = 115200


def crc16(d: bytes) -> int:
    c = 0xFFFF
    for b in d:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c & 0xFFFF


def crc_ok(fr: bytes) -> bool:
    return len(fr) >= 4 and crc16(fr[:-2]) == (fr[-2] | (fr[-1] << 8))


def describe(fr: bytes) -> str:
    """尽量把一帧解释成人话（只认最常见的两种：读请求与读响应）"""
    if len(fr) < 4:
        return ""
    addr, fc = fr[0], fr[1]
    if fc in (0x03, 0x04) and len(fr) == 8:
        return ("-> 从机地址 %d FC%02d 起始 %d 数量 %d"
                % (addr, fc, int.from_bytes(fr[2:4], "big"),
                   int.from_bytes(fr[4:6], "big")))
    if (fc & 0x80) and len(fr) == 5:
        return "-> 从机地址 %d 异常响应 FC%02X 码 %d" % (addr, fc, fr[2])
    if fc in (0x03, 0x04) and len(fr) >= 5:
        return ("-> 从机地址 %d FC%02d 响应 %d 字节数据"
                % (addr, fc, fr[2]))
    return ""


def main():
    if len(sys.argv) < 2:
        print("用法: python tools/listen_bus.py <COM口> [秒数]")
        return 2

    port = sys.argv[1]
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0

    try:
        s = serial.Serial(port, BAUD, bytesize=8, parity=serial.PARITY_NONE,
                          stopbits=1, timeout=0.05)
    except Exception as e:                      # noqa: BLE001
        print("打不开 %s: %s" % (port, e))
        return 2

    print("=" * 74)
    print("  被动监听 %s @ %d，%.1f 秒（本脚本不发任何字节）" % (port, BAUD, seconds))
    print("=" * 74)
    print()
    print("  ⚠️ 若 STM32 已启用主站，这里应当**周期性**出现请求帧。")
    print("     一直安静 = 板子没发 / 模块没上总线 / 不在同一条线上。")
    print()

    t0 = time.time()
    segments = 0
    total = 0
    buf = b""
    last_rx = None

    try:
        while time.time() - t0 < seconds:
            n = s.in_waiting
            if n:
                chunk = s.read(n)
                now = time.time() - t0
                gap = "" if last_rx is None else "  (距上一段 %.3f s)" % (now - last_rx)
                print("  [%7.3f s] 收到 %d 字节: %s%s"
                      % (now, len(chunk), chunk.hex().upper(), gap))
                segments += 1
                total += len(chunk)
                buf += chunk
                last_rx = now
            else:
                time.sleep(0.005)
    finally:
        s.close()

    print()
    print("  ---- 汇总 ----")
    print("  收到 %d 段，共 %d 字节" % (segments, total))
    if segments == 0:
        print("  ⚠️ 总线上**一个字节都没有**")
        print("     按这个顺序查：模块供电 -> A/B 是否接到适配器 -> A/B 是否接反")
        print("                   -> DE(PA1) 是否接到模块的 DE/RE")
    else:
        print("  ✅ 总线**有流量** —— 至少有一个方向是通的")

    # 按 8 字节切（Modbus 读请求固定 8 字节）做一次粗略解释
    for i in range(0, len(buf) - 7, 8):
        fr = buf[i:i + 8]
        print("    %s  CRC%s %s"
              % (fr.hex().upper(), "OK" if crc_ok(fr) else "错", describe(fr)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
