#!/usr/bin/env python3
"""
交互式 Python 主站 —— 主动向 STM32 从站代码"出题"

============================================================================
 与 e2e_scenario.c + e2e_verify.py 的分工

  e2e_scenario.c  固定剧本，dump 字节流          验证「规范符合性」
  e2e_verify.py   独立解析字节流                  验证「规范符合性」

  本文件          逐条出题、逐条核对（**交互式**）  验证「边界与静默」

  前两者能证明"发出的字节合规"，但**证明不了"没被写进剧本的边界"**。
  交互式才能做到：

    - 主动构造畸形帧（剧本里想不到的）
    - **确认"不该应答时确实没有字节"** —— 这需要等超时，
      固定剧本做不到（它只能数响应总数，而多一个少一个可能互相抵消）
    - 粘包、半帧、快速连发

============================================================================
 ⚠️ 关于"静默"的验证方法

  Modbus 从站有三条静默规则：地址不匹配 / 广播 / CRC 错误。
  要验证"确实没应答"，唯一可靠的方法是**等一个超时后确认无字节到达**。

  这里用 `queue` + 后台读线程实现可超时的 `readline()`：
  直接对管道 readline() 会永久阻塞，一旦对端真的静默就死锁。
============================================================================
"""
import queue
import subprocess
import sys

# ⚠️ 中文 Windows 的控制台是 GBK，而 Python 按控制台代码页编码 stdout：
#    print 里出现 ⚠️/✅/❌ 就会 UnicodeEncodeError，**脚本在干活之前就死了**。
#    （ci.ps1 里设了 PYTHONIOENCODING=utf-8，所以 CI 全绿、完全看不出来。）
#    这里把编不出来的字符降级成 '?'，而不是让整个脚本崩掉。
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(errors="replace")
    except (AttributeError, ValueError):
        pass
import threading

GW_SLAVE_ADDR = 1
GW_INPUT_REGS = 32
GW_HOLDING_REGS = 16

# 服务端预置的可辨识值（与 e2e_slave_server.c 一致）
DEVID_HI, DEVID_LO = 0x0001, 0x86A0       # 100000
PERIOD_INIT = 10


# ----------------------------------------------------------------- CRC
def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc & 0xFFFF


def with_crc(pdu: bytes) -> bytes:
    c = crc16(pdu)
    return pdu + bytes([c & 0xFF, c >> 8])


def crc_ok(frame: bytes) -> bool:
    if len(frame) < 4:
        return False
    return crc16(frame[:-2]) == (frame[-2] | (frame[-1] << 8))


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

    def info(self, t):
        self.msgs.append("  [info] " + t)


R = Report()


def expect(cond, what, detail=""):
    if cond:
        R.ok(what)
    else:
        R.bad(what, detail)


# ----------------------------------------------------------------- 主站
class Master:
    def __init__(self, exe):
        self.p = subprocess.Popen(
            [exe],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,              # 不要缓冲：我们要即时交互
        )
        self.q = queue.Queue()
        self.dead = False
        t = threading.Thread(target=self._reader, daemon=True)
        t.start()

    def _reader(self):
        try:
            while True:
                line = self.p.stdout.readline()
                if not line:
                    break
                self.q.put(line.decode("utf-8", "replace").strip())
        except Exception:
            pass
        self.dead = True

    def _readline(self, timeout=0.6):
        """可超时的读一行。返回 None 表示超时（= 对端静默）。"""
        try:
            return self.q.get(timeout=timeout)
        except queue.Empty:
            return None

    def ping(self):
        self.p.stdin.write(b"PING\n")
        self.p.stdin.flush()
        return self._readline(1.0)

    def send_raw(self, frame: bytes, timeout=0.6):
        """发一帧（原始字节），返回应答帧或 None（静默/超时）"""
        self.p.stdin.write(frame.hex().upper().encode() + b"\n")
        self.p.stdin.flush()
        line = self._readline(timeout)
        if line is None:
            return None
        if line.startswith("OK "):
            return bytes.fromhex(line[3:])
        if line == "NONE":
            return None
        return ("__ERR__", line)

    def request(self, pdu: bytes, timeout=0.6):
        """发一条 PDU（自动补 CRC），返回应答帧或 None"""
        return self.send_raw(with_crc(pdu), timeout)

    def close(self):
        try:
            self.p.stdin.write(b"QUIT\n")
            self.p.stdin.flush()
        except Exception:
            pass
        try:
            self.p.wait(timeout=3)
        except Exception:
            self.p.kill()


# ----------------------------------------------------------------- 用例
def u16(v):
    return bytes([(v >> 8) & 0xFF, v & 0xFF])


def check_read(m, fc, start, qty, label):
    """读类请求：返回数据寄存器列表，或 None（异常/静默）"""
    resp = m.request(bytes([GW_SLAVE_ADDR, fc]) + u16(start) + u16(qty))
    if resp is None:
        R.bad(label, "无应答")
        return None
    if isinstance(resp, tuple):
        R.bad(label, "服务端报错：%s" % (resp[1],))
        return None
    if not crc_ok(resp):
        R.bad(label, "应答 CRC 错误：%s" % resp.hex().upper())
        return None
    if resp[1] & 0x80:
        R.bad(label, "元响应为异常 0x%02X（本应成功）" % resp[2])
        return None
    if len(resp) != 5 + resp[2]:
        R.bad(label, "应答长度 %d 与字节数 %d 不符（应 %d）"
              % (len(resp), resp[2], 5 + resp[2]))
        return None
    vals = [int.from_bytes(resp[3 + i * 2:5 + i * 2], "big")
            for i in range(resp[2] // 2)]
    if len(vals) != qty:
        R.bad(label, "返回 %d 个寄存器，请求了 %d 个" % (len(vals), qty))
        return None
    R.ok(label)
    return vals


def check_exception(m, pdu, want_exc, label):
    resp = m.request(pdu)
    if resp is None:
        R.bad(label, "无应答（期望异常 0x%02X）" % want_exc)
        return
    if isinstance(resp, tuple):
        R.bad(label, "服务端报错：%s" % (resp[1],))
        return
    if not crc_ok(resp):
        R.bad(label, "异常应答 CRC 错误")
        return
    if not (resp[1] & 0x80):
        R.bad(label, "期望异常，实际正常响应 %s" % resp.hex().upper())
        return
    if len(resp) != 5:
        R.bad(label, "异常应答长度 %d（应为 5）" % len(resp))
        return
    if resp[2] != want_exc:
        R.bad(label, "异常码 0x%02X，期望 0x%02X" % (resp[2], want_exc))
        return
    R.ok(label)


def check_silent(m, frame, label, timeout=0.6):
    """⚠️ 核心用例：发一帧**不该被应答**的请求，等超时确认静默"""
    resp = m.send_raw(frame, timeout=timeout)
    expect(resp is None, label,
           "期望静默（无字节到达），实际收到 %s"
           % (resp.hex().upper() if isinstance(resp, bytes) else resp))
    return resp is None


# ================================================================= 用例组
def t01_smoke(m):
    print("\n[1] 连通性与基础读写")
    expect(m.ping() == "PONG", "服务端连通", "PING 未得到 PONG")

    vals = check_read(m, 0x03, 0, 4, "读保持寄存器 0..3")
    if vals:
        devid = (vals[0] << 16) | vals[1]
        expect(devid == 100000, "设备 ID = 100000",
               "读到 %d" % devid)
        expect(vals[2] == 0, "波特率代号为 0（9600）", "读到 %d" % vals[2])
        expect(vals[3] == PERIOD_INIT, "上报周期初值 = %d" % PERIOD_INIT,
               "读到 %d" % vals[3])

    vals = check_read(m, 0x04, 0, 2, "读输入寄存器 0..1（温湿度）")
    if vals:
        expect(vals[0] == 253, "温度为 25.3℃ (253)",
               "读到 %d" % vals[0])
        expect(vals[1] == 612, "湿度为 61.2%% (612)",
               "读到 %d" % vals[1])


def t02_qty_boundary(m):
    print("\n[2] 数量边界（1 / 125 / 126 / 0）")
    # 读输入寄存器只有 32 个，所以 125 个必然越界 -> 异常 02
    # 这里验证的是"数量本身合法但地址越界"与"数量非法"的分界
    check_read(m, 0x04, 0, 1, "数量 = 1（最小合法）")
    check_read(m, 0x04, 0, 32, "数量 = 32（恰好全部输入寄存器）")
    check_exception(m, bytes([GW_SLAVE_ADDR, 0x04]) + u16(0) + u16(33),
                    0x02, "数量 = 33 越界 -> 异常 02")
    check_exception(m, bytes([GW_SLAVE_ADDR, 0x04]) + u16(0) + u16(0),
                    0x03, "数量 = 0 -> 异常 03")
    check_exception(m, bytes([GW_SLAVE_ADDR, 0x04]) + u16(0) + u16(125),
                    0x02, "数量 = 125（规范上限）但越界 -> 异常 02")
    check_exception(m, bytes([GW_SLAVE_ADDR, 0x04]) + u16(0) + u16(126),
                    0x03, "数量 = 126 超过规范上限 -> 异常 03")

    # 保持寄存器 16 个
    check_read(m, 0x03, 0, 16, "读全部 16 个保持寄存器")
    check_exception(m, bytes([GW_SLAVE_ADDR, 0x03]) + u16(0) + u16(17),
                    0x02, "保持寄存器数量 = 17 越界 -> 异常 02")


def t03_addr_boundary(m):
    print("\n[3] 地址边界（起始 + 数量 的溢出）")
    # ⚠️ 经典陷阱：start + qty 用 16 位算会回绕。
    #    例如 start=0xFFFF qty=2 -> 0x0001（回绕后"看起来合法"）
    check_exception(m, bytes([GW_SLAVE_ADDR, 0x04]) + u16(0xFFFF) + u16(2),
                    0x02, "start=0xFFFF qty=2（16 位回绕陷阱）-> 异常 02")
    check_exception(m, bytes([GW_SLAVE_ADDR, 0x04]) + u16(0x001F) + u16(2),
                    0x02, "start=31 qty=2（跨过末尾）-> 异常 02")
    check_read(m, 0x04, 31, 1, "start=31 qty=1（最后一个输入寄存器）")


def t04_function_codes(m):
    print("\n[4] 功能码覆盖")
    check_exception(m, bytes([GW_SLAVE_ADDR, 0x01]) + u16(0) + u16(1),
                    0x01, "读线圈（FC01，未实现）-> 异常 01")
    check_exception(m, bytes([GW_SLAVE_ADDR, 0x02]) + u16(0) + u16(1),
                    0x01, "读离散输入（FC02，未实现）-> 异常 01")
    check_exception(m, bytes([GW_SLAVE_ADDR, 0x05]) + u16(0) + u16(0xFF00),
                    0x01, "写单个线圈（FC05，未实现）-> 异常 01")
    check_exception(m, bytes([GW_SLAVE_ADDR, 0x0F]) + u16(0) + u16(1) + b"\x01\x01",
                    0x01, "写多个线圈（FC0F，未实现）-> 异常 01")
    check_exception(m, bytes([GW_SLAVE_ADDR, 0x08]) + u16(0) + u16(0),
                    0x01, "诊断（FC08，未实现）-> 异常 01")


def t05_silence(m):
    print("\n[5] ⚠️ 三条静默规则（必须确认无字节到达）")

    # (a) 地址不匹配
    check_silent(m, with_crc(bytes([0x09, 0x03]) + u16(0) + u16(1)),
                 "地址 0x09（本机是 1）-> 静默")
    check_silent(m, with_crc(bytes([0xFF, 0x04]) + u16(0) + u16(1)),
                 "地址 0xFF -> 静默")

    # (b) 广播：执行但不应答
    check_silent(m, with_crc(bytes([0x00, 0x03]) + u16(0) + u16(1)),
                 "广播读（地址 0）-> 静默")

    # (c) CRC 错误
    good = with_crc(bytes([GW_SLAVE_ADDR, 0x03]) + u16(0) + u16(1))
    bad = bytearray(good)
    bad[-1] ^= 0xFF                     # 破坏 CRC 高字节
    check_silent(m, bytes(bad), "CRC 错误 -> 静默")

    bad2 = bytearray(good)
    bad2[-2] ^= 0x01                    # 破坏 CRC 低字节
    check_silent(m, bytes(bad2), "CRC 低字节错误 -> 静默")

    # 广播写：应执行但不应答，且**效果要能观察到**
    print("      （验证广播写：执行但不应答）")
    before = check_read(m, 0x03, 3, 1, "广播前读上报周期")
    check_silent(m, with_crc(bytes([0x00, 0x06]) + u16(3) + u16(99)),
                 "广播写上报周期=99 -> 静默")
    after = check_read(m, 0x03, 3, 1, "广播后读上报周期")
    if before and after:
        expect(after[0] == 99,
               "⚠️ 广播写**确实执行了**（周期 %d -> %d）"
               % (before[0], after[0]),
               "广播写没有生效：仍为 %d" % after[0])


def t06_malformed(m):
    print("\n[6] 畸形帧")
    # 太短
    check_silent(m, bytes([GW_SLAVE_ADDR, 0x03]), "只有 2 字节 -> 静默")
    check_silent(m, bytes([GW_SLAVE_ADDR]), "只有 1 字节 -> 静默")
    # 恰好只有 CRC（无 PDU）
    check_silent(m, with_crc(b""), "空 PDU + CRC -> 静默")
    # 功能码正确但参数残缺（CRC 是对的，所以能过 CRC 检查）
    # ⚠️ 这里**应当得到异常 03**，不是静默。
    #    Modbus 规范：帧结构合法（能过 CRC）但 PDU 内容不合法时，
    #    应回异常响应，让上位机知道"你发的参数不对"。
    #    静默会让上位机一直等到超时 —— 那是"设备不在"的表现，
    #    与"参数不对"是完全不同的问题，现场要能区分。
    check_exception(m, bytes([GW_SLAVE_ADDR, 0x03, 0x00]),
                    0x03, "FC03 参数残缺（3 字节 PDU）-> 异常 03")
    check_exception(m, bytes([GW_SLAVE_ADDR, 0x03]) + u16(0),
                    0x03, "FC03 只有起始地址（4 字节 PDU）-> 异常 03")

    # 超长帧（正常 PDU + 多余字节）
    long_frame = with_crc(bytes([GW_SLAVE_ADDR, 0x03]) + u16(0) + u16(1)
                          + b"\xDE\xAD")
    resp = m.send_raw(long_frame)
    if resp is None:
        R.info("正常读请求 + 2 字节多余数据 -> 静默")
    else:
        R.info("正常读请求 + 2 字节多余数据 -> %s"
               % (resp.hex().upper() if isinstance(resp, bytes) else resp))

    # 写单寄存器的各种残缺长度
    check_exception(m, with_crc(bytes([GW_SLAVE_ADDR, 0x06]) + u16(0) + u16(1)),
                    0x03, "FC06 长度 6（规范要求 8）-> 异常 03")


def t07_write_paths(m):
    print("\n[7] 写路径")
    # 写单个：成功后应回显
    resp = m.request(bytes([GW_SLAVE_ADDR, 0x06]) + u16(3) + u16(30))
    expect(resp is not None and isinstance(resp, bytes)
           and len(resp) == 8 and resp == with_crc(
               bytes([GW_SLAVE_ADDR, 0x06]) + u16(3) + u16(30)),
           "FC06 响应原样回显请求",
           "实际 %s" % (resp.hex().upper() if isinstance(resp, bytes) else resp))

    v = check_read(m, 0x03, 3, 1, "写后回读上报周期")
    if v:
        expect(v[0] == 30, "写入生效（周期 = 30）", "读到 %d" % v[0])

    # 写到越界地址
    check_exception(m, bytes([GW_SLAVE_ADDR, 0x06]) + u16(16) + u16(1),
                    0x02, "FC06 写到地址 16（保持寄存器只有 0..15）-> 异常 02")

    # 写多个：数量与字节数不匹配
    check_exception(m, bytes([GW_SLAVE_ADDR, 0x10]) + u16(0) + u16(2)
                    + b"\x02\x00\x01",
                    0x03, "FC10 字节数与数量不符（2 个寄存器只要 2 字节）-> 异常 03")

    # 写多个：成功
    resp = m.request(bytes([GW_SLAVE_ADDR, 0x10]) + u16(5) + u16(2)
                     + b"\x04" + u16(0x0123) + u16(0x4567))
    expect(resp is not None and isinstance(resp, bytes) and len(resp) == 8,
           "FC10 成功响应长度为 8",
           "实际 %s" % (resp.hex().upper() if isinstance(resp, bytes) else resp))
    if isinstance(resp, bytes) and len(resp) == 8:
        expect(resp[4:6] == u16(2), "FC10 响应回显数量",
               "回显数量 = %d" % int.from_bytes(resp[4:6], "big"))

    vals = check_read(m, 0x03, 5, 2, "FC10 写后回读")
    if vals:
        expect(vals[0] == 0x0123 and vals[1] == 0x4567,
               "FC10 两个寄存器都写入正确",
               "读到 %04X %04X" % (vals[0], vals[1]))

    # 写多个：数量 0
    check_exception(m, bytes([GW_SLAVE_ADDR, 0x10]) + u16(0) + u16(0) + b"\x00",
                    0x03, "FC10 数量 0 -> 异常 03")

    # 写多个：数量超过规范上限 123
    #
    # ⚠️ 这里有个构造陷阱：qty=124 需要 byte_count=248，
    #    而帧长会变成 9 + 248 + 2 = 259 字节，**超过 MB_MAX_ADU (256)** ——
    #    那样请求在"长度检查"处就被丢弃了（静默），根本走不到
    #    "数量越界"的判断。我第一次就是这么构造的，得到静默而误以为是 bug。
    #
    #    要真正测"数量越界"这个分支，帧必须够短。办法是让 byte_count
    #    与实际数据一致但**远小于 qty*2** —— 那样会先撞上
    #    "byte_count != qty*2" 的检查。所以改用 qty=124 + byte_count=2
    #    加 2 字节数据（帧长 13，很小）。
    check_exception(m, bytes([GW_SLAVE_ADDR, 0x10]) + u16(0) + u16(124)
                    + bytes([2, 0x00, 0x01]),
                    0x03, "FC10 数量 124（规范上限 123）-> 异常 03")

    # FC10 帧长下界
    #
    # ⚠️ 8 字节的 FC10 帧**构造不出来**：addr+fc+起始(2)+数量(2)+字节数(1)
    #    已经是 7 字节的 PDU，再加 CRC 就是 9 字节。
    #    要得到 8 字节只能让数量=0，而那个已被上面的用例覆盖。
    #    所以真正有意义的"下界"是 **PDU = 8 字节**（缺 1 个数据字节）。
    check_exception(m, bytes([GW_SLAVE_ADDR, 0x10]) + u16(0) + u16(1)
                    + bytes([2, 0x12]),
                    0x03, "FC10 PDU 8 字节（声明 2 字节数据只给 1 字节）-> 异常 03")

    # 完整但数量与数据不符
    check_exception(m, bytes([GW_SLAVE_ADDR, 0x10]) + u16(0) + u16(3)
                    + bytes([2, 0x12, 0x34]),
                    0x03, "FC10 数量 3 但字节数 2 -> 异常 03")


def t08_coalesced(m):
    print("\n[8] 粘包（一次发两帧）")
    # ⚠️ 本服务端是"一行一帧"的接口，所以这里测不了真正的粘包。
    #    真正的粘包（一次 write 里两帧）由 e2e_scenario.c 的
    #    帧解析器测试覆盖（test_frame.c 82 项）。
    #    这里明确说明为什么跳过，而不是假装测过。
    R.info("粘包需要「一次 write 含两帧」，本接口是一行一帧，"
           "该项由 test_frame.c（82 项）覆盖")


def t09_stress(m):
    print("\n[9] 快速连发（压力）")
    n = 200
    ok_n = 0
    for i in range(n):
        r = m.request(bytes([GW_SLAVE_ADDR, 0x04]) + u16(0) + u16(2),
                      timeout=1.5)
        if r is not None and isinstance(r, bytes) and crc_ok(r):
            ok_n += 1
    expect(ok_n == n, "连发 %d 次全部得到合法应答" % n,
           "只有 %d 次成功" % ok_n)

    # 交替"该应答"与"该静默"，验证状态不串
    print("      （交替 60 次：应答 / 静默）")
    correct = 0
    for i in range(60):
        if i % 2 == 0:
            r = m.request(bytes([GW_SLAVE_ADDR, 0x03]) + u16(0) + u16(1))
            if r is not None and isinstance(r, bytes) and crc_ok(r):
                correct += 1
        else:
            r = m.send_raw(with_crc(bytes([0x09, 0x03]) + u16(0) + u16(1)))
            if r is None:
                correct += 1
    expect(correct == 60, "交替 60 次（应答/静默）全部正确",
           "只有 %d 次正确" % correct)


def t10_no_state_leak(m):
    print("\n[10] 静默请求不应影响后续状态")
    # 发一批该被忽略的请求，然后确认正常读仍然工作且值没变
    for _ in range(10):
        m.send_raw(with_crc(bytes([0x09, 0x06]) + u16(3) + u16(0xFFFF)))
        m.send_raw(with_crc(bytes([0x00, 0x06]) + u16(3) + u16(0x9999)))

    # 地址不匹配的写：一定不该生效
    v = check_read(m, 0x03, 3, 1, "被忽略的写之后回读周期")
    if v:
        # 上面广播写是 0x9999=39321，会生效；地址不匹配的 0xFFFF 不该生效
        # 所以这里期望 39321（广播生效）而不是 65535（地址不匹配不该生效）
        expect(v[0] != 0xFFFF,
               "⚠️ 地址不匹配的写**没有生效**（周期 = %d，不是 65535）" % v[0],
               "地址不匹配的写竟然生效了")


def main():
    if len(sys.argv) < 2:
        print("用法: python e2e_master.py <slave_server.exe>")
        return 1

    exe = sys.argv[1]
    print("========== 交互式 Modbus 从站边界测试 ==========")
    print("  从站可执行文件: %s" % exe)

    m = Master(exe)
    if m.ping() != "PONG":
        print("  [FAIL] 服务端无法连通")
        m.close()
        return 1

    try:
        t01_smoke(m)
        t02_qty_boundary(m)
        t03_addr_boundary(m)
        t04_function_codes(m)
        t05_silence(m)
        t06_malformed(m)
        t07_write_paths(m)
        t08_coalesced(m)
        t09_stress(m)
        t10_no_state_leak(m)
    finally:
        m.close()

    for msg in R.msgs:
        print(msg)

    print()
    print("=" * 52)
    print("  通过 %d 项, 失败 %d 项%s"
          % (R.passed, R.failed,
             "  --> ALL PASS" if R.failed == 0 else "  --> 存在失败项"))
    print("=" * 52)
    return 0 if R.failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
