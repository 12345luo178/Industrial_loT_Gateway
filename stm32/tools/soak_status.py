# -*- coding: utf-8 -*-
"""
浸泡进度与健康摘要：把 864 行的 CSV 变成"一眼能看"的几行（只读 CSV，不碰板子）

============================================================================
 为什么需要它（而不是"三天后看 CSV"）
============================================================================
 tools/soak_monitor.py 每 300 s 写一行 CSV，并在行内 `notes` 列标出异常。
 72 小时会有约 864 行 —— 中途想确认"还健康吗"时，翻 CSV 不现实。

 本脚本只做**呈现**，不做新判据（判据在 soak_monitor.py 里，单一真源）：
   * 进度：已采多少点 / 覆盖多久 / 还差多少 / 预计何时结束
   * 覆盖连续性：uptime 是否单调递增（倒退 = 中间复位过）
   * 健康：异常行数、s_feed_reject 最大值、RCC_CSR 复位位、
           IWDG 配置是否漂、元数据两页 seq 是否变、最老任务心跳
   * 收尾判据：是否真的连续覆盖了 259200 s（72 h）

 ⚠️ 本脚本**只读 CSV**，不连 STM32、不碰 SWD —— 浸泡期间可以随时跑。
============================================================================
 用法
============================================================================
    python tools/soak_status.py                      # 自动找最新的 soak_*.csv
    python tools/soak_status.py --csv soak_lvgl_2026-10-15.csv
    python tools/soak_status.py --hours 72           # 指定目标时长（默认 72）
    python tools/soak_status.py --selftest           # 自检解析逻辑
============================================================================
"""
import argparse
import csv
import glob
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.dirname(_HERE)

# 判据期望值（与 soak_monitor.py 的 IWDG_EXPECT_* 同源；这里只读取展示）
IWDG_EXPECT_PR = 5
IWDG_EXPECT_RLR = 999

# 逐任务心跳的"保守"阈值（仅用于**呈现**，不替代 soak_monitor 的判据）：
#   soak_monitor 的判据是 age <= period_ms + 1000，而 period_ms 是从 ELF 的 DWARF
#   现取的、**没有写进 CSV**（它只写 st*/age*）。所以本脚本无法从 CSV 重算精确限值。
#   本项目所有任务的 period_ms 都远小于 50 ms ⇒ 精确限值 >= 1050 ms。
#   取 1050 作阈值：**超过它必定违规**（不会漏报），但可能对 period 较大的任务偏紧。
#   注意：真正违反判据的行，soak_monitor 已经把结论写进 notes 了 —— 那种情况
#   会在上面的"异常行"里体现，不依赖这个阈值。
AGE_ALERT_MS = 1050

# 挂钟与 t_elapsed_s 跨度的允许偏差（秒）。两者都来自挂钟，只该差一个固定偏移；
# 超过这个值就提示"PC 时钟可能被校正过"——因为 soak_monitor 的 --hours 停止判据
# 用的是挂钟 time.time()，时钟被校正会影响"是否真的跑满 72h"。
# 取样间隔 300 s，正常抖动在个位数秒；给 60 s 既不会误报也不会漏掉明显校正。
CLOCK_DRIFT_ALERT_S = 60

# CSV 里的任务名列（与 soak_monitor 的 TASK_NAMES 同序）
TASK_NAMES = ["SLAVE", "MASTER", "CAN", "UPLINK", "SENSOR", "HOUSE", "UI"]


def find_latest_csv():
    pats = [os.path.join(_REPO, "soak_*.csv"),
            os.path.join(_REPO, "build", "archive", "soak_*.csv")]
    files = []
    for p in pats:
        files += glob.glob(p)
    if not files:
        return None
    return max(files, key=os.path.getmtime)


def _int(row, key):
    """
    取一个整数列。

    ⚠️ `soak_monitor.py` 用 `csv.DictWriter` 写的是 **Python int 的十进制表示**
       （实测 `rcc_csr` 写作 `3`，不是 `0x00000003`）。但中文文档里习惯把
       `RCC_CSR` 写成 `0x…`，将来手工造/核对数据时很可能照抄成十六进制，
       所以这里顺带接受 `0x` 前缀 —— 比"只认十进制"少一次踩坑。
    """
    v = (row.get(key) or "").strip()
    if v == "":
        return None
    try:
        if v.lower().startswith("0x"):
            return int(v, 16)
        f = float(v)
        # 十进制整数字符串走这条；'3.0' 之类也容忍
        return int(f)
    except ValueError:
        return None


def load(csv_path):
    """返回 (rows, header)。rows 是 dict 列表。"""
    with open(csv_path, "r", encoding="utf-8", newline="") as f:
        rd = csv.DictReader(f)
        return list(rd), rd.fieldnames or []


def _wall_span_s(first, last):
    """
    (首行 t_wall) -> (末行 t_wall) 的挂钟跨度，单位秒；解析不了返回 None。

    用途：与 `t_elapsed_s` 的跨度对比。后者是 `time.time() - t0`（**挂钟**），
    前者也来自挂钟，所以两者天然只差一个固定偏移；一旦**明显发散**，
    说明采样期间挂钟被校正过（Windows NTP 对时）。

    ⚠️ 为什么关心：`soak_monitor` 的 `--hours` 停止判据用的是 `time.time()`。
       挂钟被回拨会让浸泡**提前**结束、被前拨会**拖长**，两者都不体现在
       `uptime_s` 里（那是 STM32 自己的、不受 PC 时钟影响）。
       所以这是"验收时长是否可靠"的**观测判据**，不是对板子的判据。
    """
    fmt = "%Y-%m-%d %H:%M:%S"
    try:
        t0 = time.mktime(time.strptime(first, fmt))
        t1 = time.mktime(time.strptime(last, fmt))
    except (ValueError, TypeError):
        return None
    return t1 - t0


def summarize(rows, fieldnames, target_hours):
    """把行集合压成一份摘要字典。**不做新判据**，只汇总已有的列。"""
    s = {
        "n": len(rows),
        "first_wall": None, "last_wall": None,
        "uptime_first": None, "uptime_last": None, "uptime_max": None,
        "monotonic": True, "regressions": 0,
        "bad_rows": 0, "bad_examples": [],
        "feed_reject_max": None,
        "csr_bad": 0,
        "iwdg_bad": 0,
        "seq_changed": False,
        "worst_age": None, "worst_task": None,
        "wall_span": None, "clock_drift": None,
        "target_s": int(target_hours * 3600),
    }
    if not rows:
        return s

    prev_uptime = None
    for r in rows:
        w = (r.get("t_wall") or "").strip()
        if w:
            if s["first_wall"] is None:
                s["first_wall"] = w
            s["last_wall"] = w

        up = _int(r, "uptime_s")
        if up is not None:
            if s["uptime_first"] is None:
                s["uptime_first"] = up
            s["uptime_last"] = up
            if s["uptime_max"] is None or up > s["uptime_max"]:
                s["uptime_max"] = up
            if prev_uptime is not None and up < prev_uptime:
                s["monotonic"] = False
                s["regressions"] += 1
            prev_uptime = up

        notes = (r.get("notes") or "").strip()
        if notes:
            s["bad_rows"] += 1
            if len(s["bad_examples"]) < 5:
                s["bad_examples"].append("%s: %s" % (w, notes))

        fr = _int(r, "feed_reject")
        if fr is not None and (s["feed_reject_max"] is None or fr > s["feed_reject_max"]):
            s["feed_reject_max"] = fr

        csr = _int(r, "rcc_csr")
        if csr is not None and (csr & 0xFE000000) != 0:
            s["csr_bad"] += 1

        pr, rlr = _int(r, "iwdg_pr"), _int(r, "iwdg_rlr")
        if (pr is not None and pr != IWDG_EXPECT_PR) or \
           (rlr is not None and rlr != IWDG_EXPECT_RLR):
            s["iwdg_bad"] += 1

        # 元数据两页 seq：pending=0 时必须一字不变
        if _int(r, "pending") == 0:
            for k in ("seq0", "seq1"):
                v = _int(r, k)
                if v is not None and s.get("_%s_ref" % k) is None:
                    s["_%s_ref" % k] = v
                elif v is not None and v != s.get("_%s_ref" % k):
                    s["seq_changed"] = True

        for i, name in enumerate(TASK_NAMES):
            st = _int(r, "st%d" % i)
            age = _int(r, "age%d" % i)
            if st in (0, 3) or age is None:
                continue
            if s["worst_age"] is None or age > s["worst_age"]:
                s["worst_age"] = age
                s["worst_task"] = name

    # 挂钟 vs t_elapsed_s 的一致性（验收时长是否可靠的观测判据）
    if s["first_wall"] and s["last_wall"]:
        s["wall_span"] = _wall_span_s(s["first_wall"], s["last_wall"])
        first_el = _int(rows[0], "t_elapsed_s")
        last_el = _int(rows[-1], "t_elapsed_s")
        if (s["wall_span"] is not None and first_el is not None
                and last_el is not None):
            s["clock_drift"] = s["wall_span"] - (last_el - first_el)
    return s


def render(s, csv_path, target_hours):
    """打印摘要。用 ASCII 标记，避免 GBK 控制台下 '?' 化（本项目踩过）。"""
    out = []
    A = out.append
    A("=" * 72)
    A(" 浸泡进度与健康摘要")
    A("=" * 72)
    A(" CSV        : %s" % csv_path)
    A(" 采样点     : %d" % s["n"])
    if s["first_wall"]:
        A(" 时间窗     : %s  ->  %s" % (s["first_wall"], s["last_wall"]))
    if s["uptime_first"] is not None:
        cov = s["uptime_last"] - s["uptime_first"]
        A(" uptime 覆盖: %s s -> %s s  (跨度 %d s)"
          % (s["uptime_first"], s["uptime_last"], cov))
        A(" 目标       : %d s (%.1f h)" % (s["target_s"], target_hours))
        pct = 100.0 * cov / s["target_s"] if s["target_s"] else 0.0
        A(" 进度       : %.1f%%" % pct)
        remain = s["target_s"] - cov
        if remain > 0:
            A(" 还差       : %d s (%.1f h)" % (remain, remain / 3600.0))
            if s["n"] >= 2 and cov > 0:
                # 用 CSV 里的挂钟跨度估速率，避免依赖本机时钟与浸泡启动时刻
                A(" 预计还需   : 约 %.1f h（按每 300 s 一点）" % (remain / 3600.0))
        else:
            A(" 覆盖已达标 : 是（>= 目标时长）")
    A("")
    A("-" * 72)
    A(" 健康判据（只汇总，不新增判据）")
    A("-" * 72)
    A(" uptime 单调递增 : %s%s"
      % ("是" if s["monotonic"] else "**否**",
         "" if s["monotonic"] else "  (倒退 %d 次 -> 中间复位过)" % s["regressions"]))
    A(" 异常行 (notes)  : %d%s"
      % (s["bad_rows"], "" if s["bad_rows"] == 0 else "  <-- 看下面例子"))
    for e in s["bad_examples"]:
        A("                   %s" % e)
    A(" feed_reject 最大: %s%s"
      % (s["feed_reject_max"],
         "" if s["feed_reject_max"] in (0, None) else "  ** 非 0 = 缺陷 AD 回归 **"))
    A(" RCC_CSR 异常复位: %d%s"
      % (s["csr_bad"], "" if s["csr_bad"] == 0 else "  ** 非预期复位 **"))
    A(" IWDG 配置漂移   : %d%s"
      % (s["iwdg_bad"],
         "" if s["iwdg_bad"] == 0 else "  ** 期望 PR=%d RLR=%d **"
         % (IWDG_EXPECT_PR, IWDG_EXPECT_RLR)))
    A(" 元数据 seq 变化 : %s"
      % ("**是**（pending=0 时应不变，磨损是唯一会随时间恶化的项）"
         if s["seq_changed"] else "否"))
    if s["clock_drift"] is not None:
        d = s["clock_drift"]
        bad = abs(d) > CLOCK_DRIFT_ALERT_S
        A(" 挂钟/单调一致性 : 差 %+.0f s (阈值 %d s)  %s"
          % (d, CLOCK_DRIFT_ALERT_S, "**疑似挂钟被校正**" if bad else "OK"))
        A("                   说明：t_wall 与 t_elapsed_s 都来自挂钟，只该差一个固定偏移。")
        A("                   明显发散 = PC 时钟被 NTP 校正过；而 --hours 停止判据用的")
        A("                   正是挂钟，所以它会影响『是否真的跑满 72h』这个验收口径。")
    if s["worst_age"] is not None:
        # ⚠️ CSV 里没有精确限值列（见 AGE_ALERT_MS 的说明），这里用保守阈值呈现，
        #    并如实写出"阈值"二字，避免让人误以为那是 soak_monitor 的精确判据。
        ok = "OK" if s["worst_age"] <= AGE_ALERT_MS else "**超保守阈值**"
        A(" 最老任务心跳    : %s age=%d ms (保守阈值 %d ms)  %s"
          % (s["worst_task"], s["worst_age"], AGE_ALERT_MS, ok))
        A("                   注意：精确判据 age<=period+1000 由 soak_monitor 判，")
        A("                   违规会出现在上面的『异常行』里。")
    A("")
    A("=" * 72)
    healthy = (s["monotonic"] and s["bad_rows"] == 0
               and s["feed_reject_max"] in (0, None)
               and s["csr_bad"] == 0 and s["iwdg_bad"] == 0
               and not s["seq_changed"]
               and (s["worst_age"] is None or s["worst_age"] <= AGE_ALERT_MS)
               and (s["clock_drift"] is None
                    or abs(s["clock_drift"]) <= CLOCK_DRIFT_ALERT_S))
    A(" 结论：%s" % ("截至目前全部健康" if healthy else "**有需要人看的地方**（见上）"))
    A("=" * 72)
    return "\n".join(out)


# ---------------------------------------------------------------------------
def selftest():
    """自检：用内存里的假 CSV 行验证汇总逻辑（不依赖真实浸泡数据）。"""
    ok = True

    def eq(label, got, want):
        nonlocal ok
        if got == want:
            print("  [ OK ] %s" % label)
        else:
            ok = False
            print("  [FAIL] %s\n         got  %r\n         want %r" % (label, got, want))

    def row(**kw):
        base = {"t_wall": "2026-10-16 00:00:00", "uptime_s": "100", "notes": "",
                "feed_reject": "0", "rcc_csr": "3", "iwdg_pr": "5",
                "iwdg_rlr": "999", "pending": "0", "seq0": "4", "seq1": "4"}
        base.update(kw)
        return base

    print("--- 正常三行 ---")
    s = summarize([row(uptime_s="100"), row(uptime_s="400"),
                   row(uptime_s="700")], [], 72)
    eq("点数", s["n"], 3)
    eq("单调", s["monotonic"], True)
    eq("异常行", s["bad_rows"], 0)
    eq("覆盖", s["uptime_last"] - s["uptime_first"], 600)

    print("--- uptime 倒退（复位）---")
    s = summarize([row(uptime_s="700"), row(uptime_s="120")], [], 72)
    eq("检出倒退", s["monotonic"], False)
    eq("倒退次数", s["regressions"], 1)

    print("--- notes 非空算异常 ---")
    s = summarize([row(notes="UI=FAULT")], [], 72)
    eq("异常行数", s["bad_rows"], 1)
    eq("有例子", len(s["bad_examples"]), 1)

    print("--- feed_reject 非 0 ---")
    s = summarize([row(feed_reject="0"), row(feed_reject="7")], [], 72)
    eq("取最大值", s["feed_reject_max"], 7)

    print("--- RCC_CSR 复位位 ---")
    # ⚠️ 真实 CSV 里是十进制（DictWriter 写 Python int）；同时验证 0x 前缀也能吃
    s = summarize([row(rcc_csr="3"), row(rcc_csr="67108867")], [], 72)   # 0x04000003
    eq("检出异常复位（十进制）", s["csr_bad"], 1)
    s = summarize([row(rcc_csr="0x04000003")], [], 72)
    eq("检出异常复位（0x 前缀）", s["csr_bad"], 1)
    s = summarize([row(rcc_csr="3")], [], 72)
    eq("良性值不误判", s["csr_bad"], 0)

    print("--- IWDG 漂移 ---")
    s = summarize([row(iwdg_pr="4")], [], 72)
    eq("检出 IWDG 漂移", s["iwdg_bad"], 1)

    print("--- 元数据 seq 变化（pending=0 时不该变）---")
    s = summarize([row(seq0="4"), row(seq0="5")], [], 72)
    eq("检出 seq 变化", s["seq_changed"], True)
    s = summarize([row(seq0="4", pending="1"), row(seq0="5", pending="1")], [], 72)
    eq("pending=1 时不判变化", s["seq_changed"], False)

    print("--- 最老心跳 ---")
    s = summarize([row(st6="1", age6="96"), row(st6="1", age6="4000")], [], 72)
    eq("取最老", s["worst_age"], 4000)
    eq("任务名", s["worst_task"], "UI")
    eq("超阈值会判不健康", s["worst_age"] > AGE_ALERT_MS, True)
    s = summarize([row(st6="1", age6="200")], [], 72)
    eq("正常值不超阈值", s["worst_age"] <= AGE_ALERT_MS, True)
    s = summarize([row(st6="3", age6="99999")], [], 72)
    eq("SUSPEND(3) 会被跳过", s["worst_age"], None)
    s = summarize([row(st6="0", age6="99999")], [], 72)
    eq("IDLE(0) 会被跳过", s["worst_age"], None)

    print("--- 挂钟 vs t_elapsed_s 一致性 ---")
    s = summarize([row(t_wall="2026-10-16 00:00:00", t_elapsed_s="0"),
                   row(t_wall="2026-10-16 00:05:00", t_elapsed_s="300")], [], 72)
    eq("挂钟跨度", s["wall_span"], 300)
    eq("无漂移", s["clock_drift"], 0)
    s = summarize([row(t_wall="2026-10-16 00:00:00", t_elapsed_s="0"),
                   row(t_wall="2026-10-16 00:05:00", t_elapsed_s="120")], [], 72)
    eq("检出漂移（挂钟比 t_elapsed 快 180 s）", s["clock_drift"], 180)
    eq("超阈值判不健康", abs(s["clock_drift"]) > CLOCK_DRIFT_ALERT_S, True)
    # 解析不了的挂钟不该让脚本崩
    s = summarize([row(t_wall="不是时间"), row(t_wall="也不是")], [], 72)
    eq("挂钟不可解析时返回 None", s["wall_span"], None)

    print("")
    print("[ OK ] soak_status 自检通过" if ok else "[FAIL] 自检未通过")
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description="浸泡进度与健康摘要（只读 CSV）")
    ap.add_argument("--csv", default="", help="CSV 路径（默认取最新的 soak_*.csv）")
    ap.add_argument("--hours", type=float, default=72.0, help="目标时长（默认 72）")
    ap.add_argument("--selftest", action="store_true", help="自检解析逻辑")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    path = args.csv or find_latest_csv()
    if not path or not os.path.isfile(path):
        print("[FAIL] 找不到 CSV。用 --csv 指定，或确认浸泡已在写文件。")
        return 1

    try:
        rows, fields = load(path)
    except Exception as exc:                            # noqa: BLE001
        print("[FAIL] 读 CSV 失败：%s" % exc)
        return 1

    if not rows:
        print("[注意] CSV 里还没有数据行 —— 浸泡刚启动时是正常的：")
        print("       首点是**立即**采的（t_elapsed_s = 0，不等一个采样周期），")
        print("       所以只要进程起来了，几秒内就该有数据行。")
        print("       文件：%s" % path)
        return 0

    print(render(summarize(rows, fields, args.hours), path, args.hours))
    return 0


if __name__ == "__main__":
    sys.exit(main())
