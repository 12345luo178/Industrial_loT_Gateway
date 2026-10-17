"""CAN 接收方向探针：**不用万用表**，用 SWD 改一个 GPIO 配置寄存器就能判断
"收发器模块到底有没有在驱动 RXD"。

============================================================================
 原理（为什么这个办法能替代万用表）
============================================================================
模块的 **RX 引脚是收发器的输出（RXD）**，接到板子的 **PB8（CAN_RX，MCU 输入）**。
而 PB8 目前是**内部上拉输入** —— 于是"读到高"什么都没证明：
收发器驱动高也读高，RXD 悬空（模块没工作）**也**读高。这就是量不到电压时卡住的地方。

把 PB8 改成**浮空输入**（去掉内部上拉，CNF 10 -> 01，nibble 0x8 -> 0x4）再读，
两种情形就分开了：

    · **仍读高**   -> 有人在**主动驱动**这条线 -> 模块有电且在正常工作
    · **读低/乱跳** -> RXD 是高阻（悬空）      -> **模块没在工作**（未供电/型号不符/接线）

再配合 PCAN 持续发帧：如果浮空状态下能**采到低电平（显性位）**，
就说明"总线 -> 收发器 -> RXD -> PB8"这条**接收**链路是通的。

⚠️ 这与前面那批"读寄存器"的诊断不同：它会**短暂修改一个硬件寄存器**。
   所以：① 默认**不写**（要显式 `--apply`）；② 无论成功失败都**恢复原值**（finally）；
   ③ 只在 PB8 那 4 个位上动手（`GPIOB_CRH` 的 bit[3:0]）。

用法：
    python tools/can_pin_probe.py                      # 只看现状（不写任何寄存器）
    python tools/can_pin_probe.py --apply              # 做浮空探针（10 秒，自动恢复）
    python tools/can_pin_probe.py --apply --pcan       # 同时让 PCAN 持续发帧（更灵敏）
"""
import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from verify_can_dual import CLI, swd_read                 # noqa: E402

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(errors="replace")

GPIOB = 0x40010C00
CRH = GPIOB + 0x04          # 引脚 8..15 的配置
IDR = GPIOB + 0x08          # 输入数据寄存器
PB8_BIT = 1 << 8
NIB_FLOAT = 0x4             # MODE=00（输入）+ CNF=01（浮空）
NIB_PULLUP = 0x8            # MODE=00 + CNF=10 + ODR=1（上拉）——工程当前值
NIB_PULLDOWN = 0x8          # MODE=00 + CNF=10 + ODR=0（下拉）：CNF 相同，靠 ODR 选上下拉
ODR = GPIOB + 0x0C


def rd32(addr):
    w = swd_read(addr, 4)
    return w[0] if w else None


def wr32(addr, value):
    p = subprocess.run([CLI, "-c", "port=SWD", "mode=hotplug",
                        "-w32", "0x%08X" % addr, "0x%08X" % value],
                       capture_output=True, text=True, errors="replace")
    return p.returncode == 0


def describe(crh):
    nib = crh & 0xF
    if nib == NIB_PULLUP:
        return '0x%X = 输入 + 上拉/下拉（工程当前值）' % nib
    if nib == NIB_FLOAT:
        return '0x%X = 输入 + 浮空' % nib
    return '0x%X（MODE=%d CNF=%d）' % (nib, nib & 0x3, (nib >> 2) & 0x3)


def tx_test(seconds):
    """★ 动态判据：把 PB9（CAN_TX）临时当普通 GPIO 输出**低电平**（= 持续显性位），
    再看 PB8（CAN_RX）会不会跟着变低。

    ⚠️ 为什么必须有这个测试（2026-10-11 第 36 轮，用户反馈已接 5V 且没接反之后）：
       静态的"上拉/下拉"判据**有个漏洞** —— 它假设收发器的 RXD 是**推挽**输出。
       若这颗芯片的 RXD 是**开漏**（只有下拉、从不驱动高），那么
       "上拉读高 + 下拉读低"在**工作正常**和**完全没接通**两种情况下**完全一样**，
       分不出来。我前面据此下的结论因此**站不住**。

       动态判据没有这个漏洞：TXD 拉低 -> 收发器把总线拉成显性 -> 它自己的 RXD
       必然输出低。**只要模块有电且在正常工作，这条一定成立**（与 RXD 是推挽
       还是开漏无关 —— 两种情况都是"拉低"）。
    """
    odr0 = rd32(ODR)
    crh0 = rd32(CRH)
    if odr0 is None or crh0 is None:
        print('  [FAIL] SWD 读不到 GPIOB')
        return 2
    # PB9 在 CRH 的 bit[7:4]；0x3 = 输出推挽 50MHz
    nib9 = (crh0 >> 4) & 0xF
    new_crh = (crh0 & ~0xF0) | 0x30
    new_odr = odr0 & ~(1 << 9)          # PB9 输出低 = 显性
    print('  PB9 当前配置 nibble = 0x%X（0xB=复用推挽，即 CAN_TX 正常态）' % nib9)
    print('  写入 GPIOB_CRH: 0x%08X -> 0x%08X（PB9 -> 推挽输出）'
          % (crh0, new_crh))
    print('  写入 GPIOB_ODR: 0x%08X -> 0x%08X（PB9 输出**低** = 持续显性）'
          % (odr0, new_odr))
    lows = highs = samples = 0
    try:
        if not wr32(CRH, new_crh) or not wr32(ODR, new_odr):
            print('  [FAIL] 写寄存器失败')
            return 1
        if rd32(CRH) != new_crh or rd32(ODR) != new_odr:
            print('  [FAIL] 回读不一致 —— 中止')
            return 1
        t0 = time.time()
        while time.time() - t0 < seconds:
            v = rd32(IDR)
            if v is None:
                break
            samples += 1
            if v & PB8_BIT:
                highs += 1
            else:
                lows += 1
            time.sleep(0.2)
    finally:
        # ⚠️ PB9 是 CAN_TX：必须恢复成复用推挽，否则后面发不出帧
        ok1 = wr32(CRH, crh0)
        ok2 = wr32(ODR, odr0)
        now = rd32(CRH)
        print('  恢复 GPIOB_CRH -> 0x%08X：%s（回读 0x%08X，PB9 nibble=0x%X）'
              % (crh0, '成功' if ok1 else '**失败**',
                 now if now is not None else 0,
                 ((now or 0) >> 4) & 0xF))
        print('  恢复 GPIOB_ODR -> 0x%08X：%s' % (odr0, '成功' if ok2 else '**失败**'))
    print('-' * 68)
    print(' 动态判据（PB9 拉低 = 显性）：采样 %d 次，PB8 高 %d / 低 %d'
          % (samples, highs, lows))
    if lows > 0:
        print(' 结论：**模块是活的** —— 它把显性位回送到了 RXD（PB8 变低）。')
        print('       TXD->总线->RXD 这条环是通的，模块有电、在正常工作。')
        print('       那故障就在**总线那一侧**：CANH/CANL 极性或接通、终端电阻、')
        print('       对端（PCAN）配置与接线。')
    else:
        print(' 结论：**PB9 拉低后 PB8 仍然不低 —— 模块没有把显性位送回 RXD**。')
        print('       可能：① 模块没供电（确认 5V 是**接在模块 VCC 上**、且与板子共地）；')
        print('             ② 模块 RX（输出）那根线**没真正接到 PB8**（"没接反"不等于"接到了"）；')
        print('             ③ 模块本身坏（换一块试）。')
    return 0


def wire_test(seconds):
    """单向（但决定性）判据：**PB9 配成下拉输入**，看它会不会被拉高。

    依据：TJA 系列收发器的 **TXD 引脚内部有上拉**（设计意图：MCU 悬空/未初始化时
    保持总线隐性）。所以若 PB9 上真接着模块的 TXD，MCU 的 ~40kΩ 内部下拉
    可能被那个上拉顶住 -> 读到**高**。

    ⚠️ 这是**单向**判据：
        · 读**高** -> 硬证据：有东西在往上拉 -> **模块的 TX 线确实接在 PB9 上**；
        · 读**低** -> **什么都不能说明**（可能没接，也可能对方上拉比 40kΩ 弱）。
    """
    crh0 = rd32(CRH)
    odr0 = rd32(ODR)
    if crh0 is None or odr0 is None:
        print('  [FAIL] SWD 读不到 GPIOB')
        return 2
    new_crh = (crh0 & ~0xF0) | NIB_PULLDOWN        # PB9 -> 输入 + 上下拉
    new_odr = (odr0 or 0) & ~(1 << 9)              # ODR bit9=0 -> 下拉
    highs = lows = samples = 0
    print('  写入 GPIOB_CRH: 0x%08X -> 0x%08X（PB9 -> 输入+上下拉）'
          % (crh0, new_crh))
    print('  写入 GPIOB_ODR: 0x%08X -> 0x%08X（bit9=0 -> 选**下拉**，~40kΩ）'
          % (odr0, new_odr))
    try:
        if not wr32(CRH, new_crh) or not wr32(ODR, new_odr):
            print('  [FAIL] 写寄存器失败')
            return 1
        if rd32(CRH) != new_crh or rd32(ODR) != new_odr:
            print('  [FAIL] 回读不一致 —— 中止')
            return 1
        t0 = time.time()
        while time.time() - t0 < seconds:
            v = rd32(IDR)
            if v is None:
                break
            samples += 1
            if v & (1 << 9):
                highs += 1
            else:
                lows += 1
            time.sleep(0.2)
    finally:
        ok1 = wr32(CRH, crh0)
        ok2 = wr32(ODR, odr0)
        now = rd32(CRH)
        print('  恢复 GPIOB_CRH -> 0x%08X：%s（PB9 nibble=0x%X，应为 0xB）'
              % (crh0, '成功' if ok1 else '**失败**',
                 ((now or 0) >> 4) & 0xF))
        print('  恢复 GPIOB_ODR -> 0x%08X：%s' % (odr0, '成功' if ok2 else '**失败**'))
    print('-' * 68)
    print(' PB9 下拉读数：采样 %d 次，高 %d / 低 %d' % (samples, highs, lows))
    if highs > 0:
        print(' 结论：**PB9 被拉高了 -> 模块的 TX 线确实接在 PB9 上** ✓')
        print('       （TXD 内部上拉顶住了 MCU 的下拉）')
        print('       那问题只剩"模块有没有电 / 模块是不是坏的"：')
        print('       · 换一个**确定的 5V 源**（USB 口）并共地，再跑 --tx-test；')
        print('       · 仍不行就换一块模块。')
    else:
        print(' 结论：下拉压住了 —— **说明不了什么**（单向判据）：')
        print('       可能"TX 线没接到 PB9"，也可能"接到了但对方上拉比 40kΩ 弱"。')
        print('       请先**肉眼确认 TX 线两端**：模块 TX/TXD 那一端 与 板子 PB9 那一端。')
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--apply', action='store_true',
                    help='真的做浮空探针（会短暂改 PB8 配置，结束自动恢复）')
    ap.add_argument('--seconds', type=float, default=10.0)
    ap.add_argument('--pcan', action='store_true',
                    help='探针期间让 PCAN 持续发帧（后台线程）')
    ap.add_argument('--tx-test', action='store_true',
                    help='★ 动态判据：临时把 PB9 当 GPIO 输出低（显性），'
                         '看 PB8 会不会跟着变低 —— 与 RXD 是推挽还是开漏无关')
    ap.add_argument('--wire-test', action='store_true',
                    help='单向判据：PB9 配成下拉输入，读高即证明"模块 TX 线确实接在 PB9 上"')
    a = ap.parse_args()

    if not CLI:
        print('  [FAIL] 找不到 STM32_Programmer_CLI')
        return 2

    if a.tx_test:
        print('=' * 68)
        print(' CAN 动态判据：TXD 拉低 -> 看 RXD 是否回低（自动恢复 PB9/PB8）')
        print('=' * 68)
        return tx_test(a.seconds)

    if a.wire_test:
        print('=' * 68)
        print(' CAN 接线单向判据：PB9 下拉 -> 读高即证明模块 TX 线接在 PB9 上')
        print('=' * 68)
        return wire_test(a.seconds)

    crh0 = rd32(CRH)
    idr0 = rd32(IDR)
    if crh0 is None or idr0 is None:
        print('  [FAIL] SWD 读不到 GPIOB（板子在线吗？）')
        return 2
    print('=' * 68)
    print(' CAN 接收方向探针（只读 SWD；--apply 时短暂改 PB8 配置）')
    print('=' * 68)
    print('  PB8 当前配置 : %s' % describe(crh0))
    print('  PB8 当前电平 : %s（带上拉时读高不能说明任何事）'
          % ('高' if idr0 & PB8_BIT else '低'))

    if not a.apply:
        print('')
        print(' 这是**只读**模式（没有改任何寄存器）。三种判据：')
        print('   --tx-test    ★ 动态、决定性：PB9 拉低（显性）-> 看 PB8 是否回低')
        print('   --wire-test  单向：PB9 下拉读高 => 模块 TX 线确实接在 PB9 上')
        print('   --apply      静态：仅供参考（⚠️ 若 RXD 是开漏，它分不出'
              '"工作正常"与"没接通"）')
        return 0

    # ---- 让 PCAN 在后台持续发帧（可选）----
    sender = None
    if a.pcan:
        try:
            from can_pcan_test import Pcan, PCAN_USBBUS1
            sender = Pcan(PCAN_USBBUS1, '500k')
            sender.start()
            print('  PCAN 已打开，探针期间持续发帧')
        except Exception as e:                              # noqa: BLE001
            print('  [注意] PCAN 打开失败（%s）—— 只做静态探针' % e)
            sender = None

    # ---- 判据：用**内部下拉**当负载 ----
    #
    # ⚠️⚠️ 第一版这里写错了（2026-10-11 当场纠正）：
    #     我原本用"浮空后读低 = 有人在驱动低"，这是**错的** ——
    #     浮空引脚本来就可能读低，"读低"根本区分不出"被驱动"和"没人管"。
    #     正确判据是反过来：**给它一个下拉，看它能不能顶住**。
    #
    #       PB8 配成**下拉输入**（CNF=10 + ODR bit8=0）后读：
    #         仍读 **高** -> 有个**推挽输出**在顶住下拉 -> 收发器在驱动 RXD（隐性=高）
    #                       -> **模块是活的**
    #         读 **低**   -> 没人驱动（高阻），下拉把它压住了
    #                       -> **模块没在工作**（未供电/型号不符/接线错/RXD 没接通）
    lows = 0
    highs = 0
    samples = 0
    odr0 = rd32(ODR)
    try:
        new_crh = (crh0 & ~0xF) | NIB_PULLDOWN          # 输入 + 上/下拉（CNF=10）
        new_odr = (odr0 or 0) & ~PB8_BIT                # ODR=0 -> 选**下拉**
        print('  写入 GPIOB_CRH: 0x%08X -> 0x%08X（PB8 -> 输入+上下拉）'
              % (crh0, new_crh))
        print('  写入 GPIOB_ODR: 0x%08X -> 0x%08X（bit8=0 -> 选**下拉**）'
              % (odr0 or 0, new_odr))
        if not wr32(CRH, new_crh) or not wr32(ODR, new_odr):
            print('  [FAIL] 写寄存器失败')
            return 1
        if rd32(CRH) != new_crh or rd32(ODR) != new_odr:
            print('  [FAIL] 回读不一致 —— 中止')
            return 1

        t0 = time.time()
        while time.time() - t0 < a.seconds:
            v = rd32(IDR)
            if v is None:
                break
            samples += 1
            if v & PB8_BIT:
                highs += 1
            else:
                lows += 1
            time.sleep(0.2)
    finally:
        # ⚠️ 无论发生什么都要恢复原值（CRH 与 ODR 都恢复）
        ok1 = wr32(CRH, crh0)
        ok2 = wr32(ODR, odr0 or 0)
        print('  恢复 GPIOB_CRH -> 0x%08X：%s' % (crh0, '成功' if ok1 else '**失败**'))
        print('  恢复 GPIOB_ODR -> 0x%08X：%s（回读 0x%08X）'
              % (odr0 or 0, '成功' if ok2 else '**失败**',
                 rd32(ODR) if rd32(ODR) is not None else 0))
        if sender is not None:
            try:
                sender.stop()
            except Exception:                               # noqa: BLE001
                pass

    print('-' * 68)
    print(' 下拉探针：采样 %d 次，高 %d 次 / 低 %d 次' % (samples, highs, lows))
    if highs > 0 and lows == 0:
        print(' 结论：**下拉顶不住 —— 有人在主动驱动 RXD 为高（隐性）**')
        print('       收发器是活的（正常的 CAN 空闲态就是隐性=高）。')
        print('       那故障就不在"模块有没有工作"，转查 TX 方向、终端、极性、对端。')
    elif lows > 0 and highs == 0:
        print(' 结论：下拉把这条线压住了 —— 但**这不足以下结论**！')
        print('       ⚠️ 2026-10-11 自我纠正：这个静态判据假设收发器的 RXD 是**推挽**输出。')
        print('          若这颗芯片的 RXD 是**开漏**（只拉低、从不驱动高），那么"上拉读高 +')
        print('          下拉读低"在**工作正常**与**完全没接通**两种情况下**完全一样**。')
        print('       请改用动态判据（与输出类型无关）：')
        print('          python tools\\can_pin_probe.py --tx-test --seconds 6')
        print('       它把 PB9 当 GPIO 拉低（持续显性），看 PB8 会不会跟着低 ——')
        print('       模块只要活着就一定会把显性位送回来。')
    else:
        print(' 结论：采样里**高低都有** -> 这条线在动（可能在接收总线活动，'
              '或处于亚稳态）。')
        print('       这时最有价值的是：让 PCAN 持续发帧再跑一次 '
              '`--apply --pcan`，看是否采到显性位。')
    return 0


if __name__ == '__main__':
    sys.exit(main())
