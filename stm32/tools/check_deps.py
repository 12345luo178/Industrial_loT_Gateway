# -*- coding: utf-8 -*-
"""
外部依赖检查：对 deps.lock 逐项报"在不在、版本对不对、有没有被人改过"

============================================================================
 为什么这个检查是**必需**的，而不是锦上添花
============================================================================

  本仓库刻意**不 vendoring** 第三方源码（LVGL / FreeRTOS 都走外部路径）。
  这个选择带来一个后果：

      **刚克隆下来的仓库，构建 LVGL 变体一定失败。**

  2026-10-15 之前，这个失败的表现是 `build_ota_images.ps1 -Lvgl` 抛一句
  "找不到 LVGL 目录" —— 信息量极低：不知道要哪个版本、不知道放哪、
  更不知道"放对了版本"这件事有多要紧。

  而**版本是要紧的**：
    * 冻结基线（72h 浸泡）用的就是 LVGL 版，基线镜像的 SHA256 已经记进
      doc/浸泡基线-2026-10-15.md。换一个 LVGL 版本 ⇒ 同一天在两台机器上
      构建出**不同字节**的镜像 ⇒ "冻结基线"这句话失去意义。
    * 本项目已经因为 LVGL 的绘制语义踩过一次坑（缺陷 AQ，LV_DRAW_COMPLEX）。
      升 minor（v8.3 / v9.x）会改 API 与控件绘制，不能随手升级。

  所以这个脚本做三件事，缺一不可：
    1) **在不在** —— 缺失时给出"跑 tools/fetch_deps.ps1"的明确指引，
       而不是让人对着 "找不到 LVGL 目录" 发呆
    2) **版本对不对** —— 直接读 lvgl.h 的 LVGL_VERSION_MAJOR/MINOR/PATCH
       与 deps.lock 逐位比对（**不信任 deps.lock 的自述**）
    3) **有没有被改过** —— 源码树聚合 SHA256 与 deps.lock 记录比对，
       防止"本地手改了一行 LVGL 源码"这种最难查的偏差

============================================================================
 用法
============================================================================

    python tools/check_deps.py              # 默认：存在性 + 版本 + 哈希
    python tools/check_deps.py --fast       # 跳过昂贵的源码树哈希（CI 用）
    python tools/check_deps.py --selftest   # 不碰本机依赖，自检解析逻辑

  退出码：0 = 全部就绪；1 = 有依赖不就绪（CI 会红）；2 = 脚本自身出错
============================================================================
"""
from __future__ import print_function

import argparse
import hashlib
import os
import re
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.dirname(_HERE)
LOCK_PATH = os.path.join(_REPO_ROOT, "deps.lock")

# LVGL 版本号在 lvgl.h 里的样子（v8.2.0 实测）
RE_VERSION = re.compile(
    r"^\s*#\s*define\s+LVGL_VERSION_(MAJOR|MINOR|PATCH)\s+(\d+)", re.M)


# ---------------------------------------------------------------------------
# deps.lock 解析（与 tools/fetch_deps.ps1 的 Read-DepsLock 保持同一套语义）
# ---------------------------------------------------------------------------
def read_lock(path=LOCK_PATH):
    """
    极简 INI：[section] + key = value。

    ⚠️ 值必须是**单行** —— 这一点与 PowerShell 侧的实现一致。
       2026-10-16 写第一版 deps.lock 时我给 note 折了行，
       结果第二行被当成"不在任何节里的野行"跳掉（脚本会 WARNING，
       但注释里的信息就悄悄丢了）。宁可 note 写长一点，也不要折行。
    """
    if not os.path.isfile(path):
        raise IOError("找不到 deps.lock：%s" % path)

    sections = {}
    current = None
    warnings = []

    with open(path, "r", encoding="utf-8") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith("#") or line.startswith(";"):
                continue

            m = re.match(r"^\[(.+)\]$", line)
            if m:
                current = m.group(1).strip()
                sections.setdefault(current, {})
                continue

            if "=" not in line:
                warnings.append("第 %d 行看不懂，已跳过：%s" % (lineno, line))
                continue
            if current is None:
                warnings.append("第 %d 行不在任何 [节] 里，已跳过：%s"
                                % (lineno, line))
                continue

            key, _, val = line.partition("=")
            sections[current][key.strip()] = val.strip()

    return sections, warnings


# ---------------------------------------------------------------------------
# 源码树聚合哈希（与 fetch_deps.ps1 的 Get-TreeSha256 必须算出同一个值）
# ---------------------------------------------------------------------------
def tree_sha256(root, keep):
    """
    逐文件 SHA256，按"相对路径(小写、正斜杠)"排序拼起来再哈希一次。

    这样得到的是**与文件系统枚举顺序无关**的稳定值：
    换个盘符、换个时间解压，结果一样；少一个文件/改一个字节，结果就变。

    ⚠️ 这个算法必须与 tools/fetch_deps.ps1 里的 Get-TreeSha256 逐字节一致 ——
       两边对同一棵树必须得出同一个哈希，否则"锁定的值"根本没法校验。
       实测锚点：D:\\lvgl\\lvgl-release-v8.2 的 lvgl.h+src/（325 个文件）
       == e74fbb104aacc77dd40f79888a3b42ed15c801923fa629618c5d46104d0c1b7f
    """
    root_full = os.path.abspath(root)
    entries = []

    for k in keep:
        rel = k.rstrip("/")
        full = os.path.join(root_full, rel.replace("/", os.sep))

        if os.path.isfile(full):
            entries.append((rel.lower(),
                            _file_sha256(full)))
        elif os.path.isdir(full):
            for dirpath, _dirnames, filenames in os.walk(full):
                for fn in filenames:
                    fp = os.path.join(dirpath, fn)
                    rp = os.path.relpath(fp, root_full).replace(os.sep, "/")
                    entries.append((rp.lower(), _file_sha256(fp)))
        else:
            raise IOError("keep 里声明的路径不存在：%s（看 %s）" % (rel, root_full))

    entries.sort()
    joined = "\n".join("%s\n%s" % (p, h) for p, h in entries)

    sha = hashlib.sha256()
    sha.update(joined.encode("utf-8"))
    return sha.hexdigest(), len(entries)


def _file_sha256(path):
    sha = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            sha.update(chunk)
    return sha.hexdigest()


# ---------------------------------------------------------------------------
# 版本比对
# ---------------------------------------------------------------------------
def read_lvgl_version(lvgl_root):
    """
    从 lvgl.h 里读真实版本号。

    ⚠️ 故意**不**相信 deps.lock 里写的 version 字段：
       那个字段是人写的，可能过期；lvgl.h 是构建真正会 include 的东西。
       两者不一致本身就是个要报出来的问题。
    """
    header = os.path.join(lvgl_root, "lvgl.h")
    if not os.path.isfile(header):
        return None, "找不到 lvgl.h：%s" % header

    with open(header, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()

    found = {}
    for m in RE_VERSION.finditer(text):
        found[m.group(1)] = int(m.group(2))

    if len(found) != 3:
        return None, "lvgl.h 里没找齐 MAJOR/MINOR/PATCH（读到 %r）" % (found,)

    return (found["MAJOR"], found["MINOR"], found["PATCH"]), None


def parse_version(text):
    """'v8.2.0' / '8.2.0' -> (8, 2, 0)；解析不了返回 None"""
    m = re.search(r"(\d+)\.(\d+)\.(\d+)", text or "")
    if not m:
        return None
    return tuple(int(x) for x in m.groups())


# ---------------------------------------------------------------------------
# 检查
# ---------------------------------------------------------------------------
def check(lock, fast=False, verbose=True):
    problems = []
    checked = 0

    for name, cfg in sorted(lock.items()):
        dest = cfg.get("dest", "")
        url = cfg.get("url_zip", "")
        keep = [k for k in cfg.get("keep", "").split(" ") if k]
        declared = cfg.get("version", "")

        if verbose:
            print("--- %s ---" % name)
            print("  声明版本 : %s" % (declared or "(未声明)"))
            print("  目标     : %s" % dest)

        # ---- 1) 在不在 ----
        if not dest or not os.path.exists(dest):
            problems.append("%s：缺少 %s" % (name, dest))
            print("  [FAIL] 不存在")
            if url:
                print("         → 跑 `powershell -File tools\\fetch_deps.ps1 -Dep %s`"
                      % name)
            else:
                print("         → 这个依赖没有发布包可下载：%s"
                      % cfg.get("note", ""))
            print("")
            continue

        if not keep:
            # FreeRTOS 这类：只校验存在性（它由 CubeMX 生成，没有独立发布包）
            n = sum(len(fs) for _r, _d, fs in os.walk(dest))
            print("  [ OK ] 存在（%d 个文件）；来源：%s" % (n, cfg.get("note", "")))
            print("")
            checked += 1
            continue

        # ---- 2) 版本对不对 ----
        if name == "lvgl":
            got, err = read_lvgl_version(dest)
            want = parse_version(declared)
            if err:
                problems.append("%s：%s" % (name, err))
                print("  [FAIL] %s" % err)
            elif want and got != want:
                problems.append("%s：lvgl.h 是 %s，deps.lock 声明 %s"
                                % (name, ".".join(map(str, got)), declared))
                print("  [FAIL] lvgl.h 实际 v%s ≠ 声明 %s"
                      % (".".join(map(str, got)), declared))
            else:
                print("  [ OK ] 版本 v%s（读自 lvgl.h，与声明一致）"
                      % ".".join(map(str, got)))
            # 本仓库自带的 LVGL 配置也必须在（它才属于本项目）
            lv_conf = os.path.join(_REPO_ROOT, "port", "lv_conf.h")
            if os.path.isfile(lv_conf):
                print("  [ OK ] 本仓库自带 port/lv_conf.h")
            else:
                problems.append("缺少 port/lv_conf.h（本项目的 LVGL 配置）")
                print("  [FAIL] 缺少 port/lv_conf.h")

        # ---- 3) 有没有被改过 ----
        locked = (cfg.get("src_sha256") or "").strip()
        if fast:
            print("  [跳过] 源码树哈希（--fast）")
        elif locked in ("", "PENDING", "N/A"):
            print("  [注意] deps.lock 里没有可用哈希，无法判断是否被改过"
                  "（跑 fetch_deps.ps1 -UpdateHashes 钉上）")
        else:
            try:
                got_hash, n = tree_sha256(dest, keep)
            except IOError as exc:
                problems.append("%s：%s" % (name, exc))
                print("  [FAIL] %s" % exc)
                print("")
                continue

            if got_hash == locked.lower():
                print("  [ OK ] 源码树 %d 个文件，聚合 SHA256 与锁定值一致" % n)
                print("         %s" % got_hash)
            else:
                problems.append("%s：源码树哈希不匹配（被人改过或换了版本）"
                                % name)
                print("  [FAIL] 源码树哈希不匹配")
                print("         锁定 %s" % locked.lower())
                print("         实际 %s  (%d 个文件)" % (got_hash, n))
        print("")
        checked += 1

    return problems, checked


# ---------------------------------------------------------------------------
# 自检：不碰本机依赖，只验解析与哈希逻辑
# ---------------------------------------------------------------------------
def selftest():
    ok = True

    def check_eq(label, got, want):
        nonlocal ok
        if got == want:
            print("  [ OK ] %s" % label)
        else:
            ok = False
            print("  [FAIL] %s\n         got  %r\n         want %r"
                  % (label, got, want))

    print("--- deps.lock 解析 ---")
    lock, warns = read_lock()
    check_eq("有 lvgl 节", "lvgl" in lock, True)
    check_eq("有 freertos 节", "freertos" in lock, True)
    check_eq("没有野行警告（note 折行会导致警告）", warns, [])
    check_eq("lvgl 声明版本", lock.get("lvgl", {}).get("version"), "v8.2.0")
    check_eq("lvgl keep 解析成两项",
             [k for k in lock.get("lvgl", {}).get("keep", "").split(" ") if k],
             ["lvgl.h", "src/"])
    check_eq("freertos 没有 url（非独立发布包）",
             lock.get("freertos", {}).get("url_zip"), "")

    print("--- 版本号解析 ---")
    check_eq("v8.2.0", parse_version("v8.2.0"), (8, 2, 0))
    check_eq("8.2.0", parse_version("8.2.0"), (8, 2, 0))
    check_eq("v9.1.0", parse_version("v9.1.0"), (9, 1, 0))
    check_eq("无法解析", parse_version("CubeMX 生成"), None)

    print("--- lvgl.h 版本读取（用临时文件，不读本机 LVGL） ---")
    import tempfile
    tmp = tempfile.mkdtemp(prefix="checkdeps_")
    try:
        with open(os.path.join(tmp, "lvgl.h"), "w", encoding="utf-8") as f:
            f.write("#define LVGL_VERSION_MAJOR 8\n"
                    "#define LVGL_VERSION_MINOR 2\n"
                    "#define LVGL_VERSION_PATCH 0\n"
                    "#define LVGL_VERSION_INFO \"\"\n")
        check_eq("读出版本", read_lvgl_version(tmp)[0], (8, 2, 0))

        # 树哈希：加一个文件、改一个字节都必须改变结果
        os.makedirs(os.path.join(tmp, "src"))
        with open(os.path.join(tmp, "src", "a.c"), "w") as f:
            f.write("int a;\n")
        h1, n1 = tree_sha256(tmp, ["lvgl.h", "src/"])
        check_eq("文件数", n1, 2)

        with open(os.path.join(tmp, "src", "b.c"), "w") as f:
            f.write("int b;\n")
        h2, n2 = tree_sha256(tmp, ["lvgl.h", "src/"])
        check_eq("加文件后哈希改变", h1 != h2, True)

        os.remove(os.path.join(tmp, "src", "b.c"))
        h3, _ = tree_sha256(tmp, ["lvgl.h", "src/"])
        check_eq("删回原样后哈希复原", h3 == h1, True)

        with open(os.path.join(tmp, "src", "a.c"), "w") as f:
            f.write("int a ;\n")          # 只多一个空格
        h4, _ = tree_sha256(tmp, ["lvgl.h", "src/"])
        check_eq("改一个字节哈希就变", h4 != h1, True)

        h5, _ = tree_sha256(tmp, ["lvgl.h", "src/"])
        check_eq("同一棵树重复算结果稳定", h5 == h4, True)
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)

    print("")
    print("[ OK ] check_deps 自检通过" if ok else "[FAIL] 自检未通过")
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description="外部依赖检查（deps.lock）")
    ap.add_argument("--fast", action="store_true",
                    help="跳过源码树哈希（CI 用；哈希要遍历 300+ 文件）")
    ap.add_argument("--selftest", action="store_true",
                    help="自检解析与哈希逻辑，不碰本机依赖")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    try:
        lock, warns = read_lock()
    except IOError as exc:
        print("[FAIL] %s" % exc)
        return 2

    for w in warns:
        print("[警告] %s" % w)

    print("=" * 70)
    print(" 外部依赖检查（deps.lock 钉死的版本）")
    print(" 仓库：%s" % _REPO_ROOT)
    print(" 模式：%s" % ("快速（跳过源码树哈希）" if args.fast else "完整"))
    print("=" * 70)

    problems, checked = check(lock, fast=args.fast)

    print("=" * 70)
    if problems:
        print(" 有 %d 项不就绪（已检查 %d 个依赖）：" % (len(problems), checked))
        for p in problems:
            print("   - %s" % p)
        print("")
        print(" 缺依赖时: powershell -File tools\\fetch_deps.ps1")
        print(" 版本不符时: 换回 deps.lock 声明的版本（别就地升级，会作废冻结基线）")
        print("=" * 70)
        return 1

    print(" 全部就绪（%d 个依赖）" % checked)
    print("=" * 70)
    return 0


if __name__ == "__main__":
    sys.exit(main())
