/**
 * @file    esp32_port.h
 * @brief   ESP32 联网模组通信层（USART3，115200 8N1）
 *
 * ============================================================================
 *  职责边界
 *
 *    本模块只做两件事：
 *      1) 把 STM32 的字节发到 USART3（uart_send）
 *      2) 把 USART3 收到的字节流喂给帧解析器，并按帧交给上层（tick）
 *
 *    **它不认识"遥测"、"指令"这些业务概念** —— 那是 gw_uplink 与
 *    gw_app 的事。这样 ESP32 侧协议升级（例如加一种新帧）时，
 *    本模块一行都不用改。
 *
 *  ----------------------------------------------------------------------------
 *  为什么接收用"字节流 + 解析器"而不是"等一帧收完"
 *
 *  ESP32 什么时候发、一次发多少，STM32 完全无法预知：
 *    - 可能一次中断只收到半帧
 *    - 可能一次收到三帧粘在一起
 *    - 上电时线路上可能有毛刺噪声
 *
 *  所以硬件层只负责"把收到的字节搬进缓冲区"，切帧交给
 *  gw_frame 的状态机（82 项测试覆盖了各种切分方式）。
 *  这条边界划清楚之后，"通信偶尔中断"这类问题能被定位到
 *  具体是哪一层的毛病：是字节没收全，还是切帧切错了。
 *
 *  ----------------------------------------------------------------------------
 *  与 RS485 端口的关键差异：**不需要方向控制**
 *
 *  RS485 是半双工，发送前要拉 DE、发完要拉回，还要参与总线仲裁。
 *  USART3 与 ESP32 是全双工点对点：TX/RX 各一根线，
 *  收发可以同时进行，没有"谁占总线"的问题。
 *  所以本模块比 rs485_port 简单得多 —— 这也说明
 *  "把硬件差异关在移植层里"是有效的：上层完全不必知道这些区别。
 * ============================================================================
 */
#ifndef ESP32_PORT_H
#define ESP32_PORT_H

#include <stdint.h>

#include "gw_frame.h"

/* ---------------- 状态码 ---------------- */
typedef enum {
    ESP32_OK            = 0,
    ESP32_ERR_PARAM     = -1,
    ESP32_ERR_NO_FRAME  = -2,       /* 当前没有完整帧可取 */
    ESP32_ERR_TOO_BIG   = -3,       /* 帧载荷超出调用方缓冲区 */
    ESP32_ERR_HAL       = -4,       /* HAL 发送失败 */
    ESP32_ERR_SIZE      = -5        /* 取帧输出参数问题（重同步/CRC 错等已内含） */
} esp32_status_t;

/* ---------------- 统计量（诊断 + 实测数据来源） ---------------- */
typedef struct {
    uint32_t tx_frames;         /* 通过本模块发出的帧数 */
    uint32_t tx_bytes;          /* 发出的字节数 */
    uint32_t tx_errors;         /* 发送失败次数 */
    uint32_t rx_bytes;          /* 收到的字节数 */
    uint32_t rx_frames;         /* 成功解析的帧数 */
    uint32_t rx_crc_errors;     /* CRC 校验失败次数 */
    uint32_t rx_overflows;      /* 接收缓冲溢出（帧被截断）次数 */
    uint32_t hal_errors;        /* HAL 报告的错误次数 */
} esp32_stats_t;

/* ==================================================================
 *  接口
 * ================================================================== */

/** 初始化（GPIO 由 CubeMX 配好；这里只做状态复位与武装接收） */
esp32_status_t esp32_port_init(void);

/**
 * 发送一帧（阻塞式，115200 下 133 字节约 12ms）
 *
 * @return ESP32_OK 成功；ESP32_ERR_HAL 发送失败
 *
 * @note 与 RS485 不同，这里**不需要仲裁**：
 *       USART3 是点对点全双工，永远可以发。
 */
esp32_status_t esp32_port_send(const uint8_t *buf, uint16_t len);

/**
 * 推进接收：把硬件缓冲里的字节喂给帧解析器
 *
 * 必须在主循环/任务上下文调用，不可在中断里调用。
 */
void esp32_port_tick(void);

/**
 * 取一帧（非阻塞，可反复调用直到返回 ERR_NO_FRAME）
 *
 * @param out_type  帧类型
 * @param payload   载荷输出缓冲
 * @param cap       payload 容量
 * @param out_len   载荷长度
 *
 * @return ESP32_OK 取到一帧；ESP32_ERR_NO_FRAME 当前没有；
 *         ESP32_ERR_PARAM/TOO_BIG 参数或长度问题（可继续调用取下一帧）
 */
esp32_status_t esp32_port_take_frame(uint8_t *out_type, uint8_t *payload,
                                     uint16_t cap, uint16_t *out_len);

/** 取统计量 */
const esp32_stats_t *esp32_port_stats(void);

/**
 * 复位 ESP32 模组（拉低 EN 引脚再释放）
 *
 * 用途：模组死机（不再响应任何帧）时由上层主动复位。
 * 硬件上 EN 引脚若没接，本函数是无害的空操作。
 */
void esp32_port_reset_module(uint32_t hold_ms);

/**
 * 取帧解析器的统计量（粘包/半包/重同步诊断）
 *
 * 与 esp32_stats_t 的区别：这里看的是"切帧质量"
 * （重同步次数、噪声字节数），而上层那份看的是"收发数量"。
 */
const gw_frame_stat_t *esp32_port_frame_stats(void);

#endif /* ESP32_PORT_H */
