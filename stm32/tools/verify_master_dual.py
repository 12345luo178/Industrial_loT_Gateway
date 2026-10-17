#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
验证 STM32 的 Modbus **主从共存** —— 一个程序同时扮演两个角色

============================================================================
 为什么需要一个程序扮两个角色

   我们要验证 STM32 上：
      ① 从站：应答上位机的读寄存器请求
      ② 主站：主动轮询下游从机，并把读回的数据写进自己的寄存器表

   正常的做法是"PC 当从机、脚本当主站"两个程序 —— 但只有一个
   USB-RS485 适配器，而 **Windows 的串口是独占的**，
   两个程序打不开同一个 COM 口。

   好在时序上不冲突：STM32 的主站请求和我的主站请求不会同时占总线
   （从站优先级更高，主站会退避）。所以**一个程序完全可以两个角色都扮**：

       循环：
         1) 扮演从机：等一小会儿，看 STM32 有没有来轮询我？
            有 -> 应答它（并记下来）
         2) 扮演主站：向 STM32 发一个读寄存器请求，读回它的状态

   ⚠️ 关键在于从机的请求必须能在**不做任何操作时被动收到**。
     用短超时轮询 in_waiting 就能做到。

============================================================================
 判据（这是"主从共存"的铁证）

   保持寄存器 8~11 = 从机侧的值（= base + i*3）
       -> 说明 STM32 的**主站**真的读到并写回了自己的寄存器表
   同时输入寄存器 26 的 bit0 = 1
       -> 说明 STM32 认为那台从机在线
   而且这一路读寄存器请求是 STM32 的**从站**在应答
       -> 两个角色**同时在工作**
============================================================================
"""
import argparse
import sys

# ⚠️ 中文 Windows 的控制台是 GBK，而 Python 按控制台代码页编码 stdout：
#    print 里出现 ⚠️/✅/❌ 就会 UnicodeEncodeError，**脚本在干活之前就死了**。
#    实测就是这么栽的：本脚本与 pc_slave_sim.py 都在打印阶段崩掉，
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

# STM32 侧的寄存器
IREG_MASTER_ONLINE = 26
IREG_MASTER_ERR    = 27
HREG_MAP_BASE      = 8       # 主站把读回的数据写到这里（保持寄存器 8~11）


def crc16(d: bytes) -> int:
    c = 0xFFFF
    for b in d:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c & 0xFFFF


def crc_ok(fr: bytes) -> bool:
    return len(fr) >= 4 and crc16(fr[:-2]) == (fr[-2] | (fr[-1] << 8))


def frame(pdu: bytes) -> bytes:
    c = crc16(pdu)
    return pdu + bytes([c & 0xFF, c >> 8])


class Dual:
    """同时当"STM32 的下游从机"和"STM32 的上位机主站" """

    def __init__(self, port, sim_addr, sim_base, stm32_addr):
        self.s = serial.Serial(port, BAUD, bytesize=8,
                               parity=serial.PARITY_NONE, stopbits=1,
                               timeout=0.02)
        self.sim_addr = sim_addr
        self.sim_base = sim_base
        self.stm32_addr = stm32_addr
        # 统计
        self.sim_requests = 0     # STM32 主站来轮询我的次数
        self.sim_answered = 0
        self.sim_bytes = b""

    def close(self):
        self.s.close()

    # ---------------- 从机角色 ----------------
    def reg_val(self, i):
        return (self.sim_base + i * 3) & 0xFFFF

    def serve_once(self, budget_s=0.02):
        """被动看 STM32 有没有来轮询我；有就应答。返回是否处理了一帧"""
        deadline = time.time() + budget_s
        buf = b""
        while time.time() < deadline and len(buf) < 8:
            n = self.s.in_waiting
            if n:
                buf += self.s.read(n)
                deadline = time.time() + 0.02
            else:
                time.sleep(0.0005)

        self.sim_bytes += buf
        if len(buf) < 8:
            return False

        # 只处理读请求（03/04）：地址1 + fc1 + start2 + qty2 + CRC2 = 8
        if not crc_ok(buf[:8]):
            return False
        if buf[0] != self.sim_addr:
            return False
        fc = buf[1]
        if fc not in (0x03, 0x04):
            return False

        self.sim_requests += 1
        start = (buf[2] << 8) | buf[3]
        qty = (buf[4] << 8) | buf[5]
        if qty < 1 or qty > 125:
            return False

        body = bytes([self.sim_addr, fc, (qty * 2) & 0xFF])
        for i in range(qty):
            v = self.reg_val(start + i)
            body += bytes([(v >> 8) & 0xFF, v & 0xFF])
        self.s.reset_input_buffer()
        self.s.write(frame(body))
        self.s.flush()
        self.sim_answered += 1
        return True

    # ---------------- 主站角色 ----------------
    def read(self, func, start, qty):
        """读 STM32 的寄存器。返回列表或 None"""
        self.s.reset_input_buffer()
        self.s.write(frame(bytes([self.stm32_addr, func])
                           + start.to_bytes(2, "big")
                           + qty.to_bytes(2, "big")))
        self.s.flush()

        want = 5 + 2 * qty
        deadline = time.time() + 1.0
        buf = b""
        while time.time() < deadline and len(buf) < want:
            n = self.s.in_waiting
            if n:
                buf += self.s.read(n)
            else:
                time.sleep(0.001)
        if len(buf) < 5 or not crc_ok(buf[:5 + (buf[2] if len(buf) > 2 else 0)]):
            return None
        if buf[1] & 0x80:
            return ("exc", buf[2])
        n = buf[2] // 2
        if len(buf) < 5 + 2 * n:
            return None
        return [int.from_bytes(buf[3 + i*2:5 + i*2], "big") for i in range(n)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", default="COM8")
    ap.add_argument("--sim-addr", type=int, default=2,
                    help="我在 STM32 眼里是什么地址（要等于 STM32 轮询的地址，"
                         "默认 2；**不能等于 --stm32**，见下）")
    ap.add_argument("--base", type=int, default=1000)
    ap.add_argument("--stm32", type=int, default=1, help="STM32 从站地址")
    ap.add_argument("--rounds", type=int, default=8)
    args = ap.parse_args()

    # ⚠️ 两个角色**不能用同一个地址** —— 这个坑上板时真栽过。
    #
    #   STM32 的帧归属派发判据是"主站在等 + 站号对得上"（见 app/gw_app.c）。
    #   如果我在从机角色用了和 --stm32 相同的地址，那么我作为主站发出的
    #   **读请求**（站号 = --stm32）会先被喂给 mb_master_on_response()：
    #   站号对、CRC 对、功能码对，只有"字节数"对不上 ——
    #   于是它被记成一次**下游从机响应错误**并跳过一个轮询项。
    #   结果：我每读一次 STM32，就亲手污染一次它的主站统计，
    #   在线位图永远是 0，看起来像"主站没工作"。
    #
    #   正确配置：STM32 本机从站 = 1（--stm32 1），我当从机 = 2（--sim-addr 2）。
    if args.sim_addr == args.stm32:
        print("=" * 72)
        print("  [拒绝运行] --sim-addr 与 --stm32 不能相同（都是 %d）" % args.sim_addr)
        print("=" * 72)
        print("  两个地址相同会让 STM32 分不清「我发出的读请求」和")
        print("  「下游从机给我的响应」，于是每次读它都误记一次从机失败。")
        print()
        print("  正确用法：")
        print("      python %s COM8 --sim-addr 2 --stm32 1 --base 1000"
              % __file__.rsplit("\\", 1)[-1])
        return 2

    d = Dual(args.port, args.sim_addr, args.base, args.stm32)

    print("=" * 72)
    print("  STM32 主从共存验证")
    print("=" * 72)
    print("  端口 %s @ %d" % (args.port, BAUD))
    print("  我当从机：地址 %d，寄存器值 = (%d + i*3)" % (args.sim_addr, args.base))
    print("  我当主站：读 STM32（地址 %d）的寄存器" % args.stm32)
    print()
    print("  ⚠️ STM32 的主站轮询周期约 1 秒（每轮主循环推一次）")
    print("     所以我需要**多等一会儿**才能被它轮询到。")
    print("=" * 72)

    try:
        # ---- 第 1 阶段：先当从机等 STM32 来轮询 ----
        print("\n  [阶段 1] 当从机，等 STM32 主站来轮询（最多 8 秒）...")
        t0 = time.time()
        last_report = t0
        while time.time() - t0 < 8.0:
            d.serve_once(0.05)
            if time.time() - last_report > 2.0:
                print("     已等 %.1f s，收到 STM32 主站请求 %d 次，应答 %d 次"
                      % (time.time() - t0, d.sim_requests, d.sim_answered))
                last_report = time.time()
        print("     -> 收到 %d 次，应答 %d 次" % (d.sim_requests, d.sim_answered))

        if d.sim_requests == 0:
            print("     ❌ STM32 主站**一次都没来轮询** —— 主站没在工作")
            print("        可能原因：")
            print("          - 轮询项登记失败（看 COM7 有没有 [ERR] 日志）")
            print("          - 我当从机的地址与 STM32 轮询的地址不一致")
            print("          - 主站超时/重试参数导致它还没发起第一轮")

        # ---- 第 2 阶段：当主站读 STM32 的寄存器 ----
        print("\n  [阶段 2] 当主站，读 STM32 的状态寄存器")
        for r in range(args.rounds):
            # ⚠️ 每轮之前先当一会儿从机，别让 STM32 的主站请求收不到应答。
            #
            # 0.3 秒不是随便写的：STM32 自从能收到响应之后，轮询周期从
            # ~300ms（超时重发）掉到 ~5.6ms（**177 次/秒**）。
            # 也就是说只要我不服务超过 ~17ms，它就会开始累计失败；
            # 累计 3 次（MB_M_FAIL_TO_OFFLINE）就把我这台从机判成离线 ——
            # 那时"在线位图"必然是 0，看起来像主站没读成功，其实是
            # **我自己的测试节奏把它拖掉的**。
            #
            # （这一条原来是 0.08 秒 + 每轮结束 sleep(0.6)，实测必然读到
            #   位图=0、错误码=4。改成"边服务边读"之后位图才是可信的。）
            d.serve_once(0.30)

            online = d.read(4, IREG_MASTER_ONLINE, 2)
            mapped = d.read(3, HREG_MAP_BASE, 4)

            if online is None or mapped is None:
                print("     [%d] 读失败（online=%s mapped=%s）"
                      % (r + 1, online, mapped))
                continue

            bmp = online[0]
            err = online[1]
            expect = [d.reg_val(i) for i in range(4)]
            ok_map = (mapped == expect)
            ok_online = ((bmp >> (d.sim_addr - 1)) & 1) == 1

            print("     [%d] 主站在线位图=0x%04X (bit%d=%d)  错误码=%d  "
                  "保持寄存器8~11=%s  %s"
                  % (r + 1, bmp, d.sim_addr - 1,
                     1 if ok_online else 0, err, mapped,
                     "✅ 与期望一致" if ok_map else "❌ 期望 %s" % expect))

            # 轮末也继续服务，而不是裸 sleep —— 否则这 0.6 秒里主站的轮询
            # 全都没人应答，等于自己把从机判离线（见上面的说明）。
            t_end = time.time() + 0.6
            while time.time() < t_end:
                d.serve_once(0.05)

    finally:
        d.close()

    print()
    print("=" * 72)
    print("  结论")
    print("=" * 72)
    expect = [d.reg_val(i) for i in range(4)]
    print("  我从机侧应答了 %d 次 STM32 轮询" % d.sim_answered)
    print("  从机侧寄存器期望值: %s" % expect)
    print()
    if d.sim_answered > 0:
        print("  ✅ STM32 的**主站**在工作（它主动来轮询了）")
    else:
        print("  ❌ STM32 的**主站**没在工作")
    print()
    print("  ⚠️ 判据（两条都要看）：")
    print("     ① 主站在线位图里**我这台从机的位**=1 —— 位号是 (从机地址-1)，")
    print("        本脚本用地址 %d，所以看 bit%d。" % (args.sim_addr, args.sim_addr - 1))
    print("     ② 保持寄存器 8~11 == 我从机侧的 (base + i*3)")
    print("     '收到请求'只说明主站在发，**不代表它收到了正确响应**。")
    print()
    print("  ⚠️ 位图如果一直是 0：先确认脚本是不是「边服务边读」")
    print("     （裸 sleep 会让主站在那段时间里累计失败，3 次就判离线 ——")
    print("      那是测试节奏造成的假离线，不是从机真的不在线）。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
