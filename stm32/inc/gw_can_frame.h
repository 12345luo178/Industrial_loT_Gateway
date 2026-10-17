/**
 * @file    gw_can_frame.h
 * @brief   CAN 报文的硬件无关表示
 *
 * ============================================================================
 *  为什么不直接用 HAL 的 CanRxHeaderTypeDef？
 *
 *  HAL 的结构体把"标识符"拆成 StdId / ExtId / IDE 三个字段，
 *  又把"帧类型"拆成 RTR / DLC 等 —— 这是硬件寄存器的视角。
 *  而本模块关心的是"这条报文是什么、值是多少"。
 *
 *  中间加一层自己表示之后：
 *    - 纯逻辑模块不依赖 HAL，可以在 PC 上单测
 *    - 换芯片（F4 的 CAN 与 F1 的 bxCAN 寄存器不同）时上层不用改
 *    - 测试里可以直接构造报文，不必去填 HAL 的结构体
 *
 *  代价只是搬运时做一次字段转换（几十纳秒），完全值得。
 * ============================================================================
 */
#ifndef GW_CAN_FRAME_H
#define GW_CAN_FRAME_H

#include <stdint.h>

/* CAN 标准帧 / 扩展帧最大数据长度（经典 CAN，非 FD） */
#define GW_CAN_MAX_DLC      8u

/* 数据长度合法范围 */
#define GW_CAN_DLC_MIN      0u
#define GW_CAN_DLC_MAX      GW_CAN_MAX_DLC

/* 标识符范围 */
#define GW_CAN_STD_ID_MAX   0x7FFu      /* 标准帧 11 位 */
#define GW_CAN_EXT_ID_MAX   0x1FFFFFFFu /* 扩展帧 29 位 */

/** 硬件无关的 CAN 报文 */
typedef struct {
    uint32_t id;        /* 标识符（标准帧只填低 11 位） */
    uint8_t  dlc;       /* 数据长度 0~8 */
    uint8_t  extended;  /* 1 = 扩展帧（29 位 ID）；0 = 标准帧（11 位） */
    uint8_t  rtr;       /* 1 = 远程帧（无数据段） */
    uint8_t  data[GW_CAN_MAX_DLC];
} gw_can_frame_t;

/**
 * 校验报文是否合法（长度与标识符范围）
 * @return 0 合法；负值非法
 */
int gw_can_frame_valid(const gw_can_frame_t *f);

/**
 * 从数据段按**大端**取一个 16 位值
 *
 * @param f      报文
 * @param offset 起始字节（0 或 2 是工业协议里最常见的两个位置）
 * @param out    输出值
 * @return 0 成功；-1 报文为空或 offset+2 超出 dlc
 *
 * @note 为什么固定大端：CAN 应用层协议（CANopen、J1939、多数厂商私有协议）
 *       在大端上是一致的。若某个现场协议是小端，用下面的
 *       gw_can_frame_get_u16_le 即可，不必改核心逻辑。
 */
int gw_can_frame_get_u16(const gw_can_frame_t *f, uint8_t offset, uint16_t *out);

/** 从数据段按小端取 16 位值（部分厂商私有协议用） */
int gw_can_frame_get_u16_le(const gw_can_frame_t *f, uint8_t offset, uint16_t *out);

#endif /* GW_CAN_FRAME_H */
