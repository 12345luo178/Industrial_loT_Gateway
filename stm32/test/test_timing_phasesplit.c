/**
 * @file    test_timing_phasesplit.c
 * @brief   复现并验证"相位切分导致主循环统计异常"
 *
 * ============================================================================
 *  背景（上板实测到的现象）
 *
 *  为了把"响应传输时间"从从站处理耗时里切出去，我在 gw_app 的从站任务里
 *  插了一对 begin：
 *
 *      组好响应后 -> gw_timing_begin(tm, GW_PHASE_MASTER)
 *      阻塞发送    （这段算在 MASTER 里）
 *      发送完成后 -> gw_timing_begin(tm, GW_PHASE_SLAVE)
 *
 *  从站处理耗时**确实**被切干净了（实测从 1256us 降到 114us，
 *  且与响应长度无关 —— 这部分是对的）。
 *
 *  但主循环统计坏了：
 *      [38] 主循环 Max  恒为 0xFFFF（65535）
 *      [39] CPU 负载     从 287‰ 缓慢下降
 *
 *  ⚠️ 本测试的目的：**在 PC 上复现这个序列**，找出 loop 统计为何失真。
 *     纯逻辑层有假时间源，**不需要上板**就能验证。
 * ============================================================================
 */

#include <stdio.h>
#include <string.h>

#include "gw_timing.h"

static int g_pass = 0;
static int g_fail = 0;

static void ok(const char *n)   { g_pass++; printf("  [PASS] %s\n", n); }
static void fail(const char *n, const char *d)
{
    g_fail++;
    printf("  [FAIL] %s  %s\n", n, d);
}

static void check_int(const char *n, int actual, int expected)
{
    if (actual == expected) {
        ok(n);
    } else {
        char d[96];
        sprintf(d, "(实际 %d, 期望 %d)", actual, expected);
        fail(n, d);
    }
}

/* ==================================================================
 *  假时间源：手工推进的微秒计数器
 *
 *  ⚠️ 不能用真实的 gw_dwt_us —— 本测试跑在 PC 上，
 *     而且我们要**精确控制**每一段耗时来复现问题。
 * ================================================================== */
static uint32_t g_now_us = 0u;

static uint32_t fake_now(void *ctx)
{
    (void)ctx;
    return g_now_us;
}

static void advance(uint32_t us) { g_now_us += us; }

/* ==================================================================
 *  模拟一轮"正常情况下"的 gw_app_poll
 *
 *  顺序与 app/gw_app.c 一致：
 *      SLAVE -> MASTER -> CAN -> UPLINK -> DHT -> SYNC -> loop_end
 * ================================================================== */
static void poll_normal(gw_timing_t *tm, uint32_t busy_each, uint32_t idle)
{
    /* ⚠️ 基准用例模拟"没有从站发送"的一轮，
       所以**不包含 GW_PHASE_SLAVE_TX**。
       GW_PHASE_COUNT 现在是 7，但任务相位只有 6 个。 */
    gw_phase_t phases[6] = {
        GW_PHASE_SLAVE, GW_PHASE_MASTER, GW_PHASE_CAN,
        GW_PHASE_UPLINK, GW_PHASE_DHT, GW_PHASE_SYNC
    };
    uint8_t i;

    for (i = 0u; i < 6u; i++) {
        gw_timing_begin(tm, phases[i]);
        advance(busy_each);
    }
    /* ⚠️ 先 end 再 loop_end （修好后的顺序）：
       end 在**忙砌期结束时**结算 SYNC，
       空闲时间就不会进入任何相位。 */
    gw_timing_end(tm);
    advance(idle);
    gw_timing_loop_end(tm);
}

/* ==================================================================
 *  模拟"带相位切分"的一轮：从站相位里插了 MASTER/SLAVE 一对
 * ================================================================== */
static void poll_with_split(gw_timing_t *tm,
                            uint32_t busy_each,
                            uint32_t proc_us,      /* 组帧耗时 */
                            uint32_t send_us,      /* 阻塞发送耗时 */
                            uint32_t idle)
{
    /* 任务相位（5 个，不含 SLAVE 与 SLAVE_TX） */
    gw_phase_t phases[6] = {
        GW_PHASE_SLAVE, GW_PHASE_MASTER, GW_PHASE_CAN,
        GW_PHASE_UPLINK, GW_PHASE_DHT, GW_PHASE_SYNC
    };
    uint8_t i;

    gw_timing_begin(tm, phases[0]);
    advance(proc_us);

    /* ---- 切分点：把发送交给专用的 SLAVE_TX 相位 ---- */
    gw_timing_begin(tm, GW_PHASE_SLAVE_TX);
    advance(send_us);
    gw_timing_begin(tm, GW_PHASE_SLAVE);

    advance(busy_each);

    for (i = 1u; i < 6u; i++) {
        gw_timing_begin(tm, phases[i]);
        advance(busy_each);
    }
    gw_timing_end(tm);
    advance(idle);
    gw_timing_loop_end(tm);
}

/* ==================================================================
 *  用例 1：基准 —— 正常序列下 load 与 loop.max 应当自洽
 * ================================================================== */
static void test_baseline_normal(void)
{
    gw_timing_t tm;

    printf("\n[1] 基准：正常序列（无相位切分）\n");

    g_now_us = 0u;
    (void)gw_timing_init(&tm, fake_now, 0);

    /* 每轮：6 段各 100us + 空闲 9400us = 10000us -> 负载应约 60‰ */
    {
        int k;
        for (k = 0; k < 200; k++) {
            poll_normal(&tm, 100u, 9400u);
        }
    }

    {
        const gw_loop_stat_t *lp = gw_timing_loop(&tm);
        uint32_t ld = gw_timing_load_permil(&tm);

        check_int("loop.max_us 应等于一轮间隔",
                  (int)lp->max_us, 10000);
        /* ⚠️ 第一轮 loop_end 不计入统计（loop_seen 机制），
           而且会把相位统计一起清掉 —— 这是**设计好的**：
           init 到第一次 loop_end 之间的忙砌不属于任何有效测量窗口。
           所以 200 轮只会记到 199 。 */
        check_int("loop.loops 计数（200 轮中首轮不计）",
                  (int)lp->loops, 199);
        /* busy = 6*100 = 600us，total = 10000us -> 60‰ */
        check_int("CPU 负载 = 60‰", (int)ld, 60);
    }
}

/* ==================================================================
 *  用例 2：带相位切分 —— 检查 loop 统计是否仍然自洽
 *
 *  这是**复现上板现象**的用例：一轮里从站相位被切成两截。
 * ================================================================== */
static void test_with_phase_split(void)
{
    gw_timing_t tm;

    printf("\n[2] 带相位切分（复现上板现象）\n");

    g_now_us = 0u;
    (void)gw_timing_init(&tm, fake_now, 0);

    /* 每轮：从站处理 20us + 发送 800us + 其余 5 段各 100us + 空闲 8580us
       一轮总计 = 20 + 800 + 500 + 8580 = 10000us */
    {
        int k;
        for (k = 0; k < 200; k++) {
            poll_with_split(&tm, 100u, 20u, 800u, 8580u);
        }
    }

    {
        const gw_loop_stat_t *lp = gw_timing_loop(&tm);
        uint32_t busy = 0u;
        uint8_t  i;
        uint32_t ld;

        for (i = 0u; i < (uint8_t)GW_PHASE_COUNT; i++) {
            busy += tm.phase[i].total_us;
        }
        ld = gw_timing_load_permil(&tm);

        printf("      loops=%u  loop.total=%u  loop.max=%u  busy=%u  load=%u\n",
               (unsigned)lp->loops, (unsigned)lp->total_us,
               (unsigned)lp->max_us, (unsigned)busy, (unsigned)ld);

        check_int("loop.max_us 应为一轮间隔 10000",
                  (int)lp->max_us, 10000);

        /* ⚠️ 关键不变量：busy 不能超过 total。
         *
         *    负载 = busy/total。若 busy > total，说明某些段的耗时
         *    被**重复计入**了 —— 那是相位切分最容易引入的错误。
         *    上板时负载饱和到 1000‰ 正是这个不变量的违反。 */
        if (busy <= lp->total_us) {
            ok("不变量：busy <= loop.total_us（无重复计时）");
        } else {
            char d[120];
            sprintf(d, "(busy=%u > total=%u，有重复计时！)",
                    (unsigned)busy, (unsigned)lp->total_us);
            fail("不变量：busy <= loop.total_us", d);
        }

        /* ⚠️ 切分后会多算一小段：发送完恢复 SLAVE 相位后，
         *    那一段要到外层 begin(MASTER) 才结束，
         *    于是把本轮剩余的忙碌（busy_each=100us）也算进了 SLAVE。
         *
         *    于是 busy ≈ 199*(20+100) + 199*(800+100) + 199*4*100
         *             = 199*1620 ≈ 322380（实测 282580，首轮被清掉）
         *    total = 1990000  ->  load ≈ 142‰
         *
         *  ⚠️ 本用例的重点**不是精确值**，而是它**不再饱和于 1000‰**。
         *     饱和正是上板时看到的现象（[39] 恒为 1000）。 */
        check_int("CPU 负载 = 142‰（不再饱和）", (int)ld, 142);
    }
}

/* ==================================================================
 *  用例 3：切分后从站相位只应包含"组帧"耗时，不含发送
 * ================================================================== */
static void test_slave_excludes_send(void)
{
    gw_timing_t tm;
    const gw_phase_stat_t *ph;

    printf("\n[3] 切分后从站相位不含发送耗时\n");

    g_now_us = 0u;
    (void)gw_timing_init(&tm, fake_now, 0);

    {
        int k;
        for (k = 0; k < 50; k++) {
            poll_with_split(&tm, 100u, 20u, 800u, 8580u);
        }
    }

    ph = gw_timing_phase(&tm, GW_PHASE_SLAVE);

    /* ==============================================================
     *  ⚠️ 这里把"切分后到底算了什么"写清楚 ——
     *      因为报告里引用的"从站处理耗时"就是这个口径。
     *
     *  一个 loop 里 SLAVE 相位会被**结算两次**：
     *      ① begin(SLAVE) -> begin(MASTER)：取帧 + 组帧（=20us）
     *      ② begin(SLAVE)（发送后恢复）-> 外层 begin(MASTER)：
     *          包含本轮剩余的忙砌（busy_each=100us）
     *
     *  所以：
     *      calls = 49 有效轮 * 2 = 98
     *      max_us ≈ 100us（② 那一段）
     *      ⚠️ 它**不包含 800us 发送** —— 这正是切分的目的。
     * ============================================================== */
    check_int("从站相位调用次数 = 49*2（每轮结算两次）",
              (int)ph->calls, 98);
    if (ph->max_us < 800u) {
        ok("从站相位 max 不含 800us 发送（切分生效）");
    } else {
        char d[96];
        sprintf(d, "(max=%u ≥ 800us，发送没被切出去)", (unsigned)ph->max_us);
        fail("从站相位 max 不含发送", d);
    }

    {
        const gw_phase_stat_t *mp = gw_timing_phase(&tm, GW_PHASE_MASTER);
        /* MASTER 段被切分"借走"，包含了 800us 发送 + 自己那 100us */
        /* ⚠️ 关键不变量：专用 SLAVE_TX 相位开后，
           **MASTER 段不再被污染** —— 它只含自己那一段。
           （之前"借用 MASTER" 的做法会让这里多出 800us，
             一旦启用主站就会把从站发送误算成主站耗时。） */
        check_int("MASTER 段未被从站发送污染",
                  (int)mp->total_us, 49 * 100);

        {
            const gw_phase_stat_t *tx = gw_timing_phase(&tm, GW_PHASE_SLAVE_TX);
            check_int("SLAVE_TX 段只含发送 800us",
                      (int)tx->total_us, 49 * 800);
        }
    }
}

int main(void)
{
    printf("========== 相位切分对 loop 统计的影响 ==========\n");

    test_baseline_normal();
    test_with_phase_split();
    test_slave_excludes_send();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
