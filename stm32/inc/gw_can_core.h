/**
 * @file    gw_can_core.h
 * @brief   CAN 报文处理与 Bus-Off 恢复（纯逻辑，可在 PC 上单元测试）
 *
 * ============================================================================
 *  本模块在网关里的定位：**只收不发**
 *
 *  用户方案明确"CAN 用 TJA1051T/3"且只用做接收。这是有道理的：
 *    - 网关的职责是把现场总线数据汇聚上来，不是去控制执行器
 *    - 一旦要发，就要处理"何时发、冲突了怎么办、发失败怎么办"，
 *      而 CAN 的仲裁与重发都由硬件完成，软件能做的很有限
 *    - 只收不发就没有"软件误发一条报文导致现场设备动作"的风险
 *
 *  所以本模块只处理接收路径。将来要扩展成能发，接口是现成的
 *  （`gw_can_port` 提供发送即可），但**默认不做**。
 *
 *  ----------------------------------------------------------------------------
 *  Bus-Off 恢复：CAN 现场最难排查的故障
 *
 *  CAN 控制器的错误计数器（TEC/REC）超过 255 时进入 **Bus-Off**：
 *  控制器**主动脱离总线**，此后完全收发不了，而且**不会自动恢复**
 *  （除非使能了自动恢复，但那样又可能反复进入 Bus-Off 刷屏）。
 *
 *  现场表现是"CAN 数据突然全没了，重启设备就好了" ——
 *  如果软件不处理，就只能靠人去现场重启。
 *
 *  本模块实现的状态机：
 *
 *     NORMAL --(错误计数超限)--> BUS_OFF
 *                                   |
 *                                   | 等待 GW_CAN_RECOVER_DELAY_MS
 *                                   v
 *                              RECOVERING --(重新初始化成功)--> NORMAL
 *                                   |
 *                                   +--(失败，且未超次数)--> 再等一轮
 *                                   |
 *                                   +--(超过 MAX_ATTEMPTS)--> FAILED
 *                                                              |
 *                                                              | 等更长时间
 *                                                              v
 *                                                         再试（不放弃）
 *
 *  两个刻意的设计：
 *
 *  ① **不无限快速重试**。Bus-Off 常见根因是"总线上有人持续发错误帧"
 *     或"终端电阻不对"，快速重试只会让错误计数再次爆掉。
 *     等待一段时间让对端先稳定，成功率明显更高。
 *
 *  ② **不永久放弃**。有人会把"恢复失败"做成终止状态，
 *     结果一次偶发干扰就让设备彻底失联。这里失败后拉长间隔继续试 ——
 *     CAN 是现场总线，偶发干扰是常态，软件必须能自愈。
 *
 *  恢复次数的统计会上报到寄存器表，运维能看出"这台设备在反复 Bus-Off"，
 *  从而去查物理层（终端电阻、线缆、接地），而不是反复重启。
 *
 *  ----------------------------------------------------------------------------
 *  通道映射策略："最新值覆盖"
 *
 *  CAN 报文是**周期性**的（例如电机控制器每 10ms 发一次转速）。
 *  寄存器表里每个通道只有一个 16 位位置，所以策略只能是"最新值覆盖"。
 *
 *  这也意味着**不需要大缓冲**：堆积 100 条历史报文的转速值毫无意义，
 *  运维要的是"现在多少"。所以本模块只保存各通道的最新值，
 *  另加一个很小的诊断环形缓冲（记录最近几条报文的 ID，便于排查映射错误）。
 * ============================================================================
 */
#ifndef GW_CAN_CORE_H
#define GW_CAN_CORE_H

#include <stdint.h>

#include "gw_can_frame.h"

/* ---------------- 规模上限 ---------------- */
#define GW_CAN_CH_COUNT         8u      /* 与寄存器表的 IREG_CAN_CH1..8 对应 */

/* 诊断环形缓冲深度：只用于"最近收到了哪些 ID"的排查，不做数据缓存。
   取 8 是因为现场排查通常只需看最近几条就能判断"是不是 ID 映射错了"。 */
#define GW_CAN_DIAG_DEPTH       8u

/* ---------------- Bus-Off 恢复参数 ----------------
 *
 * ⚠️ 这三个参数必须是"时间"，不能靠状态机自身"每个 tick 试一次"来实现退避。
 *
 * 踩过的坑：最初把恢复写成"进入 RECOVERING 状态后每个 tick 都试一次重新初始化"，
 * 于是 10ms 的节拍下**每秒尝试 100 次**，3 次失败立刻进 FAILED。
 * 表面上"代码里写了退避"，实际上退避窗口被 tick 频率吃掉了。
 *
 * 教训：任何"间隔"都必须用**时间**表达（下次允许的时刻），
 *       而不是靠"每次调用试一次、失败就计数"来隐式实现 ——
 *       后者的实际间隔完全取决于调用频率，是隐藏的耦合。
 */
#define GW_CAN_RECOVER_DELAY_MS 1000u   /* 首次恢复前等待：让对端先稳定 */
#define GW_CAN_RECOVER_MAX_TRIES 3u     /* 连续这么多次失败后拉长间隔 */
#define GW_CAN_RECOVER_LONG_MS  10000u  /* 拉长后的间隔 */

/* ---------------- CAN 控制器状态 ---------------- */
typedef enum {
    GW_CAN_ST_UNINIT     = 0,   /* 未初始化 */
    GW_CAN_ST_NORMAL     = 1,   /* 正常收发 */
    GW_CAN_ST_ERROR_WARN = 2,   /* 错误计数偏高，但还能通信 */
    GW_CAN_ST_BUS_OFF    = 3,   /* 已脱离总线，等待恢复 */
    GW_CAN_ST_RECOVERING = 4,   /* 正在尝试恢复 */
    GW_CAN_ST_FAILED     = 5    /* 多次恢复失败，进入长间隔重试 */
} gw_can_state_t;

/* ---------------- 通道映射项 ---------------- */
typedef struct {
    uint16_t can_id;        /* 匹配的 CAN 标识符（标准帧 11 位） */
    uint8_t  channel;       /* 映射到哪个通道（0~7） */
    uint8_t  enabled;       /* 1 = 启用该映射 */
} gw_can_map_t;

/* ---------------- 统计量（诊断 + 实测数据来源） ---------------- */
typedef struct {
    uint32_t rx_total;          /* 收到的报文总数（含被过滤掉的） */
    uint32_t rx_accepted;       /* 通过滤波并被处理的报文数 */
    uint32_t rx_filtered;       /* 被硬件/软件滤波拒绝的报文数 */
    uint32_t rx_unmapped;       /* 通过滤波但没匹配到任何通道 */
    uint32_t rx_malformed;      /* 长度不足、无法取值的报文 */
    uint32_t bus_off_count;     /* 进入 Bus-Off 的次数 */
    uint32_t recover_tries;     /* 恢复尝试总次数 */
    uint32_t recover_ok;        /* 恢复成功次数 */
    uint32_t recover_fail;      /* 恢复失败次数 */
} gw_can_stat_t;

/* ---------------- 诊断记录（最近收到的报文 ID） ---------------- */
typedef struct {
    uint16_t id;
    uint8_t  dlc;
} gw_can_diag_t;

/* ---------------- 上下文 ---------------- */
typedef struct {
    /* ---- 配置 ---- */
    gw_can_map_t map[GW_CAN_CH_COUNT];
    uint16_t     filt_id;           /* 滤波 ID（标准帧 11 位有效） */
    uint16_t     filt_mask;         /* 滤波掩码；0 = 全通过 */

    /* ---- 通道数据（最新值覆盖） ---- */
    uint16_t     ch_value[GW_CAN_CH_COUNT];
    uint8_t      ch_valid;          /* 位图：哪些通道收到过数据 */

    /* ---- 诊断环形缓冲 ---- */
    gw_can_diag_t diag[GW_CAN_DIAG_DEPTH];
    uint8_t       diag_head;
    uint8_t       diag_count;

    /* ---- 状态机 ---- */
    gw_can_state_t state;
    uint32_t       state_ms;        /* 进入当前状态以来经过的毫秒 */
    uint32_t       next_try_ms;     /* 距"下次允许尝试恢复"还有多少毫秒 */
    uint32_t       ms_acc;          /* 总毫秒累加器（诊断用） */
    uint8_t        recover_tries;   /* 本轮连续失败次数 */

    /* ---- 回调（由移植层注入，便于单测） ---- */
    /**
     * 请求移植层重新初始化 CAN 控制器（从 Bus-Off 恢复）
     * @return 0 成功；非 0 失败
     */
    int (*reinit)(void *ctx);
    void *reinit_ctx;

    gw_can_stat_t stat;
    uint8_t       inited;
} gw_can_core_t;

/* ==================================================================
 *  接口
 * ================================================================== */

/**
 * 初始化
 *
 * @param c           上下文
 * @param filt_id     硬件滤波 ID（标准帧 11 位）
 * @param filt_mask   硬件滤波掩码（0 = 全通过）
 * @param reinit      恢复回调（可为 NULL：那时恢复只能靠上层轮询状态）
 * @param reinit_ctx  回调上下文
 * @return 0 成功；负值参数非法
 */
int gw_can_core_init(gw_can_core_t *c, uint16_t filt_id, uint16_t filt_mask,
                     int (*reinit)(void *ctx), void *reinit_ctx);

/**
 * 配置通道映射
 *
 * @param c        上下文
 * @param can_id   要匹配的标准帧 ID
 * @param channel  映射到的通道下标（0~GW_CAN_CH_COUNT-1）
 * @return 0 成功；负值参数非法
 *
 * @note 同一个 can_id 重复配置会覆盖前一条 —— 这比"报错"更实用：
 *       现场调试时常常反复改映射，报错会逼着人多写一次清除逻辑。
 */
int gw_can_core_set_map(gw_can_core_t *c, uint16_t can_id, uint8_t channel);

/** 清除所有通道映射 */
void gw_can_core_clear_map(gw_can_core_t *c);

/**
 * 处理一条收到的 CAN 报文（由移植层在中断后或轮询时调用）
 *
 * @return 1 = 已映射到某个通道并更新了数据
 *         0 = 通过滤波但没匹配到通道（计入 rx_unmapped）
 *        -1 = 被硬件滤波拒绝（正常情况下移植层不会把这种帧送进来）
 *        -2 = 报文非法（长度不足等）
 */
int gw_can_core_on_frame(gw_can_core_t *c, const gw_can_frame_t *f);

/**
 * 周期推进（处理 Bus-Off 恢复状态机）
 *
 * @param elapsed_ms 距上次调用经过的毫秒数
 */
void gw_can_core_tick(gw_can_core_t *c, uint32_t elapsed_ms);

/**
 * 通知核心：控制器进入了 Bus-Off
 *
 * 由移植层在 HAL_CAN_GetError() 报告 Bus-Off 时调用。
 * 为什么不自己检测：Bus-Off 是硬件状态，软件"猜"不如硬件"报"可靠。
 */
void gw_can_core_notify_bus_off(gw_can_core_t *c);

/**
 * 通知核心：控制器恢复到正常态
 *
 * 由移植层在检测到控制器重新可用时调用（例如 HAL 报告错误清零）。
 */
void gw_can_core_notify_normal(gw_can_core_t *c);

/**
 * 通知核心：控制器进入错误警告态（错误计数偏高但还能通信）
 *
 * 提前知道"快出问题了"比事后恢复更有价值 —— 运维可以在它彻底掉线前介入。
 */
void gw_can_core_notify_error_warn(gw_can_core_t *c);

/**
 * 通知核心：**端口层初始化失败**（硬件根本没起来）
 *
 * ============================================================================
 *  ⚠️⚠️ 为什么必须有这个接口（2026-10-14 上板实测抓到的"假报正常"）
 *
 *  `gw_can_core_init()` 是**纯逻辑层**的初始化，它无从知道硬件到底起没起来，
 *  所以它只能乐观地把状态置成 `NORMAL`。而 `can_port_init()` 在
 *  `HAL_CAN_Start()` 超时失败时会**提前 return**（板子上没有 CAN 总线 /
 *  收发器没工作时，bxCAN 拿不到 11 个连续隐性位，永远退不出初始化模式），
 *  于是核心就**停在 NORMAL 没人管**。
 *
 *  后果链条（实测）：
 *      core.state = NORMAL
 *        -> `gw_pipeline_status_word()` 置起 `IREG_STATUS_BIT_CAN_OK`
 *        -> 界面显示「CAN   总线正常」
 *      而同一个时刻 `MSR.INAK = 1`（控制器还在初始化模式，一帧都收不了），
 *      自检也如实报 `[FAIL] CAN` —— **三处互相矛盾，其中"正常"那个是假的**。
 *
 *  ⚠️ 这类"假报正常"比"报失败"危险得多：报失败会有人去看，假报正常不会。
 *
 *  ----------------------------------------------------------------------------
 *  为什么置成 `FAILED` 而不是 `UNINIT`：
 *      `UNINIT` 的语义是"这一层**从未**初始化过"，自检会把它当 **SKIP**
 *      （见 `chk_can()`）—— 那就又把问题藏起来了。
 *      而这里的实情是"初始化过、但**失败了**"，必须让自检与状态字都如实报出来。
 *
 *  为什么不干脆让调用方直接写 `c->state`：
 *      状态迁移要**同时**维护 `state_ms` / `next_try_ms` / `recover_tries`，
 *      散在外面改迟早漏一个（这正是 `enter_state()` 存在的理由）。
 *
 *  @note 置位后会**退避一个短窗口**再让状态机的恢复流程去重试 ——
 *        即"没总线时每 1s 重试、连续 3 次后拉长到 10s"，
 *        所以**后插上总线能自愈**，不需要重启设备。
 */
void gw_can_core_mark_failed(gw_can_core_t *c);

/**
 * 把通道数据同步到寄存器表
 *
 * @param c     上下文
 * @param regs  寄存器表
 * @param base  通道 0 对应的输入寄存器偏移（通常是 IREG_CAN_CH1）
 *
 * @note 只写**有效**的通道（ch_valid 位为 1）。
 *       无效通道**保持寄存器表里的原值不动** ——
 *       清零会让上位机无法区分"通道没数据"和"数据真的是 0"。
 *       这正是 IREG_STATUS_BIT_CAN_OK 与 ch_valid 要一起看的原因。
 */
void gw_can_core_sync_regs(const gw_can_core_t *c, uint16_t *iregs,
                           uint16_t base, uint16_t ireg_count);

/**
 * 取一条诊断记录（非阻塞）
 * @return 1 取到；0 没有
 */
int gw_can_core_take_diag(gw_can_core_t *c, gw_can_diag_t *out);

/** 当前控制器状态 */
gw_can_state_t gw_can_core_state(const gw_can_core_t *c);

/** 通道有效位图 */
uint8_t gw_can_core_valid_mask(const gw_can_core_t *c);

/** 取统计量 */
const gw_can_stat_t *gw_can_core_stats(const gw_can_core_t *c);

/** 状态转字符串（日志用） */
const char *gw_can_state_str(gw_can_state_t st);

#endif /* GW_CAN_CORE_H */
