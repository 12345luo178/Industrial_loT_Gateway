/**
 * @file    gw_uplink.h
 * @brief   上云链路管理：遥测上报 + 断线缓存补传 + 重连退避（纯逻辑可测）
 *
 * ============================================================================
 *  这个模块解决什么问题
 *
 *  没有它的话，"上报遥测"就是"把数据丢给 ESP32"这么简单。
 *  但现场的真实情况是：
 *
 *    - Wi-Fi 会断（路由器重启、信号衰减、AP 切换）
 *    - Broker 会踢连接（鉴权过期、心跳超时）
 *    - 断网可能持续几分钟到几小时
 *    - 上行链路（STM32->ESP32）本身也可能丢帧
 *
 *  所以需要回答四个问题：
 *    1) 断网期间的数据放哪？（环形缓存，满了怎么办要有明确策略）
 *    2) 怎么知道"现在能发"？（链路状态由 ESP32 上报，不是一个布尔量）
 *    3) 重连失败后多久再试？（指数退避，但有上限；鉴权失败要停止重试）
 *    4) 恢复后怎么补？（按序号顺序补传，且不能阻塞实时数据）
 *
 *  ----------------------------------------------------------------------------
 *  与"主站离线退避"的区别（容易混淆，所以写清楚）
 *
 *    modbus_master 的离线退避  针对的是**下游从机**（RS485 上的仪表）
 *    gw_uplink 的重连退避      针对的是**上游链路**（Wi-Fi/MQTT）
 *
 *  两者的策略类似（连续失败 -> 退避 -> 探测 -> 恢复），
 *  但作用对象、时间尺度、失败后果完全不同，所以是两个独立模块。
 *  共用一套"退避"实现反而会让两者的参数互相牵制。
 *
 *  ----------------------------------------------------------------------------
 *  环形缓存的溢出策略（必须明确，否则"不丢数据"是假话）
 *
 *  满了之后只有两种选择：
 *    (a) 丢最新 —— 保留历史，但设备"失忆"，恢复后补的是旧数据
 *    (b) 丢最旧 —— 保留最新，历史出现空洞
 *
 *  本项目选 **(b) 丢最旧**：巡检类数据越新越有价值，
 *  而且丢掉的条数会通过 IREG_OFFLINE_DROP 上报到寄存器表，
 *  运维能看见"丢了多少"，而不是以为"一条没丢"。
 *  **把丢包计数暴露出来，比假装不丢包更重要。**
 * ============================================================================
 */
#ifndef GW_UPLINK_H
#define GW_UPLINK_H

#include <stdint.h>

#include "gw_frame.h"

/* ---------------- 规模与时间参数 ---------------- */
#define GW_UPLINK_RING_DEPTH     512u   /* 环形缓存条数（512 × 28B ≈ 14KB） */

/* 重连退避序列（秒）：1, 2, 4, 8, 16, 32, 60（上限 60） */
#define GW_UPLINK_BACKOFF_MAX_S  60u

/* 心跳：即使没有遥测也要定期发，让两端都知道链路还活着 */
#define GW_UPLINK_HEARTBEAT_S    30u

/* 发送后等待 ACK 的超时（毫秒） */
#define GW_UPLINK_ACK_TIMEOUT_MS 2000u

/* 单帧最多重发次数（超过则本次上报失败，计入统计） */
#define GW_UPLINK_MAX_RETRY      3u

/* 补传时每批最多发多少条 —— 避免恢复瞬间把链路打满，
   也避免 STM32 长时间占用串口导致下行指令被延迟处理 */
#define GW_UPLINK_FLUSH_BATCH    8u

/* ---------------- 链路状态（与 IREG_MQTT_STATUS 语义一致） ---------------- */
typedef enum {
    GW_LINK_DISCONNECTED = 0,   /* 未连接 */
    GW_LINK_CONNECTING   = 1,   /* 连接中（等待 ESP32 上报结果） */
    GW_LINK_CONNECTED    = 2,   /* 已连接，可发数据 */
    GW_LINK_AUTH_FAILED  = 3    /* 鉴权失败：停止自动重试，等上层处理 */
} gw_link_state_t;

/* ---------------- 结果码 ---------------- */
typedef enum {
    GW_UPLINK_OK            = 0,
    GW_UPLINK_ERR_PARAM     = -1,
    GW_UPLINK_ERR_OFFLINE   = -2,   /* 链路未连接，无法发送 */
    GW_UPLINK_ERR_TIMEOUT   = -3,   /* 等待 ACK 超时 */
    GW_UPLINK_ERR_SEND      = -4    /* 发送回调失败 */
} gw_uplink_err_t;

/* ---------------- 统计量（诊断 + 实测数据来源） ---------------- */
typedef struct {
    uint32_t queued;            /* 累计入队条数 */
    uint32_t sent_ok;           /* 成功上报条数（收到 ACK） */
    uint32_t send_fail;         /* 重试用尽后仍失败的条数 */
    uint32_t dropped_overflow;  /* 因缓存满而丢弃的最旧条数 */
    uint32_t heartbeats;        /* 心跳发送次数 */
    uint32_t reconnect_tries;   /* 重连尝试次数 */
    uint32_t reconnects;        /* 重连成功次数 */
    uint32_t backoff_events;    /* 进入退避的次数 */
    uint16_t last_rtt_ms;       /* 最近一次 ACK 往返时间 */
    uint16_t max_rtt_ms;        /* 历史最大 RTT（现场排查看最慢的一次） */
    uint32_t last_offline_s;    /* 最近一次断网持续时长（秒） */
} gw_uplink_stat_t;

/* ---------------- 上下行回调（由上层注入，便于单测） ---------------- */
typedef struct {
    /**
     * 发送一帧到 ESP32
     * @return 0 成功，非 0 失败
     */
    int (*send)(void *ctx, const uint8_t *buf, uint16_t len);

    /**
     * 取当前毫秒时间戳（单调递增）
     *
     * 用函数指针而不是直接调 HAL_GetTick：这样单元测试可以把
     * "断网 10 分钟后的补传"在毫秒内跑完且完全确定。
     */
    uint32_t (*now_ms)(void *ctx);

    void *ctx;
} gw_uplink_io_t;

/* ---------------- 链路上下文 ---------------- */
typedef struct {
    /* 环形缓存（存遥测结构体，不存字节流 —— 便于按序补传与去重） */
    gw_telemetry_t ring[GW_UPLINK_RING_DEPTH];
    uint16_t       head;            /* 入队位置 */
    uint16_t       tail;            /* 出队位置 */
    uint16_t       count;           /* 当前积压条数 */

    /* 序号：单调递增，供云端去重（QoS1 可能重复投递） */
    uint32_t       next_seq;
    uint32_t       dev_id;          /* 设备 ID，写入每条遥测 */

    /* 链路状态机 */
    gw_link_state_t state;
    uint32_t       state_ms;        /* 进入当前状态的时间戳 */
    uint32_t       backoff_s;       /* 当前退避时长（秒） */
    uint32_t       offline_since_s; /* 断网起点（秒，0 表示未断网） */

    /* 心跳与等待 ACK */
    uint32_t       hb_ms;           /* 上次心跳的时间戳 */
    uint8_t        awaiting_ack;
    uint8_t        retry_left;
    uint32_t       ack_ms;          /* 发出待确认帧的时间戳 */
    gw_telemetry_t pending;         /* 正在等确认的那一帧（见 .c 开头的说明） */

    /* 时间戳（毫秒），由 tick 维护 */
    uint32_t       last_ms;
    uint32_t       ms_acc;          /* 毫秒累加器（满 1000 进位到 uptime_s） */
    uint32_t       uptime_s;        /* 内部秒计数（用于退避与心跳判断） */

    /* 补传节流：本批已发条数 */
    uint8_t        flush_in_batch;

    /* 延迟处理的入站帧（见 .c 里 on_frame 的说明）。
       用途：等 ACK 期间若收到"链路已连接"的通知，不能当场切换状态
       （会把刚发出的请求作废），先存起来，等 ACK 处理完再应用。 */
    uint8_t        defer_valid;
    uint8_t        defer_type;
    uint8_t        defer_payload[4];
    uint16_t       defer_len;

    gw_uplink_io_t  io;
    gw_uplink_stat_t stat;
    uint8_t        inited;
} gw_uplink_t;

/* ==================================================================
 *  接口
 * ================================================================== */

/**
 * 初始化上云链路
 * @param u   上下文
 * @param io  发送与时间回调
 * @param dev_id 设备 ID（写入每条遥测）
 */
gw_uplink_err_t gw_uplink_init(gw_uplink_t *u, const gw_uplink_io_t *io,
                               uint32_t dev_id);

/**
 * 周期推进（在主循环或任务里调用）
 *
 * 负责：
 *   - 维护毫秒/秒计数
 *   - 链路断开时按退避策略触发重连（通过发送 REQUEST_CONNECT 帧）
 *   - 连接正常时按周期发心跳
 *   - 处理"发送后等 ACK"的超时与重试
 *   - 有积压且链路正常时按批补传
 *
 * @param u          上下文
 * @param elapsed_ms 距上次调用经过的毫秒数
 */
void gw_uplink_tick(gw_uplink_t *u, uint32_t elapsed_ms);

/**
 * 提交一条遥测（入队 + 在条件允许时立即尝试发送）
 *
 * @param u   上下文
 * @param t   遥测数据（seq 字段由本模块填充，调用方不必设）
 * @return GW_UPLINK_OK 已入队；GW_UPLINK_ERR_PARAM 入参非法
 *
 * @note 无论链路是否连接都会入队成功 —— 这样调用方不必关心链路状态。
 *       断开时数据留在环形缓存里，恢复后自动补传。
 */
gw_uplink_err_t gw_uplink_submit(gw_uplink_t *u, gw_telemetry_t *t);

/**
 * 处理来自 ESP32 的帧（链路状态、ACK、指令等）
 *
 * @param u          上下文
 * @param type       帧类型
 * @param payload    载荷
 * @param len        载荷长度
 * @return GW_UPLINK_OK 已处理；GW_UPLINK_ERR_PARAM 无法识别（由上层决定如何处理）
 *
 * 本模块处理：GW_FT_LINK_STATUS / GW_FT_ACK / GW_FT_NACK
 * 其它类型（如 GW_FT_COMMAND）返回 ERR_PARAM，交给上层业务处理 ——
 * **本模块不解释业务指令**，只负责链路。
 */
gw_uplink_err_t gw_uplink_on_frame(gw_uplink_t *u, uint8_t type,
                                   const uint8_t *payload, uint16_t len);

/** 供上层主动上报链路状态（例如 ESP32 重启后需要重新告知） */
void gw_uplink_set_link_state(gw_uplink_t *u, gw_link_state_t st);

/** 当前链路状态 */
gw_link_state_t gw_uplink_state(const gw_uplink_t *u);

/** 当前积压条数（用于写入 IREG_OFFLINE_DEPTH） */
uint16_t gw_uplink_pending(const gw_uplink_t *u);

/** 取统计量（用于写入 IREG_OFFLINE_DROP 等诊断寄存器） */
const gw_uplink_stat_t *gw_uplink_stats(const gw_uplink_t *u);

/** 取当前退避时长（秒），用于日志与诊断 */
uint32_t gw_uplink_backoff_s(const gw_uplink_t *u);

/** 错误码转字符串 */
const char *gw_uplink_strerror(gw_uplink_err_t e);

/** 链路状态转字符串 */
const char *gw_link_state_str(gw_link_state_t st);

#endif /* GW_UPLINK_H */
