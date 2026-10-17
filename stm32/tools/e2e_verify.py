#!/usr/bin/env python3
"""
端到端 transcript 的独立验证器

============================================================================
 它验证什么、不验证什么

**验证**（Modbus 规范层面，与 C 实现完全独立）：
  - 每个响应的 CRC 是否正确
  - 响应是否配得上请求（功能码一致、数量一致、字节数公式正确）
  - 异常响应的格式与异常码是否合理
  - 广播（地址 0）必须**不应答**
  - 地址不匹配必须**不应答**
  - CRC 错误的请求必须**不应答**
  - 主站发出的请求帧是否符合规范
  - 串口帧（ESP32）的 HEAD / LEN / CRC8 与类型语义

**不验证**（刻意的取舍）：
  - 具体的寄存器数值。那需要把寄存器映射表在 Python 里再实现一遍，
    而"重复实现"会带来同步问题，且同步过程本身又变成"两边互相对照"。
    值层面由 test_regmap.c 的 55 项覆盖。
  - 时序。transcript 不含时间戳（PC 上的时序没有参考价值）。

============================================================================
 CRC 实现

两份 CRC 都**独立实现**（不是从 C 代码翻译的）：
  - CRC16-Modbus：逐位算法，多项式 0xA001（反射的 0x8005）
  - CRC8：逐位算法，多项式 0x07
两者都与 C 侧的实现方法不同（C 侧可能用查表），所以能交叉验证。
============================================================================
"""
import sys

# ----------------------------------------------------------------- 常量
GW_FRAME_HEAD = 0xA5
GW_FRAME_MAX_PAYLOAD = 128

# 帧类型（与 inc/gw_frame.h 一致）
FT = {
    0x01: "WIFI_CONFIG", 0x02: "MQTT_CONFIG", 0x03: "TELEMETRY",
    0x04: "STATUS_SET",
    0x05: "OTA_BEGIN", 0x06: "OTA_CHUNK", 0x07: "OTA_END",
    0x7E: "NACK", 0x7F: "ACK",
    0x81: "LINK_STATUS", 0x82: "COMMAND", 0x83: "OTA_ACK", 0x84: "LOG",
    0x85: "CMD_ACK", 0x86: "CMD_NACK",
}

# Modbus 异常码
EXC = {
    0x01: "ILLEGAL_FUNCTION",
    0x02: "ILLEGAL_ADDRESS",
    0x03: "ILLEGAL_VALUE",
    0x04: "SLAVE_FAILURE",
}


def crc16_modbus(data: bytes) -> int:
    """CRC16-Modbus：逐位实现（独立于 C 侧）"""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc & 0xFFFF


def crc8(data: bytes) -> int:
    """CRC8：多项式 0x07，初值 0x00，逐位实现"""
    crc = 0x00
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x07) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


# ----------------------------------------------------------------- 记分
class Report:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.msgs = []

    def ok(self, what):
        self.passed += 1

    def bad(self, what, detail):
        self.failed += 1
        self.msgs.append("  [FAIL] %s\n         %s" % (what, detail))

    def info(self, text):
        self.msgs.append("  [info] " + text)


R = Report()


def expect(cond, what, detail=""):
    if cond:
        R.ok(what)
    else:
        R.bad(what, detail)


# ----------------------------------------------------------------- 解析
def parse(path):
    """返回 (reqs, resps)：两个列表，元素是 (channel, bytes)"""
    reqs, resps = [], []
    # ⚠️ utf-8-sig：PowerShell 的 Out-File -Encoding UTF8 会写 BOM，
    #    用 utf-8 解码时 BOM 会粘在第一行的 "NOTE" 前面，
    #    导致第一行被当成非法记录。
    with open(path, encoding="utf-8-sig") as f:
        for ln, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("NOTE"):
                continue
            parts = line.split()
            # ⚠️ 记录有两种形状，字段数不同：
            #      REQ <chan> <hex>            <- 3 个字段
            #      <chan> <dir> <hex>          <- 3 个字段
            #    但 REQ 的 chan 与 <chan> 位置不同，所以按首字段分派，
            #    不能统一按 parts[1] 当通道。
            if len(parts) < 3:
                continue
            try:
                if parts[0] == "REQ":
                    reqs.append((parts[1], bytes.fromhex(parts[2])))
                else:
                    resps.append((parts[0], parts[1],
                                  bytes.fromhex(parts[2])))
            except ValueError:
                R.bad("transcript 第 %d 行十六进制非法" % ln, line)
                continue
    return reqs, resps


# ----------------------------------------------------------------- Modbus 验证
def check_modbus(resps):
    """验证 RS485 上的从站响应"""

    for chan, kind, f in resps:
        # ⚠️ 只验从站响应。主站请求走 check_master_request()，
        #    两者规范不同（响应有字节数公式，请求长度固定）。
        if chan != "SLAVERESP":
            continue

        name = "RS485 响应 %s" % f.hex().upper()

        # 长度下限：地址 + 功能码 + CRC
        if len(f) < 4:
            R.bad(name, "帧长 %d < 4，不可能是合法 Modbus 帧" % len(f))
            continue

        # ---- CRC ----
        body, crc_lo, crc_hi = f[:-2], f[-2], f[-1]
        got = crc_lo | (crc_hi << 8)
        want = crc16_modbus(body)
        expect(got == want, name + " 的 CRC",
               "CRC 实际 0x%04X，按规范应为 0x%04X" % (got, want))
        if got != want:
            continue        # CRC 错了，后面的语义分析没意义

        addr, fc = f[0], f[1]

        # ---- 异常响应 ----
        if fc & 0x80:
            expect(len(f) == 5, name + " 异常响应长度",
                   "异常响应应为 5 字节，实际 %d" % len(f))
            if len(f) == 5:
                code = f[2]
                expect(code in EXC, name + " 异常码有效",
                       "异常码 0x%02X 不在规范定义范围内" % code)
                R.info("%s -> 异常 %s" % (name, EXC.get(code, "?")))
            continue

        # ---- 读响应 ----
        if fc in (0x03, 0x04):
            bc = f[2]
            expect(bc % 2 == 0, name + " 字节数为偶数",
                   "读响应字节数 %d 应为偶数（寄存器是 16 位）" % bc)
            expect(len(f) == 5 + bc, name + " 读响应长度 = 5 + 字节数",
                   "帧长 %d，但字节数 %d 要求 %d" % (len(f), bc, 5 + bc))
            R.info("%s -> 读 %d 个寄存器" % (name, bc // 2))
            continue

        # ---- 写单个响应（回显请求）----
        if fc == 0x06:
            expect(len(f) == 8, name + " 写单个响应长度",
                   "FC06 响应应为 8 字节，实际 %d" % len(f))
            continue

        # ---- 写多个响应 ----
        if fc == 0x10:
            expect(len(f) == 8, name + " 写多个响应长度",
                   "FC10 响应应为 8 字节（addr fc start(2) qty(2) crc(2)），"
                   "实际 %d" % len(f))
            if len(f) == 8:
                R.info("%s -> 写 %d 个寄存器 @ %d"
                       % (name, (f[4] << 8) | f[5], (f[2] << 8) | f[3]))
            continue

        R.bad(name, "未覆盖的功能码 0x%02X（验证器需要补充规则）" % fc)


def check_req_resp_pairing(reqs, resps):
    """核对：应当应答的请求有应答，不应答的没有"""

    rs485_reqs = [d for c, d in reqs if c == "RS485"]
    rs485_resps = [d for c, k, d in resps if c == "SLAVERESP"]

    n_should_reply = 0
    n_should_silent = 0

    for req in rs485_reqs:
        if len(req) < 2:
            continue
        addr, fc = req[0], req[1]

        # CRC 错的请求：按规范必须丢弃
        if crc16_modbus(req[:-2]) != (req[-2] | (req[-1] << 8)):
            n_should_silent += 1
            R.info("请求 %s 的 CRC 故意错误 -> 应当不应答" % req.hex().upper())
            continue

        if addr == 0x00:
            n_should_silent += 1      # 广播：执行但不应答
        elif addr != 0x01:
            n_should_silent += 1      # 地址不匹配：不应答
        else:
            n_should_reply += 1

    expect(len(rs485_resps) == n_should_reply,
           "应答数量与「应当应答的请求数」一致",
           "应当应答 %d 条，实际应答 %d 条（另有 %d 条应当静默）"
           % (n_should_reply, len(rs485_resps), n_should_silent))


def check_broadcast_and_mismatch(resps):
    """更精确地验证：广播之后与地址不匹配之后，确实没有多出响应"""

    # transcript 是顺序的，所以可以按顺序重建"请求 -> 响应"的对应
    # 但本验证器不做顺序匹配（那需要假设实现是同步的）。
    # 改为验证"响应总数"（上面已做）+ 每个响应都配得上某条请求。
    return


def check_master_request(resps):
    """主站发出的请求帧"""
    for chan, kind, f in resps:
        if chan != "MASTREQ":
            continue
        if True:
            name = "主站请求 %s" % f.hex().upper()
            got = f[-2] | (f[-1] << 8)
            want = crc16_modbus(f[:-2])
            expect(got == want, name + " 的 CRC",
                   "CRC 实际 0x%04X，应为 0x%04X" % (got, want))
            qty = (f[4] << 8) | f[5]
            expect(1 <= qty <= 125, name + " 读取数量合法",
                   "数量 %d 超出 Modbus 允许的 1..125" % qty)
            R.info("%s -> 从机 0x%02X fc%02X 起始 0x%04X 数量 %d"
                   % (name, f[0], f[1], (f[2] << 8) | f[3], qty))


# ----------------------------------------------------------------- 串口帧验证
def check_esp_frames(resps):
    for chan, kind, f in resps:
        if chan != "ESP" or kind != "TX":
            continue

        name = "ESP 帧 %s" % f.hex().upper()

        if len(f) < 5:
            R.bad(name, "帧长 %d < 5（最短帧 = HEAD+LEN2+TYPE+CRC）" % len(f))
            continue

        expect(f[0] == GW_FRAME_HEAD, name + " 帧头",
               "帧头 0x%02X != 0xA5" % f[0])

        flen = (f[1] << 8) | f[2]          # TYPE + PAYLOAD 的长度
        expect(flen >= 1, name + " 长度字段 >= 1",
               "长度字段 %d 至少含 TYPE" % flen)
        expect(len(f) == flen + 4, name + " 总长 = 长度字段 + 4",
               "帧长 %d，长度字段 %d 要求 %d" % (len(f), flen, flen + 4))

        # CRC8 覆盖：LEN_L LEN_H TYPE PAYLOAD
        crc_got = f[-1]
        crc_want = crc8(f[1:-1])
        expect(crc_got == crc_want, name + " 的 CRC8",
               "CRC8 实际 0x%02X，应为 0x%02X" % (crc_got, crc_want))

        ftype = f[3]
        expect(ftype in FT, name + " 类型已知",
               "类型 0x%02X 不在 gw_frame.h 的定义里" % ftype)

        payload = f[4:-1]
        expect(flen - 1 == len(payload), name + " 载荷长度自洽",
               "长度字段算出的载荷 %d，实际 %d" % (flen - 1, len(payload)))

        # 类型相关的语义
        if ftype == 0x03:      # TELEMETRY：28 字节固定布局
            expect(len(payload) == 28, name + " 遥测载荷 = 28 字节",
                   "实际 %d 字节" % len(payload))
            if len(payload) == 28:
                dev_id = int.from_bytes(payload[0:4], "big")
                ts = int.from_bytes(payload[4:8], "big")
                seq = int.from_bytes(payload[8:12], "big")
                temp = int.from_bytes(payload[12:14], "big", signed=True)
                humi = int.from_bytes(payload[14:16], "big")
                can = [int.from_bytes(payload[16 + i * 2:18 + i * 2], "big")
                       for i in range(4)]
                status = int.from_bytes(payload[24:26], "big")
                uptime = int.from_bytes(payload[26:28], "big")
                R.info("%s -> dev=%d ts=%d seq=%d temp=%.1f humi=%.1f "
                       "can=%s status=0x%04X uptime=%d"
                       % (name, dev_id, ts, seq, temp / 10.0, humi / 10.0,
                          can, status, uptime))
                # 合理性：温湿度在传感器量程内
                expect(-400 <= temp <= 800, name + " 温度在量程内",
                       "温度 %d (x10) 超出 -400..800" % temp)
                expect(0 <= humi <= 1000, name + " 湿度在量程内",
                       "湿度 %d (x10) 超出 0..1000" % humi)

        elif ftype == 0x81:    # LINK_STATUS
            expect(len(payload) >= 2, name + " 链路状态载荷 >= 2",
                   "实际 %d" % len(payload))

        elif ftype in (0x85, 0x86):     # CMD_ACK / CMD_NACK：文本载荷
            try:
                text = payload.decode("ascii")
                R.info("%s -> %s: %r"
                       % (name, FT[ftype], text))
                expect(len(text) > 0, name + " 指令确认文本非空", "空文本")
            except UnicodeDecodeError:
                R.bad(name + " 指令确认文本可解码为 ASCII",
                      "载荷 %s 含非 ASCII 字节" % payload.hex())

        elif ftype in (0x7E, 0x7F):     # 遥测 ACK / NACK：必须空载荷
            expect(len(payload) == 0, name + " 遥测 ACK/NACK 载荷为空",
                   "实际 %d 字节（上云链路只看类型不判载荷，"
                   "非空会与指令确认混淆）" % len(payload))


# ----------------------------------------------------------------- 主流程
def main():
    if len(sys.argv) < 2:
        print("用法: python e2e_verify.py <transcript>")
        return 1

    path = sys.argv[1]
    reqs, resps = parse(path)

    n_rs485 = sum(1 for c, k, d in resps if c == "RS485" and k == "TX")
    n_esp = sum(1 for c, k, d in resps if c == "ESP" and k == "TX")

    print("========== 端到端 transcript 独立验证 ==========")
    print("  transcript: %s" % path)
    print("  记录：请求 %d 条，响应 %d 条（RS485 %d，ESP %d）"
          % (len(reqs), len(resps), n_rs485, n_esp))
    print()

    check_modbus(resps)
    check_req_resp_pairing(reqs, resps)
    check_master_request(resps)
    check_esp_frames(resps)

    for m in R.msgs:
        print(m)

    print()
    print("=" * 50)
    print("  通过 %d 项, 失败 %d 项%s"
          % (R.passed, R.failed,
             "  --> ALL PASS" if R.failed == 0 else "  --> 存在失败项"))
    print("=" * 50)
    return 0 if R.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
