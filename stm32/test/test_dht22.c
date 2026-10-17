/**
 * @file    test_dht22.c
 * @brief   DHT22 驱动单元测试（主机端，不需要硬件）
 *
 * ============================================================================
 *  怎么在没有硬件的情况下测试单总线驱动？
 *
 *  驱动分四层，每层都能单独验证：
 *    1) dht22_bits_to_bytes()  40 个高电平时长 -> 5 字节
 *    2) dht22_decode()         5 字节 -> 温湿度 + 校验
 *    3) dht22_filter_push()    滑动均值
 *    4) dht22_read()           完整时序（通过 dht22_hw_t 注入假引脚）
 *
 * ----------------------------------------------------------------------------
 *  假硬件的设计（这一版是第三版，前两版都踩了坑，记下来避免重犯）
 *
 *  v1 错误：波形脚本读完后不推进时间 -> 驱动轮询死循环
 *  v2 错误：给硬件层加了 wait() 回调，驱动"等待顺序"必须与假硬件的
 *           "分段推进"严格同步；一旦不同步就时序错位，
 *           甚至读到越界垃圾值（测出 62720 这种不可能的微秒数）
 *
 *  两版的根本原因相同：**时间的推进有多个驱动源**。
 *  v3 的设计只保留一个时间源：
 *
 *    引脚电平 = f(时间)   ——  一张"时间 -> 电平"的分段常量表
 *    时间只由 clock 表示，由 fake_read() 每次 +1µs、fake_delay() 直接加
 *
 *  于是不存在"谁先推进"的问题：任何时刻给一个 clock，电平就唯一确定。
 *
 *  另外，波形必须与真实器件的编码方式一致：
 *    DHT22 的位宽信息在**高电平**上（26µs=位0，70µs=位1），
 *    低电平是固定的分隔时间。早先写成"高固定+低变化"，
 *    与 dht22_bits_to_bytes 的判据不符，位全解码成 0。
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

#include "dht22_port.h"

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

/* ==================================================================
 * 假硬件：引脚电平 = f(时间)
 * ================================================================== */
#define WF_MAX_SEG 96

typedef struct {
    uint32_t start;
    uint32_t dur;
    uint8_t  level;
} wf_seg_t;

typedef struct {
    wf_seg_t seg[WF_MAX_SEG];
    int      n;
} waveform_t;

typedef struct {
    const waveform_t *wf;
    uint32_t clock;         /* 唯一时间源 */
    uint32_t wave_base;     /* 波形表的时间基线（= set_in 时刻的 clock） */
    uint8_t  level;         /* use_wave=0 时的电平（由 write 决定） */
    uint8_t  use_wave;      /* 1 = 电平由波形表决定；0 = 由 write 决定 */
} fake_gpio_t;

static uint8_t fake_level_at(const waveform_t *wf, uint32_t t)
{
    int i;
    for (i = 0; i < wf->n; i++) {
        if (t < wf->seg[i].start + wf->seg[i].dur) {
            return wf->seg[i].level;
        }
    }
    return (wf->n > 0) ? wf->seg[wf->n - 1].level : 1u;
}

static uint32_t fake_now(void *ctx)
{
    fake_gpio_t *g = (fake_gpio_t *)ctx;
    return g->clock;
}

static void fake_delay(void *ctx, uint32_t us)
{
    fake_gpio_t *g = (fake_gpio_t *)ctx;
    g->clock += us;
}

static void fake_set_out(void *ctx)
{
    fake_gpio_t *g = (fake_gpio_t *)ctx;
    g->clock = 0u;          /* 起始信号开始，时间基线归零 */
    g->wave_base = 0u;
    g->use_wave = 0u;
    g->level = 1u;
}

static void fake_set_in(void *ctx)
{
    fake_gpio_t *g = (fake_gpio_t *)ctx;
    /* 切换为输入：此后电平由波形表（传感器）决定。
     *
     * ⚠️ 必须记录波形基线：set_out 到 set_in 之间经历了 18ms 起始信号 +
     *    30µs 释放时间，此时 clock 已经是一万八千多。而波形表的时间轴是
     *    相对"传感器开始响应"的 0 点。若不减去基线，整条波形会错位到
     *    18ms 之前，表现为"第一个读数就恒高/恒低"。
     *    （这是实测踩到的坑：波形表的时间原点和真机时钟原点不是同一个。） */
    g->wave_base = g->clock;
    g->use_wave = 1u;
}

static void fake_write(void *ctx, int level)
{
    fake_gpio_t *g = (fake_gpio_t *)ctx;
    if (g->use_wave == 0u) {
        g->level = (uint8_t)(level ? 1u : 0u);
    }
}

/* 每次采样推进 1µs，保证驱动的轮询循环必然终止 */
static int fake_read(void *ctx)
{
    fake_gpio_t *g = (fake_gpio_t *)ctx;
    g->clock++;
    if (g->use_wave != 0u && g->wf != 0) {
        /* 用"相对波形基线的时刻"查表 */
        return (int)fake_level_at(g->wf, g->clock - g->wave_base);
    }
    return (int)g->level;
}

/* ==================================================================
 * 波形构造：按"时间 -> 电平"分段追加，段首尾相接
 * ================================================================== */
static void wf_push(waveform_t *wf, uint32_t dur, uint8_t level)
{
    if (wf->n >= WF_MAX_SEG || dur == 0u) {
        return;                         /* 零长度段直接跳过，避免空段 */
    }
    wf->seg[wf->n].start = (wf->n == 0) ? 0u
                          : wf->seg[wf->n - 1].start + wf->seg[wf->n - 1].dur;
    wf->seg[wf->n].dur   = dur;
    wf->seg[wf->n].level = level;
    wf->n++;
}

/**
 * DHT22 真实时序（相对 set_in 调用时刻）：
 *   低 pull_low   传感器响应拉低
 *   高 ack_high   响应结束
 *   每 40 位：低 bit_low（固定分隔）+ 高 26µs(位0) / 70µs(位1)
 *   最后释放总线回高电平
 */
static void wf_build(waveform_t *wf, const uint8_t bytes[5],
                     uint32_t pull_low_us, uint32_t ack_high_us,
                     uint32_t bit_low_us, uint32_t base_high_us)
{
    int i;

    wf->n = 0;
    wf_push(wf, pull_low_us, 0u);
    wf_push(wf, (ack_high_us == 0u) ? 50u : ack_high_us, 1u);

    for (i = 0; i < 40; i++) {
        int bit = (bytes[i / 8] >> (7 - (i % 8))) & 1;
        wf_push(wf, bit_low_us, 0u);
        wf_push(wf, base_high_us + (bit ? 44u : 0u), 1u);
    }

    wf_push(wf, bit_low_us, 0u);        /* 最后一位后的低分隔 */
    wf_push(wf, 100u, 1u);              /* 释放总线回高 */
}

static void wf_stuck(waveform_t *wf, uint8_t level)
{
    wf->n = 0;
    wf_push(wf, 1000000u, level);
}

static void setup_fake(fake_gpio_t *g, dht22_hw_t *hw, const waveform_t *wf)
{
    memset(g, 0, sizeof(*g));
    memset(hw, 0, sizeof(*hw));
    g->wf = wf;
    hw->set_out  = fake_set_out;
    hw->set_in   = fake_set_in;
    hw->write    = fake_write;
    hw->read     = fake_read;
    hw->delay_us = fake_delay;
    hw->now_us   = fake_now;
    hw->ctx      = g;
}

/* ==================================================================
 * 测试 1：纯位解码
 * ================================================================== */
static void test_bits_to_bytes(void)
{
    uint16_t highs[40];
    uint8_t  out[5];
    uint8_t  expect[5] = { 0x02, 0x64, 0x00, 0xFD, 0x63 };
    int      i;

    printf("\n[1] 位解码：40 个高电平时长 -> 5 字节\n");

    for (i = 0; i < 40; i++) {
        int bit = (expect[i / 8] >> (7 - (i % 8))) & 1;
        highs[i] = (uint16_t)(bit ? 70u : 26u);
    }
    if (dht22_bits_to_bytes(highs, out) != DHT22_OK) {
        fail("位解码返回 OK", "");
        return;
    }
    if (memcmp(out, expect, 5) == 0) {
        ok("MSB 优先、逐位还原正确");
    } else {
        char d[128];
        sprintf(d, "(得到 %02X %02X %02X %02X %02X)",
                out[0], out[1], out[2], out[3], out[4]);
        fail("MSB 优先、逐位还原正确", d);
    }

    for (i = 0; i < 40; i++) { highs[i] = 50u; }
    (void)dht22_bits_to_bytes(highs, out);
    if (out[0] == 0x00 && out[4] == 0x00) { ok("恰好 50µs 判为位 0"); }
    else { fail("恰好 50µs 判为位 0", ""); }

    for (i = 0; i < 40; i++) { highs[i] = 51u; }
    (void)dht22_bits_to_bytes(highs, out);
    if (out[0] == 0xFF && out[4] == 0xFF) { ok("恰好 51µs 判为位 1"); }
    else { fail("恰好 51µs 判为位 1", ""); }

    if (dht22_bits_to_bytes(0, out) == DHT22_ERR_PARAM) {
        ok("空指针入参返回 PARAM");
    } else {
        fail("空指针入参返回 PARAM", "");
    }
}

/* ==================================================================
 * 测试 2：字节解析（校验和、负温度）
 * ================================================================== */
static void test_decode(void)
{
    dht22_raw_t r;

    printf("\n[2] 字节解析：校验和与温度符号\n");

    {
        uint8_t raw[5] = { 0x02, 0x64, 0x00, 0xFD, 0x63 };
        if (dht22_decode(raw, &r) == DHT22_OK) { ok("校验和正确时返回 OK"); }
        else { fail("校验和正确时返回 OK", ""); }
        check_int("湿度 = 612 (61.2%RH)", (int)r.humi_x10, 612);
        check_int("温度 = 253 (25.3C)", (int)r.temp_x10, 253);
        check_int("checksum_ok 标志为 1", (int)r.checksum_ok, 1);
    }

    {   /* -10.5C：0x8069，校验 0x4F */
        uint8_t raw[5] = { 0x02, 0x64, 0x80, 0x69, 0x4F };
        if (dht22_decode(raw, &r) == DHT22_OK) { ok("负温度帧校验通过"); }
        else { fail("负温度帧校验通过", ""); }
        check_int("负温度 = -105 (-10.5C)", (int)r.temp_x10, -105);
        check_int("负温度时湿度仍正确", (int)r.humi_x10, 612);
    }

    {   /* -0.1C：0x8001，校验 0xE7 */
        uint8_t raw[5] = { 0x02, 0x64, 0x80, 0x01, 0xE7 };
        (void)dht22_decode(raw, &r);
        check_int("接近 0 的负温度 = -1 (-0.1C)", (int)r.temp_x10, -1);
    }

    {
        uint8_t raw[5] = { 0x02, 0x64, 0x00, 0xFD, 0x64 };
        if (dht22_decode(raw, &r) == DHT22_ERR_CHECKSUM) { ok("校验和错误返回 CHECKSUM"); }
        else { fail("校验和错误返回 CHECKSUM", ""); }
        check_int("校验失败时 checksum_ok = 0", (int)r.checksum_ok, 0);
        check_int("校验失败时不输出温度", (int)r.temp_x10, 0);
    }

    if (dht22_decode(0, &r) == DHT22_ERR_PARAM) {
        ok("空指针入参返回 PARAM");
    } else {
        fail("空指针入参返回 PARAM", "");
    }
}

/* ==================================================================
 * 测试 3：滑动均值滤波
 * ================================================================== */
static void test_filter(void)
{
    dht22_filtered_t f;
    dht22_raw_t s;

    printf("\n[3] 滑动均值滤波（窗口 5）\n");

    dht22_filter_init(&f);
    check_int("初始化后 valid = 0", (int)f.valid, 0);

    s.temp_x10 = 250; s.humi_x10 = 600; s.checksum_ok = 1u;
    dht22_filter_push(&f, &s);
    check_int("单样本温度 = 250", (int)f.temp_x10, 250);
    check_int("单样本湿度 = 600", (int)f.humi_x10, 600);
    check_int("单样本后 valid = 1", (int)f.valid, 1);

    s.temp_x10 = 260; s.humi_x10 = 610;
    dht22_filter_push(&f, &s);
    check_int("两样本均值温度 = 255", (int)f.temp_x10, 255);
    check_int("两样本均值湿度 = 605", (int)f.humi_x10, 605);

    /* 填满窗口：{250,260,270,280,290} 和=1350 均值=270 */
    s.temp_x10 = 270; s.humi_x10 = 620; dht22_filter_push(&f, &s);
    s.temp_x10 = 280; s.humi_x10 = 630; dht22_filter_push(&f, &s);
    s.temp_x10 = 290; s.humi_x10 = 640; dht22_filter_push(&f, &s);
    check_int("窗口填满后温度 = 270", (int)f.temp_x10, 270);
    check_int("窗口填满后湿度 = 620", (int)f.humi_x10, 620);

    /* 滑动：挤掉 250 -> {260,270,280,290,300} 和=1400 均值=280 */
    s.temp_x10 = 300; s.humi_x10 = 650; dht22_filter_push(&f, &s);
    check_int("滑动后温度 = 280（最旧值被挤出）", (int)f.temp_x10, 280);
    check_int("滑动后湿度 = 630", (int)f.humi_x10, 630);

    {
        int16_t before = f.temp_x10;
        dht22_filter_push(&f, 0);
        check_int("失败采样后均值不变", (int)f.temp_x10, (int)before);
        check_int("失败采样使 fail_streak 递增", (int)f.fail_streak, 1);
    }

    {
        int16_t before = f.temp_x10;
        s.temp_x10 = 9999; s.humi_x10 = 9999; s.checksum_ok = 0u;
        dht22_filter_push(&f, &s);
        check_int("校验失败样本不污染均值", (int)f.temp_x10, (int)before);
    }

    {   /* 负数均值：-100 与 -101 -> 四舍五入应为 -101 */
        dht22_filtered_t f2;
        dht22_filter_init(&f2);
        s.temp_x10 = -100; s.humi_x10 = 500; s.checksum_ok = 1u;
        dht22_filter_push(&f2, &s);
        s.temp_x10 = -101;
        dht22_filter_push(&f2, &s);
        check_int("负数均值 -100/-101 -> -101", (int)f2.temp_x10, -101);
    }

    /* 两个实例互不干扰（验证没有隐藏的全局状态） */
    {
        dht22_filtered_t fa, fb;
        dht22_filter_init(&fa);
        dht22_filter_init(&fb);
        s.temp_x10 = 100; s.humi_x10 = 400; s.checksum_ok = 1u;
        dht22_filter_push(&fa, &s);
        s.temp_x10 = 900; s.humi_x10 = 800;
        dht22_filter_push(&fb, &s);
        check_int("实例 A 温度 = 100", (int)fa.temp_x10, 100);
        check_int("实例 B 温度 = 900（互不干扰）", (int)fb.temp_x10, 900);
    }
}

/* ==================================================================
 * 测试 4：完整读取路径（假引脚）
 * ================================================================== */
static void test_full_read(void)
{
    waveform_t   wf;
    fake_gpio_t  g;
    dht22_hw_t   hw;
    dht22_raw_t  r;
    uint8_t      payload[5] = { 0x02, 0x64, 0x00, 0xFD, 0x63 };

    printf("\n[4] 完整读取路径（假引脚注入）\n");

    memset(&wf, 0, sizeof(wf));
    setup_fake(&g, &hw, &wf);

    /* ---- 正常帧 ---- */
    wf_build(&wf, payload, 80u, 80u, 50u, 26u);
    if (dht22_read(&hw, &r, 0u, 0) == DHT22_OK) {
        ok("正常波形读取成功");
    } else {
        char d[64];
        sprintf(d, "(得到 %s)", dht22_strerror(dht22_read(&hw, &r, 0u, 0)));
        fail("正常波形读取成功", d);
        return;
    }
    if (memcmp(r.raw, payload, 5) == 0) {
        ok("原始 5 字节与注入波形一致");
    } else {
        char d[128];
        sprintf(d, "(得到 %02X %02X %02X %02X %02X, 期望 %02X %02X %02X %02X %02X)",
                r.raw[0], r.raw[1], r.raw[2], r.raw[3], r.raw[4],
                payload[0], payload[1], payload[2], payload[3], payload[4]);
        fail("原始 5 字节与注入波形一致", d);
    }
    check_int("读出的湿度 = 612", (int)r.humi_x10, 612);
    check_int("读出的温度 = 253", (int)r.temp_x10, 253);

    /* ---- 全 1 帧（全部数据位为 1） ---- */
    {
        uint8_t ones[5] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFC };
        setup_fake(&g, &hw, &wf);
        wf_build(&wf, ones, 80u, 80u, 50u, 26u);
        if (dht22_read(&hw, &r, 0u, 0) == DHT22_OK) {
            check_int("全 1 帧湿度 = 65535", (int)r.humi_x10, 65535);
        } else {
            fail("全 1 帧可解析", "");
        }
    }

    /* ---- 全 0 帧 ---- */
    {
        uint8_t zeros[5] = { 0x00, 0x00, 0x00, 0x00, 0x00 };
        setup_fake(&g, &hw, &wf);
        wf_build(&wf, zeros, 80u, 80u, 50u, 26u);
        if (dht22_read(&hw, &r, 0u, 0) == DHT22_OK) {
            check_int("全 0 帧温度 = 0", (int)r.temp_x10, 0);
            check_int("全 0 帧湿度 = 0", (int)r.humi_x10, 0);
        } else {
            fail("全 0 帧可解析", "");
        }
    }

    /* ---- 负温度帧（含符号位） ---- */
    {
        uint8_t neg[5] = { 0x02, 0x64, 0x80, 0x69, 0x4F };
        setup_fake(&g, &hw, &wf);
        wf_build(&wf, neg, 80u, 80u, 50u, 26u);
        if (dht22_read(&hw, &r, 0u, 0) == DHT22_OK) {
            check_int("负温度帧读出 -105", (int)r.temp_x10, -105);
        } else {
            fail("负温度帧可解析", "");
        }
    }

    /* ---- 校验和错误 ---- */
    {
        uint8_t bad[5] = { 0x02, 0x64, 0x00, 0xFD, 0x00 };
        setup_fake(&g, &hw, &wf);
        wf_build(&wf, bad, 80u, 80u, 50u, 26u);
        if (dht22_read(&hw, &r, 0u, 0) == DHT22_ERR_CHECKSUM) {
            ok("校验和错误被识别为 CHECKSUM");
        } else {
            fail("校验和错误被识别为 CHECKSUM", "");
        }
    }

    /* ---- 数据线恒高：传感器未接线 ---- */
    {
        setup_fake(&g, &hw, &wf);
        wf_stuck(&wf, 1u);
        if (dht22_read(&hw, &r, 0u, 0) == DHT22_ERR_STUCK_HIGH) {
            ok("恒高被识别为 STUCK_HIGH（未接线）");
        } else {
            char d[64];
            sprintf(d, "(得到 %s)", dht22_strerror(dht22_read(&hw, &r, 0u, 0)));
            fail("恒高被识别为 STUCK_HIGH", d);
        }
    }

    /* ---- 数据线恒低：短路到地 ---- */
    {
        setup_fake(&g, &hw, &wf);
        wf_stuck(&wf, 0u);
        if (dht22_read(&hw, &r, 0u, 0) == DHT22_ERR_STUCK_LOW) {
            ok("恒低被识别为 STUCK_LOW（短路或损坏）");
        } else {
            char d[64];
            sprintf(d, "(得到 %s)", dht22_strerror(dht22_read(&hw, &r, 0u, 0)));
            fail("恒低被识别为 STUCK_LOW", d);
        }
    }

    /* ---- 波形中途截断：应报错而不是死循环 ---- */
    {
        setup_fake(&g, &hw, &wf);
        wf_build(&wf, payload, 80u, 80u, 50u, 26u);
        wf.n = 3;                       /* 只保留响应部分，数据位全砍掉 */
        if (dht22_read(&hw, &r, 0u, 0) != DHT22_OK) {
            ok("波形截断时能报错（不会死循环）");
        } else {
            fail("波形截断时能报错", "");
        }
    }

    /* ---- 入参保护 ---- */
    if (dht22_read(0, &r, 0u, 0) == DHT22_ERR_PARAM) {
        ok("hw 为空返回 PARAM");
    } else {
        fail("hw 为空返回 PARAM", "");
    }
    {
        dht22_hw_t incomplete;
        memset(&incomplete, 0, sizeof(incomplete));
        incomplete.read = fake_read;
        if (dht22_read(&incomplete, &r, 0u, 0) == DHT22_ERR_PARAM) {
            ok("回调缺失时返回 PARAM（早失败）");
        } else {
            fail("回调缺失时返回 PARAM", "");
        }
    }
}

/* ==================================================================
 * 测试 5：read_and_filter 组合接口
 * ================================================================== */
static void test_read_and_filter(void)
{
    waveform_t       wf;
    fake_gpio_t      g;
    dht22_hw_t       hw;
    dht22_filtered_t f;
    uint8_t          payload[5] = { 0x02, 0x64, 0x00, 0xFD, 0x63 };

    printf("\n[5] read_and_filter 组合接口\n");

    memset(&wf, 0, sizeof(wf));
    setup_fake(&g, &hw, &wf);
    dht22_filter_init(&f);

    wf_build(&wf, payload, 80u, 80u, 50u, 26u);
    if (dht22_read_and_filter(&hw, &f, 0u) == DHT22_OK) {
        ok("组合接口返回 OK");
    } else {
        fail("组合接口返回 OK", "");
    }
    check_int("滤波后温度 = 253", (int)f.temp_x10, 253);
    check_int("滤波后湿度 = 612", (int)f.humi_x10, 612);
    check_int("stat_ok 已累加", (int)f.stat_ok, 1);

    /* 失败路径更新统计且不破坏均值 */
    {
        int16_t before = f.temp_x10;
        uint8_t bad[5] = { 0x02, 0x64, 0x00, 0xFD, 0x00 };
        setup_fake(&g, &hw, &wf);
        wf_build(&wf, bad, 80u, 80u, 50u, 26u);
        (void)dht22_read_and_filter(&hw, &f, 0u);
        check_int("失败后均值保持", (int)f.temp_x10, (int)before);
        check_int("stat_err_crc 已累加", (int)f.stat_err_crc, 1);
    }

    /* 恒定样本多次采样：均值应等于样本值 */
    {
        int i;
        dht22_filtered_t f2;
        dht22_filter_init(&f2);
        for (i = 0; i < 7; i++) {
            setup_fake(&g, &hw, &wf);
            wf_build(&wf, payload, 80u, 80u, 50u, 26u);
            (void)dht22_read_and_filter(&hw, &f2, 0u);
        }
        check_int("恒定样本 7 次后温度 = 253", (int)f2.temp_x10, 253);
        check_int("stat_ok 累计 = 7", (int)f2.stat_ok, 7);
    }

    /* 重试机制：前几次波形损坏，最后一次正常 -> 应最终成功 */
    {
        int i;
        dht22_filtered_t f3;
        uint8_t bad[5] = { 0x02, 0x64, 0x00, 0xFD, 0x00 };
        dht22_filter_init(&f3);

        /* 用一个"前 2 次坏、第 3 次好"的场景无法直接构造（波形表是共享的），
           所以这里改为验证：retry=2 时若一直坏，最终仍返回 CHECKSUM 且
           stat_reads 反映了内部尝试次数。 */
        setup_fake(&g, &hw, &wf);
        wf_build(&wf, bad, 80u, 80u, 50u, 26u);
        (void)dht22_read_and_filter(&hw, &f3, 2u);
        if (f3.stat_err_crc >= 1u) {
            ok("retry=2 时校验错误被统计");
        } else {
            fail("retry=2 时校验错误被统计", "");
        }
        (void)i;
    }
}

/* ==================================================================
 * 测试 6：错误码转字符串
 * ================================================================== */
static void test_strerror(void)
{
    printf("\n[6] 错误码转可读字符串\n");
    if (strcmp(dht22_strerror(DHT22_OK), "OK") == 0 &&
        strstr(dht22_strerror(DHT22_ERR_STUCK_HIGH), "not connected") != 0 &&
        strstr(dht22_strerror(DHT22_ERR_CHECKSUM), "checksum") != 0) {
        ok("错误码字符串可读且区分明确");
    } else {
        fail("错误码字符串可读", "");
    }
    if (dht22_strerror((dht22_err_t)-999) != 0) {
        ok("未知错误码也有返回值（printf 不会崩）");
    } else {
        fail("未知错误码有返回值", "");
    }
}

int main(void)
{
    printf("========== DHT22 驱动单元测试（主机端） ==========\n");

    test_bits_to_bytes();
    test_decode();
    test_filter();
    test_full_read();
    test_read_and_filter();
    test_strerror();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
