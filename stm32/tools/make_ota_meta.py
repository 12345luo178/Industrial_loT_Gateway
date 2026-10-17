#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
造一条 OTA 元数据记录（40 字节，version=2），用于**板上验证引导程序的行为**

============================================================================
 为什么需要它

  引导程序的"试用计数 / 回滚 / 双页交替"这些行为，只有在元数据里
  `pending=1` 时才会发生。而正常途径（真做一次 OTA）需要 ESP32/上云链路。

  这个工具让你**手工造一条记录**直接写进元数据页（用
  `STM32_Programmer_CLI -w <file> 0x08008000`），于是板上可以确定性地复现：

      ① 试用计数递增（每次启动 +1，且**跳转前落盘**）
      ② 试满 GW_OTA_MAX_BOOT_TRIES 次 -> 回滚到另一槽、pending 清零
      ③ 双页 ping-pong：写盘在两页间交替、seq 递增、**任何时刻两页都有效**
      ④ 常态启动零擦写（pending=0 之后不再写）

============================================================================
 用法

    python tools/make_ota_meta.py --out meta.bin --active B --pending 1 \\
                                  --boot-tries 0 --seq 1
    python tools/make_ota_meta.py --decode <读回的 40 字节 hex>

  写进板子（⚠️ CLI 会先擦目标页，这是我们要的）：
    STM32_Programmer_CLI -c port=SWD -w meta.bin 0x08008000 -v

 ⚠️ 不要在**单镜像运行**时手工擦 0x08008000 —— 那个地址属于正在运行的应用
    代码（见交接文档缺陷 L），会把设备擦挂。
"""

import argparse
import struct
import sys
import zlib

try:
    sys.stdout.reconfigure(errors="replace")
except Exception:
    pass

# 与 inc/gw_ota.h 保持一致（改协议时这里要跟着改）
MAGIC = 0x4F544130          # "OTA0"
VERSION = 2                 # 2 = 带 seq 的双页 ping-pong
STATE_VALID = 1
SLOT = {"A": 0, "B": 1, "NONE": 0xFF}
SLOT_NAME = {0: "A", 1: "B", 0xFF: "NONE"}
META_LEN = 40


def layout():
    """字段布局（与 _Static_assert(sizeof(gw_ota_meta_t) == 40) 对应）

    ⚠️ 注意 version 是 u16，后面有 2 字节**结构体对齐填充**（编译器插的，
       不是我们写的字段）—— 造记录时必须留出来，否则 seq 会错位。
    """
    return [
        ("magic", 0, "I"),          # u32
        ("version", 4, "H"),        # u16
        ("_pad0", 6, "2s"),         # 对齐填充
        ("seq", 8, "I"),            # u32
        ("state", 12, "B"),         # u8
        ("active", 13, "B"),        # u8
        ("pending", 14, "B"),       # u8
        ("boot_tries", 15, "B"),    # u8
        ("reserved0", 16, "2s"),
        ("pending_len", 20, "I"),
        ("pending_crc", 24, "I"),
        ("confirmed_len", 28, "I"),
        ("confirmed_crc", 32, "I"),
        ("meta_crc", 36, "I"),
    ]


def build(active, pending, boot_tries, seq, pending_len=0, pending_crc=0,
          confirmed_len=0, confirmed_crc=0):
    buf = bytearray(META_LEN)
    fields = {
        "magic": MAGIC, "version": VERSION, "seq": seq,
        "state": STATE_VALID, "active": SLOT[active], "pending": pending,
        "boot_tries": boot_tries, "pending_len": pending_len,
        "pending_crc": pending_crc, "confirmed_len": confirmed_len,
        "confirmed_crc": confirmed_crc, "meta_crc": 0,
    }
    for name, off, fmt in layout():
        if name in fields:
            if fmt == "2s":
                buf[off:off + 2] = b"\x00\x00"
            else:
                struct.pack_into("<" + fmt, buf, off, fields[name])
    # ==================================================================
    # ⚠️ CRC 覆盖**整 40 字节**（meta_crc 字段先清 0，再连它自己一起算）
    #
    #    固件侧是 `gw_ota_meta_seal()`：把 meta_crc 清 0 后
    #    `crc32_calc((const uint8_t *)m, sizeof(*m))` —— sizeof 是 40，
    #    所以末尾 4 个字节（清 0 的 meta_crc）**也在 CRC 里**。
    #
    #    ⚠️ 第一版这里只算了前 36 字节。工具自己的 decode 也按同一个错约定
    #       校验，于是"自洽但错" —— 看起来一切正常。
    #       只有拿**固件自己的 gw_ota_meta_valid()** 去验才暴露出来
    #       （它返回 0），这就是"判据要先自证"的又一个实例。
    # ==================================================================
    crc = zlib.crc32(bytes(buf)) & 0xFFFFFFFF
    struct.pack_into("<I", buf, 36, crc)
    return bytes(buf)


def decode(data):
    if len(data) < META_LEN:
        return "长度不足（%d < %d）" % (len(data), META_LEN)
    out = []
    vals = {}
    for name, off, fmt in layout():
        if fmt == "2s":
            vals[name] = data[off:off + 2]
        else:
            vals[name] = struct.unpack_from("<" + fmt, data, off)[0]
    chk = bytearray(data[:META_LEN])
    chk[36:40] = b"\x00\x00\x00\x00"      # 与固件一致：meta_crc 清 0 后算整块
    crc_calc = zlib.crc32(bytes(chk)) & 0xFFFFFFFF
    out.append("magic       = 0x%08X %s" % (vals["magic"],
               "('OTA0' OK)" if vals["magic"] == MAGIC else "(!! 不是 OTA0)"))
    out.append("version     = %d %s" % (vals["version"],
               "(OK)" if vals["version"] == VERSION else "(!! 与当前版本不符 -> 判无效)"))
    out.append("seq         = %d" % vals["seq"])
    out.append("state       = %d %s" % (vals["state"],
               "(VALID)" if vals["state"] == STATE_VALID else "(!! 非 VALID)"))
    out.append("active      = %s" % SLOT_NAME.get(vals["active"], vals["active"]))
    out.append("pending     = %d" % vals["pending"])
    out.append("boot_tries  = %d" % vals["boot_tries"])
    out.append("pending_len/crc     = %d / 0x%08X" % (vals["pending_len"], vals["pending_crc"]))
    out.append("confirmed_len/crc   = %d / 0x%08X" % (vals["confirmed_len"], vals["confirmed_crc"]))
    out.append("meta_crc    = 0x%08X  计算值 0x%08X  %s"
               % (vals["meta_crc"], crc_calc,
                  "OK" if vals["meta_crc"] == crc_calc else "!! 不匹配 -> 判无效"))
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description="造/解 OTA 元数据记录（version=2）")
    ap.add_argument("--out", help="输出 40 字节 bin")
    ap.add_argument("--active", default="A", choices=["A", "B"], help="active 槽")
    ap.add_argument("--pending", type=int, default=0, help="1 = 有新固件待确认")
    ap.add_argument("--boot-tries", type=int, default=0, dest="boot_tries")
    ap.add_argument("--seq", type=int, default=1, help="落盘代数（越大越新）")
    ap.add_argument("--pending-len", type=int, default=0, dest="pending_len")
    ap.add_argument("--pending-crc", type=lambda s: int(s, 0), default=0, dest="pending_crc")
    ap.add_argument("--decode", help="解码 40 字节 hex（如从 SWD 读回）")
    args = ap.parse_args()

    if args.decode:
        raw = bytes.fromhex(args.decode.replace(" ", "").replace("\n", ""))
        print(decode(raw))
        return 0

    data = build(args.active, args.pending, args.boot_tries, args.seq,
                 args.pending_len, args.pending_crc)
    print("构造的记录：")
    print(decode(data))
    if args.out:
        with open(args.out, "wb") as f:
            f.write(data)
        print("\n已写出 %s（%d 字节）" % (args.out, len(data)))
        print("写进板子：STM32_Programmer_CLI -c port=SWD -w %s 0x08008000 -v" % args.out)
    print("\nhex: " + data.hex())
    return 0


if __name__ == "__main__":
    sys.exit(main())
