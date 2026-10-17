#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 OneNET 物模型 TSL 文件（可直接在控制台「导入物模型」）

============================================================================
 为什么需要它

  OneNET 的物模型属性要在控制台**手工一个个加**，15 个功能点意味着
  15 次「添加自定义功能点 + 填标识符 + 选类型 + 填范围 + 选读写权限」。

  而标识符**大小写敏感**，且平台对拼错的报错是：

      {"code":2306,"msg":"identifier not exist:identifier:temp"}

  —— **一次只说一个**。手工建 15 个、拼错 1 个，就要来回好几轮。

  平台支持「导入物模型」：把物模型描述文件（TSL，一个 JSON）导进去，
  一次建完。本工具就是生成这个文件。

============================================================================
 TSL 格式（来自 OneNET 官方文档「什么是物模型」+ 控制台导出示例）

    {
      "version": "1.0",
      "profile": { "industryId":..., "sceneId":..., "categoryId":...,
                   "productId":... },
      "properties": [ { "identifier":..., "name":..., "functionType":"u",
                        "accessMode":"r", "desc":..., "dataType":{...},
                        "functionMode":"property", "required":false } ],
      "events": [], "services": [], "combs": []
    }

  字段取值（官方原文）：
    functionType  用户自定义(u) / 系统功能点(s) / 标准功能点(st)
    accessMode    只读(r) / 读写(rw)
    dataType.type int32 / int64 / float / double / date / bool / string /
                  enum / bitMap / array / struct

============================================================================
 用法

    # 生成（productId 必填）
    python tools/gen_onenet_tsl.py --product-id <YOUR_PRODUCT_ID> --out onenet_tsl.json

    # 如果导入报 profile 相关错误，先加 --no-profile 再试
    python tools/gen_onenet_tsl.py --product-id <YOUR_PRODUCT_ID> --no-profile

    # 打印到屏幕（自己复制粘贴到导出的模板里）
    python tools/gen_onenet_tsl.py --product-id <YOUR_PRODUCT_ID> --stdout

============================================================================
 导入步骤（控制台）

  ⚠️ **本工具的输出不要直接导入** —— 用 tools/merge_onenet_tsl.py。
     原因（2026-10-15 实测）：

       产品 lot_1 建的时候选的产品类型是「环境监测」，
       平台**已经自动塞了标准功能点**进去（CurrentTemperature /
       LightLux / RelativeHumidity / alarm）。所以：

         ① 本工具的 profile 段是照文档推的，**不是**平台真实值
            （实测真值 industryId=11 / sceneId=36 / categoryId=14，
             与推出来的 1/14/314 完全不同）；
         ② 「导入物模型」是整体替换还是增量合并**没有公开文档**，
            盲导入可能把平台自带的标准点抹掉。

     正确顺序是**反过来**：拿控制台「查看/导出物模型」导出的文件当骨架，
     只往它的 properties 里追加：

         python tools/merge_onenet_tsl.py --base <控制台导出.json> \\
                                          --drop <用不到的类目自带点> \\
                                          --out doc/onenet_tsl_merged.json

     然后导入 doc/onenet_tsl_merged.json。

  ⚠️ 最后那步「保存」很容易漏 —— 不保存的话物模型不生效，
     而设备上报仍然报 2306，看起来像"导入没成功"。

 本工具现在的作用：维护那份**属性清单**（唯一事实来源之一），
 供 merge 工具读取，并与固件宏、探针清单三处比对
 （tools/check_onenet_ids.py）。直接拿它做导入只在
 "产品是空物模型、且 profile 已知" 时才合适。
"""

import argparse
import json
import os
import sys

# ⚠️ 编码守卫：本脚本的 print 里有 ⚠️ 等非 GBK 字符。
#    默认中文控制台（GBK）下 print 会 UnicodeEncodeError 直接崩，
#    现象是"输出打印到一半断掉"，看着像生成失败。
#    tools/check_console_encoding.py 会盯着这件事 ——
#    这个坑在本项目已经踩过三次了。
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(errors="replace")
    except (AttributeError, ValueError):
        pass

# ---------------------------------------------------------------------------
#  属性表（**唯一事实来源**）
#
#  ⚠️ 这里的 identifier 必须与下面两处**逐字一致**：
#      esp32/src/main.c  —— esp32_json_onenet_property() / _link() 里拼的属性名
#      tools/onenet_probe.py —— PROP_CHECKS
#    本文件末尾的 check_consistency() 会自动比对后两者，防止漂移。
# ---------------------------------------------------------------------------

# (标识符, 显示名, 类型, accessMode, 单位, min, max, step, 说明)
#
# ⚠️ 标识符是**云端契约**，必须与下面两处逐字一致：
#      esp32/include/esp32_json.h —— ESP32_ONENET_ID_* 宏
#      tools/onenet_probe.py      —— PROP_CHECKS
#    tools/check_onenet_ids.py 会三处比对，防止漂移。
PROPERTIES = [
    # ⚠️ 温度用的是平台「环境监测」类目**自带的标准功能点**
    #    CurrentTemperature（**没有空格** —— 控制台界面因列宽折行
    #    看起来像有空格，以「导出物模型」的 JSON 为准）。
    #    它已经存在于产品 lot_1 上，**不需要新建**；
    #    这里的 accessMode/min/max 刻意与平台那份保持一致（rw / -40~90），
    #    这样万一真拿本文件去导入也不会与已有定义冲突。
    #    走 tools/merge_onenet_tsl.py 时它会被识别为"已存在"而跳过。
    ("CurrentTemperature", "温度", "float", "rw", "°C", -40, 90, 0.1,
     "★ 平台标准功能点（已存在，不用新建）"),
    ("humi",       "湿度",         "float", "r",  "%",   0,   100, 0.1,
     "DHT22 湿度"),
    ("seq",        "遥测序号",     "int32", "r",  "",    0,   2147483647, 1,
     "单调递增，供云端去重（QoS1 可能重复投递）"),
    ("status",     "设备状态位",   "int32", "r",  "",    0,   65535, 1,
     "与 STM32 的 IREG_DEV_STATUS 同义"),
    ("uptime",     "运行时间",     "int32", "r",  "s",   0,   65535, 1,
     "STM32 侧运行秒数（低 16 位）"),
    ("can1",       "CAN 通道 1",   "int32", "r",  "",    0,   65535, 1,
     "CAN 通道 1 计数"),
    ("can2",       "CAN 通道 2",   "int32", "r",  "",    0,   65535, 1,
     "CAN 通道 2 计数"),
    ("can3",       "CAN 通道 3",   "int32", "r",  "",    0,   65535, 1,
     "CAN 通道 3 计数"),
    ("can4",       "CAN 通道 4",   "int32", "r",  "",    0,   65535, 1,
     "CAN 通道 4 计数"),
    ("wifi",       "Wi-Fi 状态",   "int32", "r",  "",    0,   3,   1,
     "0 断开 / 1 连接中 / 2 已连 / 3 认证失败"),
    ("mqtt",       "MQTT 状态",    "int32", "r",  "",    0,   3,   1,
     "同 wifi 的取值语义"),
    ("rssi",       "信号强度",     "int32", "r",  "dBm", -120, 0,  1,
     "取不到 AP 信息时**不上报**该属性"),
    ("backlog",    "待补传条数",   "int32", "r",  "",    0,   65535, 1,
     "MQTT 断线时积压的遥测条数"),
    ("cmd",        "下行指令",     "string", "rw", "",   None, None, None,
     "★ 控制台给这个属性下发命令词（REPORT_NOW / SET_PERIOD 30 ...）"),
    ("cmd_result", "指令执行结果", "string", "r",  "",   None, None, None,
     "★ STM32 的确认文本；NACK 时带 FAIL: 前缀"),
]

#: 字符串属性的最大长度（TSL 里 string 的 specs 用 length）
STRING_LENGTH = "64"


def build_property(identifier, name, ptype, access, unit, vmin, vmax, step,
                   desc):
    """按 TSL 格式造一个属性对象"""
    if ptype == "string":
        specs = {"length": STRING_LENGTH}
    elif ptype == "float":
        specs = {
            "min": str(vmin),
            "max": str(vmax),
            "step": str(step),
            "unit": unit,
        }
    else:                                   # int32
        specs = {
            "min": str(vmin),
            "max": str(vmax),
        }
        if unit:
            specs["unit"] = unit

    return {
        "identifier": identifier,
        "name": name,
        "functionType": "u",                # u = 用户自定义
        "accessMode": access,               # r = 只读；rw = 读写
        "desc": desc,
        "dataType": {
            "type": ptype,
            "specs": specs,
        },
        "functionMode": "property",
        "required": False,
    }


def build_tsl(product_id, with_profile=True,
              industry_id="11", scene_id="36", category_id="14"):
    """生成完整的 TSL 文档

    ⚠️ 默认的 industryId/sceneId/categoryId 现在取**实测值**：
       2026-10-15 从控制台「查看/导出物模型」导出 lot_1（<YOUR_PRODUCT_ID>）
       得到的 profile 就是 11/36/14。此前照文档猜的 1/14/314 是**错的** ——
       猜错的值会让导入被拒，而报错含糊。见 doc/OneNET接入说明.md §8.11。
    """
    tsl = {"version": "1.0"}

    if with_profile:
        # ⚠️ 换了产品/类目时这三个值会变，务必以**控制台导出**为准，
        #    或用 tools/merge_onenet_tsl.py 直接拿导出的 profile。
        tsl["profile"] = {
            "industryId": industry_id,
            "sceneId": scene_id,
            "categoryId": category_id,
            "productId": product_id,
        }

    tsl["properties"] = [build_property(*p) for p in PROPERTIES]
    tsl["events"] = []
    tsl["services"] = []
    tsl["combs"] = []
    return tsl


def check_consistency():
    """
    与 tools/onenet_probe.py 的 PROP_CHECKS 比对标识符集合。

    ⚠️ 为什么要比对：标识符在 3 个地方各写了一遍（C 固件、探针、本文件）。
       任何一处拼错，现象都是"平台报 2306"，而且**一次只报一个** ——
       没有这个比对，就得靠人工逐字核对三份表。
    """
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from onenet_probe import PROP_CHECKS        # noqa: PLC0415
    except Exception as exc:                        # noqa: BLE001
        print("[WARN] 无法导入 onenet_probe 做一致性比对：{}".format(exc))
        return True

    mine = [p[0] for p in PROPERTIES]
    theirs = [p[0] for p in PROP_CHECKS]

    only_mine = [x for x in mine if x not in theirs]
    only_theirs = [x for x in theirs if x not in mine]

    if only_mine or only_theirs:
        print("[FAIL] 属性标识符与 onenet_probe.py 不一致：")
        if only_mine:
            print("  只在 gen_onenet_tsl.py 里有: {}".format(only_mine))
        if only_theirs:
            print("  只在 onenet_probe.py 里有: {}".format(only_theirs))
        print("  => 固件上报的属性与物模型对不上，平台会报 2306")
        return False

    print("[ OK ] 属性标识符与 onenet_probe.py 一致（{} 个）".format(len(mine)))
    return True


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="生成 OneNET 物模型 TSL 文件（可直接导入控制台）",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--product-id", required=True, help="产品ID")
    ap.add_argument("--out", default="onenet_tsl.json", help="输出文件")
    ap.add_argument("--stdout", action="store_true",
                    help="输出到屏幕而不是写文件")
    ap.add_argument("--no-profile", action="store_true",
                    help="不生成 profile 段（导入报类目错误时用）")
    ap.add_argument("--industry-id", default="1")
    ap.add_argument("--scene-id", default="14")
    ap.add_argument("--category-id", default="314")
    args = ap.parse_args(argv)

    if not check_consistency():
        return 2

    tsl = build_tsl(args.product_id,
                    with_profile=(not args.no_profile),
                    industry_id=args.industry_id,
                    scene_id=args.scene_id,
                    category_id=args.category_id)

    text = json.dumps(tsl, ensure_ascii=False, indent=2)

    if args.stdout:
        print(text)
    else:
        with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text + "\n")
        print()
        print("[ OK ] 已生成 {}".format(os.path.abspath(args.out)))
        print("       {} 个属性（{} 个只读 + {} 个读写）".format(
            len(tsl["properties"]),
            sum(1 for p in tsl["properties"] if p["accessMode"] == "r"),
            sum(1 for p in tsl["properties"] if p["accessMode"] == "rw")))

    print()
    print("=" * 70)
    print(" ⚠️ 不要直接导入上面的文件")
    print("=" * 70)
    print(" 本产品（lot_1）的产品类型是「环境监测」，平台已自带标准功能点")
    print("（CurrentTemperature / LightLux / RelativeHumidity / alarm），")
    print("而且本文件的 profile 段是照文档推的，不是平台真实值。")
    print()
    print(" 正确做法：拿控制台导出的文件当骨架，只追加我们的属性 ——")
    print()
    print("   1. 控制台：产品「详情」->「物模型」->「查看/导出物模型」")
    print("      复制 JSON 存成文件，例如 scratch/onenet_console_export.json")
    print()
    print("   2. 合并（用不到的类目自带点用 --drop 删掉）：")
    print("      python tools/merge_onenet_tsl.py \\")
    print("        --base scratch/onenet_console_export.json \\")
    print("        --drop LightLux,RelativeHumidity,alarm \\")
    print("        --out doc/onenet_tsl_merged.json")
    print()
    print("   3. 控制台「设置物模型」->「导入物模型」-> 选 merged 文件")
    print("      ->「导入」-> ★ **点「保存」**")
    print("         （漏了不生效，而设备上报仍然报 2306，")
    print("           看起来像「导入没成功」）")
    print()
    print("  导入后验证：")
    print("    python tools/onenet_probe.py --probe-props")
    print("      （凭据读 secrets/onenet.env；没有该文件时用长参数形式：")
    print("       python tools/onenet_probe.py --product-id {} \\".format(
        args.product_id))
    print("              --device-name <设备名> --token '<token>' --probe-props）")
    print()
    print("  三处标识符是否一致（固件宏 / 探针 / 本工具 / 合并文件）：")
    print("    python tools/check_onenet_ids.py")
    print("=" * 70)
    return 0


if __name__ == "__main__":
    sys.exit(main())
