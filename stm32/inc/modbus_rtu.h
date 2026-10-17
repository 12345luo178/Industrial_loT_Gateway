/**
 * @file    modbus_rtu.h
 * @brief   Modbus RTU 从站协议引擎（与硬件完全解耦，可在 PC 上单元测试）
 *
 * 设计要点（高内聚 / 低耦合）：
 *   - 本模块只做"字节进 -> 字节出"，不触碰任何 HAL、中断或寄存器。
 *   - 它不知道数据从哪来（USART + DMA + IDLE 中断）、也不知道往哪去（RS485 收发器），
 *     这些由移植层（port/rs485_port.c）负责调用。
 *   - 结果是：协议逻辑可以在 PC 上完整跑单元测试，不必等硬件就位，
 *     这也是 CI 能自动验证协议正确性的前提。
 */
#ifndef MODBUS_RTU_H
#define MODBUS_RTU_H

#include <stdint.h>

/* ---------------- 功能码 ---------------- */
#define MB_FC_READ_HOLDING      0x03u   /* 读保持寄存器 */
#define MB_FC_READ_INPUT        0x04u   /* 读输入寄存器 */
#define MB_FC_WRITE_SINGLE      0x06u   /* 写单个保持寄存器 */
#define MB_FC_WRITE_MULTIPLE    0x10u   /* 写多个保持寄存器 */

/* ---------------- 异常码 ---------------- */
#define MB_EXC_ILLEGAL_FUNCTION 0x01u   /* 非法功能码 */
#define MB_EXC_ILLEGAL_ADDRESS  0x02u   /* 非法数据地址 */
#define MB_EXC_ILLEGAL_VALUE    0x03u   /* 非法数据值 */
#define MB_EXC_SLAVE_FAILURE    0x04u   /* 从站故障（保留：用于内部错误上报） */

/* ---------------- 规模上限 ---------------- */
/* 寄存器表容量：与 inc/modbus_regs.h 的映射表一一对应，改动必须同步。
   单元测试里的"读满 32 个输入寄存器"与"读 33 个被拒"两条用例
   是这两个常量的契约测试 —— 只要两处不同步，测试立刻失败。 */
/* ⚠️ 原设计是 32 个（0x00~0x1F）。
   2026-10-09 为了放「片内耗时测量区」（36~39）扩到 40 个。
   这是临时扩展：PC 端量到的响应延迟里 USB 驱动占了绝大部分，
   不能当从站性能写报告，所以必须用 DWT 在片内测。
   Modbus 规范上限是 125 个，40 在范围内。
   定位完成后应当收回 32 并把 28~39 还原为保留。 */
#define MB_INPUT_REG_COUNT      40u     /* 输入寄存器个数（IREG_ 0x00~0x27） */
#define MB_HOLDING_REG_COUNT    16u     /* 保持寄存器个数（HREG_ 0x00~0x0F） */

#define MB_MAX_ADU              256u    /* 最大 ADU：站号1 + 功能1 + 数据252 + CRC2 */
#define MB_MAX_READ_QTY         125u    /* 规范上限：一次最多读 125 个寄存器 */
#define MB_MAX_WRITE_QTY        123u    /* 规范上限：一次最多写 123 个寄存器 */

/* ---------------- 从站地址范围 ---------------- */
#define MB_ADDR_BROADCAST       0u      /* 广播地址：执行但不响应 */
#define MB_ADDR_MIN             1u
#define MB_ADDR_MAX             247u

/* ---------------- 寄存器表 ---------------- */
typedef struct {
    uint16_t input[MB_INPUT_REG_COUNT];     /* 功能码 04，只读 */
    uint16_t holding[MB_HOLDING_REG_COUNT]; /* 功能码 03/06/16，读写 */
} mb_regs_t;

/* ---------------- 从站上下文 ---------------- */
typedef struct {
    uint8_t    slave_addr;
    mb_regs_t *regs;

    uint8_t    tx_buf[MB_MAX_ADU];
    uint16_t   tx_len;

    /* 运行统计：既是调试手段，也是简历上"实测数据"的来源。
       这几个计数会被映射到输入寄存器，上位机可直接读取做诊断。 */
    uint32_t   stat_rx_frames;      /* 通过校验并寻址到本机的帧数 */
    uint32_t   stat_crc_errors;     /* CRC 校验失败帧数 */
    uint32_t   stat_exceptions;     /* 发出的异常响应数 */
    uint32_t   stat_addr_miss;      /* 站号不匹配而丢弃的帧数 */
} mb_slave_t;

/**
 * 初始化从站并清零寄存器表
 * @param s     从站上下文（由调用方分配，通常为静态变量）
 * @param addr  本机从站地址 1~247
 * @param regs  寄存器表
 */
void mb_slave_init(mb_slave_t *s, uint8_t addr, mb_regs_t *regs);

/**
 * 处理一帧完整的 Modbus RTU 请求
 * @param s       从站上下文
 * @param req     指向完整请求帧（含末尾 2 字节 CRC）
 * @param req_len 请求帧长度
 * @return 1 = 有响应待发送，响应在 s->tx_buf，长度为 s->tx_len
 *         0 = 无需响应（广播 / 站号不匹配 / CRC 错 / 帧长非法）
 *
 * @note 本函数不负责发送。调用方拿到返回 1 后，须依次：
 *       1) 拉高 RS485 DE 进入发送态
 *       2) 发送 s->tx_buf 共 s->tx_len 字节
 *       3) 等待 USART 的 TC(发送完成) 标志置位
 *       4) 拉低 DE 回到接收态   <-- 少了这一步会截断最后一字节
 */
int mb_slave_process(mb_slave_t *s, const uint8_t *req, uint16_t req_len);

#endif /* MODBUS_RTU_H */
