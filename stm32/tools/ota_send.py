"""把固件按 OTA 帧格式发给板子（ESP32/上云那条 UART 链路）。

============================================================================
 为什么需要它（2026-10-11 第 35 轮）
============================================================================
交接文档 §二·八（第 34 轮补的"OTA 能力边界"）写着：

    "把固件从链路传进槽"（`gw_ota_bridge` 那条路）**从未上板** ——
    目前只有主机测试（`test_ota_bridge` 132 项 + 应用层帧分派 20 项）；
    要真的验一次，需要一个"按 OTA 帧格式把固件发下去"的上位机工具，
    **仓库里没有**。

本工具补上这一环 —— **只在落地之后、板子跑生产版 main 时才有意义**
（当前板上的 L0 版 main 根本没接桥）。

============================================================================
 线格式（全部对着仓库代码核过，不是照记忆写的）
============================================================================
串口帧（`inc/gw_frame.h`）：

    A5 | LEN_L LEN_H | TYPE | PAYLOAD... | CRC8
    LEN = 1 + len(PAYLOAD)（**含 TYPE**，小端）
    CRC8 = poly 0x07 / init 0x00 / 不反射，**覆盖 LEN_L..PAYLOAD**
           （直接复用 tools/e2e_verify.py 的 crc8 —— 它刻意用逐位法，
             与 C 侧的查表法实现不同，属于"方法不同的交叉验证"）

载荷（`src/gw_ota_bridge.c` 的 pack/unpack）：

    OTA_BEGIN (0x05)：u16 major, u16 minor, u32 total_len, u32 crc32   (=12B)
                      crc32 是**整包** CRC32（标准 IEEE，见 src/gw_crc32.c：
                      poly 0xEDB88320 / init 0xFFFFFFFF / 末尾取反 = zlib.crc32）
    OTA_CHUNK (0x06)：u32 offset, data[1..124]      (124 = 128 - 4)
    OTA_END   (0x07)：空载荷
    板子回的 OTA_ACK (0x83)：u8 phase, u8 result, u32 progress         (=6B)
        phase : 1=BEGIN 2=CHUNK 3=END
        result: 0=OK 1=PAYLOAD 2=STATE 3=FLASH 4=BUSY 5=DUPLICATE
                6=CRC 7=NOSPACE
        ⚠️ 4=BUSY 表示**本片还没写**，要稍后重发；5=DUPLICATE 表示已忽略，不算失败。

用法：
    python tools/ota_send.py --bin build/ota/app_b.bin --dry-run      # 只看计划，不发
    python tools/ota_send.py --bin build/ota/app_b.bin --port COM5   # 真发
    python tools/ota_send.py --selftest                              # 自检（无需板子）
"""
import argparse
import io
import os
import struct
import subprocess
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e2e_verify import crc8                     # noqa: E402  逐位 CRC8（交叉验证用）

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(errors="replace")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

HEAD = 0xA5
FT_OTA_BEGIN, FT_OTA_CHUNK, FT_OTA_END = 0x05, 0x06, 0x07
FT_OTA_ACK = 0x83
CHUNK_DATA_MAX = 128 - 4
SLOT_BYTES = 136 * 1024

PHASE_NAME = {1: 'BEGIN', 2: 'CHUNK', 3: 'END'}
RESULT_NAME = {0: 'OK', 1: 'PAYLOAD', 2: 'STATE', 3: 'FLASH', 4: 'BUSY',
               5: 'DUPLICATE', 6: 'CRC', 7: 'NOSPACE'}
RESULT_BUSY, RESULT_DUP, RESULT_OK = 4, 5, 0


def build_frame(ftype, payload):
    """按 gw_frame.h 的格式组帧

    ⚠️ CRC8 覆盖 **LEN_L + LEN_H + TYPE + PAYLOAD** —— 不是只覆盖 TYPE+PAYLOAD。
       第一版我漏了那两个 LEN 字节，是**自检当场抓住的**（判据抄自
       tools/e2e_verify.py 的校验式 `crc8(f[1:-1])`，那份是逐位实现、
       与 C 侧查表法方法不同，属于刻意做的交叉验证）。
    ⚠️⚠️ **LEN 是大端**（高字节在前）。我第一版按小端写 —— 这**只有和 C 侧逐字节
       比对才能发现**（`e2e_verify` 的 CRC 校验与字节序无关，抓不到），
       而它在板子上会导致解析器切不出帧、整条传输必然失败。
       是 `ota_ref_frames.c`（用固件自己的 `gw_frame_build`）把它抓出来的。
    """
    assert len(payload) <= 128, 'payload 超过 GW_FRAME_MAX_PAYLOAD'
    body = bytes([ftype]) + payload
    ln = len(body)
    head = bytes([HEAD, (ln >> 8) & 0xFF, ln & 0xFF])       # ← 大端
    return head + body + bytes([crc8(head[1:] + body)])


def frame_begin(major, minor, total, crc):
    return build_frame(FT_OTA_BEGIN, struct.pack('>HHII', major, minor,
                                                 total, crc))


def frame_chunk(offset, data):
    return build_frame(FT_OTA_CHUNK, struct.pack('>I', offset) + data)


def frame_end():
    return build_frame(FT_OTA_END, b'')


def parse_frames(buf):
    """从字节流里切出完整帧（够用即可：头 + 长度 + CRC 校验）"""
    out = []
    i = 0
    while i + 4 <= len(buf):
        if buf[i] != HEAD:
            i += 1
            continue
        ln = (buf[i + 1] << 8) | buf[i + 2]          # ⚠️ 大端（见 build_frame）
        total = 4 + ln
        if ln < 1 or i + total > len(buf):
            break
        f = buf[i:i + total]
        if crc8(f[1:-1]) == f[-1]:
            out.append(f)
        i += total
    return out


def ack_of(frames):
    """取最后一个 OTA_ACK：(phase, result, progress)"""
    got = None
    for f in frames:
        if f[3] == FT_OTA_ACK and len(f) >= 4 + 6:
            phase, result, progress = struct.unpack('>BBI', f[4:10])
            got = (phase, result, progress)
    return got


def cross_check_c():
    """★ 与 **C 侧真实现**逐字节比对（编译 tools/ota_ref_frames.c 并运行）

    ⚠️ 为什么值得多这一步：本工具是照着文档**另写**的一份上位机实现。
       前面那些结构检查（类型/长度/小端/CRC 覆盖范围）能证明"我按规范写了"，
       但证明不了"我和板子固件讲的是同一种话" —— 而这一步用的是板子固件**自己的**
       `gw_frame_build()` / `gw_ota_pack_begin()` / `gw_ota_pack_chunk()`。
    ⚠️ 没有 C 编译器就**跳过并如实说明**（不假装通过）。
    """
    src = os.path.join(REPO, 'tools', 'ota_ref_frames.c')
    if not os.path.isfile(src):
        print('   [跳过] 找不到 tools/ota_ref_frames.c')
        return 0
    exe = os.path.join(REPO, 'build', '_ota_ref_frames.exe')
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    cmd = ['gcc', '-I' + os.path.join(REPO, 'inc'),
           '-I' + os.path.join(REPO, 'port'), '-I' + os.path.join(REPO, 'app'),
           '-I' + os.path.join(REPO, 'drivers'), '-o', exe, src,
           os.path.join(REPO, 'src', 'gw_frame.c'),
           os.path.join(REPO, 'src', 'gw_ota_bridge.c'),
           # ⚠️ 桥依赖状态机与 CRC32 —— 第一版漏了这两个，链接失败
           os.path.join(REPO, 'src', 'gw_ota.c'),
           os.path.join(REPO, 'src', 'gw_crc32.c')]
    p = subprocess.run(cmd, capture_output=True, text=True,
                       encoding='utf-8', errors='replace')
    if p.returncode != 0:
        print('   [跳过] 编译参考帧程序失败（没装主机 gcc？）：%s'
              % p.stderr.strip().splitlines()[-1][:100] if p.stderr else '')
        return 0

    r = subprocess.run([exe], capture_output=True, text=True,
                       encoding='utf-8', errors='replace')
    ref = {}
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0] in ('BEGIN', 'CHUNK', 'END', 'CHUNK_MAX'):
            ref[parts[0]] = parts[1]

    data = bytes(range(20))
    big = bytes((0xA5 + i) & 0xFF for i in range(CHUNK_DATA_MAX))
    mine = {
        'BEGIN': frame_begin(1, 2, 0x00012345, 0xDEADBEEF).hex(),
        'CHUNK': frame_chunk(0x11223344, data).hex(),
        'END': frame_end().hex(),
        'CHUNK_MAX': frame_chunk(0, big).hex(),
    }
    bad = 0
    if not ref:
        print('   [跳过] 参考帧程序没有输出')
        return 0
    for k in ('BEGIN', 'CHUNK', 'END', 'CHUNK_MAX'):
        if k not in ref:
            print('   [跳过] 参考输出缺 %s' % k)
            continue
        same = (ref[k].lower() == mine[k].lower())
        print('   [%s] 与 C 侧 %s 帧逐字节一致（%d 字节）'
              % (' OK ' if same else 'FAIL', k, len(mine[k]) // 2))
        if not same:
            print('        C 侧: %s' % ref[k][:120])
            print('        我这里: %s' % mine[k][:120])
            bad += 1
    try:
        os.remove(exe)
    except OSError:
        pass
    return 1 if bad else 0


def end2end(bin_path, chunk_max):
    """★ 主机侧端到端：我的帧 -> 真解析器 -> 真桥 -> 假 Flash 重组

    ⚠️ 这一步验的是**语义**（BEGIN 字段、分片偏移与顺序、END、容量检查），
       而不是帧格式（那由 cross_check_c 逐字节保证）。
       不用板子 —— 所以落地之前就能把"上位机与固件讲不讲得通"验掉。
    """
    src = os.path.join(REPO, 'tools', 'ota_ref_end2end.c')
    if not os.path.isfile(src):
        print('  [跳过] 找不到 tools/ota_ref_end2end.c')
        return 0
    exe = os.path.join(REPO, 'build', '_ota_ref_end2end.exe')
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    cmd = ['gcc', '-I' + os.path.join(REPO, 'inc'),
           '-I' + os.path.join(REPO, 'port'), '-I' + os.path.join(REPO, 'app'),
           '-I' + os.path.join(REPO, 'drivers'), '-o', exe, src,
           os.path.join(REPO, 'src', 'gw_frame.c'),
           os.path.join(REPO, 'src', 'gw_ota_bridge.c'),
           os.path.join(REPO, 'src', 'gw_ota.c'),
           os.path.join(REPO, 'src', 'gw_crc32.c')]
    p = subprocess.run(cmd, capture_output=True, text=True,
                       encoding='utf-8', errors='replace')
    if p.returncode != 0:
        last = (p.stderr or '').strip().splitlines()
        print('  [跳过] 编译端到端校验程序失败：%s'
              % (last[-1][:120] if last else ''))
        return 0

    img = io.open(bin_path, 'rb').read()
    crc = zlib.crc32(img) & 0xFFFFFFFF
    stream = frame_begin(0, 1, len(img), crc)
    off = 0
    while off < len(img):
        d = img[off:off + chunk_max]
        stream += frame_chunk(off, d)
        off += len(d)
    stream += frame_end()

    # ⚠️ 必须**按行切好**再喂：C 侧是按行配对的（每行内两字符凑一字节）。
    #    第一版直接把整个 78KB 的 hex 作为一行传过去 -> C 侧 fgets 按 8191 字符
    #    截断 -> 切点落在半个字节上 -> 全错位 -> 解析出 0 帧。
    hex_text = stream.hex()
    wrapped = '\n'.join(hex_text[i:i + 64]
                        for i in range(0, len(hex_text), 64)) + '\n'

    r = subprocess.run([exe], input=wrapped, capture_output=True,
                       text=True, encoding='utf-8', errors='replace')
    lines = [l.strip() for l in r.stdout.splitlines() if l.strip()]
    frames = [l for l in lines if l.startswith('FRAME')]
    slot = next((l for l in lines if l.startswith('SLOT')), '')
    bad = 0

    n_beg = sum(1 for l in frames if l.split()[1] == '0x05')
    n_chk = sum(1 for l in frames if l.split()[1] == '0x06')
    n_end = sum(1 for l in frames if l.split()[1] == '0x07')
    n_err = sum(1 for l in frames if l.split()[2] != '0')
    want_chk = (len(img) + chunk_max - 1) // chunk_max
    for name, ok in (
            ('桥收到 BEGIN 1 帧', n_beg == 1),
            ('桥收到 CHUNK %d 帧（期望 %d）' % (n_chk, want_chk),
             n_chk == want_chk),
            ('桥收到 END 1 帧', n_end == 1),
            ('**没有任何一帧被拒**（result 全 0）', n_err == 0)):
        print('   [%s] %s' % (' OK ' if ok else 'FAIL', name))
        if not ok:
            bad += 1

    parts = slot.split()
    got_bytes = int(parts[1]) if len(parts) >= 3 else -1
    got_crc = int(parts[2], 16) if len(parts) >= 3 else 0
    ok_size = (got_bytes == len(img))
    ok_crc = (got_crc == crc)
    print('   [%s] 假 Flash 重组字节数 %d（源文件 %d）'
          % (' OK ' if ok_size else 'FAIL', got_bytes, len(img)))
    print('   [%s] 重组内容 CRC32 0x%08X（源文件 0x%08X）'
          % (' OK ' if ok_crc else 'FAIL', got_crc, crc))
    if not (ok_size and ok_crc):
        bad += 1
    try:
        os.remove(exe)
    except OSError:
        pass
    return 1 if bad else 0


# ---------------------------------------------------------------- 自检
def selftest():
    print(' 自检（不需要板子）：线格式与仓库代码的一致性')
    bad = 0

    data = bytes(range(20))
    f = frame_begin(1, 2, 0x00012345, 0xDEADBEEF)
    checks = [
        ('帧头 = 0xA5', f[0] == HEAD),
        ('BEGIN 类型 = 0x05', f[3] == FT_OTA_BEGIN),
        ('LEN = 1 + 载荷(12) = 13', ((f[1] << 8) | f[2]) == 13),
        ('整帧长度 = 4 + LEN = 17', len(f) == 17),
        ('载荷按**大端**布局（与 C 侧 put_u16/put_u32 一致）',
         struct.unpack('>HHII', f[4:16]) == (1, 2, 0x00012345, 0xDEADBEEF)),
        ('CRC8 覆盖 LEN_L..PAYLOAD', f[-1] == crc8(f[1:-1])),
    ]
    c = frame_chunk(0x11223344, data)
    checks += [
        ('CHUNK 类型 = 0x06', c[3] == FT_OTA_CHUNK),
        ('CHUNK 偏移大端 = 0x11223344',
         struct.unpack('>I', c[4:8])[0] == 0x11223344),
        ('CHUNK 数据原样', c[8:8 + len(data)] == data),
        ('CHUNK LEN = 1 + 4 + 20 = 25', ((c[1] << 8) | c[2]) == 25),
    ]
    e = frame_end()
    checks += [
        ('END 类型 = 0x07', e[3] == FT_OTA_END),
        ('END LEN = 1（空载荷）', ((e[1] << 8) | e[2]) == 1),
        ('END 整帧 = 5 字节', len(e) == 5),
    ]
    # 整包 CRC32 与板子一致（src/gw_crc32.c = 标准 IEEE 反射式）
    # 已知向量：CRC32("123456789") = 0xCBF43926（业界标准值）
    checks.append(('CRC32 已知向量："123456789" -> 0xCBF43926',
                   zlib.crc32(b'123456789') & 0xFFFFFFFF == 0xCBF43926))
    # 组帧 -> 解帧 往返
    stream = f + c + e
    got = parse_frames(stream)
    checks.append(('组帧/解帧往返：3 帧全解出', len(got) == 3))
    checks.append(('往返后 CRC 仍然有效',
                   all(x[-1] == crc8(x[1:-1]) for x in got)))

    for name, ok in checks:
        print('   [%s] %s' % (' OK ' if ok else 'FAIL', name))
        if not ok:
            bad += 1
    print(' 交叉验证（与 C 侧真实现逐字节比对）：')
    bad += cross_check_c()
    total = len(checks) + 4
    print(' 自检结果：%d/%d 通过（含交叉验证）' % (total - bad, total))
    return 1 if bad else 0


# ---------------------------------------------------------------- 发送
def send(bin_path, port, baud, major, minor, chunk_max, timeout):
    img = io.open(bin_path, 'rb').read()
    if len(img) == 0 or len(img) > SLOT_BYTES:
        print('  [FAIL] 镜像大小 %d 不合理（槽位上限 %d）' % (len(img), SLOT_BYTES))
        return 2
    crc = zlib.crc32(img) & 0xFFFFFFFF
    print(' 发送 %s：%d 字节  CRC32=0x%08X  版本 %d.%d'
          % (bin_path, len(img), crc, major, minor))

    try:
        import serial
    except ImportError:
        print('  [FAIL] 需要 pyserial（pip install pyserial）')
        return 2
    sp = serial.Serial(port, baud, timeout=0.2)
    print(' 串口已打开：%s @ %d' % (port, baud))

    def wait_ack(phase, tmo):
        """等一个对应相位的 ACK；返回 (result, progress)"""
        buf = b''
        deadline = time.time() + tmo
        while time.time() < deadline:
            buf += sp.read(256)
            a = ack_of(parse_frames(buf))
            if a and a[0] == phase:
                return a[1], a[2]
        return None, None

    # BEGIN
    sp.write(frame_begin(major, minor, len(img), crc))
    r, _ = wait_ack(1, timeout)
    if r != RESULT_OK:
        print('  [FAIL] BEGIN 未获 OK（result=%s）—— 常见原因：'
              '容量超分区(7) / 载荷长度(1) / 状态(2)'
              % (RESULT_NAME.get(r, r) if r is not None else '超时'))
        sp.close()
        return 1
    print(' BEGIN ACK OK（板子开始擦除槽位；擦除是延迟执行的）')

    # CHUNK
    off = 0
    n_ok = n_busy = n_dup = 0
    while off < len(img):
        d = img[off:off + chunk_max]
        sp.write(frame_chunk(off, d))
        t0 = time.time()
        while True:
            r, prog = wait_ack(2, max(1.0, timeout))
            if r == RESULT_OK:
                n_ok += 1
                break
            if r == RESULT_DUP:
                n_dup += 1
                break
            if r == RESULT_BUSY and time.time() - t0 < 30.0:
                n_busy += 1
                time.sleep(0.05)            # 背压：稍后重发本片
                sp.write(frame_chunk(off, d))
                continue
            print('  [FAIL] 分片 offset=%d 被拒：result=%s'
                  % (off, RESULT_NAME.get(r, r) if r is not None else '超时'))
            sp.close()
            return 1
        off += len(d)
        if (n_ok % 50) == 0:
            print('   进度 %d/%d 字节（OK=%d BUSY 重发=%d 重复=%d）'
                  % (off, len(img), n_ok, n_busy, n_dup))

    # END
    sp.write(frame_end())
    r, _ = wait_ack(3, max(5.0, timeout))
    if r != RESULT_OK:
        print('  [FAIL] END 未获 OK（result=%s）—— CRC 不符(6) 会走到这里'
              % (RESULT_NAME.get(r, r) if r is not None else '超时'))
        sp.close()
        return 1
    print(' END ACK OK —— 板子随后会**延时重启**进入新固件')
    print(' 统计：分片 OK=%d  BUSY 重发=%d  重复=%d' % (n_ok, n_busy, n_dup))
    print(' ⚠️ 新固件起来后必须在 30 秒内完成"确认"，否则 3 次启动后会回滚')
    sp.close()
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default='')
    ap.add_argument('--port', default='', help='例如 COM5（不填=不发送）')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--major', type=int, default=0)
    ap.add_argument('--minor', type=int, default=1)
    ap.add_argument('--chunk', type=int, default=CHUNK_DATA_MAX)
    ap.add_argument('--timeout', type=float, default=3.0)
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--e2e', action='store_true',
                    help='主机侧端到端：把本工具产生的帧喂给真解析器+真桥（不用板子）')
    a = ap.parse_args()

    if a.selftest:
        return selftest()
    if a.e2e:
        if not a.bin or not os.path.isfile(a.bin):
            print('  [FAIL] --e2e 需要 --bin <固件.bin>')
            return 2
        print(' 主机侧端到端校验（不需要板子）：%s' % a.bin)
        return end2end(a.bin, a.chunk)
    if not a.bin or not os.path.isfile(a.bin):
        print('  [FAIL] 需要 --bin <固件.bin>')
        return 2

    if a.dry_run or not a.port:
        img = io.open(a.bin, 'rb').read()
        crc = zlib.crc32(img) & 0xFFFFFFFF
        n = (len(img) + a.chunk - 1) // a.chunk
        print(' 计划（--dry-run；**没有发送任何字节**）')
        print('   镜像      : %s（%d 字节）' % (a.bin, len(img)))
        print('   整包 CRC32: 0x%08X' % crc)
        print('   分片      : %d 片 × ≤%d 字节' % (n, a.chunk))
        print('   BEGIN 帧  : %s' % frame_begin(a.major, a.minor, len(img),
                                                 crc).hex(' '))
        print('   第 1 片   : %s ...' % frame_chunk(0, img[:a.chunk]).hex(' ')[:80])
        print('   END 帧    : %s' % frame_end().hex(' '))
        if not a.port:
            print(' ⚠️ 没有 --port：只打印计划。真发需要 --port COMx（落地后板子跑生产版 main 才有意义）')
        return 0

    return send(a.bin, a.port, a.baud, a.major, a.minor, a.chunk, a.timeout)


if __name__ == '__main__':
    sys.exit(main())
