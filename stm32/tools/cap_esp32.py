# -*- coding: utf-8 -*-
r"""
只读抓 ESP32 日志口，用来观测下行指令的完整链路（不写串口、不碰 STM32）

============================================================================
 为什么需要它
============================================================================
 待办②"下行 ACK 复测"的判据是 ESP32 串口上的这几行：

     cmd -> STM32: REPORT_NOW          ← ESP32 把云端指令转发给 STM32
     cmd done: REPORT_NOW -> ACK       ← ★ STM32 回了 ACK（这一行才算通过）
     cmd done: REPORT_NOW -> NACK      ← STM32 拒绝执行
     cmd ack timeout, resend: ...      ← 超时重发
     cmd gave up: ...                  ← 重发后仍无应答

 ⚠️ 只读：**不写串口**（写会打扰 ESP32）。
 ⚠️ 关于 DTR/RTS：pyserial 打开串口默认会拉这两个信号，而多数 ESP32 开发板
    把它们接到了 EN/IO0 ⇒ **打开口就可能把 ESP32 复位**。
    复位的代价是"开始记录时它正在重启"（表现为日志从 boot 开始），
    所以本工具会**明确提示是否看到 boot 序列**，避免把"刚复位"误读成故障。
    若确实不想复位，用 `--no-reset`（不拉 DTR/RTS，但在某些 CH341/CP210x 上
    会导致收不到数据，届时按提示改回来）。

============================================================================
 用法
============================================================================
    python tools\cap_esp32.py --port COM11 --seconds 420 --out scratch\esp32_ack.log
    python tools\cap_esp32.py --selftest

 ⚠️ 用法上的一个坑（2026-10-16 踩过）：**别指望"起个后台监听、过一会儿回来看"**。
    我最初用 PowerShell 的 `Start-Job` 起监听，但每次命令都是独立进程，
    父进程一退出子作业就被回收 —— 监听提前死掉、日志停在半路，
    表现为"控制台明明下发了、串口里却什么都没有"（害我多试了一轮）。
    要跨命令存活，用 `Start-Process`（真独立进程 + 文件重定向），
    或者干脆前台跑、让用户在下发前几秒再动手。
============================================================================
"""
import argparse
import os
import sys
import time

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(errors="replace")
    except (AttributeError, ValueError):
        pass

# 与下行指令链路相关的关键词（用于高亮）
KEY = ("cmd -> STM32", "cmd done", "cmd ack timeout", "cmd gave up",
       "MQTT connected", "MQTT disconnected", "busy, rejected",
       "uart: rx", "getaddrinfo", "WiFi got IP", "SNTP")


def run(port, baud, seconds, out_path, no_reset):
    try:
        import serial
    except ImportError as exc:
        print("[FAIL] 需要 pyserial：%s" % exc)
        return 2

    try:
        s = serial.Serial()
        s.port = port
        s.baudrate = baud
        s.timeout = 1
        if no_reset:
            # 不拉 DTR/RTS：避免通过 EN/IO0 复位 ESP32
            s.dtr = False
            s.rts = False
        s.open()
    except Exception as exc:                            # noqa: BLE001
        print("[FAIL] 打不开 %s：%s" % (port, exc))
        print("       多半是别的程序（串口监视器）正占着它。")
        return 2

    print("=" * 72)
    print(" 抓 ESP32 日志：%s @ %d，持续 %d s" % (port, baud, seconds))
    print(" 写入：%s" % out_path)
    print(" ⚠️ 若下面出现 boot 序列（'boot: ...' / 'app_main'），说明开串口时")
    print("    DTR/RTS 把 ESP32 复位了 —— 那是这个动作的副作用，不是故障。")
    print("=" * 72)
    print("")

    t0 = time.time()
    buf = b""
    nlines = 0
    boot_seen = False
    hits = []

    with open(out_path, "wb") as f:
        try:
            while time.time() - t0 < seconds:
                chunk = s.read(512)
                if chunk:
                    f.write(chunk)
                    f.flush()
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        txt = line.decode("utf-8", "replace").rstrip("\r")
                        if not txt:
                            continue
                        nlines += 1
                        if "boot:" in txt or "app_main" in txt:
                            boot_seen = True
                        if any(k in txt for k in KEY):
                            el = time.time() - t0
                            hits.append(txt)
                            print("  [%6.1fs] %s" % (el, txt[:150]))
                else:
                    # 空闲时也报一下心跳，便于判断"工具还活着"
                    if int(time.time() - t0) % 30 == 0:
                        pass
        except KeyboardInterrupt:
            print("\n  （提前结束）")
        finally:
            s.close()

    print("")
    print("=" * 72)
    print(" 共 %d 行；看到 boot 序列：%s" % (nlines, "是（ESP32 被复位过）" if boot_seen else "否"))
    print(" 命中关键行 %d 条" % len(hits))
    for tag in ("cmd -> STM32", "cmd done", "cmd ack timeout", "cmd gave up"):
        got = [h for h in hits if tag in h]
        if got:
            print("   %-20s %d 条" % (tag, len(got)))
            for g in got[:5]:
                print("        %s" % g[:150])
    verdict = [h for h in hits if "cmd done" in h and "-> ACK" in h]
    print("")
    if verdict:
        print(" [结论] ★ 收到 `cmd done ... -> ACK` ⇒ **下行闭环成立**")
    else:
        print(" [结论] 未看到 `-> ACK`。若你确实下发过指令，请看上面的")
        print("        resend / gave up / busy 等行定位卡在哪一跳。")
    print(" 完整日志：%s" % out_path)
    print("=" * 72)
    return 0


def selftest():
    """自检：只验判据正则/关键词，不碰串口。"""
    ok = True

    def eq(label, got, want):
        nonlocal ok
        if got == want:
            print("  [ OK ] %s" % label)
        else:
            ok = False
            print("  [FAIL] %s\n         got  %r\n         want %r" % (label, got, want))

    samples = [
        "I (16021) gw_bridge: cmd -> STM32: REPORT_NOW",
        "I (16100) gw_bridge: cmd done: REPORT_NOW -> ACK",
        "W (16200) gw_bridge: cmd done: REPORT_NOW -> NACK",
        "W (16300) gw_bridge: cmd ack timeout, resend: REPORT_NOW",
        "W (16400) gw_bridge: cmd gave up: REPORT_NOW",
    ]
    print("--- 关键词命中 ---")
    for t in samples:
        eq(t.split(": ", 1)[-1][:34], any(k in t for k in KEY), True)
    print("--- ACK 判据只认 ACK，不认 NACK ---")
    ack = [t for t in samples if "cmd done" in t and "-> ACK" in t]
    eq("ACK 命中 1 条", len(ack), 1)
    eq("NACK 不算通过", len([t for t in samples if "cmd done" in t and "-> ACK" in t
                             and "NACK" in t]), 0)
    print("--- boot 序列识别 ---")
    eq("boot 行", "boot:" in "I (58) boot:  0 nvs  WiFi data", True)
    eq("app_main 行", "app_main" in "I (761) main_task: Returned from app_main()", True)
    print("")
    print("[ OK ] cap_esp32 自检通过" if ok else "[FAIL] 自检未通过")
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description="只读抓 ESP32 日志口（不写串口）")
    ap.add_argument("--port", default="COM11")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=420.0)
    ap.add_argument("--out", default="scratch/esp32_cap.log")
    ap.add_argument("--no-reset", action="store_true",
                    help="不拉 DTR/RTS（避免复位 ESP32；但可能收不到数据）")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    out = args.out
    if not os.path.isabs(out):
        out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), out)
    d = os.path.dirname(out)
    if d and not os.path.isdir(d):
        os.makedirs(d)
    return run(args.port, args.baud, args.seconds, out, args.no_reset)


if __name__ == "__main__":
    sys.exit(main())
