#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
OneNET 接入探针 —— 在 **PC 上**验证凭据与物模型，不用碰 ESP32/STM32

============================================================================
 为什么需要它

  OneNET 这条链路上有 4 个独立变量，任何一个是错的，现象都一样：
  「连不上」或「发出去没反应」。

      ① token 算法（base64decode、URL 编码、参数顺序）
      ② 鉴权三要素是否配套（clientId=设备名 / username=产品ID / password=token）
      ③ 物模型属性标识符是否和控制台里建的一致
      ④ 固件本身

  如果直接烧固件去试，一次往返（改-编译-烧录-接线-看日志）好几分钟，
  而且**根本分不清是哪一条错了**。

  本工具用**纯 socket** 实现最小 MQTT 3.1.1 客户端（零第三方依赖，
  不装 paho 也能跑），在 PC 上 2 秒就能把 ①②③ 验掉。
  剩下 ④ 才是固件的事。

  ⚠️ 它**不碰 STM32**，所以**不影响正在跑的 72h 浸泡**。

============================================================================
 用法

    # 0) 先自检（不联网）
    python tools/onenet_probe.py --selftest

    # 0.5) 推荐：把凭据放进 secrets/onenet.env（已 gitignore），之后就不用再粘
    #      凭据来源优先级：命令行 > 环境变量 > 该文件
    #      于是日常只需要一条短命令：
    python tools/onenet_probe.py --probe-props

    # 1) 只验凭据（连上就断）
    python tools/onenet_probe.py --product-id <pid> --device-name <dn> \\
                                 --access-key "<密钥>"

    # 2) 连上 + 发一条属性上报 + 等应答
    python tools/onenet_probe.py --product-id <pid> --device-name <dn> \\
                                 --access-key "<密钥>" --post

    # 3) 顺带订 30 秒下行，看平台下发的属性设置长什么样
    python tools/onenet_probe.py ... --post --listen 30

    # 4) 忽略凭据文件（排查"到底用的哪份凭据"）
    python tools/onenet_probe.py --no-env-file --product-id ... --token ...

============================================================================
 凭据文件怎么读（避免"看起来读了其实没读到"）

    纯 key=value 文本，值**不需要**加引号；# 开头是注释；
    没有 = 的行直接跳过。键名固定这几个：

        ONENET_PRODUCT_ID=...
        ONENET_DEVICE_NAME=...
        ONENET_TOKEN=...          # token 与 access key 二者给一个即可
        ONENET_ACCESS_KEY=...
        ONENET_HOST=studio-mqtt.heclouds.com
        ONENET_PORT=1883

    ⚠️ token 里有 %2F %3D 这类百分号编码，**不要**再加引号以外的转义；
       整行直接粘贴即可（只有值两端是成对引号时才会被剥掉）。

============================================================================
 返回码怎么读（CONNACK）

    0  accepted                 连接被接受
    1  unacceptable protocol    协议版本不对（我们发的是 3.1.1 = level 4）
    2  identifier rejected      clientId 不合法（OneNET 要求 = 设备名称）
    3  server unavailable       平台侧不可用
    4  bad username or password  ★ 最常见：产品ID / token 错
    5  not authorized           ★ 设备名不存在，或 token 与设备不匹配

  rc=4/5 时按这个顺序查：
    1. 产品ID 是不是**产品**的 ID（不是设备 ID）
    2. clientId 是不是**设备名称**（不是设备 ID、不是产品ID）
    3. token 的 et 是不是已经过期
    4. 访问密钥是不是复制完整（它只在创建时完整显示一次）
"""

import argparse
import base64
import hashlib
import hmac
import os
import re
import socket
import struct
import sys
import time
import urllib.parse

# ⚠️ 编码守卫：本脚本会打印 ⚠️ 等非 GBK 字符。
#    默认中文控制台（GBK）下 print 会 UnicodeEncodeError 直接崩，
#    而崩的位置在**第一条输出之后**，看起来像"探针跑了一半就没了"。
#    tools/check_console_encoding.py 会盯着这件事。
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(errors="replace")
    except (AttributeError, ValueError):
        pass

# ---------------------------------------------------------------------------
#  凭据来源：命令行 > 环境变量 > 本地凭据文件
#
#  为什么要有第三档：
#    OneNET 的 password（token）是一串 ~150 字符、带 %2F %3D 的东西，
#    每跑一次探针就要粘贴一次，还容易在 shell 里被截断或吃掉字符
#    （% 在某些终端里会被解释）。粘错一次的现象是 rc=4「鉴权失败」，
#    看起来像"token 算法不对"，实际上只是复制粘贴出了问题。
#
#    把凭据放进 secrets/onenet.env（**已 gitignore**），
#    之后只需要 `python tools/onenet_probe.py --probe-props` 一条短命令。
# ---------------------------------------------------------------------------

# 仓库根目录 = 本文件所在目录的上一级
_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_ENV_FILE = os.path.join(_REPO_ROOT, "secrets", "onenet.env")

# 每个字段的读取顺序：CLI 参数名 -> 环境变量名 -> 文件里的键名
CRED_FIELDS = (
    # (argparse 属性, 环境变量, 文件键, 中文名)
    ("product_id",  "ONENET_PRODUCT_ID",  "ONENET_PRODUCT_ID",  "产品ID"),
    ("device_name", "ONENET_DEVICE_NAME", "ONENET_DEVICE_NAME", "设备名称"),
    ("token",       "ONENET_TOKEN",       "ONENET_TOKEN",       "token"),
    ("access_key",  "ONENET_ACCESS_KEY",  "ONENET_ACCESS_KEY",  "访问密钥"),
    ("host",        "ONENET_HOST",        "ONENET_HOST",        "broker 地址"),
    ("port",        "ONENET_PORT",        "ONENET_PORT",        "broker 端口"),
)


def load_env_file(path):
    """读 key=value 形式的凭据文件。

    规则刻意做得很窄，避免"看起来读了其实没读到"：
      - 忽略空行与 # 开头的注释
      - 键名两侧空白去掉；值两侧空白去掉，**不**去掉引号以外的内容
      - 允许值用单/双引号包起来（去掉即可，不做转义解释）
      - 没有 = 的行直接跳过（不报错，兼容用户自己加的分节标题）
    """
    data = {}
    if not path or not os.path.isfile(path):
        return data
    with open(path, "r", encoding="utf-8") as fh:
        for raw in fh:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if "=" not in line:
                continue
            key, _, val = line.partition("=")
            key = key.strip()
            val = val.strip()
            if len(val) >= 2 and val[0] == val[-1] and val[0] in ("'", '"'):
                val = val[1:-1]
            if key:
                data[key] = val
    return data


def resolve_credentials(args, env_file):
    """按 命令行 > 环境变量 > 文件 的顺序补齐凭据，返回 (来源说明, 警告列表)"""
    file_data = load_env_file(env_file)
    notes = []
    warns = []

    for attr, env_name, file_key, cn in CRED_FIELDS:
        if getattr(args, attr, None):
            notes.append("{:<10} <- 命令行".format(cn))
            continue
        if os.environ.get(env_name):
            setattr(args, attr, os.environ[env_name])
            notes.append("{:<10} <- 环境变量 {}".format(cn, env_name))
            continue
        if file_data.get(file_key):
            setattr(args, attr, file_data[file_key])
            notes.append("{:<10} <- {}".format(
                cn, os.path.relpath(env_file, _REPO_ROOT)))
            continue

    # port 从文件里读到的是字符串，要转回 int
    if args.port is not None:
        try:
            args.port = int(args.port)
        except (TypeError, ValueError):
            warns.append("broker 端口 {:r} 不是整数，回退到 1883".format(args.port))
            args.port = 1883

    return notes, warns


# ---------------------------------------------------------------------------
#  OneNET token（与 tools/onenet_token.py 同一套算法）
# ---------------------------------------------------------------------------

TOKEN_VERSION = "2018-10-31"


def build_token(product_id, device_name, access_key,
                method="sha1", et=None, days=3650):
    """生成 OneNET MQTT password（已 URL 编码）"""
    if et is None:
        et = int(time.time()) + int(days) * 86400
    et = int(et)

    res = "products/{}/devices/{}".format(product_id, device_name)
    sfs = "\n".join([str(et), method, res, TOKEN_VERSION])

    key_bytes = base64.b64decode(access_key.strip(), validate=False)
    if not key_bytes:
        raise ValueError("访问密钥解码后为空，检查是否复制完整")

    sign = base64.b64encode(
        hmac.new(key_bytes, sfs.encode("utf-8"),
                 getattr(hashlib, method)).digest()).decode("ascii")

    q = lambda v: urllib.parse.quote(v, safe="")          # noqa: E731
    return "&".join([
        "version=" + q(TOKEN_VERSION),
        "res="     + q(res),
        "et="      + q(str(et)),
        "method="  + q(method),
        "sign="    + q(sign),
    ]), et


# ---------------------------------------------------------------------------
#  MQTT 3.1.1 —— 最小实现
# ---------------------------------------------------------------------------

CONNECT, CONNACK = 1, 2
PUBLISH, PUBACK = 3, 4
SUBSCRIBE, SUBACK = 8, 9
PINGREQ, PINGRESP = 12, 13
DISCONNECT = 14

CONNACK_TEXT = {
    0: "accepted 连接被接受",
    1: "unacceptable protocol version 协议版本不对",
    2: "identifier rejected clientId 不合法",
    3: "server unavailable 平台侧不可用",
    4: "bad username or password ★ 产品ID 或 token 错",
    5: "not authorized ★ 设备名不存在，或 token 与设备不匹配",
}


def enc_len(n):
    """MQTT 剩余长度：7 位一组，最高位是续接标志"""
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        if n > 0:
            b |= 0x80
        out.append(b)
        if n == 0:
            return bytes(out)


def enc_str(s):
    """MQTT 字符串：2 字节大端长度 + UTF-8"""
    b = s.encode("utf-8") if isinstance(s, str) else s
    return struct.pack(">H", len(b)) + b


def build_connect(client_id, username=None, password=None, keepalive=60,
                  clean_session=True):
    """MQTT CONNECT 报文"""
    flags = 0
    if clean_session:
        flags |= 0x02
    payload = enc_str(client_id)
    if username is not None:
        flags |= 0x80
        payload += enc_str(username)
    if password is not None:
        flags |= 0x40
        payload += enc_str(password)

    var = enc_str("MQTT") + bytes([4]) + bytes([flags]) + \
        struct.pack(">H", keepalive)

    body = var + payload
    return bytes([CONNECT << 4]) + enc_len(len(body)) + body


def build_subscribe(packet_id, topics):
    """topics: [(filter, qos), ...]"""
    body = struct.pack(">H", packet_id)
    for t, q in topics:
        body += enc_str(t) + bytes([q])
    return bytes([(SUBSCRIBE << 4) | 0x02]) + enc_len(len(body)) + body


def build_publish(topic, payload, qos=0, packet_id=None, retain=False):
    flags = (qos << 1) | (1 if retain else 0)
    body = enc_str(topic)
    if qos > 0:
        body += struct.pack(">H", packet_id)
    if isinstance(payload, str):
        payload = payload.encode("utf-8")
    body += payload
    return bytes([(PUBLISH << 4) | flags]) + enc_len(len(body)) + body


def build_simple(ptype):
    return bytes([ptype << 4, 0])


class Mqtt:
    """够用就好的 MQTT 客户端：CONNECT / SUBSCRIBE / PUBLISH / PING"""

    def __init__(self, host, port, timeout=10.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.sock = None
        self.rbuf = bytearray()

    # ---- 连接与收发 ----
    def connect(self, client_id, username=None, password=None):
        self.sock = socket.create_connection((self.host, self.port),
                                             timeout=self.timeout)
        self.sock.sendall(build_connect(client_id, username, password))
        ptype, _flags, body = self._read_packet()
        if ptype != CONNACK:
            raise IOError("期望 CONNACK(2)，收到报文类型 {}".format(ptype))
        if len(body) < 2:
            raise IOError("CONNACK 长度不足")
        return body[1]                      # 返回码

    def _recv_some(self):
        chunk = self.sock.recv(4096)
        if not chunk:
            raise IOError("对端关闭了连接")
        self.rbuf.extend(chunk)

    def _read_packet(self):
        """读一个完整报文；返回 (type, flags, body)"""
        while True:
            # 固定头至少 2 字节
            if len(self.rbuf) >= 2:
                mult = 1
                rlen = 0
                i = 1
                while True:
                    if i >= len(self.rbuf):
                        break
                    b = self.rbuf[i]
                    rlen += (b & 0x7F) * mult
                    mult *= 128
                    i += 1
                    if not (b & 0x80):
                        total = i + rlen
                        if len(self.rbuf) >= total:
                            hdr = self.rbuf[0]
                            body = bytes(self.rbuf[i:total])
                            del self.rbuf[:total]
                            return (hdr >> 4) & 0x0F, hdr & 0x0F, body
                        break
                    if mult > 128 ** 3:
                        raise IOError("剩余长度字段非法")
            self._recv_some()

    def _read_packet_timeout(self, timeout):
        """带超时地读一个报文；超时返回 None"""
        end = time.time() + timeout
        old = self.sock.gettimeout()
        try:
            while True:
                left = end - time.time()
                if left <= 0:
                    return None
                self.sock.settimeout(left)
                try:
                    return self._read_packet()
                except socket.timeout:
                    return None
        finally:
            self.sock.settimeout(old)

    def subscribe(self, packet_id, topics):
        self.sock.sendall(build_subscribe(packet_id, topics))
        ptype, _f, body = self._read_packet()
        if ptype != SUBACK:
            raise IOError("期望 SUBACK(9)，收到 {}".format(ptype))
        return body[2:]                     # 每个订阅的返回码

    def publish(self, topic, payload, qos=0, packet_id=None):
        self.sock.sendall(build_publish(topic, payload, qos, packet_id))

    def ping(self):
        self.sock.sendall(build_simple(PINGREQ))

    def close(self):
        try:
            if self.sock is not None:
                self.sock.sendall(build_simple(DISCONNECT))
                self.sock.close()
        except Exception:                   # noqa: BLE001
            pass
        self.sock = None


def parse_publish(body):
    """从 PUBLISH 报文体里取出 (topic, payload)"""
    if len(body) < 2:
        return "", ""
    tlen = struct.unpack(">H", body[:2])[0]
    topic = body[2:2 + tlen].decode("utf-8", "replace")
    rest = body[2 + tlen:]
    # QoS>0 时有 2 字节 packet id
    # （本工具只发 QoS0，但平台回的可能带 id —— 这里宽松处理）
    return topic, rest.decode("utf-8", "replace")


def parse_token(tok):
    """
    把 token 拆成 dict（value 已 URL 解码），并做基本结构检查。

    @return (parts, err)；err 非 None 表示结构就不对

    ⚠️ 为什么要专门做这一步：token 是**手工拼出来的字符串**，
       少一个 `&`、`res` 里的产品ID 与 topic 里写的不一致，
       表现都是"鉴权失败"—— 和"密钥错"完全一样，分不出来。
       拆开逐字段比对，能把"结构性错误"和"密钥错误"分开。
    """
    parts = {}

    if not tok:
        return None, "token 为空"

    for kv in tok.split("&"):
        if "=" not in kv:
            return None, "token 里有不含 '=' 的片段: {!r}".format(kv)
        k, v = kv.split("=", 1)
        parts[k] = urllib.parse.unquote(v)

    return parts, None


def verify_token(parts, product_id, device_name):
    """
    逐字段校验 token 与"打算用的产品/设备"是否自洽。

    @return 问题列表（空 = 没问题）
    """
    problems = []

    if parts.get("version") != TOKEN_VERSION:
        problems.append("version 应为 {!r}，实得 {!r}".format(
            TOKEN_VERSION, parts.get("version")))

    want_res = "products/{}/devices/{}".format(product_id, device_name)
    if parts.get("res") != want_res:
        problems.append(
            "res 与 topic 用的产品/设备**不一致**：\n"
            "        期望 {!r}\n"
            "        实得 {!r}\n"
            "        ^^^ 这会导致「连上了但收发都不通」，且极难定位".format(
                want_res, parts.get("res")))

    if parts.get("method") not in ("md5", "sha1", "sha256"):
        problems.append("method 不认识: {!r}".format(parts.get("method")))

    if not parts.get("sign"):
        problems.append("缺 sign 字段")

    et = parts.get("et")
    if et is None:
        problems.append("缺 et（过期时间）字段")
    else:
        try:
            et_i = int(et)
            if et_i < time.time():
                problems.append(
                    "et 已过期：{} ({})".format(
                        et, time.strftime("%Y-%m-%d %H:%M:%S",
                                          time.localtime(et_i))))
        except ValueError:
            problems.append("et 不是整数: {!r}".format(et))

    return problems


def token_et(parts):
    """从拆好的 token 里取 et（unix 秒）；取不到返回 None"""
    try:
        return int(parts.get("et"))
    except (TypeError, ValueError):
        return None


# ---------------------------------------------------------------------------
#  物模型属性清单（用于逐个点名检查）
#
#  ⚠️ 平台对"属性不存在"的报错是：
#        {"code":2306,"msg":"identifier not exist:identifier:temp"}
#     它**一次只说一个**。所以一次性发 9 个属性，只能知道"至少有一个不存在"，
#     改完再试还是同一个错误、只是名字变了 —— 来回好几轮。
#     逐个发就一次问清楚，直接拿到"缺哪些"的完整清单。
# ---------------------------------------------------------------------------

PROP_CHECKS = [
    # (标识符, 类型, 值模板 —— %d 是毫秒时间戳, 说明)
    # ⚠️ 标识符是**云端契约**，必须与 esp32/include/esp32_json.h 里的
    #    ESP32_ONENET_ID_* 宏、以及 doc/onenet_tsl_merged.json 逐字一致。
    #    tools/check_onenet_ids.py 会三处比对，防止悄悄漂移。
    # ⚠️ 温度用的是平台「环境监测」类目自带的**标准功能点**
    #    CurrentTemperature（**没有空格** —— 控制台界面因列宽折行
    #    看起来像有空格，以「导出物模型」的 JSON 为准）。
    ("CurrentTemperature", "float",
     '{"value":23.5,"time":%d}',   "温度（平台标准点，有符号）"),
    ("humi",       "float",  '{"value":56.0,"time":%d}',   "湿度"),
    ("seq",        "int32",  '{"value":1,"time":%d}',      "递增序号"),
    ("status",     "int32",  '{"value":3,"time":%d}',      "设备状态位"),
    ("uptime",     "int32",  '{"value":42,"time":%d}',     "运行时间(s)"),
    ("can1",       "int32",  '{"value":0,"time":%d}',      "CAN 通道 1"),
    ("can2",       "int32",  '{"value":0,"time":%d}',      "CAN 通道 2"),
    ("can3",       "int32",  '{"value":0,"time":%d}',      "CAN 通道 3"),
    ("can4",       "int32",  '{"value":0,"time":%d}',      "CAN 通道 4"),
    ("wifi",       "int32",  '{"value":2,"time":%d}',      "Wi-Fi 状态"),
    ("mqtt",       "int32",  '{"value":2,"time":%d}',      "MQTT 状态"),
    ("rssi",       "int32",  '{"value":-62,"time":%d}',    "信号强度(dBm)"),
    ("backlog",    "int32",  '{"value":0,"time":%d}',      "待补传条数"),
    ("cmd",        "string", '{"value":"PROBE","time":%d}', "★ 下行指令（读写）"),
    ("cmd_result", "string", '{"value":"PROBE","time":%d}', "★ 指令执行结果"),
]


def build_post_payload(ts_ms=None, msg_id="1"):
    """
    生成一条**完整**的属性上报报文（`--post` 用）。

    ★ 刻意从 PROP_CHECKS 派生，而不是手写字面量。
      上一版的 `--post` 把 9 个标识符硬编在字符串里，于是温度那处
      在改成平台标准点 `CurrentTemperature` 时**漏改**了 ——
      `--probe-props` 15 个全 200，`--post` 却回 2306，
      看起来像"物模型没导入成功"，实则是探针自己发了旧名字。
      派生之后这种漂移在结构上不可能再发生。

    @param ts_ms  属性时间戳（毫秒）；None = 取当前时间
    @param msg_id 报文 id（平台用它关联 post/reply）
    @return JSON 字符串
    """
    if ts_ms is None:
        ts_ms = int(time.time() * 1000)
    items = ",".join('"{}":{}'.format(name, tpl % ts_ms)
                     for name, _ptype, tpl, _desc in PROP_CHECKS)
    return '{{"id":"{}","version":"1.0","params":{{{}}}}}'.format(msg_id, items)


def probe_properties(c, topic_post, timeout=8.0):
    """
    逐个属性发一条上报，看平台认不认。

    @return [(标识符, 类型, 说明, code, msg), ...]；code 为 None 表示没应答
    """
    results = []

    for i, (name, ptype, tpl, desc) in enumerate(PROP_CHECKS, start=1):
        ts = int(time.time() * 1000)
        payload = ('{{"id":"{}","version":"1.0","params":{{"{}":{}}}}}'
                   .format(100 + i, name, tpl % ts))

        try:
            c.publish(topic_post, payload)
        except Exception as exc:            # noqa: BLE001
            results.append((name, ptype, desc, None, "发布失败: {}".format(exc)))
            continue

        pkt = c._read_packet_timeout(timeout)
        if pkt is None:
            results.append((name, ptype, desc, None, "8 秒无应答"))
            continue

        ptype_got, _f, body = pkt
        if ptype_got != PUBLISH:
            results.append((name, ptype, desc, None,
                            "应答报文类型 {}（不是 PUBLISH）".format(ptype_got)))
            continue

        _t, pl = parse_publish(body)
        m = re.search(r'"code"\s*:\s*(-?\d+)', pl)
        code = int(m.group(1)) if m else None
        results.append((name, ptype, desc, code, pl))

    return results


# ---------------------------------------------------------------------------
#  自检
# ---------------------------------------------------------------------------

def selftest():
    fails = []

    # 剩余长度编码
    cases = [(0, b"\x00"), (127, b"\x7f"), (128, b"\x80\x01"),
             (16383, b"\xff\x7f"), (16384, b"\x80\x80\x01")]
    for n, want in cases:
        got = enc_len(n)
        if got != want:
            fails.append("enc_len({}) = {} 期望 {}".format(n, got, want))

    # 字符串编码
    if enc_str("MQTT") != b"\x00\x04MQTT":
        fails.append("enc_str('MQTT') 不对")
    if enc_str("") != b"\x00\x00":
        fails.append("enc_str('') 不对")

    # CONNECT 固定头
    pkt = build_connect("dev1")
    if pkt[0] != 0x10:
        fails.append("CONNECT 首字节应为 0x10，实得 0x%02X" % pkt[0])
    if b"MQTT" not in pkt or bytes([4]) not in pkt:
        fails.append("CONNECT 里缺少协议名或协议级别 4")

    # CONNECT flags：有 username/password 时 bit7/bit6 要置位
    # （直接在变长头之后定位，避免手算偏移）
    pkt2 = build_connect("d", "u", "p")
    idx = pkt2.index(b"MQTT") + 4
    if pkt2[idx] != 4:
        fails.append("协议级别应为 4")
    if pkt2[idx + 1] != 0xC2:
        fails.append("connect flags 应为 0xC2（clean + user + pass），"
                     "实得 0x%02X" % pkt2[idx + 1])

    # PUBLISH QoS0
    pub = build_publish("a/b", "hi")
    if pub[0] != 0x30:
        fails.append("PUBLISH QoS0 首字节应为 0x30")

    # 用独立路径再算一遍 token，与 onenet_token.py 对齐
    key = base64.b64encode(b"testkey123").decode()
    tok, _ = build_token("dafdfadfafdaf", "che1", key,
                         method="sha1", et=1537255523)
    want = ("version=2018-10-31"
            "&res=products%2Fdafdfadfafdaf%2Fdevices%2Fche1"
            "&et=1537255523"
            "&method=sha1"
            "&sign=JPKaZaHGG%2BG5ghKJ3%2FnhhBf%2F3mI%3D")
    if tok != want:
        fails.append("token 与 onenet_token.py 的向量不一致:\n  {}\n  {}"
                     .format(want, tok))

    # ★ 完整上报报文（--post）必须覆盖 PROP_CHECKS 里的**每一个**标识符，
    #   且不得出现旧名字 "temp"。
    #   回归背景：上一版 --post 把标识符手编在字符串里，改成平台标准点
    #   CurrentTemperature 时漏改，导致 --probe-props 15 个全 200、
    #   --post 却回 2306 —— 看起来像"物模型没导入成功"，实为探针自身漂移。
    post = build_post_payload(ts_ms=1700000000000, msg_id="1")
    for _name, _pt, _tpl, _desc in PROP_CHECKS:
        if '"{}":'.format(_name) not in post:
            fails.append("--post 报文缺标识符 {}（应从 PROP_CHECKS 派生）"
                         .format(_name))
    if '"CurrentTemperature":' not in post:
        fails.append('--post 报文里没有 CurrentTemperature')
    if '"temp":' in post:
        fails.append('--post 报文里出现了旧标识符 "temp"（应为 '
                     'CurrentTemperature）')
    try:
        import json as _json
        obj = _json.loads(post)
        got = sorted(obj["params"].keys())
        want = sorted(n for n, _t, _tp, _d in PROP_CHECKS)
        if got != want:
            fails.append("--post 报文的属性集合与 PROP_CHECKS 不一致")
    except Exception as exc:                # noqa: BLE001
        fails.append("--post 报文不是合法 JSON：{}".format(exc))

    if fails:
        print("[FAIL] onenet_probe 自检未通过：")
        for f in fails:
            print("  - {}".format(f))
        return 1
    print("[ OK ] onenet_probe 自检通过"
          "（报文编码 + token 向量 + --post 覆盖 {} 个标识符）"
          .format(len(PROP_CHECKS)))
    return 0


# ---------------------------------------------------------------------------
#  主流程
# ---------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="OneNET 接入探针（PC 侧，零第三方依赖）",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--product-id",  help="产品ID")
    ap.add_argument("--device-name", help="设备名称")
    ap.add_argument("--access-key",  help="访问密钥（base64）—— 用来**生成** token")
    ap.add_argument("--token",       help="直接给现成的 token（password）—— "
                                         "跳过生成。与 --access-key 二选一")
    # 默认值刻意留成 None：好让 环境变量 / 凭据文件 有机会生效。
    # 真正的默认值在凭据解析之后补。
    ap.add_argument("--host", default=None)
    ap.add_argument("--port", type=int, default=None)
    ap.add_argument("--method", default="sha1")
    ap.add_argument("--et", type=int, default=None)
    ap.add_argument("--env-file", default=DEFAULT_ENV_FILE,
                    help="本地凭据文件（默认 secrets/onenet.env，已 gitignore）")
    ap.add_argument("--no-env-file", action="store_true",
                    help="不读凭据文件，强制只用命令行/环境变量")
    ap.add_argument("--post", action="store_true",
                    help="连上后发一条完整的属性上报")
    ap.add_argument("--probe-props", action="store_true",
                    help="★ 逐个属性点名检查，给出「缺哪些」的完整清单")
    ap.add_argument("--listen", type=float, default=0.0,
                    help="订阅下行并等待 N 秒")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    # ---- 补齐凭据：命令行 > 环境变量 > secrets/onenet.env ----
    env_file = None if args.no_env_file else args.env_file
    notes, warns = resolve_credentials(args, env_file)
    for w in warns:
        print("[警告] {}".format(w), file=sys.stderr)
    if notes:
        print("[凭据来源]")
        for n in notes:
            print("  " + n)

    # 兜底默认值（前面刻意没设，见 argparse 注释）
    if not args.host:
        args.host = "studio-mqtt.heclouds.com"
    if not args.port:
        args.port = 1883

    missing = [n for n, v in (("--product-id", args.product_id),
                              ("--device-name", args.device_name)) if not v]
    if missing:
        ap.error("缺少参数: {}\n"
                 "（也可以写进 {} 或设成环境变量 ONENET_PRODUCT_ID / "
                 "ONENET_DEVICE_NAME）".format(" ".join(missing),
                                               DEFAULT_ENV_FILE))
    if not args.token and not args.access_key:
        ap.error("需要 --access-key（生成 token）或 --token（直接给现成 token）\n"
                 "（也可以写进 {} 的 ONENET_TOKEN= 一行）".format(
                     DEFAULT_ENV_FILE))

    # ---- 取 token：现成的优先，否则用访问密钥算 ----
    if args.token:
        token = args.token
        et = None
        print("[信息] 使用**现成**的 token（不重新生成）")
    else:
        try:
            token, et = build_token(args.product_id, args.device_name,
                                    args.access_key, method=args.method,
                                    et=args.et)
        except Exception as exc:            # noqa: BLE001
            print("[ERROR] token 生成失败：{}".format(exc), file=sys.stderr)
            return 2

    # ---- token 结构自检（把"结构性错误"和"密钥错误"分开）----
    parts, terr = parse_token(token)
    if terr:
        print("[ERROR] token 结构不对：{}".format(terr), file=sys.stderr)
        return 2

    problems = verify_token(parts, args.product_id, args.device_name)
    if et is None:
        et = token_et(parts)

    print("=" * 70)
    print(" OneNET 接入探针")
    print("=" * 70)
    print(" broker   : {}:{}".format(args.host, args.port))
    print(" clientId : {}   (= 设备名称)".format(args.device_name))
    print(" username : {}   (= 产品ID)".format(args.product_id))
    print(" token 拆解：")
    for k in ("version", "res", "et", "method"):
        v = parts.get(k)
        if k == "et" and v is not None:
            try:
                v = "{}  ({})".format(
                    v, time.strftime("%Y-%m-%d %H:%M:%S",
                                     time.localtime(int(v))))
            except ValueError:
                pass
        print("   {:<8} = {}".format(k, v))
    print("   {:<8} = {}".format("sign", (parts.get("sign") or "")[:44] + "..."))
    print("-" * 70)

    if problems:
        print("[FAIL] token 自检**不通过**：")
        for p in problems:
            print("  - {}".format(p))
        print()
        print("这些是**结构**问题，连都不用连就知道是错的。")
        return 2
    print("[ OK ] token 结构自检通过（version / res / method / et / sign）")
    print()
    c = Mqtt(args.host, args.port)
    try:
        rc = c.connect(args.device_name, args.product_id, token)
    except Exception as exc:                # noqa: BLE001
        print("[FAIL] 连接失败：{}".format(exc), file=sys.stderr)
        print("       网络不通 / DNS 解析不了 / 被防火墙挡了")
        return 3

    print("CONNACK rc={}  ->  {}".format(rc, CONNACK_TEXT.get(rc, "未知")))
    if rc != 0:
        print()
        print("[FAIL] 鉴权未通过。按这个顺序查：")
        print("  1. username 必须是**产品ID**（不是设备ID）")
        print("  2. clientId 必须是**设备名称**（不是设备ID、不是产品ID）")
        print("  3. et 是否已过期（上面有打印）")
        print("  4. 访问密钥是否复制完整（只在创建时完整显示一次）")
        print("  5. 产品数据格式是否选的是「物模型 / OneJSON」")
        c.close()
        return 4

    print("[ OK ] 鉴权通过 —— token 算法与三要素都正确")
    print()

    base = "$sys/{}/{}".format(args.product_id, args.device_name)
    topic_post = base + "/thing/property/post"
    topic_post_reply = base + "/thing/property/post/reply"
    topic_set = base + "/thing/property/set"

    # ---- 订阅应答与下行 ----
    topics = [(topic_post_reply, 0)]
    if args.listen > 0:
        topics.append((topic_set, 0))
    try:
        codes = c.subscribe(1, topics)
        print("[ OK ] 订阅成功")
        for (t, _q), code in zip(topics, codes):
            print("        {}  (code {})".format(t, code))
            if code == 0x80:
                print("        ^^^ 平台**拒绝**了这个订阅 —— "
                      "多半是 topic 拼错或产品类型不对")
    except Exception as exc:                # noqa: BLE001
        print("[WARN] 订阅失败：{}".format(exc))

    # ---- 发一条属性上报 ----
    if args.post:
        # ★ 从 PROP_CHECKS 派生（覆盖全部属性），不再手写字面量
        payload = build_post_payload()

        print()
        print("[发送] {}  ->  {} 字节".format(topic_post, len(payload)))
        print("       {}".format(payload[:120] + "..."))
        try:
            c.publish(topic_post, payload)
            print("[ OK ] 已发布")
        except Exception as exc:            # noqa: BLE001
            print("[FAIL] 发布失败：{}".format(exc))
            c.close()
            return 5

        pkt = c._read_packet_timeout(10.0)
        if pkt is None:
            print("[WARN] 10 秒内没收到应答 —— 可能只是没订阅到 reply topic")
        else:
            ptype, _f, body = pkt
            if ptype == PUBLISH:
                t, pl = parse_publish(body)
                print("[收到] {}".format(t))
                print("       {}".format(pl))
                if '"code":200' in pl.replace(" ", ""):
                    print("[ OK ] 平台接受了属性上报 —— "
                          "物模型标识符与报文格式都对")
                else:
                    print("[WARN] 平台返回了非 200 —— 看上面的 msg")
                    print("       code=2306 identifier not exist → 物模型里没这个"
                          "标识符（拼错 / 没导入 / 导入后没点「保存」）")
            else:
                print("[收到] 报文类型 {}（不是 PUBLISH）".format(ptype))

    # ---- 逐个属性点名检查 ----
    if args.probe_props:
        print()
        print("=" * 70)
        print(" 物模型属性点名检查（共 {} 个）".format(len(PROP_CHECKS)))
        print("=" * 70)
        print(" ⚠️ 平台一次只说一个「属性不存在」，所以这里逐个发。")
        print()

        res = probe_properties(c, topic_post)

        have, lack, unknown = [], [], []
        for name, ptype, desc, code, detail in res:
            if code == 200:
                mark = "OK  "
                have.append((name, ptype, desc))
            elif code == 2306:
                mark = "缺失"
                lack.append((name, ptype, desc))
            else:
                mark = "?   "
                unknown.append((name, ptype, desc, code, detail))

            code_s = "code={}".format(code) if code is not None else "无应答"
            print("  [{}] {:<12} {:<7} {:<16} {}".format(
                mark, name, ptype, desc, code_s))

        print()
        print("-" * 70)
        if not lack and not unknown:
            print("[ OK ] 全部 {} 个属性都已存在，物模型配置正确".format(len(have)))
        else:
            if lack:
                print("[缺 {} 个] 去 OneNET 控制台 → 产品 → 物模型 → 添加功能点："
                      .format(len(lack)))
                print()
                print("  {:<12} {:<8} {}".format("标识符", "类型", "说明"))
                print("  " + "-" * 44)
                for name, ptype, desc in lack:
                    print("  {:<12} {:<8} {}".format(name, ptype, desc))
                print()
                print("  ⚠️ 标识符**大小写敏感**，必须一模一样。")
                print("  ⚠️ 读写权限：只有 CurrentTemperature（平台标准点自带 rw）")
                print("     和 cmd（下行指令走它）是**读写**；其余 13 个都是**只读**。")
            if unknown:
                print()
                print("[异常 {} 个] 既不是 200 也不是 2306 —— 看下面原始应答："
                      .format(len(unknown)))
                for name, _pt, _d, code, detail in unknown:
                    print("  {}: code={}  {}".format(name, code, detail))

        c.close()
        print()
        return 0 if (not lack and not unknown) else 1

    # ---- 等下行的属性设置 ----
    if args.listen > 0:
        print()
        print("[等待] 订阅下行 {} 秒 —— 去 OneNET 控制台下发一次属性设置"
              .format(args.listen))
        print("       下行报文长这样："
              '{"id":"123","params":{"cmd":"REPORT_NOW"}}')
        end = time.time() + args.listen
        while True:
            left = end - time.time()
            if left <= 0:
                break
            pkt = c._read_packet_timeout(left)
            if pkt is None:
                break
            ptype, _f, body = pkt
            if ptype == PUBLISH:
                t, pl = parse_publish(body)
                print("[收到] {}".format(t))
                print("       {}".format(pl))
                print("       ^^^ 固件里 esp32_json_onenet_parse_set() "
                      "要能从这个报文里取出 cmd 与 id")
            elif ptype == PINGRESP:
                pass

    c.close()
    print()
    print("=" * 70)
    print(" 探针结束。凭据没问题的话，接下来才是固件的事：")
    print("   powershell -File tools\\esp32_build.ps1 -Flash COM<x>")
    print("=" * 70)
    return 0


if __name__ == "__main__":
    sys.exit(main())
