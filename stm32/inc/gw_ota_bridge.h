/**
 * @file    gw_ota_bridge.h
 * @brief   OTA 下行帧分发（纯逻辑，可在 PC 上单元测试）
 *
 * ============================================================================
 *  这个模块补上了什么
 *
 *  `gw_ota.c` 的升级逻辑（111 项测试）早就写好了，但**没人调用它** ——
 *  ESP32 发来的 `GW_FT_OTA_BEGIN/CHUNK/END` 在应用层是"显式忽略"。
 *
 *  本模块就是那座桥：
 *
 *      ESP32 ──帧──► 本模块 ──► gw_ota_*() ──► Flash
 *                      │
 *                      └──► ACK/NACK 帧 ──► ESP32
 *
 *  ----------------------------------------------------------------------------
 *  为什么不塞进 gw_app_t
 *
 *  `gw_app_t` 是 15.8 KB。如果 OTA 的上下文挂在它里面，
 *  那么测"OTA 帧解析对不对"就得构造一个 15.8 KB 的对象 ——
 *  测试会变得又慢又难写，于是就不会有人写。
 *
 *  独立成一个模块之后，OTA 的帧处理可以单独测（本文件配套 90+ 项）。
 *  代价是 `main.c` 里多一处调用 —— 那个代价小得多。
 *
 *  ----------------------------------------------------------------------------
 *  ⚠️ 三个必须处理的细节
 *
 *  **① BEGIN 会阻塞 1.4~2.7 秒**
 *     擦除一个分区 136 KB = 68 页 × 20~40 ms（分区表 v2）。这在**主循环里**是灾难：
 *     RS485 从站收不到帧、看门狗可能复位。
 *     所以本模块提供 `gw_ota_bridge_take_begin()`：
 *     **先解析并应答，擦除交给调用方在"方便的时候"做**。
 *     上 FreeRTOS 后应该在一个低优先级任务里做，且每擦一页让出一次。
 *
 *  **② 分片必须限速**
 *     如果上位机以最高速率连发分片，每个分片都要写 Flash（一次约 2 ms），
 *     累积起来会让其他任务饿死。
 *     本模块限制"每个 tick 最多写 N 个分片"（`GW_OTA_CHUNKS_PER_TICK`），
 *     超出部分回 NACK 让上位机稍后重发 ——
 *     **这比"静默变慢"好**：上位机能感知到背压，而不是以为链路卡了。
 *
 *  **③ END 成功之后不能立刻重启**
 *     要先 ACK 出去（否则上位机不知道成功了），
 *     再由调用方延时重启。本模块只负责置 `reboot_pending` 标志。
 * ============================================================================
 */
#ifndef GW_OTA_BRIDGE_H
#define GW_OTA_BRIDGE_H

#include <stdint.h>

#include "gw_frame.h"
#include "gw_ota.h"

/* ==================================================================
 *  OTA 帧的载荷布局（大端，与帧头 LEN 字段的约定一致）
 *
 *  ⚠️ 这三个布局是 **STM32 与 ESP32 之间的契约**。
 *     改任何一个都必须同时改 esp32 侧，且递增协议版本。
 * ================================================================== */

/** OTA_BEGIN 载荷：major(2) minor(2) total_len(4) crc32(4) = 12 字节 */
#define GW_OTA_BEGIN_PAYLOAD_LEN    12u

/** OTA_CHUNK 载荷：offset(4) + data(N)，N = len - 4，最大 124 字节 */
#define GW_OTA_CHUNK_HDR_LEN        4u

/** OTA_CHUNK 里数据部分的最大长度 */
#define GW_OTA_CHUNK_DATA_MAX       (GW_FRAME_MAX_PAYLOAD - GW_OTA_CHUNK_HDR_LEN)

/** OTA_END 载荷：无（长度必须为 0） */
#define GW_OTA_END_PAYLOAD_LEN      0u

/* ==================================================================
 *  背压：每个 tick 最多处理多少个分片
 *
 *  单次 Flash 写入约 2 ms（半字编程 + 忙等待）。
 *  取 4 意味着一个 tick 最多占用约 8 ms ——
 *  对 Modbus 从站的 5000 µs 硬超时来说仍然偏长，
 *  所以**调用方应该在两次 tick 之间让出 CPU**
 *  （裸机下是主循环转一圈，RTOS 下是 vTaskDelay(1)）。
 * ================================================================== */
#define GW_OTA_CHUNKS_PER_TICK      4u

/* ==================================================================
 *  结果码（回给 ESP32，便于上位机区分失败原因）
 * ================================================================== */
typedef enum {
    GW_OTA_BR_OK            = 0,    /* 成功 */
    GW_OTA_BR_ERR_PAYLOAD   = 1,    /* 载荷长度/内容非法 */
    GW_OTA_BR_ERR_STATE     = 2,    /* 当前状态不允许（例如没 BEGIN 就发 CHUNK） */
    GW_OTA_BR_ERR_FLASH     = 3,    /* Flash 操作失败 */
    GW_OTA_BR_ERR_BUSY      = 4,    /* 限速：稍后重发本片 */
    GW_OTA_BR_ERR_DUPLICATE = 5,    /* 重复分片（已忽略，不算失败） */
    GW_OTA_BR_ERR_CRC       = 6,    /* 整包 CRC 不符 */
    GW_OTA_BR_ERR_NOSPACE   = 7     /* 固件超出分区容量 */
} gw_ota_br_t;

/* ==================================================================
 *  上下文
 * ================================================================== */
typedef struct {
    gw_ota_t  ota;              /* 升级状态机 */
    gw_ota_io_t io;             /* Flash 接口（由调用方填） */

    /* ---- BEGIN 的延迟执行 ----
       理由见文件头的 ①。 */
    uint16_t begin_major;
    uint16_t begin_minor;
    uint32_t begin_total;
    uint32_t begin_crc;
    uint8_t  begin_pending;     /* 1 = 有待执行的擦除 */

    uint8_t  chunks_this_tick;  /* 本 tick 已处理的分数（限速用） */
    uint8_t  reboot_pending;    /* 1 = 升级完成，调用方应延时重启 */

    /* 统计（现场能看出"走到哪一步失败的"） */
    uint32_t chunks_ok;
    uint32_t chunks_dup;
    uint32_t chunks_busy;
    uint32_t errors;

    uint8_t  inited;
} gw_ota_bridge_t;

/* ==================================================================
 *  接口
 * ================================================================== */

/**
 * 初始化
 *
 * @param b     上下文
 * @param io    Flash 接口（可以是 NULL —— 那样只在解析层工作，用于测试）
 * @param target 目标分区（通常是当前 active 的**另一个**）
 * @return 0 成功
 */
int gw_ota_bridge_init(gw_ota_bridge_t *b, const gw_ota_io_t *io,
                       gw_ota_slot_t target);

/**
 * 处理一个 OTA 帧
 *
 * @param b        上下文
 * @param type     帧类型（GW_FT_OTA_BEGIN / CHUNK / END / ACK）
 * @param payload  载荷
 * @param len      载荷长度
 * @param out      输出：要回给 ESP32 的 ACK/NACK 载荷
 * @param cap      out 的容量
 * @param out_len  输出长度
 * @return GW_OTA_BR_*
 *
 * ⚠️ 返回 `GW_OTA_BR_ERR_BUSY` 时，**分片还没写入**，
 *    调用方应把它排进下一轮（或让上位机重发）。
 *    这与"失败"不同：失败是"这片坏了"，BUSY 是"现在忙，待会儿再来"。
 */
gw_ota_br_t gw_ota_bridge_frame(gw_ota_bridge_t *b, uint8_t type,
                                const uint8_t *payload, uint16_t len,
                                uint8_t *out, uint16_t cap,
                                uint16_t *out_len);

/**
 * 每个 tick 调一次（复位限速计数）
 */
void gw_ota_bridge_tick(gw_ota_bridge_t *b);

/**
 * 是否有待执行的 BEGIN（调用方应在方便时执行擦除）
 */
int gw_ota_bridge_begin_pending(const gw_ota_bridge_t *b);

/**
 * 执行待处理的 BEGIN（**会阻塞 2~3 秒**）
 *
 * @return 0 成功（升级进入 DOWNLOAD）；<0 失败
 *
 * ⚠️ 调用位置很关键：
 *    裸机下应在主循环的**空闲时刻**（例如没有 RS485 帧要处理时）；
 *    RTOS 下应在一个低优先级任务里，且**擦除期间要能喂狗**。
 *
 *    本项目采用的办法：调用方分多次调 —— 每次调 `gw_ota_bridge_tick()`
 *    之后再调本函数，`gw_ota_begin()` 内部的擦除是逐页进行的，
 *    所以只要调用方在两次调用之间让出 CPU，狗就不会叫。
 *    （⚠️ 这一点需要在 `port/gw_ota_port.c` 的 erase 实现里配合：
 *      每擦一页调一次 `vTaskDelay(1)` 或喂一次狗。）
 */
int gw_ota_bridge_do_begin(gw_ota_bridge_t *b);

/**
 * 是否应该重启（END 成功之后）
 */
int gw_ota_bridge_reboot_pending(const gw_ota_bridge_t *b);

/** 取 OTA 状态（写入寄存器 HREG_OTA_STATE） */
gw_ota_state_t gw_ota_bridge_state(const gw_ota_bridge_t *b);

/** 结果码字符串（日志用） */
const char *gw_ota_br_str(gw_ota_br_t r);

/* ==================================================================
 *  载荷编解码（供 ESP32 侧与测试使用）
 *
 *  ⚠️ 把编解码暴露出来是为了让**测试能构造真实载荷**，
 *     而不是在测试里手写魔数 —— 后者在布局变化时不会报错，
 *     只会静默失效。
 * ================================================================== */

/** 编码 OTA_BEGIN 载荷 */
uint16_t gw_ota_pack_begin(uint16_t major, uint16_t minor,
                           uint32_t total_len, uint32_t crc32,
                           uint8_t *out, uint16_t cap);

/** 解码 OTA_BEGIN 载荷 */
int gw_ota_unpack_begin(const uint8_t *in, uint16_t len,
                        uint16_t *major, uint16_t *minor,
                        uint32_t *total_len, uint32_t *crc32);

/** 编码 OTA_CHUNK 载荷 */
uint16_t gw_ota_pack_chunk(uint32_t offset, const uint8_t *data,
                           uint16_t data_len, uint8_t *out, uint16_t cap);

/** 解码 OTA_CHUNK 载荷 */
int gw_ota_unpack_chunk(const uint8_t *in, uint16_t len,
                        uint32_t *offset, const uint8_t **data,
                        uint16_t *data_len);

/* ==================================================================
 *  ACK/NACK 载荷格式（回给 ESP32）
 *
 *      偏移 0   1 字节   OTA 阶段：1=BEGIN 2=CHUNK 3=END
 *      偏移 1   1 字节   结果码（GW_OTA_BR_*）
 *      偏移 2   4 字节   进度/上下文（大端）：
 *                        BEGIN -> 0
 *                        CHUNK -> 已成功接收的字节数
 *                        END   -> 固件总长度
 *
 *  为什么回进度而不是只回成败：
 *    上位机（或云端）能显示进度条，也能在重连后知道"传到哪了"。
 *    只回成败的话，重连后只能从头传。
 * ================================================================== */
#define GW_OTA_ACK_PAYLOAD_LEN      6u

/** OTA 阶段码 */
typedef enum {
    GW_OTA_PHASE_BEGIN = 1,
    GW_OTA_PHASE_CHUNK = 2,
    GW_OTA_PHASE_END   = 3
} gw_ota_phase_t;

/** 编码 ACK 载荷 */
uint16_t gw_ota_pack_ack(uint8_t phase, gw_ota_br_t result,
                         uint32_t progress, uint8_t *out, uint16_t cap);

#endif /* GW_OTA_BRIDGE_H */
