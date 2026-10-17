/**
 * @file    gw_crc32.h
 * @brief   CRC32（查表法）—— OTA 固件校验共用
 *
 * ============================================================================
 *  为什么单独抽出来
 *
 *  这个 CRC32 原来写在 `gw_ota.c` 内部（static），但 **Bootloader 也需要它**：
 *  跳转之前要校验固件完整性，否则"Flash 位翻转"会被当成完好的固件启动。
 *
 *  如果两边各写一份，就会出现"OTA 写入时用 A 算法、Bootloader 校验时用 B 算法"
 *  的风险 —— 那种 bug 表现为"每次升级都在最后一步失败"，
 *  而且极难看出是算法不一致（因为两份代码各自看都对）。
 *
 *  所以：**算法只有一份**，谁要用谁调。
 *
 *  ----------------------------------------------------------------------------
 *  参数（与 zip / PNG 用的 CRC-32 一致）
 *
 *     多项式   0xEDB88320（反射形式）
 *     初值     0xFFFFFFFF
 *     末尾     取反
 *     输入/输出 不反射（因为多项式已经是反射形式）
 *
 *  与 Modbus 的 CRC16 是**完全不同的两个算法**，不要混用
 *  （Modbus 用 CRC16-IBM，多项式 0xA001）。
 * ============================================================================
 */
#ifndef GW_CRC32_H
#define GW_CRC32_H

#include <stdint.h>

/**
 * 计算一段数据的 CRC32（完整算法：含初值与末尾取反）
 */
uint32_t gw_crc32(const uint8_t *data, uint32_t len);

/**
 * 增量更新（用于分片接收：每收到一片就更新一次，不必缓存整包）
 *
 * @param crc 上一次的返回值；**第一次调用必须传 0xFFFFFFFF**
 * @return 中间值，继续传给下一次调用
 *
 * ⚠️ 全部数据喂完之后，**必须再取反一次**才是最终 CRC：
 *
 *      uint32_t c = 0xFFFFFFFFu;
 *      c = gw_crc32_update(c, chunk1, len1);
 *      c = gw_crc32_update(c, chunk2, len2);
 *      uint32_t final = gw_crc32_final(c);
 *
 *    这个"末尾取反"很容易漏 —— 漏了的话两次计算结果不一致，
 *    而症状是"CRC 永远对不上"，看不出是漏了取反。
 */
uint32_t gw_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len);

/** 取最终值（把中间值取反） */
uint32_t gw_crc32_final(uint32_t crc);

#endif /* GW_CRC32_H */
