/**
 * @file    gw_app.c
 * @brief   应用层实现（纯逻辑：只依赖 vtable，不依赖 HAL，可在 PC 上单测）
 */
#include "gw_app.h"
#include "modbus_regs.h"
#include "gw_devinfo.h"

#include <string.h>

/* ==================================================================
 *  内部：主站需要的两个回调
 *
 *  这两个回调把主站与"物理总线"和"寄存器表"连接起来，
 *  但主站本身不知道它们背后是 RS485 还是别的总线 ——
 *  这正是把主站写成回调式接口的价值。
 * ================================================================== */

/* 主站的"发送请求"回调
 *
 * ============================================================================
 *  这里体现了总线仲裁的核心：**发之前必须先拿到总线**
 *
 *  主站的发送流程比从站复杂，因为它要"持有总线直到收到响应"：
 *
 *      申请总线  ->  发送  ->  进入等待窗口  ->  ...  ->  收到响应/超时后释放
 *      ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ 本回调负责这三步
 *
 *  返回非 0 表示"本轮没拿到总线"（被从站占着，或另一台主站在用）。
 *  这**不是错误** —— 主站可以等，下一轮 tick 会重发同一个请求。
 *  主站引擎收到非 0 后会按"发送失败"处理（消耗一次重试），
 *  所以这里要尽量把"没拿到总线"与"发送真的失败了"区分开：
 *  两者都由调用方转成非 0，但都不会丢数据（cur_index 不变）。
 * ============================================================================ */
static int app_master_send(void *ctx, const uint8_t *buf, uint16_t len)
{
    gw_app_t *app = (gw_app_t *)ctx;

    if (app == 0 || buf == 0 || len == 0u) {
        return -1;
    }
    if (app->port.rs485_send_held == 0) {
        /* 移植层没提供主站发送路径：退回从站发送接口。
           这样在只做从站的阶段（L4）也能用，只是没有仲裁保护。 */
        if (app->port.rs485_send == 0) {
            return -1;
        }
        return app->port.rs485_send(app->port.ctx, buf, len);
    }

    /* ---- 1) 申请总线 ----
       拿不到就如实返回失败，绝不"硬发" ——
       硬发会导致两个节点同时驱动总线，双方收到的都是回环，CRC 必错。

       ⚠️⚠️ 但**同一次事务内的重发**不要再申请一次（2026-10-10 上板实测抓到的缺陷）：
       主站超时重发时正处在"自己还持着总线"的状态（等响应期间不能放），
       而仲裁规则是"主站只有总线完全空闲才拿得到" —— 同一个 owner
       再申请一样返回 BUSY。于是重发帧永远发不出去，`max_retry` 形同虚设。

       实测现象（很好认）：总线上相邻请求帧的间隔**恒等于**
       "超时 + 轮询节拍"（1.30s = 300ms + 1000ms），
       而不会出现"重发帧在 300ms 后又出现一次"的成对形态。

       ⚠️ 判据必须是"**事务还没结束**"（`awaiting != 0`），**不能**用
          "当前是否持有总线"：一轮正常轮询结束（收到响应）到下一轮开始之间，
          总线可能还握在手里（要等应用层在轮询末尾释放）——
          用后者会把"两轮之间的正常请求"也当成重发，于是**总线永远不释放**，
          多主站总线上的其它节点再没机会说话。
          （这个坑当场被 test_app_bus 的 3 条断言抓到。）

       ⚠️ 为什么修在应用层而不是移植层：
         应用层自己知道"我处在同一次事务里"，移植层的"空闲才给"实现
         对多主站总线是正确的，不能为了这个场景放松。 */
    if (app->master.awaiting == 0u) {
        if (app->port.rs485_bus_acquire != 0
            && app->port.rs485_bus_acquire(app->port.ctx) != 0) {
            app->master_deferred++;
            return -1;
        }
    }

    /* ---- 1.5) 确认"确实持有的是主站身份" ----
     *
     * ⚠️ 这一步是 2026-10-10 加的，防的是一种**静默**的移植层接线错误：
     *     vtable 的 rs485_bus_acquire 配成申请**从站**角色，
     *     而 rs485_send_held 按**主站**身份发送。
     *
     *     这种配法在单测里完全看不出来（假移植层两边都返回成功），
     *     上板的表现是：主站每帧都在"发送"时 BUSY ——
     *     重试四万多次、一帧都没上总线，对外只看到"主站不工作"。
     *
     *     这里主动问一次"我持有的是主站吗"，把静默变成可计数的信号。
     *     移植层没提供该回调时不检查（保持向后兼容，不误报）。 */
    if (app->port.rs485_master_holds_bus != 0
        && app->port.rs485_master_holds_bus(app->port.ctx) == 0) {
        app->master_not_held++;
        if (app->port.rs485_release_master != 0) {
            (void)app->port.rs485_release_master(app->port.ctx);
        }
        return -1;
    }

    /* ---- 2) 发送（此时已持有总线） ---- */
    if (app->port.rs485_send_held(app->port.ctx, buf, len) != 0) {
        /* 发送失败：必须释放，否则总线被永久占用，整个网络都通信不了 */
        if (app->port.rs485_release_master != 0) {
            (void)app->port.rs485_release_master(app->port.ctx);
        }
        return -1;
    }

    /* ---- 3) 进入等待窗口 ----
       仲裁层靠这个标志才能在"收到针对本机的请求"时做出抢占决策。 */
    if (app->port.rs485_enter_wait != 0) {
        app->port.rs485_enter_wait(app->port.ctx);
    }
    return 0;
}

/* 主站的"写入寄存器表"回调。
 *
 * 越界检查放在这里而不是主站里：主站不需要知道本地寄存器表有多大，
 * 它只负责"把读到的值交给上层"。越界判定属于寄存器表的语义。 */
static int app_master_store(void *ctx, uint8_t area, uint16_t base,
                            const uint16_t *values, uint16_t qty)
{
    gw_app_t *app = (gw_app_t *)ctx;
    uint16_t  i;
    uint16_t *dst;
    uint16_t  limit;

    if (app == 0 || app->regs == 0 || values == 0) {
        return -1;
    }

    if (area == 0u) {
        dst   = app->regs->input;
        limit = MB_INPUT_REG_COUNT;
    } else {
        dst   = app->regs->holding;
        limit = MB_HOLDING_REG_COUNT;
    }

    /* 整体拒绝，而不是部分写入：部分写入会让上位机读到"一半新一半旧"的数据，
       比完全失败更难排查。这与从站引擎处理越界请求的策略保持一致。 */
    if ((uint32_t)base + (uint32_t)qty > (uint32_t)limit) {
        return -1;
    }

    for (i = 0u; i < qty; i++) {
        dst[base + i] = values[i];
    }
    return 0;
}

/* ==================================================================
 *  内部：检测并处理"主站等待窗口被从站抢占"
 *
 * ============================================================================
 *  为什么必须在主站推进**之前**做这件事
 *
 *  场景：主站发完请求，正占着总线等响应。此时上位机来读本机寄存器。
 *  仲裁层判定"从站有硬超时，主站可以等"，于是抢占等待窗口，
 *  让从站先把响应发出去。
 *
 *  抢占的后果是：总线的持有权被释放了（仲裁层里 owner 变成 SLAVE 再回到
 *  NONE），但**主站引擎并不知道**，它还以为自己在等响应，继续计时。
 *
 *  如果不管，会出现两个错误：
 *    1) 主站最终超时 -> 把"我们自己让出总线"记成**从机响应超时**，
 *       从机 fail_streak 增加，连续几次后一台正常的从机被判成离线
 *    2) 轮询项被跳过，那台从机这一轮的数据就丢了
 *
 *  正确做法：发现等待窗口没了，就放弃当前等待，
 *  但**保留轮询位置与重试预算** —— 下一轮 tick 重发同一个请求，
 *  对从机而言完全无感。
 *
 *  用 rs485_in_wait 回调来检测：它查询的是仲裁层里的真实状态，
 *  而不是应用层自己的推测。这样"是否被抢占"只有一个真相来源。
 * ================================================================== */
static void app_check_master_preempt(gw_app_t *app)
{
    if (app->port.rs485_in_wait == 0) {
        return;                     /* 移植层不支持检测，只能依赖超时路径 */
    }
    if (app->master.awaiting == 0u) {
        return;                     /* 主站本来就没在等 */
    }
    if (app->port.rs485_in_wait(app->port.ctx) != 0) {
        return;                     /* 等待窗口还在，没被抢占 */
    }

    /* ---- 确实被抢占了 ---- */
    if (mb_master_abort_pending(&app->master) == 0) {
        app->master_preempted++;

        /* 释放主站可能仍持有的总线。
           正常情况下仲裁层抢占时已经改过 owner，这里会返回 -1（无需释放），
           但显式调用一次是防御性的：万一移植层的抢占实现没清干净，
           不释放就会让总线永久占用 —— 那是最坏的结果。 */
        if (app->port.rs485_release_master != 0) {
            (void)app->port.rs485_release_master(app->port.ctx);
        }
    }
}

/* ==================================================================
 *  初始化
 * ================================================================== */
int gw_app_init(gw_app_t *app, mb_slave_t *slave, const gw_port_t *port)
{
    mb_master_err_t mret;

    if (app == 0 || slave == 0 || port == 0) {
        return -1;
    }
    if (port->rs485_take_frame == 0 || port->rs485_send == 0 || port->get_regs == 0) {
        /* 缺任一必需回调都无法工作，早失败好过运行到一半崩 */
        return -2;
    }

    memset(app, 0, sizeof(*app));

    app->slave = slave;
    app->port  = *port;

    /* 取寄存器表。若移植层还没准备好，直接失败。 */
    app->regs = port->get_regs(port->ctx);
    if (app->regs == 0) {
        return -3;
    }

    /* ---- 1) 数据管道 ---- */
    gw_pipeline_init(&app->pipe, GW_FW_VERSION_MAJOR, GW_FW_VERSION_MINOR);

    /* ---- 2) 主站 ---- */
    mret = mb_master_init(&app->master,
                          app_master_send, app,
                          app_master_store, app);
    if (mret != MB_M_OK) {
        return -4;
    }

    /* 默认时序：现场仪表的响应通常 <100ms；超时给 300ms 留余量，
       重试 1 次。这些值后续可由保持寄存器覆盖。 */
    (void)mb_master_set_timing(&app->master, 300u, 1u);

    /* ---- 3) 初始同步：让上位机第一次读就有确定值 ----
       不做这一步的话，寄存器表全是 0，现场无法区分
       "通信成功但无数据"和"根本没通信"。 */
    gw_pipeline_sync_regs(&app->pipe, app->regs);

    app->inited = 1u;
    return 0;
}

/* 前向声明：任务入口会调用它们，但定义在文件更靠后的位置。
   放在注释块之前，避免插进注释中间破坏它。 */
static void app_poll_uplink(gw_app_t *app, uint32_t elapsed_ms);
static void app_check_master_preempt(gw_app_t *app);

/* ==================================================================
 *  任务入口
 *
 *  ⚠️ 每个入口只接一个 `void *ctx`，没有 elapsed_ms 参数。
 *
 *  这是刻意的 —— 与 FreeRTOS 的 `TaskFunction_t`（`void (*)(void *)`）
 *  签名完全一致。这样上 RTOS 时可以直接：
 *
 *      xTaskCreate(gw_app_task_entry_slave, "slave", 256, app, 5, NULL);
 *
 *  **业务代码一行都不用改**。
 *
 *  代价是"本轮经过了多少毫秒"必须从别处拿 —— 存在 `app->tick_ms` 里，
 *  由 `gw_app_poll()` 在调度前写入。
 * ================================================================== */

/**
 * RS485 从站：应答上位机（**有硬超时，优先级最高**）
 *
 * 上位机的请求有硬超时要求（Modbus 主站通常等几十到几百毫秒），
 * 所以这个任务必须最先跑，且不能被采样或主站轮询挤到后面。
 */
void gw_app_task_entry_slave(void *ctx)
{
    gw_app_t *app = (gw_app_t *)ctx;
    uint8_t   frame[MB_MAX_ADU];
    uint16_t  flen = 0u;

    if (app == 0 || app->inited == 0u) {
        return;
    }

    /* 先检测主站等待窗口是否被抢占。
       必须在主站推进**之前**做：抢占发生后要先放弃等待，
       否则主站会在"已经失去总线"的状态下继续计时，
       最后把抢占算成从机超时（详见 app_check_master_preempt）。 */
    app_check_master_preempt(app);

    /* ⚠️ 刷新寄存器表里的"实时字段"（诊断、耗时统计等）。
     *
     * 必须在 take_frame 之前 —— 因为紧接着的 mb_slave_process()
     * 就会从 app->regs 里读数据组响应帧。
     *
     * ⚠️ 为什么不能放在 get_regs 回调里做这件事：
     *    get_regs 只在 gw_app_init() 时被调用一次（取指针），
     *    之后不再调。放在那里 = 只刷新一次 = Modbus 永远读到
     *    启动时的陈旧快照。这个坑上板排查时真踩过。 */
    if (app->port.refresh_regs != 0) {
        app->port.refresh_regs(app->port.ctx);
    }

    if (app->port.rs485_take_frame(app->port.ctx, frame,
                                   (uint16_t)sizeof(frame), &flen) == 0
        && flen > 0u) {
        app->slave_rx_frames++;

        /* ==============================================================
         *  ⚠️⚠️ 帧的归属派发 —— 主从共存的关键一步
         *
         *  踩过的坑（真实缺陷，上板才暴露）：
         *
         *     `mb_master_on_response()` **在生产代码里从来没被调用过**。
         *     全工程搜它只有两处：`modbus_master.c` 的定义，和 `test` 目录下
         *     手工喂响应的测试。**应用层一处都没有。**
         *
         *     后果：主站发完请求后没人把响应交给它，它只能一路超时重试；
         *     而超时期间**一直持有总线**（等响应时必须持权），
         *     于是从站也收不进来 —— 整条总线被自己锁死。
         *
         *     这就是"主站启用后从站不再应答、且从不发请求"的真因。
         *     ⚠️ PC 单测发现不了：测试是**手工调** mb_master_on_response 的，
         *        它恰好掩盖了"生产代码里没人调"这件事。
         *
         *  ------------------------------------------------------------------
         *  为什么派发判据是"主站是否在等"而不是"站号像谁"
         *
         *     `mb_master_on_response()` 内部会检查**站号是否等于当前
         *     轮询项的目标**，不符就返回错误码。所以：
         *
         *        主站在等 + 站号对得上 -> 这是主站的响应，收下
         *        主站在等 + 站号对不上 -> 不是给主站的，**必须继续交给从站**
         *
         *     ⚠️ 不能简单地把帧"要么给主站、要么给从站"二选一 ——
         *        那样从站的请求会在主站等待窗口里被吞掉。
         *        实测场景确实会发生：PC 在 STM32 主站等待期间发来请求。
         * ============================================================== */
        if (app->master.awaiting != 0u) {
            mb_master_err_t mr = mb_master_on_response(&app->master,
                                                       frame, flen);

            /* ACK：确实是主站等的那个响应，已经被消费掉了 */
            if (mr == MB_M_OK || mr == MB_M_ERR_EXCEPTION) {
                return;
            }
            /* 其它错误码（站号/CRC/功能码不符）说明这帧不是给主站的 ——
               落到下面交给从站引擎。
               ⚠️ 注意 mb_master_on_response 在返回错误码时**可能已经把
                  本次请求判失败**（那正是"收到一帧不合规的响应"的语义），
                  这对主站是正确的：它会重试。 */
        }

        /* 交给从站引擎：返回非 0 表示有响应要发 */
        if (mb_slave_process(app->slave, frame, flen) != 0) {
            /* ==========================================================
             *  ⚠️ 把"响应传输"从 GW_PHASE_SLAVE 里切出去
             *
             *  实测发现的：`rs485_send()` 是**阻塞发送**，
             *  而它就在 GW_PHASE_SLAVE 的测量区间内，于是
             *  "从站处理耗时"的 Max 被传输时间主导：
             *
             *      13 字节响应: 理论传输 1128us，实测 Max 1260us
             *      69 字节响应: 理论传输 5990us，实测 Max 6340us
             *
             *  这两个数字差 5 倍，混在一个指标里没法用 ——
             *  报告需要的是**处理耗时**（取帧 + 组帧），
             *  它跟帧长无关，才能反映实时性。
             *
             *  ------------------------------------------------------------------
             *  ⚠️ 曾经的做法（已废弃，记录在此以免重犯）：
             *     我最初把发送"借"给了 GW_PHASE_MASTER
             *     （发送前 begin(MASTER)、发送后 begin(SLAVE)）。
             *     从站指标确实切干净了，但**污染了 MASTER 的统计** ——
             *     L0 没有主站时看不出来，一旦启用主站，
             *     它的耗时里就混着从站的发送时间。
             *
             *     **借用别的段的相位是错的**：那不是"省一段"，
             *     而是把一个指标的错误转嫁给另一个指标。
             *
             *  正确做法：新开一段 GW_PHASE_SLAVE_TX 专门量传输。
             *  这样两个数都准确，而且含义清楚：
             *      SLAVE      -> 处理耗时（与帧长无关，实时性指标）
             *      SLAVE_TX   -> 传输耗时（与帧长成正比，阻塞时长证据）
             * ========================================================== */
            gw_timing_begin(app->timing, GW_PHASE_SLAVE_TX);

            /* 从站发送走 rs485_send：它内部会申请总线（从站优先级最高）、
               发送、然后立即释放。不需要跨调用持有。 */
            if (app->port.rs485_send(app->port.ctx,
                                     app->slave->tx_buf,
                                     app->slave->tx_len) == 0) {
                app->slave_tx_frames++;
            }

            gw_timing_begin(app->timing, GW_PHASE_SLAVE);
        }
    }
}

/**
 * Modbus 主站轮询（**无硬超时，可以等**）
 *
 * 主站是"状态机 + 无阻塞推进"，每轮推一下即可。
 * 它自己会判断超时、重试、离线退避。
 *
 * ⚠️ 总线释放的逻辑在这里，它是本项目踩坑最多的一处。
 *    完整的踩坑记录保留在 `gw_app.h` 末尾与 README 里，
 *    核心结论：**靠显式查询持权状态决策，不要猜。**
 */
void gw_app_task_entry_master(void *ctx)
{
    gw_app_t *app = (gw_app_t *)ctx;
    uint32_t  ms;

    if (app == 0 || app->inited == 0u) {
        return;
    }

    ms = (app->tick_ms > 0xFFFFu) ? 0xFFFFu : app->tick_ms;
    mb_master_poll(&app->master, (uint16_t)ms);

    /* ---- 总线释放：三种情况分别处理 ----
     *
     *   主站忙等响应时：持权 = 1，等待窗口 = 1  -> 保持，什么都不做
     *   收到响应后：    持权 = 1，等待窗口 = 1  -> 释放
     *   被从站抢占后：  持权 = 0，等待窗口 = 0  -> 无需再动
     *
     * 为什么不能"无条件每轮释放"：rs485_exit_wait 同时做
     * "清等待窗口"与"释放总线"两件事，在主站仍需要持权等响应时调它
     * 会把总线提前释放 —— 主站永远收不到响应。
     *
     * 为什么不能"只在不再等待时释放"：mb_master_poll 超时后会立刻
     * 推进并重新发请求（awaiting 又变回 1），那个条件永远不成立，
     * 于是总线永久泄漏、整个 RS485 网络瘫掉。 */
    if (app->master.awaiting != 0u) {
        /* 主站还在等响应：必须保持持权 */
    } else if (app->port.rs485_master_holds_bus != 0) {
        if (app->port.rs485_master_holds_bus(app->port.ctx) != 0) {
            /* 已结束等待但仍持权：正常释放。
               漏掉它总线就永久泄漏 —— 这是**最关键的清理点**。 */
            if (app->port.rs485_exit_wait != 0) {
                app->port.rs485_exit_wait(app->port.ctx);
            }
        }
        /* 已不持权：被从站抢占过，总线已由抢占方处理 */
    } else if (app->port.rs485_exit_wait != 0) {
        /* 移植层没提供持权查询：保守地"结束等待就释放"。
           可能多做一次无害的调用，但绝不会泄漏总线。 */
        app->port.rs485_exit_wait(app->port.ctx);
    }
}

/**
 * CAN 现场总线（**纯内存操作，阻塞特性最好**）
 *
 * 未启用 CAN 时只做一次状态同步（如实报告"不可用"）。
 */
void gw_app_task_entry_can(void *ctx)
{
    gw_app_t *app = (gw_app_t *)ctx;

    if (app == 0 || app->inited == 0u) {
        return;
    }

    if (app->can_core != 0 && app->can.tick != 0) {
        app->can.tick(app->can.ctx, app->tick_ms);
    }
    gw_app_sync_can_to_pipe(app);
}

/**
 * 上云链路（**唯一可能超过 10ms 节拍的任务**）
 *
 * 包含帧解析与可能的串口发送 —— 发 133 字节约 12ms。
 * 所以它的优先级不能排太高：一旦它占住 CPU，
 * 从站响应（有硬超时）就会被推迟。
 */
void gw_app_task_entry_uplink(void *ctx)
{
    gw_app_t *app = (gw_app_t *)ctx;

    if (app == 0 || app->inited == 0u) {
        return;
    }
    app_poll_uplink(app, app->tick_ms);
}

/**
 * DHT22 采样
 *
 * ⚠️ **为什么它必须单独成任务**：
 *   单总线时序要求采样期间**关中断约 5ms**。
 *   如果它和别的活塞在同一个任务里，那 5ms 会连带阻塞同任务内的
 *   其他工作（在 FreeRTOS 下表现为"同任务的其他事全被推迟"）。
 *   独立成任务之后，"关中断"的影响范围被限制在它自己身上。
 *
 * 顺带说明：本任务虽然每轮都被调用，但实际采样由
 * `gw_pipeline_take_sample_due()` 控制，仍是 2.5 秒一次 ——
 * **"每轮调用"与"每轮做事"是两件事**。
 */
void gw_app_task_entry_sensor(void *ctx)
{
    gw_app_t *app = (gw_app_t *)ctx;

    if (app == 0 || app->inited == 0u) {
        return;
    }

    /* 采样节拍推进放在本任务里而不是 house：
       它属于"采样节拍"的一部分，放一起更内聚。 */
    gw_pipeline_tick(&app->pipe, app->tick_ms);

    if (gw_pipeline_take_sample_due(&app->pipe) != 0) {
        app->dht_samples++;
        if (app->port.dht_sample != 0) {
            (void)app->port.dht_sample(app->port.ctx, &app->pipe.dht,
                                       GW_DHT_RETRY_DEFAULT);
        } else {
            /* 硬件层未注入（例如 DWT 初始化失败）：按采样失败处理，
               让状态位如实反映"这个传感器没数据"，而不是假装正常。 */
            app->pipe.dht.stat_reads++;
            if (app->pipe.dht.fail_streak < 0xFFu) {
                app->pipe.dht.fail_streak++;
            }
        }
    }
}

/**
 * 汇聚与看护（**必须排在最后**）
 *
 * 做三件事：
 *   1) 把各模块状态搬进管道
 *   2) 同步到寄存器表（让上位机下一轮读到最新值）
 *   3) 让时间预算统计结算这一轮
 *
 * ⚠️ **喂狗也属于这里**。它只有在前面所有任务都让路之后才会跑，
 *    所以"狗被喂了"就意味着"所有任务都跑过了"——
 *    这就是 gw_app_watchdog_ok() 能检出"某个任务卡死"的原理。
 */
void gw_app_task_entry_house(void *ctx)
{
    gw_app_t *app = (gw_app_t *)ctx;

    if (app == 0 || app->inited == 0u) {
        return;
    }

    gw_app_sync_master_to_pipe(app);
    gw_app_sync_slave_to_pipe(app);
    gw_app_sync_uplink_to_pipe(app);
    gw_pipeline_sync_regs(&app->pipe, app->regs);

    /* ==============================================================
     *  ⚠️ 必须先 end() 再 loop_end() —— 否则"空闲时间"会被算成忙
     *
     *  踩过的坑（PC 测试复现，`test_timing_phasesplit.c` 用例 1）：
     *
     *      `loop.total_us` 是"上一次 loop_end 到这一次 loop_end 的间隔"，
     *      它**包含空闲**（HAL_Delay、等待下一次 poll）。
     *      而各相位的 total_us 之和（busy）里，最后一段 `SYNC` 是
     *      **等到下一轮第一次 begin 才结算**的 —— 那时空闲已经过去了。
     *
     *      结果：SYNC 把空闲时间吸了进去，busy ≈ total，
     *      **CPU 负载恒等于 100%**。
     *
     *      实测现象：负载 1000‰（饱和），而且会随时间缓慢漂移。
     *      ⚠️ 这个缺陷在相位切分之前就存在，只是不明显；
     *         写 `test_timing_phasesplit.c` 时在基准用例里复现了。
     *
     *  修法：在这里 `end()` 一次，把 SYNC 在**忙碌期结束时就结算**，
     *        空闲时间就不会进入任何相位。
     * ============================================================== */
    gw_timing_end(app->timing);

    /* 一轮结束：记录本轮总耗时（间隔含空闲）。
       这一步是"CPU 负载"与"最坏一轮"数据的来源。
       ⚠️ 顺序不能反：end 必须在 loop_end 之前（见上面的说明）。 */
    gw_timing_loop_end(app->timing);
}

/* ==================================================================
 *  任务定义表
 *
 *  顺序即**优先级**（数组下标越小优先级越高），
 *  也与原来 gw_app_poll 里的固定执行顺序完全一致 ——
 *  所以"有调度器"与"无调度器"两条路径的行为相同。
 *
 *  预算（budget_us）来源：按 72MHz、10ms 节拍估算的合理上限。
 *
 *  ⚠️ 硬超时（deadline_us）**只给真正有硬性要求的任务填**。
 *     给所有任务都填会让"实时性达标"这个判据失去意义 ——
 *     主站轮询慢一点只是性能问题，不是实时性失败。
 * ================================================================== */
/* ⚠️ 界面任务的入口是**空实现**：真正干活在 gw_rtos_ui_tick 钩子里
   （RTOS 路径每轮按实测 dt 调），这样"多久刷一次"由周期决定。
   裸机路径不跑界面，所以这里空着是安全的。 */
static void gw_app_task_entry_ui(void *ctx) { (void)ctx; }

static const gw_task_def_t s_task_defs[GW_TASK_COUNT] = {
    /* name       entry                      period  budget   deadline */
    { "SLAVE",  gw_app_task_entry_slave,      0u,     500u,    5000u },
    { "MASTER", gw_app_task_entry_master,     0u,     200u,       0u },
    { "CAN",    gw_app_task_entry_can,        0u,     300u,       0u },
    { "UPLINK", gw_app_task_entry_uplink,     0u,   20000u,       0u },
    { "SENSOR", gw_app_task_entry_sensor,     0u,   10000u,       0u },
    { "HOUSE",  gw_app_task_entry_house,      0u,     200u,       0u },
    /* ⚠️ 只有 UI 填了真实周期（50ms）：它是唯一一个"多久刷一次"有意义的任务，
       填对周期也让心跳判据给出 period+1000ms 的合理窗口。 */
    { "UI",     gw_app_task_entry_ui,        50u,     800u,       0u }
};

const gw_task_def_t *gw_app_task_defs(void)
{
    return s_task_defs;
}

void gw_app_enable_sched(gw_app_t *app, gw_sched_t *sched)
{
    if (app == 0) {
        return;
    }
    app->sched = sched;

    /* ⚠️ 裸机下同一个调度器既是"分派者"也是"喂狗判据"，所以一起填。
        RTOS 下**只**填 wd_sched（见 gw_app_enable_watchdog_sched）。
        ⚠️ 只有传入非空时才跟着填 wd_sched：把 sched 置空（退出协作式调度）
           **不该**顺带把独立的喂狗判据也关掉 —— 那等于又给了一条
           "静默变成无条件喂狗"的路（缺陷 U 的同类）。 */
    if (sched != 0) {
        app->wd_sched = sched;
    }
}

void gw_app_enable_watchdog_sched(gw_app_t *app, gw_sched_t *sched)
{
    if (app == 0) {
        return;
    }
    /* ⚠️ 只动 wd_sched：故意**不**动 app->sched，
        否则 gw_app_poll() 会跑去看调度器分派任务，而 RTOS 下
        任务是由内核调度的 —— 两边都去推进同一个任务。 */
    app->wd_sched = sched;
}

int gw_app_watchdog_ok(gw_app_t *app)
{
    gw_sched_t *wd;

    if (app == 0 || app->inited == 0u) {
        return 0;
    }

    /* 判据来源：优先"只用于喂狗的"那个（RTOS 配置），
       其次才是裸机那个（既是分派也是判据）。 */
    wd = (app->wd_sched != 0) ? app->wd_sched : app->sched;

    if (wd != 0) {
        /* 两套判据都在这里，别在这层再写一套 ——
           否则迟早和 gw_sched 那边不一致。 */
        return gw_sched_watchdog_ok(wd);
    }

    /* 无调度器（顺序调用模型）：无条件允许。
       顺序模型下"poll 返回了"本身就意味着所有段都跑过了，
       不存在"某个任务悄悄死掉而主循环还在跑"的情况。
       ⚠️ RTOS 下**不该**走到这里：gw_rtos_create_tasks() 会把
          wd_sched 填上（缺陷 U 就是"没填 -> 无条件喂狗"）。 */
    return 1;
}

/* ==================================================================
 *  下行指令执行
 *
 *  ⚠️ 两条纪律：
 *
 *  1) **危险指令先验令牌，再做任何事。**
 *     顺序不能反 —— 如果先执行再验令牌，"误发一条 restore"
 *     就已经把参数清掉了。
 *
 *  2) **执行失败要如实回错误码，不能假装成功。**
 *     假装成功的后果是上位机以为参数已持久化、或者以为设备要重启了，
 *     而实际什么都没发生 —— 现场会按错误的假设继续排查，
 *     浪费的时间比直接报错多得多。
 * ================================================================== */

gw_cmd_result_t gw_app_exec_command(gw_app_t *app, const char *text,
                                    uint16_t len)
{
    gw_cmd_t        cmd;
    gw_cmd_result_t r;


    if (app == 0 || app->inited == 0u) {
        return GW_CMD_ERR_INTERNAL;
    }

    app->cmd_rx++;

    /* ---- 1) 解析（无副作用） ---- */
    r = gw_cmd_parse(text, len, &cmd);
    if (r != GW_CMD_OK) {
        goto fail;
    }

    /* ---- 2) 空指令：静默忽略，不计入统计 ----
       MQTT 上偶尔会有空消息，把它当成"错误"会让日志被噪声填满。 */
    if (cmd.id == GW_CMD_NONE) {
        app->cmd_rx--;                  /* 撤销刚才的计数 */
        return GW_CMD_OK;
    }

    /* ---- 3) ⚠️ 令牌校验必须在执行之前 ---- */
    r = gw_cmd_check_token(&cmd);
    if (r != GW_CMD_OK) {
        goto fail;
    }

    /* ---- 4) 执行 ---- */
    switch (cmd.id) {

    case GW_CMD_REPORT_NOW:
        /* 只置标志，不在这里直接发遥测 ——
           "什么时候发"属于上云策略，由上层决定。
           这样指令执行是纯粹的状态修改，没有阻塞风险。 */
        app->report_now = 1u;
        r = GW_CMD_OK;
        break;

    case GW_CMD_SET_PERIOD:
        /* ⚠️ 这里只校验**被改的这一个字段**，不做整表校验。
         *
         * 踩过的坑：最初写的是
         *     app->regs->holding[HREG_REPORT_PERIOD] = arg0;
         *     gw_params_from_regs(&p, app->regs);
         *     if (gw_params_validate(&p) != GW_PARAMS_OK) { 报错 }
         *
         * 结果是**在寄存器表部分未初始化时会误拒绝**：
         * 例如从站地址还没写过（值为 0），gw_params_validate 会
         * 因为"地址非法"判整个结构体不合法 —— 于是 `period 30`
         * 这个完全合法的指令被拒了。
         *
         * 这在实际场景里是会发生的：协议允许上位机只写需要改的寄存器，
         * 其余保持上电初值（0）。**改一个参数不该因为别的字段
         * 还没初始化而失败。**
         *
         * 正确的做法是按字段校验 —— 参数范围的定义仍然只有一份
         * （GW_CMD_PERIOD_MIN/MAX 与 gw_devinfo 的宏保持一致），
         * 只是不再做"整表体检"。 */
        if (cmd.arg0 < GW_CMD_PERIOD_MIN || cmd.arg0 > GW_CMD_PERIOD_MAX) {
            r = GW_CMD_ERR_RANGE;
            break;
        }
        app->regs->holding[HREG_REPORT_PERIOD] = (uint16_t)cmd.arg0;
        r = GW_CMD_OK;
        /* 不需要再同步别的地方：上报周期是**每轮从寄存器表读**的
           （见 app_poll_uplink 的说明），所以改完寄存器立刻就生效。
           这正是"寄存器表是唯一真相"带来的好处 ——
           参数只有一份，不存在"改了寄存器但某个副本没更新"。 */
        break;

    case GW_CMD_CLEAR_STICKY:
        {
            uint16_t mask = (uint16_t)(cmd.arg0 & 0xFFFFu);

            /* 只允许清"可清"的位：IWDG/HARDFAULT 是历史事件，
               清掉它们是运维的正当需求（例如排完障之后）。
               但 DEV_STATUS 里那些"当前状态"位不在 sticky 里，
               所以这里天然清不到它们。 */
            app->pipe.sticky_flags &= (uint16_t)(~mask);
        }
        r = GW_CMD_OK;
        break;

    case GW_CMD_GET_STATUS:
        /* 状态请求：不改任何东西，结果由 ACK 载荷带回去
           （调用方用 gw_app_make_cmd_reply 时把状态字作为 detail）。 */
        r = GW_CMD_OK;
        break;

    case GW_CMD_SET_UPLINK:
        app->uplink_enabled = (cmd.flag != 0u) ? 1u : 0u;
        r = GW_CMD_OK;
        break;

    case GW_CMD_SAVE:
        if (app->port.param_save == 0) {
            /* 移植层没提供：明确回"当前状态不允许"，
               而不是假装成功 —— 假装成功会让上位机
               以为参数已经持久化了。 */
            r = GW_CMD_ERR_STATE;
            break;
        }
        r = (app->port.param_save(app->port.ctx) == 0)
            ? GW_CMD_OK : GW_CMD_ERR_INTERNAL;
        break;

    case GW_CMD_RESTORE:
        if (app->port.param_restore == 0) {
            r = GW_CMD_ERR_STATE;
            break;
        }
        r = (app->port.param_restore(app->port.ctx) == 0)
            ? GW_CMD_OK : GW_CMD_ERR_INTERNAL;
        break;

    case GW_CMD_REBOOT:
        if (app->port.reboot == 0) {
            r = GW_CMD_ERR_STATE;
            break;
        }
        /* ⚠️ 先记录结果再调 reboot：
           reboot 实现会延时后复位，但它**可能不返回** ——
           如果指望返回后再统计，这条指令就永远统计不到。 */
        app->cmd_ok++;
        app->cmd_last_result = (uint8_t)GW_CMD_OK;
        app->cmd_last_id     = (uint8_t)cmd.id;
        app->report_now      = 0u;
        app->port.reboot(app->port.ctx);
        return GW_CMD_OK;               /* 正常情况下走不到这里 */

    default:
        r = GW_CMD_ERR_UNKNOWN;
        break;
    }

    /* ---- 5) 统计 ---- */
    app->cmd_last_result = (uint8_t)r;
    app->cmd_last_id     = (uint8_t)cmd.id;
    if (r == GW_CMD_OK) {
        app->cmd_ok++;
    } else {
        app->cmd_err++;
    }
    return r;

fail:
    app->cmd_err++;
    app->cmd_last_result = (uint8_t)r;
    app->cmd_last_id     = (uint8_t)cmd.id;
    return r;
}

void gw_app_enable_ota(gw_app_t *app, int (*handler)(void *, uint8_t,
                                                     const uint8_t *,
                                                     uint16_t),
                       void *ctx)
{
    if (app == 0) {
        return;
    }
    app->ota_handler = handler;
    app->ota_ctx     = ctx;
}

uint32_t gw_app_ota_frames(const gw_app_t *app)
{
    return (app == 0) ? 0u : app->ota_frames;
}

int gw_app_take_report_now(gw_app_t *app)
{
    int v;

    if (app == 0 || app->report_now == 0u) {
        return 0;
    }
    v = 1;
    app->report_now = 0u;               /* 读取即消费 */
    return v;
}

uint16_t gw_app_make_cmd_reply(uint8_t id, uint8_t result, uint32_t detail,
                               char *out, uint16_t cap)
{
    const char *name;
    const char *res;
    uint16_t    n;
    uint32_t    v;
    char        num[12];
    uint16_t    nl = 0u;

    if (out == 0 || cap < 8u) {
        return 0u;
    }

    name = gw_cmd_id_str((gw_cmd_id_t)id);
    res  = gw_cmd_result_str((gw_cmd_result_t)result);

    /* 手写拼接而不是 snprintf：
       snprintf 在嵌入式上要链进格式化代码（几 KB），
       而这里只需要拼"字符串 + 空格 + 无符号十进制数"。
       自己做还避免了 %u 在不同 ABI 下的长度差异问题。 */
    n = 0u;
    while (*name != '\0' && n < (uint16_t)(cap - 1u)) {
        out[n++] = *name++;
    }
    if (n < (uint16_t)(cap - 1u)) {
        out[n++] = ' ';
    }
    while (*res != '\0' && n < (uint16_t)(cap - 1u)) {
        out[n++] = *res++;
    }

    /* detail 只在非 0 时附加，避免 `REPORT OK 0` 这种噪声 */
    if (detail != 0u && n < (uint16_t)(cap - 1u)) {
        v = detail;
        do {
            num[nl++] = (char)('0' + (v % 10u));
            v /= 10u;
        } while (v != 0u && nl < (uint16_t)sizeof(num));

        {   /* 上面已判过 n < cap-1（cppcheck: identicalInnerCondition） */
            out[n++] = ' ';
        }
        while (nl > 0u && n < (uint16_t)(cap - 1u)) {
            out[n++] = num[--nl];
        }
    }

    out[n] = '\0';
    return n;
}

/* ==================================================================
 *  主循环推进
 * ================================================================== */

/**
 * 推进应用层（超级循环里周期调用）
 *
 * 两种模式，行为**完全一致**：
 *   - 有调度器：交给 gw_sched_run() 按优先级与周期调度
 *   - 无调度器：按 s_task_defs 的顺序直接调用各任务入口
 *
 * 因为两种模式的顺序与周期一致，测试可以在两条路径上跑同一套断言 ——
 * "任务化"这件事因此能在 PC 上被完整验证，而不是等上了 RTOS 才知道。
 */
void gw_app_poll(gw_app_t *app, uint32_t elapsed_ms)
{
    if (app == 0 || app->inited == 0u) {
        return;
    }

    app->loop_count++;

    /* 任务入口没有 elapsed_ms 参数（为了与 FreeRTOS 签名一致），
       所以每轮把它写进上下文。 */
    app->tick_ms = elapsed_ms;

    if (app->sched != 0) {
        (void)gw_sched_run(app->sched, app, elapsed_ms);
        return;
    }

    /* ---- 顺序模式（顺序与调度模式一致） ---- */
    gw_timing_begin(app->timing, GW_PHASE_SLAVE);
    gw_app_task_entry_slave(app);

    gw_timing_begin(app->timing, GW_PHASE_MASTER);
    gw_app_task_entry_master(app);

    gw_timing_begin(app->timing, GW_PHASE_CAN);
    gw_app_task_entry_can(app);

    gw_timing_begin(app->timing, GW_PHASE_UPLINK);
    gw_app_task_entry_uplink(app);

    gw_timing_begin(app->timing, GW_PHASE_DHT);
    gw_app_task_entry_sensor(app);

    gw_timing_begin(app->timing, GW_PHASE_SYNC);
    gw_app_task_entry_house(app);
}

/* ==================================================================
 *  状态搬运
 * ================================================================== */

/**
 * 把上云链路的状态搬进管道
 *
 * 上报的三个字段与寄存器表的定义一一对应：
 *   pipe.mqtt_status    -> IREG_MQTT_STATUS      云端连接状态
 *   pipe.offline_depth -> IREG_OFFLINE_DEPTH    断网缓存积压条数
 *   pipe.offline_drop  -> IREG_OFFLINE_DROP     缓存溢出丢包数
 *
 * 为什么把"积压条数"和"丢包数"都上报：
 *   只报积压会让人以为"数据都在排队，没丢"；只报丢包看不出趋势。
 *   两个一起看才能判断是该扩容缓存还是该排查链路。
 */
void gw_app_sync_uplink_to_pipe(gw_app_t *app)
{
    const gw_uplink_stat_t *st;

    if (app == 0 || app->inited == 0u) {
        return;
    }
    if (app->uplink_enabled == 0u) {
        /* 未启用上云：如实报告"未连接"，而不是留一个旧的乐观值 */
        app->pipe.mqtt_status    = GW_LINK_DISCONNECTED;
        app->pipe.offline_depth = 0u;
        app->pipe.offline_drop  = 0u;
        return;
    }

    app->pipe.mqtt_status    = (uint16_t)gw_uplink_state(&app->uplink);
    app->pipe.offline_depth = gw_uplink_pending(&app->uplink);

    st = gw_uplink_stats(&app->uplink);
    if (st != 0) {
        /* 丢包数用低 16 位：完整 32 位计数留在 uplink 统计里，
           寄存器表只放一个 16 位概览（溢出后回绕，上位机按差值判断增长）。 */
        app->pipe.offline_drop = (uint16_t)(st->dropped_overflow & 0xFFFFu);
    }
}

/* ==================================================================
 *  模块间状态搬运
 * ================================================================== */
void gw_app_sync_master_to_pipe(gw_app_t *app)
{

    if (app == 0 || app->inited == 0u) {
        return;
    }

    app->pipe.master_online_bmp = mb_master_online_bitmap(&app->master);

    /* 取"最近一次主站错误"用于上报。
       这里选第一台离线从机的错误码作为整体错误码 ——
       比"统计里错误最多的那台"更有现场意义：
       运维关心的是"现在出了什么问题"，而不是历史累计。 */
    {
        uint16_t err = 0u;
        uint8_t  n   = mb_master_slave_count(&app->master);
        uint8_t  i;


        for (i = 0u; i < n; i++) {
            uint8_t addr = 0u;
            const mb_m_slave_stat_t *ss;

            /* 通过访问器遍历，而不是直接读 master.addr[]/stat[]：
               那些数组的内部布局属于主站模块的实现细节。
               早期它们是"按地址索引的大数组"，现在换成了紧凑表，
               应用层不该因为这种内部变化而需要修改。 */
            if (mb_master_slave_at(&app->master, i, &addr) != 0) {
                continue;
            }
            ss = mb_master_slave_stat(&app->master, addr);
            if (ss != 0 && ss->online == 0u && ss->last_err != 0u) {
                err = ss->last_err;
                break;              /* 报第一台离线的即可，避免信息过载 */
            }
        }
        app->pipe.master_err = err;
    }
}

void gw_app_sync_slave_to_pipe(gw_app_t *app)
{
    if (app == 0 || app->slave == 0 || app->inited == 0u) {
        return;
    }

    /* 从站引擎自己维护的统计是本模块的直接数据源。
       注意：这里取的是"引擎累计值"，而不是本层转交的帧数 ——
       两者含义不同：
         slave->stat_rx_frames  协议引擎处理的合法帧数（含非法功能码）
         app->slave_rx_frames   移植层取到并转交的帧数
       上报用引擎的计数，因为它对"通信质量"的刻画更准确。 */
    app->pipe.mb_rx  = app->slave->stat_rx_frames;
    app->pipe.mb_err = app->slave->stat_crc_errors;
    app->pipe.mb_exc = app->slave->stat_exceptions;
}

gw_pipeline_t *gw_app_pipeline(gw_app_t *app)
{
    return (app == 0) ? 0 : &app->pipe;
}

mb_master_t *gw_app_master(gw_app_t *app)
{
    return (app == 0) ? 0 : &app->master;
}

int gw_app_master_add_item(gw_app_t *app, const mb_m_poll_item_t *item)
{
    if (app == 0 || item == 0) {
        return GW_APP_ITEM_ERR_PARAM;
    }

    /* ⚠️ 拦下"轮询地址 = 本机从站地址" —— 理由见 gw_app.h 里那段说明。
       这里**必须显式返回一个可区分的码**，不能只打日志：
       日志会被淹没，而这个配置错误的表现是"主站安静地什么都不做"，
       与"主站没启用"完全一样，事后极难定位。 */
    if (app->slave != 0 && item->slave == app->slave->slave_addr) {
        return GW_APP_ITEM_ERR_LOCAL;
    }

    if (mb_master_add_item(&app->master, item) != MB_M_OK) {
        return GW_APP_ITEM_ERR_ENGINE;
    }
    return GW_APP_ITEM_OK;
}

/* ==================================================================
 *  上云链路
 * ================================================================== */

/* gw_uplink 的发送回调：转交给移植层 */
static int app_uplink_send(void *ctx, const uint8_t *buf, uint16_t len)
{
    gw_app_t *app = (gw_app_t *)ctx;

    if (app == 0 || app->up.esp_send == 0) {
        return -1;
    }
    return app->up.esp_send(app->up.ctx, buf, len);
}

/* gw_uplink 的时间回调。
 *
 * 若移植层没提供 now_ms，就退化为"用于累计的毫秒计数"：
 * uplink 只用时间做秒级的超时与退避判断，精度要求不高，
 * 所以退化方案完全够用。**不提供 now_ms 不应导致上云不可用。** */
static uint32_t app_uplink_now_ms(void *ctx)
{
    gw_app_t *app = (gw_app_t *)ctx;

    if (app->up.now_ms != 0) {
        return app->up.now_ms(app->up.ctx);
    }
    return app->pipe.uptime_s * 1000u + app->pipe.ms_acc;
}

int gw_app_enable_uplink(gw_app_t *app, const gw_uplink_port_t *up,
                         uint32_t dev_id)
{
    gw_uplink_io_t io;

    if (app == 0 || app->inited == 0u) {
        return -1;
    }
    if (up == 0 || up->esp_send == 0 || up->esp_tick == 0
        || up->esp_take_frame == 0) {
        /* 明确禁用：不是错误，只是没接 ESP32 */
        app->uplink_enabled = 0u;
        return 0;
    }

    app->up = *up;

    io.send   = app_uplink_send;
    io.now_ms = app_uplink_now_ms;
    io.ctx    = app;

    if (gw_uplink_init(&app->uplink, &io, dev_id) != GW_UPLINK_OK) {
        app->uplink_enabled = 0u;
        return -2;
    }

    app->uplink_enabled = 1u;
    return 0;
}

int gw_app_submit_telemetry(gw_app_t *app)
{
    gw_telemetry_t t;
    uint8_t        i;

    if (app == 0 || app->inited == 0u) {
        return -2;
    }
    if (app->uplink_enabled == 0u) {
        return -1;
    }

    /* ---- 从管道打包遥测 ----
     *
     * 注意这里**不直接读 pipe.dht.last_* 的内部约定**，而是走管道暴露的
     * 状态字。这样管道内部换了滤波策略或加了新传感器，这里不用改。
     *
     * 设备 ID 与序号由 gw_uplink 统一填充（见 gw_uplink_submit），
     * 所以这里不设 —— 避免两个地方各填一份导致不一致。 */
    memset(&t, 0, sizeof(t));

    /* ⚠️ 从未有过有效读数时用哨兵值，而不是 0 ——
       0℃ / 0%RH 都是合法读数，云端无法与"没有数据"区分
       （status 位里也有 DHT_OK，但载荷本身不该是假的）。 */
    t.temp_x10 = (app->pipe.dht.valid != 0u)
                 ? app->pipe.dht.temp_x10
                 : (int16_t)GW_DHT_REG_UNAVAILABLE;
    t.humi_x10 = (app->pipe.dht.valid != 0u)
                 ? app->pipe.dht.humi_x10
                 : (uint16_t)GW_DHT_REG_UNAVAILABLE;
    t.status   = gw_pipeline_status_word(&app->pipe);
    t.uptime_s = (uint16_t)(app->pipe.uptime_s & 0xFFFFu);

    /* CAN 通道：从**管道**读，因为管道是"采集值 -> 寄存器"的唯一中转站。
     *
     * 为什么不是直接读寄存器表：寄存器表里的值正是管道写进去的，
     * 两者在 poll 结束时一致；但在 poll 中途（例如刚收完 CAN 报文、
     * 还没走到最后的 sync_regs）两者可能不同步。
     * 读管道能保证"上报的是本轮最新的采集结果"。
     *
     * 早期实现读的是 `pipe.can_ch[]` 但**没有任何代码往那份副本里写**，
     * 于是遥测里的 CAN 值恒为 0 —— 而测试因为断言的就是 0 所以通过了。
     * 现在 `gw_app_sync_can_to_pipe()` 会填入它，两边对上了。 */
    for (i = 0u; i < GW_TELEM_CAN_SLOTS; i++) {
        t.can[i] = app->pipe.can_ch[i];
    }

    /* 时间戳由 ESP32 侧补（它有 NTP）。这里填本机运行秒数，
       便于在没有 NTP 时也能看出数据的时间关系。 */
    t.ts = app->pipe.uptime_s;

    if (gw_uplink_submit(&app->uplink, &t) != GW_UPLINK_OK) {
        return -2;
    }
    app->telemetry_seq++;
    return 0;
}

gw_uplink_t *gw_app_uplink(gw_app_t *app)
{
    return (app == 0 || app->uplink_enabled == 0u) ? 0 : &app->uplink;
}

/* ==================================================================
 *  CAN 现场总线
 * ================================================================== */

int gw_app_enable_can(gw_app_t *app, const gw_can_port_t *can)
{
    if (app == 0 || app->inited == 0u) {
        return -1;
    }

    /* 明确禁用：不是错误，只是没接 CAN 收发器 */
    if (can == 0 || can->tick == 0 || can->get_core == 0) {
        app->can_core = 0;
        return 0;
    }

    app->can = *can;

    /* 拿核心指针。移植层在初始化时创建核心并完成通道映射配置，
       应用层只读取它的结果 —— 不参与"映射策略"的决策。 */
    app->can_core = can->get_core(can->ctx);
    if (app->can_core == 0) {
        return -2;
    }

    app->can_offline_ms = 0u;
    return 0;
}

gw_can_core_t *gw_app_can(gw_app_t *app)
{
    return (app == 0) ? 0 : app->can_core;
}

void gw_app_note_boot_cause(gw_app_t *app, gw_boot_cause_t cause)
{
    if (app == 0) {
        return;
    }

    /* ⚠️ 只把"异常启动"记成 sticky 位。
       上电/复位引脚/软件复位（含 OTA 主动重启）都是正常启动 ——
       给它们置位会让上位机把每一次正常重启都报成故障。 */
    switch (cause) {
    case GW_BOOT_IWDG:
        /* 看门狗复位 = 程序曾经卡死到喂不上狗。
           这是 OTA 回滚机制真正生效的证据，现场排查时最该看到的一条。 */
        gw_pipeline_set_sticky(&app->pipe, IREG_STATUS_BIT_IWDG_RESET);
        break;
    case GW_BOOT_FAULT:
        /* 故障处理程序复位（HardFault/总线错误…）。
           ⚠️ 枚举里没有单独的 HARDFAULT —— "异常复位"统一是 GW_BOOT_FAULT，
              与寄存器状态位 IREG_STATUS_BIT_HARDFAULT 对应。 */
        gw_pipeline_set_sticky(&app->pipe, IREG_STATUS_BIT_HARDFAULT);
        break;
    default:
        break;
    }
}

/* ==================================================================
 *  时间预算统计
 *
 *  接入方式非常简单：只存一个指针，应用层在每段边界调
 *  `gw_timing_begin()`。所有统计逻辑都在 gw_timing.c（纯逻辑，已测试）。
 *
 *  为什么不做成回调表（像 RS485/ESP32 那样）：
 *    `gw_timing_t` 是"可测的纯数据结构"，不需要移植层提供硬件能力 ——
 *    唯一需要外部注入的是"读微秒"这一个函数，
 *    它在 `gw_timing_init()` 时就配好了。
 *    再包一层 vtable 只会增加间接调用，没有收益。
 *
 *  `gw_timing_*` 系列函数在收到 NULL 时会直接返回（见 gw_timing.c），
 *  所以应用层**不需要**在每处调用前判断 `app->timing != 0` ——
 *  少写 6 处条件判断，就少 6 处可能写错的地方。
 * ================================================================== */
void gw_app_enable_timing(gw_app_t *app, gw_timing_t *tm)
{
    if (app == 0) {
        return;
    }
    app->timing = tm;
}

/**
 * 把 CAN 通道数据与状态搬进数据管道
 *
 * ============================================================================
 *  ⚠️ 这里必须写**管道**，不能直接写寄存器表。
 *
 *  踩过的坑：最初这个函数直接往 `regs->input[IREG_CAN_CH1..8]` 写值，
 *  结果全部被 `gw_pipeline_sync_regs()` 覆盖成 0 ——
 *  因为管道同步是 poll 的**最后一步**，它会无条件把
 *  `pipe.can_ch[]` / `pipe.can_rx` / 状态字写回寄存器表。
 *
 *  也就是说：**管道才是寄存器表的数据源**，任何直接写寄存器表的操作
 *  只要发生在 `gw_pipeline_sync_regs()` 之前，都会被静默覆盖。
 *
 *  这个坑的特点是"不报错、不崩溃，只是值变成 0"，
 *  而且测试如果只断言"写了没写"（而不是断言最终值）就发现不了。
 *
 *  正确的分层是：
 *      CAN 核心 --(本函数)--> 数据管道 --(gw_pipeline_sync_regs)--> 寄存器表
 *  这样也与其他数据源（DHT22 / Modbus 统计 / 上云状态）保持一致。
 * ============================================================================
 */
void gw_app_sync_can_to_pipe(gw_app_t *app)
{
    gw_can_state_t st;
    const gw_can_stat_t *cst;
    uint8_t i;

    if (app == 0 || app->inited == 0u) {
        return;
    }

    /* ---- 未启用 CAN：如实报告"不可用"，并清掉通道值 ----
     *
     * 为什么这里要清通道值、而不像"单个通道无数据"那样保留原值：
     *   两者的语义完全不同：
     *     "CAN 在跑但这个通道没收到报文" -> 保留原值，靠有效位图区分
     *     "根本没有 CAN"               -> 全部清零，因为那些值不可能是真的
     *   不清的话，上一次运行（或另一个配置）残留的值会一直挂在那里，
     *   上位机看到的就是"CAN 明明没接，却有数据"。 */
    if (app->can_core == 0) {
        for (i = 0u; i < 8u; i++) {
            app->pipe.can_ch[i] = 0u;
        }
        app->pipe.can_ok      = 0u;
        app->pipe.can_rx      = 0u;
        app->can_offline_ms   = 0u;
        return;
    }

    /* ---- 1) 通道值 -> 管道 ---- */
    for (i = 0u; i < GW_CAN_CH_COUNT; i++) {
        /* 只搬"有效"通道的值。
           无效通道保持管道里的原值 —— 这与 gw_can_core_sync_regs 的策略一致：
           清零会让上位机无法区分"通道没数据"与"数据真的是 0"。 */
        if ((gw_can_core_valid_mask(app->can_core) & (uint8_t)(1u << i)) != 0u) {
            app->pipe.can_ch[i] = app->can_core->ch_value[i];
        }
    }

    /* ---- 2) 收帧计数 -> 管道 ---- */
    cst = gw_can_core_stats(app->can_core);
    if (cst != 0) {
        app->pipe.can_rx = cst->rx_total;
    }

    /* ---- 3) 状态位 ----
     *
     * ⚠️ 状态字的 CAN_OK 位来自 `pipe.can_ok`，**不是** sticky 位。
     *    我一开始写成了 `gw_pipeline_set_sticky(..., IREG_STATUS_BIT_CAN_OK)`，
     *    结果状态字里那一位永远是 0 —— 因为 `gw_pipeline_status_word()`
     *    读的是 `p->can_ok`，而 sticky 只参与 IWDG/HardFault 两位。
     *
     *    语义上的区别很重要：
     *      can_ok       是**当前**状态（每轮刷新）—— CAN 现在能不能用
     *      sticky_flags 是**历史**事件（需显式清除）—— 曾经发生过什么
     *    CAN 可用性是"当前状态"，所以属于前者。
     *
     * 只有 NORMAL 与 ERROR_WARN 才算可用：
     *   ERROR_WARN 时数据仍在进来，报"不可用"会造成大量误报；
     *   BUS_OFF / RECOVERING / FAILED 时确实收不到数据，必须如实报告。 */
    st = gw_can_core_state(app->can_core);
    if (st == GW_CAN_ST_NORMAL || st == GW_CAN_ST_ERROR_WARN) {
        app->pipe.can_ok    = 1u;
        app->can_offline_ms = 0u;
    } else {
        app->pipe.can_ok = 0u;

        /* 累计"CAN 不可用"的持续时长。
           为什么要单独统计：Bus-Off 次数只能说明"发生过"，
           而持续时长能说明"现在还没好" —— 后者才是运维决定
           "要不要现在去看"的依据。 */
        if (app->can_offline_ms < 0xFFFFFF00u) {
            app->can_offline_ms += GW_APP_TICK_MS;
        }
    }
}

int gw_app_take_command(gw_app_t *app, uint8_t *payload, uint16_t cap,
                        uint16_t *out_len)
{
    if (app == 0 || payload == 0 || out_len == 0) {
        return -1;
    }
    if (app->cmd_pending == 0u) {
        return -1;
    }

    *out_len = (app->last_cmd_len > cap) ? cap : app->last_cmd_len;
    memcpy(payload, app->last_cmd, *out_len);
    app->cmd_pending = 0u;
    return 0;
}

/**
 * 内部：处理来自 ESP32 的帧
 *
 * 分派原则：**能交给 gw_uplink 的一律交出去**，应用层只处理
 * uplink 明确不认识的业务类型（下行指令、日志）。
 *
 * 这样 gw_uplink 的链路状态机（退避、补传、ACK）是唯一的一份，
 * 应用层不会因为"顺手处理一下 ACK"而与它产生状态分歧。
 */
static void app_handle_esp_frame(gw_app_t *app, uint8_t type,
                                 const uint8_t *payload, uint16_t len)
{
    /* 先给 uplink：它处理 LINK_STATUS / ACK / NACK */
    if (app->uplink_enabled != 0u) {
        if (gw_uplink_on_frame(&app->uplink, type, payload, len) == GW_UPLINK_OK) {
            return;             /* uplink 已处理 */
        }
    }

    /* uplink 不认识的类型：业务帧 */
    switch (type) {
    case GW_FT_COMMAND:
        /* 下行指令：解析 + 执行。
         *
         * ⚠️ 这里**直接执行**，不做"先缓存、稍后处理"。
         *    原因是这些指令都是幂等的、非阻塞的：
         *      立即上报 -> 只置一个标志
         *      改周期   -> 只写一个寄存器
         *      保存参数 -> 阻塞几十毫秒（可接受，且不会打乱时序）
         *      重启     -> 由移植层延时复位
         *
         *    反过来，"先缓存"会引入一个真实的危险：
         *    一条 `restore` 可能在缓冲里等到下一次心跳才被执行 ——
         *    那时操作员已经以为它没生效、可能又发了别的指令。
         *    **指令的时效性比"统一在某个时机执行"更重要**。
         *
         * 执行结果通过 ACK/NACK 回给 ESP32（见 gw_app_make_cmd_reply），
         * 同时记进 cmd_ok / cmd_err / cmd_last_result 供寄存器上报。 */
        {
            gw_cmd_result_t r = gw_app_exec_command(app,
                                                    (const char *)payload,
                                                    len);
            uint32_t detail = 0u;

            /* 状态请求：把当前状态字作为 detail 带回去，
               这样一条 `status` 就能拿到设备概况，不用再读寄存器。 */
            if (app->cmd_last_id == (uint8_t)GW_CMD_GET_STATUS
                && r == GW_CMD_OK) {
                detail = (uint32_t)gw_pipeline_status_word(&app->pipe);
            }
            /* 改周期成功：回显新的周期值 */
            if (app->cmd_last_id == (uint8_t)GW_CMD_SET_PERIOD
                && r == GW_CMD_OK) {
                detail = app->regs->holding[HREG_REPORT_PERIOD];
            }

            /* 兼容旧接口：仍然把原文缓存一份，供上层轮询取走。 */
            app->last_cmd_len = (len > (uint16_t)sizeof(app->last_cmd))
                                ? (uint16_t)sizeof(app->last_cmd) : len;
            if (app->last_cmd_len > 0u) {
                memcpy(app->last_cmd, payload, app->last_cmd_len);
            }
            app->cmd_pending = 1u;

            /* 把结果回给 ESP32。
               ⚠️ 无论成功失败都回 —— 上位机必须能区分
                  "指令执行了" 与 "指令丢了"，否则现场只能靠猜。

               ⚠️ 发送走 `app->up.esp_send`（上云移植层），
                  因为它要经过 uplink 的发送路径 —— 那样才与
                  遥测、心跳共用同一条串口与同一套缓冲管理。
                  直接写 app->port 是错的：那是 RS485 侧。 */
            if (app->up.esp_send != 0) {
                uint8_t  frame[GW_FRAME_MAX_TOTAL];
                char     reply[48];
                uint16_t rl;
                uint16_t flen = 0u;
                /* ⚠️ 名字不能叫 `type` —— 那会遮蔽本函数的参数 `type`
                   （cppcheck 的 shadowArgument 告警）。
                   遮蔽在这里虽然无害（内层不用外层的值），
                   但读代码的人会误以为它指的就是同一个东西。 */
                uint8_t  reply_type;

                /* ⚠️ 用**专用的** CMD_ACK / CMD_NACK，不要复用 ACK / NACK。
                   后者的语义被上云链路占用（"遥测送达/被拒"），
                   而且上云只看类型、不判载荷。复用会导致：
                     一次"指令令牌错误"被当成"遥测 T1 被拒"，
                     进而**误触发一次遥测重传**，重试统计也被污染。
                   详见 gw_frame.h 里这两个类型的说明。 */
                reply_type = (r == GW_CMD_OK) ? (uint8_t)GW_FT_CMD_ACK
                                              : (uint8_t)GW_FT_CMD_NACK;
                rl = gw_app_make_cmd_reply(app->cmd_last_id, (uint8_t)r,
                                           detail, reply, sizeof(reply));
                if (rl > 0u
                    && gw_frame_build(frame, (uint16_t)sizeof(frame),
                                      reply_type,
                                      (const uint8_t *)reply, rl,
                                      &flen) == GW_FRAME_OK) {
                    (void)app->up.esp_send(app->up.ctx, frame, flen);
                }
            }
        }
        break;

    /* ---- OTA 下行帧：转给注入的处理器 ----
     *
     * ⚠️ 在此之前这三个类型是"显式忽略"的 —— 也就是
     *    `gw_ota.c` 的 111 项测试与 `gw_ota_bridge.c` 的 132 项测试
     *    **从来没有被真正调用过**。测试全绿不等于功能接了线。
     *
     * 用回调而不是直接包含 gw_ota_bridge.h：后者依赖
     * gw_ota.h -> port/gw_flash_map.h（硬件地址表），
     * 应用层一旦包含就失去了"纯逻辑、可 PC 测试"的性质。
     */
    case GW_FT_OTA_BEGIN:
    case GW_FT_OTA_CHUNK:
    case GW_FT_OTA_END:
        if (app->ota_handler != 0) {
            int orc = app->ota_handler(app->ota_ctx, type, payload, len);

            app->ota_frames++;
            app->ota_last_result = (int8_t)((orc > 127) ? 127
                                          : ((orc < -128) ? -128 : orc));
            if (orc < 0) {
                /* ⚠️ 失败要计数但**不回 NACK**：
                   回 NACK 是 OTA 处理器自己的职责（它知道阶段码与进度，
                   应用层不知道）。应用层重复回一个会造成两条应答。 */
                app->ota_errors++;
            }
        }
        /* 未注入处理器时静默忽略：那不是错误，
           只是"本固件没编 OTA 功能"。 */
        break;

    case GW_FT_LOG:
    case GW_FT_OTA_ACK:
        /* 日志转发与 OTA 分片确认（ESP32 -> STM32）：
           当前未实现对应功能，显式忽略而不是当成错误。 */
        break;

    default:
        /* 未知类型：忽略。不做任何"猜测式处理" ——
           对不认识的帧做动作比忽略它危险得多。 */
        break;
    }
}

/**
 * 内部：推进上云链路（每轮调用）
 *
 * 处理顺序：
 *   1) 让移植层把硬件缓冲的字节喂给帧解析器
 *   2) 取出所有完整帧并分派
 *   3) 推进 uplink 状态机（退避/心跳/补传/超时）
 *   4) 到期则打包提交一条遥测
 */
static void app_poll_uplink(gw_app_t *app, uint32_t elapsed_ms)
{
    if (app->uplink_enabled == 0u || app->up.esp_tick == 0) {
        return;
    }

    /* ---- 1) 收字节 ---- */
    app->up.esp_tick(app->up.ctx);

    /* ---- 2) 取帧并分派 ----
       循环取到没有为止：一次 tick 可能收到多帧（粘包）。 */
    {
        uint8_t  type = 0u;
        uint8_t  payload[GW_FRAME_MAX_PAYLOAD];
        uint16_t len = 0u;
        int      guard;

        for (guard = 0; guard < 16; guard++) {
            if (app->up.esp_take_frame(app->up.ctx, &type, payload,
                                       (uint16_t)sizeof(payload), &len) != 0) {
                break;
            }
            app_handle_esp_frame(app, type, payload, len);
        }
    }

    /* ---- 3) 推进链路状态机 ---- */
    gw_uplink_tick(&app->uplink, elapsed_ms);

    /* ---- 4) 上报节拍 ----
       管道里的 sample_due 是"采集节拍"，这里需要的是"上报节拍"。
       两者可以不同：本地采集可以比上报更频繁（寄存器要实时），
       而上报太频繁会浪费流量与云端配额。

       上报周期取保持寄存器的 HREG_REPORT_PERIOD（秒）。
       为 0 或非法时退回默认值，避免"配置写错就再也不上报"。 */
    {
        uint16_t period_s = 10u;        /* 默认 10 秒 */

        if (app->regs != 0) {
            uint16_t v = app->regs->holding[HREG_REPORT_PERIOD];
            if (v >= 1u && v <= 3600u) {
                period_s = v;
            }
        }

        app->uplink_report_acc_ms += elapsed_ms;
        if (app->uplink_report_acc_ms >= (uint32_t)period_s * 1000u) {
            app->uplink_report_acc_ms = 0u;
            (void)gw_app_submit_telemetry(app);
        }
    }
}
