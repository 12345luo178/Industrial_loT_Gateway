#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
用 STM32 自己的 ADC 当"万用表"（通过 SWD 配置，不需要改固件）

============================================================================
 为什么需要它

  排查 CAN 物理层时最想要的就是万用表：量总线两端的电压，看"驱动的那一端
  有没有真的输出差分、另一端有没有收到"。手边没有万用表时，STM32 的 ADC
  就是现成的电压表 —— 而且它本来就在板子上、本来就连着 SWD。

  典型用法（CAN 排障）：

      # 把两根跳线当"表笔"：
      #   绿模块 H 端子 -> PA0      （A 端）
      #   克隆模块 H 端子 -> PA4     （B 端）
      # ⚠️ 原来那根 H 线保持不动，这两根只是"并联测量"
      python tools/swd_adc_probe.py --pins PA0,PA4 --watch 60

      同时让克隆模块连续发帧：
      python tools/can_pcan_test.py --send --id 0x123 --count 200000 --period 0

  判读（CAN 总线，VREF=3.3V）：

      空闲（隐性）     H 与 L 都在 2.3~2.6 V
      有帧在传（显性） H 抬到 ~3.5 V、L 降到 ~1.5 V
      ⚠️ ADC/万用表读的是**平均值**：帧很密时能明显看到偏离 2.5 V；
         偶尔发一帧则几乎看不出 —— 所以要用 --period 0 连续发。

  于是：
      A、B 两端一起变化     -> 这根线是通的
      只有一端变化          -> **断点就在两端之间**（线/端子）
      两端都不动（都在2.5V） -> 驱动端没在驱动（适配器/收发器总线侧问题）
      两端都 0V 或乱跳      -> 这里根本不是 CAN_H（或收发器没供电）

============================================================================
 用法

    python tools/swd_adc_probe.py --list
    python tools/swd_adc_probe.py --pins PA0,PA4 --samples 8
    python tools/swd_adc_probe.py --pins PA0,PA4 --watch 60
    python tools/swd_adc_probe.py --pins PC0 --vref 3.30

 ⚠️ 它会把这些引脚改成**模拟输入**（原来是浮空/未用都无所谓）。
 ⚠️ 每次 SWD 读写约 1 秒，所以一个采样点要 2~3 秒 —— 看"秒级变化"够用，
    看不到帧级的抖动（那本来也不需要）。
"""

import argparse
import os
import sys
import time

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(errors="replace")
    except Exception:
        pass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from verify_can_dual import CLI, swd_read              # noqa: E402


def swd_read_word(addr):
    """读一个 32 位字（复用 verify_can_dual 里的 CLI 调用）"""
    w = swd_read(addr, 4)
    return w[0] if w else None

# ---------------- STM32F103 寄存器 ----------------
RCC_APB2ENR = 0x40021018
RCC_CFGR = 0x40021004

GPIO_BASE = {"PA": 0x40010800, "PB": 0x40010C00, "PC": 0x40011000}
GPIO_CRL_OFF = 0x00
GPIO_CRH_OFF = 0x04

ADC1_SR = 0x40012400
ADC1_CR2 = 0x40012408
ADC1_SMPR1 = 0x4001240C
ADC1_SMPR2 = 0x40012410
ADC1_SQR1 = 0x4001242C
ADC1_SQR3 = 0x40012434
ADC1_DR = 0x4001244C

# 引脚 -> ADC1 通道（STM32F103）
PIN_CH = {
    "PA0": 0, "PA1": 1, "PA2": 2, "PA3": 3,
    "PA4": 4, "PA5": 5, "PA6": 6, "PA7": 7,
    "PB0": 8, "PB1": 9,
    "PC0": 10, "PC1": 11, "PC2": 12, "PC3": 13, "PC4": 14, "PC5": 15,
    # ⚠️ 芯片内部通道，用来**自证 ADC 是好的**：
    #    VREFINT 标称 1.20V（芯片自带基准），读它应该得到 ≈1200mV。
    #    没有这个自检，"0mV" 到底是"引脚真的 0V"还是"ADC 根本没工作"就分不清 ——
    #    今天已经被两次"自造数据"坑过，这种判据必须先自证。
    "TS": 16,           # 内部温度传感器
    "VREFINT": 17,      # 内部 1.20V 基准
}

# 各端口的 APB2 时钟位
GPIO_CLK_BIT = {"PA": 2, "PB": 3, "PC": 4}
ADC1_CLK_BIT = 9


def swd_write(addr, value):
    import subprocess
    subprocess.run([CLI, "-c", "port=SWD", "mode=hotplug",
                    "-w32", "0x%08X" % addr, "0x%08X" % value],
                   capture_output=True, text=True, errors="replace")


def adc_setup(pins):
    """把引脚改成模拟输入，并启动 ADC1"""
    apb2 = swd_read_word(RCC_APB2ENR) or 0
    apb2 |= (1 << ADC1_CLK_BIT)
    for p in pins:
        if p in GPIO_CLK_BIT:          # 内部通道（VREFINT/TS）没有端口
            apb2 |= (1 << GPIO_CLK_BIT[p])
    swd_write(RCC_APB2ENR, apb2)

    # ADCPRE = PCLK2/6 = 12 MHz（ADC 时钟上限 14 MHz）
    cfgr = swd_read_word(RCC_CFGR) or 0
    cfgr = (cfgr & ~(0x3 << 14)) | (0x2 << 14)
    swd_write(RCC_CFGR, cfgr)

    # 引脚 -> 模拟输入（MODE=00 CNF=00 -> 半字节 0x0）
    for p in pins:
        if p not in GPIO_CLK_BIT:
            continue                   # 内部通道跳过
        port, num = p[:2], int(p[2:])
        base = GPIO_BASE[port]
        if num < 8:
            reg, shift = base + GPIO_CRL_OFF, num * 4
        else:
            reg, shift = base + GPIO_CRH_OFF, (num - 8) * 4
        val = swd_read_word(reg) or 0
        val = (val & ~(0xF << shift)) | (0x0 << shift)
        swd_write(reg, val)

    # 采样时间：全部设成最大（239.5 周期），省得挑
    swd_write(ADC1_SMPR1, 0x00FFFFFF)
    swd_write(ADC1_SMPR2, 0x3FFFFFFF)
    swd_write(ADC1_SQR1, 0x00000000)        # 序列长度 = 1
    # ⚠️ 软件触发必须写成 EXTSEL=SWSTART(111) + EXTTRIG=1 + ADON=1：
    #    只写 ADON|SWSTART 的话 SWSTART **不起作用**（转换不会发生），
    #    DR 里会留着上一次的值 —— 于是"三个不同通道读出同一个数"。
    #    这个坑是拿内部 VREFINT 自检时抓出来的（详见 adc_read 的 EOC 校验）。
    swd_write(ADC1_CR2, ADC_CR2_CFG)
    time.sleep(0.05)


# ADC_CR2 配置：ADON | CONT=0 | TSVREFE | EXTTRIG | EXTSEL=SWSTART
#
# ⚠️⚠️ TSVREFE(bit23) 是**必须**的：不置它，内部基准(VREFINT)/温度传感器
#     那条模拟通路根本没接通 —— 读 VREFINT 得到的是**垃圾值**，
#     而我的"自校准"正是拿它当 1.20V 基准用，于是整把尺子都是假的
#     （现象：VREFINT 在同一轮里漂 10%、TS 读 0、所有通道读数可疑）。
#     漏 EXTTRIG/EXTSEL 是"转换不跑"，漏 TSVREFE 是"参考是假的" ——
#     两次都靠"内部基准自检"才暴露。测量手段必须先自证。
ADC_CR2_CFG = (1 << 0) | (1 << 23) | (1 << 20) | (0x7 << 17)


def adc_read(ch):
    """读一次（单位：原始值 0..4095）

    ⚠️ 必须校验 EOC：
       SR.EOC=1 才代表"这一次转换真的发生了"。不校验的话，
       转换没跑时读到的是**上一次的残留值** —— 会得到"所有通道读数相同"
       这种看起来合理、其实完全错误的结果（我就是这么被它骗了一次）。
    """
    swd_write(ADC1_SQR3, ch & 0x1F)
    swd_write(ADC1_CR2, ADC_CR2_CFG | (1 << 22))     # ... | SWSTART
    time.sleep(0.02)

    sr = swd_read_word(ADC1_SR)
    if sr is None or (sr & 0x2) == 0:               # EOC 没置 -> 没转换
        return None

    # ⚠️ 不再单独写 SR 清 EOC：F1 上**读 DR 就会自动清 EOC** ✓
    #    （每次 SWD 往返 1~2 秒，省一次就快一截 —— 采样点数直接影响能否看出变化）
    return swd_read_word(ADC1_DR)


def read_pin_list(spec):
    pins = [p.strip().upper() for p in spec.split(",") if p.strip()]
    for p in pins:
        if p not in PIN_CH:
            raise SystemExit("不认识的引脚 %s（支持 PA0-7 / PB0-1 / PC0-5）" % p)
    return pins


def fmt_mv(mv):
    if mv is None:
        return "   --  "
    return "%6d mV" % mv


def main():
    ap = argparse.ArgumentParser(
        description="用 STM32 的 ADC 当万用表（SWD 配置，不改固件）",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pins", help="要测的引脚，逗号分隔，如 PA0,PA4")
    ap.add_argument("--list", action="store_true", help="列出可用的 ADC 引脚")
    ap.add_argument("--samples", type=int, default=6, help="每个引脚取多少次平均")
    ap.add_argument("--watch", type=float, default=0.0, metavar="SEC",
                    help="持续采样 SEC 秒（看变化用）")
    ap.add_argument("--vref", type=float, default=3.30, help="参考电压（默认 3.3V）")
    args = ap.parse_args()

    if args.list or not args.pins:
        print("可用的 ADC1 引脚：")
        for p, c in PIN_CH.items():
            print("  %-4s ADC1_IN%d" % (p, c))
        print("\n用法示例：python tools/swd_adc_probe.py --pins PA0,PA4 --watch 60")
        return 0

    if not CLI:
        print("[FAIL] 找不到 STM32_Programmer_CLI（可用 GW_STLINK_CLI 指定）")
        return 2

    pins = read_pin_list(args.pins)

    print("=" * 66)
    print("  STM32 ADC 当万用表   VREF = %.2f V" % args.vref)
    print("  测量引脚: %s" % " ".join(pins))
    print("=" * 66)

    adc_setup(pins)

    # ---- 用内部 VREFINT（标称 1.20V）自校准参考电压 ----
    # ⚠️ 不能直接假设 VREF = 3.30V：实测按 3.30V 算，VREFINT 读出 1319mV
    #    （标称 1200mV），说明实际 VDDA 约 3.0V。用内准反推，读数才有意义。
    vref = args.vref
    vref_raw = [adc_read(PIN_CH["VREFINT"]) for _ in range(6)]
    vref_raw = [v for v in vref_raw if v]
    if vref_raw:
        avg = sum(vref_raw) / len(vref_raw)
        spread = (max(vref_raw) - min(vref_raw)) / avg * 100.0 if avg else 999.0
        vdda = 1.20 * 4095.0 / avg if avg > 0 else 0.0
        if avg > 100:
            vref = vdda
        print("[ OK ] 内部基准自检：VREFINT 原始 %.0f（6 次离散度 %.1f%%）"
              % (avg, spread))
        print("       推算 VDDA = %.2f V" % vdda)
        # ⚠️ 参考不可信就别给数字：离散度大 / VDDA 离谱 = 自作聪明地报错更好
        if spread > 3.0:
            print("       [WARN] 内部基准离散度偏大 -> 读数只能当**粗略**判据")
        if vdda < 2.5 or vdda > 3.6:
            print("       [WARN] 推算 VDDA=%.2f V 不在 2.5~3.6V 合理区间 -> 参考可能不可信"
                  % vdda)
            print("              （若刚置过 TSVREFE，先多读几轮让它稳定）")
    else:
        print("[WARN] 内部基准读不到，按 --vref %.2f 计算" % vref)

    print("[ OK ] 已把 %s 配成模拟输入并启动 ADC1" % ",".join(pins))
    print("       参考值：CAN 空闲时 H/L 都约 2.3~2.6V；")
    print("       有帧在传时 H 抬到 ~3.5V、L 降到 ~1.5V（ADC 看的是平均值）")

    def sample_all():
        out = {}
        for p in pins:
            vals = []
            for _ in range(max(1, args.samples)):
                v = adc_read(PIN_CH[p])
                if v is not None:
                    vals.append(v)
            out[p] = (sum(vals) / len(vals)) if vals else None
        return out

    if args.watch <= 0.0:
        res = sample_all()
        print()
        for p in pins:
            mv = None if res[p] is None else res[p] * vref * 1000.0 / 4095.0
            print("  %-4s = %s   (原始 %.0f)" %
                  (p, fmt_mv(mv), res[p] if res[p] is not None else -1))
        return 0

    # ---- 持续采样：每轮打印一行，方便一边动线/发帧一边看 ----
    t0 = time.time()
    print("\n  时间      " + "".join("%-12s" % p for p in pins))
    try:
        while time.time() - t0 < args.watch:
            res = sample_all()
            line = "  %5.1fs   " % (time.time() - t0)
            for p in pins:
                mv = None if res[p] is None else res[p] * vref * 1000.0 / 4095.0
                line += "%-12s" % (fmt_mv(mv).strip() if mv is not None else "--")
            print(line)
    except KeyboardInterrupt:
        pass
    print("\n[完成] 引脚保持在模拟输入状态（对固件无影响）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
