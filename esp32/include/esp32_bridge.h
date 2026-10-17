/**
 * @file    esp32_bridge.h
 * @brief   ESP32 桥接状态机（纯逻辑，可在 PC 上单元测试）
 *
 * ============================================================================
 *  这个模块负责什么
 *
 *  它是"云端"与"STM32"之间的翻译与缓冲：
 *
 *      STM32 ──串口帧──► [桥接] ──JSON/MQTT──► 云端
 *      STM32 ◄─串口帧── [桥接] ◄──JSON/MQTT── 云端
 *
 *  本文件**不含任何 ESP-IDF / Arduino 调用** —— 那些在 `main.c` 里。
 *  这里只有三件可测的事：
 *
 *  **① 重连退避**（报告要求：1s、2s、4s，上限 60s）
 *  **② 待确认指令**（发给 STM32 后等 ACK，超时重发，有次数上限）
 *  **③ 遥测积压**（MQTT 断了的时候不能丢数据）
 *
 *  ----------------------------------------------------------------------------
 *  两个容易写错的地方，本模块专门处理
 *
 *  **退避必须"成功后重置"**
 *     只写"失败就翻倍"是不够的。如果连上一次之后不退避计数器，
 *     下次断线会直接从 60s 开始 —— 现场表现是"重启后第一次断线
 *     要等一分钟才能重连"。
 *
 *  **重发必须换一个"请求 ID"或至少标记重发**
 *     不换的话，如果第一次的 ACK 只是**迟到**（不是丢失），
 *     设备会执行两次。对本项目的指令来说大多是幂等的
 *     （REPORT_NOW / GET_STATUS 无所谓），但 `SET_PERIOD` 会
 *     被重复执行 —— 值一样所以无害，可 `RESTORE`（恢复出厂）
 *     重复执行会**多消耗一次 Flash 擦写**。
 * ============================================================================
 */
#ifndef ESP32_BRIDGE_H
#define ESP32_BRIDGE_H

#include <stdint.h>

/* ==================================================================
 *  重连退避
 * ================================================================== */

/** 退避序列的起点（秒）—— 报告要求 1s、2s、4s… */
#define ESP32_BACKOFF_MIN_S     1u

/** 退避上限（秒）—— 报告要求上限 60s */
#define ESP32_BACKOFF_MAX_S     60u

typedef struct {
    uint32_t next_try_ms;       /* 下次允许重连的时刻（毫秒） */
    uint32_t cur_delay_s;       /* 当前退避时长（秒） */
    uint32_t attempts;          /* 连续失败次数（成功后清零） */
    uint8_t  waiting;           /* 1 = 正在退避等待 */
    uint8_t  inited;
} esp32_backoff_t;

int  esp32_backoff_init(esp32_backoff_t *b);

/**
 * 记录一次连接失败，安排下次重连时刻
 * @param now_ms 当前时刻
 */
void esp32_backoff_fail(esp32_backoff_t *b, uint32_t now_ms);

/**
 * 记录连接成功（**必须重置退避**）
 */
void esp32_backoff_ok(esp32_backoff_t *b);

/**
 * 现在是否允许重连
 * @return 1 = 可以尝试
 */
int esp32_backoff_ready(const esp32_backoff_t *b, uint32_t now_ms);

/** 当前退避时长（秒），用于上报到链路状态 */
uint32_t esp32_backoff_delay(const esp32_backoff_t *b);

/* ==================================================================
 *  待确认指令（ESP32 -> STM32 -> ACK）
 * ================================================================== */

/** 发出后等 ACK 的超时（毫秒） */
#define ESP32_CMD_ACK_TIMEOUT_MS    2000u

/** 最大重发次数（不含首次发送） */
#define ESP32_CMD_MAX_RETRY         2u

/** 指令文本缓冲 */
#define ESP32_CMD_TEXT_MAX          64u

typedef struct {
    char     text[ESP32_CMD_TEXT_MAX];  /* 待确认的指令文本 */
    uint32_t req_id;            /* 云端请求 ID（回执关联） */
    uint32_t sent_at_ms;        /* 上次发送时刻 */
    uint16_t backlog_index;     /* 在云端积压队列里的位置（无需等待时用） */
    uint8_t  has_req_id;
    uint8_t  retry_left;
    uint8_t  active;            /* 1 = 有一条指令在等待确认 */
    uint8_t  inited;
} esp32_cmd_track_t;

int esp32_cmd_track_init(esp32_cmd_track_t *t);

/**
 * 登记一条待发送的指令
 *
 * @return 0 成功；
 *         1 已有一条在等待（**必须排队或拒绝**，不能覆盖）；
 *         <0 参数非法
 *
 * ⚠️ 为什么"已有一条在等"要返回 1 而不是覆盖：
 *    覆盖会让前一条指令**永远收不到回执** ——
 *    云端会一直等，最后超时。而设备其实什么都没做。
 *    明确返回 1，调用方可以选择排队或告诉云端"忙"。
 */
int esp32_cmd_track_start(esp32_cmd_track_t *t, const char *text,
                          uint32_t req_id, int has_req_id, uint32_t now_ms);

/**
 * 收到 ACK/NACK
 *
 * @param ack_text STM32 返回的确认文本（形如 "PERIOD OK"）
 * @param ok       1 = ACK，0 = NACK
 * @return 1 = 匹配上了（调用方应发回执给云端并取下一条）；
 *         0 = 没有待确认的指令（迟到的 ACK，忽略）
 */
int esp32_cmd_track_ack(esp32_cmd_track_t *t, const char *ack_text,
                        int ok, char *out_text, uint16_t cap,
                        uint32_t *out_req_id, int *out_has_req_id);

/**
 * 推进超时检查
 *
 * @return 0 = 什么都不做；
 *         1 = **应该重发**（`retry_left` 已减 1）；
 *         2 = **应该放弃**（重发次数用完，调用方应给云端回失败）
 */
int esp32_cmd_track_tick(esp32_cmd_track_t *t, uint32_t now_ms);

/** 是否有指令在等待确认 */
int esp32_cmd_track_busy(const esp32_cmd_track_t *t);

/**
 * 取当前待确认指令的文本（**用于重发**）
 *
 * @return 1 = 取到了；0 = 没有在等的指令
 *
 * ⚠️ 为什么需要它：`tick` 返回 1（应该重发）时，调用方必须能拿到
 *    **那条指令的原文**。我第一版实现忘了这个接口，
 *    结果重发时发了一个**空帧** —— 那种 bug 的现象是
 *    "指令偶尔不生效"，而日志上看起来"重发过了"。
 */
int esp32_cmd_track_text(const esp32_cmd_track_t *t,
                         char *out, uint16_t cap, uint16_t *out_len);

/**
 * 取回待放弃指令的文本与请求 ID（`tick` 返回 2 之后调用）
 * @return 1 = 有内容
 */
int esp32_cmd_track_take_failed(esp32_cmd_track_t *t,
                                char *out_text, uint16_t cap,
                                uint32_t *out_req_id, int *out_has_req_id);

/* ==================================================================
 *  遥测积压（MQTT 断线时用）
 * ================================================================== */

/** 积压深度（条） */
#define ESP32_TELEM_RING_DEPTH      64u

/** 一条遥测 JSON 的最大长度
 *
 *  ⚠️ 从 256 提到 640（2026-10-15）。原因：切到 OneNET 物模型后端后，
 *     一条属性上报报文的最坏情况约 445 字节（id + 5 个遥测属性
 *     + 4 个 CAN 通道 + 每个都带 13 位毫秒时间戳），256 装不下。
 *     装不下的后果不是"截断"，而是 `esp32_telem_ring_push()`
 *     直接拒收（见 esp32_bridge.c 的长度检查）—— 也就是
 *     **MQTT 断线期间的数据会被静默丢掉**，而这正是这个队列存在的理由。
 *
 *     代价：64 × 640 = 40 KB 静态 .bss（原 16 KB）。
 *     ESP32 有 520 KB SRAM，这个代价可以接受。
 *
 *  ⚠️ 必须 >= `esp32_json.h` 的 `ESP32_ONENET_JSON_MAX`。
 *     两者若不一致，OneNET 后端下积压会静默失效。 */
#define ESP32_TELEM_JSON_MAX        640u

typedef struct {
    char     buf[ESP32_TELEM_RING_DEPTH][ESP32_TELEM_JSON_MAX];
    uint16_t head;
    uint16_t tail;
    uint16_t count;
    uint32_t dropped;           /* 因队列满而丢掉的条数 */
    uint8_t  inited;
} esp32_telem_ring_t;

int esp32_telem_ring_init(esp32_telem_ring_t *r);

/**
 * 入队
 * @return 0 成功；1 = **队列满，丢掉了最旧的一条**；<0 参数非法
 *
 * ⚠️ 满了丢**最旧**而不是最新：
 *    最新的一条反映设备当前状态，对运维更有价值。
 *    丢最旧的代价是"历史有一段缺口"，
 *    而丢最新会让云端看到的永远是过期数据 —— 那更糟。
 *
 * ⚠️ 丢弃必须**计数并上报**（`dropped`）。
 *    静默丢弃会让云端以为"数据是连续的"，
 *    在做趋势分析时得出错误结论。
 */
int esp32_telem_ring_push(esp32_telem_ring_t *r, const char *json,
                          uint16_t len);

/**
 * 出队一条（成功后才真正移除）
 * @return 1 = 取到了；0 = 空
 */
int esp32_telem_ring_peek(esp32_telem_ring_t *r, char *out, uint16_t cap,
                          uint16_t *out_len);

/** 确认已发出，移除队首 */
void esp32_telem_ring_pop(esp32_telem_ring_t *r);

/** 当前积压条数 */
uint16_t esp32_telem_ring_count(const esp32_telem_ring_t *r);

/** 累计丢弃条数 */
uint32_t esp32_telem_ring_dropped(const esp32_telem_ring_t *r);

#endif /* ESP32_BRIDGE_H */
