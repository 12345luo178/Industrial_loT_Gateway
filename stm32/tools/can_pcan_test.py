#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
PCAN（PEAK-System）USB-CAN 适配器：发帧 / 收帧 / 看总线状态

============================================================================
 为什么需要它（它是 CAN 验证里的"第二个节点"）

  CAN 与 RS485 有一个本质区别：**发送方必须收到 ACK，这一帧才算发成功**。
  所以单节点（只有 STM32 自己）永远发不出去：它只能一直重发 ->
  发送错误计数上升 -> 错误被动 -> Bus-Off。

  这就是为什么"STM32 的 CAN 通不通"必须引入第二个节点 —— 也就是这个 PCAN。

  ⚠️ 本项目固件侧 CAN 是**只收不发**（`tx_frames` 设计上应为 0），
     所以默认只能看到 PCAN -> STM32 这一个方向。
     但这一条已经能证明 STM32 收发器的**收和发都通**：
     STM32 收到帧的那一刻必须回 ACK，而 ACK 正是它自己的收发器发出去的。
     所以：
         PCAN 发送成功（无 ACK 错误） + STM32 收帧计数 +1
             -> STM32 的 TJA1051 收、发两条通路都正常，接线与终端也对

============================================================================
 依赖

  只需要 PEAK 官方 PCAN-Basic（装完系统里会有 PCANBasic.dll）：
      https://www.peak-system.com/PCAN-Basic.239.0.html
  本脚本用 ctypes 直接调 PCANBasic.dll，**不需要 python-can / 不需要 pip 装包**。

============================================================================
 用法

    python tools/can_pcan_test.py --status
        只看：驱动在不在、设备认没认到、总线当前状态

    python tools/can_pcan_test.py --send --id 0x123 --data 11 22 33 --count 3
        发 3 帧（间隔 100ms），每帧打印结果与总线状态

    python tools/can_pcan_test.py --monitor 10
        听 10 秒，把收到的帧打印出来（带时间戳）

    python tools/can_pcan_test.py --send --id 0x123 --count 200 --period 5
        压力发送（检查有没有 ACK 错误 / Bus-Off）

 ⚠️ 500 kbps 必须两边一致（本工程是 500 kbps：36MHz / (4 x 18)）
"""

import argparse
import ctypes
import ctypes.util
import os
import sys
import time

# ⚠️ Windows 中文控制台默认 GBK，打印非 GBK 字符会抛 UnicodeEncodeError
#    让脚本在"干活之前"就崩掉。所有工具统一加这个守卫（CI 有检查）。
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(errors="replace")
    except Exception:
        pass


# ==================================================================
# PCAN-Basic 常量
#
# ⚠️ 这些值**逐个核对过 PEAK 官方头文件**（PCAN-Basic 包里的 Include/PCANBasic.h）：
#      PCAN_USBBUS1 = 0x51 / USBBUS2 = 0x52
#      PCAN_BAUD_1M = 0x0014 / 500K = 0x001C / 250K = **0x011C**
#      PCAN_MESSAGE_STANDARD = 0x00 / EXTENDED = 0x02
#      TPCANMsg: DWORD ID + BYTE MSGTYPE + BYTE LEN + BYTE DATA[8]（= 16 字节，含对齐）
#    我第一版把 250K 写成了 0x0011（凭空猜的），核对时才发现 —— 这类
#    "看起来人畜无害的数字" 正是最容易错、又最难从现象反推的地方。
# ==================================================================

PCAN_NONEBUS = 0x00
PCAN_USBBUS1 = 0x51
PCAN_USBBUS2 = 0x52

PCAN_BAUD_1M = 0x0014
PCAN_BAUD_500K = 0x001C
PCAN_BAUD_250K = 0x011C

PCAN_MESSAGE_STANDARD = 0x00
PCAN_MESSAGE_RTR = 0x01
PCAN_MESSAGE_EXTENDED = 0x02

PCAN_ERROR_OK = 0x00000
PCAN_ERROR_XMTFULL = 0x00001
PCAN_ERROR_OVERRUN = 0x00002
PCAN_ERROR_BUSLIGHT = 0x00004
PCAN_ERROR_BUSHEAVY = 0x00008
PCAN_ERROR_BUSOFF = 0x00010
PCAN_ERROR_QRCVEMPTY = 0x00020
PCAN_ERROR_QOVERRUN = 0x00040
PCAN_ERROR_QXMTFULL = 0x00080
PCAN_ERROR_REGTEST = 0x00100
PCAN_ERROR_NODRIVER = 0x00200
PCAN_ERROR_HWINUSE = 0x00400
PCAN_ERROR_ILLHW = 0x01400

# 总线错误族（任何一位被置上都说明物理层有问题）
PCAN_ERROR_ANYBUSERR = (PCAN_ERROR_BUSLIGHT | PCAN_ERROR_BUSHEAVY
                        | PCAN_ERROR_BUSOFF)


class TPCANMsg(ctypes.Structure):
    _fields_ = [("ID", ctypes.c_uint32),
                ("MSGTYPE", ctypes.c_ubyte),
                ("LEN", ctypes.c_ubyte),
                ("DATA", ctypes.c_ubyte * 8)]


class TPCANTimestamp(ctypes.Structure):
    _fields_ = [("millis", ctypes.c_uint32),
                ("millis_overflow", ctypes.c_uint16),
                ("micros", ctypes.c_uint16)]


def find_dll():
    """按优先级找 PCANBasic.dll；找不到给出可操作的提示"""
    cands = []
    env = os.environ.get("PCANBASIC_DLL")
    if env:
        cands.append(env)
    here = os.path.dirname(os.path.abspath(__file__))
    cands.append(os.path.join(here, "PCANBasic.dll"))
    windir = os.environ.get("SystemRoot", r"C:\Windows")
    cands.append(os.path.join(windir, "System32", "PCANBasic.dll"))
    cands.append(os.path.join(windir, "SysWOW64", "PCANBasic.dll"))
    for c in cands:
        if c and os.path.isfile(c):
            return c
    found = ctypes.util.find_library("PCANBasic")
    return found


class Pcan:
    """PCAN-Basic 的极薄封装（只用到本工程需要的几个调用）"""

    def __init__(self, channel=PCAN_USBBUS1):
        path = find_dll()
        if not path:
            raise RuntimeError(
                "找不到 PCANBasic.dll —— PCAN-Basic 驱动/API 没装。\n"
                "  官方指引（PCAN-Basic 包里的 ReadMe.txt 第 114-125 行）：\n"
                "   1) **Device Driver Setup（推荐）**：一个安装程序同时装\n"
                "      设备驱动 + PCANBasic.dll（进 System32）+ PCAN-View：\n"
                "         https://www.peak-system.com/quick/DrvSetup\n"
                "      装完插上 PCAN-USB，设备管理器里应出现\n"
                "      'PEAK-System Technik -> PCAN-USB'\n"
                "   2) 只手工装 DLL（**不解决驱动**，硬件仍认不出来）：\n"
                "      x64\\PCANBasic.dll -> C:\\Windows\\System32（需管理员）\n"
                "      或像现在这样临时指定给本脚本：\n"
                "         set PCANBASIC_DLL=D:\\can\\x64\\PCANBasic.dll\n"
                "  ⚠️ API 包（含 Include/x64/x86/Samples 的那个 ZIP）**不含**\n"
                "     .inf/.sys，所以设备驱动必须单独装。")
        self.dll = ctypes.WinDLL(path)
        self.channel = channel
        self.opened = False

        d = self.dll
        d.CAN_Initialize.argtypes = [ctypes.c_uint16, ctypes.c_uint16,
                                     ctypes.c_uint32, ctypes.c_uint32,
                                     ctypes.c_uint32]
        d.CAN_Initialize.restype = ctypes.c_uint32
        d.CAN_Uninitialize.argtypes = [ctypes.c_uint16]
        d.CAN_Uninitialize.restype = ctypes.c_uint32
        d.CAN_Write.argtypes = [ctypes.c_uint16, ctypes.POINTER(TPCANMsg)]
        d.CAN_Write.restype = ctypes.c_uint32
        d.CAN_Read.argtypes = [ctypes.c_uint16, ctypes.POINTER(TPCANMsg),
                               ctypes.POINTER(TPCANTimestamp)]
        d.CAN_Read.restype = ctypes.c_uint32
        d.CAN_GetStatus.argtypes = [ctypes.c_uint16]
        d.CAN_GetStatus.restype = ctypes.c_uint32
        d.CAN_GetErrorText.argtypes = [ctypes.c_uint32, ctypes.c_uint16,
                                       ctypes.c_char_p]
        d.CAN_GetErrorText.restype = ctypes.c_uint32

    def error_text(self, status):
        buf = ctypes.create_string_buffer(256)
        lang = 0x09            # PCAN_LANGUAGENEUTRAL 附近值；失败也不影响
        try:
            self.dll.CAN_GetErrorText(ctypes.c_uint32(status),
                                      ctypes.c_uint16(lang), buf)
            txt = buf.value.decode("utf-8", "replace")
        except Exception:
            txt = ""
        if not txt:
            txt = "0x%05X" % status
        return txt.strip()

    def open(self, baud=PCAN_BAUD_500K):
        st = self.dll.CAN_Initialize(self.channel, baud, 0, 0, 0)
        self.opened = (st == PCAN_ERROR_OK)
        return st

    def close(self):
        if self.opened:
            self.dll.CAN_Uninitialize(self.channel)
            self.opened = False

    def status(self):
        return self.dll.CAN_GetStatus(self.channel)

    def send(self, can_id, data, extended=False):
        m = TPCANMsg()
        m.ID = can_id
        m.MSGTYPE = PCAN_MESSAGE_EXTENDED if extended else PCAN_MESSAGE_STANDARD
        m.LEN = len(data)
        for i, b in enumerate(data[:8]):
            m.DATA[i] = b
        return self.dll.CAN_Write(self.channel, ctypes.byref(m))

    def recv(self):
        """取一帧；没有就返回 (None, 0)"""
        m = TPCANMsg()
        ts = TPCANTimestamp()
        st = self.dll.CAN_Read(self.channel, ctypes.byref(m), ctypes.byref(ts))
        if st != PCAN_ERROR_OK:
            return None, st
        data = bytes(m.DATA[i] for i in range(m.LEN))
        t = ts.millis + ts.micros / 1000.0
        return (m.ID, m.MSGTYPE, data, t), PCAN_ERROR_OK


def describe_bus_status(st):
    """把状态码翻译成"人话 + 该查什么" """
    if st == PCAN_ERROR_OK:
        return "正常（总线空闲、无错误）", True
    if st & PCAN_ERROR_BUSOFF:
        return ("Bus-Off（控制器已脱离总线）—— 典型原因是**线上没有第二个节点回 ACK**，"
                "或 CAN_H/CAN_L 接反/断路、收发器没供电", False)
    if st & PCAN_ERROR_BUSHEAVY:
        return ("错误被动（发送错误计数 >= 128）—— 大量帧没被 ACK，"
                "查接线、终端电阻、波特率是否一致", False)
    if st & PCAN_ERROR_BUSLIGHT:
        return ("错误警告（发送错误计数 >= 96）—— 偶发无 ACK / 干扰，"
                "查 120Ω 终端与共地", False)
    if st & PCAN_ERROR_ILLHW:
        return "设备不存在或未被驱动占用（PCAN 没插好 / 驱动没起来）", False
    if st & PCAN_ERROR_NODRIVER:
        return "驱动未加载（PCAN-Basic 没装好，或需要管理员权限）", False
    return ("状态码 0x%05X" % st), False


def main():
    ap = argparse.ArgumentParser(
        description="PCAN（PEAK）USB-CAN 发帧/收帧/看状态（不依赖 python-can）",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--channel", default="usb1",
                    choices=["usb1", "usb2"], help="PCAN 通道（默认 usb1）")
    ap.add_argument("--bitrate", default="500k",
                    choices=["500k", "250k", "1m"], help="波特率（默认 500k）")
    ap.add_argument("--status", action="store_true", help="只打印驱动/设备/总线状态")
    ap.add_argument("--send", action="store_true", help="发送模式")
    ap.add_argument("--monitor", type=float, default=0.0,
                    metavar="SEC", help="监听 SEC 秒并打印收到的帧")
    ap.add_argument("--id", default="0x123", help="帧 ID（支持 0x 前缀）")
    ap.add_argument("--data", nargs="*", default=["11", "22", "33", "44"],
                    help="数据字节（十六进制，最多 8 个）")
    ap.add_argument("--count", type=int, default=1, help="发送帧数")
    ap.add_argument("--period", type=float, default=100.0,
                    metavar="MS", help="发送间隔（毫秒）")
    ap.add_argument("--extended", action="store_true", help="用扩展帧（29 位 ID）")
    args = ap.parse_args()

    ch = PCAN_USBBUS1 if args.channel == "usb1" else PCAN_USBBUS2
    baud = {"500k": PCAN_BAUD_500K, "250k": PCAN_BAUD_250K,
            "1m": PCAN_BAUD_1M}[args.bitrate]

    print("=" * 70)
    print("  PCAN USB-CAN 工具   通道=%s  波特率=%s" % (args.channel, args.bitrate))
    print("=" * 70)

    try:
        bus = Pcan(ch)
    except Exception as e:
        print("[FAIL] %s" % e)
        return 2

    print("[ OK ] PCANBasic.dll: %s" % find_dll())

    st = bus.open(baud)
    if st != PCAN_ERROR_OK:
        print("[FAIL] CAN_Initialize 失败: %s" % bus.error_text(st))
        desc, _ = describe_bus_status(st)
        print("       %s" % desc)
        print("       排查：1) PCAN 插好了吗 2) PCAN-Basic 装了吗（设备管理器应看到 PCAN-USB）")
        print("             3) 通道号对不对（--channel usb1/usb2）")
        return 2

    print("[ OK ] CAN_Initialize 成功（%s）" % args.bitrate)
    st = bus.status()
    desc, ok = describe_bus_status(st)
    print("[%s] 总线状态: %s" % (" OK " if ok else "WARN", desc))
    # ⚠️⚠️ 2026-10-11 加这段警告的由来（别再被它骗一次）：
    #    我在排查"PCAN 发 -> 板子收"时，把上面这行"正常"当成了
    #    "总线上有节点在 ACK，所以链路是通的"，据此得出**完全相反**的结论。
    #    后来做了对照实验：**故意用 250k 发 30 帧**（板子是 500k，物理上
    #    不可能有节点 ACK）—— 它**照样报"正常（总线空闲、无错误）"**。
    #    原因：① PCAN-Basic 没有读 TEC/REC 的 API（PCANBasic.dll 里
    #    CAN_GetErrorCounters **不存在**）；② CAN_Write 返回的"成功"只表示
    #    **已排队**；③ 这行状态是 PCAN 自己看到的，证明不了对面在收。
    #    所以：**判断"链路通不通"只能看对面**（本项目看板子的收帧计数，
    #    而且要用长窗口 —— 板子每 5 秒的回环自检也会 +1）。
    print("       ⚠️ 这行只反映 PCAN 自己看到的总线状态，**不能**证明对面在收发：")
    print("          · 写入成功 = 已排队，不代表被 ACK（无 ACK 时它仍可能报“正常”）")
    print("          · 本工具读不到 TEC/REC（PCANBasic.dll 没有那个 API）")
    print("          · 判链路要**看对面的计数**：verify_can_dual.py --via swd")

    rc = 0

    if args.send:
        try:
            can_id = int(args.id, 16) if args.id.lower().startswith("0x") else int(args.id, 16)
        except ValueError:
            print("[FAIL] 帧 ID 不是合法十六进制: %s" % args.id)
            bus.close()
            return 2

        data = []
        for b in args.data:
            try:
                data.append(int(b, 16) & 0xFF)
            except ValueError:
                print("[FAIL] 数据字节不是合法十六进制: %s" % b)
                bus.close()
                return 2
        if not data:
            data = [0x11, 0x22, 0x33, 0x44]
        if len(data) > 8:
            print("[FAIL] 数据超过 8 字节")
            bus.close()
            return 2

        print("\n---- 发送 %d 帧：ID=0x%03X DLC=%d 数据=%s 间隔=%.0fms ----"
              % (args.count, can_id, len(data),
                 " ".join("%02X" % b for b in data), args.period))

        ok_n = 0
        fail_st = None
        for i in range(args.count):
            st = bus.send(can_id, data, args.extended)
            if st == PCAN_ERROR_OK:
                ok_n += 1
                if i < 5 or (i + 1) == args.count:
                    print("  [%3d] 写入成功（已排队）" % (i + 1))
            else:
                fail_st = st
                print("  [%3d] 写入失败: %s" % (i + 1, bus.error_text(st)))
                rc = 1
                break
            if args.period > 0 and (i + 1) < args.count:
                time.sleep(args.period / 1000.0)

        # ⚠️ 关键一步：CAN_Write 只表示"排进发送队列"，
        #    能不能真的发出去要看 ACK。发完再读状态才是判据。
        time.sleep(0.3)
        st = bus.status()
        desc, bus_ok = describe_bus_status(st)
        print("\n  写入队列: %d/%d" % (ok_n, args.count))

        # ⚠️ "队列满"是**没有 ACK 的间接铁证**：
        #    发送队列只有在硬件一直完不成发送时才会被塞满
        #    （帧在等 ACK / 反复重发）。实测 200000 帧跑到 36839 就报这个。
        if fail_st is not None:
            print("  最后一次写入失败原因: %s" % bus.error_text(fail_st))
            if fail_st & (PCAN_ERROR_QXMTFULL | PCAN_ERROR_XMTFULL):
                print("  ⚠️ 发送队列被塞满 —— 说明前面的帧**一直没被 ACK**：")
                print("     总线上没有第二个节点在应答（接线/收发器/波特率之一有问题）。")
                print("     注意这**不代表**适配器坏了：它只是没人应答。")

        print("  [%s] 发送后总线状态: %s" % (" OK " if bus_ok else "FAIL", desc))
        if not bus_ok:
            rc = 1
            print("\n  ⚠️ 如果 STM32 侧收帧计数**没涨**，与这里的状态是一致的：")
            print("     说明这一帧根本没被 ACK —— 查 CAN_H/CAN_L 是否接对（不能反）、")
            print("     两边是否共地、STM32 侧收发器是否供电、两端 120Ω。")

    if args.monitor > 0.0:
        print("\n---- 监听 %.1f 秒（收到的帧会打印出来）----" % args.monitor)
        t_end = time.time() + args.monitor
        n = 0
        while time.time() < t_end:
            frame, st = bus.recv()
            if frame is None:
                if st not in (PCAN_ERROR_QRCVEMPTY, PCAN_ERROR_OK):
                    print("  [WARN] 读失败: %s" % bus.error_text(st))
                time.sleep(0.005)
                continue
            fid, mtype, data, t = frame
            n += 1
            kind = "EXT" if (mtype & PCAN_MESSAGE_EXTENDED) else "STD"
            print("  [%8.3f] %s 0x%03X DLC=%d  %s"
                  % (time.time() % 1000.0, kind, fid, len(data),
                     " ".join("%02X" % b for b in data)))
        print("  共收到 %d 帧" % n)
        if n == 0:
            print("  ⚠️ 一帧都没收到。注意：本项目固件 CAN 是**只收不发**，")
            print("     所以这里收到 0 帧是预期的 —— 验证方向应该是"
                  "'PCAN 发 -> 板子收'（用 --send + 看板子的收帧计数）。")

    bus.close()
    print("\n[完成]")
    return rc


if __name__ == "__main__":
    sys.exit(main())
