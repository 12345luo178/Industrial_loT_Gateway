/**
 * @file    gw_sched.c
 * @brief   任务划分与协作式调度实现（纯逻辑，可在 PC 上单元测试）
 */
#include "gw_sched.h"

#include <string.h>

/* ==================================================================
 *  内部：取时间
 * ================================================================== */
static uint32_t sched_now(const gw_sched_t *s)
{
    if (s->now_us == 0) {
        return 0u;
    }
    return s->now_us(s->time_ctx);
}

/* 毫秒时间源（RTOS 心跳判据用；没接则返回 0，判据自动退回 progress） */
static uint32_t sched_now_ms(const gw_sched_t *s)
{
    if (s->now_ms == 0) {
        return 0u;
    }
    return s->now_ms(s->now_ms_ctx);
}

/* ==================================================================
 *  内部：结算一次任务执行
 * ================================================================== */
static void note_run(gw_sched_t *s, gw_task_id_t id, uint32_t dt_us)
{
    gw_task_stat_t *st = &s->stat[id];

    st->runs++;
    st->total_us += dt_us;

    if (st->runs == 1u) {
        /* ⚠️ 第一次必须直接赋值。
           若沿用 `if (dt < min) min = dt` 而 min 初值为 0，
           最小值会永远停在 0，统计就没意义了。
           （这个坑在 gw_timing 里也踩过一次，这里保持一致的处理。） */
        st->min_us = dt_us;
    } else if (dt_us < st->min_us) {
        st->min_us = dt_us;
    }
    if (dt_us > st->max_us) {
        st->max_us = dt_us;
    }

    if (dt_us > (uint32_t)s->tasks[id].budget_us) {
        st->over_budget++;
    }

    /* 硬超时判定：只有**声明了** deadline 的任务才算。
       对没有硬超时要求的任务（例如主站轮询），
       "耗时超过 X" 只是性能问题，不是实时性失败 ——
       两者必须分开统计，否则报告里会混进一大堆无意义的"超时"。 */
    if (s->tasks[id].deadline_us > 0u
        && dt_us > (uint32_t)s->tasks[id].deadline_us) {
        st->deadline_miss++;
    }

    /* 只要任务跑过就算"有进展"，无论它本次是否做了实事。
       这一点很重要：喂狗检查的是"任务有没有被调度到"，
       而不是"任务有没有干活" —— 后者会随业务空闲而假报警
       （例如没有上位机请求时从站任务当然"没做事"）。 */
    s->progress++;

    /* ⚠️ 顺便登记心跳：万一裸机构建也接了毫秒时钟，心跳判据就能用
        **同一套字段**工作，不必为两种模型维护两套记账。 */
    if (s->now_ms != 0) {
        s->last_beat_ms[id] = sched_now_ms(s);
        s->beat_seen[id]    = 1u;
    }
}

/* ==================================================================
 *  对外接口
 * ================================================================== */

int gw_sched_init(gw_sched_t *s, const gw_task_def_t *defs,
                  uint32_t (*now_us)(void *ctx), void *time_ctx)
{
    uint8_t i;

    if (s == 0 || defs == 0) {
        return -1;
    }
    /* 每个任务都必须有任务体：少一个会导致空指针调用 */
    for (i = 0u; i < (uint8_t)GW_TASK_COUNT; i++) {
        if (defs[i].entry == 0) {
            return -2;
        }
    }

    memset(s, 0, sizeof(*s));

    for (i = 0u; i < (uint8_t)GW_TASK_COUNT; i++) {
        s->tasks[i] = defs[i];
        s->state[i] = GW_TASK_ST_IDLE;
        s->due_ms[i] = 0u;          /* 启动后立即到期 */
    }

    s->now_us   = now_us;
    s->time_ctx = time_ctx;
    s->inited   = 1u;
    return 0;
}

void gw_sched_start(gw_sched_t *s)
{
    uint8_t i;

    if (s == 0 || s->inited == 0u) {
        return;
    }
    /* ⚠️ 记下心跳判据的计时基准：还没登记过心跳的任务，
        以"启动时刻"为基准算它有没有迟到。
        必须在任务开始跑之前记，否则启动瞬间会被误判为"全部迟到"。 */
    s->wd_ref_ms = sched_now_ms(s);
    for (i = 0u; i < (uint8_t)GW_TASK_COUNT; i++) {
        if (s->state[i] == GW_TASK_ST_IDLE) {
            s->state[i] = GW_TASK_ST_READY;
        }
        /* 全部立即到期：让所有任务在第一次 run 时就跑一遍，
           避免"某个任务要等一个周期才开始"造成的启动延迟。 */
        s->due_ms[i] = 0u;
    }
}

uint8_t gw_sched_run(gw_sched_t *s, void *ctx, uint32_t elapsed_ms)
{
    uint8_t  i;
    uint8_t  ran = 0u;

    if (s == 0 || s->inited == 0u) {
        return 0u;
    }

    /* ---- 1) 统一推进各任务的到期计时 ----
       放在循环外统一处理，而不是在每个任务里各减一次：
       统一处理能保证"所有任务看到的是同一个 elapsed_ms"，
       不会因为循环中途 return 而让后面的任务少减一次。 */
    for (i = 0u; i < (uint8_t)GW_TASK_COUNT; i++) {
        if (elapsed_ms >= s->due_ms[i]) {
            s->due_ms[i] = 0u;
        } else {
            s->due_ms[i] = (uint32_t)(s->due_ms[i] - elapsed_ms);
        }
    }

    /* ---- 2) 按优先级顺序执行到期任务 ---- */
    for (i = 0u; i < (uint8_t)GW_TASK_COUNT; i++) {
        uint32_t t0;
        uint32_t t1;

        if (s->state[i] == GW_TASK_ST_SUSPEND) {
            continue;               /* 被挂起：跳过，且不计入 progress */
        }
        if (s->state[i] == GW_TASK_ST_IDLE) {
            continue;               /* 还没 start */
        }
        if (s->due_ms[i] != 0u) {
            continue;               /* 未到期 */
        }

        t0 = sched_now(s);
        s->state[i] = GW_TASK_ST_RUNNING;
        s->tasks[i].entry(ctx);
        s->state[i] = GW_TASK_ST_READY;
        t1 = sched_now(s);

        /* 无符号减法处理回绕（与 gw_timing 同样的理由） */
        note_run(s, (gw_task_id_t)i, t1 - t0);

        /* 装填下次到期时间 */
        s->due_ms[i] = s->tasks[i].period_ms;

        ran++;
    }

    s->loops++;
    return ran;
}

int gw_sched_watchdog_ok(gw_sched_t *s)
{
    int ok;

    if (s == 0 || s->inited == 0u) {
        return 0;                   /* 未初始化：不允许喂狗（保守） */
    }

    /* ---- 判据一：按任务心跳 + 截止时间（接了毫秒时钟时）----
     *
     * ⚠️⚠️ 这是 RTOS 下**唯一有意义**的判据（缺陷 U）：
     *    RTOS 下 `gw_sched_run()` 不被调用，`progress` 永远是 0，
     *    所以判据二会让喂狗**永远被拒**；而"应用层有没有挂调度器"
     *    又不能作为判据（那正是缺陷 U 的成因：没人挂 -> 无条件允许）。
     *
     * 判据：每个已启动、未挂起的任务，都必须在
     *       `period_ms + GW_SCHED_WD_SLACK_MS` 之内登记过心跳。
     * 无符号减法处理回绕（与 gw_sched_run 里算间隔同样的理由）。
     *
     * ⚠️⚠️⚠️ 缺陷 AD（2026-10-13 板上实测定位）：**读 base 与读 now 的
     *       先后顺序不能反**，否则会产生 0.2% 的**假拒喂**。
     *
     *   本函数在 RTOS 下由 `vApplicationIdleHook()` 调用 ——
     *   跑在**优先级最低的空闲任务**里，随时会被任何业务任务抢占。
     *
     *   旧写法是"函数开头取一次 now，再在循环里逐个读 last_beat_ms[i]"：
     *
     *       now  = sched_now_ms(s);              <-- 在这里取
     *       for (i...) {
     *           base = s->last_beat_ms[i];       <-- 在这里读
     *           if (now - base > limit) 拒喂;    <-- base 可能比 now 还新
     *       }
     *
     *   （⚠️ 上面这段故意不写注释定界符：写在块注释里会把注释提前结束掉，
     *     编译器接着就会报 stray '\342' 之类的一堆怪错 —— 已踩过。）
     *
     *   两次读之间被抢占时，任务会趁机登记心跳，于是
     *   `base = last_beat_ms[i]` 比 `now` 还新 —— 无符号减法**回绕**成
     *   ~42.9 亿，远超 limit -> 判成"迟到 49 天"-> 拒喂。
     *
     *   板上实测（GDB 在 return 0 处下断点抓到的那一刻）：
     *        now(r0)          = 1893985   ← 判据手里的"现在"
     *        xTickCount       = 1893999   ← 真实当前 tick
     *        last_beat_ms[4]  = 1893999   ← 比 now 新 14 ms（SENSOR 刚心跳过）
     *        now - base       = -14 -> 4294967282 > limit(1000) -> 假拒喂
     *
     *   而同一时刻**所有任务的真实年龄都 < 50 ms**（远小于 1000 ms 阈值），
     *   也就是说"拒喂"与"任务迟到"完全无关。
     *
     *   症状极具误导性：`s_feed_reject` 以 ~21 次/秒匀速增长
     *   （喂狗本身被调用 ~9900 次/秒，即 0.2%），既不是突发也不是卡死，
     *   而且**没有任何任务真的迟到** —— 查"哪个任务卡了"会一无所获。
     *
     *   修法：**先读 base、后读 now**。tick 只增不减，所以
     *   `now >= base` 恒成立，`now - base` 就是真实年龄，回绕不可能发生。
     *   （把 now 挪进循环、每个任务各取一次，顺带把窗口缩到最小。）
     *
     * ⚠️ 为什么值得修（它当前并不致命）：空闲钩子每秒被调 ~9900 次，
     *    漏掉 21 次无伤大雅。但它让 `gw_rtos_feed_rejected()` 这个
     *    **现场诊断接口彻底失真**（文档写着"非零说明有任务超时"），
     *    而且"靠调用频率高才不出事"本身就是隐患：一旦钩子调用变稀
     *    （例如将来把喂狗挪到别的任务），同样的 0.2% 就会变成真风险。 */
    if (s->now_ms != 0) {
        uint8_t i;

        ok = 1;
        for (i = 0u; i < (uint8_t)GW_TASK_COUNT; i++) {
            uint32_t base;
            uint32_t limit;
            uint32_t now;           /* ★ 每个任务单独取，且必须在 base 之后取 */

            if (s->state[i] == GW_TASK_ST_SUSPEND) {
                continue;           /* 主动挂起的任务：不该算它死 */
            }
            if (s->state[i] == GW_TASK_ST_IDLE) {
                continue;           /* 还没 start（理论上 start 后不会有） */
            }

            /* ★★ 顺序不能反（缺陷 AD）：先读 base，再读 now。
               反过来的话，被抢占期间登记的心跳会让 base 比 now 还新，
               无符号减法回绕成天文数字 -> 假拒喂。见上面那段说明。 */
            base  = (s->beat_seen[i] != 0u) ? s->last_beat_ms[i] : s->wd_ref_ms;
            now   = sched_now_ms(s);
            limit = (uint32_t)s->tasks[i].period_ms + GW_SCHED_WD_SLACK_MS;

            if ((uint32_t)(now - base) > limit) {
                ok = 0;
                break;
            }
        }
        return ok;
    }

    /* ---- 判据二：进展判据（裸机；行为与以前完全一致）----
     *
     * 判断依据："自上次喂狗以来，有没有任务被调度过"。
     *
     * ⚠️ 它的真实语义是"**完全停摆**才拒喂"（详见头文件里的说明）：
     *    5 个任务照跑、1 个任务死了，progress 照样增长 -> 仍然会喂。
     *    裸机下这个洞由"任务体在 gw_sched_run() 里顺序执行"盖住：
     *    某个任务死循环会让整个 gw_sched_run() 不返回。 */
    ok = (s->progress > 0u) ? 1 : 0;

    /* 无论允不允许，都清零 —— 语义是"消费掉这一轮的进展"。
       如果只在允许时清零，那么"卡住之后再恢复"会因为残留计数
       而多喂一次，掩盖刚发生过的问题。 */
    s->progress = 0u;

    return ok;
}

void gw_sched_set_clock_ms(gw_sched_t *s,
                           uint32_t (*now_ms)(void *ctx), void *ctx)
{
    if (s == 0 || s->inited == 0u) {
        return;
    }
    s->now_ms     = now_ms;
    s->now_ms_ctx = ctx;

    /* 设完立刻记一次基准：调用方约定在 gw_sched_start() 之前调用，
       而 start() 也会再记一次（两次都记是无害的幂等操作）。 */
    s->wd_ref_ms = sched_now_ms(s);
}

void gw_sched_note_beat(gw_sched_t *s, gw_task_id_t id)
{
    if (s == 0 || s->inited == 0u) {
        return;
    }
    if ((uint32_t)id >= (uint32_t)GW_TASK_COUNT) {
        return;
    }
    s->last_beat_ms[id] = sched_now_ms(s);
    s->beat_seen[id]    = 1u;

    /* 心跳同样算"有进展"：这样即使没接毫秒时钟（判据二），
       登记过心跳的任务也不会被当成"什么都没干"。 */
    s->progress++;
}

void gw_sched_suspend(gw_sched_t *s, gw_task_id_t id)
{
    if (s == 0 || s->inited == 0u) {
        return;
    }
    if ((uint32_t)id >= (uint32_t)GW_TASK_COUNT) {
        return;
    }
    s->state[id] = GW_TASK_ST_SUSPEND;
}

void gw_sched_resume(gw_sched_t *s, gw_task_id_t id)
{
    if (s == 0 || s->inited == 0u) {
        return;
    }
    if ((uint32_t)id >= (uint32_t)GW_TASK_COUNT) {
        return;
    }
    if (s->state[id] == GW_TASK_ST_SUSPEND) {
        s->state[id] = GW_TASK_ST_READY;
        s->due_ms[id] = 0u;         /* 恢复后立即跑一次，不等一个周期 */
    }
}

gw_task_state_t gw_sched_state(const gw_sched_t *s, gw_task_id_t id)
{
    if (s == 0 || (uint32_t)id >= (uint32_t)GW_TASK_COUNT) {
        return GW_TASK_ST_IDLE;
    }
    return s->state[id];
}

const gw_task_stat_t *gw_sched_stat(const gw_sched_t *s, gw_task_id_t id)
{
    if (s == 0 || (uint32_t)id >= (uint32_t)GW_TASK_COUNT) {
        return 0;
    }
    return &s->stat[id];
}

const gw_task_def_t *gw_sched_def(const gw_sched_t *s, gw_task_id_t id)
{
    if (s == 0 || (uint32_t)id >= (uint32_t)GW_TASK_COUNT) {
        return 0;
    }
    return &s->tasks[id];
}

gw_task_id_t gw_sched_worst_task(const gw_sched_t *s)
{
    gw_task_id_t worst = GW_TASK_SLAVE;
    uint32_t     best = 0u;
    uint8_t      i;

    if (s == 0) {
        return GW_TASK_SLAVE;
    }
    for (i = 0u; i < (uint8_t)GW_TASK_COUNT; i++) {
        if (s->stat[i].runs == 0u) {
            continue;
        }
        /* 按 **max** 判而不是按 total/平均：
           实时系统里决定优先级的是最坏情况。
           （这条依据在 gw_timing 里已经验证过一次，
             这里保持同一原则。） */
        if (s->stat[i].max_us > best) {
            best  = s->stat[i].max_us;
            worst = (gw_task_id_t)i;
        }
    }
    return worst;
}

int gw_sched_deadlines_met(const gw_sched_t *s)
{
    uint8_t i;

    if (s == 0) {
        return 0;
    }
    for (i = 0u; i < (uint8_t)GW_TASK_COUNT; i++) {
        if (s->stat[i].deadline_miss > 0u) {
            return 0;
        }
    }
    return 1;
}

const char *gw_sched_task_name(gw_task_id_t id)
{
    switch (id) {
    case GW_TASK_SLAVE:  return "SLAVE";
    case GW_TASK_MASTER: return "MASTER";
    case GW_TASK_CAN:    return "CAN";
    case GW_TASK_UPLINK: return "UPLINK";
    case GW_TASK_SENSOR: return "SENSOR";
    case GW_TASK_HOUSE:  return "HOUSE";
        case GW_TASK_UI:     return "UI";
    default:             return "?";
    }
}

const char *gw_sched_state_str(gw_task_state_t st)
{
    switch (st) {
    case GW_TASK_ST_IDLE:    return "IDLE";
    case GW_TASK_ST_READY:   return "READY";
    case GW_TASK_ST_RUNNING: return "RUNNING";
    case GW_TASK_ST_SUSPEND: return "SUSPEND";
    case GW_TASK_ST_FAULT:   return "FAULT";
    default:                 return "?";
    }
}
