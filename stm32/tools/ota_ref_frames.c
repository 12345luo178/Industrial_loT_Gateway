/**
 * @file    ota_ref_frames.c
 * @brief   用**仓库自己的 C 实现**生成 OTA 参考帧（供 tools/ota_send.py 逐字节比对）
 *
 *  为什么要有这个文件：
 *    `ota_send.py` 是照着 `gw_frame.h` / `gw_ota_bridge.c` 的格式**另写**的一份
 *    上位机实现。照着文档写、和"真的和 C 侧一致"是两件事 —— 所以这里用 C 侧
 *    自己的 `gw_frame_build()` + `gw_ota_pack_begin()` / `gw_ota_pack_chunk()`
 *    生成几帧固定输入下的字节流，打印成十六进制；Python 侧编译并运行它，
 *    再和自己算的帧**逐字节比对**。
 *
 *  ⚠️ 这是"用被测实现当标准答案"的**反向**用法：这里比的不是"C 对不对"，
 *     而是"**我的上位机实现和板子固件是不是讲同一种话**"。
 *
 *  编译（主机 gcc，纯逻辑，不碰硬件）：
 *      gcc -Iinc -Iport -Iapp -Idrivers -o ota_ref_frames tools/ota_ref_frames.c \
 *          src/gw_frame.c src/gw_ota_bridge.c
 */
#include <stdio.h>
#include <string.h>

#include "gw_frame.h"
#include "gw_ota_bridge.h"

static void dump(const char *tag, const uint8_t *b, uint16_t n)
{
    uint16_t i;

    printf("%s ", tag);
    for (i = 0u; i < n; i++) {
        printf("%02x", b[i]);
    }
    printf("\n");
}

int main(void)
{
    uint8_t  payload[GW_FRAME_MAX_PAYLOAD];
    uint8_t  frame[GW_FRAME_MAX_TOTAL];
    uint16_t plen;
    uint16_t flen = 0u;
    uint8_t  data[20];
    uint8_t  i;

    for (i = 0u; i < (uint8_t)sizeof(data); i++) {
        data[i] = (uint8_t)i;
    }

    /* 1) BEGIN：major=1 minor=2 total=0x00012345 crc=0xDEADBEEF */
    plen = gw_ota_pack_begin(1u, 2u, 0x00012345u, 0xDEADBEEFu,
                             payload, (uint16_t)sizeof(payload));
    (void)gw_frame_build(frame, GW_FRAME_MAX_TOTAL, GW_FT_OTA_BEGIN,
                         payload, plen, &flen);
    dump("BEGIN", frame, flen);

    /* 2) CHUNK：offset=0x11223344，数据 0..19 */
    plen = gw_ota_pack_chunk(0x11223344u, data, (uint16_t)sizeof(data),
                             payload, (uint16_t)sizeof(payload));
    (void)gw_frame_build(frame, GW_FRAME_MAX_TOTAL, GW_FT_OTA_CHUNK,
                         payload, plen, &flen);
    dump("CHUNK", frame, flen);

    /* 3) END：空载荷 */
    (void)gw_frame_build(frame, GW_FRAME_MAX_TOTAL, GW_FT_OTA_END,
                         0, 0u, &flen);
    dump("END", frame, flen);

    /* 4) 满载荷 CHUNK（124 字节数据），验最大片 */
    {
        uint8_t big[GW_OTA_CHUNK_DATA_MAX];

        for (i = 0u; i < (uint8_t)sizeof(big); i++) {
            big[i] = (uint8_t)(0xA5u + i);
        }
        plen = gw_ota_pack_chunk(0u, big, (uint16_t)sizeof(big),
                                 payload, (uint16_t)sizeof(payload));
        (void)gw_frame_build(frame, GW_FRAME_MAX_TOTAL, GW_FT_OTA_CHUNK,
                             payload, plen, &flen);
        dump("CHUNK_MAX", frame, flen);
    }

    return 0;
}
