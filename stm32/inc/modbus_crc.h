/**
 * @file    modbus_crc.h
 * @brief   CRC16-Modbus 校验，与硬件无关，可在 PC 上单元测试
 */
#ifndef MODBUS_CRC_H
#define MODBUS_CRC_H

#include <stdint.h>

/**
 * CRC16-Modbus
 *   多项式 0x8005(反射后为 0xA001)，初值 0xFFFF，输入输出均反射，无异或输出。
 *   标准测试向量: {01 03 00 00 00 0A} -> 0xCDC5
 */
uint16_t mb_crc16(const uint8_t *data, uint16_t len);

#endif /* MODBUS_CRC_H */
