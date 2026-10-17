"""重冻基线后，把 §二·八 的 SHA256 台账**自动**更新掉。

============================================================================
 为什么需要它
============================================================================
交接文档 §二·八 的台账（commit / 构建命令 / 四个 bin 的大小+SHA256）是**证据链**：
`tools/soak_report.py` 的验收报告会**逐字比对**它。落地当天第 9 步要"重冻 golden"，
台账必须跟着改 —— 而**手抄 64 位哈希**只要错一个字符，
要么让报告误报、要么（更糟）把错误固化成"证据"。

所以：解析 → 替换 → **再用 soak_report 自己的解析器做往返校验**，
并拒绝在**工作区不干净**时登记（台账必须对应一个明确的 commit）。

用法：
    python tools/freeze_baseline.py --images build/ota \\
        --command "tools\\build_ota_images.ps1 -Rtos -Optimize s -SlotB -Golden" \\
        --dry-run
    # 确认无误后去掉 --dry-run

⚠️ 工具**只改** §二·八 里那三行元数据与四行镜像；**不改**"产物来源"那段散文 ——
   换生产版 main 之后那段必须由人重写（工具会在结束时提醒）。
"""
import argparse
import hashlib
import io
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import soak_report                                    # noqa: E402

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(errors="replace")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINS = ['bootloader.bin', 'app_a.bin', 'app_b.bin', 'app_golden.bin']


def sha256_file(p):
    h = hashlib.sha256()
    with io.open(p, 'rb') as f:
        for c in iter(lambda: f.read(65536), b''):
            h.update(c)
    return h.hexdigest()


def git_state():
    def run(*a):
        return subprocess.run(['git'] + list(a), cwd=REPO, capture_output=True,
                              text=True, errors='replace').stdout.strip()
    return run('rev-parse', '--short', 'HEAD'), run('rev-parse', 'HEAD'), \
        run('status', '--porcelain')


def pat_replace(txt, pattern, new, what):
    m = re.search(pattern, txt)
    if not m:
        raise SystemExit('  [FAIL] 台账里找不到 %s（模式 %s）' % (what, pattern[:50]))
    return txt[:m.start()] + new + txt[m.end():]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--images', default=os.path.join(REPO, 'build', 'ota'))
    ap.add_argument('--doc', default=os.path.join(REPO, 'doc', '上板调试交接.md'))
    ap.add_argument('--command', required=True,
                    help='**实际**用的构建命令（原样记进台账）')
    ap.add_argument('--when', default='', help='构建时间，默认取镜像的 mtime')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--allow-dirty', action='store_true',
                    help='允许在工作区不干净时登记（默认拒绝）')
    ap.add_argument('--set-meta', action='store_true',
                    help='同时更新 commit/构建命令/构建时间三行。'
                         '默认只更新镜像行——那三行描述的是这批镜像是'
                         '什么时候、用什么造的，只有现在正在重冻时才该改')
    a = ap.parse_args()

    short, full, dirty = git_state()
    if dirty and not a.allow_dirty:
        print('  [FAIL] 工作区不干净 —— 拒绝登记（台账要对应一个明确的 commit）。')
        print('        先提交，或用 --allow-dirty 明确接受。')
        for line in dirty.splitlines()[:6]:
            print('        ' + line)
        return 2

    rows = []
    for n in BINS:
        f = os.path.join(a.images, n)
        if not os.path.isfile(f):
            print('  [FAIL] 缺镜像：%s' % f)
            return 2
        rows.append((n, os.path.getsize(f), sha256_file(f), os.path.getmtime(f)))
    when = a.when
    if not when:
        import time
        when = time.strftime('%Y-%m-%d %H:%M', time.localtime(min(r[3] for r in rows)))

    print('=' * 70)
    print(' 重冻台账（%s）' % (('dry-run，不写文件' if a.dry_run else '写入')
                            + ('（含元数据三行）' if a.set_meta
                               else '（仅镜像行）')))
    print('   commit   : %s（%s）%s' % (short, full, '  ⚠️ 工作区不干净' if dirty else ''))
    print('   命令     : %s' % a.command)
    print('   时间     : %s' % when)
    for n, size, h, _ in rows:
        print('   %-16s %7d B  %s' % (n, size, h))
    print('=' * 70)

    raw = io.open(a.doc, 'rb').read()
    nl = b'\r\n' if b'\r\n' in raw else b'\n'
    txt = raw.decode('utf-8')
    new = txt
    if a.set_meta:
        new = pat_replace(new, r'\|\s*commit\s*\|\s*`[0-9a-fA-F]+`[^|]*\|',
                          '| commit | `%s`（`%s`） |' % (short, full), 'commit 行')
        new = pat_replace(new, r'\|\s*构建命令\s*\|\s*`[^`]*`\s*\|',
                          '| 构建命令 | `%s` |' % a.command, '构建命令行')
        new = pat_replace(new, r'\|\s*构建时间\s*\|\s*[^|]*\|',
                          '| 构建时间 | %s |' % when, '构建时间行')
    else:
        print(' 只更新镜像行；commit/命令/时间保持原样（未给 --set-meta）。')
    for n, size, h, _ in rows:
        # ⚠️ 模式要**容忍任意内容**（按文件名定位，后面两格随便是什么）：
        #    测试发现，若台账那行被手抄改坏（哈希里带非十六进制字符），
        #    原先要求 64 位十六进制的模式就匹配不上 -> 工具反而修不了坏行。
        new = pat_replace(
            new,
            r'\|\s*`' + re.escape(n) + r'`\s*\|[^|]*\|[^|]*\|',
            '| `%s` | %d B | `%s` |' % (n, size, h), '%s 行' % n)

    if new == txt:
        print(' 台账本来就是最新的（无变化）。')
    elif a.dry_run:
        import difflib
        d = [l for l in difflib.unified_diff(txt.splitlines(), new.splitlines(),
                                            lineterm='', n=0)
             if l.startswith(('+', '-')) and not l.startswith(('+++', '---'))]
        print(' dry-run：台账有变化（未写文件）；具体差异：')
        for l in d[:12]:
            print('   ' + l[:150])
    else:
        io.open(a.doc, 'wb').write(new.encode('utf-8').replace(b'\r\n', b'\n')
                                   if nl == b'\n' else new.encode('utf-8'))
        print(' 已写入 %s' % a.doc)

    # ---- 往返校验：用 soak_report 自己的解析器读回来 ----
    if not a.dry_run:
        soak_report.LEDGER = os.path.relpath(a.doc, REPO)
        res = soak_report.baseline_check(a.images)
        bad = [x for x in res if x[1]]
        for name, st in res:
            print('   [%s] %s %s' % (' OK ' if not st else 'FAIL', name, st))
        if bad:
            print('  [FAIL] 往返校验没过 —— 台账与镜像不一致（请检查 doc 是否被别处引用）')
            return 1
        print(' 往返校验通过：soak_report 的台账比对全部一致 ✓')

    print('')
    print(' ⚠️ 提醒：§二·八 里"产物来源"那段散文（工程 main.c 是 L0 衍生 vs 生产版）')
    print('    是**语义判断**，本工具不动它。若这批镜像是用生产版 main + -Rtos 构建的，')
    print('    必须由人重写那一段（否则台账记的哈希与"来源"说法会互相矛盾）。')
    return 0


if __name__ == '__main__':
    sys.exit(main())
