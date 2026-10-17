/**
 * @file    gw_frame.h
 * @brief   STM32 与 ESP32 之间的串口帧协议（含粘包/半包/噪声重同步）
 *
 * ============================================================================
 *  为什么串口上"收一帧"比想象中难
 *
 *  串口是**字节流**，不是消息流。上位机一次 read 拿到的可能是：
 *
 *    (a) 恰好一帧                     —— 最理想，但不能假设总是这样
 *    (b) 半帧（后半段还在路上）
 *    (c) 两帧甚至三帧粘在一起
 *    (d) 帧前面有几个噪声字节（上电毛刺、波特率切换残留）
 *    (e) 帧中间丢了几个字节（缓冲区溢出）
 *
 *  所以"按帧头找到 → 读长度 → 收够再解析"这一套是必需的，
 *  而且必须处理"帧头出现在载荷中间"这种假同步：
 *
 *     [A5 05 01 99 A5 02 ...]     载荷里出现 0xA5，不能当成新帧头
 *
 *  ----------------------------------------------------------------------------
 *  帧格式
 *
 *    偏移  长度  字段    说明
 *    ----  ----  ------  ------------------------------------------
 *    0     1     HEAD    固定 0xA5
 *    1     2     LEN     TYPE + PAYLOAD 的总长度（大端）
 *    3     1     TYPE    帧类型
 *    4     LEN-1 PAYLOAD 载荷（LEN-1 字节）
 *    4+LEN 1     CRC8    覆盖 LEN_L + LEN_H + TYPE + PAYLOAD
 *
 *  为什么用"长度 + CRC8"而不是转义（byte stuffing）：
 *    转义要逐字节判断与改写，收发两侧都得做，代码更复杂、开销更大；
 *    而"长度 + 校验"只需在收齐后验一次 CRC，
 *    代价是载荷里可以出现 0xA5（由长度字段兜住，不会误解）。
 *    代价与收益相比明显更划算。
 *
 *  ----------------------------------------------------------------------------
 *  重同步策略（本模块最容易被忽略、也最容易出错的地方）
 *
 *  收到坏帧时**不能整块丢弃**，而要：
 *    1) 从"帧头之后一个字节"开始重新搜索 0xA5
 *    2) 保留缓冲区里的剩余字节（它可能是下一帧的开头）
 *
 *  否则一旦错位，后面所有正确的帧都会被连带丢掉 ——
 *  表现为"通信偶尔中断，需要重启才能恢复"。
 *
 *  本模块是纯逻辑：字节进、帧出，不依赖 HAL、不依赖串口。
 * ============================================================================
 */
#ifndef GW_FRAME_H
#define GW_FRAME_H

#include <stdint.h>

/* ---------------- 帧头与规模上限 ---------------- */
#define GW_FRAME_HEAD           0xA5u
#define GW_FRAME_MAX_PAYLOAD    128u    /* 载荷上限：够放遥测结构体与指令 */
#define GW_FRAME_MAX_LEN        (GW_FRAME_MAX_PAYLOAD + 1u)     /* +TYPE */
#define GW_FRAME_MAX_TOTAL      (GW_FRAME_MAX_LEN + 5u)         /* +HEAD+LEN+CRC */
#define GW_FRAME_RX_BUF_SIZE    256u    /* 接收缓冲区（可容纳多个帧） */

/* ---------------- 帧类型 ---------------- */
typedef enum {
    GW_FT_NONE          = 0x00,

    /* --- STM32 -> ESP32 --- */
    GW_FT_WIFI_CONFIG   = 0x01,     /* 配网：SSID + '\0' + PASSWORD */
    GW_FT_MQTT_CONFIG   = 0x02,     /* MQTT 参数：host + '\0' + 序列化的端口等 */
    GW_FT_TELEMETRY     = 0x03,     /* 遥测数据（固定长度二进制结构体） */
    GW_FT_STATUS_SET    = 0x04,     /* 上报周期等运行参数 */
    GW_FT_OTA_BEGIN     = 0x05,     /* OTA：开始（版本 + 总长度 + CRC32） */
    GW_FT_OTA_CHUNK     = 0x06,     /* OTA：数据分片 */
    GW_FT_OTA_END       = 0x07,     /* OTA：结束 */

    /* --- ESP32 -> STM32 --- */
    GW_FT_LINK_STATUS   = 0x81,     /* 链路状态：Wi-Fi/MQTT 状态 + RSSI */
    GW_FT_COMMAND       = 0x82,     /* 下行指令（文本，由应用层解释） */
    GW_FT_OTA_ACK       = 0x83,     /* OTA 分片确认 */
    GW_FT_LOG           = 0x84,     /* ESP32 侧日志转发 */

    /* --- 双向 --- */
    /**
     * 遥测送达确认（ESP32 -> STM32）
     *
     * ⚠️ 载荷**必须为空**。上云链路（`gw_uplink.c`）据此清 `awaiting_ack`
     *    并计入 `sent_ok`。
     */
    GW_FT_ACK           = 0x7F,

    /**
     * 遥测被拒绝（ESP32 -> STM32）
     *
     * 上云链路据此**停止等待、不重试**，并计入 `send_fail`。
     */
    GW_FT_NACK          = 0x7E,

    /**
     * ★ 下行指令执行成功（STM32 -> ESP32）
     *
     * 载荷是文本，形如 `"PERIOD OK"`。由 ESP32 转发到云端。
     *
     * ============================================================================
     *  ⚠️ 为什么指令确认要**独立两个帧类型**，而不是复用 ACK/NACK
     *
     *  这两个类型曾经与 ACK/NACK 共用，导致一个**真实缺陷**：
     *
     *    `gw_uplink_on_frame()` 对 ACK/NACK 的语义是"遥测送达/被拒"，
     *    而且它**不判载荷**（只看类型）。所以：
     *
     *      场景：STM32 发出遥测 T1，进入 awaiting_ack；
     *            此时收到一条**下行指令**并按了 `REBOOT`，
     *            但令牌错了 -> STM32 回 `GW_FT_NACK`
     *
     *      结果：上云链路把这条 NACK 当成"T1 被拒"，
     *            清掉 awaiting_ack、计入 send_fail、
     *            **并且会重传 T1**（因为它以为对端不接受）。
     *
     *    一次"指令令牌错误"就这样变成了一次"遥测重传" ——
     *    统计和重试逻辑同时被污染，而现场完全看不出关联。
     *
     *  分开之后：指令的成败走 `GW_FT_CMD_ACK` / `GW_FT_CMD_NACK`，
     *  上云链路不认识这两个类型，`default` 分支返回 `ERR_PARAM`
     *  让上层（应用层）处理 —— 边界清晰，互不干扰。
     * ============================================================================
     */
    GW_FT_CMD_ACK       = 0x85,

    /** ★ 下行指令执行失败（STM32 -> ESP32）。载荷 = 文本，如 `"SAVE BAD_TOKEN"` */
    GW_FT_CMD_NACK      = 0x86
} gw_frame_type_t;

/* ---------------- 结果码 ---------------- */
typedef enum {
    GW_FRAME_OK             = 0,
    GW_FRAME_ERR_PARAM      = -1,
    GW_FRAME_ERR_TOO_BIG    = -2,   /* 载荷超限 */
    GW_FRAME_ERR_BAD_LEN    = -3,   /* 长度字段非法 */
    GW_FRAME_ERR_CRC        = -4,   /* CRC8 校验失败 */
    GW_FRAME_ERR_NO_SPACE   = -5    /* 发送缓冲不足 */
} gw_frame_err_t;

/* ---------------- 接收解析状态 ---------------- */
typedef enum {
    GW_RX_WAIT_HEAD  = 0,   /* 搜索帧头 */
    GW_RX_WAIT_LEN   = 1,   /* 收长度低字节 */
    GW_RX_WAIT_LEN2  = 2,   /* 收长度高字节 */
    GW_RX_WAIT_TYPE  = 3,   /* 收类型 */
    GW_RX_WAIT_BODY  = 4,   /* 收载荷 */
    GW_RX_WAIT_CRC   = 5    /* 收 CRC */
} gw_rx_state_t;

/* ---------------- 统计量（诊断 + 实测数据来源） ---------------- */
typedef struct {
    uint32_t rx_frames;         /* 成功解析的帧数 */
    uint32_t rx_bytes;          /* 累计喂入字节数 */
    uint32_t rx_crc_err;        /* CRC 错误次数 */
    uint32_t rx_len_err;        /* 长度字段非法次数 */
    uint32_t rx_garbage;        /* 被丢弃的噪声字节数 */
    uint32_t rx_resync;         /* 重同步次数（帧头重新搜索） */
    uint32_t rx_dropped;        /* 因缓冲区满而丢弃的字节数 */
    uint32_t tx_frames;         /* 成功组帧数 */
} gw_frame_stat_t;

/* ---------------- 解析器 ---------------- */
typedef struct {
    /* 接收：状态机 + 逐字节累积 */
    gw_rx_state_t state;
    uint8_t       buf[GW_FRAME_MAX_TOTAL];
    uint16_t      idx;              /* 当前帧已收字节数 */
    uint16_t      need;             /* 当前帧总长（含头/长度/CRC） */

    /* 一次 feed 里若粘了多帧，第一帧收满时会剩下若干字节属于后续帧。
       它们被暂存在这里，下次 feed 时优先处理 ——
       这样调用方只需"feed 一次 + 反复 take"，不必自己管理剩余字节。 */
    uint8_t       pending[GW_FRAME_MAX_TOTAL];
    uint16_t      pending_len;

    /* 收到完整帧时的输出 */
    uint8_t       out_type;
    uint8_t       out_payload[GW_FRAME_MAX_PAYLOAD];
    uint16_t      out_len;          /* 载荷长度 */

    gw_frame_stat_t stat;
} gw_frame_parser_t;

/* ==================================================================
 *  纯逻辑接口
 * ================================================================== */

/** 初始化解析器 */
void gw_frame_parser_init(gw_frame_parser_t *p);

/**
 * 喂入字节流（可在任意边界切分，内部自己处理粘包与半包）
 *
 * @param p    解析器
 * @param data 字节流
 * @param len  字节数
 *
 * @note 本函数只负责"累积并识别帧"，**不回调**。
 *       调用方喂完之后反复调 gw_frame_take() 取帧即可。
 *       这样设计是为了避免在解析过程中引入回调重入问题。
 */
void gw_frame_feed(gw_frame_parser_t *p, const uint8_t *data, uint16_t len);

/**
 * 取出一帧（非阻塞）
 *
 * @param p         解析器
 * @param out_type  帧类型
 * @param payload   载荷输出缓冲
 * @param cap       payload 缓冲容量
 * @param out_len   载荷长度
 * @return GW_FRAME_OK 取到一帧；GW_FRAME_ERR_NO_SPACE 当前没有完整帧
 *
 * @note 载荷超长时返回 ERR_TOO_BIG，但**已收的帧被丢弃**，状态机复位，
 *       继续处理后续字节（不会卡死）。
 */
gw_frame_err_t gw_frame_take(gw_frame_parser_t *p, uint8_t *out_type,
                             uint8_t *payload, uint16_t cap, uint16_t *out_len);

/**
 * 组一帧
 *
 * @param out       输出缓冲
 * @param cap       缓冲容量
 * @param type      帧类型
 * @param payload   载荷（可为 NULL，此时 payload_len 必须为 0）
 * @param payload_len 载荷长度
 * @param out_len   实际帧长度
 * @return GW_FRAME_OK / ERR_TOO_BIG / ERR_PARAM
 */
gw_frame_err_t gw_frame_build(uint8_t *out, uint16_t cap, uint8_t type,
                              const uint8_t *payload, uint16_t payload_len,
                              uint16_t *out_len);

/** 取统计量 */
const gw_frame_stat_t *gw_frame_stats(const gw_frame_parser_t *p);

/** 错误码转字符串 */
const char *gw_frame_strerror(gw_frame_err_t e);

/** 帧类型转字符串（日志用） */
const char *gw_frame_type_str(uint8_t type);

/* ==================================================================
 *  遥测数据结构（STM32 与 ESP32 之间的二进制契约）
 *
 *  为什么用固定长度二进制而不是直接传 JSON 字符串：
 *    1) STM32 上没有 JSON 库，手写 JSON 拼接既占 Flash 又容易出格式错
 *       （忘记转义、数字格式化、缓冲区溢出）
 *    2) 固定长度二进制只需 memcpy，且长度可编译期校验
 *    3) 转 JSON 的工作交给 ESP32（资源充足、有成熟的 ArduinoJson/cJSON）
 *
 *  这正是"把合适的活分给合适的芯片"的体现。
 *
 *  ⚠️ 所有字段**显式按字节序列化**，不直接传结构体：
 *     结构体有对齐填充，STM32（32 位）与 ESP32（32 位，但 Xtensa 对齐规则
 *     与 ARM 不同）之间的布局不保证一致。
 *     跨芯片传结构体是经典陷阱 —— 换编译器或加选项就会静默错位。
 * ================================================================== */

#define GW_TELEMETRY_LEN    28u     /* 下面字段的固定序列化长度 */

typedef struct {
    uint32_t dev_id;        /* 设备 ID */
    uint32_t ts;            /* 时间戳（秒；由 ESP32 提供 NTP 时间更可靠） */
    uint32_t seq;           /* 单调递增序号，供云端去重（QoS1 可能重复投递） */
    int16_t  temp_x10;      /* 温度 ×10 */
    uint16_t humi_x10;      /* 湿度 ×10 */
    uint16_t can[4];        /* CAN 通道 1~4（够用即可，减小帧长） */
    uint16_t status;        /* 设备状态位（与 IREG_DEV_STATUS 同义） */
    uint16_t uptime_s;      /* 运行时间（秒，低 16 位） */
} gw_telemetry_t;

/**
 * 把遥测结构体序列化成固定长度字节（大端）
 * @return 写入的字节数（= GW_TELEMETRY_LEN），出错返回 0
 */
uint16_t gw_telemetry_pack(const gw_telemetry_t *t, uint8_t *out, uint16_t cap);

/**
 * 从固定长度字节反序列化遥测结构体（供 ESP32 侧或测试使用）
 * @return 0 成功，负值失败
 */
int gw_telemetry_unpack(const uint8_t *in, uint16_t len, gw_telemetry_t *out);

#endif /* GW_FRAME_H */
