#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""RTOS 版的板级验收：一条命令把迁移指南的验收判据读全（走 SWD，不占串口）。

============================================================================
 为什么要有它
============================================================================

  迁移指南末尾有一张"验收标准"表（任务都在跑 / 喂狗被拒 0 次 / 栈水位 …），
  但落地当天只能**靠看串口手工判断** —— 而串口里那几行是给人看的，
  不是给"判据"看的：数错一行、漏看一条都很容易。

  这个脚本把判据变成读数：

    · `s_feed_ok` / `s_feed_reject`  （gw_rtos.c 的喂狗计数）-> 被拒必须为 0
    · `s_iwdg_on`                     -> 狗到底开没开（没开时上面那个计数不算数）
    · 喂狗**速率**（两次采样求差）     -> "喂狗时机"是否正常（空闲钩子在跑）
    · IWDG PR/RLR、RCC_CSR            -> 配置与"有没有非预期复位"
    · `g_regs` 的 uptime / dev_status / CAN 收帧 / MB 错误计数

  ⚠️ 它**只读**（SWD hotplug），不 halt、不影响浸泡或运行中的固件。

============================================================================
 用法
============================================================================

    python tools/rtos_state.py                       # 默认看 build/ota/app_a.elf
    python tools/rtos_state.py --elf build/rtos_link/app_rtos_link.elf
    python tools/rtos_state.py --gap 3               # 两次采样间隔（秒），看喂狗速率

  退出码：0 = 验收判据全过；1 = 有判据未过；2 = 无法判定（不是 RTOS 构建 / 读不到）
============================================================================
"""
import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from verify_can_dual import (swd_read, words_to_regs,      # noqa: E402
                             nm_g_regs, NM, check_image)

# ⚠️ 编码守卫（print 里有 ⚠️/✅）
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(errors="replace")
    except (AttributeError, ValueError):
        pass

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ⚠️ 这里原本写死 `UW_TICK = 0x2000026C`，但**全文件从未使用**（死代码）。
#    已删除：这类"过时常量"留在代码里迟早会被误用。
#    真正要读时基的是 soak_monitor.py，它现在**从 ELF 现取** uwTick 地址。
RCC_CSR   = 0x40021024
IWDG_BASE = 0x40003000
CSR_RESET_MASK = 0xFE000000
IWDG_PR_EXPECT, IWDG_RLR_EXPECT = 4, 999
STATUS_BIT_DHT_OK, STATUS_BIT_CAN_OK = 1 << 0, 1 << 1

# gw_rtos.c 里的静态符号（nm 取地址）
SYMS = ("s_feed_ok", "s_feed_reject", "s_iwdg_on")


def nm(elf, names):
    """取符号地址；返回 {name: addr}（找不到就不含该键）"""
    try:
        out = subprocess.run([NM, "-S", elf], capture_output=True, text=True,
                             errors="replace")
    except OSError:
        return {}
    got = {}
    for line in out.stdout.splitlines():
        p = line.split()
        if len(p) >= 4 and p[3] in names:
            try:
                got[p[3]] = int(p[0], 16)
            except ValueError:
                pass
    return got


def u32(addr):
    w = swd_read(addr, 4)
    return w[0] if w else None


def evaluate(snap):
    """★ 纯逻辑：把一次快照判成 (fails, warns)

    ⚠️ 单独抽出来的理由：判据是**验收的依据**，必须能脱离板子被自证 ——
       否则"落地当天读了一次、看着没问题"就成了唯一的验证方式。
       自证见 selfproof（合成快照喂进来，逐条判据都要能报错）。

    snap 字段（None = 读不到，按"注意"处理，不当成失败）：
        feed_ok, feed_rej, feed_rate, iwdg_on, iwdg_pr, iwdg_rlr, csr,
        dev, up, mb_err, can_rx
    """
    fails, warns = [], []

    # 喂狗被拒次数必须是 0（迁移指南验收标准）
    if snap.get("feed_rej") is not None and snap["feed_rej"] != 0:
        fails.append("喂狗被拒 %d 次（验收要求为 0）" % snap["feed_rej"])

    # 喂狗速率：空闲钩子在跑才会持续喂
    fr = snap.get("feed_rate")
    if fr is not None and fr < 1.0:
        fails.append("喂狗速率近乎 0（%.1f 次/秒）—— 空闲任务没在跑？" % fr)

    # 狗没开时，上面的喂狗计数不代表狗在工作 —— 只是"注意"，不是失败
    if snap.get("iwdg_on") == 0:
        warns.append("IWDG 还没使能（此时喂狗计数不代表狗在工作）")

    if snap.get("iwdg_pr") is not None and (
            snap["iwdg_pr"] != IWDG_PR_EXPECT or snap["iwdg_rlr"] != IWDG_RLR_EXPECT):
        fails.append("IWDG 配置不是 4/999（实际 %s/%s）"
                     % (snap["iwdg_pr"], snap["iwdg_rlr"]))

    if snap.get("csr") is not None and (snap["csr"] & CSR_RESET_MASK):
        fails.append("RCC_CSR 有复位标志 0x%08X（发生过非预期复位）" % snap["csr"])

    dev = snap.get("dev")
    if dev is not None:
        if not (dev & STATUS_BIT_DHT_OK):
            warns.append("DHT_OK = 0")
        if not (dev & STATUS_BIT_CAN_OK):
            warns.append("CAN_OK = 0")

    if snap.get("up") is None:
        warns.append("读不到活的 g_regs（应用没在刷新？）")

    return fails, warns


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=os.path.join(REPO, "build", "ota", "app_a.elf"))
    ap.add_argument("--gap", type=float, default=2.0, help="两次采样间隔秒（看喂狗速率）")
    ap.add_argument("--gregs", type=lambda x: int(x, 0), default=None)
    ap.add_argument("--image-only", action="store_true",
                    help="只核对「板子上跑的镜像 == 这份 ELF」，不读任何变量")
    a = ap.parse_args()

    print("=" * 68)
    print(" RTOS 板级验收（只读 SWD）")
    print("   ELF: %s" % a.elf)
    print("=" * 68)

    # ★★ 先核对镜像一致性：不一致就**不许继续读**（读出来的是别的变量）
    ok_img, detail = check_image(a.elf)
    if ok_img is False:
        print("  [无法判定] **板子上跑的不是这份 ELF** —— 读数不可信，已中止")
        print("             %s" % detail)
        print("             多半是 --elf 指的旧镜像（比如默认的 build/ota/app_a.elf）。")
        print("             请用**刚烧进去的那份** ELF 重跑。")
        return 2
    if ok_img is None:
        print("  [注意] 镜像一致性无法核对（%s）—— 继续，但读数仅供参考" % detail)
    else:
        print("  [ OK ] 镜像一致：%s" % detail)

    if a.image_only:
        return 0 if ok_img else 2

    syms = nm(a.elf, SYMS)
    if not syms:
        print("  [无法判定] 这份镜像里没有 %s" % "/".join(SYMS))
        print("             说明它**不是 RTOS 构建**（普通镜像里是 gw_rtos.c 被排除的）。")
        print("             落地后请用 --elf 指向新烧进去的那份镜像。")
        return 2

    fails = []
    warns = []
    snap = {}

    # ---- g_regs：活表 ----
    cands = [a.gregs] if a.gregs else nm_g_regs(a.elf)
    gregs = None
    for c in cands:
        w = swd_read(c, 0x70)
        if not w:
            continue
        regs = words_to_regs(w)
        if len(regs) >= 40 and ((regs[11] << 16) | regs[12]) > 0:
            gregs = regs
            break
    if gregs is None:
        snap["up"] = None
    else:
        snap["up"] = (gregs[11] << 16) | gregs[12]
        snap["dev"] = gregs[10]
        snap["mb_err"] = (gregs[16] << 16) | gregs[15]
        snap["can_rx"] = (gregs[18] << 16) | gregs[17]
        print("  运行时长      : %d 秒（input[12]）" % snap["up"])
        print("  dev_status    : 0x%04X  (DHT_OK=%d CAN_OK=%d)"
              % (snap["dev"], 1 if snap["dev"] & STATUS_BIT_DHT_OK else 0,
                 1 if snap["dev"] & STATUS_BIT_CAN_OK else 0))
        print("  MB 错误帧     : %d" % snap["mb_err"])
        print("  CAN 收帧      : %d" % snap["can_rx"])

    # ---- 喂狗计数（两次采样求速率）----
    # ⚠️ 目标读不到（ST-LINK 断连 / 板子没上电）时就别继续 —— 否则下面每个
    #    符号都读成 None，最后崩在 `None & 0xFF` 上（2026-10-11 实测：
    #    DEV_USB_COMM_ERR 时就是崩在那一行，看起来像"读到了但值不对"）。
    if u32(0x20000274) is None:        # uwTick：目标活着就该读得到
        print("  [FAIL] 读不到目标 —— 检查 ST-LINK/USB 连接与板子供电后重跑。")
        print("         实测常见原因：调试器 USB 断连（DEV_USB_COMM_ERR）；"
              "或板子处于 IWDG 复位循环")
        return 2

    feed_ok_1 = u32(syms["s_feed_ok"]) if "s_feed_ok" in syms else None
    feed_ok_2 = feed_ok_1
    rej_1 = u32(syms["s_feed_reject"]) if "s_feed_reject" in syms else None
    _iw = u32(syms["s_iwdg_on"]) if "s_iwdg_on" in syms else None
    snap["iwdg_on"] = (_iw & 0xFF) if _iw is not None else None
    if a.gap > 0 and feed_ok_1 is not None:
        time.sleep(a.gap)
        feed_ok_2 = u32(syms["s_feed_ok"])
    snap["feed_ok"] = feed_ok_2
    snap["feed_rej"] = u32(syms["s_feed_reject"]) if "s_feed_reject" in syms else None

    print("  IWDG 使能标记 : %s" % snap["iwdg_on"])
    print("  喂狗成功      : %s -> %s" % (feed_ok_1, feed_ok_2))
    print("  喂狗被拒      : %s -> %s" % (rej_1, snap["feed_rej"]))
    if (a.gap > 0 and feed_ok_1 is not None and feed_ok_2 is not None):
        snap["feed_rate"] = (feed_ok_2 - feed_ok_1) / a.gap
        print("  喂狗速率      : %.0f 次/秒（空闲钩子的调用频率）" % snap["feed_rate"])

    # ---- IWDG 配置与复位标志 ----
    w = swd_read(IWDG_BASE, 16)
    if w and len(w) >= 3:
        snap["iwdg_pr"], snap["iwdg_rlr"] = w[1], w[2]
        print("  IWDG PR/RLR   : %d / %d" % (snap["iwdg_pr"], snap["iwdg_rlr"]))
    w = swd_read(RCC_CSR, 4)
    if w:
        snap["csr"] = w[0]
        print("  RCC_CSR       : 0x%08X" % snap["csr"])

    fails, warns = evaluate(snap)

    print("-" * 68)
    for f in fails:
        print("  [FAIL] %s" % f)
    for x in warns:
        print("  [注意] %s" % x)
    if fails:
        print("  结论：**未通过**（先解决上面 [FAIL] 行）")
        return 1
    print("  结论：**通过**（判据全过；%s）"
          % ("有注意事项，见上" if warns else "无注意事项"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
