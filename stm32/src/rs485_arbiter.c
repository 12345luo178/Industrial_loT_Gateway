/**
 * @file    rs485_arbiter.c
 * @brief   RS485 半双工总线仲裁实现（纯逻辑，无任何硬件依赖）
 */
#include "rs485_arbiter.h"

#include <string.h>

/* ==================================================================
 *  ★ 仲裁的原子性钩子（**弱符号**；缺陷 Z，2026-10-11）
 *
 *  ⚠️ 本模块的每个"改状态"的函数都是**检查 -> 动作**两步：
 *
 *        rs485_arb_acquire():  r = decide(owner, role); ... owner = role;
 *        rs485_arb_enter_wait(): if (owner != MASTER) return -1; in_wait = 1;
 *
 *  裸机下这是安全的：主站与从站的两段代码在**同一个主循环里顺序执行**，
 *  单次调用中间不会被换走。
 *
 *  **RTOS 下不成立**（抢占式调度）：
 *
 *        [主站任务] decide(owner=NONE, MASTER) -> OK        <-- 在这里被抢占
 *        [从站任务] decide(owner=NONE, SLAVE)  -> OK -> owner=SLAVE，开始发送
 *        [主站任务] （继续）owner = MASTER，也开始发送
 *        ==> 总线上同时有两个发送者 -> 帧冲突/乱码
 *
 *  ⚠️ 这正是"半双工总线仲裁"要防的事，却因为少了原子性而漏掉。
 *     （诊断特征：从站侧帧错误率上升、主站超时重试，而两边自己都认为
 *       "我拿到了总线"。）
 *
 *  ⚠️ 做法：只把"决策 + 改状态"这一段用**临界区**包住（几条指令、约 1 µs），
 *     **不**包住整个收发过程 —— 因为从站"抢占主站等待窗口"是本模块的**设计语义**
 *     （`decide()` 里写得很清楚），用粗粒度互斥会把它一起锁死。
 *
 *  ⚠️ 裸机 / 主机测试里是空实现；RTOS 由 `port/gw_rtos.c` 给强定义
 *     （`taskENTER_CRITICAL()`）。⚠️ 临界区**可嵌套**，所以这里不必担心和
 *     调用方的临界区叠加。
 * ================================================================== */
__attribute__((weak)) void rs485_arb_guard_enter(void)
{
}

__attribute__((weak)) void rs485_arb_guard_exit(void)
{
}

/* 决策表的唯一实现。头文件里有一份同样的表，两边必须一致 ——
   测试会穷举所有 (owner, role) 组合，任何偏差都会被立刻发现。 */
static rs485_acq_result_t decide(rs485_role_t owner, rs485_role_t req)
{
    if (req == RS485_ROLE_SLAVE) {
        /* 从站：最高优先级，可以抢占主站的等待窗口 */
        return (owner == RS485_ROLE_SLAVE) ? RS485_ACQ_BUSY : RS485_ACQ_OK;
    }
    if (req == RS485_ROLE_MASTER) {
        /* 主站：只有在总线完全空闲时才能拿 */
        return (owner == RS485_ROLE_NONE) ? RS485_ACQ_OK : RS485_ACQ_BUSY;
    }
    return RS485_ACQ_PARAM;
}

void rs485_arb_init(rs485_arb_t *a)
{
    if (a == 0) {
        return;
    }
    memset(a, 0, sizeof(*a));
    a->owner = RS485_ROLE_NONE;
}

rs485_acq_result_t rs485_arb_acquire(rs485_arb_t *a, rs485_role_t role)
{
    rs485_acq_result_t r;

    if (a == 0) {
        return RS485_ACQ_PARAM;
    }

    /* ★ 决策 + 改状态必须原子（缺陷 Z）—— 见文件头的抢占时序说明 */
    rs485_arb_guard_enter();

    r = decide(a->owner, role);

    if (r == RS485_ACQ_OK) {
        /* 抢占的副作用：主站的等待窗口作废，必须一起清掉。
           否则主站会以为自己还在等响应，而实际上总线已经交给了从站 ——
           结果是主站永远等不到响应也不会重试。 */
        if (role == RS485_ROLE_SLAVE && a->owner == RS485_ROLE_MASTER) {
            if (a->in_wait != 0u) {
                a->in_wait = 0u;
                a->stat.wait_preempted++;
            }
            a->stat.slave_preempted++;
            a->stat.master_deferred++;
        }
        a->owner = role;
    } else if (r == RS485_ACQ_BUSY && role == RS485_ROLE_MASTER) {
        /* 主站拿不到：计一次"让路"，这是诊断"主站被饿死"的关键指标。
           若这个计数持续增长而从站 tx 不变，说明有别的从站在长期占线。 */
        a->stat.master_deferred++;
    }

    rs485_arb_guard_exit();

    return r;
}

void rs485_arb_release(rs485_arb_t *a)
{
    if (a == 0) {
        return;
    }
    rs485_arb_guard_enter();
    a->owner   = RS485_ROLE_NONE;
    a->in_wait = 0u;
    rs485_arb_guard_exit();
}

int rs485_arb_enter_wait(rs485_arb_t *a)
{
    int r = 0;

    if (a == 0) {
        return -1;
    }

    rs485_arb_guard_enter();

    if (a->owner != RS485_ROLE_MASTER) {
        /* 只有主站持权时才谈得上"等响应"。
           真出现说明调用时序错了（例如忘了先 acquire，或者已被从站抢占），
           此时不应静默接受 —— 静默会让主站永远等一个不存在的响应。 */
        r = -1;
    } else {
        a->in_wait = 1u;
        a->stat.wait_entered++;
    }

    rs485_arb_guard_exit();

    return r;
}

void rs485_arb_exit_wait(rs485_arb_t *a)
{
    if (a == 0) {
        return;
    }
    rs485_arb_guard_enter();
    a->in_wait = 0u;
    /* 等待结束即释放发送权（若仍由主站持有）。
       若已被从站抢占，则 owner 已是 SLAVE，这里不能动它。 */
    if (a->owner == RS485_ROLE_MASTER) {
        a->owner = RS485_ROLE_NONE;
    }
    rs485_arb_guard_exit();
}

int rs485_arb_in_wait(const rs485_arb_t *a)
{
    return (a != 0 && a->in_wait != 0u) ? 1 : 0;
}

rs485_role_t rs485_arb_owner(const rs485_arb_t *a)
{
    return (a == 0) ? RS485_ROLE_NONE : a->owner;
}

const rs485_arb_stat_t *rs485_arb_stats(const rs485_arb_t *a)
{
    return (a == 0) ? 0 : &a->stat;
}

const char *rs485_role_str(rs485_role_t role)
{
    switch (role) {
    case RS485_ROLE_SLAVE:  return "SLAVE";
    case RS485_ROLE_MASTER: return "MASTER";
    default:                return "NONE";
    }
}
