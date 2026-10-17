/**
 * @file    modbus_master.h
 * @brief   Modbus RTU 主站协议引擎（与硬件完全解耦，可在 PC 上单元测试）
 *
 * ============================================================================
 *  为什么主站比从站更重要？
 *
 *  只做从站，本设备只是一个"带 Modbus 接口的采集终端" ——
 *  等上位机来问，被动应答。
 *  有了主站，它才能主动去轮询现场的多台仪表/控制器，把数据汇聚上来，
 *  这时它才是**网关**。
 *
 *  从站与主站的技术难点完全不同：
 *    从站：被动、无状态（一问一答），难点在协议正确性与异常码
 *    主站：主动、有状态（轮询调度、超时、重试、故障隔离），
 *          难点在**时序与状态机**，以及"某台从机坏了不能拖垮整条总线"
 *
 * ----------------------------------------------------------------------------
 *  设计要点
 *
 *  1) 与从站一样只做"字节进 -> 字节出"：
 *     发送由调用方执行（通过 send 回调），本模块不认识 UART，也不碰 DE 引脚。
 *     这样它能在 PC 上完整单测。
 *
 *  2) 时间由外部推进（modbus_master_tick 传入 elapsed_ms）：
 *     不使用任何全局时钟、不依赖 HAL_GetTick。
 *     好处是单元测试可以把"3 秒超时"在微秒内跑完，并且完全确定。
 *
 *  3) 故障隔离：连续失败达阈值的从机进入"离线退避"状态，
 *     被跳过若干轮才重试一次。否则一台掉线的从机会让轮询周期
 *     被超时拖长数倍，在线从机的数据刷新率跟着下降 ——
 *     这是现场最常见的"网关变慢"根因。
 *
 *  4) 只保留一个"当前请求"，不做请求队列：
 *     Modbus RTU 是半双工串行总线，同一时刻总线上只能有一个请求。
 *     维护队列反而会掩盖"上一次还没结束"的错误。
 * ============================================================================
 */
#ifndef MODBUS_MASTER_H
#define MODBUS_MASTER_H

#include <stdint.h>

/* ---------------- 规模上限（可按需调整） ---------------- */
#define MB_M_MAX_SLAVES     16u     /* 最多管理的从机数 */
#define MB_M_MAX_POLL_ITEMS 32u     /* 最多轮询项数 */

/* ---------------- 健康度阈值 ---------------- */
#define MB_M_FAIL_TO_OFFLINE 3u     /* 连续失败多少次判定为离线 */
#define MB_M_OFFLINE_SKIP    5u     /* 离线后跳过多少轮才重试一次 */

/* ---------------- 错误码 ---------------- */
typedef enum {
    MB_M_OK              = 0,
    MB_M_ERR_PARAM       = -1,  /* 入参非法 */
    MB_M_ERR_TIMEOUT     = -2,  /* 等待响应超时 */
    MB_M_ERR_CRC         = -3,  /* 响应 CRC 校验失败 */
    MB_M_ERR_ADDR        = -4,  /* 响应站号与请求不符 */
    MB_M_ERR_FUNC        = -5,  /* 响应功能码与请求不符 */
    MB_M_ERR_LEN         = -6,  /* 响应长度非法 */
    MB_M_ERR_EXCEPTION   = -7,  /* 从机返回异常响应 */
    MB_M_ERR_BUSY        = -8   /* 上一请求尚未结束 */
} mb_master_err_t;

/* ---------------- 轮询项：读什么、放到哪 ---------------- */
typedef struct {
    uint8_t  slave;         /* 从机地址 1~247 */
    uint8_t  func;          /* 0x03 读保持寄存器 / 0x04 读输入寄存器 */
    uint16_t start;         /* 从机侧的起始寄存器地址 */
    uint16_t qty;           /* 读取数量，1~125 */
    uint16_t map_base;      /* 读回的数据写入本地寄存器表的起始偏移 */
    uint8_t  map_area;      /* 0 = 输入寄存器区，1 = 保持寄存器区 */
} mb_m_poll_item_t;

/* ---------------- 单台从机的运行状态 ---------------- */
typedef struct {
    uint8_t  online;            /* 1 = 在线（最近一次请求成功） */
    uint8_t  fail_streak;       /* 连续失败次数 */
    uint8_t  skip_count;        /* 离线退避：还需跳过多少轮 */
    uint8_t  last_err;          /* 最近一次错误码（MB_M_*），用于诊断 */
    uint16_t last_rtt_ms;       /* 最近一次响应时间 */
    uint16_t max_rtt_ms;        /* 历史最大响应时间 */
    uint32_t cnt_ok;            /* 累计成功次数 */
    uint32_t cnt_fail;          /* 累计失败次数 */
    uint32_t cnt_timeout;       /* 累计超时次数 */
    uint32_t cnt_exception;     /* 累计异常响应次数 */
} mb_m_slave_stat_t;

/* ---------------- 主站总统计 ---------------- */
typedef struct {
    uint32_t cnt_requests;      /* 发出的请求总数 */
    uint32_t cnt_responses;     /* 收到的合法响应总数 */
    uint32_t cnt_retries;       /* 重试次数 */
    uint32_t cnt_poll_rounds;   /* 完成的轮询轮数 */
    uint32_t cnt_recover;       /* 从机恢复次数（离线->在线） */
    uint32_t cnt_offline;       /* 从机转离线次数 */
} mb_m_stats_t;

/* ---------------- 主站上下文 ---------------- */
typedef struct {
    /* 配置 */
    mb_m_poll_item_t  items[MB_M_MAX_POLL_ITEMS];
    uint16_t          item_count;
    uint16_t          timeout_ms;       /* 单次请求等待响应的超时 */
    uint8_t           max_retry;        /* 单次请求最大重试次数（0 = 不重试） */

    /**
     * 轮询节拍：两次请求之间**至少要空出多少毫秒**
     *
     * 0 = 不限制（一问一答连着发，旧行为）。
     *
     * ⚠️ 为什么必须有这个字段（2026-10-10 上板实测）：
     *    下游从机应答够快时（PC 模拟从机 ~2ms），主站会**全速轮询** ——
     *    实测 177 次/秒、CPU 负载 1000‰、单轮耗时打满。
     *    那不是"性能好"，而是：
     *      - 把 RS485 总线长期占着（同总线上其它节点没机会说话）
     *      - 现场仪表承受每秒上百次无意义访问
     *      - 主循环没有余量，将来加上云/OTA/多从机就会互相挤
     *    一个真实网关的轮询周期是**秒级**，不是"能多快就多快"。
     */
    uint16_t          poll_interval_ms;

    /* 运行状态 */
    uint8_t           cur_index;        /* 当前轮询项下标 */
    uint8_t           retry_left;       /* 本次请求剩余重试次数 */
    uint8_t           awaiting;         /* 1 = 已发出请求，等待响应 */
    uint8_t           force_retry;      /* 1 = 本次发送是在"离线重试"，成功后要清退避计数 */
    uint16_t          elapsed_ms;       /* 当前请求已等待时间 */
    uint16_t          idle_ms;          /* 距上一次请求发出的空闲时间（节拍用） */
    uint8_t           req_buf[8];       /* 请求帧（固定 8 字节） */

    /* 统计 */
    /* ---- 从机状态表 ----
     *
     * 按"已登记的从机"紧凑存储，而不是按地址 1..247 开大数组。
     *
     * 踩过的坑：早期版本写成 `mb_m_slave_stat_t slave[248]`，
     * 一个数组就占 248 × 25 ≈ 6.2 KB ——
     * 而 MB_M_MAX_SLAVES 明明定义的是 16（最多管 16 台从机）。
     * 这 6.2 KB 在 64 KB RAM 里占了近 10%，纯粹是浪费：
     * 既无必要（没人会把 248 台仪表挂同一条 485 总线），
     * 又与模块自己的规模常量自相矛盾。
     *
     * 现在用 addr[] 记地址、stat[] 记状态，靠 addr_to_slot() 映射。
     * 查表是 O(n) 但 n ≤ 16，开销可忽略。
     * addr[i] == 0 表示该槽为空（Modbus 合法从机地址是 1..247）。 */
    uint8_t           addr[MB_M_MAX_SLAVES];
    mb_m_slave_stat_t stat[MB_M_MAX_SLAVES];
    uint8_t           slave_count;      /* 已登记的从机数 */
    mb_m_stats_t      stats;

    /* 输出回调：把请求帧交给移植层发送。返回 0 = 成功，非 0 = 失败。
       用 ctx 而不是全局变量，保证可重入、可多实例、可单测。 */
    int  (*send)(void *ctx, const uint8_t *buf, uint16_t len);
    void  *send_ctx;

    /* 输出回调：把读回的数据写入本地寄存器表。
       返回 0 = 成功，非 0 = 越界/非法（视为本次轮询失败）。
       之所以做成回调而不是直接操作 mb_regs_t，
       是为了让协议层不依赖寄存器表的布局（保持低耦合）。 */
    int  (*store)(void *ctx, uint8_t area, uint16_t base,
                  const uint16_t *values, uint16_t qty);
    void  *store_ctx;
} mb_master_t;

/* ==========================================================================
 *  初始化和配置
 * ========================================================================== */

/**
 * 初始化主站
 * @param m          主站上下文（调用方分配，通常为静态变量）
 * @param send       发送回调（必须提供）
 * @param send_ctx   传给发送回调的上下文
 * @param store      寄存器写入回调（必须提供）
 * @param store_ctx  传给写入回调的上下文
 * @return MB_M_OK 或 MB_M_ERR_PARAM
 */
mb_master_err_t mb_master_init(mb_master_t *m,
                               int (*send)(void *ctx, const uint8_t *buf, uint16_t len),
                               void *send_ctx,
                               int (*store)(void *ctx, uint8_t area, uint16_t base,
                                            const uint16_t *values, uint16_t qty),
                               void *store_ctx);

/**
 * 添加一个轮询项
 *
 * 校验：地址 1~247、功能码 0x03/0x04、数量 1~125、映射区 0/1、未超出容量。
 *
 * @return MB_M_OK / MB_M_ERR_PARAM
 */
mb_master_err_t mb_master_add_item(mb_master_t *m, const mb_m_poll_item_t *item);

/** 清空轮询表（保留统计量） */
void mb_master_clear_items(mb_master_t *m);

/** 设置超时与重试（超时范围 10~5000ms，重试 0~5 次） */
mb_master_err_t mb_master_set_timing(mb_master_t *m, uint16_t timeout_ms, uint8_t max_retry);

/**
 * 设置轮询节拍：两次请求之间至少空出多少毫秒
 *
 * @param m           主站上下文
 * @param interval_ms 间隔（0 ~ MB_M_MAX_POLL_INTERVAL_MS）
 *                    0 = 不限制（一问一答连着发；向后兼容旧行为）
 * @return MB_M_OK / MB_M_ERR_PARAM
 *
 * 语义（很重要，决定了"节拍从什么时候开始算"）：
 *   它保证**上一次事务结束之后**（收到响应 / 超时收尾 / 重试耗尽）
 *   至少空闲 interval_ms 才发下一个请求。
 *   所以它是"**请求之间的最小空闲**"，不是"固定发送周期"：
 *   从机响应慢的时候周期会自动变长（这是期望的行为，
 *   否则慢从机会被追着发，总线更堵）。
 *
 * ⚠️ 首次请求不受节拍限制（初始空闲视为"已经等够了"），
 *    否则上电后要干等一个节拍才开始轮询。
 *
 * ⚠️ 发送失败**不**重新计时：失败的那一帧没上总线，下一个 poll
 *    应当立刻重试，而不是再等一个间隔。
 *
 * ⚠️ 与 timeout_ms 是两件事：
 *    timeout_ms   单次请求等响应的上限（超时 -> 重试/失败）
 *    本函数       两次请求之间的礼貌间隔
 */
mb_master_err_t mb_master_set_interval(mb_master_t *m, uint16_t interval_ms);

/** 轮询节拍的上限（毫秒）。超过它没有意义，也避免 uint16 溢出带来的歧义 */
#define MB_M_MAX_POLL_INTERVAL_MS   60000u

/* ==========================================================================
 *  运行接口
 * ========================================================================== */

/**
 * 推进主站状态机（应在主循环或周期任务里调用）
 *
 * @param m          主站上下文
 * @param elapsed_ms 距上次调用经过的毫秒数
 *
 * 行为：
 *   - 空闲时：取下一个轮询项（自动跳过处于退避期的离线从机）并发起请求
 *   - 等待中：累加 elapsed_ms，超时则重试；重试耗尽则标记失败并推进
 *   - 若发送回调失败，等同一次失败处理
 *
 * @note 本函数不做任何阻塞等待，必须在循环里被周期调用。
 */
void mb_master_poll(mb_master_t *m, uint16_t elapsed_ms);

/**
 * 把收到的响应帧交给主站处理
 *
 * @param m    主站上下文
 * @param buf  响应帧（含末尾 CRC）
 * @param len  帧长
 * @return MB_M_OK 表示本次请求成功完成；
 *         其它值为错误码，且**已经计入统计并推进到下一项**
 *
 * @note 即使不在等待状态（awaiting=0）也可以安全调用，
 *       此时会按"响应站号与当前项不符"处理，不会破坏状态机。
 */
mb_master_err_t mb_master_on_response(mb_master_t *m, const uint8_t *buf, uint16_t len);

/* ==========================================================================
 *  查询接口（用于诊断与寄存器上报）
 * ========================================================================== */

/** 取某台从机的状态；slave 超范围返回 NULL */
const mb_m_slave_stat_t *mb_master_slave_stat(const mb_master_t *m, uint8_t slave);

/** 取总统计量 */
const mb_m_stats_t *mb_master_stats(const mb_master_t *m);

/** 在线从机位图：bit0 = 从机 1，bit(n-1) = 从机 n（最多 16 台） */
uint16_t mb_master_online_bitmap(const mb_master_t *m);

/** 主站是否空闲（可用它判断"能否安全占用总线"） */
int mb_master_is_idle(const mb_master_t *m);

/** 当前轮询进度（已完成的项数 / 总项数），用于上报告警 */
uint16_t mb_master_current_index(const mb_master_t *m);

/** 错误码转可读字符串（日志用） */
const char *mb_master_strerror(mb_master_err_t err);

/**
 * 放弃当前正在等待的请求（不判定为从机失败）
 *
 * ============================================================================
 *  为什么需要这个接口，而不是"就当超时处理"
 *
 *  主站发完请求后会占着总线等响应。如果此时本机（作为从站）需要响应
 *  上位机的请求，总线仲裁会**抢占**这个等待窗口（从站有硬超时，主站可以等）。
 *
 *  抢占之后如果直接走超时路径，会出两个错：
 *    1) 把"我们自己主动让出总线"记成**从机响应超时** ——
 *       从机的 fail_streak 会增加，连续几次就把一台正常的从机判成离线
 *    2) 轮询项会跳过去，那台从机的数据在这一轮就没了
 *
 *  正确做法：放弃等待，但**保留 cur_index 与 retry_left**，
 *  下一轮 tick 重新发同一个请求 —— 对从机而言完全无感。
 *
 *  @return 0 = 确实放弃了一个在等的请求；-1 = 当时并没有在等（无需处理）
 * ============================================================================
 */
int mb_master_abort_pending(mb_master_t *m);

/**
 * 取已登记的从机数量
 *
 * 供应用层遍历从机状态（例如把最近错误上报到寄存器表）。
 * 之所以不直接暴露 addr[]/stat[]：那些数组的内部布局属于本模块的实现细节，
 * 上层只该按"第 i 台从机"来访问。
 */
uint8_t mb_master_slave_count(const mb_master_t *m);

/**
 * 按序号取已登记从机的地址
 * @return 0 成功（*addr 已填）；-1 序号越界
 */
int mb_master_slave_at(const mb_master_t *m, uint8_t index, uint8_t *addr);

#endif /* MODBUS_MASTER_H */
