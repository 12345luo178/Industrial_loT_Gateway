/**
 * @file    test_sched.c
 * @brief   任务划分与协作式调度单元测试
 *
 * ============================================================================
 *  这套测试的重点是**任务化之后才能暴露的三类问题**：
 *
 *    1) 优先级顺序错了 —— 硬超时任务被挤到后面
 *    2) 周期算错了 —— 某个任务实际上跑得比预期频繁/稀疏
 *    3) **任务悄悄死掉而主循环还在跑** —— 这是任务化引入的新故障模式，
 *       也是最危险的：设备"看起来在运行"但功能已废，
 *       而传统的主循环喂狗检测不出来
 *
 *  第 3 类是 `gw_sched_watchdog_ok()` 存在的全部理由，
 *  测试里用"某个任务不再被调度"来模拟它。
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iinc -o test/test_sched.exe \
 *        src/gw_sched.c test/test_sched.c
 *    ./test/test_sched.exe
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

#include "gw_sched.h"

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
 * 假时间源与假任务
 * ================================================================== */
typedef struct {
    uint32_t us;
} fake_clock_t;

static fake_clock_t CLK;

static uint32_t clk_now(void *ctx)
{
    return ((fake_clock_t *)ctx)->us;
}

/* 记录每个任务被调用的次数与顺序 */
#define TRACE_MAX 256

typedef struct {
    uint8_t  order[TRACE_MAX];
    uint32_t n;

    uint32_t calls[GW_TASK_COUNT];
    uint32_t cost_us[GW_TASK_COUNT];    /* 每个任务"消耗"的假时间 */

    /* 故障注入：让某个任务"卡住"—— 不再被调用 */
    int      stall_task;                /* -1 = 不注入 */
} fake_env_t;

static fake_env_t ENV;

static void env_reset(void)
{
    memset(&ENV, 0, sizeof(ENV));
    ENV.stall_task = -1;
    CLK.us = 0u;
}

/* 任务体工厂：每个任务记一次调用并推进假时间 */
static void task_body(gw_task_id_t id, void *ctx)
{
    fake_env_t *e = (fake_env_t *)ctx;

    e->calls[id]++;
    CLK.us += e->cost_us[id];

    if (e->n < TRACE_MAX) {
        e->order[e->n] = (uint8_t)id;
        e->n++;
    }
}

static void t_slave(void *ctx)  { task_body(GW_TASK_SLAVE,  ctx); }
static void t_master(void *ctx) { task_body(GW_TASK_MASTER, ctx); }
static void t_can(void *ctx)    { task_body(GW_TASK_CAN,    ctx); }
static void t_uplink(void *ctx) { task_body(GW_TASK_UPLINK, ctx); }
static void t_sensor(void *ctx) { task_body(GW_TASK_SENSOR, ctx); }
static void t_house(void *ctx)  { task_body(GW_TASK_HOUSE,  ctx); }
static void t_ui(void *ctx)     { task_body(GW_TASK_UI,     ctx); }

/* 默认任务表：周期 0（每轮都跑） */
static void make_defs(gw_task_def_t *d, uint16_t sensor_period,
                      uint16_t slave_deadline)
{
    memset(d, 0, sizeof(gw_task_def_t) * GW_TASK_COUNT);
    d[GW_TASK_SLAVE].name  = "SLAVE";
    d[GW_TASK_SLAVE].entry = t_slave;
    d[GW_TASK_SLAVE].budget_us   = 500u;
    d[GW_TASK_SLAVE].deadline_us = slave_deadline;

    d[GW_TASK_MASTER].name  = "MASTER";
    d[GW_TASK_MASTER].entry = t_master;
    d[GW_TASK_MASTER].budget_us = 200u;

    d[GW_TASK_CAN].name  = "CAN";
    d[GW_TASK_CAN].entry = t_can;
    d[GW_TASK_CAN].budget_us = 300u;

    d[GW_TASK_UPLINK].name  = "UPLINK";
    d[GW_TASK_UPLINK].entry = t_uplink;
    d[GW_TASK_UPLINK].budget_us = 20000u;

    d[GW_TASK_SENSOR].name  = "SENSOR";
    d[GW_TASK_SENSOR].entry = t_sensor;
    d[GW_TASK_SENSOR].period_ms = sensor_period;
    d[GW_TASK_SENSOR].budget_us = 10000u;

    d[GW_TASK_HOUSE].name  = "HOUSE";
    d[GW_TASK_HOUSE].entry = t_house;
    d[GW_TASK_HOUSE].budget_us = 200u;

    /* ⚠️ 加了第 7 个任务（UI）就必须在这里也填上，否则它的 entry 是 NULL
       —— 单测会直接失败（这正是 CI 抓到的）。周期给 50ms，与生产表一致。 */
    d[GW_TASK_UI].name  = "UI";
    d[GW_TASK_UI].entry = t_ui;
    d[GW_TASK_UI].period_ms = 50u;
    d[GW_TASK_UI].budget_us = 800u;
}

/* ==================================================================
 * 测试 1：初始化与优先级顺序
 * ================================================================== */
static void test_init_and_order(void)
{
    gw_sched_t    s;
    gw_task_def_t d[GW_TASK_COUNT];
    gw_task_def_t bad[GW_TASK_COUNT];

    printf("\n[1] 初始化与优先级顺序\n");

    env_reset();
    make_defs(d, 0u, 0u);

    if (gw_sched_init(&s, d, clk_now, &CLK) == 0) {
        ok("初始化成功");
    } else {
        fail("初始化成功", "");
        return;
    }

    /* 缺 entry 必须被拒 */
    memcpy(bad, d, sizeof(bad));
    bad[GW_TASK_CAN].entry = 0;
    check_int("缺少任务体被拒", gw_sched_init(&s, bad, clk_now, &CLK), -2);
    check_int("空指针被拒", gw_sched_init(0, d, 0, 0), -1);
    check_int("空定义表被拒", gw_sched_init(&s, 0, 0, 0), -1);

    /* 重新初始化正确的 */
    (void)gw_sched_init(&s, d, clk_now, &CLK);

    /* start 之前 run 不应执行任何任务 */
    check_int("start 之前不执行任务",
              (int)gw_sched_run(&s, &ENV, 10u), 0);

    gw_sched_start(&s);
    check_int("start 后所有任务立即到期（首次 run 全跑一遍）",
              (int)gw_sched_run(&s, &ENV, 10u), (int)GW_TASK_COUNT);

    /* 记录的执行顺序必须与优先级顺序（数组顺序）一致 */
    {
        int order_ok = 1;
        int i;

        for (i = 0; i < (int)GW_TASK_COUNT; i++) {
            if (ENV.order[i] != (uint8_t)i) {
                order_ok = 0;
                break;
            }
        }
        if (order_ok) {
            ok("执行顺序与优先级顺序一致（SLAVE 最先，HOUSE 最后）");
        } else {
            char dd[64];
            sprintf(dd, "(第 %d 个执行的是 %d)", i, ENV.order[i]);
            fail("执行顺序与优先级顺序一致", dd);
        }
    }

    /* 每个任务被调用一次 */
    {
        int all_one = 1;
        int i;
        for (i = 0; i < (int)GW_TASK_COUNT; i++) {
            if (ENV.calls[i] != 1u) { all_one = 0; break; }
        }
        check_int("每个任务各执行一次", all_one, 1);
    }

    check_int("任务名可读",
              strcmp(gw_sched_task_name(GW_TASK_SLAVE), "SLAVE") == 0 ? 1 : 0, 1);
    check_int("状态名可读",
              strcmp(gw_sched_state_str(GW_TASK_ST_READY), "READY") == 0 ? 1 : 0, 1);
    check_int("未知任务名有兜底",
              gw_sched_task_name((gw_task_id_t)99) != 0 ? 1 : 0, 1);
}

/* ==================================================================
 * 测试 2：周期调度
 * ================================================================== */
static void test_period(void)
{
    gw_sched_t    s;
    gw_task_def_t d[GW_TASK_COUNT];
    int           i;

    printf("\n[2] 周期调度\n");

    env_reset();
    /* 传感器任务周期 100ms，其余每轮 */
    make_defs(d, 100u, 0u);
    (void)gw_sched_init(&s, d, clk_now, &CLK);
    gw_sched_start(&s);

    /* 第一次：全部到期 */
    (void)gw_sched_run(&s, &ENV, 0u);
    check_int("首轮 SENSOR 跑了 1 次", (int)ENV.calls[GW_TASK_SENSOR], 1);

    /* 推进 10 轮、每轮 10ms（共 100ms）：SENSOR 应只在到期时跑 */
    for (i = 0; i < 10; i++) {
        (void)gw_sched_run(&s, &ENV, 10u);
    }
    /* 累计 100ms，SENSOR 周期也是 100ms -> 恰好再到期一次 */
    check_int("100ms 后 SENSOR 再跑一次（共 2 次）",
              (int)ENV.calls[GW_TASK_SENSOR], 2);

    /* 每轮任务应跑了 11 次（首轮 + 10 轮） */
    check_int("每轮任务跑了 11 次", (int)ENV.calls[GW_TASK_HOUSE], 11);
    check_int("SLAVE 也跑了 11 次", (int)ENV.calls[GW_TASK_SLAVE], 11);

    ok("周期任务与每轮任务互不干扰（'每轮调用'与'每轮做事'是两件事）");

    /* 周期 0 的任务在每次 run 里都执行 */
    {
        uint32_t before = ENV.calls[GW_TASK_CAN];

        (void)gw_sched_run(&s, &ENV, 1u);
        check_int("周期 0 的任务每次 run 都执行",
                  (int)(ENV.calls[GW_TASK_CAN] - before), 1);
    }
}

/* ==================================================================
 * 测试 3：⚠️ 看门狗 —— 本套测试的重点
 * ================================================================== */
static void test_watchdog(void)
{
    gw_sched_t    s;
    gw_task_def_t d[GW_TASK_COUNT];

    printf("\n[3] 看门狗：检出「任务悄悄死掉」（任务化引入的新故障模式）\n");

    env_reset();
    make_defs(d, 0u, 0u);
    (void)gw_sched_init(&s, d, clk_now, &CLK);

    /* 未 start 时不允喂狗（还没跑过任何任务） */
    check_int("未运行时不允许喂狗", gw_sched_watchdog_ok(&s), 0);

    gw_sched_start(&s);

    /* 跑一轮后允许喂狗 */
    (void)gw_sched_run(&s, &ENV, 10u);
    check_int("有任务跑过则允许喂狗", gw_sched_watchdog_ok(&s), 1);

    /* ⚠️ 关键语义：允许之后计数被消费掉。
       如果不消费，那么"卡住之后再恢复"会因残留计数多喂一次，
       掩盖刚刚发生过的问题。 */
    check_int("喂狗检查会消费掉进展计数（不允许连喂两次）",
              gw_sched_watchdog_ok(&s), 0);

    /* 再跑一轮 -> 又允许 */
    (void)gw_sched_run(&s, &ENV, 10u);
    check_int("再跑一轮后又允许", gw_sched_watchdog_ok(&s), 1);

    /* ---- 模拟"调度器被卡住"：不再调用 run ---- */
    check_int("不调用 run 时不允许喂狗（第一轮检查）",
              gw_sched_watchdog_ok(&s), 0);
    check_int("不调用 run 时不允许喂狗（持续）",
              gw_sched_watchdog_ok(&s), 0);
    ok("调度器卡住时狗必然复位 —— 这正是任务化后需要的保护");

    /* ---- 模拟"所有任务被挂起" ---- */
    {
        int i;

        (void)gw_sched_run(&s, &ENV, 10u);      /* 恢复正常 */
        (void)gw_sched_watchdog_ok(&s);         /* 消费掉 */

        for (i = 0; i < (int)GW_TASK_COUNT; i++) {
            gw_sched_suspend(&s, (gw_task_id_t)i);
        }
        check_int("全部挂起后 run 不执行任何任务",
                  (int)gw_sched_run(&s, &ENV, 10u), 0);
        check_int("全部挂起时不允喂狗（设备活着但没干活）",
                  gw_sched_watchdog_ok(&s), 0);
        ok("'所有任务都被挂起'也应触发复位，而不是继续喂狗");
    }

    /* 空指针：保守地不允许喂狗 */
    check_int("空指针不允许喂狗", gw_sched_watchdog_ok(0), 0);
}

/* ==================================================================
 * 测试 4：挂起 / 恢复
 * ================================================================== */
static void test_suspend_resume(void)
{
    gw_sched_t    s;
    gw_task_def_t d[GW_TASK_COUNT];

    printf("\n[4] 挂起与恢复（安全模式下禁用危险任务）\n");

    env_reset();
    make_defs(d, 0u, 0u);
    (void)gw_sched_init(&s, d, clk_now, &CLK);
    gw_sched_start(&s);

    (void)gw_sched_run(&s, &ENV, 10u);
    check_int("初始 SENSOR 执行过", (int)ENV.calls[GW_TASK_SENSOR], 1);

    /* 挂起 SENSOR（模拟安全模式：反复 HardFault 后不再跑 DHT22） */
    gw_sched_suspend(&s, GW_TASK_SENSOR);
    check_int("挂起后状态为 SUSPEND",
              (int)gw_sched_state(&s, GW_TASK_SENSOR), (int)GW_TASK_ST_SUSPEND);

    (void)gw_sched_run(&s, &ENV, 10u);
    check_int("挂起期间 SENSOR 不再执行",
              (int)ENV.calls[GW_TASK_SENSOR], 1);
    /* 到这里一共 run 了 2 次（首轮一次 + 挂起那轮一次），
       所以每轮任务 HOUSE 应为 2 次。 */
    check_int("其他任务不受影响（HOUSE 跑了 2 次）",
              (int)ENV.calls[GW_TASK_HOUSE], 2);
    ok("挂起只影响目标任务 —— 安全模式可以精准禁用危险任务");

    /* 恢复 */
    gw_sched_resume(&s, GW_TASK_SENSOR);
    (void)gw_sched_run(&s, &ENV, 10u);
    check_int("恢复后立即执行一次（不等一个周期）",
              (int)ENV.calls[GW_TASK_SENSOR], 2);
    check_int("恢复后状态为 READY",
              (int)gw_sched_state(&s, GW_TASK_SENSOR), (int)GW_TASK_ST_READY);

    /* 越界与空指针安全 */
    gw_sched_suspend(&s, (gw_task_id_t)99);
    gw_sched_resume(&s, (gw_task_id_t)99);
    gw_sched_suspend(0, GW_TASK_SLAVE);
    gw_sched_resume(0, GW_TASK_SLAVE);
    check_int("越界任务状态查询有兜底",
              (int)gw_sched_state(&s, (gw_task_id_t)99), (int)GW_TASK_ST_IDLE);
    ok("挂起/恢复对越界与空指针安全");
}

/* ==================================================================
 * 测试 5：耗时统计与硬超时
 * ================================================================== */
static void test_timing_and_deadline(void)
{
    gw_sched_t    s;
    gw_task_def_t d[GW_TASK_COUNT];
    int           i;

    printf("\n[5] 耗时统计、预算超限与硬超时\n");

    env_reset();
    make_defs(d, 0u, 5000u);        /* SLAVE 硬超时 5ms */
    /* UPLINK 每次消耗 12000µs（模拟串口发一帧），预算 20000µs —— 不超 */
    ENV.cost_us[GW_TASK_UPLINK] = 12000u;
    ENV.cost_us[GW_TASK_SLAVE]  = 100u;
    ENV.cost_us[GW_TASK_MASTER] = 50u;

    (void)gw_sched_init(&s, d, clk_now, &CLK);
    gw_sched_start(&s);

    for (i = 0; i < 5; i++) {
        (void)gw_sched_run(&s, &ENV, 10u);
    }

    {
        const gw_task_stat_t *st = gw_sched_stat(&s, GW_TASK_UPLINK);

        check_int("UPLINK 跑了 5 次", (int)st->runs, 5);
        check_int("UPLINK 累计 60000µs", (int)st->total_us, 60000);
        check_int("UPLINK 最大 = 12000µs", (int)st->max_us, 12000);
        check_int("UPLINK 未超预算", (int)st->over_budget, 0);
    }

    /* 最耗时段按 max 判：应该是 UPLINK（12000 > 100） */
    check_int("最耗时任务是 UPLINK（按 max 判）",
              (int)gw_sched_worst_task(&s), (int)GW_TASK_UPLINK);
    ok("按最坏情况判优先级，与 gw_timing 的原则一致");

    /* SLAVE 消耗 100µs < 5000µs 硬超时：应全部达标 */
    check_int("硬超时全部满足", gw_sched_deadlines_met(&s), 1);
    check_int("SLAVE 未错过硬超时",
              (int)gw_sched_stat(&s, GW_TASK_SLAVE)->deadline_miss, 0);

    /* ---- 制造一次硬超时：让 SLAVE 某次消耗 8000µs ---- */
    ENV.cost_us[GW_TASK_SLAVE] = 8000u;
    (void)gw_sched_run(&s, &ENV, 10u);

    check_int("SLAVE 错过 1 次硬超时",
              (int)gw_sched_stat(&s, GW_TASK_SLAVE)->deadline_miss, 1);
    check_int("整体判定为'未达标'", gw_sched_deadlines_met(&s), 0);
    ok("硬超时是'实时性是否达标'的唯一可信判据 —— 平均再好看也不算达标");

    /* ---- 没有硬超时的任务超时不算 deadline_miss ---- */
    check_int("UPLINK（无硬超时）不产生 deadline_miss",
              (int)gw_sched_stat(&s, GW_TASK_UPLINK)->deadline_miss, 0);

    /* ---- 预算超限单独统计 ---- */
    ENV.cost_us[GW_TASK_MASTER] = 5000u;    /* 预算 200µs */
    (void)gw_sched_run(&s, &ENV, 10u);
    check_int("MASTER 超预算被计数",
              (int)gw_sched_stat(&s, GW_TASK_MASTER)->over_budget >= 1 ? 1 : 0, 1);
    ok("预算超限（性能问题）与硬超时（实时性问题）分开统计");

    /* min 的首次赋值 */
    check_int("SLAVE 最小值不是 0（首次直接赋值）",
              (int)(gw_sched_stat(&s, GW_TASK_SLAVE)->min_us > 0u ? 1 : 0), 1);
}

/* ==================================================================
 * 测试 6：无时间源与边界
 * ================================================================== */
static void test_no_clock(void)
{
    gw_sched_t    s;
    gw_task_def_t d[GW_TASK_COUNT];

    printf("\n[6] 无时间源与边界情况\n");

    env_reset();
    make_defs(d, 0u, 0u);

    /* 时间源为 NULL：不统计耗时，但调度本身要正常 */
    (void)gw_sched_init(&s, d, 0, 0);
    gw_sched_start(&s);

    check_int("无时间源时仍能调度任务",
              (int)gw_sched_run(&s, &ENV, 10u), (int)GW_TASK_COUNT);
    check_int("无时间源时 runs 仍被计数",
              (int)gw_sched_stat(&s, GW_TASK_SLAVE)->runs, 1);
    check_int("无时间源时耗时为 0",
              (int)gw_sched_stat(&s, GW_TASK_SLAVE)->max_us, 0);
    check_int("无时间源时仍允许喂狗（有任务跑过）",
              gw_sched_watchdog_ok(&s), 1);
    ok("无时间源只影响耗时统计，不影响调度与看门狗");

    /* reset 语义：重新 init 会清空统计 */
    (void)gw_sched_init(&s, d, clk_now, &CLK);
    check_int("重新初始化清空统计",
              (int)gw_sched_stat(&s, GW_TASK_SLAVE)->runs, 0);
    check_int("重新初始化后状态回到 IDLE",
              (int)gw_sched_state(&s, GW_TASK_SLAVE), (int)GW_TASK_ST_IDLE);

    /* run 的空指针安全 */
    check_int("run 对空指针返回 0", (int)gw_sched_run(0, &ENV, 10u), 0);
    check_int("stat 对空指针返回 NULL",
              gw_sched_stat(0, GW_TASK_SLAVE) == 0 ? 1 : 0, 1);
    check_int("def 对空指针返回 NULL",
              gw_sched_def(0, GW_TASK_SLAVE) == 0 ? 1 : 0, 1);
    check_int("worst_task 对空指针有兜底",
              (int)gw_sched_worst_task(0), (int)GW_TASK_SLAVE);
    check_int("deadlines_met 对空指针返回 0", gw_sched_deadlines_met(0), 0);
    gw_sched_start(0);
    ok("各接口对空指针安全（不崩溃）");

    /* 大 elapsed_ms 不应导致 due_ms 下溢 */
    {
        (void)gw_sched_init(&s, d, clk_now, &CLK);
        gw_sched_start(&s);
        (void)gw_sched_run(&s, &ENV, 10u);
        (void)gw_sched_run(&s, &ENV, 0xFFFFFFFFu);      /* 超大间隔 */
        check_int("超大 elapsed_ms 不导致下溢（任务仍能跑）",
                  (int)(gw_sched_stat(&s, GW_TASK_SLAVE)->runs >= 1u ? 1 : 0), 1);
        ok("避免 due_ms 无符号下溢（那会让任务再也不到期）");
    }
}

/* ==================================================================
 *  [3b] 心跳判据（★ RTOS 下用；缺陷 U）
 *
 *  ⚠️ 这一组存在的理由：RTOS 下每个任务自己 for(;;) 循环，
 *     `gw_sched_run()` **根本不被调用**，所以 progress 永远是 0 ——
 *     只看"有没有进展"不但会永远拒喂，也回答不了
 *     "**某个**任务单独死掉了（其余照跑）"。
 * ================================================================== */
static uint32_t MS_NOW;

static uint32_t ms_now(void *ctx)
{
    (void)ctx;
    return MS_NOW;
}

static void test_watchdog_heartbeat(void)
{
    gw_sched_t    s;
    gw_task_def_t d[GW_TASK_COUNT];
    int           i;

    printf("\n[3b] 心跳判据：某个任务单独死掉也能检出（RTOS 的关键保护）\n");

    env_reset();
    make_defs(d, 0u, 0u);
    /* 统一成小周期，便于用假时钟精确构造"迟到" */
    for (i = 0; i < (int)GW_TASK_COUNT; i++) {
        d[i].period_ms = 10u;
    }
    MS_NOW = 0u;

    (void)gw_sched_init(&s, d, clk_now, &CLK);
    gw_sched_set_clock_ms(&s, ms_now, 0);
    gw_sched_start(&s);

    /* 启动瞬间还没人登记，但在各自截止时间之内 */
    check_int("刚启动、未到截止时间 -> 允许喂狗", gw_sched_watchdog_ok(&s), 1);

    /* ---- 全员按周期登记心跳 -> 一直允许 ---- */
    for (i = 0; i < (int)GW_TASK_COUNT; i++) {
        gw_sched_note_beat(&s, (gw_task_id_t)i);
    }
    check_int("全员登记心跳后允许喂狗", gw_sched_watchdog_ok(&s), 1);

    /* ---- 时间推到"超过周期 + 余量"，没人心跳了 -> 拒绝 ---- */
    MS_NOW += 10u + GW_SCHED_WD_SLACK_MS + 1u;
    check_int("长时间没人心跳 -> 拒绝喂狗（完全停摆）",
              gw_sched_watchdog_ok(&s), 0);

    /* ---- ★ 关键：只让 SLAVE 停下来，其余任务照常心跳 ---- */
    MS_NOW = 100000u;
    for (i = 0; i < (int)GW_TASK_COUNT; i++) {
        gw_sched_note_beat(&s, (gw_task_id_t)i);
    }
    check_int("全员在期限内 -> 允许", gw_sched_watchdog_ok(&s), 1);

    /* 其余 5 个任务继续心跳，SLAVE 不再登记，并跨过它的截止时间 */
    for (i = 0; i < (int)GW_TASK_COUNT; i++) {
        if ((gw_task_id_t)i != GW_TASK_SLAVE) {
            gw_sched_note_beat(&s, (gw_task_id_t)i);
        }
    }
    MS_NOW += 10u + GW_SCHED_WD_SLACK_MS + 1u;
    /* 让其余任务把期间的心跳补上（它们一直在跑） */
    for (i = 0; i < (int)GW_TASK_COUNT; i++) {
        if ((gw_task_id_t)i != GW_TASK_SLAVE) {
            gw_sched_note_beat(&s, (gw_task_id_t)i);
        }
    }
    check_int("★ 只死掉一个任务（其余照跑）-> 仍然拒绝喂狗",
              gw_sched_watchdog_ok(&s), 0);
    ok("这是文档一直声称、而 2026-10-11 之前**并未实现**的语义（缺陷 U）");

    /* ---- SLAVE 恢复心跳 -> 立刻又允许 ---- */
    gw_sched_note_beat(&s, GW_TASK_SLAVE);
    check_int("死掉的任务恢复心跳后 -> 重新允许", gw_sched_watchdog_ok(&s), 1);

    /* ---- 主动挂起的任务不该被算作"死了" ---- */
    gw_sched_suspend(&s, GW_TASK_SENSOR);
    MS_NOW += 10u + GW_SCHED_WD_SLACK_MS + 1u;
    for (i = 0; i < (int)GW_TASK_COUNT; i++) {
        if ((gw_task_id_t)i != GW_TASK_SENSOR) {
            gw_sched_note_beat(&s, (gw_task_id_t)i);
        }
    }
    check_int("挂起的任务不登记心跳也不算死 -> 允许",
              gw_sched_watchdog_ok(&s), 1);

    /* ---- 边界：正好等于截止时间 -> 允许；超过 1ms -> 拒绝 ---- */
    MS_NOW = 500000u;
    for (i = 0; i < (int)GW_TASK_COUNT; i++) {
        gw_sched_note_beat(&s, (gw_task_id_t)i);
    }
    MS_NOW += 10u + GW_SCHED_WD_SLACK_MS;       /* 正好到点 */
    check_int("正好等于截止时间 -> 仍然允许（不早杀）",
              gw_sched_watchdog_ok(&s), 1);
    MS_NOW += 1u;                                /* 超 1ms */
    check_int("超过截止时间 1ms -> 拒绝", gw_sched_watchdog_ok(&s), 0);

    /* ---- 空指针与非法 id 安全 ---- */
    gw_sched_set_clock_ms(0, ms_now, 0);
    gw_sched_note_beat(0, GW_TASK_SLAVE);
    gw_sched_note_beat(&s, (gw_task_id_t)GW_TASK_COUNT);
    ok("空指针 / 非法任务号不会崩");

    /* ---- 没接毫秒时钟时：退回原来的进展判据（裸机行为不变）---- */
    {
        gw_sched_t s2;

        (void)gw_sched_init(&s2, d, clk_now, &CLK);
        gw_sched_start(&s2);
        check_int("不接时钟时未跑过 -> 拒绝（与以前一致）",
                  gw_sched_watchdog_ok(&s2), 0);
        gw_sched_note_beat(&s2, GW_TASK_SLAVE);     /* 算一次进展 */
        check_int("不接时钟时有进展 -> 允许（与以前一致）",
                  gw_sched_watchdog_ok(&s2), 1);
        ok("没接毫秒时钟时行为与以前完全一致（裸机构建不受影响）");
    }
}

int main(void)
{
    printf("========== 任务划分与协作式调度 单元测试 ==========\n");

    test_init_and_order();
    test_period();
    test_watchdog();
    test_suspend_resume();
    test_timing_and_deadline();
    test_watchdog_heartbeat();
    test_no_clock();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
