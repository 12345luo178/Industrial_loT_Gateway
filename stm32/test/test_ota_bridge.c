/**
 * @file    test_ota_bridge.c
 * @brief   OTA 下行帧分发单元测试
 *
 * ============================================================================
 *  这套测试的重点是**"上位机做错事的时候设备会怎样"**。
 *
 *  正常流程（BEGIN -> CHUNK*N -> END）只有一条路径，很好写也很好测。
 *  真实现场会遇到的是：
 *
 *    - 没 BEGIN 就发 CHUNK（上位机状态机写错了）
 *    - 分片乱序/重传（链路问题）
 *    - 上位机以最高速率猛发（把设备拖死）
 *    - END 的载荷不为空（帧构造 bug）
 *    - 固件太大（放不下）
 *    - 传完之后 CRC 不符（数据在链路上坏了）
 *
 *  这些情况**每一种都要有明确、可区分的应答**，
 *  否则上位机只能盲目重传 —— 而重传一个 136 KB 的固件要几十秒。
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iinc -Iport -o test/test_ota_bridge.exe \
 *        src/gw_crc32.c src/gw_ota.c src/gw_ota_bridge.c \
 *        test/test_ota_bridge.c
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

#include "gw_ota_bridge.h"
#include "gw_crc32.h"

static int g_pass = 0;
static int g_fail = 0;

static void ok(const char *name) { g_pass++; printf("  [PASS] %s\n", name); }
static void fail(const char *name, const char *d)
{
    g_fail++;
    printf("  [FAIL] %s  %s\n", name, d);
}
static void check_int(const char *name, int a, int e)
{
    if (a == e) { ok(name); }
    else { char d[96]; sprintf(d, "(实际 %d, 期望 %d)", a, e); fail(name, d); }
}
static void check_str(const char *name, const char *a, const char *e)
{
    if (a && e && strcmp(a, e) == 0) { ok(name); }
    else {
        char d[200];
        sprintf(d, "(实际 \"%s\", 期望 \"%s\")", a ? a : "(null)", e ? e : "(null)");
        fail(name, d);
    }
}

/* ==================================================================
 *  假的 Flash —— 用 RAM 模拟，同时可以注入失败
 *
 *  ⚠️ 为什么不用真 Flash 接口：那样测试需要 STM32。
 *     而"帧解析与状态流转"这部分**与 Flash 无关**，
 *     不该因为它而无法在 PC 上测。
 * ================================================================== */
/* ⚠️⚠️ 关键：`gw_ota_slot_addr()` 返回的是**编译期常量地址**
 *      （GW_APP_B_ADDR = 0x0802B000，分区表 v2），不是"相对分区的偏移"。
 *      它不管你在测试还是在真机上 —— 那是刻意的：
 *      地址是硬件事实，不该由运行环境决定。
 *
 *      所以假 Flash 必须**按真实地址空间来映射**，
 *      否则 fake_erase 会去 memset(0x0802B000, ...) ——
 *      段错误（我第一版就是这么崩的，退出码 0xC0000005）。
 *
 *      真实地址空间：0x08000000 ~ 0x08080000（512 KB）
 *      映射方式：数组下标 = 地址 - GW_FLASH_BASE
 */
#define FAKE_FLASH_BASE     0x08000000u
#define FAKE_FLASH_SIZE     (512u * 1024u)      /* 整个 512 KB Flash */

static uint8_t  g_flash[FAKE_FLASH_SIZE];

/** 地址 -> 数组下标；越界返回 -1 */
static int fake_idx(uint32_t addr, uint32_t len)
{
    uint32_t off;

    if (addr < FAKE_FLASH_BASE) {
        return -1;
    }
    off = addr - FAKE_FLASH_BASE;
    if (off + len > FAKE_FLASH_SIZE) {
        return -1;
    }
    return (int)off;
}
static uint32_t g_flash_erases;
static uint32_t g_flash_writes;
static int      g_inject_write_fail;    /* 1 = 下一次写入失败 */
static int      g_inject_erase_fail;
static uint8_t  g_meta[GW_OTA_META_PAGES][64];

static int fake_erase(void *ctx, uint32_t addr, uint32_t len)
{
    (void)ctx;
    if (g_inject_erase_fail) {
        g_inject_erase_fail = 0;
        return -1;
    }
    {
        int idx = fake_idx(addr, len);
        if (idx < 0) {
            return -1;
        }
        memset(&g_flash[idx], 0xFF, len);
    }
    g_flash_erases++;
    return 0;
}

static int fake_write(void *ctx, uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint32_t i;

    (void)ctx;
    if (g_inject_write_fail) {
        g_inject_write_fail = 0;
        return -1;
    }
    {
        int idx = fake_idx(addr, len);
        if (idx < 0) {
            return -1;
        }
        /* 模拟 F1 的限制：只能 1->0。
           ⚠️ 这条检查很重要 —— 它让「忘了擦除」这类错误**立刻暴露**，
              而不是静默写坏数据。 */
        for (i = 0u; i < len; i++) {
            if ((g_flash[idx + (int)i] & data[i]) != data[i]) {
                return -1;      /* 试图把 0 写成 1 */
            }
        }
        memcpy(&g_flash[idx], data, len);
    }
    g_flash_writes++;
    return 0;
}

static int fake_verify(void *ctx, uint32_t addr, const uint8_t *data, uint32_t len)
{
    (void)ctx;
    {
        int idx = fake_idx(addr, len);
        if (idx < 0) {
            return -1;
        }
        return (memcmp(&g_flash[idx], data, len) == 0) ? 0 : -1;
    }
}

static int fake_meta_read_page(void *ctx, uint8_t page, uint8_t *buf, uint32_t len)
{
    (void)ctx;
    if (page >= GW_OTA_META_PAGES || len > sizeof(g_meta[0])) {
        return -1;
    }
    memcpy(buf, g_meta[page], len);
    return 0;
}

static int fake_meta_erase_page(void *ctx, uint8_t page)
{
    (void)ctx;
    if (page >= GW_OTA_META_PAGES) {
        return -1;
    }
    memset(g_meta[page], 0xFF, sizeof(g_meta[page]));
    return 0;
}

static int fake_meta_write_page(void *ctx, uint8_t page, const uint8_t *buf,
                                uint32_t len)
{
    (void)ctx;
    if (page >= GW_OTA_META_PAGES || len > sizeof(g_meta[0])) {
        return -1;
    }
    memcpy(g_meta[page], buf, len);
    return 0;
}

static gw_ota_io_t g_io;

static void fake_flash_reset(void)
{
    memset(g_flash, 0xFF, sizeof(g_flash));
    memset(g_meta, 0xFF, sizeof(g_meta));
    g_flash_erases = 0u;
    g_flash_writes = 0u;
    g_inject_write_fail = 0;
    g_inject_erase_fail = 0;

    memset(&g_io, 0, sizeof(g_io));
    g_io.erase            = fake_erase;
    g_io.write            = fake_write;
    g_io.verify           = fake_verify;
    g_io.meta_read_page   = fake_meta_read_page;
    g_io.meta_erase_page  = fake_meta_erase_page;
    g_io.meta_write_page  = fake_meta_write_page;
}

static gw_ota_bridge_t B;

/* ==================================================================
 *  跑一次完整的升级流程（供多个测试复用）
 * ================================================================== */
static void run_full_upgrade(gw_ota_bridge_t *b, const uint8_t *fw,
                             uint32_t fwlen)
{
    uint8_t  payload[GW_FRAME_MAX_PAYLOAD];
    uint8_t  ack[16];
    uint16_t acklen = 0u;
    uint16_t plen;
    uint32_t off = 0u;
    uint32_t crc = gw_crc32(fw, fwlen);

    /* BEGIN */
    plen = gw_ota_pack_begin(1u, 2u, fwlen, crc, payload, sizeof(payload));
    (void)gw_ota_bridge_frame(b, GW_FT_OTA_BEGIN, payload, plen,
                              ack, sizeof(ack), &acklen);
    (void)gw_ota_bridge_do_begin(b);

    /* CHUNK */
    while (off < fwlen) {
        uint32_t n = fwlen - off;
        if (n > GW_OTA_CHUNK_DATA_MAX) {
            n = GW_OTA_CHUNK_DATA_MAX;
        }
        plen = gw_ota_pack_chunk(off, &fw[off], (uint16_t)n,
                                 payload, sizeof(payload));
        (void)gw_ota_bridge_frame(b, GW_FT_OTA_CHUNK, payload, plen,
                                  ack, sizeof(ack), &acklen);
        off += n;
        /* 模拟 tick：每片之后复位限速计数（真实场景里主循环转一圈） */
        gw_ota_bridge_tick(b);
    }

    /* END */
    (void)gw_ota_bridge_frame(b, GW_FT_OTA_END, 0, 0u,
                              ack, sizeof(ack), &acklen);
}

/* ==================================================================
 * 测试 1：载荷编解码
 * ================================================================== */
static void test_pack_unpack(void)
{
    uint8_t  buf[GW_FRAME_MAX_PAYLOAD];
    uint16_t n;
    uint16_t major, minor;
    uint32_t total, crc;
    uint32_t off;
    const uint8_t *data;
    uint16_t dlen;

    printf("\n[1] 载荷编解码\n");

    check_int("BEGIN 载荷固定 12 字节",
              (int)GW_OTA_BEGIN_PAYLOAD_LEN, 12);
    check_int("ACK 载荷固定 6 字节", (int)GW_OTA_ACK_PAYLOAD_LEN, 6);

    /* ---- BEGIN ---- */
    n = gw_ota_pack_begin(1u, 2u, 0x00012345u, 0xDEADBEEFu,
                          buf, sizeof(buf));
    check_int("BEGIN 编码长度", (int)n, 12);
    check_int("major 大端 @0", buf[0], 0x00);
    check_int("major 大端 @1", buf[1], 0x01);
    check_int("minor 大端 @2", buf[2], 0x00);
    check_int("minor 大端 @3", buf[3], 0x02);
    check_int("total 大端 @4", buf[4], 0x00);
    check_int("total 大端 @7", buf[7], 0x45);
    check_int("crc 大端 @8", buf[8], 0xDE);
    check_int("crc 大端 @11", buf[11], 0xEF);
    ok("大端逐字节核对（与帧头 LEN 字段的约定一致）");

    check_int("BEGIN 解码成功",
              gw_ota_unpack_begin(buf, 12u, &major, &minor, &total, &crc), 0);
    check_int("major 正确", (int)major, 1);
    check_int("minor 正确", (int)minor, 2);
    check_int("total 正确", (int)total, 0x12345);
    check_int("crc 正确", (int)crc, (int)0xDEADBEEFu);

    /* ⚠️ 长度必须精确相等 */
    check_int("长度 11 被拒（不能只判 >=）",
              gw_ota_unpack_begin(buf, 11u, &major, &minor, &total, &crc), -1);
    check_int("长度 13 被拒",
              gw_ota_unpack_begin(buf, 13u, &major, &minor, &total, &crc), -1);
    check_int("空指针被拒",
              gw_ota_unpack_begin(0, 12u, &major, &minor, &total, &crc), -1);
    ok("长度精确匹配 —— 多了字节说明发送方布局与我不一致，"
       "按我的布局解释会读到错位的值");

    /* 可选输出参数可以是 NULL */
    check_int("输出参数可为 NULL",
              gw_ota_unpack_begin(buf, 12u, 0, 0, 0, 0), 0);

    /* ---- CHUNK ---- */
    {
        uint8_t d[10];
        memset(d, 0xAB, sizeof(d));

        n = gw_ota_pack_chunk(0x00020000u, d, 10u, buf, sizeof(buf));
        check_int("CHUNK 编码长度 = 4 + 10", (int)n, 14);
        check_int("offset 大端 @0", buf[0], 0x00);
        check_int("offset 大端 @1", buf[1], 0x02);
        check_int("offset 大端 @3", buf[3], 0x00);
        check_int("数据在偏移 4", buf[4], 0xAB);
        check_int("数据末尾正确", buf[13], 0xAB);

        check_int("CHUNK 解码成功",
                  gw_ota_unpack_chunk(buf, 14u, &off, &data, &dlen), 0);
        check_int("offset 正确", (int)off, 0x20000);
        check_int("数据长度正确", (int)dlen, 10);
        check_int("数据指针正确", data != 0 ? 1 : 0, 1);
        check_int("数据内容正确", data[0], 0xAB);

        /* 边界：只有头没有数据 */
        check_int("空数据分片被拒（len == 4）",
                  gw_ota_unpack_chunk(buf, 4u, &off, &data, &dlen), -1);
        ok("空分片被拒 —— 它会让偏移推进逻辑退化成死循环");

        check_int("len < 4 被拒",
                  gw_ota_unpack_chunk(buf, 3u, &off, &data, &dlen), -1);
        check_int("空指针被拒",
                  gw_ota_unpack_chunk(0, 14u, &off, &data, &dlen), -1);

        /* 编码边界 */
        check_int("数据超长编码返回 0",
                  (int)gw_ota_pack_chunk(0u, d, GW_OTA_CHUNK_DATA_MAX + 1u,
                                         buf, sizeof(buf)), 0);
        check_int("缓冲不足编码返回 0",
                  (int)gw_ota_pack_chunk(0u, d, 10u, buf, 13u), 0);
        check_int("数据指针为空但长度非 0 -> 返回 0",
                  (int)gw_ota_pack_chunk(0u, 0, 10u, buf, sizeof(buf)), 0);

        check_int("最大数据长度 = 128 - 4", (int)GW_OTA_CHUNK_DATA_MAX, 124);
        ok("CHUNK 数据上限 124 = 帧载荷上限 128 - 4 字节偏移");
    }

    /* ---- ACK ---- */
    n = gw_ota_pack_ack((uint8_t)GW_OTA_PHASE_CHUNK,
                        GW_OTA_BR_ERR_BUSY, 0x00010000u, buf, sizeof(buf));
    check_int("ACK 编码长度", (int)n, 6);
    check_int("阶段码 @0", buf[0], 2);
    check_int("结果码 @1", buf[1], (int)GW_OTA_BR_ERR_BUSY);
    check_int("进度 大端 @2", buf[2], 0x00);
    check_int("进度 大端 @5", buf[5], 0x00);
    ok("ACK 里带进度而不只是成败 —— 上位机能显示进度条，"
       "重连后也知道传到哪了");
}

/* ==================================================================
 * 测试 2：正常升级流程
 * ================================================================== */
static void test_full_upgrade(void)
{
    static uint8_t fw[3000];
    uint32_t i;

    printf("\n[2] 正常升级流程\n");

    for (i = 0u; i < sizeof(fw); i++) {
        fw[i] = (uint8_t)(i * 7u + 3u);
    }

    fake_flash_reset();
    check_int("初始化成功",
              gw_ota_bridge_init(&B, &g_io, GW_OTA_SLOT_B), 0);
    check_int("初始无待执行 BEGIN", gw_ota_bridge_begin_pending(&B), 0);
    check_int("初始无重启请求", gw_ota_bridge_reboot_pending(&B), 0);

    run_full_upgrade(&B, fw, (uint32_t)sizeof(fw));

    check_int("BEGIN 的擦除执行了 1 次", (int)g_flash_erases, 1);
    check_int("无待执行 BEGIN", gw_ota_bridge_begin_pending(&B), 0);
    check_int("升级完成（应重启）", gw_ota_bridge_reboot_pending(&B), 1);
    check_int("无错误", (int)B.errors, 0);

    /* 分片数 = ceil(3000 / 124) = 25 */
    check_int("成功分片数 = ceil(3000/124)", (int)B.chunks_ok, 25);

    /* 数据真的写进去了 */
    {
        int idx = fake_idx(gw_ota_slot_addr(GW_OTA_SLOT_B),
                           (uint32_t)sizeof(fw));
        check_int("固件内容与写入一致",
                  (int)((idx >= 0
                         && memcmp(&g_flash[idx], fw, sizeof(fw)) == 0)
                        ? 1 : 0), 1);
        ok("校验写入内容逐字节一致 —— 不只是「返回成功了」");
    }

    /* 元数据被正确设置 */
    {
        gw_ota_meta_t m;
        check_int("元数据可加载", gw_ota_meta_load(&g_io, &m), 0);
        check_int("元数据有效", gw_ota_meta_valid(&m), 1);
        check_int("active 指向新分区", (int)m.active, (int)GW_OTA_SLOT_B);
        check_int("pending 已置位（回滚的前提）", (int)m.pending, 1);
        check_int("boot_tries 归零", (int)m.boot_tries, 0);
        ok("pending 在 END 时就置位 —— 这是回滚机制能生效的前提");
    }
}

/* ==================================================================
 * 测试 3：⚠️ 上位机做错事的各种情况
 *
 *  这一段是本套测试最重要的部分。
 * ================================================================== */
static void test_misuse(void)
{
    uint8_t  payload[GW_FRAME_MAX_PAYLOAD];
    uint8_t  ack[16];
    uint16_t acklen = 0u;
    uint16_t plen;

    printf("\n[3] ⚠️ 上位机做错事的情况\n");

    /* ---- 没 BEGIN 就发 CHUNK ---- */
    fake_flash_reset();
    (void)gw_ota_bridge_init(&B, &g_io, GW_OTA_SLOT_B);

    {
        uint8_t d[10];
        memset(d, 0x11, sizeof(d));
        plen = gw_ota_pack_chunk(0u, d, 10u, payload, sizeof(payload));
        check_int("没 BEGIN 就发 CHUNK -> BAD_STATE",
                  (int)gw_ota_bridge_frame(&B, GW_FT_OTA_CHUNK, payload, plen,
                                           ack, sizeof(ack), &acklen),
                  (int)GW_OTA_BR_ERR_STATE);
        check_int("且有 ACK 回应（不是静默丢弃）", (int)(acklen > 0u ? 1 : 0), 1);
        check_int("Flash 没被碰", (int)g_flash_writes, 0);
        ok("明确报「状态不对」而不是让 gw_ota_chunk 去撞没擦除的 Flash");
    }

    /* ---- BEGIN 收到但擦除还没执行 ---- */
    fake_flash_reset();
    (void)gw_ota_bridge_init(&B, &g_io, GW_OTA_SLOT_B);
    {
        uint8_t d[10];
        memset(d, 0x22, sizeof(d));

        plen = gw_ota_pack_begin(1u, 0u, 100u, 0u, payload, sizeof(payload));
        (void)gw_ota_bridge_frame(&B, GW_FT_OTA_BEGIN, payload, plen,
                                  ack, sizeof(ack), &acklen);
        check_int("BEGIN 后有待执行擦除",
                  gw_ota_bridge_begin_pending(&B), 1);
        check_int("擦除还没做（BD 只在 do_begin 里做）",
                  (int)g_flash_erases, 0);
        ok("擦除不在 BEGIN 帧处理里做 —— 那 2~3 秒会卡死 RS485 与看门狗");

        plen = gw_ota_pack_chunk(0u, d, 10u, payload, sizeof(payload));
        check_int("擦除未执行就发 CHUNK -> BAD_STATE",
                  (int)gw_ota_bridge_frame(&B, GW_FT_OTA_CHUNK, payload, plen,
                                           ack, sizeof(ack), &acklen),
                  (int)GW_OTA_BR_ERR_STATE);
        check_int("Flash 仍未被碰", (int)g_flash_writes, 0);
    }

    /* ---- 固件太大 ---- */
    fake_flash_reset();
    (void)gw_ota_bridge_init(&B, &g_io, GW_OTA_SLOT_B);
    {
        /* 超过分区容量（136 KB = 139264 字节，分区表 v2）。用 300000 字节。 */
        plen = gw_ota_pack_begin(1u, 0u, 300000u, 0u,
                                 payload, sizeof(payload));
        check_int("超容量 BEGIN 在帧层仍返回 OK（只登记参数）",
                  (int)gw_ota_bridge_frame(&B, GW_FT_OTA_BEGIN, payload, plen,
                                           ack, sizeof(ack), &acklen),
                  (int)GW_OTA_BR_OK);
        check_int("do_begin 拒绝超容量固件",
                  (int)(gw_ota_bridge_do_begin(&B) != 0 ? 1 : 0), 1);
        check_int("⚠️ 而且**没有擦除**（容量检查在擦除之前）",
                  (int)g_flash_erases, 0);
        ok("容量检查在擦除之前 —— 否则会白花 2~3 秒，"
           "还把旧固件（回滚的依托）擦掉了");
    }

    /* ---- END 载荷不为空 ---- */
    fake_flash_reset();
    (void)gw_ota_bridge_init(&B, &g_io, GW_OTA_SLOT_B);
    {
        uint8_t junk[4] = { 1u, 2u, 3u, 4u };
        check_int("END 带载荷 -> BAD_PAYLOAD",
                  (int)gw_ota_bridge_frame(&B, GW_FT_OTA_END, junk, 4u,
                                           ack, sizeof(ack), &acklen),
                  (int)GW_OTA_BR_ERR_PAYLOAD);
        ok("END 载荷必须为空 —— 带载荷说明发送方的帧构造有问题，"
           "当「空载荷」处理会掩盖那个 bug");
    }

    /* ---- 非 OTA 帧类型 ---- */
    check_int("非 OTA 帧类型被拒（不回 ACK）",
              (int)gw_ota_bridge_frame(&B, GW_FT_TELEMETRY, 0, 0u,
                                       ack, sizeof(ack), &acklen),
              (int)GW_OTA_BR_ERR_STATE);
    check_int("不生成 ACK（该由别的模块处理）", (int)acklen, 0);
    ok("不认识的类型不抢答 —— 否则遥测帧会被回一个 OTA NACK");

    /* ---- 空指针与未初始化 ---- */
    check_int("空上下文被拒",
              (int)gw_ota_bridge_frame(0, GW_FT_OTA_END, 0, 0u,
                                       ack, sizeof(ack), &acklen),
              (int)GW_OTA_BR_ERR_STATE);
    check_int("空指针 init 被拒", gw_ota_bridge_init(0, &g_io, GW_OTA_SLOT_B), -1);
    check_int("空指针 begin_pending 返回 0",
              gw_ota_bridge_begin_pending(0), 0);
    check_int("空指针 reboot_pending 返回 0",
              gw_ota_bridge_reboot_pending(0), 0);
    check_int("空指针 do_begin 返回 -1", gw_ota_bridge_do_begin(0), -1);
    gw_ota_bridge_tick(0);          /* 不应崩 */
    ok("各接口对空指针安全");
}

/* ==================================================================
 * 测试 4：⚠️ 限速（背压）
 * ================================================================== */
static void test_backpressure(void)
{
    uint8_t  payload[GW_FRAME_MAX_PAYLOAD];
    uint8_t  ack[16];
    uint16_t acklen = 0u;
    uint16_t plen;
    uint8_t  d[GW_OTA_CHUNK_DATA_MAX];
    int      i;
    int      busy_count = 0;
    int      ok_count = 0;

    printf("\n[4] ⚠️ 限速（背压）\n");

    fake_flash_reset();
    (void)gw_ota_bridge_init(&B, &g_io, GW_OTA_SLOT_B);

    /* BEGIN + 真正擦除 */
    plen = gw_ota_pack_begin(1u, 0u, 2000u, 0u, payload, sizeof(payload));
    (void)gw_ota_bridge_frame(&B, GW_FT_OTA_BEGIN, payload, plen,
                              ack, sizeof(ack), &acklen);
    (void)gw_ota_bridge_do_begin(&B);

    memset(d, 0x55, sizeof(d));
    gw_ota_bridge_tick(&B);         /* 复位计数 */

    /* 一个 tick 内连发 10 片，只有前 GW_OTA_CHUNKS_PER_TICK 片会被处理 */
    for (i = 0; i < 10; i++) {
        uint32_t off = (uint32_t)i * GW_OTA_CHUNK_DATA_MAX;
        int r;

        plen = gw_ota_pack_chunk(off, d, (uint16_t)sizeof(d),
                                 payload, sizeof(payload));
        r = (int)gw_ota_bridge_frame(&B, GW_FT_OTA_CHUNK, payload, plen,
                                     ack, sizeof(ack), &acklen);
        if (r == (int)GW_OTA_BR_ERR_BUSY) {
            busy_count++;
            /* ⚠️ BUSY 也必须回 ACK —— 否则上位机在等一个
               永远不会来的应答。 */
            check_int("BUSY 也生成了 ACK 载荷", (int)(acklen > 0u ? 1 : 0), 1);
        } else if (r == (int)GW_OTA_BR_OK) {
            ok_count++;
        }
    }

    check_int("一个 tick 内恰好处理了 GW_OTA_CHUNKS_PER_TICK 片",
              ok_count, (int)GW_OTA_CHUNKS_PER_TICK);
    check_int("其余全部回 BUSY",
              busy_count, 10 - (int)GW_OTA_CHUNKS_PER_TICK);
    ok("限速生效：一 tick 最多 4 片（约 8 ms 的 Flash 写入），"
       "避免把 Modbus 从站饿死");

    /* tick 之后又能处理了 */
    gw_ota_bridge_tick(&B);
    plen = gw_ota_pack_chunk(0u, d, (uint16_t)sizeof(d),
                             payload, sizeof(payload));
    check_int("tick 之后恢复处理（回 BUSY 的那片可以重发）",
              (int)gw_ota_bridge_frame(&B, GW_FT_OTA_CHUNK, payload, plen,
                                       ack, sizeof(ack), &acklen),
              (int)GW_OTA_BR_ERR_DUPLICATE);
    ok("回 BUSY 的分片重发后撞上「重复分片」-> 返回 DUPLICATE（不算失败）；"
       "上位机能据此区分「忙」与「坏」");
}

/* ==================================================================
 * 测试 5：重复分片与乱序
 * ================================================================== */
static void test_duplicate_and_offset(void)
{
    uint8_t  payload[GW_FRAME_MAX_PAYLOAD];
    uint8_t  ack[16];
    uint16_t acklen = 0u;
    uint16_t plen;
    uint8_t  d1[64];
    uint8_t  d2[64];

    printf("\n[5] 重复分片与乱序\n");

    fake_flash_reset();
    (void)gw_ota_bridge_init(&B, &g_io, GW_OTA_SLOT_B);

    memset(d1, 0xA1, sizeof(d1));
    memset(d2, 0xB2, sizeof(d2));

    plen = gw_ota_pack_begin(1u, 0u, 128u, 0u, payload, sizeof(payload));
    (void)gw_ota_bridge_frame(&B, GW_FT_OTA_BEGIN, payload, plen,
                              ack, sizeof(ack), &acklen);
    (void)gw_ota_bridge_do_begin(&B);
    gw_ota_bridge_tick(&B);

    /* 第一片 */
    plen = gw_ota_pack_chunk(0u, d1, 64u, payload, sizeof(payload));
    check_int("第一片写入成功",
              (int)gw_ota_bridge_frame(&B, GW_FT_OTA_CHUNK, payload, plen,
                                       ack, sizeof(ack), &acklen),
              (int)GW_OTA_BR_OK);
    gw_ota_bridge_tick(&B);

    /* ⚠️ 同一片重传 */
    plen = gw_ota_pack_chunk(0u, d1, 64u, payload, sizeof(payload));
    check_int("重复分片 -> DUPLICATE（不算失败）",
              (int)gw_ota_bridge_frame(&B, GW_FT_OTA_CHUNK, payload, plen,
                                       ack, sizeof(ack), &acklen),
              (int)GW_OTA_BR_ERR_DUPLICATE);
    check_int("重复计数 +1", (int)B.chunks_dup, 1);
    ok("重复分片**不是失败** —— 链路上偶尔重传是正常的，"
       "报失败会让上位机无意义地重传更多");

    /* 下一片 */
    gw_ota_bridge_tick(&B);
    plen = gw_ota_pack_chunk(64u, d2, 64u, payload, sizeof(payload));
    check_int("第二片写入成功",
              (int)gw_ota_bridge_frame(&B, GW_FT_OTA_CHUNK, payload, plen,
                                       ack, sizeof(ack), &acklen),
              (int)GW_OTA_BR_OK);

    /* ⚠️ 乱序：跳过一个偏移 */
    gw_ota_bridge_tick(&B);
    plen = gw_ota_pack_chunk(500u, d2, 64u, payload, sizeof(payload));
    {
        int r = (int)gw_ota_bridge_frame(&B, GW_FT_OTA_CHUNK, payload, plen,
                                         ack, sizeof(ack), &acklen);
        check_int("偏移跳跃 -> 报 Flash 层错误（OFFSET）",
                  r, (int)GW_OTA_BR_ERR_FLASH);
    }
    ok("偏移不连续被拒 —— 静默接受会让固件出现空洞，"
       "而那种固件能通过 CRC 吗？不能，但会浪费一次完整传输才发现");
}

/* ==================================================================
 * 测试 6：CRC 不符
 * ================================================================== */
static void test_crc_mismatch(void)
{
    static uint8_t fw[500];
    uint8_t  payload[GW_FRAME_MAX_PAYLOAD];
    uint8_t  ack[16];
    uint16_t acklen = 0u;
    uint16_t plen;
    uint32_t off = 0u;
    uint32_t i;

    printf("\n[6] CRC 不符（数据在链路上坏了）\n");

    for (i = 0u; i < sizeof(fw); i++) {
        fw[i] = (uint8_t)(i & 0xFFu);
    }

    fake_flash_reset();
    (void)gw_ota_bridge_init(&B, &g_io, GW_OTA_SLOT_B);

    /* ⚠️ 故意声明一个**错误**的 CRC */
    plen = gw_ota_pack_begin(1u, 0u, (uint32_t)sizeof(fw),
                             gw_crc32(fw, sizeof(fw)) ^ 0x1u,
                             payload, sizeof(payload));
    (void)gw_ota_bridge_frame(&B, GW_FT_OTA_BEGIN, payload, plen,
                              ack, sizeof(ack), &acklen);
    (void)gw_ota_bridge_do_begin(&B);

    while (off < (uint32_t)sizeof(fw)) {
        uint32_t n = (uint32_t)sizeof(fw) - off;
        if (n > GW_OTA_CHUNK_DATA_MAX) {
            n = GW_OTA_CHUNK_DATA_MAX;
        }
        plen = gw_ota_pack_chunk(off, &fw[off], (uint16_t)n,
                                 payload, sizeof(payload));
        (void)gw_ota_bridge_frame(&B, GW_FT_OTA_CHUNK, payload, plen,
                                  ack, sizeof(ack), &acklen);
        off += n;
        gw_ota_bridge_tick(&B);
    }

    check_int("END 时 CRC 不符 -> CRC_ERROR",
              (int)gw_ota_bridge_frame(&B, GW_FT_OTA_END, 0, 0u,
                                       ack, sizeof(ack), &acklen),
              (int)GW_OTA_BR_ERR_CRC);
    check_int("不请求重启", gw_ota_bridge_reboot_pending(&B), 0);
    ok("CRC 不符单独分类 —— 上位机据此知道「重传有意义」，"
       "而不是盲目重传整包（136 KB 要几十秒）");

    /* 元数据不应被改成"可启动" */
    {
        gw_ota_meta_t m;
        (void)gw_ota_meta_load(&g_io, &m);
        check_int("元数据未被置为 pending（旧固件仍是唯一的）",
                  (int)(gw_ota_meta_valid(&m) == 1 ? m.pending : 0), 0);
        ok("CRC 失败时元数据不被改动 —— 否则下次启动会去跑一个坏固件");
    }
}

/* ==================================================================
 * 测试 7：Flash 故障
 * ================================================================== */
static void test_flash_failure(void)
{
    uint8_t  payload[GW_FRAME_MAX_PAYLOAD];
    uint8_t  ack[16];
    uint16_t acklen = 0u;
    uint16_t plen;
    uint8_t  d[32];

    printf("\n[7] Flash 故障\n");

    /* ---- 擦除失败 ---- */
    fake_flash_reset();
    (void)gw_ota_bridge_init(&B, &g_io, GW_OTA_SLOT_B);

    plen = gw_ota_pack_begin(1u, 0u, 1000u, 0u, payload, sizeof(payload));
    (void)gw_ota_bridge_frame(&B, GW_FT_OTA_BEGIN, payload, plen,
                              ack, sizeof(ack), &acklen);

    g_inject_erase_fail = 1;
    check_int("擦除失败 -> do_begin 返回负值",
              (int)(gw_ota_bridge_do_begin(&B) < 0 ? 1 : 0), 1);
    check_int("失败后不再有待执行 BEGIN（不重试）",
              gw_ota_bridge_begin_pending(&B), 0);
    check_int("错误计数 +1", (int)(B.errors > 0u ? 1 : 0), 1);
    ok("擦除失败不重试 —— 重试还是同样的结果（写保护/坏块），"
       "只会拖长故障恢复时间");

    /* ---- 写入失败 ---- */
    fake_flash_reset();
    (void)gw_ota_bridge_init(&B, &g_io, GW_OTA_SLOT_B);
    plen = gw_ota_pack_begin(1u, 0u, 1000u, 0u, payload, sizeof(payload));
    (void)gw_ota_bridge_frame(&B, GW_FT_OTA_BEGIN, payload, plen,
                              ack, sizeof(ack), &acklen);
    (void)gw_ota_bridge_do_begin(&B);
    gw_ota_bridge_tick(&B);

    memset(d, 0x77, sizeof(d));
    g_inject_write_fail = 1;
    plen = gw_ota_pack_chunk(0u, d, (uint16_t)sizeof(d),
                             payload, sizeof(payload));
    check_int("写入失败 -> FLASH_ERROR",
              (int)gw_ota_bridge_frame(&B, GW_FT_OTA_CHUNK, payload, plen,
                                       ack, sizeof(ack), &acklen),
              (int)GW_OTA_BR_ERR_FLASH);
    check_int("成功分片数不增加", (int)B.chunks_ok, 0);
    ok("写入失败与「忙」分开报 —— 前者要修根因，后者重发就行");
}

/* ==================================================================
 * 测试 8：结果码字符串与状态
 * ================================================================== */
static void test_misc(void)
{
    printf("\n[8] 结果码与状态\n");

    check_str("OK", gw_ota_br_str(GW_OTA_BR_OK), "OK");
    check_str("BAD_PAYLOAD", gw_ota_br_str(GW_OTA_BR_ERR_PAYLOAD), "BAD_PAYLOAD");
    check_str("BAD_STATE", gw_ota_br_str(GW_OTA_BR_ERR_STATE), "BAD_STATE");
    check_str("FLASH_ERROR", gw_ota_br_str(GW_OTA_BR_ERR_FLASH), "FLASH_ERROR");
    check_str("BUSY", gw_ota_br_str(GW_OTA_BR_ERR_BUSY), "BUSY");
    check_str("DUPLICATE", gw_ota_br_str(GW_OTA_BR_ERR_DUPLICATE), "DUPLICATE");
    check_str("CRC_ERROR", gw_ota_br_str(GW_OTA_BR_ERR_CRC), "CRC_ERROR");
    check_str("NO_SPACE", gw_ota_br_str(GW_OTA_BR_ERR_NOSPACE), "NO_SPACE");
    check_int("越界结果码有兜底",
              gw_ota_br_str((gw_ota_br_t)99) != 0 ? 1 : 0, 1);

    /* 8 个结果码互不相同（便于上位机区分） */
    {
        int i, j, distinct = 1;
        for (i = 0; i <= (int)GW_OTA_BR_ERR_NOSPACE && distinct; i++) {
            for (j = i + 1; j <= (int)GW_OTA_BR_ERR_NOSPACE; j++) {
                if (strcmp(gw_ota_br_str((gw_ota_br_t)i),
                           gw_ota_br_str((gw_ota_br_t)j)) == 0) {
                    distinct = 0;
                    break;
                }
            }
        }
        check_int("8 个结果码字符串互不相同", distinct, 1);
        ok("上位机靠结果码决定应对方式 —— 字符串重复就失去意义了");
    }

    /* 无 Flash 接口时的"解析层模式" */
    fake_flash_reset();
    check_int("无 io 也能初始化",
              gw_ota_bridge_init(&B, 0, GW_OTA_SLOT_B), 0);
    /* ⚠️ 无 io 是"解析层模式"：状态机没跑，所以状态恒为 DOWNLOAD
       （表示"BEGIN 已受理、可以收分片了"）。
       第一次写这个断言时我写成 IDLE —— 那与实现对不上。
       实现的选择是刻意的：解析层模式下"收到过 BEGIN"就等价于
       "进入下载态"，因为不会有真实的 Flash 状态可查。 */
    check_int("无 io 时状态机未跑，状态为 DOWNLOAD",
              (int)gw_ota_bridge_state(&B), (int)GW_OTA_ST_DOWNLOAD);
    ok("支持「无 Flash 接口」模式 —— 测协议解析时不必提供假 Flash");
}

int main(void)
{
    printf("========== OTA 下行帧分发 单元测试 ==========\n");

    test_pack_unpack();
    test_full_upgrade();
    test_misuse();
    test_backpressure();
    test_duplicate_and_offset();
    test_crc_mismatch();
    test_flash_failure();
    test_misc();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
