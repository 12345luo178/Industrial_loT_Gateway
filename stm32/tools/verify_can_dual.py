#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CAN 端到端验证：PCAN 发帧 -> STM32 收帧 -> 计数出现在 Modbus 输入寄存器里

============================================================================
 它在验什么（这条链路一次性穿过了所有环节）

    [PCAN 适配器] --CAN_H/L--> [TJA1051 收发器] --> [bxCAN 控制器]
        --> [gw_can_core 收帧统计] --> [应用层发布到寄存器]
        --> [Modbus RTU 从站] --RS485--> [本脚本读回来]

    只要输入寄存器 17/18（CAN 收帧计数，32 位）涨了，就说明这一整条链路都通。
    而且因为 STM32 收到帧时必须回 ACK，而 ACK 是它用**自己的收发器**发出去的，
    所以"PCAN 发送无 ACK 错误" 同时证明了 STM32 收发器的收、发两条通路。

============================================================================
 为什么必须有两个节点

  CAN 的发送方必须收到 ACK，单节点永远发不出去（会一直重发直到 Bus-Off）。
  所以"用 PCAN 当第二个节点"不是可选项，是前提。

============================================================================
 用法

    python tools/verify_can_dual.py --rs485 COM4 --slave 1
        默认：先读基线 -> PCAN 发 5 帧 ID=0x123 -> 再读 -> 判定

    python tools/verify_can_dual.py --rs485 COM4 --slave 1 --id 0x321 --count 20
    python tools/verify_can_dual.py --rs485 COM4 --slave 1 --read-only
        只读板子侧（你想用 PCAN-View 手工发帧时用这个）

 ⚠️ 需要：PEAK PCAN-Basic（PCANBasic.dll）+ 板子上的固件已启用 CAN。
    CAN 是"只收不发"，所以本脚本只验 PCAN->STM32 方向。
"""

import argparse
import glob
import os
import subprocess
import sys
import time

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(errors="replace")
    except Exception:
        pass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from modbus_master_serial import Master, crc_ok          # noqa: E402
from can_pcan_test import (Pcan, PCAN_USBBUS1, PCAN_USBBUS2,   # noqa: E402
                           PCAN_BAUD_1M, PCAN_BAUD_250K, PCAN_BAUD_500K,
                           PCAN_ERROR_OK, describe_bus_status)

# ---- 寄存器索引（与 inc/modbus_regs.h 一一对应）----
IREG_CAN_CH1 = 2          # 通道值 CH1..CH8 = 2..9
IREG_CAN_CH_COUNT = 8
IREG_DEV_STATUS = 10
IREG_DATA_UPTIME = 12     # 运行秒数（用来判断"这张表是不是活的"）
IREG_CAN_RX_LO = 17
IREG_CAN_RX_HI = 18

STATUS_BIT_DHT_OK = 1 << 0
STATUS_BIT_CAN_OK = 1 << 1
STATUS_BIT_MASTER_OK = 1 << 8


# ==================================================================
#  SWD 读寄存器表（不依赖 RS485 —— 调试时 RS485 常常正好是坏的）
#
#  ⚠️ 踩过的坑：ELF 里可能有**多个同名 static 符号**（本工程的 `g_regs`
#     就有两份：模板一份、另一个 TU 一份）。按名字取第一个很可能取到
#     **没在用的那份**（永远全 0），于是误判成"板子卡死了"。
#     这里把所有同名符号都找出来，再用"运行秒数 > 0"挑出真正在用的那张表。
# ==================================================================

def _find_tool(env_name, patterns, exe):
    v = os.environ.get(env_name)
    if v and os.path.isfile(v):
        return v
    for pat in patterns:
        for m in glob.glob(pat):
            if os.path.isfile(m):
                return m
    # 最后兜底：PATH 里找
    from shutil import which
    return which(exe)


CLI = _find_tool(
    "GW_STLINK_CLI",
    [r"D:\stm32cubeIDE\*\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools"
     r".cubeprogrammer*\tools\bin\STM32_Programmer_CLI.exe",
     r"C:\ST\*\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools"
     r".cubeprogrammer*\tools\bin\STM32_Programmer_CLI.exe"],
    "STM32_Programmer_CLI.exe")

NM = _find_tool(
    "GW_ARM_NM",
    [r"D:\stm32cubeIDE\*\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools"
     r".gnu-tools-for-stm32*\tools\bin\arm-none-eabi-nm.exe",
     r"C:\ST\*\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools"
     r".gnu-tools-for-stm32*\tools\bin\arm-none-eabi-nm.exe"],
    "arm-none-eabi-nm.exe")

# ⚠️⚠️ 2026-10-11：默认 ELF 必须优先指向**槽位化镜像**，不能是 CubeMX IDE 那份。
#
#   槽位化之后板上跑的是 `build/ota/app_a.elf`（我们的 .ld + VECT_TAB 偏移），
#   而 `D:\stm32cubeIDE\gateway2\Debug\gateway2.elf` 是**另一个二进制**：
#   BSS 布局不同 -> `g_regs` 的符号地址不同 -> 这个工具**静默失效**
#   （报"所有 g_regs 候选地址都读不出有效数据"），
#   而那个现象看起来非常像"板子卡死了" —— 今天就是这么被骗了一次。
#
#   ⚠️ 同理：**每次重新编译后符号地址都会变**，所以这里每次都从 ELF 现取，
#      并且把"用的是哪份 ELF"打出来（免得又对着错的文件排半天）。
IDE_ELF = r"D:\stm32cubeIDE\gateway2\Debug\gateway2.elf"


def _pick_default_elf():
    here = os.path.dirname(os.path.abspath(__file__))
    ota = os.path.join(os.path.dirname(here), "build", "ota", "app_a.elf")
    if os.path.isfile(ota):
        return ota
    return IDE_ELF


DEFAULT_ELF = _pick_default_elf()


def nm_g_regs(elf):
    """找出 ELF 里所有名为 g_regs 的符号地址（可能不止一个）"""
    out = subprocess.run([NM, "-S", elf], capture_output=True, text=True)
    addrs = []
    for line in out.stdout.splitlines():
        p = line.split()
        if len(p) >= 4 and p[3] == "g_regs":
            try:
                addrs.append(int(p[0], 16))
            except ValueError:
                pass
    return addrs


def nm_symbol(elf, name):
    """从 ELF 里取某个符号的地址（取不到返回 None）

    ⚠️ 为什么要有通用查询：`g_regs` 早就会从 ELF 现取，但 `uwTick` 的地址
       一直**写死在工具里** —— 而 BSS 布局一变它就不一样了（实测：L0 版
       `0x2000026C`，加了 FreeRTOS 的版本 `0x20000274`）。写死的后果是
       **静默读错内存**：CSV 里那一列全是垃圾，却不报错。
    """
    try:
        out = subprocess.run([NM, "-S", elf], capture_output=True, text=True)
    except Exception:                                   # noqa: BLE001
        return None
    for line in out.stdout.splitlines():
        p = line.split()
        if len(p) >= 4 and p[3] == name:
            try:
                return int(p[0], 16)
            except ValueError:
                pass
    return None


def swd_read(addr, nbytes):
    """用 ST-LINK 读内存（hotplug 模式，不占串口）"""
    out = subprocess.run([CLI, "-c", "port=SWD", "mode=hotplug",
                          "-r32", "0x%08X" % addr, "0x%X" % nbytes],
                         capture_output=True, text=True, errors="replace")
    words = []
    for line in out.stdout.splitlines():
        line = line.strip()
        if line.startswith("0x") and ":" in line:
            for w in line.split(":", 1)[1].split():
                try:
                    words.append(int(w, 16))
                except ValueError:
                    pass
    return words


# ============================================================================
#  ★ 镜像一致性检查（2026-10-11 加；从 rtos_state.py 移到这里做**公共实现**）
#
#  ⚠️⚠️ 为什么所有"按地址读活目标"的工具都必须先做这一步：
#     这些工具是**按 ELF 里的地址**去读板子的。如果板子上跑的不是这份 ELF，
#     读回来的就是**别的变量**，而数值完全可能看着很正常
#     -> 给出**假的验收结论**（比"没测"更坏）。
#
#     ⚠️ 而这件事在落地当天**大概率发生**：默认 ELF 都是浸泡基线那份
#        `build/ota/app_a.elf`，落地后板上是新镜像 —— 忘了换 ELF 就会中招。
#
#  做法：取 ELF 里 `main` 的前 16 字节（按 PT_LOAD 映射从 ELF 文件取），
#        与板子 Flash 同地址的实际内容逐字节比对；不一致 -> 调用方应**拒绝出读数**。
# ============================================================================
def nm_main_addr(elf):
    """取 ELF 里 main 的地址（取不到返回 None）"""
    try:
        out = subprocess.run([NM, "-S", elf], capture_output=True, text=True,
                             errors="replace")
    except OSError:
        return None
    for line in out.stdout.splitlines():
        p = line.split()
        if len(p) >= 4 and p[3] == "main":
            try:
                return int(p[0], 16)
            except ValueError:
                return None
    return None


def elf_bytes_at(elf, vaddr, n):
    """从 ELF 文件里按 PT_LOAD 映射取 vaddr 处的 n 个字节"""
    import struct
    try:
        data = open(elf, "rb").read()
    except OSError:
        return None
    if len(data) < 52 or data[:4] != b"\x7fELF":
        return None
    phoff = struct.unpack_from("<I", data, 28)[0]
    phentsize = struct.unpack_from("<H", data, 42)[0]
    phnum = struct.unpack_from("<H", data, 44)[0]
    for i in range(phnum):
        off = phoff + i * phentsize
        if off + 32 > len(data):
            break
        p_type, p_offset, p_vaddr, _pa, p_filesz = struct.unpack_from(
            "<IIIII", data, off)
        if p_type != 1:                     # PT_LOAD
            continue
        if p_vaddr <= vaddr and vaddr + n <= p_vaddr + p_filesz:
            fo = p_offset + (vaddr - p_vaddr)
            return list(data[fo:fo + n])
    return None


def image_matches(elf_part, tgt_part):
    """★ 纯逻辑：ELF 里的字节与板子上读回的字节是否一致（None = 读不到）"""
    if not elf_part or not tgt_part:
        return None
    if len(elf_part) != len(tgt_part):
        return False
    return all(int(a) & 0xFF == int(b) & 0xFF for a, b in zip(elf_part, tgt_part))


def check_image(elf):
    """返回 (verdict, detail)：True=一致 / False=不一致 / None=无法判定"""
    addr = nm_main_addr(elf)
    if addr is None:
        return None, "ELF 里找不到 main 符号"
    want = elf_bytes_at(elf, addr, 16)
    if want is None:
        return None, "从 ELF 文件里取不出 main@0x%08X 的字节" % addr
    w = swd_read(addr, 16)
    if not w:
        return None, "SWD 读不到 0x%08X" % addr
    got = []
    for x in w:
        got += [x & 0xFF, (x >> 8) & 0xFF, (x >> 16) & 0xFF, (x >> 24) & 0xFF]
    r = image_matches(want, got[:len(want)])
    if r is None:
        return None, "无法比对"
    detail = ("main@0x%08X  ELF=%s  板上=%s"
              % (addr, " ".join("%02X" % b for b in want[:8]),
                 " ".join("%02X" % b for b in got[:8])))
    return r, detail


def guard_image(elf, allow_unknown=True):
    """给工具用的统一入口：打印结论，返回 False 表示**应当中止**。

    allow_unknown=True 时"无法判定"只提示不中止（读不到符号等不是致命问题）。
    """
    ok, detail = check_image(elf)
    if ok is False:
        print("  [中止] **板子上跑的不是这份 ELF** —— 读数不可信")
        print("         %s" % detail)
        print("         多半是 --elf 指的旧镜像（默认就是浸泡基线那份）。"
              "请用**刚烧进去的那份**重跑。")
        return False
    if ok is None:
        print("  [注意] 镜像一致性无法核对（%s）—— 继续，但读数仅供参考" % detail)
        return bool(allow_unknown)
    print("  [ OK ] 镜像一致：%s" % detail)
    return True


def words_to_regs(words):
    """32 位字 -> 16 位寄存器数组（小端：低半字在前）"""
    regs = []
    for w in words:
        regs.append(w & 0xFFFF)
        regs.append((w >> 16) & 0xFFFF)
    return regs


# ==================================================================
#  CAN1 错误状态寄存器（F103 上地址固定）
#
#  ⚠️ 为什么一定要读它：只报"收帧计数没涨"是**无法定位**的 ——
#     同样是"没涨"，下面两种情况的处理方向完全相反：
#        REC/LEC 有值 -> 信号到达了收发器但解不出来 -> 物理层极性/接线/终端
#        TEC/REC/LEC 全 0 -> 信号压根没到收发器 -> 供电/RXD 接线/没接到收发器
#     实测（2026-10-10）就是靠这个分出来的：PCAN 发 6000 帧时
#     STM32 的 REC 打满 255、LEC=3（ACK 错误）—— 而内部回环完全正常，
#     于是问题被锁定在"总线上有电平但解不出来"。
# ==================================================================
CAN1_ESR_ADDR = 0x40006418

_LEC_TEXT = {
    0: "无错误",
    1: "位填充错误",
    2: "格式错误",
    3: "ACK 错误",
    4: "位隐性错误",
    5: "位显性错误",
    6: "CRC 错误",
    7: "软件设置",
}


def decode_esr(esr):
    """把 CAN_ESR 拆成人话"""
    if esr is None:
        return None
    return {
        "raw": esr,
        "lec": esr & 0x7,
        "lec_text": _LEC_TEXT.get(esr & 0x7, "?"),
        "txbo": (esr >> 3) & 1,
        "txep": (esr >> 4) & 1,
        "rxep": (esr >> 5) & 1,
        "txw": (esr >> 6) & 1,
        "rxw": (esr >> 7) & 1,
        "tec": (esr >> 16) & 0xFF,
        "rec": (esr >> 24) & 0xFF,
    }


def swd_snapshot(elf, addr=None):
    """通过 SWD 取一次快照；addr 为空时自动挑"活着"的那张寄存器表"""
    cands = [addr] if addr else nm_g_regs(elf)
    if not cands:
        return None, "ELF 里找不到 g_regs（--elf 指对了吗？）"

    for a in cands:
        regs = words_to_regs(swd_read(a, 40))       # input[0..19]
        if len(regs) < 19:
            continue
        # input[12] = 运行秒数：>0 说明应用层在刷新这张表
        if regs[IREG_DATA_UPTIME] > 0 or len(cands) == 1:
            esr_words = swd_read(CAN1_ESR_ADDR, 4)
            return {
                "addr": a,
                "ch": regs[IREG_CAN_CH1:IREG_CAN_CH1 + IREG_CAN_CH_COUNT],
                "status": regs[IREG_DEV_STATUS],
                "rx32": regs[IREG_CAN_RX_LO] | (regs[IREG_CAN_RX_HI] << 16),
                "uptime": regs[IREG_DATA_UPTIME],
                "dht": (regs[0], regs[1]),
                "esr": decode_esr(esr_words[0] if esr_words else None),
            }, None
    return None, ("所有 g_regs 候选地址都读不出有效数据"
                  "（用的是 %s）——⚠️ 若刚重新编译过，符号地址会变；"
                  "槽位化后请确认 --elf 指向 build/ota/app_a.elf" % elf)


def read_inputs(m, addr, start, qty, tries=4):
    """读输入寄存器，返回整数列表；失败返回 None

    ⚠️ 必须重试（2026-10-10 血的教训）：
       这块板子的**主站每 ~0.8 秒就在同一条总线上发一次轮询**，
       单次请求有很大概率与它撞车 —— 撞了就是"无应答/CRC 错"。
       我第一版只发一次，于是把"能正常应答的从站"误判成"RS485 不通"，
       白白查了半天接线。凡是在"总线上已经有主站"的场景里做探测，
       都要重试，并把"偶尔成功"当成"通路正常"。
    """
    for _ in range(max(1, tries)):
        pdu = bytes([addr, 4]) + start.to_bytes(2, "big") \
              + qty.to_bytes(2, "big")
        _req, resp = m.request(pdu)
        if len(resp) >= 5 and crc_ok(resp):
            if resp[0] == addr and resp[1] == 4:
                bc = resp[2]
                if bc == qty * 2 and len(resp) >= 3 + bc + 2:
                    return [int.from_bytes(resp[3 + i * 2:5 + i * 2], "big")
                            for i in range(bc // 2)]
        time.sleep(0.15)
    return None


def detect_slave(m, preferred):
    """自动识别板子的从站地址：先试给定值，再试 1/2 里另一个

    ⚠️ 默认值曾经写错（写成 2），结果一路报"读不到寄存器"。
    自动识别一次，比让人记住"本机从站 1、被轮询的模拟从机 2"更可靠。
    """
    for a in ([preferred] + [x for x in (1, 2) if x != preferred]):
        if read_inputs(m, a, IREG_DEV_STATUS, 1, tries=3) is not None:
            return a
    return None


class BoardSide:
    """读板子侧数据的两种通道：Modbus(RS485) 或 SWD

    ⚠️ 为什么 SWD 这条通道值得有：调试 CAN 的时候，RS485 常常正好也是坏的
       （动线、共地、A/B 接反都可能导致），而 CAN 验证不该被它卡住。
       SWD 只占调试口，不碰 RS485。
    """

    def __init__(self, via, rs485, baud, slave, elf, regs_addr):
        self.via = via
        self.slave = slave
        self.elf = elf
        self.regs_addr = regs_addr
        self.m = None
        if via == "modbus":
            self.m = Master(rs485, baud, timeout=0.6)
            # 自动识别从站地址（默认值写错过一次，见 detect_slave 的注释）
            real = detect_slave(self.m, slave)
            if real is not None:
                self.slave = real

    def close(self):
        if self.m is not None:
            self.m.close()

    def snapshot(self):
        if self.via == "swd":
            snap, err = swd_snapshot(self.elf, self.regs_addr)
            if snap is None:
                return None, err
            return snap, None
        low = read_inputs(self.m, self.slave, IREG_CAN_CH1, IREG_CAN_CH_COUNT)
        st = read_inputs(self.m, self.slave, IREG_DEV_STATUS, 1)
        rx = read_inputs(self.m, self.slave, IREG_CAN_RX_LO, 2)
        if low is None or st is None or rx is None:
            return None, "读不到输入寄存器（地址 %d）" % self.slave
        return {
            "ch": low,
            "status": st[0],
            "rx32": rx[0] | (rx[1] << 16),
            "uptime": 0,
            "dht": (0, 0),
            "addr": 0,
            "esr": None,        # Modbus 读不到 MCU 内部寄存器
        }, None


def main():
    ap = argparse.ArgumentParser(
        description="CAN 端到端验证：PCAN 发帧 -> STM32 收帧 -> 计数可见",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--via", default="modbus", choices=["modbus", "swd"],
                    help="从哪里读板子的收帧计数（默认 modbus；RS485 不通时用 swd）")
    ap.add_argument("--rs485", default="COM4", help="RS485 调试口（默认 COM4）")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--slave", type=int, default=1, help="板子的 Modbus 从站地址（本机从站=1，被轮询的模拟从机=2；写错会自动识别）")
    ap.add_argument("--elf", default=DEFAULT_ELF, help="SWD 模式用的 ELF（找符号地址）")
    ap.add_argument("--regs-addr", default=None,
                    help="直接指定 g_regs 地址（0x 开头），跳过自动寻找")
    ap.add_argument("--channel", default="usb1", choices=["usb1", "usb2"])
    ap.add_argument("--bitrate", default="500k", choices=["500k", "250k", "1m"])
    ap.add_argument("--id", default="0x123", help="测试帧 ID")
    ap.add_argument("--data", nargs="*", default=["11", "22", "33", "44"])
    ap.add_argument("--count", type=int, default=5)
    ap.add_argument("--baseline", type=float, default=11.0,
                    help="发帧前静默量回环自检基线的秒数（默认 11 秒，覆盖 ≥2 个回环周期；"
                         "0 = 不量，退回标称的 1 帧/5 秒）")
    ap.add_argument("--extended", action="store_true")
    ap.add_argument("--read-only", action="store_true",
                    help="只读板子侧（用 PCAN-View 手工发帧时用）")
    ap.add_argument("--image-only", action="store_true",
                    help="只核对「板子上跑的镜像 == 这份 ELF」，不做任何收发")
    args = ap.parse_args()

    print("=" * 72)
    print("  CAN 端到端验证（PCAN 发帧 -> STM32 收帧 -> 计数可见）")
    print("=" * 72)

    # ---------------- 0) 先核对镜像一致性（按地址读活目标的前提）----------------
    if args.via == "swd" or args.image_only:
        if not guard_image(args.elf):
            return 2
        if args.image_only:
            return 0

    # ---------------- 1) 先读板子侧基线 ----------------
    if args.via == "swd":
        if not CLI or not NM:
            print("[FAIL] 找不到 STM32_Programmer_CLI / arm-none-eabi-nm")
            print("       可以用环境变量指定：GW_STLINK_CLI / GW_ARM_NM")
            return 2

    try:
        board = BoardSide(args.via, args.rs485, args.baud, args.slave,
                          args.elf, args.regs_addr)
    except Exception as e:
        print("[FAIL] 建立读通道失败（%s）: %s" % (args.via, e))
        if args.via == "modbus":
            print("       用 python tools/modbus_master_serial.py --list 看可用串口")
            print("       ⚠️ RS485 不通时可直接用 --via swd 绕过它")
        return 2

    base, err = board.snapshot()
    if base is None:
        board.close()
        print("[FAIL] 读不到板子的寄存器（通道 %s）: %s" % (args.via, err))
        if args.via == "modbus":
            print("       先确认板子在跑、RS485 接线与地址正确：")
            print("       python tools/modbus_master_serial.py --port %s --probe"
                  % args.rs485)
            print("       或改用 SWD：--via swd")
        return 2

    print("[ OK ] 板子在线（通道 %s%s）：CAN 收帧计数 = %d"
          % (args.via,
             ("，寄存器表 @0x%08X" % base["addr"]) if args.via == "swd" else "",
             base["rx32"]))
    print("       DEV_STATUS = 0x%04X  (DHT_OK=%d CAN_OK=%d MASTER_OK=%d)"
          % (base["status"],
             (base["status"] & STATUS_BIT_DHT_OK) != 0,
             (base["status"] & STATUS_BIT_CAN_OK) != 0,
             (base["status"] & STATUS_BIT_MASTER_OK) != 0))
    if (base["status"] & STATUS_BIT_CAN_OK) == 0:
        print("  [WARN] DEV_STATUS 里 CAN_OK = 0 —— 控制器可能不在正常态"
              "（自检的 CAN 项也会因此失败）")

    if args.read_only:
        board.close()
        print("\n[只读模式] 现在可以用 PCAN-View 或 can_pcan_test.py 发帧，"
              "再重新跑本脚本对比计数。")
        return 0

    # ---------------- 1.5) 先静默量一段"回环自检基线" ----------------
    #
    #  ⚠️⚠️ 为什么必须先量基线（2026-10-11 加，我自己被这件事误导过一轮）：
    #
    #    L0 层每 5 秒做一次 CAN 回环自检，而它**也会给收帧计数 +1**。
    #    所以"发 N 帧、增量 M"这句话里的 M **不全是外部帧**：
    #      发 40 帧、增量 1 —— 看起来"收到 1 帧"，其实那 1 帧就是回环，
    #                         外部帧**一帧都没收到**（当初就是这么被骗的）。
    #    先在不发帧的情况下量一段时间（默认 11 秒，覆盖 ≥2 个回环周期），
    #    得到"回环率"，再按发帧窗口扣掉 —— 剩下的才是外部帧的净数量。
    loop_rate = None                  # 帧/秒（回环自检贡献）
    if args.baseline > 0:
        b1 = base
        time.sleep(args.baseline)
        b2, _e = board.snapshot()
        if b2 is not None:
            loop_rate = (b2["rx32"] - b1["rx32"]) / args.baseline
            print("  静默基线 %.0f 秒：收帧计数 %d -> %d，回环率 ≈ %.2f 帧/秒"
                  % (args.baseline, b1["rx32"], b2["rx32"], loop_rate))
            base = b2               # 以基线结束点为发帧的参照
            print("[ OK ] 板子在线（通道 %s%s）：CAN 收帧计数 = %d"
                  % (args.via,
                     ("，寄存器表 @0x%08X" % base["addr"]) if args.via == "swd" else "",
                     base["rx32"]))
    t_ref = time.time()

    # ---------------- 2) PCAN 发帧 ----------------
    ch = PCAN_USBBUS1 if args.channel == "usb1" else PCAN_USBBUS2
    baud = {"500k": PCAN_BAUD_500K, "250k": PCAN_BAUD_250K, "1m": PCAN_BAUD_1M}[args.bitrate]
    try:
        bus = Pcan(ch)
    except Exception as e:
        board.close()
        print("[FAIL] %s" % e)
        return 2

    st = bus.open(baud)
    if st != PCAN_ERROR_OK:
        board.close()
        print("[FAIL] PCAN 打开失败: %s" % bus.error_text(st))
        return 2

    can_id = int(args.id, 16)
    data = [int(b, 16) & 0xFF for b in args.data][:8] or [0x11, 0x22, 0x33, 0x44]

    print("\n---- 从 PCAN 发 %d 帧：ID=0x%03X 数据=%s ----"
          % (args.count, can_id, " ".join("%02X" % b for b in data)))

    sent = 0
    for i in range(args.count):
        if bus.send(can_id, data, args.extended) != PCAN_ERROR_OK:
            print("  [%3d] 写入发送队列失败" % (i + 1))
            break
        sent += 1
        time.sleep(0.02)

    time.sleep(0.35)                       # 等 ACK 与板子的应用层发布
    st = bus.status()
    desc, bus_ok = describe_bus_status(st)
    print("  PCAN 总线状态: %s" % desc)
    bus.close()

    # ---------------- 3) 再读板子侧 ----------------
    after = None
    for _ in range(6):                     # 板子每轮才刷新一次寄存器，给它时间
        after, _err = board.snapshot()
        if after is not None and after["rx32"] > base["rx32"]:
            break
        time.sleep(0.25)
    board.close()

    if after is None:
        print("[FAIL] 发送后读不到寄存器")
        return 2

    delta = after["rx32"] - base["rx32"]
    window_s = time.time() - t_ref

    # 扣掉"回环自检"的贡献：优先用**实测基线率**×窗口；没量基线时退回
    # "每 5 秒一帧"的标称值（并明确说这是标称，判据要留余量）。
    if loop_rate is not None:
        loop_est_f = loop_rate * window_s
        src = "实测基线 %.2f 帧/秒" % loop_rate
    else:
        loop_est_f = window_s / 5.0
        src = "标称 1 帧/5 秒（没量基线，判据要留余量）"
    loop_est = int(round(loop_est_f))
    ext = delta - loop_est_f

    print("\n---- 结果 ----")
    print("  CAN 收帧计数: %d -> %d  (增量 %d，发送 %d)"
          % (base["rx32"], after["rx32"], delta, sent))
    print("  发帧窗口       : %.1f 秒" % window_s)
    print("  回环自检贡献   : ≈ %.1f 帧（%s）" % (loop_est_f, src))
    print("                  **外部帧净收到 ≈ %.1f 帧**（总增量 - 回环贡献）" % ext)
    print("  通道值 CH1..CH8: %s"
          % " ".join("%5d" % v for v in after["ch"]))
    print("  DEV_STATUS = 0x%04X" % after["status"])

    esr = after.get("esr")
    if esr is not None:
        print("  STM32 CAN 错误状态: TEC=%d REC=%d LEC=%d(%s) RXEP=%d TXEP=%d"
              % (esr["tec"], esr["rec"], esr["lec"], esr["lec_text"],
                 esr["rxep"], esr["txep"]))

    rc = 0
    # ⚠️ 判据基于**外部帧净收到**（扣掉回环自检），不是原始增量 —— 见上面说明。
    if ext >= sent and sent > 0:
        print("\n  [PASS] CAN 端到端通："
              "PCAN -> 收发器 -> bxCAN -> 应用 -> Modbus 寄存器")
        if esr is not None and esr["rec"] == 0 and esr["tec"] == 0:
            print("         且 STM32 侧错误计数全 0，"
                  "说明收发器 + 接线 + 终端都是好的。")
        if all(v == 0 for v in after["ch"]):
            print("  ℹ️ 通道值全 0：可能是没配 ID->通道映射"
                  "（L0 层把 0x123 映射到通道 1，可用 --id 0x123 验证）")
    elif ext > max(1.0, sent * 0.1):
        rc = 1
        print("\n  [WARN] 外部帧只收到约 %.1f / %d —— 真有丢帧。"
              "查 120Ω 终端、共地、线长与波特率一致性。" % (ext, sent))
    else:
        rc = 1
        print("\n  [FAIL] **外部帧一帧都没收到**"
              "（总增量 %d，回环自检预期就占 %.1f 帧）。" % (delta, loop_est_f))
        print("         先按下面**分流判据**确定方向：")

        if esr is None:
            print("         （本次走 Modbus 通道，读不到 STM32 的错误寄存器；")
            print("           建议加 --via swd，它能直接给出 TEC/REC/LEC）")
        elif esr["rec"] > 0 or esr["lec"] != 0:
            print("         ① STM32 的错误计数**不为 0**"
                  "（TEC=%d REC=%d LEC=%d %s）——"
                  % (esr["tec"], esr["rec"], esr["lec"], esr["lec_text"]))
            print("            ⚠️⚠️ 先别下结论：错误计数器**只在成功收帧时递减**，")
            print("                旧值会一直冻在这里（我自己就被它骗过一次 ——")
            print("                把 TXD 拉成显性做实验，留下的 REC=255 看起来")
            print("                像是'外部流量打出来的'）。")
            print("                **判据是增量**：先复位板子（STM32_Programmer_CLI -rst），")
            print("                确认 ESR=0，再发帧，然后看 REC 有没有从 0 涨起来：")
            print("                  涨  -> 信号到达了收发器但解不出来（极性/接线/终端）")
            print("                  仍 0 -> 信号压根没到（收发器供电/接线/没接到同一条总线）")
            print("            首选怀疑 **CAN_H / CAN_L 接反或某根不通**（H 必须对 H、L 对 L）。")
        else:
            print("         ① STM32 的错误计数**全 0**（TEC=REC=0、LEC 无错误）——")
            print("            说明信号**压根没到收发器**（总线是干净的隐性）。")
            print("            查：收发器 VCC/GND、RXD 是否真接到 PB8、")
            print("                H/L 是否接到了**收发器的** CAN_H/CAN_L（不是 MCU 引脚）。")

        print("         ② 波特率两边必须一致（本工程 500 kbps）。")
        print("         ③ 板上自检的 CAN 项是否 [ OK ]；")
        print("            CAN 回环自检（input[17] 每 5 秒 +1）能不能涨 ——")
        print("            回环涨而外部不涨 => 问题一定在收发器/接线这一段。")
        print("         ④ ⚠️ PCAN 侧显示'无错误'**不能**证明 ACK 成功：")
        print("            PCAN-Basic 默认开启 bus-off 自动恢复，且 CAN_Write 只表示入队。")
        print("            判据以 **STM32 侧的错误计数**为准。")

    print("\n[完成]")
    return rc


if __name__ == "__main__":
    sys.exit(main())
