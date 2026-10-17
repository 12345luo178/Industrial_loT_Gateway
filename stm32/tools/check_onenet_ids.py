#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
OneNET 物模型属性标识符 —— 三处一致性检查

============================================================================
 它防的是什么

  "属性标识符"这件事在本项目里写了 **三份**：

      ① esp32/include/esp32_json.h   ESP32_ONENET_ID_* 宏（固件真正用的）
      ② tools/onenet_probe.py        PROP_CHECKS（PC 探针点名用的）
      ③ tools/gen_onenet_tsl.py      PROPERTIES（生成物模型用的）

  三份只要有一处不一致，现象**完全一样**：

      平台回 {"code":2306,"msg":"identifier not exist:identifier:xxx"}

  而平台**一次只报一个**缺失的属性。所以三处漂移之后，
  你要么手工逐字核对三份表，要么来回十几轮"改一个试一次"。

  ⇒ 这个检查把三处钉在一起。任何一处改错，CI 当场红。

============================================================================
 检查项

  A. 宏集合 == 探针集合（逐个比对，报告只在某一边出现的）
  B. 宏集合 == 生成器集合
  C. 每个宏在 esp32_json.c 里**至少被引用一次**
     —— 这一条同时兜住"漏改字面量"：如果有人把
        jw_prop_u32(&w, ESP32_ONENET_ID_SEQ, ...) 写回
        jw_prop_u32(&w, "seq", ...)，对应的宏就没人用了，C 会红。
  D. 宏的值互不重复（两个宏指向同一个标识符必然是笔误）
  E. 若 doc/onenet_tsl_merged.json 存在，宏集合必须是它的**子集**
     （合并文件里还会含平台自带的标准功能点，所以是子集而非相等）

  刻意**不**做的事：扫描 esp32_json.c 里的字符串字面量找"漏改"。
  因为 "seq"/"status"/"humi" 这些在**通用后端（EMQX）**里是合法字面量
  （那是另一份契约，用扁平 JSON），朴素扫描会全是误报。
  C 已经用更可靠的方式覆盖了同一个风险。

============================================================================
 用法

    python tools/check_onenet_ids.py

  返回码 0 = 通过；1 = 有不一致。
"""

import io
import json
import os
import re
import sys

# ⚠️ 编码守卫：本脚本会打印 ⚠️ 等非 GBK 字符。
#    默认中文控制台（GBK）下 print 会 UnicodeEncodeError 直接崩。
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(errors="replace")
    except (AttributeError, ValueError):
        pass

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.dirname(_HERE)

HEADER = os.path.join(_REPO_ROOT, "esp32", "include", "esp32_json.h")
IMPL = os.path.join(_REPO_ROOT, "esp32", "src", "esp32_json.c")
MERGED = os.path.join(_REPO_ROOT, "doc", "onenet_tsl_merged.json")

#: `#define ESP32_ONENET_ID_XXX "值"`
_MACRO_RE = re.compile(
    r'^[ \t]*#define[ \t]+(ESP32_ONENET_ID_[A-Z0-9_]+)[ \t]+"([^"]*)"',
    re.MULTILINE)


def read_text(path):
    """按 UTF-8 读，去掉 BOM。

    ⚠️ 不能直接用 open(encoding="utf-8") 然后假设没有 BOM ——
       本项目有专门的 check_bom.py 在防这件事，但那是另一个步骤，
       这里自己兜住，免得因为一个 BOM 就报"宏一个都没找到"。
    """
    with io.open(path, "r", encoding="utf-8-sig", errors="replace") as fh:
        return fh.read()


def parse_macros(text):
    """返回 [(宏名, 值), ...]，按出现顺序"""
    return [(m.group(1), m.group(2)) for m in _MACRO_RE.finditer(text)]


def load_probe_ids():
    """从 tools/onenet_probe.py 取 PROP_CHECKS 的标识符列表"""
    sys.path.insert(0, _HERE)
    from onenet_probe import PROP_CHECKS            # noqa: PLC0415
    return [p[0] for p in PROP_CHECKS]


def load_tsl_ids():
    """从 tools/gen_onenet_tsl.py 取 PROPERTIES 的标识符列表"""
    sys.path.insert(0, _HERE)
    from gen_onenet_tsl import PROPERTIES           # noqa: PLC0415
    return [p[0] for p in PROPERTIES]


def diff_sets(a, b):
    """返回 (只在 a 里的, 只在 b 里的)，保持 a/b 的顺序"""
    sa, sb = set(a), set(b)
    return ([x for x in a if x not in sb], [x for x in b if x not in sa])


def main():
    fails = []
    warns = []

    # ---- 读 C 头文件 ----
    if not os.path.isfile(HEADER):
        print("[ERROR] 找不到 {}".format(HEADER), file=sys.stderr)
        return 1
    hdr = read_text(HEADER)
    macros = parse_macros(hdr)

    if not macros:
        print("[ERROR] 在 {} 里一个 ESP32_ONENET_ID_* 宏都没找到。".format(
            os.path.relpath(HEADER, _REPO_ROOT)), file=sys.stderr)
        print("        宏的写法必须是：#define ESP32_ONENET_ID_XXX \"值\"",
              file=sys.stderr)
        return 1

    macro_names = [m[0] for m in macros]
    macro_vals = [m[1] for m in macros]

    print("=" * 70)
    print(" OneNET 物模型属性标识符 —— 三处一致性检查")
    print("=" * 70)
    print(" ① 固件宏（esp32_json.h）      : {} 个".format(len(macros)))

    # ---- A. 与探针比对 ----
    try:
        probe_ids = load_probe_ids()
    except Exception as exc:                        # noqa: BLE001
        print("[ERROR] 导入 tools/onenet_probe.py 失败：{}".format(exc),
              file=sys.stderr)
        return 1
    print(" ② PC 探针（onenet_probe.py）  : {} 个".format(len(probe_ids)))

    only_m, only_p = diff_sets(macro_vals, probe_ids)
    if only_m or only_p:
        fails.append("宏与探针的标识符集合不一致")
        if only_m:
            print("   [FAIL] 只在固件宏里有: {}".format(only_m))
        if only_p:
            print("   [FAIL] 只在探针里有  : {}".format(only_p))
    else:
        print("   [ OK ] 与探针一致")

    # ---- B. 与 TSL 生成器比对 ----
    try:
        tsl_ids = load_tsl_ids()
    except Exception as exc:                        # noqa: BLE001
        print("[ERROR] 导入 tools/gen_onenet_tsl.py 失败：{}".format(exc),
              file=sys.stderr)
        return 1
    print(" ③ TSL 生成器（gen_onenet_tsl）: {} 个".format(len(tsl_ids)))

    only_m2, only_t = diff_sets(macro_vals, tsl_ids)
    if only_m2 or only_t:
        fails.append("宏与 TSL 生成器的标识符集合不一致")
        if only_m2:
            print("   [FAIL] 只在固件宏里有: {}".format(only_m2))
        if only_t:
            print("   [FAIL] 只在 TSL 里有  : {}".format(only_t))
    else:
        print("   [ OK ] 与 TSL 生成器一致")

    # ---- C. 每个宏必须在 esp32_json.c 里被引用 ----
    impl = read_text(IMPL) if os.path.isfile(IMPL) else ""
    unused = [n for n in macro_names
              if not re.search(r'\b' + re.escape(n) + r'\b', impl)]
    if unused:
        fails.append("有宏定义了但没被使用")
        print("   [FAIL] 以下宏在 esp32_json.c 里一次都没用到：")
        for n in unused:
            print("            {}".format(n))
        print("          => 多半是某处又写回了字符串字面量。")
        print("             宏没人用就不会有人发现它过期了 ——")
        print("             这类'定义了但没接上'的漂移最难查。")
    else:
        print("   [ OK ] {} 个宏都在 esp32_json.c 里被引用".format(
            len(macro_names)))

    # ---- D. 宏的值必须互不重复 ----
    seen = {}
    dups = []
    for name, val in macros:
        if val in seen:
            dups.append((val, seen[val], name))
        else:
            seen[val] = name
    if dups:
        fails.append("有两个宏指向同一个标识符")
        print("   [FAIL] 宏的值重复：")
        for val, a, b in dups:
            print("            {!r} 同时属于 {} 和 {}".format(val, a, b))
    else:
        print("   [ OK ] 宏的值互不重复")

    # ---- E. 与已生成的合并物模型比对（子集）----
    if os.path.isfile(MERGED):
        try:
            with io.open(MERGED, "r", encoding="utf-8-sig") as fh:
                doc = json.load(fh)
            merged_ids = [p.get("identifier")
                          for p in (doc.get("properties") or [])]
        except Exception as exc:                    # noqa: BLE001
            warns.append("读 {} 失败：{}".format(
                os.path.relpath(MERGED, _REPO_ROOT), exc))
            merged_ids = None

        if merged_ids is not None:
            missing = [v for v in macro_vals if v not in merged_ids]
            if missing:
                fails.append("有标识符不在合并后的物模型里")
                print("   [FAIL] 以下标识符在 {} 里不存在：".format(
                    os.path.relpath(MERGED, _REPO_ROOT)))
                for m in missing:
                    print("            {}".format(m))
                print("          => 固件会报这个属性，而平台不认识 ⇒ code=2306")
            else:
                print("   [ OK ] {} 个标识符都在合并物模型里"
                      "（合并文件共 {} 个属性，含平台自带的标准点）".format(
                          len(macro_vals), len(merged_ids)))
    else:
        print("   [跳过] 还没有 {}（控制台导出后生成）".format(
            os.path.relpath(MERGED, _REPO_ROOT)))

    # ---- 收尾 ----
    print()
    if warns:
        for w in warns:
            print("[WARN] {}".format(w))
    if fails:
        print("=" * 70)
        print("[FAIL] OneNET 标识符一致性检查未通过（{} 项）".format(len(fails)))
        for f in fails:
            print("  - {}".format(f))
        print()
        print("  提示：标识符是**云端契约**，大小写敏感。改名字时必须")
        print("  同时改 ① esp32/include/esp32_json.h  ② tools/onenet_probe.py")
        print("  ③ tools/gen_onenet_tsl.py  ④ 控制台物模型（重新导入+保存）")
        print("=" * 70)
        return 1

    print("=" * 70)
    print("[ OK ] 三处标识符完全一致（{} 个）".format(len(macro_vals)))
    print("=" * 70)
    return 0


if __name__ == "__main__":
    sys.exit(main())
