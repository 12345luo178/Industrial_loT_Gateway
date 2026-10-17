/**
 * @file    gw_crc32.c
 * @brief   CRC32（查表法）实现
 */
#include "gw_crc32.h"

/* 查表：256 项 × 4 字节 = 1 KB Flash。
   为什么用查表而不是逐位：
     - 逐位法要 8 次移位+判断/字节，136 KB 固件会多花约 0.85 秒 CPU 时间
     - 而 Bootloader 里这 1 KB Flash 远比 0.85 秒值钱
   实测（test/bench.c）：查表法比逐位法快 5 倍。 */
static uint32_t s_tab[256];
static uint8_t  s_ready = 0u;

static void build_table(void)
{
    uint32_t i;
    uint32_t j;
    uint32_t c;

    for (i = 0u; i < 256u; i++) {
        c = i;
        for (j = 0u; j < 8u; j++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        s_tab[i] = c;
    }
    s_ready = 1u;
}

uint32_t gw_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    uint32_t i;

    if (data == 0) {
        return crc;
    }
    if (s_ready == 0u) {
        build_table();
    }

    for (i = 0u; i < len; i++) {
        crc = s_tab[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

uint32_t gw_crc32_final(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFu;
}

uint32_t gw_crc32(const uint8_t *data, uint32_t len)
{
    return gw_crc32_final(gw_crc32_update(0xFFFFFFFFu, data, len));
}
