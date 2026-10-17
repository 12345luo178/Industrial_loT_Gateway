#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把我们的 15 个属性**合并进** OneNET 控制台导出的物模型文件

============================================================================
 为什么需要它（而不是直接导入 gen_onenet_tsl.py 的输出）

  gen_onenet_tsl.py 生成的是"我们猜的"TSL：顶层 version/profile、
  属性里带 functionMode/required/desc …… 这些字段是从**官方 API 文档**
  推出来的，不是从这台产品上真实导出的。

  而控制台的「导入物模型」校验有多严、认不认某些字段，**没有公开文档**。
  盲导入的结果只有两种：成功，或者被拒（且报错信息往往很含糊）。

  更麻烦的是：产品 `lot_1` 建的时候选的产品类型是「环境监测」，
  平台**已经自动塞了一条标准功能点**进去：

      功能类别=标准   功能名称=温度   标识符=`Current Temperature`

  所以"整体替换"和"追加合并"会得到完全不同的结果，而盲导入
  属于哪一种并不明确。

  ⇒ 正确顺序是**反过来**：拿平台自己导出的文件当骨架，
     我们只往它的 properties 里**追加**，别的字段一概不动。
     这样格式一定是平台认的（因为它就是平台产出的）。

============================================================================
 用法

    # 1) 控制台：产品 →「物模型」→「查看/导出物模型」→ 复制 JSON 存成文件
    #    例如存到 scratch/onenet_console_export.json

    # 2) 合并（只看结果不写文件）
    python tools/merge_onenet_tsl.py --base scratch/onenet_console_export.json --dry-run

    # 3) 正式写出来
    python tools/merge_onenet_tsl.py --base scratch/onenet_console_export.json \\
                                     --out doc/onenet_tsl_merged.json

    # 4) 控制台「设置物模型」→「导入物模型」→ 选 doc/onenet_tsl_merged.json
    #    →「导入」→ ★ 点「保存」

============================================================================
 它做的三件事

  ① **保留** base 的所有顶层字段（version / profile / events / services / ...）
     —— 一个都不改，也不删。profile 段尤其重要：里面是平台自己的
     productId / categoryId，我们猜的那个很可能是错的。

  ② **保留** base 里已有的属性（标准功能点 `Current Temperature` 等）。
     默认只**追加**缺失的；`--replace-existing` 才会覆盖同名标识符。
     默认不覆盖是有意的：标准功能点有时不允许删除，
     覆盖它可能连带把导入搞挂，而它对我们无害（就是多个用不上的点）。

  ③ **把我们的属性对象裁剪成 base 的字段集**。
     这是本工具最关键的一步：base 的属性对象里出现哪些键，
     我们输出的属性就只带哪些键。
     理由：如果 base 里没有 `required` 而平台其实不认这个字段，
     我们硬塞进去就可能被拒 —— 而"多一个字段"这种事
     平台**不会**给出有用的报错。
     反过来，base 里有而我们没有的键（比如 `required`），
     就从 base 里**同类型**的属性抄一份默认值。
"""

import argparse
import json
import os
import sys

# ⚠️ 编码守卫：本脚本会打印 ⚠️ 等非 GBK 字符。
#    默认中文控制台（GBK）下 print 会 UnicodeEncodeError 直接崩，
#    现象是"报告打印到一半断掉"，看着像合并逻辑出错。
#    tools/check_console_encoding.py 会盯着这件事。
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(errors="replace")
    except (AttributeError, ValueError):
        pass

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.dirname(_HERE)
DEFAULT_ADD = os.path.join(_REPO_ROOT, "doc", "onenet_tsl.json")


# ---------------------------------------------------------------------------
#  读入
# ---------------------------------------------------------------------------

def load_json(path, what):
    if not os.path.isfile(path):
        print("[ERROR] {}不存在：{}".format(what, path), file=sys.stderr)
        return None
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except ValueError as exc:
        print("[ERROR] {}不是合法 JSON：{}\n        {}".format(
            what, path, exc), file=sys.stderr)
        return None


def collect_props(doc, what):
    """取出 properties 数组，并做基本形状校验"""
    if not isinstance(doc, dict):
        print("[ERROR] {}的顶层不是对象（JSON 得是 {{...}}）".format(what),
              file=sys.stderr)
        return None
    props = doc.get("properties")
    if props is None:
        print("[ERROR] {}里没有 properties 字段。".format(what), file=sys.stderr)
        print("        如果导出的是「属性列表」而不是完整物模型，"
              "请换用完整导出。", file=sys.stderr)
        return None
    if not isinstance(props, list):
        print("[ERROR] {}的 properties 不是数组".format(what), file=sys.stderr)
        return None
    return props


def key_set(props):
    """属性对象里出现过的所有键（按首次出现顺序，输出稳定）"""
    seen = []
    for p in props:
        if not isinstance(p, dict):
            continue
        for k in p:
            if k not in seen:
                seen.append(k)
    return seen


def sample_of_type(props, dtype):
    """找一个 dataType.type == dtype 的属性当"抄默认值"的样板"""
    for p in props:
        dt = p.get("dataType")
        if isinstance(dt, dict) and dt.get("type") == dtype:
            return p
    return None


# ---------------------------------------------------------------------------
#  合并
# ---------------------------------------------------------------------------

def adapt_property(prop, base_keys, base_props, dropped):
    """把我们的属性裁剪/补齐成 base 的字段集

    返回新的 dict。**不修改**入参。

    dropped 是一个 {键名: 出现次数} 的累加器 —— 用来在报告里
    汇总"我们带了但 base 没有、因此被丢掉的键"。

    ⚠️ 这里有个刻意的地方：**每一个** 不在 base_keys 里的键都会被丢掉，
    并且**每一个**都要记进 dropped。
    第一版实现只遍历白名单 IDENTITY_KEYS，结果 `functionMode` 这种
    "在白名单外、base 里也没有"的键被**静默丢弃** —— 报告里一个字都不提。
    这类"悄悄改了"比直接报错危险得多：用户以为导入了原样，
    实际少了字段，出问题也无从查起。
    """
    out = {}
    dtype = (prop.get("dataType") or {}).get("type")
    ident = prop.get("identifier")

    # ① 逐个键过一遍：base 里有就留，没有就丢 + 记账
    for k, v in prop.items():
        if k == "dataType":
            continue                    # dataType 单独按 base 的形状裁（见下）
        if k in base_keys:
            out[k] = v
        else:
            dropped[k] = dropped.get(k, 0) + 1

    # ② dataType 内部也按 base 的形状裁（specs 的键名可能不同）
    src_dt = prop.get("dataType")
    if isinstance(src_dt, dict):
        dt_keys = None
        for bp in base_props:
            bdt = bp.get("dataType")
            if isinstance(bdt, dict):
                dt_keys = list(bdt.keys())
                break
        if dt_keys is None:
            out["dataType"] = src_dt
        else:
            newdt = {}
            for k in dt_keys:
                if k in src_dt:
                    newdt[k] = src_dt[k]
                else:
                    dropped["dataType." + k] = dropped.get(
                        "dataType." + k, 0) + 1
            # specs 里的键 base 可能没列全（比如 string 的 length）
            specs = src_dt.get("specs")
            if isinstance(specs, dict) and isinstance(newdt.get("specs"), dict):
                for k, v in specs.items():
                    if k not in newdt["specs"]:
                        dropped["specs." + k] = dropped.get(
                            "specs." + k, 0) + 1
            out["dataType"] = newdt

    # ③ base 里有、我们没有的键 ⇒ 从同类型的 base 属性抄一份
    for k in base_keys:
        if k in out:
            continue
        donor = sample_of_type(base_props, dtype)
        if donor is not None and k in donor:
            out[k] = donor[k]
            dropped.setdefault("_filled", []).append(
                "{!r}.{} <- {!r} 的默认值 {!r}".format(
                    ident, k, donor.get("identifier"), donor[k]))
    return out


def merge(base, add, replace_existing=False, drop=()):
    """返回 (merged_doc, report)

    drop 是要从 base 里**删除**的标识符集合（例如产品类目自带、
    但本项目用不到的标准功能点）。
    """
    base_props = base.get("properties") or []
    add_props = add.get("properties") or []

    bkeys = key_set(base_props)
    report = {
        "base_count": len(base_props),
        "add_count": len(add_props),
        "added": [],
        "skipped": [],
        "replaced": [],
        "warnings": [],
        "dropped": {},          # {键名: 被丢掉几次}（属性**字段**被裁掉）
        "filled": [],           # 从 base 抄默认值的明细
        "base_keys": bkeys,
        "removed": [],          # 从 base 删掉的**属性**
        "remove_missing": [],   # 要求删但没找到的标识符
    }

    # ---- ① 先按 drop 删 base 里的属性 ----
    # ⚠️ "要求删但没找到"必须报告，不能静默忽略：
    #    否则用户以为删掉了，实际上平台上还留着 —— 而平台上多一个
    #    用不到的功能点，在控制台里看起来跟"删成功了"没区别。
    drop_set = {d for d in drop if d}
    if drop_set:
        keep = []
        for p in base_props:
            ident = p.get("identifier") if isinstance(p, dict) else None
            if ident in drop_set:
                report["removed"].append(ident)
                drop_set.discard(ident)
                continue
            keep.append(p)
        base_props = keep
        report["remove_missing"] = sorted(drop_set)

    existing = {}
    for p in base_props:
        if isinstance(p, dict) and p.get("identifier"):
            existing[p["identifier"]] = p

    merged_props = list(base_props)
    dropped = report["dropped"]

    for prop in add_props:
        ident = prop.get("identifier")
        if not ident:
            report["warnings"].append("我们的属性里有缺 identifier 的，已跳过")
            continue
        if ident in existing:
            if not replace_existing:
                report["skipped"].append(ident)
                continue
            report["replaced"].append(ident)
            merged_props = [p for p in merged_props
                            if not (isinstance(p, dict)
                                    and p.get("identifier") == ident)]
        report["added"].append(ident)
        adapted = adapt_property(prop, bkeys, base_props, dropped)
        report["filled"].extend(dropped.pop("_filled", []))
        merged_props.append(adapted)

    out = dict(base)                    # ① 顶层字段一个不动
    out["properties"] = merged_props
    return out, report


# ---------------------------------------------------------------------------
#  打印
# ---------------------------------------------------------------------------

def print_report(report, out_path, dry_run):
    r = report
    print("=" * 70)
    print(" 合并结果")
    print("=" * 70)
    print(" base 原有属性 : {} 个".format(r["base_count"]))
    if r["removed"]:
        print(" 删除(--drop) : {} 个  -> {}".format(
            len(r["removed"]), ", ".join(r["removed"])))
    print(" 我们提供     : {} 个".format(r["add_count"]))
    print(" 追加         : {} 个".format(len(r["added"])))
    if r["skipped"]:
        print(" 跳过(已存在) : {} 个  -> {}".format(
            len(r["skipped"]), ", ".join(r["skipped"])))
    if r["replaced"]:
        print(" 覆盖(已存在) : {} 个  -> {}".format(
            len(r["replaced"]), ", ".join(r["replaced"])))
    print(" 合并后总数   : {} 个".format(
        r["base_count"] - len(r["removed"])
        + len(r["added"]) - len(r["replaced"])))
    print()
    print(" base 的属性字段集（我们按它裁剪）:")
    print("   " + ", ".join(repr(k) for k in r["base_keys"]))

    if r["remove_missing"]:
        print()
        print("-" * 70)
        print(" ⚠️ --drop 要求删除、但 base 里**没找到**的标识符")
        print("-" * 70)
        for k in r["remove_missing"]:
            print("   {!r}".format(k))
        print()
        print("   拼错名字就会这样 —— 不会报错，只是**什么都没删**。")
        print("   所以这里必须喊出来：否则你会在控制台看到那个功能点还在，")
        print("   却以为'删过了'。请核对导出 JSON 里的 identifier。")

    if r["dropped"]:
        print()
        print("-" * 70)
        print(" ⚠️ 被丢掉的键（我们的属性带了，但 base 里没有）")
        print("-" * 70)
        for k, n in sorted(r["dropped"].items()):
            print("   {:<20} 出现在 {} 个属性里".format(repr(k), n))
        print()
        print("   这是**有意**的：base 里没有的字段，平台可能不认，")
        print("   而'多一个字段'平台不会给有用的报错，所以宁可不要。")
        print("   如果丢掉的是你需要的字段，说明该字段平台确实不支持，")
        print("   请改成别的方式表达（例如并进 desc）。")

    if r["filled"]:
        print()
        print("-" * 70)
        print(" 从 base 抄了默认值的键（{} 处）".format(len(r["filled"])))
        print("-" * 70)
        for f in r["filled"]:
            print("   " + f)

    if r["warnings"]:
        print()
        print("-" * 70)
        print(" 警告")
        print("-" * 70)
        for w in r["warnings"]:
            print("   " + w)

    print()
    print("=" * 70)
    if dry_run:
        print(" --dry-run：没有写文件。去掉该参数即可生成。")
    else:
        print(" 已写出：{}".format(out_path))
        print()
        print(" 导入步骤：")
        print("   产品管理 -> 对应产品「详情」->「设置物模型」")
        print("   ->「导入物模型」-> 选上面这个文件 ->「导入」")
        print("   -> ★ **点「保存」**（漏了不生效，且现象仍是 code=2306）")
        print()
        print(" 导入后验证：")
        print("   python tools/onenet_probe.py --probe-props")
    print("=" * 70)


# ---------------------------------------------------------------------------
#  自检（不联网、不需要真的控制台导出）
# ---------------------------------------------------------------------------

#: 自检用的"控制台导出"仿真 —— 照 2026-10-15 从 lot_1 真实导出的内容
#: 简化而成：profile 用真实值，属性含 2 条平台标准点 + 1 条用户自定义点。
#: ⚠️ 注意标识符是 CurrentTemperature（**没有空格**）——
#:    控制台界面上因列宽折行看起来像有空格，以导出 JSON 为准。
SELFTEST_BASE = {
    "version": "1.0",
    "profile": {"industryId": "11", "sceneId": "36", "categoryId": "14",
                "productId": "<YOUR_PRODUCT_ID>"},
    "properties": [
        {"identifier": "CurrentTemperature", "name": "温度",
         "accessMode": "rw", "functionType": "st", "desc": "",
         "dataType": {"type": "float",
                      "specs": {"min": "-40", "max": "90", "step": "0.1",
                                "unit": "摄氏度 / ℃"}},
         "functionMode": "property", "required": False},
        {"identifier": "LightLux", "name": "光照度",
         "accessMode": "rw", "functionType": "st", "desc": "",
         "dataType": {"type": "double",
                      "specs": {"min": "0", "max": "100000", "step": "0.01",
                                "unit": "流明 / lx"}},
         "functionMode": "property", "required": False},
        {"identifier": "alarm", "name": "告警类型",
         "accessMode": "rw", "functionType": "u", "desc": "",
         "dataType": {"type": "int32",
                      "specs": {"min": "0", "max": "255", "step": "",
                                "unit": ""}},
         "functionMode": "property", "required": False},
    ],
    "events": [], "services": [], "combs": [],
}

#: 自检里"要删掉"的那几个（本项目用不到的类目自带点）
SELFTEST_DROP = ["LightLux", "alarm"]


def selftest():
    fails = []

    def chk(cond, msg):
        if not cond:
            fails.append(msg)

    add = load_json(DEFAULT_ADD, "add")
    if add is None:
        print("[FAIL] 读不到 {}".format(DEFAULT_ADD))
        return 1

    base_ids = [p["identifier"] for p in SELFTEST_BASE["properties"]]
    add_ids = [p["identifier"] for p in add["properties"]]

    # ⓪ 前置：我们的清单里必须**含** CurrentTemperature（温度用平台标准点）
    chk("CurrentTemperature" in add_ids,
        "我们的属性清单里没有 CurrentTemperature")
    chk("CurrentTemperature" in base_ids,
        "自检 base 里没有 CurrentTemperature")
    # ⓪' 而且不该再有旧的 temp
    chk("temp" not in add_ids,
        "我们的属性清单里还留着旧的 temp（应当已改为 CurrentTemperature）")

    # ① 基本合并（不 drop）
    merged, rep = merge(SELFTEST_BASE, add)
    props = merged["properties"]
    # CurrentTemperature 两边都有 ⇒ 默认跳过，不重复添加
    n_skip = 1
    chk(len(props) == len(base_ids) + len(add_ids) - n_skip,
        "合并后应有 {} 个属性，实际 {}".format(
            len(base_ids) + len(add_ids) - n_skip, len(props)))
    chk(rep["skipped"] == ["CurrentTemperature"],
        "CurrentTemperature 应被跳过，实际 {}".format(rep["skipped"]))

    # ② 顶层字段一个都不能变
    for k, v in SELFTEST_BASE.items():
        if k == "properties":
            continue
        chk(merged.get(k) == v, "顶层字段 {!r} 被改动了".format(k))

    # ③ base 里原有的属性必须原样保留（一个字都不许动）
    for bp in SELFTEST_BASE["properties"]:
        got = [p for p in props if p.get("identifier") == bp["identifier"]]
        chk(len(got) == 1,
            "base 原有属性 {!r} 丢了".format(bp["identifier"]))
        if got:
            chk(got[0] == bp,
                "base 原有属性 {!r} 被改动了".format(bp["identifier"]))

    # ④ 我们的属性必须都在
    got_ids = [p["identifier"] for p in props]
    for i in add_ids:
        chk(i in got_ids, "我们的属性 {!r} 没进去".format(i))

    # ⑤ base 里**有** functionMode ⇒ 必须保留（不能被裁掉）
    #    （第一版自检假设 base 没有这个键，与真实导出不符；
    #      真实导出里是有的，所以这里改成断言"保留"。）
    for p in props:
        if p["identifier"] == "CurrentTemperature":
            continue
        chk("functionMode" in p,
            "属性 {!r} 丢了 functionMode（base 里是有的）".format(
                p["identifier"]))
    chk(not rep["dropped"].get("functionMode"),
        "functionMode 不该被记为丢弃: {}".format(rep["dropped"]))

    # ⑥ base 里有、我们没有的键要补上
    for p in props:
        if p["identifier"] == "CurrentTemperature":
            continue
        for k in ("required", "desc"):
            chk(k in p, "属性 {!r} 缺 {!r}".format(p["identifier"], k))

    # ⑦ 重复标识符默认跳过，不重复添加
    #    ⚠️ base 要用 **merged**（已含我们的属性），否则 existing 里
    #    根本没有它们，"跳过"这条逻辑压根不会被触发 ——
    #    第一版这里传的是 SELFTEST_BASE，于是断言假通过/假失败各一次。
    merged2, rep2 = merge(merged, add)
    chk(len(merged2["properties"]) == len(props),
        "再合并一次属性数变了（重复添加）")
    chk(len(rep2["skipped"]) == len(add_ids),
        "应跳过 {} 个，实际 {}".format(len(add_ids), len(rep2["skipped"])))
    chk("humi" in rep2["skipped"], "已存在的 humi 没有被跳过")

    # ⑧ --replace-existing 时是覆盖不是新增
    merged3, rep3 = merge(merged, add, replace_existing=True)
    chk(len(merged3["properties"]) == len(props),
        "replace-existing 下属性数变了: {}".format(
            len(merged3["properties"])))
    chk(len(rep3["replaced"]) == len(add_ids),
        "应覆盖 {} 个，实际 {}".format(len(add_ids), len(rep3["replaced"])))
    chk("humi" in rep3["replaced"], "replace-existing 没报告 replaced")
    chk(not rep3["skipped"], "replace-existing 下不该有 skipped")

    # ⑨ dataType 的 specs 键也要按 base 裁
    for p in props:
        if p["identifier"] == "CurrentTemperature":
            continue
        dt = p.get("dataType") or {}
        chk("type" in dt, "属性 {!r} 的 dataType 缺 type".format(
            p["identifier"]))

    # ⑩ --drop：删掉指定的原有属性
    merged4, rep4 = merge(SELFTEST_BASE, add, drop=SELFTEST_DROP)
    ids4 = [p["identifier"] for p in merged4["properties"]]
    chk(sorted(rep4["removed"]) == sorted(SELFTEST_DROP),
        "--drop 没报告删对: {}".format(rep4["removed"]))
    for d in SELFTEST_DROP:
        chk(d not in ids4, "--drop 后 {!r} 还在".format(d))
    chk("CurrentTemperature" in ids4,
        "--drop 误删了 CurrentTemperature")
    chk(len(ids4) == len(base_ids) - len(SELFTEST_DROP)
        + len(add_ids) - n_skip,
        "--drop 后总数不对: {}".format(len(ids4)))
    chk(not rep4["remove_missing"],
        "不该有 remove_missing: {}".format(rep4["remove_missing"]))

    # ⑪ --drop 拼错名字时必须**喊出来**，不能静默忽略
    _, rep5 = merge(SELFTEST_BASE, add, drop=["LightLux", "NoSuchPoint"])
    chk(rep5["remove_missing"] == ["NoSuchPoint"],
        "拼错的 --drop 名字没被报告: {}".format(rep5["remove_missing"]))
    chk(rep5["removed"] == ["LightLux"],
        "能删的那个没删: {}".format(rep5["removed"]))

    # ⑫ --drop 不能影响顶层字段
    for k, v in SELFTEST_BASE.items():
        if k == "properties":
            continue
        chk(merged4.get(k) == v,
            "--drop 后顶层字段 {!r} 被改动了".format(k))

    # ⑬ 结果必须能被 json 序列化（中文不转义也能）
    try:
        s = json.dumps(merged, ensure_ascii=False)
        chk("温度" in s, "中文被转义了")
        json.loads(s)
    except Exception as exc:            # noqa: BLE001
        fails.append("JSON 往返失败: {}".format(exc))

    if fails:
        print("[FAIL] merge_onenet_tsl 自检未通过：")
        for f in fails:
            print("  - {}".format(f))
        return 1
    print("[ OK ] merge_onenet_tsl 自检通过（14 组断言）")
    return 0


# ---------------------------------------------------------------------------
#  主流程
# ---------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="把我们的属性合并进控制台导出的物模型（保留平台自己的格式）",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base",
                    help="★ 控制台「查看/导出物模型」导出的 JSON（当格式骨架）")
    ap.add_argument("--add", default=DEFAULT_ADD,
                    help="我们的属性来源（默认 doc/onenet_tsl.json）")
    ap.add_argument("--out", default=None,
                    help="输出文件（默认 doc/onenet_tsl_merged.json）")
    ap.add_argument("--drop", action="append", default=[],
                    metavar="IDENT",
                    help="★ 从 base 里**删除**指定标识符（可重复，"
                         "也可用逗号分隔）。用于清掉产品类目自带、"
                         "本项目用不到的标准功能点，例如 "
                         "--drop LightLux,RelativeHumidity,alarm")
    ap.add_argument("--replace-existing", action="store_true",
                    help="标识符已存在时覆盖（默认跳过，更安全）")
    ap.add_argument("--dry-run", action="store_true",
                    help="只报告不写文件")
    ap.add_argument("--selftest", action="store_true",
                    help="自检（不联网、不需要控制台导出）")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    if not args.base:
        ap.error("需要 --base（控制台「查看/导出物模型」导出的 JSON）\n"
                 "       如果只想验证工具本身，用 --selftest")

    out_path = args.out or os.path.join(_REPO_ROOT, "doc",
                                        "onenet_tsl_merged.json")

    base = load_json(args.base, "base（控制台导出）")
    add = load_json(args.add, "add（我们的属性）")
    if base is None or add is None:
        return 2

    if collect_props(base, "base") is None:
        return 2
    if collect_props(add, "add") is None:
        return 2

    # --drop 支持 "A,B" 与多次 --drop A --drop B 两种写法
    drop = []
    for item in args.drop:
        for part in str(item).split(","):
            part = part.strip()
            if part:
                drop.append(part)

    merged, report = merge(base, add, replace_existing=args.replace_existing,
                           drop=drop)

    print_report(report, out_path, args.dry_run)

    if args.dry_run:
        return 0

    # ⚠️ 有拼错的 --drop 名字时**不写文件**：写出去会让人以为删干净了。
    if report["remove_missing"]:
        print()
        print("[ERROR] --drop 里有 base 中不存在的标识符：{}".format(
            ", ".join(report["remove_missing"])))
        print("        为避免「以为删了其实没删」，本次**不写文件**。")
        print("        请核对导出 JSON 里的 identifier 后重跑。")
        return 3

    outdir = os.path.dirname(out_path)
    if outdir and not os.path.isdir(outdir):
        os.makedirs(outdir, exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as fh:
        json.dump(merged, fh, ensure_ascii=False, indent=2)
        fh.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
