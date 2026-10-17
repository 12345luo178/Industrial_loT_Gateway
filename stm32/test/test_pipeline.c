/**
 * @file    test_pipeline.c
 * @brief   数据管道单元测试（主机端）
 *
 * ============================================================================
 *  这一层测什么？
 *
 *  管道本身不做采集、不做通信，它只做"翻译"：
 *      采集值  -> 寄存器值（单位、拆字、顺序）
 *      各家状态 -> device_status 位域
 *      计时累加 -> uptime
 *
 *  这类"翻译层"最容易出的问题是**规则不一致**：
 *    同一件事在三个地方各写一遍，改了一处忘了另两处。
 *  所以测试重点是"契约"而不是"算法"：
 *    32 位量是否严格高字在前、失败时是否保留旧值、连续失败几次才清状态位。
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iinc -Idrivers -o test/test_pipeline.exe \
 *        drivers/dht22.c src/gw_pipeline.c test/test_pipeline.c
 *    ./test/test_pipeline.exe
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

#include "gw_pipeline.h"
#include "modbus_rtu.h"
#include "modbus_regs.h"

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

static void check_u32(const char *name, uint32_t actual, uint32_t expected)
{
    if (actual == expected) {
        ok(name);
    } else {
        char d[128];
        sprintf(d, "(实际 %lu, 期望 %lu)",
                (unsigned long)actual, (unsigned long)expected);
        fail(name, d);
    }
}

/* ==================================================================
 * 假 DHT22 硬件：按"时间 -> 电平"分段表驱动，与 test_dht22.c 同一套做法
 * ================================================================== */
#define WF_MAX_SEG 96

typedef struct { uint32_t start, dur; uint8_t level; } wf_seg_t;
typedef struct { wf_seg_t seg[WF_MAX_SEG]; int n; } waveform_t;

typedef struct {
    const waveform_t *wf;
    uint32_t clock;
    uint32_t wave_base;
    uint8_t  level;
    uint8_t  use_wave;
} fake_gpio_t;

static uint8_t level_at(const waveform_t *wf, uint32_t t)
{
    int i;
    for (i = 0; i < wf->n; i++) {
        if (t < wf->seg[i].start + wf->seg[i].dur) {
            return wf->seg[i].level;
        }
    }
    return (wf->n > 0) ? wf->seg[wf->n - 1].level : 1u;
}

static uint32_t f_now(void *c) { fake_gpio_t *g = c; return g->clock; }
static void f_delay(void *c, uint32_t us) { fake_gpio_t *g = c; g->clock += us; }

static void f_set_out(void *c)
{
    fake_gpio_t *g = c;
    g->clock = 0u; g->wave_base = 0u; g->use_wave = 0u; g->level = 1u;
}
static void f_set_in(void *c)
{
    fake_gpio_t *g = c;
    g->wave_base = g->clock;
    g->use_wave = 1u;
}
static void f_write(void *c, int lv)
{
    fake_gpio_t *g = c;
    if (g->use_wave == 0u) { g->level = (uint8_t)(lv ? 1u : 0u); }
}
static int f_read(void *c)
{
    fake_gpio_t *g = c;
    g->clock++;
    if (g->use_wave != 0u && g->wf != 0) {
        return (int)level_at(g->wf, g->clock - g->wave_base);
    }
    return (int)g->level;
}

/* 追加一段"时间 -> 电平" */
static void wf_push(waveform_t *wf, uint32_t dur, uint8_t level)
{
    if (wf->n >= WF_MAX_SEG || dur == 0u) { return; }
    wf->seg[wf->n].start = (wf->n == 0) ? 0u
                          : wf->seg[wf->n - 1].start + wf->seg[wf->n - 1].dur;
    wf->seg[wf->n].dur = dur;
    wf->seg[wf->n].level = level;
    wf->n++;
}

/* DHT22 真实时序：低响应 + 高响应 + 40×(低分隔 + 高位宽) + 释放 */
static void wf_build(waveform_t *wf, const uint8_t bytes[5])
{
    int i;
    wf->n = 0;
    wf_push(wf, 80u, 0u);
    wf_push(wf, 80u, 1u);
    for (i = 0; i < 40; i++) {
        int bit = (bytes[i / 8] >> (7 - (i % 8))) & 1;
        wf_push(wf, 50u, 0u);
        wf_push(wf, 26u + (bit ? 44u : 0u), 1u);
    }
    wf_push(wf, 50u, 0u);
    wf_push(wf, 100u, 1u);
}

/* ==================================================================
 * 测试 1：时间累加与采样节拍
 * ================================================================== */
static void test_tick(void)
{
    gw_pipeline_t p;

    printf("\n[1] 时间累加与采样节拍\n");

    gw_pipeline_init(&p, 1u, 0u);
    check_u32("初始化后 uptime = 0", p.uptime_s, 0u);

    /* 累加 999ms 不足以进位到 1 秒 */
    gw_pipeline_tick(&p, 999u);
    check_u32("999ms 后 uptime 仍为 0", p.uptime_s, 0u);

    /* 再 1ms 即满 1 秒 */
    gw_pipeline_tick(&p, 1u);
    check_u32("满 1000ms 后 uptime = 1", p.uptime_s, 1u);

    /* 连续大步进：3 次 1000ms -> uptime = 4 */
    gw_pipeline_tick(&p, 3000u);
    check_u32("累计 4 秒", p.uptime_s, 4u);

    /* 采样节拍：GW_SAMPLE_PERIOD_MS = 2500ms */
    gw_pipeline_init(&p, 1u, 0u);
    check_int("初始化后无采样请求", gw_pipeline_take_sample_due(&p), 0);

    gw_pipeline_tick(&p, 2499u);
    check_int("2499ms 时未到采样点", gw_pipeline_take_sample_due(&p), 0);

    gw_pipeline_tick(&p, 1u);
    check_int("2500ms 时触发采样", gw_pipeline_take_sample_due(&p), 1);
    check_int("读取后标志清零（第二次返回 0）", gw_pipeline_take_sample_due(&p), 0);

    /* 大步进：一次 6000ms 应触发采样，且不应积压多次 */
    gw_pipeline_init(&p, 1u, 0u);
    gw_pipeline_tick(&p, 6000u);
    check_int("大步进后触发一次采样", gw_pipeline_take_sample_due(&p), 1);
    check_int("不积压第二次采样", gw_pipeline_take_sample_due(&p), 0);

    /* 两个实例互不干扰（验证没有函数级 static 状态） */
    {
        gw_pipeline_t a, b;
        gw_pipeline_init(&a, 1u, 0u);
        gw_pipeline_init(&b, 1u, 0u);
        gw_pipeline_tick(&a, 1000u);
        check_u32("实例 A uptime = 1", a.uptime_s, 1u);
        check_u32("实例 B uptime = 0（互不干扰）", b.uptime_s, 0u);
    }
}

/* ==================================================================
 * 测试 2：状态字位定义
 * ================================================================== */
static void test_status_word(void)
{
    gw_pipeline_t p;

    printf("\n[2] device_status 状态字\n");

    gw_pipeline_init(&p, 1u, 0u);
    check_int("初始状态字为 0（无任何有效数据）", gw_pipeline_status_word(&p), 0);

    /* 有有效读数 -> DHT_OK */
    p.dht.valid = 1u;
    p.dht.temp_x10 = 250;
    p.dht.humi_x10 = 600;
    p.dht.fail_streak = 0u;
    if ((gw_pipeline_status_word(&p) & IREG_STATUS_BIT_DHT_OK) != 0u) {
        ok("有有效读数时置 DHT_OK");
    } else {
        fail("有有效读数时置 DHT_OK", "");
    }

    /* 连续失败达阈值 -> 清 DHT_OK，但读数仍保留 */
    p.dht.fail_streak = GW_DHT_FAIL_LIMIT;
    if ((gw_pipeline_status_word(&p) & IREG_STATUS_BIT_DHT_OK) == 0u) {
        ok("连续失败达阈值时清 DHT_OK");
    } else {
        fail("连续失败达阈值时清 DHT_OK", "");
    }
    check_int("清 DHT_OK 后旧读数仍保留", (int)p.dht.temp_x10, 250);

    /* 失败计数回到阈值以下 -> 恢复 DHT_OK */
    p.dht.fail_streak = GW_DHT_FAIL_LIMIT - 1u;
    if ((gw_pipeline_status_word(&p) & IREG_STATUS_BIT_DHT_OK) != 0u) {
        ok("失败计数降低后恢复 DHT_OK");
    } else {
        fail("失败计数降低后恢复 DHT_OK", "");
    }

    /* CAN */
    p.can_ok = 1u;
    if ((gw_pipeline_status_word(&p) & IREG_STATUS_BIT_CAN_OK) != 0u) {
        ok("can_ok = 1 时置 CAN_OK");
    } else {
        fail("can_ok = 1 时置 CAN_OK", "");
    }
    p.can_ok = 0u;
    if ((gw_pipeline_status_word(&p) & IREG_STATUS_BIT_CAN_OK) == 0u) {
        ok("can_ok = 0 时清 CAN_OK");
    } else {
        fail("can_ok = 0 时清 CAN_OK", "");
    }

    /* MQTT：连通时应同时置 WIFI_OK 与 MQTT_OK */
    p.mqtt_status = IREG_MQTT_ST_CONNECTED;
    {
        uint16_t w = gw_pipeline_status_word(&p);
        if ((w & IREG_STATUS_BIT_MQTT_OK) != 0u &&
            (w & IREG_STATUS_BIT_WIFI_OK) != 0u) {
            ok("MQTT 连通时同时置 MQTT_OK 与 WIFI_OK");
        } else {
            fail("MQTT 连通时同时置 MQTT_OK 与 WIFI_OK", "");
        }
    }
    p.mqtt_status = IREG_MQTT_ST_CONNECTING;
    if ((gw_pipeline_status_word(&p) & IREG_STATUS_BIT_MQTT_OK) == 0u) {
        ok("MQTT 连接中时不置 MQTT_OK");
    } else {
        fail("MQTT 连接中时不置 MQTT_OK", "");
    }

    /* sticky 位：置位后持续为 1，直到显式清除 */
    gw_pipeline_set_sticky(&p, IREG_STATUS_BIT_IWDG_RESET);
    if ((gw_pipeline_status_word(&p) & IREG_STATUS_BIT_IWDG_RESET) != 0u) {
        ok("sticky 位置位后状态字反映它");
    } else {
        fail("sticky 位置位后状态字反映它", "");
    }
    /* 一次 tick 或多次读状态字都不会自动清除 */
    (void)gw_pipeline_status_word(&p);
    if ((gw_pipeline_status_word(&p) & IREG_STATUS_BIT_IWDG_RESET) != 0u) {
        ok("sticky 位不会自动清除");
    } else {
        fail("sticky 位不会自动清除", "");
    }
    gw_pipeline_clear_sticky(&p, IREG_STATUS_BIT_IWDG_RESET);
    if ((gw_pipeline_status_word(&p) & IREG_STATUS_BIT_IWDG_RESET) == 0u) {
        ok("显式清除后 sticky 位归零");
    } else {
        fail("显式清除后 sticky 位归零", "");
    }

    /* 参数校验位也走 sticky 机制 */
    gw_pipeline_set_sticky(&p, IREG_STATUS_BIT_PARAM_OK);
    if ((gw_pipeline_status_word(&p) & IREG_STATUS_BIT_PARAM_OK) != 0u) {
        ok("参数校验位由 sticky 机制驱动");
    } else {
        fail("参数校验位由 sticky 机制驱动", "");
    }

    /* 主站在线位图非 0 -> MASTER_OK */
    p.master_online_bmp = 0x0005u;
    if ((gw_pipeline_status_word(&p) & IREG_STATUS_BIT_MASTER_OK) != 0u) {
        ok("主站在线位图非 0 时置 MASTER_OK");
    } else {
        fail("主站在线位图非 0 时置 MASTER_OK", "");
    }
    p.master_online_bmp = 0u;
    if ((gw_pipeline_status_word(&p) & IREG_STATUS_BIT_MASTER_OK) == 0u) {
        ok("主站无在线从机时清 MASTER_OK");
    } else {
        fail("主站无在线从机时清 MASTER_OK", "");
    }
}

/* ==================================================================
 * 测试 3：32 位量拆分与重组
 * ================================================================== */
static void test_split_join(void)
{
    uint16_t hi = 0u, lo = 0u;
    const uint32_t cases[5] = { 0u, 1u, 0x00010000u, 0x12345678u, 0xFFFFFFFFu };
    int i;

    printf("\n[3] 32 位量拆分与重组（必须严格高字在前）\n");

    for (i = 0; i < 5; i++) {
        gw_split_u32(cases[i], &hi, &lo);
        if (gw_join_u32(hi, lo) == cases[i]) {
            ok("拆分后重组结果一致");
        } else {
            char d[96];
            sprintf(d, "(0x%08lX -> hi=0x%04X lo=0x%04X)",
                    (unsigned long)cases[i], hi, lo);
            fail("拆分后重组结果一致", d);
        }
    }

    /* 明确检查字节序：0x12345678 应拆成 hi=0x1234, lo=0x5678 */
    gw_split_u32(0x12345678u, &hi, &lo);
    if (hi == 0x1234u && lo == 0x5678u) {
        ok("0x12345678 -> 高字 0x1234 / 低字 0x5678");
    } else {
        char d[64];
        sprintf(d, "(hi=0x%04X lo=0x%04X)", hi, lo);
        fail("高字在前拆分正确", d);
    }

    /* 空指针应安全 */
    gw_split_u32(0x11223344u, 0, 0);
    ok("拆分时空指针安全（不崩溃）");
}

/* ==================================================================
 * 测试 4：寄存器同步（核心契约）
 * ================================================================== */
static void test_sync_regs(void)
{
    gw_pipeline_t p;
    mb_regs_t     regs;
    uint16_t      hi, lo;

    printf("\n[4] 寄存器同步\n");

    gw_pipeline_init(&p, 2u, 7u);
    memset(&regs, 0, sizeof(regs));

    /* 准备数据 */
    p.dht.valid = 1u;
    p.dht.temp_x10 = (int16_t)-105;     /* -10.5℃ */
    p.dht.humi_x10 = 612u;
    p.dht.fail_streak = 0u;
    p.can_ok = 1u;
    p.can_ch[0] = 0xBEEFu;
    p.can_ch[7] = 0x1234u;
    p.uptime_s = 0x000186A0u;           /* 100000 秒 */
    p.mb_rx = 0x00010002u;
    p.mb_err = 7u;
    p.can_rx = 0x00020003u;
    p.mb_exc = 2u;
    p.mqtt_status = IREG_MQTT_ST_CONNECTED;
    p.offline_depth = 42u;
    p.offline_drop = 3u;
    p.master_online_bmp = 0x0007u;
    p.master_err = 1u;

    gw_pipeline_sync_regs(&p, &regs);

    check_int("温度（含负号）写入正确",
              (int)(int16_t)regs.input[IREG_TEMP_X10], -105);
    check_int("湿度写入正确", (int)regs.input[IREG_HUMI_X10], 612);

    check_int("CAN 通道 1 写入正确", (int)regs.input[IREG_CAN_CH1], 0xBEEF);
    check_int("CAN 通道 8 写入正确", (int)regs.input[IREG_CAN_CH8], 0x1234);

    gw_split_u32(p.uptime_s, &hi, &lo);
    check_int("uptime 高字正确", (int)regs.input[IREG_UPTIME_HI], hi);
    check_int("uptime 低字正确", (int)regs.input[IREG_UPTIME_LO], lo);
    check_u32("uptime 从寄存器重组正确",
              gw_join_u32(regs.input[IREG_UPTIME_HI], regs.input[IREG_UPTIME_LO]),
              0x000186A0u);

    /* 32 位计数：高低字必须都在，且能重组 */
    gw_split_u32(p.mb_rx, &hi, &lo);
    check_int("Modbus 收帧计数高字", (int)regs.input[IREG_MB_RX_HI], hi);
    check_int("Modbus 收帧计数低字", (int)regs.input[IREG_MB_RX_LO], lo);
    check_u32("Modbus 收帧计数重组正确",
              gw_join_u32(regs.input[IREG_MB_RX_HI], regs.input[IREG_MB_RX_LO]),
              0x00010002u);

    check_u32("CAN 收帧计数重组正确",
              gw_join_u32(regs.input[IREG_CAN_RX_HI], regs.input[IREG_CAN_RX_LO]),
              0x00020003u);
    check_int("Modbus 错误帧计数（小于 65536 时高字为 0）",
              (int)regs.input[IREG_MB_ERR_HI], 0);
    check_int("Modbus 错误帧计数低字", (int)regs.input[IREG_MB_ERR_LO], 7);
    check_int("异常响应计数低字", (int)regs.input[IREG_MB_EXC_LO], 2);

    check_int("MQTT 状态写入正确",
              (int)regs.input[IREG_MQTT_STATUS], IREG_MQTT_ST_CONNECTED);
    check_int("断网缓存深度写入正确", (int)regs.input[IREG_OFFLINE_DEPTH], 42);
    check_int("缓存溢出丢包数写入正确", (int)regs.input[IREG_OFFLINE_DROP], 3);

    check_int("固件主版本写入正确", (int)regs.input[IREG_FW_VER_MAJOR], 2);
    check_int("固件次版本写入正确", (int)regs.input[IREG_FW_VER_MINOR], 7);
    check_int("主站在线位图写入正确", (int)regs.input[IREG_MASTER_ONLINE], 0x0007);
    check_int("主站错误码写入正确", (int)regs.input[IREG_MASTER_ERR], 1);

    /* 状态字应与 gw_pipeline_status_word 一致 */
    check_int("状态字与 status_word 一致",
              (int)regs.input[IREG_DEV_STATUS],
              (int)gw_pipeline_status_word(&p));

    /* 保留区不应被本模块写入 */
    check_int("保留区 0x1C 未被写入", (int)regs.input[IREG_RESERVED_28], 0);
    check_int("保留区 0x1F 未被写入", (int)regs.input[IREG_RESERVED_31], 0);

    /* 空指针安全 */
    gw_pipeline_sync_regs(&p, 0);
    gw_pipeline_sync_regs(0, &regs);
    ok("寄存器同步对空指针安全");
}

/* ==================================================================
 * 测试 5：失败时保留旧值（重要契约）
 * ================================================================== */
static void test_stale_value_retention(void)
{
    gw_pipeline_t p;
    mb_regs_t     regs;

    printf("\n[5] 采集失败时保留旧值（不能清零）\n");

    gw_pipeline_init(&p, 1u, 0u);
    memset(&regs, 0, sizeof(regs));

    /* 先写一次有效值 */
    p.dht.valid = 1u;
    p.dht.temp_x10 = 253;
    p.dht.humi_x10 = 612u;
    p.dht.fail_streak = 0u;
    gw_pipeline_sync_regs(&p, &regs);
    check_int("首次同步写入温度 253", (int)(int16_t)regs.input[IREG_TEMP_X10], 253);

    /* 模拟连续失败：fail_streak 增长，但 dht 的读数不变 */
    p.dht.fail_streak = GW_DHT_FAIL_LIMIT;
    gw_pipeline_sync_regs(&p, &regs);
    check_int("失败后温度仍为 253（未被清零）",
              (int)(int16_t)regs.input[IREG_TEMP_X10], 253);
    check_int("失败后湿度仍为 612", (int)regs.input[IREG_HUMI_X10], 612);
    if ((regs.input[IREG_DEV_STATUS] & IREG_STATUS_BIT_DHT_OK) == 0u) {
        ok("失败后 DHT_OK 位被清除（状态如实反映）");
    } else {
        fail("失败后 DHT_OK 位被清除", "");
    }

    /* 对比：如果实现"失败就清零"，上位机会把 0 当成 0℃ 而不是"无数据"，
       这与"保留旧值 + 清状态位"相比是更糟的选择。这里把这个理由写成断言。 */
    ok("语义确认：清状态位而非清数据（0℃ 是合法读数，不能用它表示无数据）");
}

/* ==================================================================
 * 测试 5.5：**从未**取得有效读数时，温湿度寄存器必须是"不可用"哨兵值
 *
 * ⚠️ 守护 2026-10-10 修掉的一个真问题：
 *    原实现只在"有有效读数"时才写那两个寄存器，于是**从没读到过**
 *    这种情况会让寄存器停在 0 —— 上位机看到的是 0.0℃ / 0.0%RH，
 *    与真实读数无法区分（而本模块注释一直写着"宁可用旧值也不要填 0"，
 *    只是漏了"连旧值都没有"这条路径）。
 *
 * ⚠️ 两种状态不能混：
 *    从无有效值       -> 哨兵值（这里根本没有数据）
 *    有旧值但当前故障 -> 保留旧值 + DHT_OK=0（数据不新鲜）
 * ================================================================== */
static void test_unavailable_sentinel(void)
{
    gw_pipeline_t p;
    mb_regs_t     regs;

    printf("\n[5.5] 从未读到过 -> 温湿度寄存器写哨兵值（不是 0）\n");

    gw_pipeline_init(&p, 1u, 0u);
    memset(&regs, 0, sizeof(regs));         /* 先全 0，模拟"刚上电" */

    p.dht.valid       = 0u;                 /* 一条有效读数都没有 */
    p.dht.fail_streak = GW_DHT_FAIL_LIMIT;
    gw_pipeline_sync_regs(&p, &regs);

    check_int("温度寄存器是哨兵值而不是 0",
              (int)regs.input[IREG_TEMP_X10], (int)GW_DHT_REG_UNAVAILABLE);
    check_int("湿度寄存器是哨兵值而不是 0",
              (int)regs.input[IREG_HUMI_X10], (int)GW_DHT_REG_UNAVAILABLE);
    check_int("哨兵值按 int16 解释是负数（-32768，不可能是真实温度）",
              (int)(int16_t)regs.input[IREG_TEMP_X10], (int)(int16_t)0x8000);
    if ((regs.input[IREG_DEV_STATUS] & IREG_STATUS_BIT_DHT_OK) == 0u) {
        ok("同时 DHT_OK = 0");
    } else {
        fail("同时 DHT_OK = 0", "");
    }

    /* 有过有效读数之后：即便当前判定故障，也应当写**上一次的有效值** */
    p.dht.valid       = 1u;
    p.dht.temp_x10    = 253;
    p.dht.humi_x10    = 612u;
    p.dht.fail_streak = GW_DHT_FAIL_LIMIT;
    gw_pipeline_sync_regs(&p, &regs);
    check_int("有旧值 + 故障 -> 仍写旧值 253（不是哨兵值）",
              (int)(int16_t)regs.input[IREG_TEMP_X10], 253);
    check_int("有旧值 + 故障 -> 仍写旧值 612（不是哨兵值）",
              (int)regs.input[IREG_HUMI_X10], 612);
}

/* ==================================================================
 * 测试 6：与真实 DHT22 驱动联调（假引脚）
 * ================================================================== */
static void test_with_real_dht(void)
{
    gw_pipeline_t  p;
    dht22_hw_t     hw;
    fake_gpio_t    g;
    waveform_t     wf;
    uint8_t        payload[5] = { 0x02, 0x64, 0x00, 0xFD, 0x63 };  /* 61.2%, 25.3C */

    printf("\n[6] 与真实 DHT22 驱动联调\n");

    memset(&g, 0, sizeof(g));
    memset(&hw, 0, sizeof(hw));
    memset(&wf, 0, sizeof(wf));
    g.wf = &wf;
    hw.set_out = f_set_out;  hw.set_in = f_set_in;
    hw.write = f_write;      hw.read = f_read;
    hw.delay_us = f_delay;   hw.now_us = f_now;
    hw.ctx = &g;

    gw_pipeline_init(&p, 1u, 0u);

    /* 成功采样 */
    wf_build(&wf, payload);
    if (gw_pipeline_sample_dht(&p, &hw, 0u) == DHT22_OK) {
        ok("管道采样成功");
    } else {
        fail("管道采样成功", "");
    }
    check_int("管道内温度 = 253", (int)p.dht.temp_x10, 253);
    check_int("管道内湿度 = 612", (int)p.dht.humi_x10, 612);
    check_int("成功计数 = 1", (int)p.dht.stat_ok, 1);
    check_int("读取尝试计数 = 1（不应被记两次）", (int)p.dht.stat_reads, 1);

    /* 失败采样：统计应各加一次，不应翻倍 */
    {
        uint8_t bad[5] = { 0x02, 0x64, 0x00, 0xFD, 0x00 };
        memset(&g, 0, sizeof(g));
        g.wf = &wf;
        wf_build(&wf, bad);
        (void)gw_pipeline_sample_dht(&p, &hw, 0u);
        check_int("失败后读取尝试计数 = 2", (int)p.dht.stat_reads, 2);
        check_int("校验错误计数 = 1（不翻倍）", (int)p.dht.stat_err_crc, 1);
        check_int("失败后温度保留 253", (int)p.dht.temp_x10, 253);
    }

    /* 硬件不可用：应按失败处理且不崩溃 */
    {
        int before = (int)p.dht.stat_reads;
        dht22_err_t e = gw_pipeline_sample_dht(&p, 0, 0u);
        if (e == DHT22_ERR_PARAM) {
            ok("硬件接口为 NULL 时返回 PARAM");
        } else {
            fail("硬件接口为 NULL 时返回 PARAM", "");
        }
        check_int("硬件不可用也计入一次尝试",
                  (int)p.dht.stat_reads, before + 1);
    }
}


/* ==================================================================
 * 测试 8：gw_pipeline_sync_from_regs 的"空操作"契约
 *
 *  ⚠️ 这个函数**目前是空的**（注释写明"没有需要反向同步的字段"），
 *     而且没有任何调用方 —— 是一个刻意预留的扩展点。
 *
 *  那为什么要测它？因为它是一个**有契约的公开 API**：
 *
 *      "调用它不改变管道与寄存器表的任何内容"
 *
 *  如果将来有人往里加了字段同步却忘了改这里，这个测试会失败，
 *  提醒他"这个 API 的语义变了，得同步更新文档与调用方"。
 *
 *  **测一个空函数不是浪费** —— 它把"现在的意图"固化下来了。
 * ================================================================== */
static void test_sync_from_regs_is_noop(void)
{
    gw_pipeline_t p;
    mb_regs_t     regs;
    mb_regs_t     before;
    gw_pipeline_t p_before;

    printf("\n[8] gw_pipeline_sync_from_regs 的空操作契约\n");

    gw_pipeline_init(&p, 1u, 0u);
    memset(&regs, 0, sizeof(regs));

    /* 造一些非默认值，确保"没变化"不是因为两边都是 0 */
    p.uptime_s        = 1234u;
    p.offline_depth   = 7u;
    p.can_ch[0]       = 4321u;
    regs.holding[HREG_REPORT_PERIOD] = 42u;
    regs.holding[HREG_BAUDRATE]      = 1u;
    regs.input[IREG_TEMP_X10]        = 250;

    p_before = p;
    before   = regs;

    gw_pipeline_sync_from_regs(&p, &regs);

    check_int("调用后管道未被修改",
              (int)(memcmp(&p, &p_before, sizeof(p)) == 0 ? 1 : 0), 1);
    check_int("调用后寄存器表未被修改",
              (int)(memcmp(&regs, &before, sizeof(regs)) == 0 ? 1 : 0), 1);
    ok("⚠️ 契约固化：这个 API 目前是空操作。若将来它开始同步字段，"
       "本测试会失败，提醒更新文档与调用方");

    /* 空指针安全 */
    gw_pipeline_sync_from_regs(0, &regs);
    gw_pipeline_sync_from_regs(&p, 0);
    gw_pipeline_sync_from_regs(0, 0);
    check_int("空指针调用不崩且无副作用",
              (int)(memcmp(&regs, &before, sizeof(regs)) == 0 ? 1 : 0), 1);
    ok("空指针安全");
}

int main(void)
{
    printf("========== 数据管道单元测试（主机端） ==========\n");

    test_tick();
    test_status_word();
    test_split_join();
    test_sync_regs();
    test_stale_value_retention();
    test_unavailable_sentinel();
    test_with_real_dht();
    test_sync_from_regs_is_noop();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
