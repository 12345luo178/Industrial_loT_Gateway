#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
识别哪个 COM 口是 STM32 的调试口（USART1）

============================================================================
 为什么需要它

 两个 USB 串口都是 CH340（VID_1A86&PID_7523），描述一模一样，
 无法从名字区分哪个是：
     调试口（USART1）—— 会持续吐 [SELFCHECK] / [READY] 日志
     RS485 口        —— 只在收到 Modbus 请求时才应答

 判据：**打开后主动读 2 秒，看有没有日志**。
 日志是周期性出现的（启动时一次性打印），所以如果设备已经启动完了，
 可能读不到 —— 那就提示用户复位一次再试。
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

PORTS = ["COM7", "COM8"]
BAUD = 115200


def probe(port: str, seconds: float = 2.0):
    print("=" * 66)
    print("  打开 %s @ %d 8N1，读 %.1f 秒 ..." % (port, BAUD, seconds))
    print("=" * 66)

    try:
        ser = serial.Serial(port=port, baudrate=BAUD, bytesize=8,
                            parity=serial.PARITY_NONE, stopbits=1,
                            timeout=0.2)
    except Exception as e:
        print("  ❌ 打开失败: %s" % e)
        return None

    # ⚠️ 不主动动 DTR/RTS。有些 USB-RS485 模块用 DTR 做方向控制，
    #    乱动会干扰总线。这里只读。

    buf = b""
    deadline = time.time() + seconds
    try:
        while time.time() < deadline:
            n = ser.in_waiting
            if n:
                buf += ser.read(n)
                deadline = time.time() + 0.3   # 收到就再多等一点
            else:
                time.sleep(0.02)
    except Exception as e:
        # ⚠️ Windows 上"串口被别的程序占用"**不只在 open() 时报**，
        #    也可能在之后的 in_waiting / read 时报 PermissionError。
        #    第一版只包了 open()，于是这里直接抛异常中断了整个脚本 ——
        #    COM7 被串口助手占着时就复现了。
        print("  ⚠️ 读取过程中出错（多半是**该端口被别的程序占用**）")
        print("     %s" % e)
        print("     -> 请关闭串口助手 / Modbus 工具后重试")
        try:
            ser.close()
        except Exception:
            pass
        return "busy"

    ser.close()

    if not buf:
        print("  ❌ 一个字节都没收到")
        return ""

    text = buf.decode("utf-8", "replace")
    printable = sum(1 for c in text if c.isprintable() or c in "\r\n\t")
    ratio = printable / max(1, len(text))

    print("  收到 %d 字节，可打印比例 %.0f%%" % (len(buf), ratio * 100))
    print("  ---- 内容（前 600 字符）----")
    for ln in text[:600].split("\n"):
        print("    " + ln.rstrip())
    print("  ----------------------------")

    if "[SELFCHECK]" in text or "[READY]" in text or "[BOOT]" in text:
        print("  >>> 判据：含 [SELFCHECK]/[READY]/[BOOT] -> **调试口（USART1）**")
        return "debug"
    if ratio > 0.9 and len(buf) > 20:
        print("  >>> 判据：可打印文本但不是我们已知的日志格式 —— 需要人判断")
        return "text"
    print("  >>> 判据：像是二进制/噪声 —— 可能是 RS485 侧的残留，或波特率不对")
    return "binary"


def main():
    print()
    print("提示：如果两个口都读不到东西，请**按一下板子的复位键**，")
    print("      然后在 3 秒内重跑本脚本（启动日志只在复位后打印一次）。")
    print()

    results = {}
    for p in PORTS:
        results[p] = probe(p)
        print()

    print("=" * 66)
    print("  结论")
    print("=" * 66)
    for p, r in results.items():
        # ⚠️ 不能用 `r if r else ...`：
        #    “打开成功但一个字节没收到”返回的是空字符串 ""，
        #    它在 Python 里是 **falsy** —— 于是会被误报成“打不开”。
        #    这两种情况含义完全不同：
        #      None -> 端口被占用 / 不存在 -> 查端口
        #      ""   -> 端口能用，但总线上没数据 -> 查硬件/供电
        if r is None:
            print("  %-6s -> ❌ 打不开（端口被占用或不存在）" % p)
        elif r == "busy":
            print("  %-6s -> 🔒 **被别的程序占用**（关掉串口助手）" % p)
        elif r == "":
            print("  %-6s -> ⚠️ 能打开，但**一个字节都没收到**" % p)
        else:
            print("  %-6s -> %s" % (p, r))

    dbg = [p for p, r in results.items() if r == "debug"]
    if len(dbg) == 1:
        other = [p for p in PORTS if p not in dbg][0]
        print()
        print("  调试口   = %s" % dbg[0])
        print("  RS485 口 = %s   <- 用这个测 Modbus" % other)
    elif not dbg:
        print()
        if all(r == "" for r in results.values()):
            print("  两个口都能打开，但都**收不到任何字节**。")
            print("  这不是端口问题，而是：")
            print("    1) 板子没上电 / 没跑起来")
            print("    2) 调试线（CH340）的 TX/RX 没接到 PA9/PA10")
            print("    3) 板子正在复位中或被按住了 RESET")
            print("    4) 固件没烧进去")
            print()
            print("  ⚠️ 最常见的原因：为了接 RS485 把板子断电了。")
        else:
            print("  没有识别出调试口。请复位板子后立刻重跑。")
    else:
        print()
        print("  ⚠️ 两个口都像调试口？可疑 —— 检查接线。")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
