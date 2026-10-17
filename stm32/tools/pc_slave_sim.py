#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
PC 端 Modbus RTU 从机模拟器 —— 用来验证 STM32 上的**主站**

============================================================================
 为什么需要它

   本工程的 STM32 可以同时当**从站**和**主站**（主从共存），
   但验证主站需要一个"有东西应答它"的从机。

   接线只有一个 USB-RS485 适配器，所以让 **PC 当从机**最省事：

       PC (本脚本) ── USB-RS485 ── MAX3485 ── STM32 (跑主站 + 从站)

   ⚠️ 但不能反向接：PC 和 STM32 都在同一条 RS485 总线上，
      所以 PC 只当从机、STM32 只当主站，不会互相打。

============================================================================
 它提供什么

   - 响应功能码 0x03（读保持寄存器）/ 0x04（读输入寄存器）
   - 寄存器内容按**确定性公式**生成，这样主站读到什么值是
     **可预测、可核对**的：      value[i] = (base + i * 3) & 0xFFFF
   - 支持通过命令行改从站地址、起始值
   - ⚠️ **不响应**地址不匹配的请求（静默规则），这样能验证主站
     真的按地址寻址，而不是"随便发一帧就当成成功"

============================================================================
 用法
     python pc_slave_sim.py COM8 --addr 2 --base 1000
     python pc_slave_sim.py COM8 --addr 2 --verbose      # 打印每一帧

  ⚠️ 地址必须与 STM32 主站登记的轮询地址一致 —— 固件里是 2
     （MB_L0_MASTER_SLAVE_ADDR）。**不要用 1**：1 是 STM32 自己
     当从站用的地址，同一地址在总线上出现两次会让 STM32 把
     "上位机发给它的请求"误判成"下游从机给它的响应"。
     详见 app/gw_app.h 里 gw_app_master_add_item() 的说明。

  ⚠️ Windows 串口独占：跑本脚本时**不能**同时跑
     verify_master_dual.py（那个脚本自己就兼任从机角色）。

 校验方法（用 tools/modbus_master_serial.py 当主站去读同一个从机）：
     python tools/modbus_master_serial.py --port COM8 --read-input 0 4
     应读出 base+0, base+3, base+6, base+9
============================================================================
"""
import argparse
import sys

# ⚠️ 中文 Windows 的控制台是 GBK，而 Python 按控制台代码页编码 stdout：
#    print 里出现 ⚠️/✅/❌ 就会 UnicodeEncodeError，**脚本在干活之前就死了**。
#    实测就是这么栽的：本脚本与 verify_master_dual.py 都在打印阶段崩掉，
#    表现成"PC 从机没启动 / 验证没做"，而 tools/ci.ps1 里设了
#    PYTHONIOENCODING=utf-8，所以 CI 全绿、完全看不出来。
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


def reg_value(base: int, i: int) -> int:
    """确定性的寄存器内容 —— 让主站读到的值可预测、可核对。

    ⚠️ 用 (base + i*3) 而不是随机或递增 1，原因：
       如果只是 +1，那"错位一个寄存器"和"完全正确"很难区分；
       用 *3 之后，错位会立刻体现为一个对不上的值。"""
    return (base + i * 3) & 0xFFFF


class Slave:
    def __init__(self, addr, base, verbose):
        self.addr = addr
        self.base = base
        self.verbose = verbose
        self.frames = 0
        self.silent = 0          # 被静默忽略的帧数
        self.exceptions = 0

    def handle(self, req: bytes):
        """返回要发的响应（bytes），或 None 表示静默不答"""
        if len(req) < 4:
            return None
        if not crc_ok(req):
            if self.verbose:
                print("    [静默] CRC 错")
            self.silent += 1
            return None
        if req[0] != self.addr:
            # 静默规则：地址不匹配 -> 不应答
            if self.verbose:
                print("    [静默] 地址 %d != 本机 %d" % (req[0], self.addr))
            self.silent += 1
            return None
        if req[0] == 0:
            return None                     # 广播：执行但不答

        fc = req[1]
        if fc not in (0x03, 0x04):
            # 非法功能码 -> 异常 01
            self.exceptions += 1
            return self._exc(fc, 0x01)

        start = (req[2] << 8) | req[3]
        qty = (req[4] << 8) | req[5]

        if qty < 1 or qty > 125:
            self.exceptions += 1
            return self._exc(fc, 0x03)
        if start + qty > 256:               # 本模拟器的寄存器总数
            self.exceptions += 1
            return self._exc(fc, 0x02)

        body = bytes([self.addr, fc, (qty * 2) & 0xFF])
        for i in range(qty):
            v = reg_value(self.base, start + i)
            body += bytes([(v >> 8) & 0xFF, v & 0xFF])
        c = crc16(body)
        return body + bytes([c & 0xFF, c >> 8])

    def _exc(self, fc, code):
        body = bytes([self.addr, fc | 0x80, code])
        c = crc16(body)
        return body + bytes([c & 0xFF, c >> 8])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--addr", type=int, default=1)
    ap.add_argument("--base", type=int, default=0)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--seconds", type=float, default=0.0,
                    help="跑多少秒后自动退出（0 = 一直跑，Ctrl-C 停止）。"
                         "⚠️ 配合「用 SWD 读 STM32 寄存器」时用它："
                         "脚本自己退出会正常关闭串口，不用强杀进程")
    args = ap.parse_args()

    s = serial.Serial(args.port, BAUD, bytesize=8,
                      parity=serial.PARITY_NONE, stopbits=1, timeout=0.05)

    sl = Slave(args.addr, args.base, args.verbose)

    print("=" * 70)
    print("  PC 端 Modbus RTU 从机模拟器")
    print("=" * 70)
    print("  端口 %s @ %d 8N1" % (args.port, BAUD))
    print("  从站地址 %d" % args.addr)
    print("  寄存器内容: value[i] = (%d + i*3) & 0xFFFF" % args.base)
    print("  例: 输入寄存器 0..3 = %s"
          % [reg_value(args.base, i) for i in range(4)])
    print()
    print("  ⚠️ 按 Ctrl-C 停止。收到请求时这里会打印统计。")
    print("=" * 70)

    try:
        last_report = time.time()
        t_start     = time.time()
        while True:
            # ---- 定时退出（--seconds）----
            # ⚠️ 为什么要这个选项：验证主站时要"让 PC 只当从机、同时用
            #    SWD 读 STM32 的寄存器"。若脚本不带时间限制，就只能强杀进程，
            #    而强杀有把串口句柄留在驱动里的风险（本项目的虚拟串口对
            #    就因此被锁过一次）。让它自己退，串口是正常关闭的。
            if args.seconds > 0.0 and (time.time() - t_start) >= args.seconds:
                print()
                print("  已跑满 %.1f 秒，正常退出。共处理 %d 帧（静默 %d，异常 %d）"
                      % (args.seconds, sl.frames, sl.silent, sl.exceptions))
                break

            # ---- 收一帧 ----
            # ⚠️ 读请求（03/04）是**固定的 8 字节**：
            #     地址1 + 功能码1 + 起始地址2 + 数量2 + CRC2 = 8
            #    第一版只读了 6 字节（地址+fc+4），CRC 就只剩 0 字节，
            #    于是每一帧都被判成 CRC 错 —— 模拟器全在静默。
            #
            # 用"读满 8 字节"而不是按静默分帧：本脚本只支持读请求，
            # 长度固定，这样最稳。超时 50ms 足以收全 8 字节
            # （8 字节 @115200 只有 0.7ms）。
            req = b""
            deadline = time.time() + 0.05
            while len(req) < 8 and time.time() < deadline:
                chunk = s.read(8 - len(req))
                if chunk:
                    req += chunk
                    deadline = time.time() + 0.05

            if len(req) < 8:
                # 没收到完整帧：空闲，定期报统计
                if time.time() - last_report > 5.0:
                    print("  [%s] 共处理 %d 帧，静默 %d，异常 %d"
                          % (time.strftime("%H:%M:%S"), sl.frames,
                             sl.silent, sl.exceptions))
                    last_report = time.time()
                continue

            sl.frames += 1
            resp = sl.handle(req)
            if args.verbose:
                print("  RX %s  ->  %s"
                      % (req.hex().upper(),
                         resp.hex().upper() if resp else "(静默)"))
            if resp:
                # ⚠️ 发之前清输入缓冲，避免把自己的回显当成请求
                s.reset_input_buffer()
                s.write(resp)
                s.flush()
                last_report = time.time()
    except KeyboardInterrupt:
        print()
        print("  停止。共处理 %d 帧（静默 %d，异常 %d）"
              % (sl.frames, sl.silent, sl.exceptions))
    finally:
        s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
