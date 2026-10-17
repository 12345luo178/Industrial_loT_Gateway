/**
 * @file    gw_ota_port.h
 * @brief   OTA 的 Flash 移植层（把 gw_ota_io_t 接到 STM32 内部 Flash）
 *
 * ============================================================================
 *  ⚠️⚠️ 本文件最关键的一件事：**擦除期间必须喂狗**
 *
 *  IWDG 超时是 1.6 秒（见 `port/FreeRTOSConfig.h`）。
 *  但擦除一个应用分区（136 KB = 68 页 × 2 KB，分区表 v2）需要 **1.4~2.7 秒**。
 *
 *  如果照直擦：
 *
 *      开始擦除 -> 约 2 秒 -> 狗叫 -> 复位 -> 升级失败 -> 又重试 -> 又复位
 *
 *  现场表现是"一升级就重启，永远升不上去"，而且日志里只有一句
 *  "OTA begin"，看不出是狗干的。
 *
 *  ----------------------------------------------------------------------------
 *  本文件的处理：**每擦一页喂一次狗**
 *
 *  `gw_ota_port_erase()` 不是一次 `HAL_FLASHEx_Erase(104 页)`，
 *  而是逐页擦 + 逐页喂狗。这样：
 *
 *    - 狗不会叫（每次擦除 20~40 ms << 3.2 s）
 *    - FreeRTOS 下还能顺便 `vTaskDelay(1)` 让出 CPU，
 *      让 Modbus 从站**在升级期间依然能响应**（这很重要：
 *      上位机在传固件的同时可能还在读寄存器）
 *
 *  ----------------------------------------------------------------------------
 *  另一个坑：写之前必须确认那一页是擦过的
 *
 *  F1 的 Flash 只能把 1 写成 0。如果对一个没擦的地址写，
 *  HAL 会返回错误 —— 但错误码（HAL_ERROR / HAL_TIMEOUT）
 *  分不清"没擦"与"坏了"。
 *  所以真正的诊断要回读：见 `gw_ota_port_verify()`。
 * ============================================================================
 */
#ifndef GW_OTA_PORT_H
#define GW_OTA_PORT_H

#include <stdint.h>

#include "gw_ota.h"

/**
 * 取 OTA 的 Flash 接口（填好回调的 `gw_ota_io_t`）
 *
 * 用法：
 *
 *      gw_ota_bridge_t ob;
 *      (void)gw_ota_bridge_init(&ob, gw_ota_port_io(), GW_OTA_SLOT_B);
 *
 * @return 接口指针（静态存储，永远非空）
 */
const gw_ota_io_t *gw_ota_port_io(void);

/**
 * 擦除统计（诊断用）
 *
 * ⚠️ 上板验证时**必须看这个**：
 *    它记录擦了多少页、喂了多少次狗。
 *    如果 `pages` 与分区页数不符，说明擦除没跑完。
 */
typedef struct {
    uint32_t pages;         /* 累计擦除页数 */
    uint32_t feeds;         /* 累计喂狗次数 */
    uint32_t write_ops;     /* 累计写操作次数（半字为单位） */
    uint32_t verify_fails;  /* 回读校验失败次数 */
    uint32_t erase_fails;   /* 擦除失败次数 */
    /* ⚠️ 元数据"写前比对命中"的次数 = **省下来的整页擦除次数**。
       它是磨损的直接度量：这个数涨说明有调用方在写没变的内容。
       正常情况（gw_ota_boot_persist 生效）它应当是 0。 */
    uint32_t meta_write_skipped;
} gw_ota_port_stat_t;

const gw_ota_port_stat_t *gw_ota_port_stats(void);

/** 清零统计（每次 BEGIN 之前调） */
void gw_ota_port_stats_reset(void);

/**
 * 探测某页是否已擦除（全 0xFF）
 *
 * @param addr 页起始地址（必须页对齐）
 * @return 1 = 已擦除；0 = 有数据；<0 参数非法
 */
int gw_ota_port_page_is_erased(uint32_t addr);

#endif /* GW_OTA_PORT_H */
