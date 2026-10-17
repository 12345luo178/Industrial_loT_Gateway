#!/usr/bin/env python3
"""
Modbus RTU 期望帧参考实现 —— 独立于 C 代码，用于生成单元测试的期望字节。

存在的意义：如果期望值是从 C 实现反推出来的，测试只能证明"代码和它自己一致"，
不能证明"代码符合协议"。所以这里用独立的 Python 实现算一遍。

寄存器映射与 inc/modbus_regs.h 保持同步（输入 32 / 保持 16）。
"""
from typing import List


def crc16(data: bytes) -> int:
    """CRC16-Modbus：多项式 0xA001（反射），初值 0xFFFF，结果低字节先发。"""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc & 0xFFFF


def frame(payload: bytes) -> bytes:
    c = crc16(payload)
    return payload + bytes([c & 0xFF, (c >> 8) & 0xFF])


def hexs(b: bytes) -> str:
    return " ".join(f"{x:02X}" for x in b)


def req_read(slave: int, func: int, start: int, qty: int) -> bytes:
    return frame(bytes([slave, func, start >> 8, start & 0xFF, qty >> 8, qty & 0xFF]))


def resp_read(slave: int, func: int, values: List[int]) -> bytes:
    payload = bytes([slave, func, len(values) * 2])
    for v in values:
        payload += bytes([(v >> 8) & 0xFF, v & 0xFF])
    return frame(payload)


def req_write_single(slave: int, reg: int, val: int) -> bytes:
    return frame(bytes([slave, 0x06, reg >> 8, reg & 0xFF, val >> 8, val & 0xFF]))


def resp_write_single(slave: int, reg: int, val: int) -> bytes:
    return req_write_single(slave, reg, val)   # 06 的响应是原样回显


def req_write_multi(slave: int, start: int, values: List[int]) -> bytes:
    payload = bytes([slave, 0x10, start >> 8, start & 0xFF,
                     len(values) >> 8, len(values) & 0xFF, len(values) * 2])
    for v in values:
        payload += bytes([(v >> 8) & 0xFF, v & 0xFF])
    return frame(payload)


def resp_write_multi(slave: int, start: int, qty: int) -> bytes:
    return frame(bytes([slave, 0x10, start >> 8, start & 0xFF, qty >> 8, qty & 0xFF]))


def resp_exception(slave: int, func: int, exc: int) -> bytes:
    return frame(bytes([slave, func | 0x80, exc]))


# 与 modbus_regs.h 同步的地址常量
IREG_TEMP_X10, IREG_HUMI_X10 = 0, 1
IREG_CAN_CH1 = 2
IREG_DEV_STATUS = 10
IREG_UPTIME_HI, IREG_UPTIME_LO = 11, 12
IREG_MB_RX_LO, IREG_MB_RX_HI = 13, 14
IREG_MQTT_STATUS = 21
IREG_MASTER_ONLINE = 26
IREG_RESERVED_31 = 31

HREG_DEVICE_ID_HI = 0
HREG_BAUDRATE = 2
HREG_PARAM_SAVE_CMD = 12
HREG_REBOOT_CMD = 14

INPUT_REG_COUNT = 32
HOLDING_REG_COUNT = 16

if __name__ == "__main__":
    print("=" * 70)
    print("Modbus RTU 参考帧生成（用于 test_modbus.c 的期望字节）")
    print("=" * 70)

    print("\n[CRC 标准向量]")
    print(f"  CRC(01 03 00 00 00 0A) = 0x{crc16(bytes([0x01,0x03,0x00,0x00,0x00,0x0A])):04X}")

    print("\n[A] 输入寄存器读满 32 个（新增：验证扩展后的上界）")
    print(f"  请求: {hexs(req_read(1, 0x04, 0, INPUT_REG_COUNT))}")
    vals = [(i * 3 + 1) & 0xFFFF for i in range(INPUT_REG_COUNT)]
    print(f"  期望: {hexs(resp_read(1, 0x04, vals))}")

    print("\n[B] 输入寄存器从地址 1 读 32 个（越界，1+32 > 32）")
    print(f"  请求: {hexs(req_read(1, 0x04, 1, INPUT_REG_COUNT))}")
    print(f"  期望: {hexs(resp_exception(1, 0x04, 0x02))}")

    print("\n[C] 读 32 个输入寄存器中的最后一个（地址 31，边界内）")
    print(f"  请求: {hexs(req_read(1, 0x04, IREG_RESERVED_31, 1))}")
    print(f"  期望: {hexs(resp_read(1, 0x04, [0x00A5]))}")

    print("\n[D] 读保持寄存器地址 15 单个（边界内）")
    print(f"  请求: {hexs(req_read(1, 0x03, 15, 1))}")
    print(f"  期望: {hexs(resp_read(1, 0x03, [0x0003]))}")

    print("\n[E] 读保持寄存器地址 16（越界）")
    print(f"  请求: {hexs(req_read(1, 0x03, 16, 1))}")
    print(f"  期望: {hexs(resp_exception(1, 0x03, 0x02))}")

    print("\n[F] 读输入寄存器数量 126（超出规范上限 125）")
    print(f"  请求: {hexs(req_read(1, 0x04, 0, 126))}")
    print(f"  期望: {hexs(resp_exception(1, 0x04, 0x03))}")

    print("\n[G] 写命令魔数 HREG_PARAM_SAVE_CMD (12) = 0x5A5A")
    print(f"  请求: {hexs(req_write_single(1, HREG_PARAM_SAVE_CMD, 0x5A5A))}")
    print(f"  期望: {hexs(resp_write_single(1, HREG_PARAM_SAVE_CMD, 0x5A5A))}")

    print("\n[H] 写多个保持寄存器 12~13（两个命令寄存器）")
    print(f"  请求: {hexs(req_write_multi(1, 12, [0x5A5A, 0x1234]))}")
    print(f"  期望: {hexs(resp_write_multi(1, 12, 2))}")

    print("\n[I] 回归：03 读保持寄存器 2 个（旧用例，地址未变）")
    print(f"  请求: {hexs(req_read(1, 0x03, 0, 2))}")
    print(f"  期望: {hexs(resp_read(1, 0x03, [0x1234, 0x5678]))}")

    print("\n[J] 回归：04 读输入寄存器 1 个（旧用例）")
    print(f"  请求: {hexs(req_read(1, 0x04, 0, 1))}")
    print(f"  期望: {hexs(resp_read(1, 0x04, [0x0102]))}")

    print("\n[K] 回归：16 写多个保持寄存器 2~3")
    print(f"  请求: {hexs(req_write_multi(1, 2, [0x1111, 0x2222]))}")
    print(f"  期望: {hexs(resp_write_multi(1, 2, 2))}")

    print("\n[L] 回归：站号不匹配（合法 CRC，发往 2 但本机是 1）")
    print(f"  请求: {hexs(req_read(2, 0x03, 0, 2))}")

    print("\n[M] 读满 16 个保持寄存器（恰好到上界）")
    print(f"  请求: {hexs(req_read(1, 0x03, 0, HOLDING_REG_COUNT))}")
    print(f"  期望: {hexs(resp_read(1, 0x03, [0] * HOLDING_REG_COUNT))}")

    print("\n[N] 结构体规模自检")
    # ⚠️ 2026-10-11 更正：这里原来写"(设计文档要求 32)"，而 README 的寄存器表
    #    也确实写着 32 个 —— 但代码早就是 40 个了（36~39 是正式的片内耗时测量区，
    #    gw_timing_port_fill() 硬性要求 count >= 40）。文档与代码不一致本身
    #    就是隐患：会让人真的按 32 去裁剪读数量。现在 README 已按真实映射
    #    改为 40 个，这里也跟着改成描述**契约范围**而不是一个过期的数字。
    #    契约：0~27 含义固定；0~31 必须存在（test_regmap 断言"至少 32"）。
    print(f"  MB_INPUT_REG_COUNT   = {INPUT_REG_COUNT}   (契约 0~31 必须存在；28~31=RS485 诊断，32~35=留空，36~39=耗时测量)")
    print(f"  MB_HOLDING_REG_COUNT = {HOLDING_REG_COUNT}   (设计文档要求 16)")
    print(f"  mb_regs_t 数据占用    = {(INPUT_REG_COUNT + HOLDING_REG_COUNT) * 2} 字节")
    print(f"  最长读响应帧 (125 寄存器) = {5 + 125 * 2} 字节  (ADU 上限 256)")
    print("=" * 70)
