/**
 * @file    test_frame.c
 * @brief   串口帧协议单元测试（粘包 / 半包 / 噪声 / 重同步）
 *
 * ============================================================================
 *  这套测试的核心是"字节流切分方式"
 *
 *  协议解析的常规路径（完整帧一次到达）几乎不会出错，
 *  真正会出问题的是**切分边界**：
 *
 *    - 一帧被切成任意多段（模拟串口 DMA 分次到达）
 *    - 多帧粘在一次喂入里（模拟接收任务被阻塞后一次性读到很多）
 *    - 帧前有噪声字节（模拟上电毛刺、波特率切换残留）
 *    - 帧头出现在载荷中间（最阴险的一种：0xA5 恰好是数据）
 *    - 一帧损坏后，后续的帧不能被连带丢掉
 *
 *  最后一条尤其重要：若坏帧处理不当，会表现为
 *  "通信偶尔中断、需要重启才能恢复"——现场极难定位。
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iinc -o test/test_frame.exe \
 *        src/gw_frame.c test/test_frame.c
 *    ./test/test_frame.c
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

#include "gw_frame.h"

static int g_pass = 0;
static int g_fail = 0;

static void ok(const char *name)
{
    g_pass++;
    printf("  [PASS] %s\n", name);
}

static void fail(const char *name, const char *detail)
{
    g_fail++;
    printf("  [FAIL] %s  %s\n", name, detail);
}

static void check_int(const char *name, int actual, int expected)
{
    if (actual == expected) {
        ok(name);
    } else {
        char d[96];
        sprintf(d, "(实际 %d, 期望 %d)", actual, expected);
        fail(name, d);
    }
}

/* 组一帧并返回长度（封装一下，减少测试噪音） */
static uint16_t mk(uint8_t *out, uint8_t type, const uint8_t *pl, uint16_t plen)
{
    uint16_t n = 0u;
    (void)gw_frame_build(out, GW_FRAME_MAX_TOTAL, type, pl, plen, &n);
    return n;
}

/* ==================================================================
 * 测试 1：基本组帧与解析
 * ================================================================== */
static void test_basic(void)
{
    gw_frame_parser_t p;
    uint8_t  frame[GW_FRAME_MAX_TOTAL];
    uint8_t  payload[GW_FRAME_MAX_PAYLOAD];
    uint8_t  type = 0u;
    uint16_t plen = 0u;
    uint16_t n;

    printf("\n[1] 基本组帧与解析\n");

    /* 空载荷 */
    n = mk(frame, GW_FT_LINK_STATUS, 0, 0u);
    check_int("空载荷帧长 = 5 (HEAD+LEN2+TYPE+CRC)", (int)n, 5);
    check_int("帧头正确", (int)frame[0], GW_FRAME_HEAD);
    check_int("长度字段 = 1 (仅 TYPE)", (int)frame[1], 0);
    check_int("长度低字节 = 1", (int)frame[2], 1);
    check_int("类型正确", (int)frame[3], GW_FT_LINK_STATUS);

    gw_frame_parser_init(&p);
    gw_frame_feed(&p, frame, n);
    check_int("解析出 1 帧", (int)gw_frame_take(&p, &type, payload,
                                               sizeof(payload), &plen),
              (int)GW_FRAME_OK);
    check_int("类型一致", (int)type, GW_FT_LINK_STATUS);
    check_int("空载荷长度为 0", (int)plen, 0);
    check_int("再取则没有帧", (int)gw_frame_take(&p, &type, payload,
                                                sizeof(payload), &plen),
              (int)GW_FRAME_ERR_NO_SPACE);

    /* 带载荷 */
    {
        uint8_t pl[4] = { 0x11, 0x22, 0x33, 0x44 };
        n = mk(frame, GW_FT_COMMAND, pl, 4u);
        check_int("带 4 字节载荷的帧长 = 9", (int)n, 9);

        gw_frame_parser_init(&p);
        gw_frame_feed(&p, frame, n);
        if (gw_frame_take(&p, &type, payload, sizeof(payload), &plen) == GW_FRAME_OK) {
            ok("带载荷的帧解析成功");
        } else {
            fail("带载荷的帧解析成功", "");
            return;
        }
        check_int("类型正确", (int)type, GW_FT_COMMAND);
        check_int("载荷长度 = 4", (int)plen, 4);
        check_int("载荷内容一致", memcmp(payload, pl, 4u) == 0 ? 1 : 0, 1);
    }

    /* 大载荷（贴近上限） */
    {
        uint8_t big[GW_FRAME_MAX_PAYLOAD];
        int i;
        for (i = 0; i < (int)sizeof(big); i++) { big[i] = (uint8_t)(i & 0xFF); }
        n = mk(frame, GW_FT_TELEMETRY, big, sizeof(big));
        check_int("最大载荷帧长 = 133", (int)n, 133);

        gw_frame_parser_init(&p);
        gw_frame_feed(&p, frame, n);
        if (gw_frame_take(&p, &type, payload, sizeof(payload), &plen) == GW_FRAME_OK) {
            check_int("最大载荷长度正确", (int)plen, (int)sizeof(big));
            check_int("最大载荷内容一致", memcmp(payload, big, sizeof(big)) == 0 ? 1 : 0, 1);
        } else {
            fail("最大载荷帧解析成功", "");
        }
    }

    /* 超限载荷应被拒 */
    {
        uint8_t too[GW_FRAME_MAX_PAYLOAD + 1];
        uint16_t out = 0u;
        memset(too, 0xAA, sizeof(too));
        if (gw_frame_build(frame, sizeof(frame), GW_FT_TELEMETRY, too,
                           sizeof(too), &out) == GW_FRAME_ERR_TOO_BIG) {
            ok("超过载荷上限被拒");
        } else {
            fail("超过载荷上限被拒", "");
        }
    }

    /* 缓冲不足应被拒 */
    {
        uint8_t small[4];
        uint8_t pl[8] = { 1,2,3,4,5,6,7,8 };
        uint16_t out = 0u;
        if (gw_frame_build(small, sizeof(small), GW_FT_COMMAND, pl, 8u, &out)
            == GW_FRAME_ERR_NO_SPACE) {
            ok("输出缓冲不足被拒");
        } else {
            fail("输出缓冲不足被拒", "");
        }
    }

    /* 空指针 */
    {
        uint16_t out = 0u;
        if (gw_frame_build(0, 10u, GW_FT_ACK, 0, 0u, &out) == GW_FRAME_ERR_PARAM) {
            ok("空输出缓冲被拒");
        } else {
            fail("空输出缓冲被拒", "");
        }
        if (gw_frame_build(frame, sizeof(frame), GW_FT_ACK, 0, 5u, &out)
            == GW_FRAME_ERR_PARAM) {
            ok("有长度但载荷为空指针被拒");
        } else {
            fail("有长度但载荷为空指针被拒", "");
        }
    }
}

/* ==================================================================
 * 测试 2：半包（一帧被切成任意多段）
 * ================================================================== */
static void test_fragmented(void)
{
    int split;

    printf("\n[2] 半包：一帧被切成任意多段喂入\n");

    /* 对每一种切分方式都验证能正确解析 —— 这是串口驱动最常见的场景：
       DMA 一次收多少完全取决于对方发的节奏，不能假设"一次 read 就是一帧"。 */
    for (split = 1; split <= 8; split++) {
        gw_frame_parser_t p;
        uint8_t  frame[GW_FRAME_MAX_TOTAL];
        uint8_t  pl[6] = { 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5 };
        uint8_t  payload[GW_FRAME_MAX_PAYLOAD];
        uint8_t  type = 0u;
        uint16_t plen = 0u;
        uint16_t n;
        uint16_t i;
        int      got = 0;

        n = mk(frame, GW_FT_TELEMETRY, pl, sizeof(pl));

        gw_frame_parser_init(&p);
        /* 每次喂 split 个字节 */
        for (i = 0u; i < n; i = (uint16_t)(i + (uint16_t)split)) {
            uint16_t chunk = (uint16_t)((n - i < (uint16_t)split)
                                        ? (n - i) : (uint16_t)split);
            gw_frame_feed(&p, &frame[i], chunk);
        }

        if (gw_frame_take(&p, &type, payload, sizeof(payload), &plen) == GW_FRAME_OK
            && plen == sizeof(pl) && memcmp(payload, pl, sizeof(pl)) == 0) {
            got = 1;
        }

        if (got) {
            char name[64];
            sprintf(name, "按 %d 字节切分喂入仍能正确解析", split);
            ok(name);
        } else {
            char name[64], d[64];
            sprintf(name, "按 %d 字节切分喂入仍能正确解析", split);
            sprintf(d, "(plen=%u)", (unsigned)plen);
            fail(name, d);
        }
    }

    /* 逐字节喂入（最极端） */
    {
        gw_frame_parser_t p;
        uint8_t  frame[GW_FRAME_MAX_TOTAL];
        uint8_t  pl[3] = { 0x01, 0x02, 0x03 };
        uint8_t  payload[GW_FRAME_MAX_PAYLOAD];
        uint8_t  type = 0u;
        uint16_t plen = 0u;
        uint16_t n = mk(frame, GW_FT_COMMAND, pl, sizeof(pl));
        uint16_t i;

        gw_frame_parser_init(&p);
        for (i = 0u; i < n; i++) {
            gw_frame_feed(&p, &frame[i], 1u);
        }
        if (gw_frame_take(&p, &type, payload, sizeof(payload), &plen) == GW_FRAME_OK
            && plen == 3u) {
            ok("逐字节喂入仍能正确解析（最极端切分）");
        } else {
            fail("逐字节喂入仍能正确解析", "");
        }
    }
}

/* ==================================================================
 * 测试 3：粘包（多帧一次喂入）
 * ================================================================== */
static void test_coalesced(void)
{
    gw_frame_parser_t p;
    uint8_t  stream[GW_FRAME_MAX_TOTAL * 4];
    uint8_t  payload[GW_FRAME_MAX_PAYLOAD];
    uint8_t  type = 0u;
    uint16_t plen = 0u;
    uint16_t total = 0u;
    uint16_t n;
    int      i;

    printf("\n[3] 粘包：多帧粘在一次喂入里\n");

    /* 三帧粘在一起，一次喂入 */
    total = 0u;
    n = mk(&stream[total], GW_FT_WIFI_CONFIG, (const uint8_t *)"ssid", 4u);
    total = (uint16_t)(total + n);
    n = mk(&stream[total], GW_FT_MQTT_CONFIG, (const uint8_t *)"host", 4u);
    total = (uint16_t)(total + n);
    n = mk(&stream[total], GW_FT_TELEMETRY, (const uint8_t *)"abc", 3u);
    total = (uint16_t)(total + n);

    printf("      [诊断] 三帧总长 = %u\n", (unsigned)total);

    gw_frame_parser_init(&p);
    gw_frame_feed(&p, stream, total);

    printf("      [诊断] 喂入后: frames=%lu crc_err=%lu resync=%lu garbage=%lu\n",
           (unsigned long)p.stat.rx_frames, (unsigned long)p.stat.rx_crc_err,
           (unsigned long)p.stat.rx_resync, (unsigned long)p.stat.rx_garbage);

    /* 应能连续取出三帧，顺序与类型都对 */
    {
        const uint8_t expect_type[3] = { GW_FT_WIFI_CONFIG,
                                         GW_FT_MQTT_CONFIG,
                                         GW_FT_TELEMETRY };
        const uint16_t expect_len[3] = { 4u, 4u, 3u };
        int all_ok = 1;

        for (i = 0; i < 3; i++) {
            if (gw_frame_take(&p, &type, payload, sizeof(payload), &plen)
                != GW_FRAME_OK) {
                all_ok = 0;
                break;
            }
            if (type != expect_type[i] || plen != expect_len[i]) {
                all_ok = 0;
                break;
            }
        }
        if (all_ok) {
            ok("三帧粘连时按顺序全部解析正确");
        } else {
            char d[96];
            sprintf(d, "(第 %d 帧: type=0x%02X plen=%u)", i, type, (unsigned)plen);
            fail("三帧粘连时按顺序全部解析正确", d);
        }
        check_int("解析后没有残留帧",
                  (int)gw_frame_take(&p, &type, payload, sizeof(payload), &plen),
                  (int)GW_FRAME_ERR_NO_SPACE);
    }

    check_int("累计解析 3 帧", (int)gw_frame_stats(&p)->rx_frames, 3);
    check_int("粘包不产生 CRC 错误", (int)gw_frame_stats(&p)->rx_crc_err, 0);
}

/* ==================================================================
 * 测试 4：噪声与重同步
 * ================================================================== */
static void test_noise_resync(void)
{
    uint8_t frame[GW_FRAME_MAX_TOTAL];
    uint8_t payload[GW_FRAME_MAX_PAYLOAD];
    uint8_t type = 0u;
    uint16_t plen = 0u;
    uint16_t n;

    printf("\n[4] 噪声与重同步\n");

    /* 帧前有噪声字节 */
    {
        gw_frame_parser_t p;
        uint8_t noise[5] = { 0x00, 0xFF, 0x12, 0x34, 0x56 };
        uint8_t stream[64];
        uint16_t off = 0u;

        n = mk(frame, GW_FT_COMMAND, (const uint8_t *)"hi", 2u);
        memcpy(stream, noise, sizeof(noise));
        off = (uint16_t)sizeof(noise);
        memcpy(&stream[off], frame, n);

        gw_frame_parser_init(&p);
        gw_frame_feed(&p, stream, (uint16_t)(off + n));
        if (gw_frame_take(&p, &type, payload, sizeof(payload), &plen) == GW_FRAME_OK
            && type == GW_FT_COMMAND && plen == 2u) {
            ok("帧前噪声被跳过，帧正确解析");
        } else {
            fail("帧前噪声被跳过", "");
        }
        check_int("噪声字节被计数", (int)gw_frame_stats(&p)->rx_garbage, 5);
    }

    /* 噪声里恰好出现帧头 0xA5（假同步） */
    {
        gw_frame_parser_t p;
        uint8_t stream[64];
        uint16_t off = 0u;

        /* 0xA5 后面跟一个非法长度（0xFFFF），应触发重同步 */
        stream[0] = GW_FRAME_HEAD;
        stream[1] = 0xFF;
        stream[2] = 0xFF;
        off = 3u;
        n = mk(frame, GW_FT_ACK, (const uint8_t *)"ok", 2u);
        memcpy(&stream[off], frame, n);

        gw_frame_parser_init(&p);
        gw_frame_feed(&p, stream, (uint16_t)(off + n));
        if (gw_frame_take(&p, &type, payload, sizeof(payload), &plen) == GW_FRAME_OK
            && type == GW_FT_ACK) {
            ok("噪声中的假帧头（非法长度）被识别并重同步");
        } else {
            fail("噪声中的假帧头被识别并重同步", "");
        }
        if (gw_frame_stats(&p)->rx_len_err >= 1u) {
            ok("非法长度被计数");
        } else {
            fail("非法长度被计数", "");
        }
    }

    /* 帧头出现在载荷中间：必须靠长度字段兜住，不能误判为新帧 */
    {
        gw_frame_parser_t p;
        uint8_t pl[6] = { 0xAA, GW_FRAME_HEAD, 0xBB, GW_FRAME_HEAD, 0xCC, 0xDD };

        n = mk(frame, GW_FT_TELEMETRY, pl, sizeof(pl));
        gw_frame_parser_init(&p);
        gw_frame_feed(&p, frame, n);
        if (gw_frame_take(&p, &type, payload, sizeof(payload), &plen) == GW_FRAME_OK
            && plen == sizeof(pl) && memcmp(payload, pl, sizeof(pl)) == 0) {
            ok("载荷内含 0xA5 时不被误判为帧头");
        } else {
            fail("载荷内含 0xA5 时不被误判为帧头", "");
        }
        check_int("载荷内含帧头不产生重同步",
                  (int)gw_frame_stats(&p)->rx_resync, 0);
    }

    /* 连续两帧之间夹噪声 */
    {
        gw_frame_parser_t p;
        uint8_t stream[64];
        uint16_t off = 0u;

        n = mk(&stream[off], GW_FT_COMMAND, (const uint8_t *)"AA", 2u);
        off = (uint16_t)(off + n);
        stream[off++] = 0x77;       /* 噪声 */
        stream[off++] = 0x88;
        n = mk(&stream[off], GW_FT_ACK, (const uint8_t *)"BB", 2u);
        off = (uint16_t)(off + n);

        gw_frame_parser_init(&p);
        gw_frame_feed(&p, stream, off);
        {
            int frames = 0;
            while (gw_frame_take(&p, &type, payload, sizeof(payload), &plen)
                   == GW_FRAME_OK) {
                frames++;
            }
            check_int("两帧夹噪声时都解析出来", frames, 2);
        }
    }
}

/* ==================================================================
 * 测试 5：坏帧不连带丢帧（最关键的一条）
 * ================================================================== */
static void test_bad_frame_does_not_cascade(void)
{
    uint8_t payload[GW_FRAME_MAX_PAYLOAD];
    uint8_t type = 0u;
    uint16_t plen = 0u;
    uint16_t n;

    printf("\n[5] 坏帧不连带丢帧（现场“通信偶尔中断”的根因）\n");

    /* 场景：好帧 -> CRC 被破坏的帧 -> 好帧。
       期望：第一帧解析成功，第二帧报 CRC 错，**第三帧仍能解析成功**。
       如果实现是"CRC 错就丢弃整个缓冲区"，第三帧会被一起丢掉，
       表现为"每隔几帧丢一帧"或"出错后就再也收不到"。 */
    {
        gw_frame_parser_t p;
        uint8_t stream[64];
        uint16_t off = 0u;
        uint16_t bad_off;
        uint16_t bad_len;
        int      frames = 0;
        int      crc_errs = 0;

        n = mk(&stream[off], GW_FT_COMMAND, (const uint8_t *)"one", 3u);
        off = (uint16_t)(off + n);

        bad_off = off;
        bad_len = mk(&stream[off], GW_FT_COMMAND, (const uint8_t *)"bad", 3u);
        stream[bad_off + bad_len - 1u] ^= 0xFFu;    /* 破坏 CRC */
        off = (uint16_t)(off + bad_len);

        n = mk(&stream[off], GW_FT_TELEMETRY, (const uint8_t *)"two", 3u);
        off = (uint16_t)(off + n);

        gw_frame_parser_init(&p);
        gw_frame_feed(&p, stream, off);

        while (1) {
            gw_frame_err_t e = gw_frame_take(&p, &type, payload,
                                             sizeof(payload), &plen);
            if (e == GW_FRAME_OK) {
                frames++;
            } else if (e == GW_FRAME_ERR_CRC) {
                crc_errs++;
            } else {
                break;
            }
        }

        printf("      [诊断] frames=%d crc_errs=%d | stat: rx=%lu crc=%lu resync=%lu garbage=%lu pending=%u\n",
               frames, crc_errs,
               (unsigned long)p.stat.rx_frames, (unsigned long)p.stat.rx_crc_err,
               (unsigned long)p.stat.rx_resync, (unsigned long)p.stat.rx_garbage,
               (unsigned)p.pending_len);

        check_int("坏帧前后两帧都被解析出来", frames, 2);
        check_int("坏帧产生 1 次 CRC 错误", crc_errs, 1);
        check_int("统计里 CRC 错误计数 = 1",
                  (int)gw_frame_stats(&p)->rx_crc_err, 1);
    }

    /* 更严格的场景：坏帧后面紧跟好帧，中间无间隔 */
    {
        gw_frame_parser_t p;
        uint8_t stream[64];
        uint16_t off = 0u;
        uint16_t bad_off;
        uint16_t bad_len;
        int      good = 0;

        bad_off = off;
        bad_len = mk(&stream[off], GW_FT_COMMAND, (const uint8_t *)"XX", 2u);
        stream[bad_off + 2u] ^= 0xFFu;      /* 破坏长度字段中的一个字节 -> 大概率 CRC 错 */
        off = (uint16_t)(off + bad_len);

        n = mk(&stream[off], GW_FT_ACK, (const uint8_t *)"YY", 2u);
        off = (uint16_t)(off + n);

        gw_frame_parser_init(&p);
        gw_frame_feed(&p, stream, off);
        while (gw_frame_take(&p, &type, payload, sizeof(payload), &plen)
               == GW_FRAME_OK) {
            good++;
        }
        if (good >= 1) {
            ok("坏帧紧邻好帧时，好帧仍被解析出来");
        } else {
            fail("坏帧紧邻好帧时，好帧仍被解析出来", "");
        }
    }

    /* 一长串纯噪声后接好帧 */
    {
        gw_frame_parser_t p;
        uint8_t stream[128];
        int i;

        for (i = 0; i < 50; i++) { stream[i] = (uint8_t)(i + 1); }
        n = mk(&stream[50], GW_FT_COMMAND, (const uint8_t *)"z", 1u);

        gw_frame_parser_init(&p);
        gw_frame_feed(&p, stream, (uint16_t)(50 + n));
        if (gw_frame_take(&p, &type, payload, sizeof(payload), &plen) == GW_FRAME_OK
            && plen == 1u && payload[0] == 'z') {
            ok("50 字节纯噪声后仍能锁定并解析好帧");
        } else {
            fail("纯噪声后仍能解析好帧", "");
        }
    }
}

/* ==================================================================
 * 测试 6：遥测数据结构序列化
 * ================================================================== */
static void test_telemetry(void)
{
    gw_telemetry_t t;
    gw_telemetry_t r;
    uint8_t buf[GW_TELEMETRY_LEN];
    uint8_t frame[GW_FRAME_MAX_TOTAL];
    uint8_t payload[GW_FRAME_MAX_PAYLOAD];
    uint8_t type = 0u;
    uint16_t plen = 0u;
    uint16_t n;

    printf("\n[6] 遥测数据结构序列化\n");

    t.dev_id   = 0x000186A0u;       /* 100000 */
    t.ts       = 0x65000000u;
    t.seq      = 0x00001234u;
    t.temp_x10 = -105;              /* -10.5℃ */
    t.humi_x10 = 612u;
    t.can[0]   = 0x1111u;
    t.can[1]   = 0x2222u;
    t.can[2]   = 0x3333u;
    t.can[3]   = 0x4444u;
    t.status   = 0x01FFu;
    t.uptime_s = 8640u;

    check_int("序列化长度 = 28", (int)gw_telemetry_pack(&t, buf, sizeof(buf)), 28);

    /* 逐字节验证大端序（这是跨芯片的契约，必须钉死） */
    check_int("dev_id 首字节（大端）", (int)buf[0], 0x00);
    check_int("dev_id 末字节", (int)buf[3], 0xA0);
    check_int("温度高字节（负数补码）", (int)buf[12], 0xFF);
    check_int("温度低字节", (int)buf[13], 0x97);        /* -105 = 0xFF97 */
    check_int("湿度高字节", (int)buf[14], 0x02);
    check_int("湿度低字节", (int)buf[15], 0x64);        /* 612 = 0x0264 */
    check_int("CAN1 高字节", (int)buf[16], 0x11);
    check_int("CAN4 低字节", (int)buf[23], 0x44);
    check_int("状态位高字节", (int)buf[24], 0x01);
    check_int("运行时间高字节", (int)buf[26], 0x21);
    check_int("运行时间低字节", (int)buf[27], 0xC0);    /* 8640 = 0x21C0 */

    /* 反序列化应完全还原 */
    if (gw_telemetry_unpack(buf, sizeof(buf), &r) == 0) {
        ok("反序列化成功");
    } else {
        fail("反序列化成功", "");
        return;
    }
    check_int("dev_id 还原", (int)(r.dev_id == t.dev_id), 1);
    check_int("时间戳还原", (int)(r.ts == t.ts), 1);
    check_int("序号还原", (int)(r.seq == t.seq), 1);
    check_int("负温度还原", (int)r.temp_x10, -105);
    check_int("湿度还原", (int)r.humi_x10, 612);
    check_int("CAN1 还原", (int)r.can[0], 0x1111);
    check_int("CAN4 还原", (int)r.can[3], 0x4444);
    check_int("状态位还原", (int)r.status, 0x01FF);
    check_int("运行时间还原", (int)r.uptime_s, 8640);

    /* 端到端：遥测 -> 帧 -> 解析 -> 还原 */
    {
        gw_frame_parser_t p;
        uint16_t pl = gw_telemetry_pack(&t, payload, sizeof(payload));
        n = mk(frame, GW_FT_TELEMETRY, payload, pl);

        gw_frame_parser_init(&p);
        gw_frame_feed(&p, frame, n);
        if (gw_frame_take(&p, &type, payload, sizeof(payload), &plen) == GW_FRAME_OK
            && type == GW_FT_TELEMETRY
            && gw_telemetry_unpack(payload, plen, &r) == 0
            && r.dev_id == t.dev_id && r.temp_x10 == t.temp_x10
            && r.humi_x10 == t.humi_x10 && r.seq == t.seq) {
            ok("端到端：遥测 -> 组帧 -> 解析 -> 还原 完全一致");
        } else {
            fail("端到端遥测链路一致", "");
        }
    }

    /* 入参保护 */
    {
        if (gw_telemetry_pack(0, buf, sizeof(buf)) == 0u &&
            gw_telemetry_pack(&t, 0, sizeof(buf)) == 0u &&
            gw_telemetry_pack(&t, buf, 4u) == 0u) {
            ok("pack 入参保护正确");
        } else {
            fail("pack 入参保护正确", "");
        }
        if (gw_telemetry_unpack(buf, 4u, &r) != 0) {
            ok("unpack 长度不足时返回失败");
        } else {
            fail("unpack 长度不足时返回失败", "");
        }
    }
}

/* ==================================================================
 * 测试 7：统计量语义与类型字符串
 * ================================================================== */
static void test_stats_and_names(void)
{
    gw_frame_parser_t p;
    const gw_frame_stat_t *st;

    printf("\n[7] 统计量与类型字符串\n");

    gw_frame_parser_init(&p);
    st = gw_frame_stats(&p);

    check_int("初始化后收帧计数为 0", (int)st->rx_frames, 0);

    /* 喂 10 字节纯噪声 */
    {
        uint8_t noise[10];
        int i;
        for (i = 0; i < 10; i++) { noise[i] = (uint8_t)(i + 1); }
        gw_frame_feed(&p, noise, 10u);
    }
    check_int("噪声字节被累计", (int)st->rx_garbage, 10);
    check_int("噪声不产生帧", (int)st->rx_frames, 0);
    check_int("喂入字节数被累计", (int)st->rx_bytes, 10);

    /* 正常帧应计入 tx/rx */
    {
        uint8_t frame[GW_FRAME_MAX_TOTAL];
        uint8_t payload[GW_FRAME_MAX_PAYLOAD];
        uint8_t type = 0u;
        uint16_t plen = 0u;
        uint16_t n = mk(frame, GW_FT_ACK, 0, 0u);
        gw_frame_feed(&p, frame, n);
        (void)gw_frame_take(&p, &type, payload, sizeof(payload), &plen);
        check_int("正常帧计入 rx_frames", (int)st->rx_frames, 1);
        check_int("正常帧不产生 resync", (int)st->rx_resync, 0);
    }

    /* 重新初始化会清零统计 */
    gw_frame_parser_init(&p);
    check_int("重新初始化后统计清零", (int)gw_frame_stats(&p)->rx_bytes, 0);

    /* 类型字符串 */
    if (strcmp(gw_frame_type_str(GW_FT_TELEMETRY), "TELEMETRY") == 0 &&
        strcmp(gw_frame_type_str(GW_FT_COMMAND), "COMMAND") == 0 &&
        strcmp(gw_frame_type_str(0xEE), "UNKNOWN") == 0) {
        ok("帧类型字符串正确（含未知类型兜底）");
    } else {
        fail("帧类型字符串正确", "");
    }

    if (strcmp(gw_frame_strerror(GW_FRAME_OK), "OK") == 0 &&
        strstr(gw_frame_strerror(GW_FRAME_ERR_CRC), "CRC") != 0 &&
        gw_frame_strerror((gw_frame_err_t)-999) != 0) {
        ok("错误码字符串可读且有兜底");
    } else {
        fail("错误码字符串可读", "");
    }

    /* 空指针安全 */
    if (gw_frame_stats(0) == 0) {
        ok("统计量查询对空指针安全");
    } else {
        fail("统计量查询对空指针安全", "");
    }
    gw_frame_parser_init(0);
    gw_frame_feed(0, 0, 0u);
    ok("初始化/喂数据对空指针安全（不崩溃）");
}

int main(void)
{
    printf("========== 串口帧协议单元测试 ==========\n");

    test_basic();
    test_fragmented();
    test_coalesced();
    test_noise_resync();
    test_bad_frame_does_not_cascade();
    test_telemetry();
    test_stats_and_names();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
