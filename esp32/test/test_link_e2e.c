/**
 * @file    test_link_e2e.c
 * @brief   STM32 → ESP32 端到端链路对接测试
 *
 * ============================================================================
 *  为什么需要这个套件（现有 31 个套件都覆盖不到）
 *
 *  现有测试都是"各测各的"：
 *    · test_frame.c         测 gw_frame.c 的编解码与遥测 pack/unpack
 *    · test_esp32_json.c    测 esp32_json_telemetry()，但 payload 是**测试自己造的**
 *    · test_esp32_bridge.c  测桥接状态机，与字节流无关
 *
 *  于是留了一个盲区：**两侧对同一份字节流的理解是否一致**。
 *  典型失效是"STM32 侧改了打包布局，ESP32 侧仍按老布局解析" ——
 *  两边各自的单元测试**都会通过**（因为各用各的构造数据），
 *  只有把 STM32 真发出来的字节喂给 ESP32 才会暴露。
 *  这类 bug 到了现场是"数据全是错的但没有任何报错"，最难查。
 *
 *  本套件把完整链路串起来跑：
 *
 *      gw_telemetry_pack()        ← 模拟 STM32 打包遥测
 *            ↓
 *      gw_frame_build()           ← 模拟 STM32 封帧
 *            ↓
 *      （字节流：中间可能粘包 / 半包 / 夹噪声 / CRC 损坏）
 *            ↓
 *      gw_frame_feed() / take()   ← 模拟 ESP32 收帧
 *            ↓
 *      esp32_json_telemetry()     ← 模拟 ESP32 转 JSON
 *            ↓
 *      逐字段校验 JSON
 *
 *  编译运行（见 doc/ESP32侧固件.md §6）：
 *    gcc -std=c99 -Wall -Wextra -Iinc -Iesp32/include -o t \
 *        src/gw_frame.c esp32/src/esp32_json.c esp32/test/test_link_e2e.c
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "gw_frame.h"
#include "esp32_json.h"
#include "esp32_bridge.h"       /* ESP32_TELEM_JSON_MAX */

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
        char d[120];
        (void)snprintf(d, sizeof(d), "(实际 %d, 期望 %d)", actual, expected);
        fail(name, d);
    }
}

/** 在 JSON 文本里找子串（JSON 是本项目自己生成的，字段顺序稳定，够用） */
static void check_has(const char *name, const char *json, const char *needle)
{
    if (json != 0 && needle != 0 && strstr(json, needle) != 0) {
        ok(name);
    } else {
        char d[400];
        (void)snprintf(d, sizeof(d), "(JSON 里找不到 \"%s\")\n         实际: %s",
                       (needle != 0) ? needle : "(null)",
                       (json != 0) ? json : "(null)");
        fail(name, d);
    }
}

/* ==================================================================
 *  两个"侧"的模拟
 * ================================================================== */

/** 模拟 STM32 侧：遥测结构体 -> 帧字节流 */
static uint16_t stm32_send_telemetry(const gw_telemetry_t *t,
                                     uint8_t *out, uint16_t cap)
{
    uint8_t  payload[GW_FRAME_MAX_PAYLOAD];
    uint16_t plen;
    uint16_t flen = 0u;

    plen = gw_telemetry_pack(t, payload, (uint16_t)sizeof(payload));
    if (plen == 0u) {
        return 0u;
    }
    if (gw_frame_build(out, cap, GW_FT_TELEMETRY, payload, plen, &flen)
        != GW_FRAME_OK) {
        return 0u;
    }
    return flen;
}

/** 模拟 ESP32 侧：从解析器取一帧。返回 1 表示取到 */
static int esp32_recv(gw_frame_parser_t *p, uint8_t *type,
                      uint8_t *payload, uint16_t cap, uint16_t *plen)
{
    return (gw_frame_take(p, type, payload, cap, plen) == GW_FRAME_OK) ? 1 : 0;
}

/** 造一份"典型"遥测（各用例在此基础上改） */
static void make_telemetry(gw_telemetry_t *t)
{
    memset(t, 0, sizeof(*t));
    t->dev_id   = 100000u;
    t->ts       = 12345u;
    t->seq      = 7u;
    t->temp_x10 = 235;          /* 23.5 C */
    t->humi_x10 = 612u;         /* 61.2 % */
    t->can[0]   = 11u;
    t->can[1]   = 22u;
    t->can[2]   = 33u;
    t->can[3]   = 44u;
    t->status   = 0x0003u;
    t->uptime_s = 4321u;
}

/* ==================================================================
 *  用例 1：单帧完整链路 + 逐字段校验
 * ================================================================== */
static void test_single(void)
{
    gw_telemetry_t    t;
    uint8_t           frame[GW_FRAME_MAX_TOTAL];
    uint16_t          flen;
    gw_frame_parser_t p;
    uint8_t           type = 0u;
    uint8_t           payload[GW_FRAME_MAX_PAYLOAD];
    uint16_t          plen = 0u;
    char              json[ESP32_TELEM_JSON_MAX];

    printf("\n--- 单帧完整链路（打包 -> 封帧 -> 解帧 -> JSON）---\n");

    make_telemetry(&t);
    flen = stm32_send_telemetry(&t, frame, (uint16_t)sizeof(frame));

    check_int("STM32 侧封帧成功", (flen > 0u) ? 1 : 0, 1);
    check_int("帧长 = 5 + 28", (int)flen, 5 + (int)GW_TELEMETRY_LEN);

    gw_frame_parser_init(&p);
    gw_frame_feed(&p, frame, flen);

    check_int("ESP32 解析出 1 帧",
              esp32_recv(&p, &type, payload, (uint16_t)sizeof(payload), &plen), 1);
    check_int("帧类型 = TELEMETRY(0x03)", (int)type, (int)GW_FT_TELEMETRY);
    check_int("载荷长度 = 28", (int)plen, (int)GW_TELEMETRY_LEN);
    check_int("没有多余帧",
              esp32_recv(&p, &type, payload, (uint16_t)sizeof(payload), &plen), 0);

    check_int("转 JSON 成功",
              (esp32_json_telemetry(payload, plen, 0u, json,
                                    (uint16_t)sizeof(json)) > 0u) ? 1 : 0, 1);

    check_has("dev_id 跨侧一致", json, "\"dev_id\":100000");
    check_has("ts 跨侧一致",     json, "\"ts\":12345");
    check_has("seq 跨侧一致",    json, "\"seq\":7");
    /* ⚠️ 温度/湿度在 JSON 里是**数字**而不是字符串（`"temp":23.5`）。
       这是 esp32_json.c 的刻意设计：数值字段让云端能直接做运算与比较，
       不必先 parse 字符串。所以下面的断言**不带引号** ——
       顺带也把这个契约钉住（哪天有人改成字符串，这里会立刻红）。 */
    check_has("温度 23.5（JSON 数字）", json, "\"temp\":23.5");
    check_has("湿度 61.2（JSON 数字）", json, "\"humi\":61.2");
    check_has("CAN 四通道",      json, "\"can\":[11,22,33,44]");
    check_has("status 跨侧一致", json, "\"status\":3");
    check_has("uptime 跨侧一致", json, "\"uptime\":4321");
}

/* ==================================================================
 *  用例 2：有符号温度（负值 + 极端值）
 * ================================================================== */
static void test_negative_temp(void)
{
    gw_telemetry_t    t;
    uint8_t           frame[GW_FRAME_MAX_TOTAL];
    uint16_t          flen;
    gw_frame_parser_t p;
    uint8_t           type = 0u;
    uint8_t           payload[GW_FRAME_MAX_PAYLOAD];
    uint16_t          plen = 0u;
    char              json[ESP32_TELEM_JSON_MAX];

    printf("\n--- 有符号温度（补码跨侧一致性）---\n");

    /* -12.3 C。⚠️ 这是最容易被"无符号解析"吃掉的一类值：
       若 ESP32 侧按 uint16 解，-123 会变成 65413 => "6541.3"，
       而两边各自的单元测试都不会发现。 */
    make_telemetry(&t);
    t.temp_x10 = -123;

    flen = stm32_send_telemetry(&t, frame, (uint16_t)sizeof(frame));
    gw_frame_parser_init(&p);
    gw_frame_feed(&p, frame, flen);
    check_int("取到帧", esp32_recv(&p, &type, payload,
                                   (uint16_t)sizeof(payload), &plen), 1);
    check_int("转 JSON", (esp32_json_telemetry(payload, plen, 0u, json,
                                               (uint16_t)sizeof(json)) > 0u) ? 1 : 0, 1);
    check_has("负温度 -12.3（不是 6541.3）", json, "\"temp\":-12.3");

    /* int16 最小值 —— 取绝对值时最容易出 UB 的那个点 */
    make_telemetry(&t);
    t.temp_x10 = (int16_t)(-32768);

    flen = stm32_send_telemetry(&t, frame, (uint16_t)sizeof(frame));
    gw_frame_parser_init(&p);
    gw_frame_feed(&p, frame, flen);
    check_int("取到帧(极端)", esp32_recv(&p, &type, payload,
                                         (uint16_t)sizeof(payload), &plen), 1);
    check_int("转 JSON(极端)", (esp32_json_telemetry(payload, plen, 0u, json,
                                                     (uint16_t)sizeof(json)) > 0u) ? 1 : 0, 1);
    check_has("int16 最小值 -> -3276.8", json, "\"temp\":-3276.8");
}

/* ==================================================================
 *  用例 3：边界值（最大 dev_id / 最大湿度 / 最大 uptime）
 * ================================================================== */
static void test_boundary(void)
{
    gw_telemetry_t    t;
    uint8_t           frame[GW_FRAME_MAX_TOTAL];
    uint16_t          flen;
    gw_frame_parser_t p;
    uint8_t           type = 0u;
    uint8_t           payload[GW_FRAME_MAX_PAYLOAD];
    uint16_t          plen = 0u;
    char              json[ESP32_TELEM_JSON_MAX];

    printf("\n--- 边界值 ---\n");

    make_telemetry(&t);
    t.dev_id   = 0xFFFFFFFFu;   /* uint32 最大 */
    t.humi_x10 = 65535u;        /* uint16 最大 => 6553.5 % */
    t.uptime_s = 0xFFFFu;

    flen = stm32_send_telemetry(&t, frame, (uint16_t)sizeof(frame));
    gw_frame_parser_init(&p);
    gw_frame_feed(&p, frame, flen);
    check_int("取到帧(边界)", esp32_recv(&p, &type, payload,
                                         (uint16_t)sizeof(payload), &plen), 1);
    check_int("转 JSON(边界)", (esp32_json_telemetry(payload, plen, 0u, json,
                                                     (uint16_t)sizeof(json)) > 0u) ? 1 : 0, 1);

    check_has("dev_id = 4294967295", json, "\"dev_id\":4294967295");
    check_has("湿度 6553.5",         json, "\"humi\":6553.5");
    check_has("uptime = 65535",      json, "\"uptime\":65535");
}

/* ==================================================================
 *  用例 4：粘包（一次 feed 多帧）
 * ================================================================== */
static void test_coalesced(void)
{
    uint8_t           buf[GW_FRAME_MAX_TOTAL * 3u];
    uint16_t          off = 0u;
    gw_frame_parser_t p;
    uint8_t           type = 0u;
    uint8_t           payload[GW_FRAME_MAX_PAYLOAD];
    uint16_t          plen = 0u;
    int               got = 0;
    int               i;

    printf("\n--- 粘包：一次喂 3 帧 ---\n");

    for (i = 0; i < 3; i++) {
        gw_telemetry_t t;
        uint16_t       flen;

        make_telemetry(&t);
        t.seq      = (uint32_t)i;
        t.temp_x10 = (int16_t)(200 + i);

        flen = stm32_send_telemetry(&t, buf + off,
                                    (uint16_t)(sizeof(buf) - off));
        if (flen == 0u) {
            break;
        }
        off = (uint16_t)(off + flen);
    }
    check_int("STM32 侧连发 3 帧", (off > 0u) ? 1 : 0, 1);

    /* 一次全喂进去 —— UART 阻塞读一次读到多帧就是这个场景 */
    gw_frame_parser_init(&p);
    gw_frame_feed(&p, buf, off);

    while (esp32_recv(&p, &type, payload, (uint16_t)sizeof(payload), &plen)) {
        got++;
    }
    check_int("取出 3 帧（一帧都不能少）", got, 3);
}

/* ==================================================================
 *  用例 5：半包（逐字节喂）
 * ================================================================== */
static void test_split(void)
{
    gw_telemetry_t    t;
    uint8_t           frame[GW_FRAME_MAX_TOTAL];
    uint16_t          flen;
    gw_frame_parser_t p;
    uint8_t           type = 0u;
    uint8_t           payload[GW_FRAME_MAX_PAYLOAD];
    uint16_t          plen = 0u;
    uint16_t          i;

    printf("\n--- 半包：逐字节喂 ---\n");

    make_telemetry(&t);
    flen = stm32_send_telemetry(&t, frame, (uint16_t)sizeof(frame));

    gw_frame_parser_init(&p);
    for (i = 0u; i < flen; i++) {
        gw_frame_feed(&p, frame + i, 1u);       /* 一次只给 1 字节 */
    }

    check_int("逐字节喂仍能取出完整帧",
              esp32_recv(&p, &type, payload, (uint16_t)sizeof(payload), &plen), 1);
    check_int("载荷长度仍为 28", (int)plen, (int)GW_TELEMETRY_LEN);
    check_int("帧类型仍为 TELEMETRY", (int)type, (int)GW_FT_TELEMETRY);
}

/* ==================================================================
 *  用例 6：噪声重同步
 * ================================================================== */
static void test_noise_resync(void)
{
    gw_telemetry_t    t;
    uint8_t           frame[GW_FRAME_MAX_TOTAL];
    uint16_t          flen;
    uint8_t           noisy[8u + GW_FRAME_MAX_TOTAL];
    gw_frame_parser_t p;
    uint8_t           type = 0u;
    uint8_t           payload[GW_FRAME_MAX_PAYLOAD];
    uint16_t          plen = 0u;

    printf("\n--- 噪声重同步 ---\n");

    make_telemetry(&t);
    flen = stm32_send_telemetry(&t, frame, (uint16_t)sizeof(frame));

    /* 前 8 字节是噪声（不含帧头 0xA5）—— 模拟上电瞬间线上的杂波 */
    memset(noisy, 0x00, 8u);
    memcpy(noisy + 8u, frame, flen);

    gw_frame_parser_init(&p);
    gw_frame_feed(&p, noisy, (uint16_t)(8u + flen));

    check_int("跳过噪声后仍能解析出帧",
              esp32_recv(&p, &type, payload, (uint16_t)sizeof(payload), &plen), 1);
    check_int("载荷长度正确", (int)plen, (int)GW_TELEMETRY_LEN);
}

/* ==================================================================
 *  用例 7：CRC 损坏必须被拒（不能"带伤执行"）
 * ================================================================== */
static void test_crc_reject(void)
{
    gw_telemetry_t    t;
    uint8_t           frame[GW_FRAME_MAX_TOTAL];
    uint16_t          flen;
    gw_frame_parser_t p;
    uint8_t           type = 0u;
    uint8_t           payload[GW_FRAME_MAX_PAYLOAD];
    uint16_t          plen = 0u;

    printf("\n--- CRC 损坏 ---\n");

    make_telemetry(&t);
    flen = stm32_send_telemetry(&t, frame, (uint16_t)sizeof(frame));

    /* 破坏最后一个字节（CRC8 所在） */
    frame[flen - 1u] = (uint8_t)(frame[flen - 1u] ^ 0xFFu);

    gw_frame_parser_init(&p);
    gw_frame_feed(&p, frame, flen);

    check_int("CRC 损坏的帧被拒（不能带伤执行）",
              esp32_recv(&p, &type, payload, (uint16_t)sizeof(payload), &plen), 0);
    check_int("CRC 错误计数 +1",
              (int)gw_frame_stats(&p)->rx_crc_err, 1);
}

/* ==================================================================
 *  用例 8：遥测帧与指令帧混流（类型不能串）
 * ================================================================== */
static void test_multi_type(void)
{
    gw_telemetry_t    t;
    uint8_t           buf[GW_FRAME_MAX_TOTAL * 2u];
    uint16_t          off = 0u;
    uint16_t          flen;
    const char       *cmd  = "SET_PERIOD 5000";
    gw_frame_parser_t p;
    uint8_t           type = 0u;
    uint8_t           payload[GW_FRAME_MAX_PAYLOAD];
    uint16_t          plen = 0u;

    printf("\n--- 遥测 + 指令混流 ---\n");

    /* 上行：遥测 */
    make_telemetry(&t);
    flen = stm32_send_telemetry(&t, buf, (uint16_t)sizeof(buf));
    check_int("遥测帧构造成功", (flen > 0u) ? 1 : 0, 1);
    off = flen;

    /* 下行：指令（文本载荷）—— 与遥测走同一条字节流 */
    flen = 0u;
    check_int("指令帧构造成功",
              (gw_frame_build(buf + off, (uint16_t)(sizeof(buf) - off),
                              GW_FT_COMMAND, (const uint8_t *)cmd,
                              (uint16_t)strlen(cmd), &flen) == GW_FRAME_OK) ? 1 : 0, 1);
    off = (uint16_t)(off + flen);

    gw_frame_parser_init(&p);
    gw_frame_feed(&p, buf, off);

    /* 第 1 帧：遥测 */
    check_int("第 1 帧取到", esp32_recv(&p, &type, payload,
                                        (uint16_t)sizeof(payload), &plen), 1);
    check_int("第 1 帧是遥测", (int)type, (int)GW_FT_TELEMETRY);
    check_int("第 1 帧载荷 28 字节", (int)plen, (int)GW_TELEMETRY_LEN);

    /* 第 2 帧：指令 */
    check_int("第 2 帧取到", esp32_recv(&p, &type, payload,
                                        (uint16_t)sizeof(payload), &plen), 1);
    check_int("第 2 帧是指令", (int)type, (int)GW_FT_COMMAND);
    check_int("第 2 帧载荷长度", (int)plen, (int)strlen(cmd));

    payload[plen] = '\0';
    check_int("指令文本原样到达（不被截断）",
              (strcmp((const char *)payload, cmd) == 0) ? 1 : 0, 1);
}

/* ==================================================================
 *  main
 * ================================================================== */
int main(void)
{
    printf("========== STM32 -> ESP32 端到端链路对接 测试 ==========\n");

    test_single();
    test_negative_temp();
    test_boundary();
    test_coalesced();
    test_split();
    test_noise_resync();
    test_crc_reject();
    test_multi_type();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
