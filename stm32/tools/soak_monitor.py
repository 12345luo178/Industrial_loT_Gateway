#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
72 小时浸泡测试的自动巡检：定时读运行态 -> 写 CSV + 发现异常立刻标出来

============================================================================
 为什么需要它（而不是"三天后看一眼串口"）
============================================================================

  浸泡测试的价值**全在增量证据**上：
    · 中间发生过 1 次复位 -> 只看首尾两个点是看不出来的
    · Flash 磨损 -> 只有"元数据 seq 有没有变"能看出来（串口什么都不打）
    · 错误计数缓慢增长 -> 只有采样序列能看出来

  所以它每 N 秒采一个点，写 CSV，并对每一列给出**明确的异常判据**。

  ⚠️ 读运行态走 **SWD（mode=hotplug）**，**不占 RS485 总线** ——
     否则巡检本身会和板子自己的主站轮询撞车，把被测对象污染了
     （交接文档 §四 的老教训：pc_slave_sim 就是这么把主站判成离线的）。

============================================================================
 每次采样读什么、异常判据是什么
============================================================================

  项              地址/来源            异常判据
  --------------  ------------------  ------------------------------------------
  运行秒数        input[12]           **必须单调递增**；倒退 = 中间复位过
  时基            uwTick @0x2000026C  采到的值应该一直在动
  复位标志        RCC_CSR @0x40021024 bit25~31 任何一位 = 发生过**非预期**复位
                                      （bit24 是 RMVF，bit1 是 LSIRDY，都不算）
  看门狗          IWDG PR/RLR         应当恒为 PR=5 / RLR=999（配置没丢）
                                      PR=5 即 /128 分频 -> 窗口 3.2s（不是 1.6s）
  元数据两页 seq  0x08008000/0x08008800  pending=0 时**必须一字不变**（零擦写）
  主站/错误计数   input[15]/[26]      持续增长要人看（不自动判死，写进 CSV）

  ★ 2026-10-13 新增：缺陷 AD 回归 + 逐任务心跳（RTOS 专用）
  --------------  ------------------  ------------------------------------------
  喂狗 / 拒喂     s_feed_ok /          **s_feed_reject 必须恒为 0**。
                  s_feed_reject        它非零曾经是缺陷 AD（判据假阳性 0.2%），
                                      修好后应为 0；一旦再涨就是 AD 回归。
  逐任务心跳      s_sched（整块 384B） 每个已启动未挂起的任务，age = now - last_beat
                                      必须 <= period_ms + 1000；超了 = 真卡了。
  任务状态        s_sched.state[]     FAULT(4) = 出过错；SUSPEND(3)/IDLE(0) 会被判据跳过
  调度时基        xTickCount          ⚠️ 这才是喂狗判据用的 now，**不是 uwTick**：
                                      两个都是 1kHz 但是**两个独立计数器**
                                      （SysTick 只调 xTaskIncrementTick()，
                                        HAL 的 uwTick 由 TIM4 驱动）。

  ⚠️ age 里含约 0.2~0.4 秒的 SWD 读取延迟（先读 sched 块、后读 xTickCount，
     所以 age 是**上界**）。健康任务 age 只有几十毫秒，离 1000ms 阈值很远，
     这个偏差不会造成误报；真卡住的任务 age 会涨到几千毫秒。

  ⚠️ 采样间隔别太短：SWD 一次往返 1~2 秒，两个数据块加起来约 4 秒。
     默认 300 秒（5 分钟）；72 小时约 864 个采样点。

============================================================================
 用法
============================================================================

    python tools/soak_monitor.py                        # 每 5 分钟一次，一直跑
    python tools/soak_monitor.py --hours 72             # 跑满 72 小时自动收尾
    python tools/soak_monitor.py --interval-s 60        # 加密采样
    python tools/soak_monitor.py --csv my_soak.csv      # 指定输出

  ⚠️ 重新编译固件后 `g_regs` 的地址会变（BSS 整体后移），要用 nm 重取：
        arm-none-eabi-nm -S build\\ota\\app_a.elf | Select-String ' g_regs$'
    本工具默认同时读**两个**同名候选地址，并挑"运行秒数 > 0"的那张活表
    （交接文档 §四 里那条判据 —— 重名符号踩过一次坑）。
============================================================================
"""
import argparse
import csv
import os
import subprocess
import sys
import time

# ⚠️ 中文 Windows 的控制台是 GBK，而 Python 按控制台代码页编码 stdout：
#    print 里出现 ⚠️/✅ 就会 UnicodeEncodeError，**脚本在干活之前就死了**。
#    （ci.ps1 里设了 PYTHONIOENCODING=utf-8，所以 CI 全绿、完全看不出来。）
#    这里把编不出来的字符降级成 '?'，而不是让整个脚本崩掉。
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(errors="replace")
    except (AttributeError, ValueError):
        pass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from verify_can_dual import (swd_read, words_to_regs,      # noqa: E402
                             nm_g_regs, nm_symbol, DEFAULT_ELF, guard_image)

ELF         = DEFAULT_ELF      # 由 --elf 覆盖（RTOS 浸泡必须指向新镜像）

UW_TICK_FALLBACK = 0x2000026C   # ⚠️ 只是 L0 版的实测值，**不要依赖它**（见下）
UW_TICK     = None              # 由 resolve_uw_tick() 从 ELF 现取后填入
_uw_note    = ""
_uw_done    = False


def resolve_uw_tick(elf):
    """从 ELF 现取 uwTick 地址（**不用过时常量兜底**）

    2026-10-11 实测：L0 版是 0x2000026C，**加了 FreeRTOS 之后变成 0x20000274** ——
    地址写死的后果是"静默读错内存"，CSV 里那一列全是垃圾却不报错。
    取不到时宁可**跳过该列**（置 None），也不写垃圾进证据里。
    """
    global UW_TICK, _uw_note, _uw_done
    if _uw_done:
        return UW_TICK
    _uw_done = True
    a = nm_symbol(elf, "uwTick")
    if a:
        UW_TICK = a
        if a != UW_TICK_FALLBACK:
            _uw_note = ("uwTick 从 ELF 现取 = 0x%08X（工具里那个内置常量 0x%08X 已过时 —— "
                        "这正是本次修掉的坑）" % (a, UW_TICK_FALLBACK))
        else:
            _uw_note = "uwTick 从 ELF 现取 = 0x%08X" % a
    else:
        UW_TICK = None
        _uw_note = ("⚠️ 从 ELF 取不到 uwTick —— 本列置空（宁缺勿假）")
    print(" [soak] " + _uw_note)
    return UW_TICK
RCC_CSR     = 0x40021024
IWDG_BASE   = 0x40003000
META_ADDR   = 0x08008000
PAGE_SIZE   = 0x800
GREGS_LEN   = 0x70          # input[0..39] + holding[0..15]

# ⚠️ 重新编译后 `g_regs` 地址会变（BSS 整体后移），所以**优先从 ELF 现取**：
#    `verify_can_dual.DEFAULT_ELF` 会优先指向槽位化的 `build/ota/app_a.elf`
#    （板上真正在跑的那份），取不到才退回下面这份内置候选。
#    —— 这不只是省事：2026-10-11 就是因为工具里留着旧地址，
#       一度以为"板子卡死了"，其实是读错了内存。
def default_gregs():
    try:
        addrs = nm_g_regs(ELF)
    except Exception:
        addrs = []
    if addrs:
        return addrs, ELF        # ⚠️ 必须返回 ELF 而不是 DEFAULT_ELF（否则守卫查的是旧镜像）
    return [0x200059D0, 0x20000E04], "(内置候选，未从 ELF 取到)"

# RCC_CSR：bit24=RMVF（不是复位标志）、bit1=LSIRDY（狗用的 LSI，正常）。
# 真正的"复位类型"是 bit25~31。
CSR_RESET_MASK = 0xFE000000

# ⚠️ 期望值必须与固件**真正写进 IWDG_PR 的那个值**一致。
#   HAL 的 IWDG_PRESCALER_x 恰好是 0..6：_4=0, _8=1, _16=2, _32=3,
#   _64=4, _128=5, _256=6。
#
#   ⚠️ 缺陷 AK（2026-10-13）：这里原来写 4（= /64，超时 1.6s），
#      但固件 2026-10-12 就把窗口改成 /128（= 5，超时 3.2s）了 ——
#      于是**每个采样点都误报 "IWDG 配置异常"**，72 小时浸泡会被污染成
#      "永远 bad"，那判据就废了。
#      （同一个不一致还让固件里那个 _Static_assert 自检静默失效 ——
#        它检查的是宏 IWDG_PRESCALER_DIV，而不是真正赋给 Prescaler 的常量。
#        已在 gateway2/Core/Src/iwdg.c 里改成共用唯一真源。）
IWDG_EXPECT_PR  = 5         # = IWDG_PRESCALER_128 -> 分频 128 -> 3.2s（LSI 标称 40kHz）
IWDG_EXPECT_RLR = 999

# ============================================================================
#  ★ 缺陷 AD 回归 + 逐任务心跳（2026-10-13 加）
#
#  为什么加：72 小时浸泡如果不记这两样，就**又一次**只能看到
#  "s_feed_reject 在涨"，却不知道是哪个任务 —— 缺陷 AD 就是这么被拖了很久的。
#
#  ⚠️ s_feed_ok / s_feed_reject / s_sched 是 gw_rtos.c 里的 `static`，
#     但它们**在符号表里**（nm 类型是小写 b），所以能像 g_regs 一样从 ELF 现取。
#     地址写死会静默读错内存（重编译后 BSS 整体后移）—— 本文件已经踩过一次。
# ============================================================================
TASK_NAMES = ["SLAVE", "MASTER", "CAN", "UPLINK", "SENSOR", "HOUSE", "UI"]

# ---- gw_sched_t 内部偏移（从 ELF 的 DWARF 取，**不是手算**）----
#  手算会错：state[] 是**1 字节**枚举（构建实际是 short-enums 语义），
#  按 4 字节算会把后面所有字段都推偏，而 struct 总大小 0x180 恰好只有
#  1 字节枚举那一种算法对得上。权威值用
#      arm-none-eabi-objdump --dwarf=info app_a.elf | grep -A4 'gw_sched_t'
#  核对过：tasks 0 / stat 112 / state 280 / due_ms 288 / progress 316 /
#          now_ms 320 / now_ms_ctx 324 / last_beat_ms 328 / beat_seen 356 /
#          wd_ref_ms 364 / loops 368 / now_us 372 / time_ctx 376 / inited 380
SCHED_SIZE   = 384          # sizeof(gw_sched_t)
SCHED_INITED = 380
O_STATE      = 280
O_PROGRESS   = 316
O_LAST_BEAT  = 328
O_BEAT_SEEN  = 356
O_WD_REF     = 364
O_LOOPS      = 368
DEF_STRIDE   = 16           # sizeof(gw_task_def_t)
DEF_PERIOD   = 8            # offsetof(gw_task_def_t, period_ms)

GW_SCHED_WD_SLACK_MS = 1000  # inc/gw_sched.h：判据的余量

STATE_NAME = {0: "IDLE", 1: "READY", 2: "RUNNING", 3: "SUSPEND", 4: "FAULT"}


class SchedSyms(object):
    """从 ELF 现取 AD 诊断所需的符号；取不全就整体降级（宁缺勿假）。"""

    def __init__(self, elf):
        self.feed_ok  = nm_symbol(elf, "s_feed_ok")
        self.feed_rej = nm_symbol(elf, "s_feed_reject")
        self.sched    = nm_symbol(elf, "s_sched")
        self.tick     = nm_symbol(elf, "xTickCount")   # ★ FreeRTOS 的 tick
        self.missing  = [n for n, a in (("s_feed_ok", self.feed_ok),
                                        ("s_feed_reject", self.feed_rej),
                                        ("s_sched", self.sched),
                                        ("xTickCount", self.tick)) if not a]
        self.ok = not self.missing

    def describe(self):
        if self.ok:
            return ("s_feed_ok=0x%08X s_feed_reject=0x%08X s_sched=0x%08X "
                    "xTickCount=0x%08X"
                    % (self.feed_ok, self.feed_rej, self.sched, self.tick))
        return ("⚠️ 取不到 %s —— AD 回归与逐任务心跳**整块跳过**（宁缺勿假）。"
                "多半是 --elf 指向了旧镜像。" % ", ".join(self.missing))


SYMS = None                 # 由 main() 填


def u32(buf, off):
    return int.from_bytes(buf[off:off + 4], "little")


def u16(buf, off):
    return int.from_bytes(buf[off:off + 2], "little")


def read_sched_block():
    """读 s_feed_reject / s_feed_ok / 整个 s_sched。

    ⚠️ 这三者在内存里是**连续**的（reject, ok, sched），所以一次 SWD 读
       拿到的就是"同一瞬间"的快照 —— 分开读会被 SWD 的几百毫秒延迟错开。
    """
    if SYMS is None or not SYMS.ok:
        return None
    w = swd_read(SYMS.feed_rej, 8 + SCHED_SIZE)
    if not w or len(w) < (8 + SCHED_SIZE) // 4:
        return None
    buf = b"".join(x.to_bytes(4, "little") for x in w)
    return {
        "feed_reject": u32(buf, 0),
        "feed_ok":     u32(buf, 4),
        "sched":       buf[8:],
    }


def read_inputs(gregs_candidates):
    """读活表 g_regs，返回 (地址, 40 个输入寄存器, 16 个保持寄存器)"""
    best = None
    for addr in gregs_candidates:
        words = swd_read(addr, GREGS_LEN)
        if not words:
            continue
        regs = words_to_regs(words)
        # 判据（交接文档 §四）：运行秒数 > 0 的那张才是活表
        if len(regs) >= 13:
            uptime = (regs[11] << 16) | regs[12]
            if uptime > 0:
                return addr, regs[:40], regs[40:56]
        if best is None:
            best = (addr, regs[:40], regs[40:56])
    return best if best else (None, None, None)


def read_meta_seqs():
    """只取两页的 seq / active / pending / boot_tries（浸泡只关心"变没变"）"""
    out = []
    for page in range(2):
        words = swd_read(META_ADDR + page * PAGE_SIZE, 40)
        if not words:
            out.append(None)
            continue
        raw = b"".join(w.to_bytes(4, "little") for w in words)[:40]
        magic = int.from_bytes(raw[0:4], "little")
        if magic != 0x4F544130:                 # "OTA0"
            out.append({"valid_magic": False})
            continue
        out.append({
            "valid_magic": True,
            "seq":         int.from_bytes(raw[8:12], "little"),
            "active":      raw[13],
            "pending":     raw[14],
            "boot_tries":  raw[15],
        })
    return out


def sample(gregs_candidates):
    """采一个点，返回 dict（读不到就填 None，不抛异常 —— 浸泡脚本要能扛住偶发读失败）"""
    s = {}
    addr, inregs, holdregs = read_inputs(gregs_candidates)
    s["gregs_addr"] = "0x%08X" % addr if addr else ""
    # ⚠️ RTOS 下 g_regs 是"按需刷新"的（从站应答前才刷），空闲时是初始化
    #    快照 -> uptime 恒 0。真正的时基是 s_uptime_ms（不受请求影响）。
    _up_ms = nm_symbol(ELF, "s_uptime_ms")
    _w = swd_read(_up_ms, 4) if _up_ms else None
    s["uptime_s"] = (_w[0] // 1000) if _w else None

    # ---- 活表里的输入寄存器（温度/湿度/状态位/各类计数）----
    #
    # ⚠️⚠️ 2026-10-16 修一个"静默失效"的 bug：这一段原先写成
    #       if inregs and s["uptime_s"] is None:   <-- 用"时基缺席"当开关
    #         ...读 13 列...
    #       else:
    #         ...把 13 列全置 None...
    #     但上面那句 `s["uptime_s"] = (_w[0] // 1000) if _w else None` 是**无条件**执行的，
    #     而 `s_uptime_ms` 这个符号**总是读得到** ⇒ 判据基本永远为假 ⇒ **永远走 else**
    #     ⇒ `temp_x10`/`humi_x10`/`dev_status`/`mb_*`/`can_rx`/`master_*`/`tm_*`
    #     共 13 列**在每一行 CSV 里都是空的**，控制台还打出 `dev=0x0000`（真值是 0x000F）。
    #     后果：作者特意保留这些列是为了"持续增长要人看"，实际上根本没人能看到 ——
    #     一个只在事后翻 CSV 才会发现的静默失效（2026-10-16 因用户比对湿度才发现）。
    #
    #     修法：**把两件事解耦**。
    #       · 时基优先级不变（符号 > 活表，最后仍被 uw_tick 覆盖）
    #       · 这 13 列**只要有活表就填**，不再拿"时基缺席"当开关
    #     另加一条护栏：读到的温度/湿度若明显越界，置 None 而不是写进 CSV
    #     ——否则"读表失败但恰好凑出个数"会被当成真数据（本项目最怕假数据）。
    _reg_uptime = None
    if inregs and len(inregs) >= 40:
        _reg_uptime       = (inregs[11] << 16) | inregs[12]
        s["dev_status"]   = inregs[10]
        s["mb_rx"]        = (inregs[14] << 16) | inregs[13]
        s["mb_err"]       = (inregs[16] << 16) | inregs[15]
        s["can_rx"]       = (inregs[18] << 16) | inregs[17]
        s["mb_exc"]       = (inregs[20] << 16) | inregs[19]
        s["master_online"] = inregs[26]
        s["master_err"]   = inregs[27]

        # ---- 计时插桩的哨兵值必须翻译成 None（2026-10-16）----
        #
        # ⚠️ `gw_timing_port.c` 用三个哨兵值表达"无法给出真值"，而它们**长得像正常数**：
        #      0xFFFE (65534) = gw_timing_port 未初始化（s_tm == 0）
        #      0xFFFD (65533) = 该相位一次都没调用过
        #      0xFFFF (65535) = 真值超过 65534，被钳住
        #    本工程**没有**调用 `gw_timing_port_init()`（全仓库无调用点），
        #    所以这四列实测恒为 0xFFFE。若原样写进 CSV，
        #    "tm_load_permil = 65534" 会被当成"CPU 负载 6553.4%"这种荒谬读数，
        #    而 65533/65535 同理会看错。**空着比写假数好** —— 与本项目"宁缺勿假"一致。
        for _key, _idx in (("tm_slave_max", 36), ("tm_loop_max", 38),
                           ("tm_load_permil", 39)):
            _v = inregs[_idx]
            s[_key] = None if _v in (0xFFFD, 0xFFFE, 0xFFFF) else _v

        # 温度是有符号（int16，负温用补码），湿度是无符号
        _t = inregs[0]
        if _t >= 0x8000:
            _t -= 0x10000
        if -400 <= _t <= 800:          # 与 gw_selfcheck_port.c 同样的合理区间
            s["temp_x10"] = _t
        else:
            s["temp_x10"] = None
        s["humi_x10"] = inregs[1] if inregs[1] <= 1000 else None
    else:
        for k in ("dev_status", "temp_x10", "humi_x10", "mb_rx",
                  "mb_err", "can_rx", "mb_exc", "master_online", "master_err",
                  "tm_slave_max", "tm_loop_max", "tm_load_permil"):
            s[k] = None

    # 时基退回：只有在 s_uptime_ms 读不到时才用活表的运行秒数
    if s["uptime_s"] is None and _reg_uptime is not None:
        s["uptime_s"] = _reg_uptime

    if not _uw_done:
        resolve_uw_tick(ELF)
    w = swd_read(UW_TICK, 4) if UW_TICK else None
    s["uw_tick"] = w[0] if w else None

    # ⚠️ uptime 最终以 **uw_tick** 为准（2026-10-11 实测）：它由 TIM4 中断驱动、
    #    与任何应用逻辑无关，是这个系统里最可靠的时基；而 s_uptime_ms 偶发读失败、
    #    g_regs 在 RTOS 下是"按需刷新"的快照（空闲时为 0）。
    if s.get("uw_tick") is not None:
        s["uptime_s"] = s["uw_tick"] // 1000

    w = swd_read(RCC_CSR, 4)
    s["rcc_csr"] = w[0] if w else None

    w = swd_read(IWDG_BASE, 16)
    if w and len(w) >= 3:
        s["iwdg_pr"]  = w[1]
        s["iwdg_rlr"] = w[2]
    else:
        s["iwdg_pr"] = s["iwdg_rlr"] = None

    meta = read_meta_seqs()
    for i, m in enumerate(meta):
        s["seq%d" % i] = m.get("seq") if m else None
        s["magic%d" % i] = (1 if m and m.get("valid_magic") else 0) if m else None
    newest = None
    for m in meta:
        if m and m.get("valid_magic"):
            if newest is None or m["seq"] > newest["seq"]:
                newest = m
    s["active"]     = newest["active"] if newest else None
    s["pending"]    = newest["pending"] if newest else None
    s["boot_tries"] = newest["boot_tries"] if newest else None

    # ---- ★ 缺陷 AD 回归 + 逐任务心跳 ----
    #  ⚠️ 顺序：先读 sched 块，后读 xTickCount。这样算出的 age 是**上界**
    #     （含 SWD 读取延迟），偏保守 —— 宁可多报一次"可能迟到"，也不要漏。
    #     健康任务 age 只有几十毫秒，离 1000ms 阈值很远，不会误报。
    sb = read_sched_block()
    s["feed_ok"] = s["feed_reject"] = None
    s["sched_ms"] = s["sched_progress"] = s["sched_loops"] = None
    for i in range(7):
        s["st%d" % i] = s["age%d" % i] = None
        s["lim%d" % i] = None
    if sb is not None:
        s["feed_ok"]     = sb["feed_ok"]
        s["feed_reject"] = sb["feed_reject"]
        sch = sb["sched"]
        w = swd_read(SYMS.tick, 4)
        now = w[0] if w else None
        s["sched_ms"]       = now
        s["sched_progress"] = u32(sch, O_PROGRESS)
        s["sched_loops"]    = u32(sch, O_LOOPS)
        for i in range(7):
            st  = sch[O_STATE + i]
            per = u16(sch, DEF_STRIDE * i + DEF_PERIOD)
            beat = u32(sch, O_LAST_BEAT + 4 * i)
            s["st%d" % i]  = st
            s["lim%d" % i] = per + GW_SCHED_WD_SLACK_MS
            if now is None:
                s["age%d" % i] = None
            else:
                age = (now - beat) & 0xFFFFFFFF
                if age > 0x7FFFFFFF:            # 回绕 -> 负数（理论上修好后不该出现）
                    age -= 0x100000000
                s["age%d" % i] = age
    return s


CSV_COLS = [
    "t_wall", "t_elapsed_s", "uptime_s", "uw_tick", "rcc_csr",
    "iwdg_pr", "iwdg_rlr", "seq0", "seq1", "active", "pending", "boot_tries",
    "dev_status", "temp_x10", "humi_x10", "mb_rx", "mb_err", "can_rx", "mb_exc",
    "master_online", "master_err", "tm_slave_max", "tm_loop_max",
    "tm_load_permil", "gregs_addr",
    # ---- ★ AD 回归 + 逐任务心跳（2026-10-13 加）----
    "feed_ok", "feed_reject", "sched_ms", "sched_progress", "sched_loops",
    "st0", "st1", "st2", "st3", "st4", "st5", "st6",
    "age0", "age1", "age2", "age3", "age4", "age5", "age6",
    "notes",
]


def verdicts(s, prev):
    """对本次采样给判据；返回 (notes_list, bad)"""
    notes = []
    bad = False

    if s["uptime_s"] is None:
        notes.append("读不到活表")
        return notes, True

    if prev is not None and prev.get("uptime_s") is not None:
        if s["uptime_s"] < prev["uptime_s"]:
            notes.append("复位!(uptime %s->%s)" % (prev["uptime_s"], s["uptime_s"]))
            bad = True
    if s["rcc_csr"] is not None and (s["rcc_csr"] & CSR_RESET_MASK):
        notes.append("复位标志=0x%08X" % s["rcc_csr"])
        bad = True
    if (s["iwdg_pr"] is not None
            and (s["iwdg_pr"] != IWDG_EXPECT_PR
                 or s["iwdg_rlr"] != IWDG_EXPECT_RLR)):
        notes.append("IWDG 配置异常 PR=%s RLR=%s" % (s["iwdg_pr"], s["iwdg_rlr"]))
        bad = True
    if prev is not None and s["pending"] == 0 and prev.get("pending") == 0:
        if (s["seq0"] != prev.get("seq0")) or (s["seq1"] != prev.get("seq1")):
            notes.append("Flash 写盘!(seq %s/%s -> %s/%s)"
                         % (prev.get("seq0"), prev.get("seq1"),
                            s["seq0"], s["seq1"]))
            bad = True

    # ---- ★ 缺陷 AD 回归检查：拒喂必须恒为 0 ----
    #  拒喂曾经是判据的假阳性（0.2%）：判据先取 now、后读 last_beat_ms，
    #  中间被抢占时任务登记了心跳 -> base 比 now 新 -> 无符号减法回绕 -> 假拒喂。
    #  2026-10-13 已修（先读 base 后读 now）。修好后这里**必须一直是 0**。
    if s.get("feed_reject") is not None:
        if prev is not None and prev.get("feed_reject") is not None:
            d = s["feed_reject"] - prev["feed_reject"]
            if d > 0:
                notes.append("拒喂 +%d (%d->%d) —— 缺陷 AD 回归!"
                             % (d, prev["feed_reject"], s["feed_reject"]))
                bad = True
        elif s["feed_reject"] != 0:
            notes.append("拒喂=%d(非零)" % s["feed_reject"])
            bad = True

    # ---- ★ 逐任务状态与心跳年龄 ----
    for i in range(7):
        st = s.get("st%d" % i)
        if st is None:
            continue
        name = TASK_NAMES[i]
        if st == 4:                       # GW_TASK_ST_FAULT
            notes.append("%s=FAULT" % name)
            bad = True
        if st in (0, 3):                  # IDLE/SUSPEND：判据会跳过它
            if prev is None or prev.get("st%d" % i) != st:
                notes.append("%s=%s(判据跳过)" % (name, STATE_NAME.get(st, st)))
            continue
        age = s.get("age%d" % i)
        lim = s.get("lim%d" % i)
        if age is not None and lim is not None and age > lim:
            notes.append("%s 心跳迟到 age=%d>%d" % (name, age, lim))
            bad = True
    return notes, bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--interval-s", type=float, default=300.0,
                    help="采样间隔秒（默认 300 = 5 分钟）")
    ap.add_argument("--hours", type=float, default=0.0,
                    help="跑满多少小时自动收尾（默认 0 = 一直跑）")
    ap.add_argument("--csv", default="soak.csv", help="CSV 输出路径")
    ap.add_argument("--elf", default=DEFAULT_ELF,
                    help="镜像 ELF（默认 build/ota/app_a.elf；烧了 RTOS 镜像就用 build/landing/app_a.elf）")
    ap.add_argument("--gregs", action="append", type=lambda x: int(x, 0),
                    default=None,
                    help="g_regs 地址（可重复；默认从 ELF 自动取，取不到用内置候选）")
    args = ap.parse_args()

    global ELF               # ⚠️ 必须声明 global：否则 ELF 变成 main 的局部变量，
                             #    而 default_gregs()/采样函数读的是模块全局（踩过）
    global SYMS
    ELF = args.elf          # ★ 让 g_regs/守卫/时基都用这份 ELF
    SYMS = SchedSyms(ELF)
    auto_gregs, elf_used = default_gregs()
    gregs = args.gregs if args.gregs else auto_gregs
    stop_at = time.time() + args.hours * 3600 if args.hours > 0 else None

    print("=" * 66)
    print(" 浸泡巡检开始：每 %.0f 秒一个采样点%s"
          % (args.interval_s,
             ("，跑满 %.1f 小时" % args.hours) if args.hours > 0 else "（一直跑）"))
    print(" 输出：%s" % os.path.abspath(args.csv))
    print(" g_regs 候选：%s" % ", ".join("0x%08X" % a for a in gregs))
    print(" 符号来源：%s" % elf_used)
    print(" AD 诊断符号：%s" % SYMS.describe())
    print(" ⚠️ 判据：uptime 单调增 / RCC_CSR 无复位位 / IWDG=%d,%d / pending=0 时 seq 不变"
          % (IWDG_EXPECT_PR, IWDG_EXPECT_RLR))
    print(" ⚠️ 新增：s_feed_reject 必须恒为 0（缺陷 AD 回归）；各任务 age <= period+1000")
    print("=" * 66)

    # ★ 先核对镜像一致性：板子上跑的必须是这份 ELF，否则下面的读数全是"别的变量"
    #  （浸泡期间板子不该被重烧；真被烧了，这条路会**拒绝启动**并说清原因）
    if not guard_image(elf_used):
        print("  拒绝启动：镜像不一致时记录的采样点没有意义。")
        print("  如果确实刚烧过新固件，请用新 ELF 重新起巡检（并换一个 CSV）。")
        return 2

    new_file = not os.path.exists(args.csv)
    f = open(args.csv, "a", newline="", encoding="utf-8")
    wr = csv.DictWriter(f, fieldnames=CSV_COLS)
    if new_file:
        wr.writeheader()

    t0 = time.time()
    prev = None
    n = 0
    bad_n = 0
    wrote_seq = False

    try:
        while True:
            s = sample(gregs)
            notes, bad = verdicts(s, prev)
            if bad:
                bad_n += 1
            s["t_wall"] = time.strftime("%Y-%m-%d %H:%M:%S")
            s["t_elapsed_s"] = int(time.time() - t0)
            s["notes"] = ";".join(notes)
            wr.writerow({k: s.get(k, "") for k in CSV_COLS})
            f.flush()
            n += 1

            # ---- ★ AD 回归 + 最老心跳（一行里给出最该看的两样）----
            extra = ""
            if s.get("feed_reject") is not None:
                extra += " rej=%d" % s["feed_reject"]
            worst_i, worst_age = None, None
            for i in range(7):
                a = s.get("age%d" % i)
                if a is None or s.get("st%d" % i) in (0, 3):
                    continue
                if worst_age is None or a > worst_age:
                    worst_i, worst_age = i, a
            if worst_i is not None:
                extra += " 最老=%s(%dms/%dms)" % (TASK_NAMES[worst_i], worst_age,
                                                  s.get("lim%d" % worst_i) or 0)

            print("[%s] #%d up=%ss uwTick=%s csr=%s IWDG=%s/%s seq=%s/%s "
                  "pend=%s dev=0x%04X%s%s"
                  % (s["t_wall"], n, s["uptime_s"], s["uw_tick"],
                     ("0x%08X" % s["rcc_csr"]) if s["rcc_csr"] is not None else "?",
                     s["iwdg_pr"], s["iwdg_rlr"], s["seq0"], s["seq1"],
                     s["pending"],
                     s["dev_status"] if s["dev_status"] is not None else 0,
                     extra,
                     ("  <<< " + ";".join(notes)) if notes else ""))

            prev = s
            if stop_at is not None and time.time() >= stop_at:
                break
            time.sleep(args.interval_s)
    except KeyboardInterrupt:
        print("\n收到中断，收尾…")
    finally:
        f.close()

    print("=" * 66)
    print(" 采样 %d 个点，异常 %d 个%s"
          % (n, bad_n, "，CSV 已写出" if n else ""))
    print(" ⚠️ 只要 bad == 0 且 uptime 单调增长到 72 小时，就算浸泡通过。")
    print("=" * 66)
    return 0


if __name__ == "__main__":
    sys.exit(main())
