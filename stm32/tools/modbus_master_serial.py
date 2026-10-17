#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Modbus RTU 串口主站 —— 用 PC 上的 USB-RS485 转换器读/写 STM32 从站

============================================================================
 用途

上板后验证"从站能不能应答"。这是**真正的端到端验证**：
PC 发真实字节，STM32 通过 MAX3485 发回真实字节 ——
中间经过电平转换、双绞线、终端电阻，与 PC 上的单测完全不同。

============================================================================
 ⚠️ 硬件前提（缺一不可）

    STM32 侧：USART2 (PA2/PA3) -> MAX3485 -> A/B 双绞线
              PA1 = DE（方向控制，我的代码里已处理）
              A/B 之间接 120Ω 终端电阻
    PC 侧：    **一个 USB-RS485 转换器**（不是 USB-TTL！）
              它的 A/B 接到 STM32 的 A/B，A 对 A、B 对 B
              共地（GND 对接）

  ⚠️ 常见错误：拿 USB-TTL 直接接 MAX3485 的 A/B ——
     那样信号电平不匹配（TTL 是单端 0~3.3V，RS485 是差分 ±2V），
     收不到任何东西，而两边看起来都"接上了"。

  ⚠️ 如果 USB-RS485 转换器内部自带 120Ω 终端电阻，
     而 STM32 侧也有一个，两个并联变成 60Ω —— 负载过重会削弱信号。
     正规做法是**整条总线只在两端各有一个 120Ω**。

============================================================================
 用法

    python modbus_master_serial.py --port COM5 --list
    python modbus_master_serial.py --port COM5 --read-input 0 2
    python modbus_master_serial.py --port COM5 --read-holding 0 4
    python modbus_master_serial.py --port COM5 --write-holding 3 30
    python modbus_master_serial.py --port COM5 --probe

  ⚠️ 波特率默认 115200，因为**你 CubeMX 里 USART2 配的是 115200**。
     本项目的 GW_RS485_UART_BAUD 与 CubeMX 的 USART2 都配 115200。
     上板后 `gw_baud_port_actual()` 会从 BRR 反算出**实际值**，
     所以以工程配置为准 —— 用 --baud 指定来对齐。

============================================================================
 建立连接时最有用的命令：--probe

    它按"最可能成功"的顺序试几种地址/波特率组合，
    并明确报告"完全没收到字节"还是"收到了但 CRC 错"。
    这两种现象的排查方向完全不同：

        完全没收到  -> 接线 / 方向控制 / 波特率 / 终端电阻
        收到但 CRC 错 -> 波特率接近但不对 / 噪声 / 地线问题
"""

import argparse
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
    print("需要 pyserial：pip install pyserial")
    sys.exit(2)


# ---------------------------------------------------------------- CRC
def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc & 0xFFFF


def with_crc(pdu: bytes) -> bytes:
    c = crc16_modbus(pdu)
    return pdu + bytes([c & 0xFF, c >> 8])


def crc_ok(frame: bytes) -> bool:
    if len(frame) < 4:
        return False
    return crc16_modbus(frame[:-2]) == (frame[-2] | (frame[-1] << 8))


# ---------------------------------------------------------------- 主站
class Master:
    def __init__(self, port, baud, timeout=0.5):
        self.ser = serial.Serial(
            port=port, baudrate=baud, bytesize=8,
            parity=serial.PARITY_NONE, stopbits=1,
            timeout=timeout,
        )
        # ⚠️ 打开串口后清一次输入缓冲：
        #    上电瞬间线上可能有垃圾，不清掉会被当成"应答"。
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def request(self, pdu: bytes, quiet_ms=4):
        """发一帧，等应答。

        ⚠️ quiet_ms = 3.5 字符时间（帧间静默）。
           9600 8N1 下 3.5 字符约 4ms；115200 下约 0.3ms。
           这里取 4ms 偏保守 —— 从站在这个静默期内不会发新帧。
        """
        self.ser.reset_input_buffer()
        frame = with_crc(pdu)
        self.ser.write(frame)
        self.ser.flush()
        time.sleep(quiet_ms / 1000.0)

        # 读满超时时间内的所有字节
        deadline = time.time() + self.ser.timeout
        buf = b""
        while time.time() < deadline:
            n = self.ser.in_waiting
            if n:
                buf += self.ser.read(n)
                deadline = time.time() + 0.05   # 收到字节后再等一小会儿
            else:
                time.sleep(0.005)
        return frame, buf


# ---------------------------------------------------------------- 显示
def hexs(b: bytes) -> str:
    return b.hex().upper() if b else "(空)"


def show(req: bytes, resp: bytes, label: str):
    print("  %s" % label)
    print("    发送: %s" % hexs(req))
    print("    接收: %s  (%d 字节)" % (hexs(resp), len(resp)))
    if not resp:
        print("    -> ❌ 没有任何字节返回")
        return None
    if not crc_ok(resp):
        print("    -> ⚠️ 收到字节但 CRC 不对（波特率接近但不对？噪声？）")
        return None
    if len(resp) >= 2 and (resp[1] & 0x80):
        exc = resp[2] if len(resp) > 2 else 0
        names = {1: "非法功能码", 2: "非法数据地址", 3: "非法数据值",
                 4: "从站故障"}
        print("    -> ⚠️ 从站回异常码 0x%02X（%s）"
              % (exc, names.get(exc, "未知")))
        return None
    print("    -> ✅ CRC 正确")
    return resp


# ---------------------------------------------------------------- 命令
def cmd_read(m, addr, func, start, qty):
    pdu = bytes([addr, func]) + start.to_bytes(2, "big") \
          + qty.to_bytes(2, "big")
    name = "读输入寄存器" if func == 4 else "读保持寄存器"
    req, resp = m.request(pdu)
    r = show(req, resp, "%s addr=%d start=%d qty=%d" % (name, addr, start, qty))
    if r:
        bc = r[2]
        vals = [int.from_bytes(r[3 + i*2:5 + i*2], "big")
                for i in range(bc // 2)]
        for i, v in enumerate(vals):
            print("      [%2d] = %5d  (0x%04X)" % (start + i, v, v))
    return r


def cmd_write(m, addr, reg, val):
    pdu = bytes([addr, 6]) + reg.to_bytes(2, "big") + val.to_bytes(2, "big")
    req, resp = m.request(pdu)
    return show(req, resp, "写单个保持寄存器 addr=%d reg=%d val=%d"
                % (addr, reg, val))


def cmd_probe(m, addrs, bauds):
    """按顺序试各种组合，明确区分"没收到"与"CRC 错" """
    print("=== 探测从站（每个组合发一次读保持寄存器 0，1 个）===")
    for baud in bauds:
        m.ser.baudrate = baud
        time.sleep(0.1)
        for addr in addrs:
            pdu = bytes([addr, 3]) + (0).to_bytes(2, "big") \
                  + (1).to_bytes(2, "big")
            # ⚠️ 每个组合**重试 3 次**（2026-10-10 的教训）：
            #    这块板子自己就是主站、每 ~0.8 秒在总线上发一次轮询，
            #    单次探测经常与它撞车 -> 表现为"无应答/CRC 错"，
            #    我因此一度把"能正常应答的从站"误判成"RS485 不通"。
            #    判据应当是：**重试里只要成功一次 = 通路正常**。
            best = b""
            for _try in range(3):
                req, resp = m.request(pdu)
                if resp and crc_ok(resp):
                    best = resp
                    break
                if resp and not best:
                    best = resp
                time.sleep(0.1)
            resp = best
            stat = "(无应答)" if not resp else (
                "CRC OK" if crc_ok(resp) else "CRC 错")
            print("  波特率 %-6d 地址 %-3d -> %-18s 收 %d 字节: %s"
                  % (baud, addr, stat, len(resp), hexs(resp)))
    print()
    print("  判读：")
    print("    全部「无应答」        -> 接线 / DE 方向 / 波特率全不对")
    print("    有「CRC 错」          -> 波特率接近但不对，或噪声/地线")
    print("    有「CRC OK」          -> ✅ 通了！记下那组波特率与地址")


# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description="Modbus RTU 串口主站")
    ap.add_argument("--port", help="串口号，例如 COM5")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--addr", type=int, default=1, help="从站地址（默认 1）")
    ap.add_argument("--list", action="store_true", help="列出串口")
    ap.add_argument("--probe", action="store_true", help="探测从站")
    ap.add_argument("--read-input", nargs=2, type=int,
                    metavar=("START", "QTY"))
    ap.add_argument("--read-holding", nargs=2, type=int,
                    metavar=("START", "QTY"))
    ap.add_argument("--write-holding", nargs=2, type=int,
                    metavar=("REG", "VAL"))
    args = ap.parse_args()

    if args.list or not args.port:
        import serial.tools.list_ports as lp
        print("可用串口：")
        for p in lp.comports():
            print("  %-8s %s" % (p.device, p.description))
        print()
        print("⚠️ 连 RS485 需要 **USB-RS485 转换器**，")
        print("   USB-TTL (CH340/CP2102) 不能直连 MAX3485 的 A/B。")
        return 0

    m = Master(args.port, args.baud)
    print("已打开 %s @ %d 8N1" % (args.port, args.baud))
    try:
        if args.probe:
            # ⚠️ 探测时把常见波特率都试一遍。
            #    工程里 USART2 配的是 115200，但我的代码默认 9600，
            #    所以两个都试，让现象说话。
            # ⚠️ 地址列表要去重并排除已包含的 args.addr。
            #    原来写的是 [args.addr, 1, 2]，当 args.addr==1 时会把地址 1
            #    测两遍，输出看起来像“同一组合跑了两次”。
            addrs = []
            for a in (args.addr, 1, 2):
                if a not in addrs:
                    addrs.append(a)
            bauds = []
            for b in (args.baud, 9600, 19200, 38400):
                if b not in bauds:
                    bauds.append(b)
            cmd_probe(m, addrs, bauds)
        if args.read_input:
            cmd_read(m, args.addr, 4, args.read_input[0], args.read_input[1])
        if args.read_holding:
            cmd_read(m, args.addr, 3, args.read_holding[0],
                     args.read_holding[1])
        if args.write_holding:
            cmd_write(m, args.addr, args.write_holding[0],
                      args.write_holding[1])
        if not any([args.probe, args.read_input, args.read_holding,
                    args.write_holding]):
            print("没有指定动作，试试 --probe 或 --read-holding 0 4")
    finally:
        m.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
