/**
 * @file    ota_ref_end2end.c
 * @brief   主机侧端到端校验：把 tools/ota_send.py 产生的帧喂给**真的解析器 + 真的桥**
 *
 *  为什么需要它：
 *    `ota_send.py` 是我照文档另写的上位机实现。"帧格式对不对"已经用
 *    `ota_ref_frames.c` 逐字节比对过了；但**语义**（BEGIN 字段、分片偏移与顺序、
 *    END、容量检查）还没验 —— 而这正是最容易写错、又最难在纸上发现的部分。
 *    这里把 Python 产出的字节流喂进板子固件**自己的** `gw_frame_feed()` /
 *    `gw_frame_take()` / `gw_ota_bridge_frame()`，用一块**内存里的假 Flash**
 *    接收数据，最后打印重组结果的字节数与 CRC32 —— Python 侧与源文件比对。
 *
 *  ⚠️ 与 `test_ota_bridge.c` 的区别：那边用 C 造的帧测桥；
 *     这边用**上位机真实产出的字节流**测"上位机与固件的兼容性"。
 *
 *  输入：stdin 的十六进制文本（换行/空格随意，解析器自己处理粘包与半包）
 *  输出（供 Python 逐行断言）：
 *      FRAME <type> <result> <phase> <progress>
 *      SLOT  <bytes> <crc32>
 *      META  <active> <pending> <boot_tries>
 *
 *  编译：由 tools/ota_send.py 的 --e2e 负责（带上 gw_frame.c / gw_ota_bridge.c 等）
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "gw_frame.h"
#include "gw_ota_bridge.h"
#include "gw_crc32.h"

static uint8_t s_flash[GW_APP_SLOT_SIZE];
static uint8_t s_meta[2 * 0x800];

static int fake_erase(void *ctx, uint32_t addr, uint32_t len)
{
    uint32_t off = addr - (uint32_t)(uintptr_t)ctx;

    if (off + len > sizeof(s_flash)) {
        return -1;
    }
    memset(&s_flash[off], 0xFF, len);
    return 0;
}

static int fake_write(void *ctx, uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint32_t off = addr - (uint32_t)(uintptr_t)ctx;

    if (off + len > sizeof(s_flash)) {
        return -1;
    }
    memcpy(&s_flash[off], data, len);
    return 0;
}

static int fake_verify(void *ctx, uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint32_t off = addr - (uint32_t)(uintptr_t)ctx;

    if (off + len > sizeof(s_flash)) {
        return -1;
    }
    return (memcmp(&s_flash[off], data, len) == 0) ? 0 : -1;
}

static int fake_meta_read(void *ctx, uint8_t page, uint8_t *buf, uint32_t len)
{
    (void)ctx;
    if (page > 1u || len > 0x800u) {
        return -1;
    }
    memcpy(buf, &s_meta[page * 0x800u], len);
    return 0;
}

static int fake_meta_erase(void *ctx, uint8_t page)
{
    (void)ctx;
    if (page > 1u) {
        return -1;
    }
    memset(&s_meta[page * 0x800u], 0xFF, 0x800u);
    return 0;
}

static int fake_meta_write(void *ctx, uint8_t page, const uint8_t *buf,
                           uint32_t len)
{
    (void)ctx;
    if (page > 1u || len > 0x800u) {
        return -1;
    }
    memcpy(&s_meta[page * 0x800u], buf, len);
    return 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

/* ⚠️ 必须"喂一点、立刻取空"—— 解析器的接收缓冲只有 GW_FRAME_RX_BUF_SIZE(256) 字节，
 *    把 40+KB 的帧流一次性灌进去会把它冲爆，之后一帧都取不到
 *    （第一版就是这样：只收到 BEGIN 1 帧）。
 *    真实固件也是这个节奏：中断里 feed、任务里 take。 */
static void drain(gw_ota_bridge_t *b, gw_frame_parser_t *p)
{
    uint8_t  payload[GW_FRAME_MAX_PAYLOAD];
    uint8_t  ack[16];
    uint16_t acklen = 0u;
    uint8_t  type = 0u;
    uint16_t plen = 0u;

    while (gw_frame_take(p, &type, payload, sizeof(payload), &plen)
           == GW_FRAME_OK) {
        gw_ota_br_t r = gw_ota_bridge_frame(b, type, payload, plen,
                                            ack, sizeof(ack), &acklen);
        uint8_t  phase = 0u;
        uint32_t prog = 0u;

        if (acklen >= 6u) {
            phase = ack[0];
            prog  = ((uint32_t)ack[2] << 24) | ((uint32_t)ack[3] << 16)
                  | ((uint32_t)ack[4] << 8) | (uint32_t)ack[5];
        }
        printf("FRAME 0x%02X %d %u %u\n", type, (int)r, phase, prog);

        /* BEGIN 之后，固件里由"内务"挑时机执行延迟擦除 —— 这里照做 */
        if (type == GW_FT_OTA_BEGIN && gw_ota_bridge_begin_pending(b)) {
            (void)gw_ota_bridge_do_begin(b);
        }

        /* ⚠️ 还必须模拟固件主循环里的 `gw_ota_bridge_tick()` ——
         *    桥用 `GW_OTA_CHUNKS_PER_TICK` 做**限速**（背压）：
         *    每个 tick 只接受固定片数，超了返回 BUSY(4) 让上位机稍后重发。
         *    第一版桩没调 tick -> 前几片之后就一直是 BUSY ->
         *    假 Flash 里只重组出 496 字节（源文件 39160）。 */
        gw_ota_bridge_tick(b);
    }
}

int main(void)
{
    static gw_ota_bridge_t   b;
    static gw_frame_parser_t p;
    gw_ota_io_t io;
    uint8_t     payload[GW_FRAME_MAX_PAYLOAD];
    uint8_t     ack[16];
    uint16_t    acklen = 0u;
    uint8_t     type = 0u;
    uint16_t    plen = 0u;
    char        line[8192];
    uint8_t     bytes[2048];
    uint32_t    i;

    memset(s_flash, 0xFF, sizeof(s_flash));
    memset(s_meta, 0xFF, sizeof(s_meta));
    memset(&io, 0, sizeof(io));
    io.erase           = fake_erase;
    io.write           = fake_write;
    io.verify          = fake_verify;
    io.meta_read_page  = fake_meta_read;
    io.meta_erase_page = fake_meta_erase;
    io.meta_write_page = fake_meta_write;
    io.ctx             = (void *)(uintptr_t)GW_APP_B_ADDR;   /* 目标槽 B */

    if (gw_ota_bridge_init(&b, &io, GW_OTA_SLOT_B) != 0) {
        printf("INIT FAIL\n");
        return 1;
    }
    gw_frame_parser_init(&p);

    while (fgets(line, (int)sizeof(line), stdin) != NULL) {
        size_t n = 0u;
        const char *q = line;

        while (*q != '\0' && n < sizeof(bytes)) {
            int hi;
            int lo;

            while (*q != '\0' && hexval((unsigned char)*q) < 0) { q++; }
            if (*q == '\0') { break; }
            hi = hexval((unsigned char)*q++);
            while (*q != '\0' && hexval((unsigned char)*q) < 0) { q++; }
            if (*q == '\0') { break; }
            lo = hexval((unsigned char)*q++);
            bytes[n++] = (uint8_t)((hi << 4) | lo);
        }
        if (n > 0u) {
            gw_frame_feed(&p, bytes, (uint16_t)n);
            drain(&b, &p);              /* ⚠️ 每喂一次就取空，别攒 */
        }
    }
    drain(&b, &p);                      /* 收尾再取一次 */

    /* 假 Flash 里被写过的区域：从槽基址起、最后一个非 0xFF 字节的位置 */
    {
        uint32_t used = 0u;

        for (i = 0u; i < sizeof(s_flash); i++) {
            if (s_flash[i] != 0xFFu) {
                used = i + 1u;
            }
        }
        printf("SLOT %u 0x%08X\n", used, gw_crc32(s_flash, used));
    }
    printf("META %u %u %u\n",
           (unsigned)s_meta[13], (unsigned)s_meta[14], (unsigned)s_meta[15]);
    return 0;
}
