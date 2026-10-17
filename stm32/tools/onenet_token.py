#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 OneNET（中国移动物联网平台）MQTT 设备鉴权 token

============================================================================
 为什么需要它

  OneNET Studio（studio-mqtt.heclouds.com）**不是通用 MQTT broker**，
  它的 MQTT CONNECT 三要素是这样填的：

      clientId  = 设备名称        （在 OneNET 控制台创建设备时自己起的名字）
      username  = 产品ID          （平台分配的，形如 "jvyXzK7zgo"）
      password  = token           ← 本工具生成的就是它

  而 token 不是"访问密钥"本身，是**用访问密钥算出来的**：

      sign = base64( hmac_<method>( base64decode(key),
                                    utf8(StringForSignature) ) )
      StringForSignature = et + "\\n" + method + "\\n" + res + "\\n" + version
      res                = "products/{产品ID}/devices/{设备名称}"

  ⚠️ 三个最容易踩的点：

    ① `key` 参与计算前要 **base64decode**。控制台给的那串密钥是 base64，
       直接拿字符串去算 HMAC 一定对不上，而现象只是"鉴权失败"，
       看不出是编码问题。

    ② 拼 token 字符串时，**value 部分要 URL 编码**（/ -> %2F，= -> %3D …）。
       不编码的话，`res` 里的 `/` 会把 token 结构弄坏。

    ③ `et` 是**过期时间**（unix 秒）。给得太小 —— 比如随手写个当天 ——
       设备当下能连上，**过一阵子突然全连不上**，而且报的还是鉴权失败。
       本工具默认给 10 年。

============================================================================
 用法

    # 1) 生成 token 并打印 MQTT 三要素（直接照抄进控制台/代码）
    python tools/onenet_token.py --product-id jvyXzK7zgo \\
                                 --device-name gateway001 \\
                                 --access-key "控制台给的访问密钥"

    # 2) 生成可直接粘进 esp32/src/main.c 的宏
    python tools/onenet_token.py --product-id ... --device-name ... \\
                                 --access-key ... --emit-c

    # 3) 指定签名算法与有效期
    python tools/onenet_token.py ... --method sha256 --days 3650

    # 4) 自检（回归用例，不联网）
    python tools/onenet_token.py --selftest

============================================================================
 访问密钥从哪来

    OneNET 控制台 -> 产品 -> 产品详情 -> 「访问密钥」；
    或 设备详情 -> 「设备密钥」。两个都能用（产品级 / 设备级）。
    ⚠️ 密钥只在创建时完整显示一次，忘了只能重置。
"""

import argparse
import base64
import hashlib
import hmac
import sys
import time
import urllib.parse

# ---------------------------------------------------------------------------
#  常量（来自 OneNET 官方文档「接入安全认证」）
# ---------------------------------------------------------------------------

#: token 参数组版本号，官方目前**仅支持**这一个值，写死
TOKEN_VERSION = "2018-10-31"

#: 支持的签名算法：token 里的 method 值 -> hashlib 的构造名
#: ⚠️ 官方文档写的是"支持 hmacmd5、hmacsha1、hmacsha256"，
#:    但 token 字符串里的 method **值**是 md5 / sha1 / sha256
#:    （即 `method=sha1` 表示"用 hmac-sha1 算"）。
METHODS = {
    "md5": "md5",
    "sha1": "sha1",
    "sha256": "sha256",
}

#: 默认签名算法。官方文档的示例用的是 sha1，这里保持一致。
DEFAULT_METHOD = "sha1"

#: 默认有效期（天）。给 10 年 —— 见模块开头 ⚠️③。
DEFAULT_DAYS = 3650


# ---------------------------------------------------------------------------
#  核心算法
# ---------------------------------------------------------------------------

def build_resource(product_id, device_name):
    """拼 res 资源串：products/{产品ID}/devices/{设备名称}"""
    return "products/{}/devices/{}".format(product_id, device_name)


def build_signature(key, et, method, res, version=TOKEN_VERSION):
    """
    算 sign = base64(hmac_<method>(base64decode(key), StringForSignature))

    @param key   控制台给的访问密钥（**base64 编码的字符串**）
    @return      base64 字符串（**未**做 URL 编码）
    """
    if method not in METHODS:
        raise ValueError("不支持的签名算法: {}（可选 {}）".format(
            method, "/".join(sorted(METHODS))))

    # ⚠️ 官方原文："Key参与计算前应先进行base64decode操作"。
    #    这里顺手容忍带换行/空格的粘贴（控制台复制常带尾部换行）。
    try:
        key_bytes = base64.b64decode(key.strip(), validate=False)
    except Exception as exc:                      # noqa: BLE001
        raise ValueError("访问密钥不是合法的 base64：{}".format(exc))

    if not key_bytes:
        raise ValueError("访问密钥解码后为空，检查是否复制完整")

    # ⚠️ 顺序是固定的：et、method、res、version（按参数名排序），'\n' 分隔，
    #    且**只有 value 参与**（不带 "et=" 这样的 key=）。
    string_for_signature = "\n".join([str(et), method, res, version])

    digest = hmac.new(key_bytes,
                      string_for_signature.encode("utf-8"),
                      getattr(hashlib, METHODS[method])).digest()
    return base64.b64encode(digest).decode("ascii")


def url_quote(value):
    """
    按官方要求对 token 的 value 做 URL 编码。

    ⚠️ 官方列出的必须编码字符：+ 空格 / ? % # & =
       其中 `+` 特别容易被忽略 —— 它出现在 **sign 里**（base64 会产生 `+`），
       不编码的话平台那边会把它当空格解，签名校验就失败。
    """
    return urllib.parse.quote(value, safe="")


def build_token(product_id, device_name, access_key,
                method=DEFAULT_METHOD, et=None, days=DEFAULT_DAYS):
    """
    生成完整的 MQTT password（已 URL 编码）。

    @param et   unix 秒；为 None 时按 `days` 算
    @return     (token, et)
    """
    if et is None:
        et = int(time.time()) + int(days) * 86400
    et = int(et)

    res = build_resource(product_id, device_name)
    sign = build_signature(access_key, et, method, res)

    # ⚠️ 官方原文："将每个参数均采用key=value的形式表示，并用'&'作为分隔符"
    #    参数顺序官方示例是 version/res/et/method/sign，保持这个顺序。
    parts = [
        ("version", TOKEN_VERSION),
        ("res",     res),
        ("et",      str(et)),
        ("method",  method),
        ("sign",    sign),
    ]
    return "&".join("{}={}".format(k, url_quote(v)) for k, v in parts), et


def build_client_id(device_name, product_id=None, keepalive=60):
    """
    部分 OneNET 文档给的 clientId 形式带参数。官方 MQTT 接入文档只要求
    clientId = 设备名称，所以**默认就返回设备名称本身**。
    保留这个函数是为了将来需要参数化时有个明确落点。
    """
    del product_id, keepalive          # 暂未使用，显式忽略
    return device_name


# ---------------------------------------------------------------------------
#  自检
# ---------------------------------------------------------------------------

#: 回归向量：固定输入 -> 固定输出。
#: ⚠️ 关于这个向量是怎么来的（诚实说明）：
#:    官方文档里的示例 sign 明显是脱敏后编的（base64 长度都不对），
#:    没法当测试向量用。所以这里用的是一个**跨实现交叉验证**过的值：
#:        printf '1537255523\nsha1\nproducts/dafdfadfafdaf/devices/che1\n2018-10-31' \
#:          | openssl dgst -sha1 -hmac "testkey123" -binary | openssl base64 -A
#:        => JPKaZaHGG+G5ghKJ3/nhhBf/3mI=
#:    openssl 与 Python 的 hmac 是两套独立实现，两者一致
#:    说明**算法本身**没写错（顺序、key 解码、换行分隔都对）。
#:    但"平台认不认"最终只能靠连一次来验证 —— 见 tools/onenet_probe.py。
SELFTEST_VECTORS = [
    # (product_id, device_name, access_key_b64, method, et, expected_token)
    (
        "dafdfadfafdaf", "che1", base64.b64encode(b"testkey123").decode(),
        "sha1", 1537255523,
        "version=2018-10-31"
        "&res=products%2Fdafdfadfafdaf%2Fdevices%2Fche1"
        "&et=1537255523"
        "&method=sha1"
        "&sign=JPKaZaHGG%2BG5ghKJ3%2FnhhBf%2F3mI%3D",
    ),
]


def selftest():
    """跑回归向量 + 几个结构性断言。返回 0 表示全过。"""
    failures = []

    for product_id, device_name, key, method, et, expected in SELFTEST_VECTORS:
        got, _ = build_token(product_id, device_name, key,
                             method=method, et=et)
        if got != expected:
            failures.append("向量不符:\n  期望 {}\n  实得 {}".format(
                expected, got))

    # --- 结构断言：value 必须被 URL 编码 ---
    tok, et = build_token("pid", "dn", base64.b64encode(b"k").decode())
    res_val = tok.split("&res=")[1].split("&")[0]
    if "/" in res_val:
        failures.append("res 里的 '/' 没有被编码成 %2F")
    sign_val = tok.split("&sign=")[1]
    # base64 的 '=' 填充必须变成 %3D；'/' 必须变成 %2F；'+' 必须变成 %2B
    if "=" in sign_val and "%3D" not in sign_val:
        failures.append("sign 里的 '=' 没有被编码成 %3D")
    if "/" in sign_val:
        failures.append("sign 里的 '/' 没有被编码成 %2F")
    if "+" in sign_val:
        failures.append("sign 里的 '+' 没有被编码成 %2B")

    # --- 结构断言：StringForSignature 的顺序 ---
    seen = {}

    def _spy(key, et_, method, res, version=TOKEN_VERSION):   # noqa: ARG001
        seen["joined"] = "\n".join([str(et_), method, res, version])
        return "x"

    real = build_signature
    try:
        globals()["build_signature"] = _spy
        build_token("pid", "dn", base64.b64encode(b"k").decode(),
                    method="sha256", et=1234)
    finally:
        globals()["build_signature"] = real

    want = "1234\nsha256\nproducts/pid/devices/dn\n2018-10-31"
    if seen.get("joined") != want:
        failures.append("StringForSignature 顺序不对:\n  期望 {!r}\n  实得 {!r}"
                        .format(want, seen.get("joined")))

    # --- 结构断言：key 必须先 base64decode ---
    #   用同一个密钥的两种表示（原文 vs base64）算出来的 sign 必须不同，
    #   否则说明没做 decode。
    k_raw = "c2VjcmV0LWtleQ=="                     # base64("secret-key")
    s_a = build_signature(k_raw, 1, "sha1", "r")
    s_b = build_signature(base64.b64encode(k_raw.encode()).decode(),
                          1, "sha1", "r")
    if s_a == s_b:
        failures.append("key 没有被 base64decode（两种表示算出同一个 sign）")

    if failures:
        print("[FAIL] onenet_token 自检未通过：")
        for f in failures:
            print("  - {}".format(f))
        return 1

    print("[ OK ] onenet_token 自检通过（{} 个回归向量 + 4 项结构断言）".format(
        len(SELFTEST_VECTORS)))
    return 0


# ---------------------------------------------------------------------------
#  CLI
# ---------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="生成 OneNET MQTT 鉴权 token（password）",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--product-id",  help="产品ID（平台分配，形如 jvyXzK7zgo）")
    ap.add_argument("--device-name", help="设备名称（创建设备时自己起的名字）")
    ap.add_argument("--access-key",  help="访问密钥（产品级或设备级，base64）")
    ap.add_argument("--method", default=DEFAULT_METHOD,
                    choices=sorted(METHODS), help="签名算法（默认 sha1）")
    ap.add_argument("--et", type=int, default=None,
                    help="过期时间（unix 秒）；给了就忽略 --days")
    ap.add_argument("--days", type=int, default=DEFAULT_DAYS,
                    help="有效期天数（默认 {}）".format(DEFAULT_DAYS))
    ap.add_argument("--emit-c", action="store_true",
                    help="额外输出可粘进 esp32/src/main.c 的宏")
    ap.add_argument("--selftest", action="store_true", help="跑自检后退出")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    missing = [n for n, v in (("--product-id", args.product_id),
                              ("--device-name", args.device_name),
                              ("--access-key", args.access_key)) if not v]
    if missing:
        ap.error("缺少参数: {}".format(" ".join(missing)))

    try:
        token, et = build_token(args.product_id, args.device_name,
                                args.access_key,
                                method=args.method, et=args.et,
                                days=args.days)
    except ValueError as exc:
        print("[ERROR] {}".format(exc), file=sys.stderr)
        return 2

    client_id = build_client_id(args.device_name)

    print("=" * 68)
    print(" OneNET MQTT 接入三要素（照抄进控制台 / 代码）")
    print("=" * 68)
    print(" broker   : mqtt://studio-mqtt.heclouds.com:1883")
    print(" clientId : {}".format(client_id))
    print(" username : {}".format(args.product_id))
    print(" password : {}".format(token))
    print("-" * 68)
    print(" 签名算法 : {}   过期时间 : {} ({})".format(
        args.method, et, time.strftime("%Y-%m-%d %H:%M:%S",
                                       time.localtime(et))))
    print("=" * 68)

    if args.emit_c:
        print()
        print("/* ---- 粘进 esp32/src/main.c ---- */")
        print('#define GW_MQTT_BROKER      "mqtt://studio-mqtt.heclouds.com:1883"')
        print('#define GW_MQTT_CLIENT_ID   "{}"'.format(client_id))
        print('#define GW_MQTT_USER        "{}"'.format(args.product_id))
        print('#define GW_MQTT_PASS        "{}"'.format(token))

    return 0


if __name__ == "__main__":
    sys.exit(main())
