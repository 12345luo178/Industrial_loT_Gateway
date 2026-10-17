/**
 * @file    gw_app.h
 * @brief   应用层：把各模块编排成完整的网关固件（裸机超级循环版）
 *
 * ============================================================================
 *  架构总览（自下而上）
 *
 *    ┌──────────────────────────────────────────────────────────┐
 *    │ gw_app.c            编排：谁在什么时机调用谁              │  <- 本文件
 *    ├──────────────────────────────────────────────────────────┤
 *    │ gw_pipeline.c       翻译：采集值 -> 寄存器表 语义         │
 *    │ modbus_master.c     主动：轮询从机、超时重试、离线退避     │
 *    │ modbus_rtu.c        被动：应答上位机的读写                │
 *    ├──────────────────────────────────────────────────────────┤
 *    │ dht22.c             DHT22 位解码/解析/滤波（纯逻辑）      │
 *    ├──────────────────────────────────────────────────────────┤
 *    │ 【移植层 vtable】    由外部注入，见 gw_port_t             │
 *    │   rs485_port.c        RS485 收发 + DE 方向 + 帧界判定     │
 *    │   dht22_port_stm32.c  PG11 引脚 + DWT 计时               │
 *    ├──────────────────────────────────────────────────────────┤
 *    │ HAL / 寄存器                                              │
 *    └──────────────────────────────────────────────────────────┘
 *
 *  应用层只做三件事：
 *    1) 初始化各模块（顺序有讲究，见 gw_app_init）
 *    2) 周期调用各模块的"推进"函数
 *    3) 在模块之间搬运少量状态（如把主站统计搬进管道）
 *
 *  ⚠️ 应用层**不写协议逻辑**，也不直接碰寄存器格式 ——
 *     那些都属于下层。应用层一旦开始"顺手解析一下帧"，分层就名存实亡。
 *
 * ----------------------------------------------------------------------------
 *  为什么移植层用函数指针表（vtable）而不是直接调用？
 *
 *  如果 gw_app.c 里直接写 `rs485_port_take_frame(...)`，它就必须包含
 *  rs485_port.h，而后者依赖 stm32f1xx_hal.h —— 于是整个应用层
 *  无法在 PC 上编译，也就无法单元测试"编排逻辑"。
 *
 *  改成 vtable 之后：
 *    - gw_app.c 只依赖一个纯函数指针结构体，PC 上可用假实现测试
 *    - STM32 上由 main.c 把真实的 rs485/dht22 实现填进 vtable
 *    - 将来换通信方式（例如改用 CAN 做从站接口）只需换一个 vtable
 *
 *  代价是多一层间接调用（每次几十纳秒），对 10ms 级的节拍完全可忽略。
 * ============================================================================
 */
#ifndef GW_APP_H
#define GW_APP_H

#include <stdint.h>

#include "gw_pipeline.h"
#include "modbus_master.h"
#include "modbus_rtu.h"
#include "modbus_regs.h"
#include "dht22_port.h"
#include "gw_uplink.h"
#include "gw_can_core.h"
#include "gw_timing.h"
#include "gw_sched.h"
#include "gw_cmd.h"
#include "gw_fault.h"       /* gw_boot_cause_t（gw_app_note_boot_cause 用） */

/* ---------------- 固件版本 ---------------- */
#define GW_FW_VERSION_MAJOR   0u
#define GW_FW_VERSION_MINOR   4u

/* ---------------- 上行遥测的打包参数 ---------------- */
#define GW_TELEM_CAN_SLOTS   4u     /* 遥测帧里带 4 路 CAN 通道（够用且帧短） */

/* ---------------- 设计预期的超级循环节拍（毫秒） ---------------- */
#define GW_APP_TICK_MS        10u

/* ---------------- 移植层接口（由上层注入实现） ----------------
 *
 * 每个回调的语义都要明确，因为它们是模块间的契约，不是随意的小工具。
 */
typedef struct {
    /**
     * 取一帧已收到的数据（非阻塞）
     * @return 0 = 取到一帧（*len 已填）；非 0 = 当前无完整帧
     *
     * 必须在任务/主循环上下文调用，不可在中断里调用。
     */
    int (*rs485_take_frame)(void *ctx, uint8_t *buf, uint16_t buf_size, uint16_t *len);

    /**
     * 以从站身份发送一帧响应
     * @return 0 = 成功；非 0 = 失败（总线忙、超时等）
     *
     * 实现内部负责：申请总线 -> 拉高 DE -> 发送 -> 等 TC -> 拉低 DE -> 释放。
     * 从站响应是"发完即完"，不需要跨调用持有总线。
     */
    int (*rs485_send)(void *ctx, const uint8_t *buf, uint16_t len);

    /* ---------------- 主站发送（需要跨调用持有总线） ----------------
     *
     * 主站的发送与从站有本质区别：
     *   从站：申请 -> 发送 -> 释放      一次调用内完成
     *   主站：申请 -> 发送 -> **持有到收到响应或超时**   跨越多次调用
     *
     * 所以主站路径必须把"申请"和"发送"拆成两步暴露给应用层，
     * 由应用层在合适的时机释放（见 gw_app_poll 里的说明）。
     * ------------------------------------------------------------------ */

    /**
     * 以主站身份申请总线发送权（只决策，不发送）
     * @return 0 = 拿到；非 0 = 总线忙，本次请求推迟（不是错误）
     */
    int (*rs485_bus_acquire)(void *ctx);

    /**
     * 发送一帧（调用前必须已通过 rs485_bus_acquire 拿到权）
     * @return 0 = 成功；非 0 = 失败
     *
     * 实现内部**不**释放总线 —— 主站要让出总线只能在
     * 收到响应、超时、或被从站抢占时，这三处都由应用层驱动。
     */
    int (*rs485_send_held)(void *ctx, const uint8_t *buf, uint16_t len);

    /** 通知移植层：主站进入"等待响应"窗口（供仲裁层做抢占决策） */
    void (*rs485_enter_wait)(void *ctx);

    /** 通知移植层：主站退出等待窗口（收到响应或超时），并释放总线 */
    void (*rs485_exit_wait)(void *ctx);

    /**
     * 主动释放主站持有的总线（被从站抢占时调用）
     *
     * 为 NULL 表示移植层不支持显式释放 —— 此时应用层会退回
     * "把被抢占的请求当作超时处理"，语义上不够准确（会误记从机失败），
     * 但不会卡死。建议实现。
     */
    int (*rs485_release_master)(void *ctx);

    /**
     * 查询当前是否处于主站等待窗口
     *
     * 应用层靠它检测"等待被抢占"：主站在等响应，但等待窗口已被清除，
     * 说明从站抢占发生了。
     * 为 NULL 时应用层无法检测抢占，只能依赖超时。
     */
    int (*rs485_in_wait)(void *ctx);

    /**
     * 查询主站是否仍持有总线（已发请求但还没收到响应/超时）
     *
     * 应用层靠它区分三种情况：
     *   主站还在等响应            -> 保持持权
     *   主站结束等待且仍持权      -> 调 rs485_exit_wait 正常释放
     *   主站结束等待但已不持权    -> 等待窗口被从站抢占
     *
     * 为 NULL 时应用层无法区分后两者，只能保守地"结束等待就释放"
     * （见 gw_app_poll 的说明）。
     */
    int (*rs485_master_holds_bus)(void *ctx);

    /** 取寄存器表指针（由移植层持有；应用层只读写，不负责分配） */
    mb_regs_t *(*get_regs)(void *ctx);

    /**
     * 刷新寄存器表里的"实时字段"（可选；NULL 表示不需要刷新）
     *
     * ⚠️ 为什么需要这个钩子 —— 这是一次真实的踩坑：
     *
     *   `get_regs` **只在 `gw_app_init()` 时被调用一次**（拿指针），
     *   之后再也不调。所以"在 get_regs 里刷新实时字段"是**错的**：
     *   那些字段只会在启动时被写一次，之后 Modbus 读到的永远是
     *   那一份**陈旧快照**。
     *
     *   我上板排查时就被这个坑住：诊断寄存器显示"模块未绑定"，
     *   而直接读内存却发现明明绑定了 —— 因为读到的是启动快照。
     *
     * 本钩子在**每轮从站任务开头**被调用（也就是每次轮到从站收帧前），
     * 移植层可以在这里把"每次都该更新"的字段（诊断、耗时统计等）
     * 写进寄存器表。
     *
     * ⚠️ 与 `get_regs` 的分工：
     *     `get_regs`     -> 只取指针，一次
     *     `refresh_regs` -> 每次从站任务前刷新内容，可多次
     *
     * ⚠️ 它**不能**用于更新业务数据（温度、计数等）——那些有各自的
     *    明确更新点，混在一起会让"谁负责写这个寄存器"变得模糊。
     *    这里只放"每次读都该是最新值"的观测类字段。
     */
    void (*refresh_regs)(void *ctx);

    /**
     * 采样一次 DHT22。
     * 为 NULL 表示硬件不可用 —— 应用层会按"采样失败"处理，
     * 而不是崩溃或跳过状态更新。
     */
    dht22_err_t (*dht_sample)(void *ctx, dht22_filtered_t *f, uint8_t retry);

    /* ------------------------------------------------------------------
     *  以下两个回调供**下行指令**使用（可选）
     *
     *  为什么要做成回调而不是直接在 app 层调用 gw_param / HAL：
     *    指令执行会碰硬件（写 Flash、复位芯片），
     *    直接调用会让 gw_app.c 依赖 Flash 与 CMSIS ——
     *    那样"指令执行"就没法在 PC 上测了。
     *
     *    做成回调之后，PC 测试可以注入一个"假装保存成功/失败"的假实现，
     *    从而覆盖到"保存失败时该回什么错误码"这类分支。
     * ------------------------------------------------------------------ */

    /**
     * 把当前寄存器表里的参数保存到 Flash
     * @return 0 成功；<0 失败
     *
     * 为 NULL 时：指令 `save` 返回 GW_CMD_ERR_STATE（明确告诉对方"本设备不支持"），
     * 而不是假装成功 —— 假装成功会让上位机以为参数已经持久化了。
     */
    int (*param_save)(void *ctx);

    /**
     * 恢复出厂参数（写默认值到 Flash 并同步到寄存器表）
     * @return 0 成功；<0 失败
     *
     * 为 NULL 时：指令 `restore` 返回 GW_CMD_ERR_STATE。
     */
    int (*param_restore)(void *ctx);

    /**
     * 重启设备
     *
     * ⚠️ 实现**不应立即返回** —— 它应该在延时后复位（例如等 200ms
     *    让串口把 ACK 发出去），否则上位机永远收不到"重启指令已接受"的确认。
     *
     * 为 NULL 时：指令 `reboot` 返回 GW_CMD_ERR_STATE
     * （比直接复位安全 —— 至少上级知道没执行）。
     */
    void (*reboot)(void *ctx);

    void *ctx;      /* 传给以上回调的上下文 */
} gw_port_t;

/* ==================================================================
 *  上云链路接口（ESP32 串口帧通信）
 *
 *  与 gw_port_t 分开的原因：
 *    gw_port_t 是"必须有的硬件能力"，缺一个就无法初始化；
 *    而上云是**可选能力** —— 只做 Modbus 网关（没有 ESP32）时，
 *    整块填 NULL 即可，应用层会照常运行，只是不产生遥测。
 *
 *  如果把两者混在一起，就会出现"没有 ESP32 就连 Modbus 从站都跑不起来"
 *  这种荒谬结果。
 * ================================================================== */
typedef struct {
    /**
     * 借助移植层发送一帧到 ESP32
     * @return 0 成功；非 0 失败
     */
    int (*esp_send)(void *ctx, const uint8_t *buf, uint16_t len);

    /**
     * 推进 ESP32 接收：把硬件缓冲的字节喂给帧解析器
     *
     * 应用层每轮调用一次。为 NULL 表示没有 ESP32。
     */
    void (*esp_tick)(void *ctx);

    /**
     * 取一帧（非阻塞）
     * @return 0 = 取到一帧；非 0 = 当前没有完整帧
     */
    int (*esp_take_frame)(void *ctx, uint8_t *out_type, uint8_t *payload,
                          uint16_t cap, uint16_t *out_len);

    /**
     * 取当前毫秒时间戳（给 gw_uplink 做超时与退避判断）
     *
     * 为 NULL 时应用层会退化为"用 poll 的 elapsed_ms 累加"，
     * 精度足够（uplink 的时间分辨率是秒级）。
     */
    uint32_t (*now_ms)(void *ctx);

    void *ctx;
} gw_uplink_port_t;

/* ==================================================================
 *  CAN 接口（现场总线，可选能力）
 *
 *  与上云一样是**可选**的：没有 CAN 收发器时整块填 NULL 即可。
 *
 *  ----------------------------------------------------------------------------
 *  ⚠️ 为什么 CAN 数据用"指针 + 默认弱实现"，而 RS485/ESP32 用回调
 *
 *  RS485 与 ESP32 的数据都**必须**经过应用层的编排才能工作
 *  （从站要响应、上云要按节拍上报），所以用回调让移植层反向调用应用层。
 *
 *  而 CAN 数据是**单向流入**的：移植层收报文 → 核心映射成通道值 →
 *  应用层直接读那些通道值写寄存器。中间不需要应用层做任何决策。
 *
 *  对这种情况，把 8 个通道的映射结果通过回调一个个搬过来，
 *  只是为了"形式统一"而增加一层间接 —— 反而让数据流更难追踪。
 *
 *  所以这里用更直接的方式：应用层持有 `gw_can_core_t*`，
 *  直接调 `gw_can_core_sync_regs()` 把值写进寄存器表。
 *  移植层只负责"推进"（收报文 + 跑状态机），不做数据搬运。
 *
 *  这样既保持了"应用层不依赖 HAL"（`gw_can_core.h` 是纯逻辑头），
 *  又不会为了形式统一而绕远路。
 * ================================================================== */
typedef struct {
    /**
     * 推进 CAN 硬件层（取报文交给核心 + 查询错误状态 + 跑恢复状态机）
     * @param elapsed_ms 距上次调用经过的毫秒数
     */
    void (*tick)(void *ctx, uint32_t elapsed_ms);

    /**
     * 取 CAN 核心上下文
     *
     * 移植层在初始化时创建核心，应用层通过这里拿到指针后
     * **直接**调用 `gw_can_core_sync_regs()` / `gw_can_core_stats()`
     * 等纯逻辑接口。
     *
     * 为 NULL 表示 CAN 未启用。
     */
    gw_can_core_t *(*get_core)(void *ctx);

    void *ctx;
} gw_can_port_t;

/* ---------------- CAN 通道在输入寄存器里的起始偏移 ---------------- */
#define GW_APP_CAN_BASE   IREG_CAN_CH1

/* ---------------- 应用层状态 ---------------- */
typedef struct {
    gw_pipeline_t  pipe;            /* 数据管道（采集值 -> 寄存器） */
    mb_master_t    master;          /* Modbus 主站 */
    gw_port_t      port;            /* 移植层实现（外部注入） */
    mb_slave_t    *slave;           /* 从站上下文（由移植层持有，这里只引用） */
    mb_regs_t     *regs;            /* 寄存器表（由移植层持有） */

    /* ---- 上云链路（可选） ---- */
    gw_uplink_t       uplink;
    gw_uplink_port_t  up;
    uint32_t          telemetry_seq;    /* 已提交的遥测条数（诊断用） */
    uint32_t          uplink_report_acc_ms; /* 上报节拍累加器 */
    uint8_t           uplink_enabled;   /* 1 = 已启用上云 */

    /* ---- CAN 现场总线（可选） ---- */
    gw_can_port_t     can;
    gw_can_core_t    *can_core;         /* 由移植层持有；NULL = 未启用 */
    uint32_t          can_offline_ms;   /* 连续异常持续时长（诊断用） */

    /**
     * 时间预算统计（可选）
     *
     * 只需要一个指针 —— 所有计时逻辑都在 `gw_timing.c`（纯逻辑，已测试）。
     * 应用层只负责在每段边界调 `gw_timing_begin()`。
     *
     * 为什么不做成回调表：
     *   `gw_timing_t` 本身是"可测的纯数据结构"，
     *   不像 RS485/ESP32 那样需要移植层提供硬件能力。
     *   多包一层 vtable 只会增加间接调用，没有收益。
     *
     * 为 NULL 时 `gw_timing_*` 内部会直接返回（见 gw_timing.c），
     * 所以**不需要在这里做条件判断**。
     */
    gw_timing_t      *timing;

    /**
     * 协作式调度器（可选）
     *
     * 填了它，`gw_app_poll()` 就不再自己按固定顺序调各段，
     * 而是交给 `gw_sched_run()` 按优先级与周期调度。
     *
     * 两者行为**完全一致**（任务表里的顺序与周期就是原来的固定顺序），
     * 区别只在于：
     *   - 有调度器时，每个任务的"实际耗时"和"是否错过硬超时"会被记录
     *   - 无调度器时，直接顺序调用，不做这些统计
     *
     * 为 NULL 时退回原来的顺序调用 —— 这样既有测试不受影响，
     * 也允许在资源紧张时关掉调度层省下 RAM。
     */
    gw_sched_t       *sched;

    /**
     * ★ 只给"喂狗判据"用的调度器（可选；RTOS 下必须填）
     *
     * ⚠️⚠️ 为什么不能复用上面的 `sched`（缺陷 U，2026-10-11）：
     *    `sched` 一旦填上，`gw_app_poll()` 就会改走 `gw_sched_run()`
     *    **去分派任务**。而 RTOS 下每个任务自己 `for(;;)` 循环、
     *    根本不该再有谁去分派 —— 所以 `gw_rtos_create_tasks()`
     *    故意没填 `sched`。可喂狗判据也挂在同一个字段上，于是
     *    `gw_app_watchdog_ok()` 走了"没调度器 -> **无条件允许**"那条路，
     *    **RTOS 下等于无条件喂狗**（也就是没养狗），而三处文档都说它
     *    在检查"6 个任务有没有跑过一轮"。
     *
     *    所以把两件事拆开：**分派用 `sched`，判据用 `wd_sched`**。
     *    `gw_app_enable_sched()` 会同时填两个（裸机：既是分派也是判据）；
     *    RTOS 只填 `wd_sched`（见 `gw_app_enable_watchdog_sched()`）。
     */
    gw_sched_t       *wd_sched;

    /**
     * 任务入口上下文用的本次节拍（毫秒）
     *
     * 任务入口只接一个 `void *ctx`，没有 elapsed_ms 参数 ——
     * 这是为了与 FreeRTOS 的任务函数签名一致（`void task(void *arg)`）。
     * 所以每轮的 elapsed_ms 存在这里，由任务入口自行读取。
     */
    uint32_t          tick_ms;

    /* ---- 下行指令（可选子系统） ---- */
    uint32_t          cmd_rx;           /* 累计收到的指令数 */
    uint32_t          cmd_ok;           /* 执行成功的 */
    uint32_t          cmd_err;          /* 失败/拒绝的 */
    uint8_t           cmd_last_result;  /* 最近一条的结果码（gw_cmd_result_t） */
    uint8_t           cmd_last_id;      /* 最近一条的指令 ID（gw_cmd_id_t） */
    uint8_t           report_now;       /* 1 = 上层请求"立即上报一次" */

    /* ==================================================================
     *  OTA 帧的处理钩子（可选）
     *
     *  ⚠️ 为什么用函数指针 + 不透明参数，而不是直接放一个 gw_ota_bridge_t：
     *
     *    `gw_ota_bridge.h` 依赖 `gw_ota.h`，而后者依赖
     *    `port/gw_flash_map.h`（分区地址表）。应用层一旦包含它，
     *    就从"纯逻辑、可 PC 测试"变成"依赖硬件地址布局"——
     *    1785 行 PC 测试的基础会被破坏。
     *
     *    用回调之后，应用层只知道"有个东西能处理 OTA 帧"，
     *    不知道它怎么落盘。`main.c` 负责把真正的桥接对象接上。
     *
     *  返回值语义（与 gw_ota_bridge_frame 的约定一致）：
     *      0  = 成功（或已忽略的重复）
     *      >0 = 非致命（例如"忙，稍后重发"）
     *      <0 = 失败
     *  应用层只用它来记日志与统计，不解释具体码值。
     * ================================================================== */
    int  (*ota_handler)(void *ctx, uint8_t type,
                        const uint8_t *payload, uint16_t len);
    void  *ota_ctx;                     /* 传给 ota_handler 的不透明参数 */
    uint32_t ota_frames;                /* 已处理的 OTA 帧数（诊断用） */
    uint32_t ota_errors;                /* OTA 帧处理失败次数 */
    int8_t   ota_last_result;           /* 最近一次的处理结果（截到 int8） */

    /* ---- 下行指令（由 ESP32 转发过来的业务指令） ----
       应用层只做"缓存 + 交给上层处理"，不解释指令语义 ——
       语义属于业务，将来加指令时不必改这里。 */
    uint8_t           last_cmd[GW_FRAME_MAX_PAYLOAD];
    uint16_t          last_cmd_len;
    uint8_t           cmd_pending;      /* 1 = 有一条未处理的指令 */

    /* 诊断：本模块自身的运行计数 */
    uint32_t       loop_count;
    uint32_t       slave_rx_frames; /* 经本层转交给从站引擎的帧数 */
    uint32_t       slave_tx_frames; /* 本层成功发出的响应数 */
    uint32_t       dht_samples;     /* 采样尝试次数 */
    uint32_t       master_deferred; /* 主站因总线被占而推迟的次数 */
    uint32_t       master_preempted;/* 主站等待窗口被从站抢占的次数 */

    /* ⚠️ "申请了却并不持有主站身份"的次数 —— 2026-10-10 加。
     *
     * 它专抓一种**静默**的移植层接线错误：vtable 的 rs485_bus_acquire
     * 与 rs485_send_held 用了不同的角色（上板时真出现过：申请配成从站，
     * 发送按主站，于是每一帧都在"第二次申请"时 BUSY）。
     *
     * 有它之前的表现：主站重试四万多次、一帧没发出去，
     * 而对外看起来只是"主站安静地什么都不做"。
     * 有它之后：这个数会一路涨，一眼就知道是接线问题，不用猜。
     *
     * 只在移植层提供了 rs485_master_holds_bus 回调时才会被检查
     * （没提供就退化成"信任调用方"，不会误报）。 */
    uint32_t       master_not_held;
    uint8_t        inited;
} gw_app_t;

/* ---------------- 需要外部提供的从站引擎对象 ----------------
 *
 * 从站上下文（mb_slave_t）与寄存器表（mb_regs_t）由移植层创建并持有，
 * 应用层通过 port.get_regs 与 gw_app_init 的 slave 参数拿到引用。
 * 这样存储归属清晰：谁分配谁负责生命周期。
 */
int gw_app_init(gw_app_t *app, mb_slave_t *slave, const gw_port_t *port);

/**
 * 启用上云链路（可选）
 *
 * @param app   应用层上下文（必须已 gw_app_init）
 * @param up    上云移植层接口（可为 NULL = 禁用上云）
 * @param dev_id 设备 ID（写进每条遥测）
 * @return 0 成功；负值失败
 *
 * 为什么要单独一个函数而不是塞进 gw_app_init：
 *   上云是可选能力。只做 Modbus 网关时不必提供 ESP32，
 *   这样"没有联网模组"不会阻塞从站功能。
 */
int gw_app_enable_uplink(gw_app_t *app, const gw_uplink_port_t *up,
                         uint32_t dev_id);

/**
 * 接入 CAN 现场总线（可选）
 *
 * @param app  应用层上下文（必须已 gw_app_init）
 * @param can  CAN 移植层接口（可为 NULL = 禁用 CAN）
 * @return 0 成功；负值失败
 *
 * 启用后应用层会：
 *   1) 每轮 poll 调用 `can.tick()` 推进硬件层（收报文 + Bus-Off 恢复）
 *   2) 把 CAN 通道值同步到输入寄存器的 IREG_CAN_CH1..8
 *   3) 把 CAN 状态位与收帧计数写进寄存器表
 */
int gw_app_enable_can(gw_app_t *app, const gw_can_port_t *can);

/**
 * 把 CAN 通道数据与状态同步到寄存器表
 *
 * 由 gw_app_poll 每轮调用；也可以由上层在"事件触发"时主动调一次。
 */
void gw_app_sync_can_to_pipe(gw_app_t *app);

/** 取 CAN 核心上下文（未启用时返回 NULL） */
gw_can_core_t *gw_app_can(gw_app_t *app);

/**
 * 启用时间预算统计（可选）
 *
 * @param app 应用层上下文
 * @param tm  统计上下文（由上层持有并初始化好时间源）；NULL = 关闭统计
 *
 * 启用后应用层会在每段的边界调用 `gw_timing_begin()`，
 * 从而得到"一轮 poll 的时间构成"。这份数据是**上 FreeRTOS 时
 * 决定任务优先级的依据**，也是答辩/简历里"量化实测数据"的来源。
 */
void gw_app_enable_timing(gw_app_t *app, gw_timing_t *tm);

/**
 * 启用协作式任务调度（可选）
 *
 * @param app   应用层上下文
 * @param sched 调度器上下文；**调用前必须已 gw_sched_init() 并 gw_sched_start()**
 *
 * 启用后 `gw_app_poll()` 改为调用 `gw_sched_run()`，
 * 各段变成独立任务、按优先级与周期调度。
 *
 * ----------------------------------------------------------------------------
 *  任务划分（`gw_app_task_defs()` 提供定义表）
 *
 *    ID               任务          周期      硬超时   为什么这样分
 *    ---------------  ------------  --------  --------  ----------------------
 *    GW_TASK_SLAVE    RS485 从站    每轮      **有**   上位机等待有硬超时
 *    GW_TASK_MASTER   主站轮询      每轮      无        可以等
 *    GW_TASK_CAN       CAN 收发      每轮      无        纯内存操作
 *    GW_TASK_UPLINK   上云链路      每轮      无        串口发送可能 12ms
 *    GW_TASK_SENSOR   DHT22 采样    2.5s      无        **关中断约 5ms**
 *    GW_TASK_HOUSE    汇聚与看护    每轮      ——        喂狗必须在这里
 *
 *  三条划分原则：
 *    ① 有硬超时的独立成任务，且优先级最高
 *    ② 会长时间关中断的必须单独成任务（否则连带阻塞同任务内的其他工作）
 *    ③ 喂狗放在最后一个任务里 —— 它只有在前面所有任务都让路之后才会跑
 *
 * ----------------------------------------------------------------------------
 *  为什么采样任务要有独立周期（2.5s）而不是每轮都跑
 *
 *  它虽然每轮都被调用，但内部由 `gw_pipeline_take_sample_due()` 判断
 *  是否真的采样 —— 所以实际采样仍是 2.5s 一次。
 *  周期字段填 0 表示"每轮都调用"，与"每轮都做事"是两件事。
 */
void gw_app_enable_sched(gw_app_t *app, gw_sched_t *sched);

/**
 * 取任务定义表（供上层 `gw_sched_init()` 使用）
 *
 * @return 静态常量数组，长度 = GW_TASK_COUNT，顺序即优先级
 */
const gw_task_def_t *gw_app_task_defs(void);

/**
 * 取任务入口上下文
 *
 * FreeRTOS 下建任务时这样用：
 *     xTaskCreate(gw_app_task_entry_slave, "slave", stack, app, PRIO, NULL);
 *
 * 各任务入口函数签名与 FreeRTOS 的 `TaskFunction_t` 一致
 * （`void (*)(void *)`），所以**业务代码零改动**即可迁移。
 */
void gw_app_task_entry_slave(void *ctx);
void gw_app_task_entry_master(void *ctx);
void gw_app_task_entry_can(void *ctx);
void gw_app_task_entry_uplink(void *ctx);
void gw_app_task_entry_sensor(void *ctx);
void gw_app_task_entry_house(void *ctx);

/**
 * ⚠️ 喂狗前的统一检查（FreeRTOS 与裸机都该走这个）
 *
 * @return 1 = 允许喂狗；0 = 不允许（有任务卡住了）
 *
 * 判据来源（优先级从高到低）：
 *   1. `wd_sched`（RTOS 下由 `gw_rtos_create_tasks()` 填）
 *   2. `sched`（裸机下由 `gw_app_enable_sched()` 填）
 *   3. 都没有 -> 返回 1（**只有顺序调用模型**才允许这样：
 *      "主循环还在跑"本身就意味着所有段都跑过了）
 *
 * ⚠️ 判据本身在 `gw_sched_watchdog_ok()` 里，有两套（心跳 / 进展），
 *    详见那里的说明 —— 别在这里再写一套。
 */
int gw_app_watchdog_ok(gw_app_t *app);

/**
 * ★ 只注册"喂狗判据"用的调度器（**不**改变 `gw_app_poll()` 的分派方式）
 *
 * RTOS 下必须调它：任务由内核调度，`gw_app_poll()` 不再被调用，
 * 但喂狗前仍然要知道"各任务的心跳还在不在"。
 * 见 `gw_app_t::wd_sched` 的说明（缺陷 U）。
 */
void gw_app_enable_watchdog_sched(gw_app_t *app, gw_sched_t *sched);

/* ==================================================================
 *  下行指令执行
 * ================================================================== */

/**
 * 执行一条下行指令（文本形式）
 *
 * @param app 应用层上下文
 * @param text 指令文本（**不必以 '\0' 结尾**）
 * @param len  长度
 * @return gw_cmd_result_t（GW_CMD_OK = 执行成功）
 *
 * 内部流程：`gw_cmd_parse()` -> `gw_cmd_check_token()` -> 执行
 *
 * ⚠️ 危险指令（save / restore / reboot）必须带确认令牌，
 *    令牌错误返回 GW_CMD_ERR_TOKEN，**不做任何动作**。
 *
 * 执行不会阻塞：`reboot` 只是调用移植层的复位回调
 * （由移植层负责延时后复位，好让 ACK 有机会发出去）。
 */
gw_cmd_result_t gw_app_exec_command(gw_app_t *app, const char *text,
                                    uint16_t len);

/**
 * 是否有"立即上报"的请求待处理
 *
 * @return 1 有；0 没有（读取时会**清除**该请求）
 *
 * 由上层（`main.c` 或上云任务）决定何时真正上报 ——
 * 应用层不直接提交遥测，因为"什么时候发"属于上云策略。
 */
int gw_app_take_report_now(gw_app_t *app);

/**
 * 注入 OTA 帧处理器
 *
 * @param app     应用上下文
 * @param handler 处理器（传 NULL 关闭）
 * @param ctx     传给 handler 的不透明参数（通常是 gw_ota_bridge_t*）
 *
 * 注入之后，`GW_FT_OTA_BEGIN` / `GW_FT_OTA_CHUNK` / `GW_FT_OTA_END`
 * 会转给 handler；未注入时这三个类型被显式忽略（不是当成错误）。
 */
void gw_app_enable_ota(gw_app_t *app, int (*handler)(void *, uint8_t,
                                                     const uint8_t *,
                                                     uint16_t),
                       void *ctx);

/**
 * 取 OTA 相关的统计（诊断用）
 * @return 已处理的 OTA 帧数
 */
uint32_t gw_app_ota_frames(const gw_app_t *app);

/**
 * 生成指令响应的载荷（回给 ESP32 的 ACK/NACK）
 *
 * @param id     被执行的指令 ID
 * @param result 结果码
 * @param detail 附加数据（例如新的上报周期；无则填 0）
 * @param out    输出缓冲（建议 >= 32 字节）
 * @param cap    容量
 * @return 写入长度；0 表示失败
 *
 * 载荷格式（文本，便于现场直接看懂）：
 *     `<ID> <RESULT>` 或 `<ID> <RESULT> <detail>`
 *     例如：`PERIOD OK 30`
 *
 * ⚠️ 为什么用文本而不是二进制：
 *    上位机收到的是一条人能直接读的消息，不需要查表解码。
 *    代价是几字节的开销 —— 在"现场排查"面前完全值得。
 */
uint16_t gw_app_make_cmd_reply(uint8_t id, uint8_t result, uint32_t detail,
                               char *out, uint16_t cap);

/**
 * 按当前上报周期打包一条遥测并提交给上云链路
 *
 * @return 0 已提交；-1 上云未启用；-2 入参问题
 *
 * 由应用层在"上报节拍到期"时调用（见 gw_app_poll 内部）。
 * 之所以暴露出来，是为了让上层能在"事件触发上报"时主动调一次
 * （例如某路 CAN 数据超限需要立刻上报）。
 */
int gw_app_submit_telemetry(gw_app_t *app);

/** 取上云链路上下文（供上层查状态/统计，或注入链路状态） */
gw_uplink_t *gw_app_uplink(gw_app_t *app);

/**
 * 取待处理的下行指令（非阻塞）
 *
 * @param payload 指令输出缓冲
 * @param cap     缓冲容量
 * @param out_len 指令长度
 * @return 0 取到一条；-1 当前没有待处理指令
 *
 * 指令语义由上层决定 —— 应用层只负责"从链路里取出来并缓存"。
 * 这样加新指令时不必改应用层。
 */
int gw_app_take_command(gw_app_t *app, uint8_t *payload, uint16_t cap,
                        uint16_t *out_len);

/**
 * 推进应用层（超级循环里周期调用）
 *
 * @param app        应用层上下文
 * @param elapsed_ms 距上次调用经过的毫秒数
 *
 * 处理顺序（按实时性从高到低）：
 *   1) 检测主站等待窗口是否被抢占，被抢占则放弃本次等待（不记从机失败）
 *   2) RS485 从站：取帧 -> 交给协议引擎 -> 发送响应
 *      （放最前面，因为上位机请求有硬超时要求）
 *   3) 主站轮询推进
 *   4) 到期则采样 DHT22
 *   5) 把统计搬进管道并同步寄存器表
 *
 * ⚠️ 步骤 1 必须在步骤 3 之前：
 *    抢占发生后要先放弃等待，否则主站会在"已经失去总线"的状态下
 *    继续计时，最后把抢占算成从机超时。
 */
void gw_app_poll(gw_app_t *app, uint32_t elapsed_ms);

/**
 * 把"本次启动的原因"记进状态位（寄存器 IREG_DEV_STATUS 的 sticky 位）
 *
 * @param app    应用层
 * @param cause  来自 gw_fault_port_init() 的启动原因
 *
 * ⚠️ 为什么需要它（2026-10-10 补的洞）：
 *    寄存器表里定义了 `IREG_STATUS_BIT_IWDG_RESET`（bit6）与
 *    `IREG_STATUS_BIT_HARDFAULT`（bit7），注释写着"sticky，仅复位清除" ——
 *    但**代码里从来没有人设置过它们**，于是上位机永远看不到
 *    "这台设备刚被看门狗/HardFault 复位过"这个最关键的现场信息。
 *    接口承诺了、实现却没有，属于"静默失效"。
 *
 * 只有 IWDG 与 HardFault 会被置位：
 *    上电/复位引脚/软件复位（含 OTA 主动重启）都是"正常启动"，
 *    给它们置位会让上位机把正常重启报成故障。
 */
void gw_app_note_boot_cause(gw_app_t *app, gw_boot_cause_t cause);

/**
 * 把主站的运行状态搬进数据管道
 *
 * 为什么单独一个函数：
 *   主站与管道是解耦的两个模块，谁都不该直接读对方的结构体。
 *   搬运由应用层显式完成，职责清楚，也便于单元测试。
 */
void gw_app_sync_master_to_pipe(gw_app_t *app);

/** 把从站引擎的统计搬进管道（收帧/错误/异常计数） */
void gw_app_sync_slave_to_pipe(gw_app_t *app);

/**
 * 把上云链路的状态搬进管道（连接状态 / 积压深度 / 丢包数）
 *
 * 未启用上云时会把这三个字段置为"未连接 / 0 / 0"，
 * 而不是保留旧值 —— 否则上位机会看到一个永远乐观的假状态。
 */
void gw_app_sync_uplink_to_pipe(gw_app_t *app);

/** 取管道指针（供后续 CAN / ESP32 模块写入自己的字段） */
gw_pipeline_t *gw_app_pipeline(gw_app_t *app);

/** 取主站指针（供配置阶段添加轮询项） */
mb_master_t *gw_app_master(gw_app_t *app);

/* ==================================================================
 *  登记一个主站轮询项（**配置期唯一入口，不要直接调 mb_master_add_item**）
 *
 *  ------------------------------------------------------------------
 *  ⚠️ 为什么要有这一层：它防的是一个在上板验证里真栽过的坑
 *
 *  「主从共存」时同一个地址会出现**两次**：
 *      - 本机自己的从站地址（GW_RS485_DEFAULT_ADDR，默认 1）
 *      - 主站轮询的下游从机地址（例如 MB_L0_MASTER_SLAVE_ADDR）
 *
 *  这两个地址**必须不同**。一旦相同，应用层的帧归属派发就只能靠
 *  "主站在等 + 站号对得上"来判断（见 gw_app.c 的派发注释），于是
 *  上位机发给**本机从站**的读请求会被先喂给 mb_master_on_response()：
 *
 *      站号对得上 -> CRC 对（请求帧本身 CRC 合法）
 *                 -> 功能码对（都是 0x04）
 *                 -> 字节数对不上（buf[2] 是起始地址高字节）
 *                 -> 判为 MB_M_ERR_LEN -> slave_stat_note_fail() + advance()
 *
 *  后果是**静默**的、而且方向反直觉：
 *      每被上位机读一次，就误记一次"下游从机失败"并跳过一个轮询项，
 *      fail_streak 累积后那台从机被判离线 ——
 *      上位机看到的在线位图永远是 0，看起来像"主站根本没工作"。
 *
 *  ⚠️ PC 单测抓不到它：test_master_dispatch.c 的用例里轮询地址是 2、
 *     本机从站是 1，**正好绕开了同地址这个组合**，所以 CI 全绿。
 *     这种"配置性的静默失效"只能在登记入口显式拦下来。
 * ================================================================== */

/** gw_app_master_add_item() 的返回码 */
#define GW_APP_ITEM_OK           0
#define GW_APP_ITEM_ERR_PARAM   (-1)    /* app / item 为空 */
#define GW_APP_ITEM_ERR_LOCAL   (-2)    /* 轮询地址 == 本机从站地址 */
#define GW_APP_ITEM_ERR_ENGINE  (-3)    /* 主站引擎拒绝（表满、项非法等） */

/**
 * 登记一个主站轮询项
 *
 * @param app  应用层上下文（必须已 gw_app_init）
 * @param item 轮询项（含义见 mb_m_poll_item_t）
 * @return GW_APP_ITEM_* 之一
 *
 * ⚠️ 地址与**登记时**的本机从站地址比较。运行期改从站地址
 *    （HREG_SLAVE_ADDR）之后不做追溯检查 —— 运行期改地址属于
 *    重新配置，应由上层自行重新登记。
 */
int gw_app_master_add_item(gw_app_t *app, const mb_m_poll_item_t *item);

#endif /* GW_APP_H */
