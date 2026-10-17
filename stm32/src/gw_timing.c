/**
 * @file    gw_timing.c
 * @brief   时间预算与负载统计实现（纯逻辑）
 */
#include "gw_timing.h"

#include <string.h>

/* 每段的预算表：与 gw_phase_t 的顺序严格对应。
   用表而不是 switch，是为了加段时"漏了改"能被编译器发现
   （数组长度由 GW_PHASE_COUNT 固定，少一个初始化项会有警告）。 */
static const uint32_t s_budget_us[GW_PHASE_COUNT] = {
    GW_BUDGET_SLAVE_US,
    GW_BUDGET_MASTER_US,
    GW_BUDGET_DHT_US,
    GW_BUDGET_CAN_US,
    GW_BUDGET_UPLINK_US,
    GW_BUDGET_SYNC_US,
    GW_BUDGET_SLAVE_TX_US      /* 必须与 gw_phase_t 的顺序严格对应 */
};

/* 编译期检查：预算表长度必须与段数一致。
   这是"加段时忘了加预算"的防线 —— 运行时静态数组越界很难查。 */
_Static_assert(sizeof(s_budget_us) / sizeof(s_budget_us[0]) == GW_PHASE_COUNT,
               "预算表长度与 GW_PHASE_COUNT 不一致，加段后要同步补预算");

/* ==================================================================
 *  内部
 * ================================================================== */
static uint32_t now_us(const gw_timing_t *tm)
{
    if (tm->now_us == 0) {
        return 0u;
    }
    return tm->now_us(tm->ctx);
}

/* 结算"当前段"，把耗时累加进对应的统计 */
static void settle(gw_timing_t *tm)
{
    uint32_t t1;
    uint32_t dt;
    gw_phase_stat_t *st;

    if (tm->in_phase == 0u) {
        return;
    }
    if ((uint32_t)tm->cur >= (uint32_t)GW_PHASE_COUNT) {
        tm->in_phase = 0u;          /* 状态被破坏：静默复位而不是越界写 */
        return;
    }

    t1 = now_us(tm);
    /* 用无符号减法处理回绕。DWT 是 32 位计数器，
       72 MHz 下约 59.6 秒回绕一次 —— 必须用减法，不能用大小比较。 */
    dt = t1 - tm->cur_start_us;

    st = &tm->phase[tm->cur];

    st->calls++;
    st->total_us += dt;

    if (st->calls == 1u) {
        /* 第一次调用：min 必须直接赋值。
           若沿用"if (dt < min) min = dt"而 min 初值为 0，
           最小值永远是 0，统计就没意义了。 */
        st->min_us = dt;
    } else if (dt < st->min_us) {
        st->min_us = dt;
    }
    if (dt > st->max_us) {
        st->max_us = dt;
    }

    if (dt > s_budget_us[tm->cur]) {
        st->over_budget++;
    }

    tm->in_phase = 0u;
}

/* ==================================================================
 *  对外接口
 * ================================================================== */

int gw_timing_init(gw_timing_t *tm, uint32_t (*now_us_fn)(void *ctx), void *ctx)
{
    if (tm == 0) {
        return -1;
    }
    memset(tm, 0, sizeof(*tm));
    tm->now_us = now_us_fn;
    tm->ctx    = ctx;
    tm->inited = 1u;
    return 0;
}

void gw_timing_begin(gw_timing_t *tm, gw_phase_t ph)
{
    if (tm == 0 || tm->inited == 0u) {
        return;
    }
    if (tm->now_us == 0) {
        return;                     /* 没配时间源：整体不工作，不崩 */
    }
    if ((uint32_t)ph >= (uint32_t)GW_PHASE_COUNT) {
        return;
    }

    /* 先结算上一段（如果还开着）—— 调用方只需要在每段开头 begin 一次，
       不必记得先 end。少一个必须记住的步骤，就少一类 bug。 */
    settle(tm);

    tm->cur          = ph;
    tm->cur_start_us = now_us(tm);
    tm->in_phase     = 1u;
}

void gw_timing_end(gw_timing_t *tm)
{
    if (tm == 0 || tm->inited == 0u) {
        return;
    }
    settle(tm);
}

void gw_timing_loop_end(gw_timing_t *tm)
{
    uint32_t t;

    if (tm == 0 || tm->inited == 0u || tm->now_us == 0) {
        return;
    }

    /* 结算最后一段（例如 GW_PHASE_SYNC 之后直接结束循环） */
    settle(tm);

    t = now_us(tm);

    /* ⚠️ 第一轮**不能计入统计**，而且段统计也要一起清掉。
     *
     * 循环耗时的定义是"上一次 loop_end 到这一次 loop_end 的间隔"，
     * 这个间隔 = 忙碌时间 + 空闲时间。
     * 第一次调用 loop_end 时，`last_loop_end_us` 还是 init 时的 0，
     * 于是间隔只包含忙碌部分 —— **没有空闲**。
     *
     * 如果只跳过循环统计、却保留段统计，就会出现**分子分母口径不一致**：
     *   分子（段累计）包含了第一轮的忙碌时间
     *   分母（循环累计）不包含第一轮的间隔
     * 结果负载被算高。
     *
     * 实测过一次：每轮忙 2000µs、空闲 8000µs（真实负载 20%）时，
     * 口径不一致会算出 250‰，偏差 25% —— 而这个偏差恰好等于
     * "第一轮忙碌时间 / 统计窗口"的比例，不会随轮数增加而消失。
     *
     * 正确做法：第一轮边界时把**段统计一起清零** ——
     * init 到第一次 loop_end 之间的那段忙碌不属于任何有效测量窗口。 */
    if (tm->loop_seen == 0u) {
        uint8_t i;

        tm->loop_seen        = 1u;
        tm->last_loop_end_us = t;

        for (i = 0u; i < (uint8_t)GW_PHASE_COUNT; i++) {
            tm->phase[i].calls       = 0u;
            tm->phase[i].total_us    = 0u;
            tm->phase[i].max_us      = 0u;
            tm->phase[i].min_us      = 0u;
            tm->phase[i].over_budget = 0u;
        }
        return;
    }

    {
        uint32_t dt = t - tm->last_loop_end_us;

        tm->loop.loops++;
        tm->loop.total_us += dt;

        if (tm->loop.loops == 1u) {
            tm->loop.min_us = dt;
        } else if (dt < tm->loop.min_us) {
            tm->loop.min_us = dt;
        }
        if (dt > tm->loop.max_us) {
            tm->loop.max_us = dt;
        }
    }

    tm->last_loop_end_us = t;
}

void gw_timing_reset(gw_timing_t *tm)
{
    uint32_t (*fn)(void *) = 0;
    void     *ctx = 0;

    if (tm == 0 || tm->inited == 0u) {
        return;
    }

    /* ⚠️ 先结算已打开的那一段，再清零。
     *
     * 踩过的坑：最初直接 `memset(tm, ...)`，把 `in_phase` 也清成 0，
     * 但**没有结算**当时正开着的那一段。结果是：
     *   - 那一段的时间戳还留在 `cur_start_us` 里，下一次 begin 时
     *     它会被当成"上一段"结算，时长算成了"从 reset 前到 reset 后"的间隔
     *   - 于是统计里凭空多出一次调用，而且时长明显偏大
     *
     * 现象是"reset 之后第一次统计的 min 不是最小值" ——
     * 一个很容易被当成"统计精度问题"而忽略的偏差。
     *
     * 正确做法：reset 也要维护状态机的一致性 —— 有开着的段就先关掉。
     * 这一条与"任何清理动作都必须幂等且不漏状态"是同一个原则。 */
    settle(tm);

    /* 保留时间源配置，只清统计。
       同时把 `loop_seen` 与 `last_loop_end_us` 一起清掉 —— 这里踩过一次：
       如果只清统计而不清 `loop_seen`，模块仍认为"第一轮已见过"，
       于是 reset 之后的第一次 `loop_end` 走的是普通计数路径，
       而热身时的"清空段统计"已经发生过一次 ——
       结果是 **reset 之后第一条真实样本被丢掉**。
       实测现象是"跑 10 轮只统计到 9 次，且最小的那一次不见了"。

       语义上 reset 应当是"从现在开始重新测量"，所以它必须让模块
       回到与刚 init 时相同的状态。 */
    fn  = tm->now_us;
    ctx = tm->ctx;

    memset(tm, 0, sizeof(*tm));
    tm->now_us = fn;
    tm->ctx    = ctx;
    tm->inited = 1u;
}

const gw_phase_stat_t *gw_timing_phase(const gw_timing_t *tm, gw_phase_t ph)
{
    if (tm == 0 || (uint32_t)ph >= (uint32_t)GW_PHASE_COUNT) {
        return 0;
    }
    return &tm->phase[ph];
}

const gw_loop_stat_t *gw_timing_loop(const gw_timing_t *tm)
{
    return (tm == 0) ? 0 : &tm->loop;
}

uint32_t gw_timing_avg_us(const gw_timing_t *tm, gw_phase_t ph)
{
    const gw_phase_stat_t *st = gw_timing_phase(tm, ph);

    if (st == 0 || st->calls == 0u) {
        return 0u;
    }
    return st->total_us / st->calls;
}

uint32_t gw_timing_load_permil(const gw_timing_t *tm)
{
    uint32_t busy = 0u;
    uint32_t total;
    uint8_t  i;

    if (tm == 0 || tm->loop.total_us == 0u) {
        return 0u;
    }

    for (i = 0u; i < (uint8_t)GW_PHASE_COUNT; i++) {
        busy += tm->phase[i].total_us;
    }

    /* 上面的入口已判过 total_us != 0，这里直接用（cppcheck 指出
       原来那个 `if (total == 0)` 是恒假分支）。 */
    total = tm->loop.total_us;

    /* 用 64 位中间量避免 busy * 1000 在 32 位上溢出。
       例如 busy = 5,000,000µs（5 秒）时，乘 1000 是 5e9，超过 uint32。 */
    {
        uint64_t v = ((uint64_t)busy * 1000u) / (uint64_t)total;

        if (v > 1000u) {
            v = 1000u;              /* 夹到 100%：统计异常时不要报告 150% */
        }
        return (uint32_t)v;
    }
}

gw_phase_t gw_timing_worst_phase(const gw_timing_t *tm)
{
    gw_phase_t worst = GW_PHASE_SLAVE;
    uint32_t   best_max = 0u;
    uint8_t    i;

    if (tm == 0) {
        return GW_PHASE_SLAVE;
    }

    for (i = 0u; i < (uint8_t)GW_PHASE_COUNT; i++) {
        if (tm->phase[i].calls == 0u) {
            continue;
        }
        /* 比的是 **max**（最坏情况），不是 total 也不是平均。
           实时系统里决定优先级的是最坏情况。 */
        if (tm->phase[i].max_us > best_max) {
            best_max = tm->phase[i].max_us;
            worst    = (gw_phase_t)i;
        }
    }
    return worst;
}

void gw_timing_priority_hint(const gw_timing_t *tm, gw_phase_t *out)
{
    uint8_t i;
    uint8_t j;

    if (out == 0) {
        return;
    }
    for (i = 0u; i < (uint8_t)GW_PHASE_COUNT; i++) {
        out[i] = (gw_phase_t)i;
    }
    if (tm == 0) {
        return;
    }

    /* 简单插入排序：按 max_us 降序。
       段数只有 6，用 O(n²) 完全够，而且代码短、不容易写错。 */
    for (i = 1u; i < (uint8_t)GW_PHASE_COUNT; i++) {
        gw_phase_t key = out[i];
        uint32_t   key_max = tm->phase[key].max_us;

        j = i;
        while (j > 0u && tm->phase[out[j - 1u]].max_us < key_max) {
            out[j] = out[j - 1u];
            j--;
        }
        out[j] = key;
    }
}

const char *gw_timing_phase_str(gw_phase_t ph)
{
    switch (ph) {
    case GW_PHASE_SLAVE:  return "SLAVE";
    case GW_PHASE_MASTER: return "MASTER";
    case GW_PHASE_DHT:    return "DHT";
    case GW_PHASE_CAN:    return "CAN";
    case GW_PHASE_UPLINK: return "UPLINK";
    case GW_PHASE_SYNC:   return "SYNC";
    case GW_PHASE_SLAVE_TX: return "SLAVE_TX";
    default:              return "?";
    }
}
