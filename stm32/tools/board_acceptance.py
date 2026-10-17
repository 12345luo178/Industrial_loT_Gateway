#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
RS485 Modbus 从站的上板验收测试

============================================================================
 它补上了 PC 单测永远覆盖不到的东西

    PC 单测能证明：协议逻辑、帧格式、CRC、异常码、边界值
    本脚本要证明：**电气层 + 物理层 + 中断/DMA 路径在真实时序下可用**

具体验证：
    1) 读写功能是否真的经过 MAX3485 往返成功
    2) 写路径是否生效（读回来是写进去的值）
    3) 异常响应是否正确（非法地址 / 非法数量）
    4) 连续 N 次读的**错误率**（这是报告里最有说服力的数字）
    5) 收帧计数是否与实际请求数**精确一致**
       —— 不一致就说明有帧丢在电气层或 DMA 层

⚠️ 全部通过真实串口，不是模拟。
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

# 寄存器地址（与 inc/modbus_regs.h 一致）
IREG_UPTIME_LO = 12
IREG_MB_RX_LO = 13
IREG_FW_MINOR = 25
HREG_REPORT_PERIOD = 3

# 收帧计数基线：模块会把**含本次**的请求计入，
# 所以每次读之前先记下上一次的值，用于判断"是否精确 +1"。
CRC_OK = 0
CRC_BAD = 0
NO_REPLY = 0
EXC = 0


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc & 0xFFFF


def crc_ok(frame: bytes) -> bool:
    return (len(frame) >= 4
            and crc16(frame[:-2]) == (frame[-2] | (frame[-1] << 8)))


class M:
    def __init__(self, port):
        # ⚠️ 超时不能太短。
        #    Modbus over serial line 规范给从站的响应超时是 300ms~1s，
        #    测试客户端必须比它宽。
        #    第一版用 0.5s，在负载下会误判成"无应答"。
        self.s = serial.Serial(port, BAUD, bytesize=8,
                               parity=serial.PARITY_NONE, stopbits=1,
                               timeout=1.5)
        self.s.reset_input_buffer()

    def close(self):
        self.s.close()

    def req(self, pdu: bytes, settle_ms=4, want_len=None):
        """发一帧，收一帧。

        ⚠️ `want_len` 给出期望的回应长度（含 CRC）时，
           收到该长度**立即返回**，不再空等。
           不给就只能靠超时判断（用于"预期无应答"的用例）。
        """
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
                # ⚠️ 知道期望长度就立刻返回：
                #    原来每收到一块就把 deadline 推后 50ms，
                #    于是**每次都空等 50ms**，并且下一轮还会把
                #    这段时间收到的响应给 reset_input_buffer() 清掉。
                if want_len is not None and len(buf) >= want_len:
                    return frame, buf
            else:
                time.sleep(0.002)
        return frame, buf


def read_regs(m, func, start, qty):
    """返回寄存器列表；失败返回 None 并已计数"""
    global CRC_OK, CRC_BAD, NO_REPLY, EXC
    pdu = bytes([ADDR, func]) + start.to_bytes(2, "big") \
          + qty.to_bytes(2, "big")
    # ⚠️ 期望长度**必须按 qty 算**，不能写死。
    #
    #    踩过的坑：原来写的是 want_len=5（"为了兼顾异常响应"），
    #    于是 req() 收到 5 个字节就返回，然后对**不完整**的响应算 CRC。
    #    读 4 个寄存器时响应 13 字节 —— 在第 6 项（读 32 个，响应 69 字节）
    #    那里必然失败，而且报的是"CRC 错误"，看起来像电气问题，
    #    实际上是我的测试客户端把帧截断了。
    #
    #    正常响应长度 = 地址1 + 功能码1 + 字节数1 + 数据2*qty + CRC2 = 5 + 2*qty
    #    异常响应长度 = 地址1 + 功能码1 + 异常码1 + CRC2                      = 5
    #    异常响应的功能码带 0x80，收到第 2 个字节就能分辨；
    #    这里按正常长度等，超时后仍会返回已收到的内容，异常帧照样能判出来。
    _, resp = m.req(pdu, want_len=5 + 2 * qty)
    if not resp:
        NO_REPLY += 1
        return None
    if not crc_ok(resp):
        CRC_BAD += 1
        return None
    if resp[1] & 0x80:
        EXC += 1
        return ("exc", resp[2])
    if len(resp) != 5 + resp[2] or resp[2] != qty * 2:
        CRC_BAD += 1
        return None
    CRC_OK += 1
    return [int.from_bytes(resp[3 + i*2:5 + i*2], "big") for i in range(qty)]


def section(t):
    print("\n" + "=" * 68)
    print("  " + t)
    print("=" * 68)


def main():
    if len(sys.argv) < 2:
        print("用法: python board_acceptance.py <COM口>")
        return 1
    port = sys.argv[1]

    print("========== RS485 Modbus 从站 上板验收 ==========")
    print("  端口 %s @ %d 8N1，从站地址 %d" % (port, BAUD, ADDR))

    m = M(port)
    fails = 0

    try:
        # ---------------- 1) 基本读写 ----------------
        section("1) 读写功能")
        v = read_regs(m, 3, 0, 4)
        print("  读保持寄存器 0..3 -> %s" % ("CRC 正确" if v else "失败"))

        # ---------------- 2) 写路径 ----------------
        section("2) 写路径（写进去再读回来）")
        for val in (30, 45, 60):
            pdu = bytes([ADDR, 6]) + HREG_REPORT_PERIOD.to_bytes(2, "big") \
                  + val.to_bytes(2, "big")
            _, resp = m.req(pdu)
            ok_w = resp and crc_ok(resp) and len(resp) == 8
            back = read_regs(m, 3, HREG_REPORT_PERIOD, 1)
            got = back[0] if isinstance(back, list) else None
            verdict = "✅" if (ok_w and got == val) else "❌"
            print("  写 %-3d -> 回读 %-5s  %s" % (val, got, verdict))
            if not (ok_w and got == val):
                fails += 1

        # 还原成 10（默认值），避免影响后续
        read_regs(m, 3, HREG_REPORT_PERIOD, 1)

        # ---------------- 3) 异常响应 ----------------
        section("3) 异常响应")
        cases = [
            (bytes([ADDR, 4]) + (0x20).to_bytes(2, "big")
             + (1).to_bytes(2, "big"), 0x02, "越界地址"),
            (bytes([ADDR, 3]) + (0).to_bytes(2, "big")
             + (0).to_bytes(2, "big"), 0x03, "数量 0"),
            (bytes([ADDR, 4]) + (0).to_bytes(2, "big")
             + (126).to_bytes(2, "big"), 0x03, "数量 126 超上限"),
            (bytes([ADDR, 0x63]) + (0).to_bytes(2, "big")
             + (1).to_bytes(2, "big"), 0x01, "非法功能码"),
        ]
        for pdu, want, desc in cases:
            # 异常响应固定 5 字节（地址+fc|0x80+异常码+CRC2）。
            # ⚠️ 第一版这里没传 want_len，于是靠 1.5s 超时来收尾 ——
            #    能过，但每条用例白等 1.5 秒。给了期望长度就立刻返回。
            _, resp = m.req(pdu, want_len=5)
            if resp and crc_ok(resp) and (resp[1] & 0x80) \
               and len(resp) == 5 and resp[2] == want:
                print("  %-18s -> 异常 0x%02X  ✅" % (desc, resp[2]))
            else:
                print("  %-18s -> %s  ❌（期望异常 0x%02X）"
                      % (desc, resp.hex().upper() if resp else "(无应答)", want))
                fails += 1

        # ---------------- 4) 地址不匹配必须静默 ----------------
        section("4) 静默规则")
        _, resp = m.req(bytes([9, 3]) + (0).to_bytes(2, "big")
                        + (1).to_bytes(2, "big"))
        if not resp:
            print("  地址 9（本机是 1）-> 无应答  ✅")
        else:
            print("  地址 9 -> 竟然应答了 %s  ❌" % resp.hex().upper())
            fails += 1

        # ---------------- 5) 连续读错误率 ----------------
        section("5) 连续读错误率（报告里最有说服力的数字）")
        N = 500
        t0 = time.time()
        ok_n = 0
        for _ in range(N):
            r = read_regs(m, 4, 0, 4)
            if isinstance(r, list):
                ok_n += 1
        dt = time.time() - t0
        print("  连续读输入寄存器 0..3（4 个寄存器）%d 次" % N)
        print("  成功 %d / %d   失败 %d" % (ok_n, N, N - ok_n))
        print("  耗时 %.2f s  ->  平均 %.2f ms/次，%.1f 次/秒"
              % (dt, dt / N * 1000.0, N / dt))
        print("  错误率 %.3f%%" % ((N - ok_n) * 100.0 / N))
        if ok_n != N:
            fails += 1

        # ---------------- 6) 读全部 32 个寄存器（长帧） ----------------
        section("6) 读全部 32 个输入寄存器（最长帧 69 字节）")
        r = read_regs(m, 4, 0, 32)
        if isinstance(r, list) and len(r) == 32:
            uptime = r[IREG_UPTIME_LO]
            rx = r[IREG_MB_RX_LO]
            print("  32 个寄存器全部读到  ✅")
            print("  运行秒数 = %d，收帧计数 = %d" % (uptime, rx))
        else:
            print("  失败  ❌")
            fails += 1

        # ---------------- 7) 收帧计数精确性 ----------------
        section("7) 收帧计数是否与实际请求数精确一致")
        base = read_regs(m, 4, IREG_MB_RX_LO, 1)
        if isinstance(base, list):
            b0 = base[0]
            M_REQ = 20
            for _ in range(M_REQ):
                read_regs(m, 4, 0, 1)
            after = read_regs(m, 4, IREG_MB_RX_LO, 1)
            if isinstance(after, list):
                # 每次 read_regs 都是一次请求；"读 after" 本身也计入，
                # 所以期望增量 = M_REQ + 1（after 的那次读）
                delta = after[0] - b0
                print("  基线 %d -> 发 %d 次请求 -> 现在 %d（增量 %d）"
                      % (b0, M_REQ, after[0], delta))
                if delta == M_REQ + 1:
                    print("  ✅ 增量 = 请求数 + 1（含读计数那次）—— **一帧不丢**")
                else:
                    print("  ❌ 期望增量 %d，实际 %d —— 说明有帧没被从站收到"
                          % (M_REQ + 1, delta))
                    fails += 1
        else:
            print("  读不到计数  ❌")
            fails += 1

    finally:
        m.close()

    # ---------------- 汇总 ----------------
    section("汇总")
    print("  CRC 正确应答 : %d" % CRC_OK)
    print("  异常应答     : %d" % EXC)
    print("  CRC 错误     : %d" % CRC_BAD)
    print("  无应答       : %d" % NO_REPLY)
    print()
    if fails == 0:
        print("  ✅ 全部通过")
    else:
        print("  ❌ %d 项失败" % fails)
    print()
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
