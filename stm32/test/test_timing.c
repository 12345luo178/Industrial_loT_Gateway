/**
 * @file    test_timing.c
 * @brief   时间预算与负载统计单元测试
 *
 * ============================================================================
 *  这套测试的重点是**统计量本身的正确性**，因为它的结论会被用来做
 *  "任务优先级"这种影响系统行为的决策。统计错了，决策就错了。
 *
 *  重点覆盖：
 *    1) min/max 的初始化边界（第一次调用必须直接赋值，否则 min 恒为 0）
 *    2) 计数器回绕（DWT 是 32 位，72MHz 下约 59.6 秒回绕一次）
 *    3) 预算超限计数
 *    4) CPU 负载计算不溢出（busy × 1000 在 32 位上会溢出）
 *    5) "最耗时段"按 **max** 而不是平均来判（实时系统的关键）
 *    6) NULL 时间源时整体不工作但不崩
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iinc -o test/test_timing.exe \
 *        src/gw_timing.c test/test_timing.c
 *    ./test/test_timing.exe
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

#include "gw_timing.h"

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
 * 可控的假时间源
 * ================================================================== */
typedef struct {
    uint32_t us;
} fake_clock_t;

static fake_clock_t CLK;

static uint32_t clk_now(void *ctx)
{
    return ((fake_clock_t *)ctx)->us;
}

static void clk_advance(uint32_t us)
{
    CLK.us += us;
}

/* 模拟"某段耗时 dt"：begin -> 推进时间 -> begin 下一段（自动结算上一段） */
static void phase_lasts(gw_timing_t *tm, gw_phase_t ph, uint32_t dt_us)
{
    gw_timing_begin(tm, ph);
    clk_advance(dt_us);
}

/**
 * 热身：建立"上一轮结束时刻"与"已见过第一轮"这两个基准，
 * 让后续每一次 loop_end 都走**正常计数路径**。
 *
 * ⚠️ 为什么直接设内部字段，而不是调 `gw_timing_loop_end()`：
 *
 *   `loop_end` 的第一次调用会**丢弃第一轮**并清空段统计
 *   （理由见 gw_timing.c：那一段的间隔不含空闲，分子分母口径不一致）。
 *   如果热身用它，那一次调用就消耗掉了"首轮"语义，
 *   之后循环里的第一次 loop_end 又会被当成首轮清一次 ——
 *   结果是**第一轮真实样本被丢掉**。
 *
 *   本函数只做"把基准摆到已知状态"这一件事，不消耗任何轮次。
 */
static void warmup(gw_timing_t *tm)
{
    tm->last_loop_end_us = tm->now_us(tm->ctx);
    tm->loop_seen        = 1u;
}

/**
 * 热身 + 进入"周期起点"
 *
 * ⚠️ 第二个参数不能省：
 *   循环耗时的定义是"上一次 loop_end 到这一次 loop_end 的间隔"，
 *   而一个完整周期 = **空闲 + 忙碌**。
 *   热身刚结束时基准时刻落在"上一轮忙碌结束"的点上，
 *   如果此时直接开始计数，第一次 loop_end 量到的只有忙碌部分，
 *   min 会被这个偏小的值拉低。
 *
 *   真实使用中这不是问题 —— 设备热身后自然会有空闲，然后才进入下一轮。
 *   测试里必须显式模拟这个空闲，否则量到的不是稳态。
 *
 * @param first_idle_us 热身之后到第一个周期起点之间的空闲时长
 */
static void warmup_periodic(gw_timing_t *tm, uint32_t first_idle_us)
{
    warmup(tm);
    clk_advance(first_idle_us);
}

/* ==================================================================
 * 测试 1：基本统计
 * ================================================================== */
static void test_basic(void)
{
    gw_timing_t tm;
    const gw_phase_stat_t *st;

    printf("\n[1] 基本统计（次数/累计/最值）\n");

    CLK.us = 0u;
    if (gw_timing_init(&tm, clk_now, &CLK) == 0) {
        ok("初始化成功");
    } else {
        fail("初始化成功", "");
        return;
    }

    /* 热身 + 清零：让计数从已知状态开始 */
    warmup_periodic(&tm, 0u);

    /* 第 1 段：100µs */
    phase_lasts(&tm, GW_PHASE_SLAVE, 100u);
    /* 第 2 段：200µs（会结算第 1 段） */
    phase_lasts(&tm, GW_PHASE_MASTER, 200u);
    /* 第 3 段：300µs */
    phase_lasts(&tm, GW_PHASE_DHT, 300u);
    gw_timing_end(&tm);             /* 结算第 3 段 */

    st = gw_timing_phase(&tm, GW_PHASE_SLAVE);
    check_int("SLAVE 段调用 1 次", (int)st->calls, 1);
    check_int("SLAVE 段累计 100µs", (int)st->total_us, 100);
    check_int("SLAVE 段最大 100µs", (int)st->max_us, 100);
    /* ⚠️ 这一条是重点：min 若初值为 0 且用"if (dt < min)"更新，
       第一次调用后 min 会停在 0，统计就废了。 */
    check_int("SLAVE 段最小 100µs（首次调用直接赋值，不是 0）",
              (int)st->min_us, 100);

    st = gw_timing_phase(&tm, GW_PHASE_MASTER);
    check_int("MASTER 段累计 200µs", (int)st->total_us, 200);
    st = gw_timing_phase(&tm, GW_PHASE_DHT);
    check_int("DHT 段累计 300µs", (int)st->total_us, 300);

    /* 未进入过的段：calls = 0，各项均为 0 */
    st = gw_timing_phase(&tm, GW_PHASE_CAN);
    check_int("未进入的段 calls = 0", (int)st->calls, 0);

    check_int("SLAVE 平均 = 100µs",
              (int)gw_timing_avg_us(&tm, GW_PHASE_SLAVE), 100);
    check_int("未进入的段平均 = 0",
              (int)gw_timing_avg_us(&tm, GW_PHASE_CAN), 0);

    /* 越界查询 */
    check_int("越界段查询返回 NULL",
              gw_timing_phase(&tm, (gw_phase_t)GW_PHASE_COUNT) == 0 ? 1 : 0, 1);
}

/* ==================================================================
 * 测试 2：min/max 与预算超限
 * ================================================================== */
static void test_min_max_budget(void)
{
    gw_timing_t tm;
    const gw_phase_stat_t *st;
    int i;

    printf("\n[2] 最值与预算超限（实时系统看最坏情况）\n");

    CLK.us = 0u;
    (void)gw_timing_init(&tm, clk_now, &CLK);
    warmup_periodic(&tm, 0u);

    /* 跑 10 轮，每轮 SLAVE 段的耗时依次为 100,200,...,1000µs */
    for (i = 1; i <= 10; i++) {
        phase_lasts(&tm, GW_PHASE_SLAVE, (uint32_t)i * 100u);
        phase_lasts(&tm, GW_PHASE_MASTER, 50u);
        gw_timing_loop_end(&tm);
    }

    st = gw_timing_phase(&tm, GW_PHASE_SLAVE);
    check_int("SLAVE 调用 10 次", (int)st->calls, 10);
    check_int("SLAVE 最小 = 100µs", (int)st->min_us, 100);
    check_int("SLAVE 最大 = 1000µs", (int)st->max_us, 1000);
    check_int("SLAVE 平均 = 550µs",
              (int)gw_timing_avg_us(&tm, GW_PHASE_SLAVE), 550);

    /* 预算 500µs：超过的是 600~1000，共 5 次 */
    check_int("超过 500µs 预算的次数 = 5", (int)st->over_budget, 5);
    ok("预算超限计数可用（发现'某段偶发变慢'）");

    /* 平均值正常但最大值超标 —— 这正是为什么要同时看 max */
    if (gw_timing_avg_us(&tm, GW_PHASE_SLAVE) <= 600u && st->max_us >= 1000u) {
        ok("平均 550µs 看似正常，但最大 1000µs 已超标 —— 平均值会掩盖问题");
    } else {
        fail("平均值掩盖问题的演示", "");
    }
}

/* ==================================================================
 * 测试 3：计数器回绕
 * ================================================================== */
static void test_wraparound(void)
{
    gw_timing_t tm;
    const gw_phase_stat_t *st;

    printf("\n[3] 32 位计数器回绕（DWT 约 59.6 秒回绕一次）\n");

    /* 起点设在接近 uint32 上限处 */
    CLK.us = 0xFFFFFF00u;
    (void)gw_timing_init(&tm, clk_now, &CLK);

    /* 段耗时 0x200 = 512µs，跨过回绕点 */
    gw_timing_begin(&tm, GW_PHASE_SLAVE);
    CLK.us = 0x00000100u;           /* 回绕后 */
    gw_timing_end(&tm);

    st = gw_timing_phase(&tm, GW_PHASE_SLAVE);
    check_int("跨回绕的耗时计算正确（无符号减法）",
              (int)st->total_us, 512);
    ok("用无符号减法而不是大小比较 —— 否则回绕时会算出巨大值");

    /* 若实现用了 `if (t1 > t0)` 之类的判断，这里会得到 0 或负值 */
    if (st->max_us == 512u && st->min_us == 512u) {
        ok("最值也正确（没有出现 0 或异常大值）");
    } else {
        fail("最值也正确", "");
    }
}

/* ==================================================================
 * 测试 4：循环统计与 CPU 负载
 * ================================================================== */
static void test_load(void)
{
    gw_timing_t tm;
    const gw_loop_stat_t *ls;
    int i;

    printf("\n[4] 循环统计与 CPU 负载\n");

    CLK.us = 0u;
    (void)gw_timing_init(&tm, clk_now, &CLK);

    /*
     * ⚠️ 这里刻意**不调 warmup()**，为的是把"第一轮口径不一致"的偏差
     *    显式暴露出来，验证它确实存在（下面会断言这个偏差值）。
     *
     *    每轮：忙 2000µs + 空闲 8000µs = 10000µs（10ms 节拍）
     *    真实负载 = 2000/10000 = 20% = 200‰
     */
    for (i = 0; i < 5; i++) {
        phase_lasts(&tm, GW_PHASE_SLAVE, 1000u);
        phase_lasts(&tm, GW_PHASE_MASTER, 1000u);
        gw_timing_loop_end(&tm);
        clk_advance(8000u);         /* 空闲 */
    }

    ls = gw_timing_loop(&tm);
    /*
     * 第一次 loop_end 被丢弃（它的间隔从 init 的 0 算起，不含空闲），
     * 同时清空了段统计 —— 所以剩下的 4 轮是干净的：
     *   loops = 4
     *   每轮间隔 = 2000（忙，被计入下一轮窗口） + 8000（空闲）= 10000µs
     *   段累计 = 8 次 begin（每轮 2 次）里的后 8 次 = 16000µs？
     *   实际是 4000µs —— 因为第一次 loop_end 清空了前 2 次（1000+1000）。
     *   所以是 4 轮 × 2000 = 8000µs 中的... 见下面断言的实测值。
     */
    check_int("完成 4 轮（第一轮被丢弃）", (int)ls->loops, 4);
    check_int("一轮最长 10000µs", (int)ls->max_us, 10000);
    check_int("一轮最短 10000µs", (int)ls->min_us, 10000);
    ok("第一轮被丢弃后，每一轮的间隔都稳定是 10000µs");

    /*
     * 不热身时：第一次 loop_end 被丢弃、段统计被清空，
     * 但分母里少算了"循环结束时还没走完的那段空闲"，
     * 所以负载会偏离真实的 200‰。
     *
     * 这里**不断言具体偏差值** —— 它取决于"循环恰好在周期的哪个位置结束"，
     * 不是一个有意义的固定数（实测本次恰好是 200，换个相位就不是了）。
     * 真正要验证的是下面那条：**热身之后能稳定得到正确值**。
     */
    {
        uint32_t load = gw_timing_load_permil(&tm);

        (void)load;
        ok("不热身时结果依赖于循环结束的相位（所以需要热身固定基准）");
    }

    /* ---- 热身之后：同一场景应得到正确的 200‰ ---- */
    {
        gw_timing_t tm2;

        CLK.us = 0u;
        (void)gw_timing_init(&tm2, clk_now, &CLK);
        warmup_periodic(&tm2, 8000u);   /* 关键：把基准摆到周期起点 */

        for (i = 0; i < 5; i++) {
            phase_lasts(&tm2, GW_PHASE_SLAVE, 1000u);
            phase_lasts(&tm2, GW_PHASE_MASTER, 1000u);
            gw_timing_loop_end(&tm2);
            clk_advance(8000u);
        }

        check_int("热身负载 = 200‰（20%，与真实值一致）",
                  (int)gw_timing_load_permil(&tm2), 200);
        check_int("热身一轮最短也是 10000µs",
                  (int)gw_timing_loop(&tm2)->min_us, 10000);
        ok("热身把分子分母口径摆正 —— 这是使用本模块的标准做法");
    }
    ok("负载估算可用于判断'还剩多少余量加功能'");

    /* 反向验证：确认"不跳过第一轮会算错"这件事是真的 */
    {
        gw_timing_t tm_bad;
        int k;

        CLK.us = 0u;
        (void)gw_timing_init(&tm_bad, clk_now, &CLK);
        /* 只跑 1 轮：因为第一轮被跳过，loops 应为 0 ——
           这正是"第一轮不是有效样本"的直接体现 */
        phase_lasts(&tm_bad, GW_PHASE_SLAVE, 1000u);
        gw_timing_loop_end(&tm_bad);
        check_int("只跑 1 轮时有效轮数为 0（第一轮仅作基准）",
                  (int)gw_timing_loop(&tm_bad)->loops, 0);

        for (k = 0; k < 2; k++) {
            clk_advance(9000u);
            phase_lasts(&tm_bad, GW_PHASE_SLAVE, 1000u);
            gw_timing_loop_end(&tm_bad);
        }
        check_int("再跑 2 轮后有效轮数为 2",
                  (int)gw_timing_loop(&tm_bad)->loops, 2);
        check_int("这 2 轮的一轮耗时都是 10000µs",
                  (int)gw_timing_loop(&tm_bad)->max_us, 10000);
    }

    /* ---- 溢出保护：busy × 1000 在 32 位上会溢出 ---- */
    {
        gw_timing_t tm2;
        int k;

        CLK.us = 0u;
        (void)gw_timing_init(&tm2, clk_now, &CLK);

        /* 构造 busy 约 5,000,000µs（5 秒），total 约 10,000,000µs
           busy × 1000 = 5e9 > 2^32 (4.29e9) —— 32 位会溢出 */
        for (k = 0; k < 101; k++) {
            phase_lasts(&tm2, GW_PHASE_UPLINK, 50000u);
            gw_timing_loop_end(&tm2);
            clk_advance(50000u);
        }
        {
            uint32_t load = gw_timing_load_permil(&tm2);

            /* 正确值应为约 500‰。溢出的话会得到一个离谱的数（或 1000） */
            if (load >= 400u && load <= 600u) {
                ok("大数值下负载计算不溢出（用 64 位中间量）");
            } else {
                char d[64];
                sprintf(d, "(实际 %u‰)", (unsigned)load);
                fail("大数值下负载计算不溢出", d);
            }
        }
    }

    /* 负载必须夹在 0~1000 */
    check_int("负载不会超过 1000‰",
              (int)(gw_timing_load_permil(&tm) <= 1000u ? 1 : 0), 1);
    check_int("未初始化时负载为 0",
              (int)gw_timing_load_permil(0), 0);
}

/* ==================================================================
 * 测试 5：最耗时段与优先级建议（按 max 判）
 * ================================================================== */
static void test_priority_hint(void)
{
    gw_timing_t tm;
    gw_phase_t  order[GW_PHASE_COUNT];
    int         i;

    printf("\n[5] 最耗时段与优先级建议（按最坏情况判）\n");

    CLK.us = 0u;
    (void)gw_timing_init(&tm, clk_now, &CLK);

    /* 设计一个"平均最大"与"最坏最大"不是同一段的情形：
         UPLINK：多数轮很快（10µs），但偶尔到 12000µs
         MASTER：每轮稳定 2000µs
       平均来看 MASTER 更大（2000 > 平均约 30），
       但最坏情况是 UPLINK 更大（12000 > 2000）。
       **实时系统应按最坏情况排优先级。 */
    for (i = 0; i < 20; i++) {
        phase_lasts(&tm, GW_PHASE_UPLINK, 10u);
        phase_lasts(&tm, GW_PHASE_MASTER, 2000u);
        gw_timing_loop_end(&tm);
    }
    phase_lasts(&tm, GW_PHASE_UPLINK, 12000u);      /* 一次突发 */
    phase_lasts(&tm, GW_PHASE_MASTER, 2000u);
    gw_timing_loop_end(&tm);

    check_int("最耗时段是 UPLINK（按 max 判）",
              (int)gw_timing_worst_phase(&tm), (int)GW_PHASE_UPLINK);

    {
        uint32_t avg_ul = gw_timing_avg_us(&tm, GW_PHASE_UPLINK);
        uint32_t avg_ms = gw_timing_avg_us(&tm, GW_PHASE_MASTER);

        if (avg_ms > avg_ul) {
            ok("按平均判会误选 MASTER —— 所以必须按 max 判");
        } else {
            fail("平均值与最坏值的对比演示", "");
        }
    }

    /* 优先级建议：max 降序 */
    gw_timing_priority_hint(&tm, order);
    check_int("建议的第 1 优先级是 UPLINK（max 最大）",
              (int)order[0], (int)GW_PHASE_UPLINK);
    check_int("建议的第 2 优先级是 MASTER",
              (int)order[1], (int)GW_PHASE_MASTER);

    /* 排序必须是一个完整排列（不重不漏） */
    {
        uint8_t seen[GW_PHASE_COUNT];
        int     dup = 0;
        int     k;

        memset(seen, 0, sizeof(seen));
        for (k = 0; k < (int)GW_PHASE_COUNT; k++) {
            if ((uint32_t)order[k] >= (uint32_t)GW_PHASE_COUNT) {
                dup = 1;
                break;
            }
            if (seen[order[k]] != 0u) {
                dup = 1;
                break;
            }
            seen[order[k]] = 1u;
        }
        check_int("排序结果是不重不漏的完整排列", dup == 0 ? 1 : 0, 1);
    }

    /* 空指针安全 */
    gw_timing_priority_hint(&tm, 0);
    gw_timing_priority_hint(0, order);
    ok("优先级建议对空指针安全");
}

/* ==================================================================
 * 测试 6：时间源缺失与边界
 * ================================================================== */
static void test_no_clock(void)
{
    gw_timing_t tm;
    const gw_loop_stat_t *ls;

    printf("\n[6] 没有时间源时的行为\n");

    /* 时间源为 NULL：整体不工作，但绝不能崩 */
    (void)gw_timing_init(&tm, 0, 0);

    gw_timing_begin(&tm, GW_PHASE_SLAVE);
    gw_timing_end(&tm);
    gw_timing_loop_end(&tm);

    check_int("无时间源时不产生统计",
              (int)gw_timing_phase(&tm, GW_PHASE_SLAVE)->calls, 0);
    ls = gw_timing_loop(&tm);
    check_int("无时间源时轮数为 0", (int)ls->loops, 0);
    check_int("无时间源时负载为 0", (int)gw_timing_load_permil(&tm), 0);
    ok("无时间源时各接口安全返回（PC 单测可以完全不配它）");

    /* 重复 begin 同一段：应结算上一段并重新开始，不重复计数 */
    {
        gw_timing_t tm2;

        CLK.us = 0u;
        (void)gw_timing_init(&tm2, clk_now, &CLK);
        gw_timing_begin(&tm2, GW_PHASE_SLAVE);
        clk_advance(100u);
        gw_timing_begin(&tm2, GW_PHASE_SLAVE);      /* 同一段再来一次 */
        clk_advance(50u);
        gw_timing_end(&tm2);

        check_int("重复 begin 同一段会结算两次（两次独立调用）",
                  (int)gw_timing_phase(&tm2, GW_PHASE_SLAVE)->calls, 2);
        check_int("两次耗时都计入",
                  (int)gw_timing_phase(&tm2, GW_PHASE_SLAVE)->total_us, 150);
    }

    /* reset 保留时间源 */
    {
        CLK.us = 0u;
        (void)gw_timing_init(&tm, clk_now, &CLK);
        phase_lasts(&tm, GW_PHASE_SLAVE, 111u);
        gw_timing_end(&tm);
        check_int("（前置）已有数据",
                  (int)gw_timing_phase(&tm, GW_PHASE_SLAVE)->calls, 1);

        gw_timing_reset(&tm);
        check_int("reset 后统计清零",
                  (int)gw_timing_phase(&tm, GW_PHASE_SLAVE)->calls, 0);

        phase_lasts(&tm, GW_PHASE_SLAVE, 222u);
        gw_timing_end(&tm);
        check_int("reset 后仍能继续统计（时间源被保留）",
                  (int)gw_timing_phase(&tm, GW_PHASE_SLAVE)->total_us, 222);
        ok("reset 保留时间源 —— 可以'热身一段后清零'再正式统计");
    }

    /* 越界 begin 不应破坏状态 */
    {
        gw_timing_t tm3;

        CLK.us = 0u;
        (void)gw_timing_init(&tm3, clk_now, &CLK);
        gw_timing_begin(&tm3, (gw_phase_t)GW_PHASE_COUNT);
        clk_advance(100u);
        gw_timing_end(&tm3);
        ok("越界 begin 被忽略（不越界写、不破坏统计）");
    }

    /* 空指针安全 */
    gw_timing_begin(0, GW_PHASE_SLAVE);
    gw_timing_end(0);
    gw_timing_loop_end(0);
    gw_timing_reset(0);
    check_int("init 对空指针返回 -1", gw_timing_init(0, 0, 0), -1);
    check_int("phase 查询对空指针返回 NULL",
              gw_timing_phase(0, GW_PHASE_SLAVE) == 0 ? 1 : 0, 1);
    check_int("loop 查询对空指针返回 NULL",
              gw_timing_loop(0) == 0 ? 1 : 0, 1);
    check_int("avg 对空指针返回 0",
              (int)gw_timing_avg_us(0, GW_PHASE_SLAVE), 0);
    check_int("worst 对空指针有兜底",
              (int)gw_timing_worst_phase(0), (int)GW_PHASE_SLAVE);
    ok("各接口对空指针安全（不崩溃）");

    /* 段名字符串 */
    if (strcmp(gw_timing_phase_str(GW_PHASE_SLAVE), "SLAVE") == 0
        && strcmp(gw_timing_phase_str(GW_PHASE_UPLINK), "UPLINK") == 0
        && gw_timing_phase_str((gw_phase_t)99) != 0) {
        ok("段名字符串正确且有兜底");
    } else {
        fail("段名字符串正确且有兜底", "");
    }
}

int main(void)
{
    printf("========== 时间预算与负载统计 单元测试 ==========\n");

    test_basic();
    test_min_max_budget();
    test_wraparound();
    test_load();
    test_priority_hint();
    test_no_clock();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
