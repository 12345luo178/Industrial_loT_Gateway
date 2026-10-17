/**
 * @file    gw_timing.h
 * @brief   时间预算与负载统计（纯逻辑，可在 PC 上单元测试）
 *
 * ============================================================================
 *  这个模块存在的理由：**任务优先级不该靠猜**
 *
 *  上 FreeRTOS 时必须回答一连串问题：
 *
 *    - 每个模块实际耗时多少？（决定优先级与时间片）
 *    - 谁的耗时波动最大？（决定谁的优先级要更高）
 *    - CPU 有多少余量？（决定还能不能加功能）
 *    - RS485 从站响应有没有超时风险？（Modbus 有硬超时要求）
 *    - 主循环一轮要多久？（决定能不能满足 10ms 节拍）
 *
 *  没有数据就只能"感觉 RS485 比较急，给它高优先级" ——
 *  这种判断在答辩与面试里站不住脚，在现场也容易出事
 *  （例如给了一个其实很慢的模块高优先级，反而拖慢了真正急的）。
 *
 *  ----------------------------------------------------------------------------
 *  为什么不用"开始计时 - 结束计时"直接算
 *
 *  那样要改所有模块的代码，侵入性大；而且**测出来的东西会随测量代码本身变化**
 *  （观测者效应）。
 *
 *  本模块的做法：**分区间累加**。
 *  主循环在每段的边界调一次 `gw_timing_mark()`，模块内部一行都不用改。
 *
 *      gw_timing_begin(&tm, GW_PHASE_SLAVE);
 *      ... 从站处理 ...
 *      gw_timing_begin(&tm, GW_PHASE_MASTER);   // 上一段自动结算
 *      ... 主站轮询 ...
 *
 *  代价是"只能在函数边界分区间"，好处是零侵入。
 *  对本项目（每个阶段本来就是独立函数调用）完全够用。
 *
 *  ----------------------------------------------------------------------------
 *  统计量为什么用"最大/最小"而不只是"平均"
 *
 *  平均值会掩盖问题。假设某段平均 10µs，但偶尔到 3000µs ——
 *  平均值看起来完全正常，而 Modbus 从站已经超时了。
 *  **实时系统里最坏情况才是决定性的**，所以 min/max 必须与平均一起看。
 * ============================================================================
 */
#ifndef GW_TIMING_H
#define GW_TIMING_H

#include <stdint.h>

/* ---------------- 时间段划分 ----------------
 *
 * 与 gw_app_poll 的处理步骤一一对应。
 * 顺序即执行顺序，便于把统计结果直接看成"一轮循环的时间构成"。
 */
typedef enum {
    GW_PHASE_SLAVE   = 0,   /* RS485 从站：取帧 + 响应（有硬超时要求） */
    GW_PHASE_MASTER  = 1,   /* 主站轮询推进 */
    GW_PHASE_DHT     = 2,   /* DHT22 采样（时序敏感，可能关中断） */
    GW_PHASE_CAN     = 3,   /* CAN 推进（收报文 + 恢复状态机） */
    GW_PHASE_UPLINK  = 4,   /* 上云链路（含帧解析与补传） */
    GW_PHASE_SYNC    = 5,   /* 寄存器同步与状态汇聚 */

    /**
     * 从站响应帧的**传输时间**（阻塞发送）
     *
     * ⚠️ 为什么要单独开一段（而不是并进 GW_PHASE_SLAVE）：
     *
     *   实测发现：把发送留在 GW_PHASE_SLAVE 里时，那个"处理耗时"
     *   被传输时间主导 —— 13 字节响应 1260us、69 字节 6340us，
     *   两个相差 5 倍的数混在一个指标里根本没法用。
     *
     *   而"处理耗时"（取帧 + 组帧）才是实时性指标，它应当**与帧长无关**。
     *
     *   所以拆成两段：
     *       GW_PHASE_SLAVE     -> 取帧 + 组帧（与帧长无关）
     *       GW_PHASE_SLAVE_TX  -> 阻塞发送（与帧长成正比）
     *
     *   这样报告里两个数都能引用，而且含义清楚。
     *
     * ⚠️ 它必须排在**最后**：`gw_timing_priority_hint()` 之类按枚举顺序
     *    推断优先级的逻辑不受影响（传输不是可调度的"任务段"）。
     */
    GW_PHASE_SLAVE_TX = 6,  /* 从站响应传输（阻塞发送） */

    GW_PHASE_COUNT   = 7
} gw_phase_t;

/* ---------------- 单段统计 ---------------- */
typedef struct {
    uint32_t calls;         /* 进入该段的次数 */
    uint32_t total_us;      /* 累计耗时（用于算平均） */
    uint32_t max_us;        /* 最大单次耗时（**实时性的关键指标**） */
    uint32_t min_us;        /* 最小单次耗时 */
    uint32_t over_budget;   /* 超过预算的次数 */
} gw_phase_stat_t;

/* ---------------- 一轮循环统计 ---------------- */
typedef struct {
    uint32_t loops;         /* 完成的轮数 */
    uint32_t total_us;      /* 累计循环耗时 */
    uint32_t max_us;        /* 最长一轮 */
    uint32_t min_us;
} gw_loop_stat_t;

/* ---------------- 预算 ----------------
 *
 * 每段的"预期上限"。超过就计数，用于发现"某段偶发变慢"。
 *
 * 数值来源：按 72 MHz、10ms 节拍估算的合理上限。
 *   RS485 从站：一帧响应约 8 字节约 8ms（9600 波特率），
 *               但那是**串口硬件**的时间，CPU 只花在组帧上（几十 µs）。
 *               这里给 500µs —— 超过说明有异常（例如在轮询等待）。
 *   主站：只是状态机推进，给 200µs。
 *   DHT22：**采样时才做事**，一次约 5ms（含关中断的时序等待）。
 *          但那段是在 gw_dht_sample 里，本模块测的是调用开销。
 *          给 200µs。真正的 5ms 阻塞由 dht22 模块内部的时序决定 ——
 *          这一点必须在文档里说清楚，否则会误以为"采样很便宜"。
 *   CAN：收报文 + 状态机，给 300µs。
 *   上云：帧解析 + 可能的串口发送。串口发送 133 字节约 12ms，
 *         所以给 20ms —— 这是**唯一可能超 10ms 节拍的段**。
 *   同步：纯内存操作，给 200µs。
 */
#define GW_BUDGET_SLAVE_US      500u
#define GW_BUDGET_MASTER_US     200u
#define GW_BUDGET_DHT_US        200u
#define GW_BUDGET_CAN_US        300u
#define GW_BUDGET_UPLINK_US     20000u
#define GW_BUDGET_SYNC_US       200u
/**
 * 从站响应传输预算
 *
 * ⚠️ 这个值**必然**远大于其它段，因为它是**阻塞发送**的线上时间：
 *      115200 下最长 ADU（256 字节）≈ 22 ms
 *       9600  下最长 ADU（256 字节）≈ 267 ms
 *    所以取 300000 µs（300 ms）作为预算 —— 只要不超它，
 *    说明传输本身没出问题（例如 TC 一直不置位导致等超时）。
 *
 * ⚠️ 不要因为"这个数很大"就以为它不重要：它正是
 *    "阻塞发送占用主循环多久"的直接证据，也是将来改 DMA 发送的动机。
 */
#define GW_BUDGET_SLAVE_TX_US   300000u

/* ---------------- 上下文 ---------------- */
typedef struct {
    gw_phase_stat_t phase[GW_PHASE_COUNT];
    gw_loop_stat_t  loop;

    gw_phase_t      cur;            /* 当前所在段 */
    uint32_t        cur_start_us;   /* 当前段开始时刻 */
    uint8_t         in_phase;       /* 1 = 已 begin，未 end */

    /* 上一轮 loop_end 到本轮 loop_begin 之间的空闲时间 */
    uint32_t        idle_us;
    uint32_t        last_loop_end_us;

    /**
     * 1 = 已经有过一次 loop_end（用于跳过第一轮）
     *
     * 第一轮的间隔是从 init 时刻算起的，**不包含空闲时间**，
     * 所以 min_us 与负载都会被它污染。详见 gw_timing.c 里的说明。
     */
    uint8_t         loop_seen;

    /**
     * 微秒时间源（由移植层注入，指向 gw_dwt_us）
     *
     * 为 NULL 时本模块整体不工作（sample 函数直接返回）——
     * 这样"没有 DWT"的平台（例如 PC 单测）可以完全不配它，
     * 而不是崩掉。
     */
    uint32_t (*now_us)(void *ctx);
    void *ctx;

    uint8_t inited;
} gw_timing_t;

/* ==================================================================
 *  接口
 * ================================================================== */

/**
 * 初始化
 * @param now_us 微秒时间源（可为 NULL = 禁用统计）
 */
int gw_timing_init(gw_timing_t *tm, uint32_t (*now_us)(void *ctx), void *ctx);

/** 进入某个时间段（会自动结算上一段） */
void gw_timing_begin(gw_timing_t *tm, gw_phase_t ph);

/** 结束当前段（不进入新段时使用） */
void gw_timing_end(gw_timing_t *tm);

/** 标记一轮循环结束 */
void gw_timing_loop_end(gw_timing_t *tm);

/** 清零所有统计（保留时间源配置） */
void gw_timing_reset(gw_timing_t *tm);

/** 取某段统计（越界返回 NULL） */
const gw_phase_stat_t *gw_timing_phase(const gw_timing_t *tm, gw_phase_t ph);

/** 取循环统计 */
const gw_loop_stat_t *gw_timing_loop(const gw_timing_t *tm);

/**
 * 取某段的平均耗时（µs）
 * @return 平均 µs；无数据时返回 0
 */
uint32_t gw_timing_avg_us(const gw_timing_t *tm, gw_phase_t ph);

/**
 * 取 CPU 负载估算（千分比，0~1000）
 *
 * 计算方式：所有段的累计耗时 / 总循环耗时。
 * 因为时间段覆盖了循环里的绝大部分，所以这个比值就是"CPU 忙的比例"。
 *
 * @return 0~1000；无数据时返回 0
 */
uint32_t gw_timing_load_permil(const gw_timing_t *tm);

/**
 * 取"最坏一轮"里最耗时的段
 *
 * 用于回答"上 FreeRTOS 后哪个任务最该优先"。
 * 判断依据不是"平均最耗时"，而是"**最坏情况下**最耗时"——
 * 实时系统里后者才决定优先级。
 */
gw_phase_t gw_timing_worst_phase(const gw_timing_t *tm);

/** 段名（日志与报告用） */
const char *gw_timing_phase_str(gw_phase_t ph);

/**
 * 按当前统计给出"建议的任务优先级排序"
 *
 * @param out 输出数组（长度 >= GW_PHASE_COUNT），从高优先级到低优先级
 *
 * 排序依据：**最大耗时降序**。
 * 理由：最坏情况耗时长的段，如果被低优先级任务阻塞，影响最大；
 * 把它放高优先级能让它的最坏情况更可控。
 *
 * ⚠️ 这只是**建议**，不是自动配置 —— 真正的优先级还要考虑
 *    "有没有硬超时要求"（RS485 从站有，主站没有）。
 *    本函数把统计事实摆出来，决策仍由人做。
 */
void gw_timing_priority_hint(const gw_timing_t *tm, gw_phase_t *out);

#endif /* GW_TIMING_H */
