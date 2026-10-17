/**
 * @file    gw_flash_stm32.h
 * @brief   STM32F103 内部 Flash 读写擦除（供 gw_param 使用）
 */
#ifndef GW_FLASH_STM32_H
#define GW_FLASH_STM32_H

#include <stdint.h>

#include "gw_param.h"

/**
 * 取 Flash 操作接口（供 gw_param_init 使用）
 *
 * 三个回调的语义：
 *   read       按字节读，地址任意（不必对齐）
 *   write      写之前目标区域必须是已擦除状态（0xFF）。
 *              内部按半字编程，**拒绝把 1 位写成 1**（即只允许 1->0），
 *              否则说明调用方忘了擦除 —— 静默接受会掩盖 bug
 *   erase_slot 擦除第 i 个槽所在的那一页
 *
 * @return 接口指针；始终非空
 */
const gw_flash_if_t *gw_flash_stm32_iface(void);

/**
 * 校验参数区是否可写（读保护、写保护检查）
 *
 * @return 0 = 可写；负值表示不可写（具体原因见实现注释）
 *
 * @note 建议在初始化参数模块之前调用一次。若参数区被写保护，
 *       后面的所有保存都会失败，而失败原因从返回值上看不出来。
 */
int gw_flash_stm32_selfcheck(void);

/** 参数区起始地址（供诊断打印） */
uint32_t gw_flash_param_base(void);

/** 参数槽数量 */
uint8_t gw_flash_param_slot_count(void);

#endif /* GW_FLASH_STM32_H */
