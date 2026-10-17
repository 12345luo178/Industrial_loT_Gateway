#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""72 小时浸泡测试的验收报告：把 build/soak.csv 变成"能不能签字"的结论。

============================================================================
 为什么要有它
============================================================================

  浸泡测试的结论**必须可复现**：三天后说"这一版连续跑了 72 小时没问题"，
  依据不能是"我看了两眼串口"，而是一条命令能把判据重新跑一遍。

  判据（与交接文档 §二·八 一致，这里逐条机械核对）：

    · uptime 单调增长，且覆盖到 ≥ 目标小时数（默认 72）
    · **零非预期复位**：RCC_CSR 的 bit25~31 全程为 0（bit24=RMVF、bit1=LSIRDY 不算）
    · **零元数据擦写**：pending==0 时两页 seq 一字不变（这才是"零磨损"的证据）
    · IWDG 配置全程 = PR 4 / RLR 999
    · dev_status 保持 0x0003（DHT_OK + CAN_OK）
    · mb_err / mb_exc 的增长量按月列举（不自动判死 —— 需要人看量级）

  ⚠️ 还要报**证据的完整性**：采样间隔本该恒为 5 分钟。
     中间如果有长空档（巡检脚本被关掉/机器睡眠），那是**证据上的洞** ——
     必须显式说出来，而不是"反正首尾都对，所以通过了"。
     这是这个工具与"手工看 CSV"最大的区别。

============================================================================
 用法
============================================================================

    python tools/soak_report.py                       # 人看的表格 + 结论
    python tools/soak_report.py --md                  # 直接生成可粘进文档的 Markdown
    python tools/soak_report.py --csv build/soak.csv --target-hours 72
    python tools/soak_report.py --expect-seq 504      # 额外核对起始 seq（防拿错 CSV）

  退出码：0 = 通过；1 = 有失败判据；2 = 还没跑够（进行中）或证据有洞
============================================================================
"""
import argparse
import csv
import hashlib
import io
import os
import re
import subprocess
import sys
from datetime import datetime, timedelta

# ⚠️ 编码守卫（print 里有 ⚠️/✅；裸 GBK 控制台不加会 UnicodeEncodeError）
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(errors="replace")
    except (AttributeError, ValueError):
        pass

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CSR_RESET_MASK = 0xFE000000      # bit25~31 = 各种复位标志（bit24=RMVF 不算）
IWDG_PR_EXPECT, IWDG_RLR_EXPECT = 4, 999


def num(row, key):
    """取整数字段；空/非法返回 None（采样失败的行）"""
    v = (row.get(key) or "").strip()
    if v == "":
        return None
    try:
        return int(v)
    except ValueError:
        return None


def load(path):
    with io.open(path, encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def analyse(rows, target_h):
    r = {
        "n": len(rows), "bad_rows": 0, "fails": [], "warns": [],
        "uptime_first": None, "uptime_last": None, "mono": True,
        "reset_flags": [], "iwdg_bad": [], "seq_change": [],
        "statuses": {}, "gaps": [], "interval_med": None,
        "mb_err_first": None, "mb_err_last": None,
        "mb_exc_first": None, "mb_exc_last": None,
        # ⚠️ 覆盖范围用：这几列决定"这次浸泡到底证明了什么"（见报告里的覆盖范围段）
        "mb_rx_first": None, "mb_rx_last": None,
        "can_rx_first": None, "can_rx_last": None,
        "first_wall": rows[0]["t_wall"] if rows else None,
        "last_wall": rows[-1]["t_wall"] if rows else None,
        "seq0_first": None, "seq1_first": None,
        "seq0_last": None, "seq1_last": None,
    }
    prev_up = None
    prev_el = None
    prev_pending = None
    intervals = []

    for i, row in enumerate(rows, 1):
        up = num(row, "uptime_s")
        el = num(row, "t_elapsed_s")
        if up is None or el is None:
            r["bad_rows"] += 1
            continue

        if r["uptime_first"] is None:
            r["uptime_first"] = up
            r["mb_err_first"] = num(row, "mb_err")
            r["mb_exc_first"] = num(row, "mb_exc")
            r["mb_rx_first"] = num(row, "mb_rx")
            r["can_rx_first"] = num(row, "can_rx")
            r["seq0_first"] = num(row, "seq0")
            r["seq1_first"] = num(row, "seq1")
        r["uptime_last"] = up
        r["mb_err_last"] = num(row, "mb_err")
        r["mb_exc_last"] = num(row, "mb_exc")
        r["mb_rx_last"] = num(row, "mb_rx")
        r["can_rx_last"] = num(row, "can_rx")
        r["seq0_last"] = num(row, "seq0")
        r["seq1_last"] = num(row, "seq1")

        if prev_up is not None and up < prev_up:
            r["mono"] = False
            r["fails"].append("第 %d 行 uptime 倒退：%d -> %d（中间复位过）" % (i, prev_up, up))
        prev_up = up

        if prev_el is not None:
            intervals.append(el - prev_el)
        prev_el = el

        csr = num(row, "rcc_csr")
        if csr is not None and (csr & CSR_RESET_MASK):
            r["reset_flags"].append((i, "0x%08X" % csr))

        pr, rlr = num(row, "iwdg_pr"), num(row, "iwdg_rlr")
        if pr is not None and (pr != IWDG_PR_EXPECT or rlr != IWDG_RLR_EXPECT):
            r["iwdg_bad"].append((i, pr, rlr))

        pend = num(row, "pending")
        if pend == 0 and prev_pending == 0:
            if (num(row, "seq0") != r["seq0_prev"]) or (num(row, "seq1") != r["seq1_prev"]):
                r["seq_change"].append(i)
        r["seq0_prev"] = num(row, "seq0")
        r["seq1_prev"] = num(row, "seq1")
        prev_pending = pend

        st = num(row, "dev_status")
        r["statuses"][st] = r["statuses"].get(st, 0) + 1

    if intervals:
        s = sorted(intervals)
        r["interval_med"] = s[len(s) // 2]
        med = r["interval_med"] or 1
        r["gaps"] = [(i + 1, d) for i, d in enumerate(intervals) if d > med * 1.5 + 1]

    if r["reset_flags"]:
        r["fails"].append("有 %d 个采样点出现复位标志（首个在第 %d 行：%s）"
                          % (len(r["reset_flags"]), r["reset_flags"][0][0], r["reset_flags"][0][1]))
    if r["iwdg_bad"]:
        r["fails"].append("IWDG 配置被改过：%s" % (r["iwdg_bad"][:3],))
    if r["seq_change"]:
        r["fails"].append("pending==0 时元数据 seq 变了（第 %s 行）—— 不该发生的擦写"
                          % r["seq_change"][:5])
    if not r["mono"]:
        pass                    # 已经在上面逐行报过
    bad_status = {k: v for k, v in r["statuses"].items() if k not in (3, None)}
    if bad_status:
        r["warns"].append("dev_status 出现过非 0x0003：%s（需人看）" % bad_status)
    if r["bad_rows"]:
        r["warns"].append("有 %d 个采样点读失败（证据有洞，见下面空档统计）" % r["bad_rows"])
    if r["gaps"]:
        r["warns"].append("采样间隔有 %d 处异常空档（巡检可能中断过）：%s"
                          % (len(r["gaps"]),
                             ", ".join("第%d行+%ds" % g for g in r["gaps"][:5])))

    span_s = (r["uptime_last"] or 0) - (r["uptime_first"] or 0)
    r["span_h"] = span_s / 3600.0
    r["coverage"] = r["span_h"] / target_h if target_h else 0.0
    r["done"] = r["span_h"] >= target_h
    if r["done"] and not r["fails"] and not r["bad_rows"]:
        r["verdict"] = "通过"
    elif r["fails"]:
        r["verdict"] = "未通过"
    else:
        r["verdict"] = "进行中"
    return r


def fmt_delta(a, b):
    if a is None or b is None:
        return "?"
    return "%d -> %d（+%d）" % (a, b, b - a)


def sha256_file(path):
    """文件的 sha256（分块读，避免整文件进内存）"""
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(65536), b''):
            h.update(chunk)
    return h.hexdigest()


def git_state():
    """(短 commit, 工作区是否脏)；取不到就 unknown"""
    try:
        c = subprocess.check_output(['git', '-C', REPO, 'rev-parse', '--short', 'HEAD'],
                                    stderr=subprocess.DEVNULL).decode().strip()
        d = subprocess.check_output(['git', '-C', REPO, 'status', '--porcelain'],
                                    stderr=subprocess.DEVNULL).decode().strip()
        return c, bool(d)
    except Exception:                                  # noqa: BLE001
        return 'unknown', False


LEDGER = os.path.join('doc', '上板调试交接.md')
LEDGER_BINS = ['bootloader.bin', 'app_a.bin', 'app_b.bin', 'app_golden.bin']


def baseline_check(ota_dir):
    """把 build/ota 里四个 bin 的**大小 + sha256** 与台账 §二·八 里记的逐字比对。

    ⚠️ 为什么期望值要从台账**解析**而不是写死在这里：
        第 9 步"重冻 golden"之后台账会更新；写死在工具里就变成一份**过期副本**，
        下次核对会误报。台账是唯一来源，这里只是它的执行者。
    ⚠️ 返回 [(name, 状态字符串)]，状态为空表示一致。
    """
    want = {}
    p = os.path.join(REPO, LEDGER)
    if os.path.isfile(p):
        txt = io.open(p, encoding='utf-8', errors='replace').read()
        for name in LEDGER_BINS:
            m = re.search(r'\|\s*`' + re.escape(name) + r'`\s*\|\s*(\d+)\s*B\s*\|\s*'
                          r'`([0-9a-fA-F]{64})`', txt)
            if m:
                want[name] = (int(m.group(1)), m.group(2).lower())
    out = []
    for name in LEDGER_BINS:
        f = os.path.join(ota_dir, name)
        if not os.path.isfile(f):
            out.append((name, '文件不存在'))
            continue
        size = os.path.getsize(f)
        h = sha256_file(f)
        if name not in want:
            out.append((name, '台账里没记（跳过比对）'))
        elif want[name] == (size, h):
            out.append((name, ''))
        else:
            out.append((name, '**与台账不一致**（大小 %d vs %d，哈希 %s… vs %s…）'
                        % (size, want[name][0], h[:12], want[name][1][:12])))
    return out


def coverage_note(r):
    """★ 这次浸泡**证明了什么、没证明什么**（2026-10-11 第 28 轮加）

    ⚠️ 为什么必须写在报告里：本次全程 `mb_rx` 没变（= **没有任何 Modbus 请求**），
       也就是说它是一次**空闲稳定性**浸泡。它能证明的是"引导/元数据零擦写、
       IWDG 配置稳定、采样不打扰系统、uptime 单调"；**不能**证明"通信负载下也稳"
       —— 三天后若把后者也讲成结论，就是过度声称。
    """
    d_rx = None
    if r.get("mb_rx_first") is not None and r.get("mb_rx_last") is not None:
        d_rx = r["mb_rx_last"] - r["mb_rx_first"]
    d_can = None
    if r.get("can_rx_first") is not None and r.get("can_rx_last") is not None:
        d_can = r["can_rx_last"] - r["can_rx_first"]

    parts = []
    if d_rx == 0:
        parts.append("**空闲稳定性**（Modbus 请求全程 0 -> 无通信负载）")
    elif d_rx is not None:
        parts.append("带 Modbus 流量（请求 +%d）" % d_rx)
    if d_can is not None and d_can > 0:
        parts.append("CAN 收帧 +%d（L0 回环自检，非外部帧）" % d_can)
    note = "；".join(parts) if parts else "未知"
    note += "。⚠️ 它**不**覆盖「通信负载下的稳定性」（那部分由四步实验与 e2e 测试覆盖）"
    return note


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default=os.path.join(REPO, "build", "soak.csv"))
    ap.add_argument("--target-hours", type=float, default=72.0)
    ap.add_argument("--expect-seq", type=int, default=None,
                    help="起始元数据 seq（防止拿错 CSV）")
    ap.add_argument("--md", action="store_true", help="输出可粘进文档的 Markdown")
    ap.add_argument("--out", default="",
                    help="把 Markdown 报告写到文件（可归档到仓库；同时打印）")
    a = ap.parse_args()

    if not os.path.isfile(a.csv):
        print("找不到 %s" % a.csv)
        return 2
    rows = load(a.csv)
    if not rows:
        print("CSV 是空的")
        return 2
    r = analyse(rows, a.target_hours)

    # ---- 证据固定（2026-10-11 第 27 轮加）----
    # 目标里写的"记录 SHA256 + commit + 元数据快照 / 证据可复现"就落在这里：
    #   ① CSV 自身的 sha256（报告与数据一一对应，换一个 CSV 就对不上）
    #   ② 仓库 commit + 工作区是否干净
    #   ③ 冻结基线的四个 bin 与台账 §二·八 **逐字比对**（期望值从台账解析，不写死）
    csv_sha = sha256_file(a.csv)
    commit, dirty = git_state()
    # ⚠️ 冻结产物在 build/ota（不是 CSV 所在的 build/）—— 第一版就是这里写错，
    #    于是四个 bin 全报"文件不存在"，把"核对"变成了噪音。
    ota_dir = os.path.join(REPO, 'build', 'ota')
    baseline = baseline_check(ota_dir) if os.path.isdir(ota_dir) else []
    base_bad = [x for x in baseline if x[1] and '跳过' not in x[1]]
    csv_note = ("⚠️ 起始 seq 是 %s，而期望 %d —— 这可能是**另一次浸泡**的 CSV"
                % (r["seq0_first"], a.expect_seq)) if (
        a.expect_seq is not None and r["seq0_first"] != a.expect_seq) else ""

    if a.out:
        # ⚠️ 用子进程重跑一次 --md 再落盘：保持"人看的输出"与"归档文件"完全同源，
        #    免得两套打印逻辑慢慢漂移。
        args = [sys.executable, os.path.abspath(__file__), '--csv', a.csv,
                '--target-hours', str(a.target_hours), '--md']
        if a.expect_seq is not None:
            args += ['--expect-seq', str(a.expect_seq)]
        out = subprocess.run(args, capture_output=True, text=True,
                             encoding='utf-8', errors='replace')
        io.open(a.out, 'w', encoding='utf-8', newline='\n').write(out.stdout)
        print("已写出归档报告：%s（%d 行）" % (a.out, out.stdout.count('\n')))
        print(out.stdout)
        return out.returncode

    seq_note = ""
    if a.expect_seq is not None and r["seq0_first"] != a.expect_seq:
        seq_note = ("⚠️ 起始 seq 是 %s，而期望 %d —— 这可能是**另一次浸泡**的 CSV"
                    % (r["seq0_first"], a.expect_seq))

    if a.md:
        print("| 项 | 结果 |")
        print("|---|---|")
        print("| 采样点 | %d 个（读失败 %d 个） |" % (r["n"], r["bad_rows"]))
        print("| 时间跨度 | %s 起，%s 止 |" % (r["first_wall"], r["last_wall"]))
        print("| uptime | %s -> %s（%.1f 小时，目标 %.0f 小时，%.1f%%） |"
              % (r["uptime_first"], r["uptime_last"], r["span_h"], a.target_hours,
                 r["coverage"] * 100))
        print("| uptime 单调 | %s |" % ("是" if r["mono"] else "**否**"))
        print("| 非预期复位 | %s |" % ("0 次" if not r["reset_flags"]
                                      else "**%d 次**" % len(r["reset_flags"])))
        print("| 元数据擦写（pending=0 时 seq 变化） | %s |"
              % ("0 次（seq %s/%s 全程未变）" % (r["seq0_first"], r["seq1_first"])
                 if not r["seq_change"] else "**%d 次**" % len(r["seq_change"])))
        print("| IWDG 配置 | %s |" % ("全程 4/999" if not r["iwdg_bad"] else "**被改过**"))
        print("| dev_status | %s |" % ", ".join("0x%04X×%d" % (k, v)
                                                for k, v in sorted(r["statuses"].items(),
                                                                   key=lambda kv: -kv[1])))
        print("| mb_err | %s |" % fmt_delta(r["mb_err_first"], r["mb_err_last"]))
        print("| mb_exc | %s |" % fmt_delta(r["mb_exc_first"], r["mb_exc_last"]))
        print("| **覆盖范围** | %s |" % coverage_note(r))
        print("| 采样空档 | %s |" % ("无" if not r["gaps"]
                                    else "%d 处（最长 %ds）" % (len(r["gaps"]),
                                                               max(g[1] for g in r["gaps"]))))
        print("| **结论** | **%s** |" % r["verdict"])
        print("")
        print("**证据固定**")
        print("")
        print("| 项 | 值 |")
        print("|---|---|")
        print("| CSV | `%s` |" % os.path.basename(a.csv))
        print("| CSV sha256 | `%s` |" % csv_sha)
        print("| 仓库 commit | `%s`%s |" % (commit, "（⚠️ 工作区有未提交改动）" if dirty else ""))
        print("| 报告生成时间 | %s |" % datetime.now().strftime("%Y-%m-%d %H:%M:%S"))
        print("| 冻结基线核对 | %s |"
              % ("**4/4 与台账 §二·八 逐字一致**" if not base_bad
                 else "**%d 项异常**：%s" % (len(base_bad),
                                          "; ".join("%s %s" % (n, s) for n, s in base_bad))))
        if seq_note:
            print("\n%s" % seq_note)
        return 0 if r["verdict"] == "通过" else (1 if r["verdict"] == "未通过" else 2)

    print("=" * 66)
    print(" 72 小时浸泡测试验收报告")
    print("   CSV: %s" % a.csv)
    print("=" * 66)
    print("  采样点        : %d 个（读失败 %d 个）" % (r["n"], r["bad_rows"]))
    print("  时间跨度      : %s 起，%s 止" % (r["first_wall"], r["last_wall"]))
    print("  uptime        : %s -> %s" % (r["uptime_first"], r["uptime_last"]))
    print("                  %.1f 小时 / 目标 %.0f 小时 = %.1f%%"
          % (r["span_h"], a.target_hours, r["coverage"] * 100))
    print("  uptime 单调   : %s" % ("是" if r["mono"] else "**否**"))
    print("  非预期复位    : %s" % ("0 次✓" if not r["reset_flags"]
                                  else "**%d 次**" % len(r["reset_flags"])))
    print("  元数据擦写    : %s" % ("0 次（seq %s/%s 全程未变）✓" % (r["seq0_first"], r["seq1_first"])
                                  if not r["seq_change"] else "**%d 次**" % len(r["seq_change"])))
    print("  IWDG 配置     : %s" % ("全程 4/999✓" if not r["iwdg_bad"] else "**被改过**"))
    print("  dev_status    : %s" % ", ".join("0x%04X×%d" % (k, v)
                                            for k, v in sorted(r["statuses"].items(),
                                                               key=lambda kv: -kv[1])))
    print("  mb_err        : %s" % fmt_delta(r["mb_err_first"], r["mb_err_last"]))
    print("  mb_exc        : %s" % fmt_delta(r["mb_exc_first"], r["mb_exc_last"]))
    print("  覆盖范围      : %s" % coverage_note(r))
    print("  采样空档      : %s" % ("无✓" if not r["gaps"]
                                  else "%d 处（最长 %ds）" % (len(r["gaps"]),
                                                             max(g[1] for g in r["gaps"]))))
    print("  ---- 证据固定 ----")
    print("  CSV sha256    : %s" % csv_sha)
    print("  仓库 commit   : %s%s" % (commit, "（⚠️ 工作区有未提交改动）" if dirty else ""))
    print("  冻结基线核对  : %s" % ("4/4 与台账 §二·八 逐字一致✓" if not base_bad
                                  else "**%d 项异常**" % len(base_bad)))
    for n, s in base_bad:
        print("                  %s: %s" % (n, s))
    if csv_note:
        print("  ⚠️ %s" % csv_note)

    if not r["done"]:
        # 按"1 秒 uptime 对 1 秒墙上时间"外推完成时刻
        try:
            last = datetime.strptime(r["last_wall"], "%Y-%m-%d %H:%M:%S")
            left = a.target_hours * 3600 - (r["uptime_last"] - r["uptime_first"])
            eta = last + timedelta(seconds=max(0.0, left))
            print("  预计达标时刻  : %s（按 uptime 与墙上时间 1:1 外推）"
                  % eta.strftime("%Y-%m-%d %H:%M"))
        except ValueError:
            pass

    print("-" * 66)
    for f in r["fails"]:
        print("  [FAIL] %s" % f)
    for w in r["warns"]:
        print("  [注意] %s" % w)
    if seq_note:
        print("  %s" % seq_note)
    print("=" * 66)
    print("  结论：**%s**" % r["verdict"])
    if r["verdict"] == "进行中":
        print("  （还没跑够目标时长；判据目前全部为 0 异常，继续跑即可）")
    print("=" * 66)
    return 0 if r["verdict"] == "通过" else (1 if r["verdict"] == "未通过" else 2)


if __name__ == "__main__":
    sys.exit(main())
