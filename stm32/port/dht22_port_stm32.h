/**
 * @file    dht22_port_stm32.h
 * @brief   DHT22 在 STM32F103 上的硬件实现接口
 *
 * 引脚：PG11（见 port/gw_config.h）
 * 延时：DWT CYCCNT（见 port/gw_dwt.h），不用 HAL_Delay（毫秒级精度不够）
 */
#ifndef DHT22_PORT_STM32_H
#define DHT22_PORT_STM32_H

#include "dht22_port.h"

/**
 * 初始化 DHT22 的 GPIO 与 DWT 计时
 *
 * 上电先让数据线保持高电平（空闲态）并等 2ms，让传感器完成内部上电复位。
 *
 * @return 0 = 成功；-1 = DWT 不可用（微秒计时无法保证，DHT22 将不可靠）
 */
int  dht22_stm32_init(void);

/** 取硬件操作接口（传给 dht22_read / dht22_read_and_filter） */
const dht22_hw_t *dht22_stm32_hw(void);

/** 便捷封装：采样一次并入滤波器（应用层最常用） */
dht22_err_t dht22_stm32_sample(dht22_filtered_t *f, uint8_t retry);

#endif /* DHT22_PORT_STM32_H */
