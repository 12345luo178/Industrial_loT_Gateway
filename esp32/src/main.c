/**
 * @file    main.c
 * @brief   ★ ESP32 侧固件 —— STM32 与云端之间的桥接
 *
 * ============================================================================
 *  架构：为什么 JSON 在 ESP32 侧生成
 *
 *      STM32 (F103)                    ESP32                    云端
 *      ────────────                    ─────                    ────
 *      采集 / Modbus / CAN
 *            │
 *            │  串口帧（二进制，28 字节遥测）
 *            ▼
 *      ──────────────────────►  转 JSON  ──── MQTT ────►  EMQX
 *                                ▲
 *                                │
 *                                └── 下行指令 JSON -> 文本帧
 *
 *  报告第 2.1 节原本写的是"STM32 侧用 cJSON 封装为 JSON"。
 *  本项目把它挪到了 ESP32 侧，理由是**把合适的活分给合适的芯片**：
 *
 *    | | STM32F103 | ESP32 |
 *    |---|---|---|
 *    | Flash | 512 KB | 4 MB |
 *    | RAM | 64 KB | 520 KB |
 *    | 主频 | 72 MHz | 240 MHz ×2 |
 *    | JSON 库 | 无（要自己塞进去） | 现成 |
 *    | TLS | 几乎不可能 | 支持 |
 *
 *  在 STM32 上拼 JSON 再发回 ESP32，等于**多做一次无意义的转换**：
 *  STM32 拼完字符串 → 拆成帧 → ESP32 收到再解析成 JSON。
 *  中间那两步都不产生价值，却各引入一类出错可能
 *  （拼错格式、帧长溢出）。
 *
 *  二进制帧 + 固定 28 字节布局的好处是：
 *    - STM32 侧只需 `memcpy`，没有字符串拼接、没有转义、没有缓冲溢出
 *    - 长度可编译期校验（`_Static_assert` 钉住）
 *    - 帧协议与遥测布局都有 PC 单元测试（1748 + 280 项）
 *
 *  ----------------------------------------------------------------------------
 *  编译验证状态（2026-10-14 更新）
 *
 *  ✅ **已能编译**：ESP-IDF v5.5.1 装在 `D:\Espressif\esp-idf`，
 *     而 `esp32/` 目录本身就是 IDF 工程，一条命令即可：
 *
 *         powershell -File tools\esp32_build.ps1
 *
 *  本文件调用的所有**协议逻辑**另有单元测试覆盖：
 *
 *      esp32_json.c    -> test_esp32_json.c    154 项
 *      esp32_bridge.c  -> test_esp32_bridge.c  126 项
 *      gw_frame.c      -> test_frame.c          82 项（与 STM32 共用）
 *
 *  也就是说：**除了"把字节喂给 UART / MQTT"这几行，其余都被测过。**
 *
 *  ----------------------------------------------------------------------------
 *  ⚠️ 刷屏防护（2026-10-14 修；实测曾刷到 44 KB/s）
 *
 *  没填凭据时 `esp_wifi_connect()` 会**秒失败** ⇒ 断线事件 ⇒ 重连 ⇒ 自激。
 *  本文件用**四道**措施压住它，改这块之前先读一遍：
 *
 *    ① `wifi_creds_are_placeholder()` —— 占位符凭据时**一次都不试**
 *       （只跑 UART 桥接，STM32 侧的帧协议仍可联调）；
 *    ② `esp_log_level_set("wifi", ESP_LOG_WARN)` —— 压掉驱动的 INFO 噪声
 *       （保留 WARN/ERROR，真出问题还看得见）；
 *    ③ Wi-Fi 退避**独立**于 MQTT —— 原来两者共用一个，MQTT 一连上
 *       就把 Wi-Fi 的 `waiting` 清掉，重连随即退化成每 500ms 一次；
 *    ④ `GW_WIFI_MIN_RETRY_MS` —— 与退避状态**无关**的硬性重连间隔下限，
 *       前三道都失效时它兜底。
 * ============================================================================
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "mqtt_client.h"

#include "esp32_json.h"
#include "esp32_bridge.h"
#include "gw_frame.h"

#include "esp_sntp.h"       /* 真实时间（见 now_ms_epoch） */
#include <sys/time.h>

/* ==================================================================
 *  ★ 云端后端选择（编译期）
 *
 *  两个后端**报文与 topic 都不同**，所以只能编译期二选一，
 *  不能运行时切：
 *
 *    GENERIC  EMQX / 自建 broker
 *             topic: dev/{id}/telemetry 等自定义 topic
 *             报文:  扁平 JSON（esp32_json_telemetry）
 *
 *    ONENET   studio-mqtt.heclouds.com（中国移动物联网平台）
 *             topic: $sys/{pid}/{dn}/thing/property/post 等系统 topic
 *             报文:  OneJSON 物模型（esp32_json_onenet_property）
 *
 *  ⚠️ 为什么必须两套：OneNET 官方明确「暂不支持用户自定义 topic，
 *     仅限使用系统 topic」，而且上行报文必须是物模型 OneJSON。
 *     详见 doc/OneNET接入说明.md。
 *
 *  ⚠️ 下面这些**与云端无关**的东西两边完全共用、一行都不用改：
 *       esp32_bridge.c 的退避 / 指令确认 / 积压队列
 *       gw_frame.c 的串口帧协议
 *       UART / Wi-Fi / 任务结构
 * ================================================================== */

#define GW_CLOUD_GENERIC   0
#define GW_CLOUD_ONENET    1

#ifndef GW_CLOUD_BACKEND
#define GW_CLOUD_BACKEND   GW_CLOUD_ONENET
#endif

/* ==================================================================
 *  硬件配置 —— ⚠️ 必须与 STM32 侧一致
 * ================================================================== */

/** UART 端口与引脚 */
#define GW_UART_NUM         UART_NUM_2
#define GW_UART_TX_GPIO     17      /* ESP32 TX -> STM32 PB11 (USART3_RX) */
#define GW_UART_RX_GPIO     16      /* ESP32 RX <- STM32 PB10 (USART3_TX) */
#define GW_UART_BAUD        115200

/** ⚠️ STM32 侧把 ESP32 的 EN 脚接在 PB12 上，可以硬复位 ESP32。
 *  这里不需要做任何事 —— 但要知道：**ESP32 被复位后
 *  本程序会从头开始，而 STM32 不会知道**。
 *  所以 STM32 侧的补传缓冲会一直积压到 ESP32 重新连上 MQTT。
 *  这是设计如此（STM32 不知道也不关心 ESP32 的状态变化）。 */
#define GW_UART_RX_BUF      1024

/** Wi-Fi 与 MQTT 配置。
 *
 *  ⚠️ 正式产品不该把凭据写死在固件里 —— 那会让"换一批设备"
 *     变成"重新编译烧录"。本项目为了源码交付的简洁性这么做，
 *     并在这里明确标注这是**已知简化**。
 *     生产做法：首次配网时通过 `GW_FT_WIFI_CONFIG` 帧写进 NVS。 */
#define GW_WIFI_SSID        "<YOUR_WIFI_SSID>"
#define GW_WIFI_PASS        "<YOUR_WIFI_PASSWORD>"

/* ==================================================================
 *  云端凭据
 *
 *  ⚠️ 两个后端的凭据**不通用**，别把 EMQX 的填到 OneNET 那边：
 *
 *    GENERIC（EMQX 等）
 *      GW_MQTT_BROKER     mqtt://<主机>:1883
 *      GW_MQTT_USER/PASS  可选（公共 broker 通常不需要）
 *      GW_MQTT_CLIENT_ID  留空 -> 由 esp-mqtt 生成随机 clientId
 *
 *    ONENET（studio-mqtt.heclouds.com）
 *      GW_MQTT_BROKER     mqtt://studio-mqtt.heclouds.com:1883
 *      GW_MQTT_CLIENT_ID  = **设备名称**（不是数字 ID！）
 *      GW_MQTT_USER       = **产品ID**
 *      GW_MQTT_PASS       = **token**（用 tools/onenet_token.py 生成）
 *      GW_ONENET_PRODUCT_ID / GW_ONENET_DEVICE_NAME  物模型 topic 要用
 *
 *  ⚠️ OneNET 三要素缺一，平台一律回 CONNACK rc=4/5 然后断开，
 *     而日志上只看到反复 `MQTT disconnected` —— 完全看不出是凭据没填。
 *     本文件在 MQTT 初始化前有一段专门的检查把原因喊出来。
 * ================================================================== */

/* ---- 按后端选 broker ---- */
#if GW_CLOUD_BACKEND == GW_CLOUD_ONENET
#define GW_MQTT_BROKER      "mqtt://studio-mqtt.heclouds.com:1883"
#else
#define GW_MQTT_BROKER      "mqtt://192.168.1.100:1883"
#endif

/* ---- OneNET 鉴权三要素（2026-10-15 填入实测通过的凭据）---- */
#define GW_MQTT_CLIENT_ID   "<YOUR_DEVICE_NAME>"            /* ← 设备名称 */
#define GW_MQTT_USER        "<YOUR_PRODUCT_ID>"    /* ← 产品ID */
/* ⚠️ token 里有**过期时间**（`et=`）。当前这份是 et=2000000000
   （2033-05-18）。到那天之前必须换一份，否则设备会突然全部连不上，
   而报的错是"鉴权失败"—— 看起来像"密钥被人改了"。
   换 token：tools/onenet_token.py（需要产品/设备的访问密钥）。
   ⚠️ 注意 token 里的 value 是 URL 编码过的（%2F 就是 '/'），
      不要手工"还原"成 '/' —— 平台要的就是编码后的形式。 */
#define GW_MQTT_PASS        "<GENERATED_TOKEN>"

/* ---- OneNET 物模型 topic 要用（{pid}/{dn}）----
   ⚠️ 必须与上面 clientId / username **一致**，否则会出现
      "MQTT 连上了，但订阅被拒 / 发布无应答"——极难二分定位。 */
#define GW_ONENET_PRODUCT_ID    "<YOUR_PRODUCT_ID>"
#define GW_ONENET_DEVICE_NAME   "w3"

#define GW_DEVICE_ID        100000u

/** MQTT QoS1：报告要求。QoS1 保证"至少一次"，
 *  所以云端可能收到重复 —— 遥测里的 `seq` 就是给云端去重用的。 */
#define GW_MQTT_QOS         1

/** 两次 `esp_wifi_connect()` 之间的**硬性**最小间隔。
 *
 *  ⚠️ 这是防刷屏的第二道闸（第一道是退避本身）。退避是"软"的：
 *     `esp32_backoff_ready()` 在 `waiting == 0` 时恒返回 1（语义是
 *     "没在退避，随时可以试"）。一旦某条路径把 `waiting` 清掉，
 *     重连任务就退化成每 500ms 一次裸重连 —— 而每次都会打一行
 *     `wifi:sta is connecting`。这一道与退避状态无关，所以退避再怎么错
 *     也不会刷屏。3 秒是"断线恢复够快"与"日志不吵"之间的折中。 */
#define GW_WIFI_MIN_RETRY_MS  3000u

/* ==================================================================
 *  topic（按后端二选一）
 *
 *  ⚠️ OneNET 的 topic 里带 {pid}/{dn}，运行时才能拼（宏里放不下），
 *     所以下面只给"格式串"，真正拼在 app_main 里做。
 * ================================================================== */
#if GW_CLOUD_BACKEND == GW_CLOUD_ONENET

/* OneNET 只认系统 topic（官方：「暂不支持用户自定义 topic」）。
   {pid} = 产品ID，{dn} = 设备名称。 */
#define GW_TOPIC_TELEMETRY  "$sys/%s/%s/thing/property/post"
#define GW_TOPIC_COMMAND    "$sys/%s/%s/thing/property/set"
#define GW_TOPIC_REPLY      "$sys/%s/%s/thing/property/set_reply"
/* OneNET 没有独立的"链路状态 topic" —— 链路状态也走属性上报。
   这里留空表示"不用这条"，见 housekeeping_task ③ 的分支。 */
#define GW_TOPIC_STATUS     ""

#else   /* GENERIC */

/** 上行 topic */
#define GW_TOPIC_TELEMETRY  "dev/%u/telemetry"
#define GW_TOPIC_STATUS     "dev/%u/status"
#define GW_TOPIC_REPLY      "dev/%u/reply"
/** 下行 topic（订阅） */
#define GW_TOPIC_COMMAND    "dev/%u/command"

#endif  /* GW_CLOUD_BACKEND */

static const char *TAG = "gw_bridge";

/* ==================================================================
 *  全局状态
 * ================================================================== */
/* ⚠️ Wi-Fi 的退避**独立**（2026-10-14 修）。
   原来它和 MQTT 共用一个 `s_backoff`：MQTT 一连上就调 `esp32_backoff_ok()`，
   顺手把 Wi-Fi 侧的 `waiting` 标志清掉 ⇒ 重连任务退化成"每 500ms 裸重连一次"。
   而每次 `esp_wifi_connect()` 都会打一行 `wifi:sta is connecting`
   ⇒ 实测刷到 44 KB/s。分开之后语义也清楚了：谁断线谁推进自己的退避。 */
static esp32_backoff_t    s_wifi_backoff;
static esp32_cmd_track_t  s_cmd;
static esp32_telem_ring_t s_ring;
static gw_frame_parser_t  s_parser;

/** OneNET 报文 id 计数器（单调递增；平台用它关联 post 与 reply）*/
static uint32_t s_onenet_msg_id = 0u;

/** SNTP 是否已同步 —— 决定 time 字段能不能填真实时间 */
static volatile int s_time_synced = 0;

/** 上次打印 "WiFi disconnected" 时的退避档位（日志节流，见 wifi_event_handler）*/
static uint32_t s_wifi_logged_delay = 0xFFFFFFFFu;

/** 网络是否启用（凭据是占位符时为 0，只跑 UART 桥接）*/
static int s_net_enabled = 0;

/* ==================================================================
 *  时间
 * ================================================================== */

/**
 * 当前时间（毫秒，unix 纪元）
 *
 * ⚠️ 这是本文件最容易搞错的一个点，说清楚：
 *
 *    `esp_timer_get_time()` 返回的是**上电以来的微秒数**，不是 unix 时间。
 *    所以 `esp_timer_get_time()/1000` 得到的是"运行了多少毫秒"，
 *    而不是"现在几点"。两者在代码里长得几乎一样，但语义完全不同。
 *
 *    - 通用后端里，把它当"上电秒数"填进扁平 JSON 的 `ts` 是**可以**的，
 *      因为那个字段本来就只是给云端排序用。
 *    - 但 OneNET 的 `time` 字段语义是**属性生成时刻**。填上电毫秒
 *      会变成 1970-01-01，控制台上的曲线时间轴全是错的 ——
 *      而且**看起来像"数据是准的，只是时间不对"**，很容易被当成
 *      "平台时区问题"排查半天。
 *
 *    所以这里的策略是：SNTP 同步过 -> 用真实时间；
 *    没同步 -> 返回 0，让调用方**不带 time 字段**。
 *    不带 time 时平台用接收时间 —— 至少不会错得离谱。
 */
static uint64_t now_ms_epoch(void)
{
    struct timeval tv;

    if (s_time_synced == 0) {
        return 0ull;
    }
    if (gettimeofday(&tv, 0) != 0) {
        return 0ull;
    }
    return ((uint64_t)tv.tv_sec * 1000ull) + (uint64_t)(tv.tv_usec / 1000);
}

/** 取下一条 OneNET 报文 id（从 1 开始；0 保留给"无 id"）*/
static uint32_t next_msg_id(void)
{
    s_onenet_msg_id++;
    if (s_onenet_msg_id == 0u) {
        s_onenet_msg_id = 1u;       /* 溢出回绕，跳过 0 */
    }
    return s_onenet_msg_id;
}

/** SNTP 同步完成的回调 */
static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    s_time_synced = 1;
    ESP_LOGI(TAG, "SNTP 时间已同步 -> OneNET time 字段用真实时间");
}

static esp_mqtt_client_handle_t s_mqtt = 0;
static volatile int  s_mqtt_connected = 0;
static volatile int  s_wifi_connected = 0;

/* ⚠️ 128 而不是 64：OneNET 的 topic 形如
   `$sys/{产品ID}/{设备名称}/thing/property/post`，
   产品ID 约 11 字符、设备名称可到 32 字符，64 会**静默截断** ——
   而截断后的 topic 平台只当"订阅了一个不存在的主题"，
   发布则直接失败，看不出是缓冲不够。 */
static char s_topic_telemetry[128];
static char s_topic_status[128];
static char s_topic_reply[128];
static char s_topic_command[128];

/* ---- 诊断计数器：STM32→ESP32 这条 UART 现在完全是黑盒
 *      （成功上报静默、RX 也没日志），现场查"板子到底有没有在发"
 *      只能靠猜。这里把"收到多少字节 / 解析出多少帧"数出来，
 *      周期性打进日志 —— 一次就能把问题三选一定位：
 *        0 字节        → 物理链路死（接线 / 供电 / STM32 没跑）
 *        有字节、0 帧  → 波特率或帧格式不对
 *        有帧          → STM32 活着，问题在别处 ---- */
static volatile uint32_t s_uart_rx_bytes  = 0u;
static volatile uint32_t s_uart_rx_frames = 0u;

/* 前置声明：handle_stm32_frame 里要回握手，实现在 MQTT 事件区 */
static void send_link_status_to_stm32(void);

/* ==================================================================
 *  发送一帧到 STM32
 * ================================================================== */
static void uart_send_frame(uint8_t type, const uint8_t *payload,
                            uint16_t len)
{
    uint8_t  frame[GW_FRAME_MAX_TOTAL];
    uint16_t flen = 0u;

    if (gw_frame_build(frame, (uint16_t)sizeof(frame), type, payload, len,
                       &flen) != GW_FRAME_OK) {
        ESP_LOGW(TAG, "build frame failed type=0x%02X len=%u", type, len);
        return;
    }
    (void)uart_write_bytes(GW_UART_NUM, (const char *)frame, flen);
}

/* ==================================================================
 *  MQTT 发布（带连接检查）
 * ================================================================== */
static int mqtt_publish(const char *topic, const char *payload)
{
    int msg_id;

    if (s_mqtt == 0 || s_mqtt_connected == 0) {
        return -1;
    }
    msg_id = esp_mqtt_client_publish(s_mqtt, topic, payload, 0,
                                     GW_MQTT_QOS, 0);
    /* ⚠️ 返回 -1 表示入队失败（通常是 outbox 满）。
       这时**必须让调用方知道**，否则遥测会被静默丢掉 ——
       而"静默丢失"是现场最难查的问题。 */
    return (msg_id < 0) ? -1 : 0;
}

/* ==================================================================
 *  处理来自 STM32 的帧
 * ================================================================== */
static void handle_stm32_frame(uint8_t type, const uint8_t *payload,
                               uint16_t len)
{
    switch (type) {

    /* ---- 遥测：转 JSON 后发云端；发不出去就积压 ---- */
    case GW_FT_TELEMETRY: {
        char     json[ESP32_TELEM_JSON_MAX];
        uint16_t jlen;

#if GW_CLOUD_BACKEND == GW_CLOUD_ONENET
        /* OneNET：物模型 OneJSON。
           time 用**真实时间**（SNTP 未同步时 now_ms_epoch() 返回 0，
           报文就不带 time 字段，由平台盖接收时间 —— 见 now_ms_epoch）。 */
        jlen = esp32_json_onenet_property(payload, len, next_msg_id(),
                                          now_ms_epoch(),
                                          json, (uint16_t)sizeof(json));
#else
        /* 通用后端：扁平 JSON。
           STM32 没有 RTC，它给的 ts 是"上电秒数"，
           用 ESP32 的时间覆盖，云端才能做时序分析。 */
        jlen = esp32_json_telemetry(payload, len,
                                    (uint32_t)(esp_timer_get_time() / 1000000),
                                    json, (uint16_t)sizeof(json));
#endif
        if (jlen == 0u) {
            ESP_LOGW(TAG, "telemetry json failed (len=%u)", len);
            break;
        }

        if (mqtt_publish(s_topic_telemetry, json) != 0) {
            /* 发不出去：入积压队列，等重连后补传。
               ⚠️ 这里是"两段式补传"的**第二段**：
                  STM32 侧的环形缓冲管"ESP32 不在线"，
                  ESP32 侧的环形缓冲管"MQTT 不在线"。
                  两层都需要 —— ESP32 在线但 MQTT 断了的情况很常见
                  （broker 重启、网络抖动）。 */
            int r = esp32_telem_ring_push(&s_ring, json, jlen);

            if (r == 1) {
                ESP_LOGW(TAG, "telemetry ring full, dropped oldest "
                              "(total dropped %u)",
                         (unsigned)esp32_telem_ring_dropped(&s_ring));
            }
        }
        break;
    }

    /* ---- 链路状态：STM32 报告自己的 Wi-Fi/MQTT 状态位 ----
       ⚠️ 这个帧在当前架构下反过来了：
          STM32 不知道 Wi-Fi/MQTT 状态（那是 ESP32 的事）。
          所以 STM32 发来的 0x04（STATUS_SET）是运行参数，
          而真正的链路状态由**本文件**主动上报（见 report_link_status）。 */
    case GW_FT_STATUS_SET:
        ESP_LOGD(TAG, "status set (%u bytes)", len);
        break;

    /* ---- STM32 的握手询问 ----
       STM32 上电后处于 DISCONNECTED，每个退避周期发一帧 MQTT_CONFIG
       询问链路，然后进 CONNECTING 等 GW_FT_LINK_STATUS。
       ⚠️ 此前这里**没有这个 case** —— 询问被当成了"unhandled frame"
       静默吞掉，STM32 便永远到不了 CONNECTED，一条遥测都发不出来。
       现在必须回链路状态（见 send_link_status_to_stm32 的说明）。 */
    case GW_FT_MQTT_CONFIG:
        ESP_LOGD(TAG, "stm32 asked for link status -> reply");
        send_link_status_to_stm32();
        break;

    /* ---- 指令确认：STM32 对下行指令的确认 ----
       ⚠️ 用 GW_FT_CMD_ACK / GW_FT_CMD_NACK，**不是** GW_FT_ACK / GW_FT_NACK。
          后两者的语义是"遥测送达/被拒"（STM32 侧的上云链路占用它们），
          与本路径无关。混用会让一次指令失败被当成一次遥测被拒。 */
    case GW_FT_CMD_ACK:
    case GW_FT_CMD_NACK: {
        char     ack[96];
        uint32_t rid = 0u;
        int      has_rid = 0;

        if (esp32_cmd_track_ack(&s_cmd, (const char *)payload,
                                (type == GW_FT_CMD_ACK) ? 1 : 0,
                                ack, (uint16_t)sizeof(ack),
                                &rid, &has_rid) != 1) {
            /* 迟到的 ACK：忽略。
               那条指令早已超时并给云端回过失败了，
               再回一次会让云端收到两条矛盾的回执。 */
            ESP_LOGD(TAG, "late ack ignored: %s", (const char *)payload);
            break;
        }

        {
            char     reply[320];
            uint16_t rlen = 0u;

#if GW_CLOUD_BACKEND == GW_CLOUD_ONENET
            /* ⚠️ OneNET 这边"回执"分**两步**，不能合成一步：
                 ① 收到下行属性设置时立刻回 set_reply（"我收到了"）
                    —— 那已经在 mqtt_event_handler 里做过了；
                 ② 执行结果作为**属性上报**回来（这里）。
               把执行结果塞进 set_reply 会让平台先超时，
               因为等 STM32 执行完要 2 秒，而平台等不了那么久。 */
            (void)rid;
            (void)has_rid;

            rlen = esp32_json_onenet_result(next_msg_id(), now_ms_epoch(),
                                            ack,
                                            (type == GW_FT_CMD_ACK) ? 1 : 0,
                                            reply, (uint16_t)sizeof(reply));
            if (rlen > 0u) {
                (void)mqtt_publish(s_topic_telemetry, reply);
                ESP_LOGI(TAG, "cmd done: %s -> %s", ack,
                         (type == GW_FT_CMD_ACK) ? "ACK" : "NACK");
            }
#else
            rlen = esp32_json_cmd_reply(rid, has_rid, ack,
                                        (type == GW_FT_CMD_ACK) ? 1 : 0,
                                        reply, (uint16_t)sizeof(reply));
            if (rlen > 0u) {
                (void)mqtt_publish(s_topic_reply, reply);
                ESP_LOGI(TAG, "cmd done: %s -> %s", ack,
                         (type == GW_FT_CMD_ACK) ? "ACK" : "NACK");
            }
#endif
        }

        /* ⚠️ 这里可以立刻取下一条排队的指令。
           本实现一次只允许一条在途（见 esp32_bridge.h 的说明），
           所以"排队"暂时用不上 —— 指令速率远低于处理速度。 */
        break;
    }

    /* ---- OTA 相关：STM32 侧尚未实现下行 OTA，显式忽略 ---- */
    case GW_FT_OTA_ACK:
        ESP_LOGD(TAG, "ota ack ignored (not implemented)");
        break;

    default:
        ESP_LOGD(TAG, "unhandled frame type 0x%02X", type);
        break;
    }
}

/* ==================================================================
 *  UART 接收任务
 *
 *  ⚠️ 用阻塞读 + 固定缓冲，而不是事件队列：
 *     事件队列在高速率下会丢事件（UART_DATA 事件带的数据长度
 *     受队列项大小限制）。阻塞读 + 循环读满更简单也更可靠。
 * ================================================================== */
static void uart_rx_task(void *arg)
{
    uint8_t buf[256];

    (void)arg;

    for (;;) {
        int n = uart_read_bytes(GW_UART_NUM, buf, sizeof(buf),
                                pdMS_TO_TICKS(100));

        if (n <= 0) {
            continue;
        }

        s_uart_rx_bytes += (uint32_t)n;   /* 诊断计数（见定义处的说明） */

        /* 喂给帧解析器（它内部处理粘包/半包/噪声重同步，
           这三点都有 test_frame.c 的 82 项测试覆盖）。 */
        gw_frame_feed(&s_parser, buf, (uint16_t)n);

        /* 一次 feed 可能产生多帧（粘包），所以要反复 take */
        for (;;) {
            uint8_t  type = 0u;
            uint8_t  payload[GW_FRAME_MAX_PAYLOAD];
            uint16_t plen = 0u;

            if (gw_frame_take(&s_parser, &type, payload,
                              (uint16_t)sizeof(payload), &plen)
                != GW_FRAME_OK) {
                break;
            }
            s_uart_rx_frames += 1u;
            handle_stm32_frame(type, payload, plen);
        }
    }
}

/* ==================================================================
 *  周期任务：补传 + 指令超时 + 链路状态上报
 * ================================================================== */
static void housekeeping_task(void *arg)
{
    uint32_t last_status_ms = 0u;
    uint32_t last_diag_ms   = 0u;

    (void)arg;

    for (;;) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        /* ---- ① 补传积压遥测 ---- */
        if (s_mqtt_connected != 0 && esp32_telem_ring_count(&s_ring) > 0u) {
            char     json[ESP32_TELEM_JSON_MAX];
            uint16_t jlen = 0u;

            /* ⚠️ "peek -> 发布 -> pop" 而不是 "pop -> 发布"。
               发布失败时条目还在队列里，下次继续试 ——
               先 pop 的话失败就等于丢数据。 */
            if (esp32_telem_ring_peek(&s_ring, json,
                                      (uint16_t)sizeof(json), &jlen) == 1) {
                if (mqtt_publish(s_topic_telemetry, json) == 0) {
                    esp32_telem_ring_pop(&s_ring);
                }
            }
        }

        /* ---- ② 指令超时检查（重发 / 放弃） ---- */
        {
            int r = esp32_cmd_track_tick(&s_cmd, now_ms);

            if (r == 1) {
                /* ⚠️ 重发必须发**那条指令的原文**，不能发空帧。
                   我第一版就是发了个空帧 —— 那种 bug 的现象是
                   "指令偶尔不生效"，而日志上看起来"重发过了"。 */
                char     text[ESP32_CMD_TEXT_MAX];
                uint16_t tlen = 0u;

                if (esp32_cmd_track_text(&s_cmd, text,
                                         (uint16_t)sizeof(text),
                                         &tlen) == 1) {
                    uart_send_frame(GW_FT_COMMAND,
                                    (const uint8_t *)text, tlen);
                    ESP_LOGW(TAG, "cmd ack timeout, resend: %s", text);
                } else {
                    ESP_LOGE(TAG, "resend failed: pending cmd unreadable");
                }
            } else if (r == 2) {
                /* 放弃：给云端回失败，否则云端会一直等到超时 */
                char     text[ESP32_CMD_TEXT_MAX];
                char     reply[320];
                uint32_t rid = 0u;
                int      has_rid = 0;

                if (esp32_cmd_track_take_failed(&s_cmd, text,
                                                (uint16_t)sizeof(text),
                                                &rid, &has_rid) == 1) {
#if GW_CLOUD_BACKEND == GW_CLOUD_ONENET
                    /* OneNET：失败也走**属性上报**（cmd_result 带 FAIL: 前缀），
                       而不是 set_reply —— set_reply 早在收到下行时就回过了。 */
                    (void)rid;
                    (void)has_rid;
                    if (esp32_json_onenet_result(next_msg_id(), now_ms_epoch(),
                                                 "STM32_NO_RESPONSE", 0,
                                                 reply,
                                                 (uint16_t)sizeof(reply)) > 0u) {
                        (void)mqtt_publish(s_topic_telemetry, reply);
                    }
#else
                    if (esp32_json_cmd_reply(rid, has_rid,
                                             "STM32_NO_RESPONSE", 0,
                                             reply, (uint16_t)sizeof(reply)) > 0u) {
                        (void)mqtt_publish(s_topic_reply, reply);
                    }
#endif
                    ESP_LOGW(TAG, "cmd gave up: %s", text);
                }
            }
        }

        /* ---- ②½ UART 链路诊断（每 15 秒） ----
           ⚠️ 这条日志是"STM32 到底有没有在发"的唯一观测点：
              0 字节 → 物理链路死；有字节 0 帧 → 波特率/帧格式不对；
              有帧   → STM32 活着，问题在别处。 */
        if ((uint32_t)(now_ms - last_diag_ms) >= 15000u) {
            last_diag_ms = now_ms;
            ESP_LOGI(TAG, "uart: rx %lu bytes / %lu frames, ring %u",
                     (unsigned long)s_uart_rx_bytes,
                     (unsigned long)s_uart_rx_frames,
                     (unsigned)esp32_telem_ring_count(&s_ring));
        }

        /* ---- ③ 周期性上报链路状态（每 30 秒） ---- */
        if (s_mqtt_connected != 0
            && (uint32_t)(now_ms - last_status_ms) >= 30000u) {
            char     json[ESP32_TELEM_JSON_MAX];
            int16_t  rssi = 0;
            int      has_rssi = 0;
            wifi_ap_record_t ap;
            uint16_t jlen = 0u;

            last_status_ms = now_ms;

            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                rssi = (int16_t)ap.rssi;
                has_rssi = 1;
            }

#if GW_CLOUD_BACKEND == GW_CLOUD_ONENET
            /* OneNET 没有独立的"链路状态 topic" —— 也走属性上报。
               ⚠️ rssi 取不到时传 has_rssi=0，**不上报**该属性，
                  而不是上报 0（0 dBm 表示贴着路由器，会误导）。 */
            jlen = esp32_json_onenet_link(
                       next_msg_id(), now_ms_epoch(),
                       s_wifi_connected ? ESP32_LINK_CONNECTED
                                        : ESP32_LINK_DISCONNECTED,
                       s_mqtt_connected ? ESP32_LINK_CONNECTED
                                        : ESP32_LINK_DISCONNECTED,
                       rssi, has_rssi,
                       esp32_telem_ring_count(&s_ring),
                       json, (uint16_t)sizeof(json));
            if (jlen > 0u) {
                (void)mqtt_publish(s_topic_telemetry, json);
            }
#else
            (void)has_rssi;
            jlen = esp32_json_link_status(GW_DEVICE_ID,
                                          s_wifi_connected ? ESP32_LINK_CONNECTED
                                                           : ESP32_LINK_DISCONNECTED,
                                          s_mqtt_connected ? ESP32_LINK_CONNECTED
                                                           : ESP32_LINK_DISCONNECTED,
                                          rssi,
                                          esp32_telem_ring_count(&s_ring),
                                          json, (uint16_t)sizeof(json));
            if (jlen > 0u) {
                (void)mqtt_publish(s_topic_status, json);
            }
#endif
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ==================================================================
 *  向 STM32 广播链路状态
 *
 *  ⚠️⚠️ 这一帧是整条上行链路的**钥匙**：
 *     STM32 的 gw_uplink 只有收到 GW_FT_LINK_STATUS 才会把状态
 *     置为 CONNECTED，之后才开始发遥测/心跳。而此前 ESP32 **从不**
 *     发它（uart_send_frame 只有 2 处、都是 COMMAND）——
 *     于是 STM32 在 DISCONNECTED↔CONNECTING 之间永久打转，
 *     一条遥测都发不出来；ESP32 侧则表现为 uart rx 0 bytes。
 *     （PC 上的 test_link_e2e 测不到这个缺口，因为它直接喂帧，
 *      没有走过"谁先开口"的握手。）
 *
 *  载荷约定见 src/gw_uplink.c 的 on_frame：
 *    [0]=wifi_state  [1]=mqtt_state（**这字节才是链路状态**）[2..3]=rssi 大端
 *    取值与 gw_link_state_t 同域：0=DISCONNECTED 2=CONNECTED
 * ================================================================== */
static void send_link_status_to_stm32(void)
{
    uint8_t          p[4];
    int16_t          rssi     = 0;
    int              has_rssi = 0;
    wifi_ap_record_t ap;

    p[0] = s_wifi_connected ? 2u : 0u;
    p[1] = s_mqtt_connected ? 2u : 0u;

    /* RSSI 取不到就填 0 并照发 —— 状态字节才是关键，
       不能因为拿不到 RSSI 把整帧吞了（那会重新退回死锁）。 */
    if (s_wifi_connected != 0
        && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = (int16_t)ap.rssi;
        has_rssi = 1;
    }
    p[2] = has_rssi ? (uint8_t)(((uint16_t)rssi) >> 8)  : 0u;
    p[3] = has_rssi ? (uint8_t)(((uint16_t)rssi) & 0xFFu) : 0u;

    uart_send_frame(GW_FT_LINK_STATUS, p, (uint16_t)sizeof(p));
}

/* ==================================================================
 *  MQTT 事件
 * ================================================================== */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;

    (void)handler_args; (void)base;

    switch ((esp_mqtt_event_id_t)event_id) {

    case MQTT_EVENT_CONNECTED:
        s_mqtt_connected = 1;
        /* ⚠️ 这里**不再**重置 Wi-Fi 退避（2026-10-14 修）。
           原来这里调 `esp32_backoff_ok(&s_backoff)`，而那个退避是
           Wi-Fi 与 MQTT **共用**的 —— 结果是"MQTT 一连上就把 Wi-Fi 的
           退避状态清掉"，重连任务随即退化成每 500ms 一次的裸重连。
           Wi-Fi 退避该由 `IP_EVENT_STA_GOT_IP` 重置（见 wifi_event_handler）；
           MQTT 自身的重连由 ESP-MQTT 客户端的自动重连负责。 */
        ESP_LOGI(TAG, "MQTT connected");

        /* 告诉 STM32"链路通了" —— 不发这帧它永远不会开始发遥测 */
        send_link_status_to_stm32();

        (void)esp_mqtt_client_subscribe(s_mqtt, s_topic_command,
                                        GW_MQTT_QOS);
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = 0;
        ESP_LOGW(TAG, "MQTT disconnected");
        /* 断开也要通知：否则 STM32 还以为链路在，遥测会白发 */
        send_link_status_to_stm32();
        break;

    case MQTT_EVENT_DATA: {
        /* ---- 云端下发指令 ---- */
        esp32_cmd_t cmd;
        char json[ESP32_ONENET_JSON_MAX];
        char msg_id[16];
        int  pr;

        if (ev->data_len <= 0 || ev->data_len >= (int)sizeof(json)) {
            ESP_LOGW(TAG, "cmd too long: %d", ev->data_len);
            break;
        }
        memcpy(json, ev->data, (size_t)ev->data_len);
        json[ev->data_len] = '\0';

#if GW_CLOUD_BACKEND == GW_CLOUD_ONENET
        /* OneNET：报文是 {"id":"123","version":"1.0","params":{"cmd":"..."}}，
           id 是**字符串**，要原样回填到 set_reply。 */
        pr = esp32_json_onenet_parse_set(json, (uint16_t)ev->data_len,
                                         &cmd, msg_id,
                                         (uint16_t)sizeof(msg_id));

        /* ⚠️ 立刻回 set_reply（"我收到了"），**不能**等 STM32 执行完。
           平台等的是"属性设置已被受理"，等得不久；而 STM32 侧
           等 ACK 要 2 秒。把执行结果塞进 set_reply 会让平台先超时，
           然后设备再回一条已经没人听的应答 —— 现象是
           "控制台上每次下发都超时，但设备其实执行了"。 */
        {
            char     ackbuf[160];
            uint16_t alen;

            alen = esp32_json_onenet_set_reply(
                       msg_id,
                       (pr == 0) ? 200 : 400,
                       (pr == 0) ? "accepted"
                                 : ((pr == -2) ? "unknown property"
                                               : "bad request"),
                       ackbuf, (uint16_t)sizeof(ackbuf));
            if (alen > 0u) {
                (void)mqtt_publish(s_topic_reply, ackbuf);
            }
        }
#else
        msg_id[0] = '\0';
        pr = esp32_json_parse_command(json, (uint16_t)ev->data_len, &cmd);
#endif

        if (pr != 0) {
            ESP_LOGW(TAG, "bad command json: %s", json);
            /* ⚠️ 解析失败也要给云端回执 ——
               否则云端会一直等到超时，看不出是"格式错了"
               还是"设备不在线"。
               （OneNET 那边上面的 set_reply 已经回了 code=400。） */
#if GW_CLOUD_BACKEND != GW_CLOUD_ONENET
            {
                char reply[220];
                if (esp32_json_cmd_reply(0u, 0, "BAD_JSON", 0,
                                         reply, (uint16_t)sizeof(reply)) > 0u) {
                    (void)mqtt_publish(s_topic_reply, reply);
                }
            }
#endif
            break;
        }

        if (esp32_cmd_track_busy(&s_cmd) != 0) {
            /* 上一条还没确认：明确告诉云端"忙"，而不是覆盖它。
               覆盖会让前一条永远收不到回执。 */
            char reply[220];
#if GW_CLOUD_BACKEND == GW_CLOUD_ONENET
            if (esp32_json_onenet_set_reply(msg_id, 400, "busy",
                                            reply, (uint16_t)sizeof(reply)) > 0u) {
                (void)mqtt_publish(s_topic_reply, reply);
            }
#else
            if (esp32_json_cmd_reply(cmd.req_id, (int)cmd.has_req_id,
                                     "BUSY", 0,
                                     reply, (uint16_t)sizeof(reply)) > 0u) {
                (void)mqtt_publish(s_topic_reply, reply);
            }
#endif
            ESP_LOGW(TAG, "busy, rejected: %s", cmd.cmd);
            break;
        }

        /* 登记并转发给 STM32。
           ⚠️ 文本指令原样转发（GW_FT_COMMAND 的载荷就是 ASCII 文本）。
              STM32 侧的解析器（gw_cmd.c，113 项测试）负责语义。 */
        if (esp32_cmd_track_start(&s_cmd, cmd.cmd, cmd.req_id,
                                  (int)cmd.has_req_id,
                                  (uint32_t)(esp_timer_get_time() / 1000)) != 0) {
            ESP_LOGW(TAG, "cmd track failed");
            break;
        }
        uart_send_frame(GW_FT_COMMAND, (const uint8_t *)cmd.cmd,
                        (uint16_t)strlen(cmd.cmd));
        ESP_LOGI(TAG, "cmd -> STM32: %s", cmd.cmd);
        break;
    }

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        s_mqtt_connected = 0;
        break;

    default:
        break;
    }
}

/* ==================================================================
 *  Wi-Fi 事件
 * ================================================================== */

/** 凭据是否还是模板里的占位符。
 *
 *  ⚠️ 为什么值得专门判断一次：
 *     占位符 SSID 会让 `esp_wifi_connect()` **秒失败** ⇒ 驱动抛
 *     `WIFI_EVENT_STA_DISCONNECTED` ⇒ 重连任务再试 ⇒ 自激。
 *     实测日志量 44 KB/s（每秒上千行 `wifi:sta is connecting`），
 *     既刷爆串口也白占 CPU，而且**看不出真正原因**
 *     （看上去像"Wi-Fi 驱动坏了"，其实是没填凭据）。
 *     与其让它空转，不如一次都不试，并把原因直接写进日志。 */
static int wifi_creds_are_placeholder(void)
{
    return (GW_WIFI_SSID[0] == '\0')
        || (strcmp(GW_WIFI_SSID, "YOUR_SSID") == 0);
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *data)
{
    (void)arg; (void)data;

    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        (void)esp_wifi_connect();
    } else if (base == WIFI_EVENT
               && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        uint32_t delay_s;

        s_wifi_connected = 0;

        /* ⚠️ 重连走退避，不要立刻重试。
           立刻重试会在 AP 掉线时形成"疯狂重连"——
           某些 AP 会因此把这台设备拉黑。 */
        esp32_backoff_fail(&s_wifi_backoff,
                           (uint32_t)(esp_timer_get_time() / 1000));

        /* ⚠️ 日志**节流**：只在退避档位变化时打一行。
           断线事件可能高频重复（凭据不对时尤其明显），每次都打
           会把串口刷爆 —— 实测 44 KB/s 里有一半是这一行贡献的。 */
        delay_s = esp32_backoff_delay(&s_wifi_backoff);
        if (delay_s != s_wifi_logged_delay) {
            s_wifi_logged_delay = delay_s;
            ESP_LOGW(TAG, "WiFi disconnected, next retry in %us",
                     (unsigned)delay_s);
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_wifi_connected = 1;

        /* ⚠️ 连上了必须**完全重置**退避（2026-10-14 补的）。
           原来这里只置了连接标志 —— 于是"连上 → 掉线 → 再连上"之后，
           退避会接着上一轮的 60s 起步，现场表现是
           "重启后第一次断线要等一分钟"。 */
        esp32_backoff_ok(&s_wifi_backoff);
        s_wifi_logged_delay = 0xFFFFFFFFu;

        ESP_LOGI(TAG, "WiFi got IP");
    }
}

/* Wi-Fi 重连检查任务（走退避 + 硬节流） */
static void wifi_reconnect_task(void *arg)
{
    uint32_t last_try_ms = 0u;
    uint32_t tries       = 0u;

    (void)arg;
    for (;;) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        /* ⚠️ 三道闸门，缺一不可：
             ① `s_wifi_connected == 0`  —— 已经连上就什么都不做；
             ② `esp32_backoff_ready()`  —— 软退避（1→2→4→…→60s 指数）；
             ③ `GW_WIFI_MIN_RETRY_MS`   —— **硬**下限，与退避状态无关。
           第 ③ 道是防刷屏的关键：退避是软的，`waiting == 0` 时
           `esp32_backoff_ready()` 恒返回 1，重连就会退化成每 500ms 一次，
           而每次 `esp_wifi_connect()` 都打一行 `wifi:sta is connecting`。 */
        if (s_wifi_connected == 0
            && esp32_backoff_ready(&s_wifi_backoff, now_ms) != 0
            && (uint32_t)(now_ms - last_try_ms) >= GW_WIFI_MIN_RETRY_MS) {
            last_try_ms = now_ms;
            tries++;
            (void)esp_wifi_connect();
            ESP_LOGI(TAG, "wifi retry #%u (backoff %us)",
                     (unsigned)tries,
                     (unsigned)esp32_backoff_delay(&s_wifi_backoff));
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ==================================================================
 *  app_main
 * ================================================================== */
void app_main(void)
{
    /* ---- 初始化纯逻辑模块 ---- */
    (void)esp32_backoff_init(&s_wifi_backoff);
    (void)esp32_cmd_track_init(&s_cmd);
    (void)esp32_telem_ring_init(&s_ring);
    gw_frame_parser_init(&s_parser);

    /* ---- 拼 topic（按后端分支）---- */
#if GW_CLOUD_BACKEND == GW_CLOUD_ONENET
    /* OneNET 系统 topic：$sys/{pid}/{dn}/thing/... —— 必须用**产品ID**和
       **设备名称**，不是数字设备 ID。
       ⚠️ 这两个值同时用于鉴权（clientId/username），填错任何一个
          都会表现为"连上了但收发都不通"，很难二分定位。 */
    snprintf(s_topic_telemetry, sizeof(s_topic_telemetry),
             GW_TOPIC_TELEMETRY,
             GW_ONENET_PRODUCT_ID, GW_ONENET_DEVICE_NAME);
    snprintf(s_topic_command, sizeof(s_topic_command),
             GW_TOPIC_COMMAND,
             GW_ONENET_PRODUCT_ID, GW_ONENET_DEVICE_NAME);
    snprintf(s_topic_reply, sizeof(s_topic_reply),
             GW_TOPIC_REPLY,
             GW_ONENET_PRODUCT_ID, GW_ONENET_DEVICE_NAME);
    s_topic_status[0] = '\0';       /* OneNET 无独立链路状态 topic */

    ESP_LOGI(TAG, "OneNET topic: %s", s_topic_telemetry);
#else
    snprintf(s_topic_telemetry, sizeof(s_topic_telemetry),
             GW_TOPIC_TELEMETRY, (unsigned)GW_DEVICE_ID);
    snprintf(s_topic_status, sizeof(s_topic_status),
             GW_TOPIC_STATUS, (unsigned)GW_DEVICE_ID);
    snprintf(s_topic_reply, sizeof(s_topic_reply),
             GW_TOPIC_REPLY, (unsigned)GW_DEVICE_ID);
    snprintf(s_topic_command, sizeof(s_topic_command),
             GW_TOPIC_COMMAND, (unsigned)GW_DEVICE_ID);
#endif

#if GW_CLOUD_BACKEND == GW_CLOUD_ONENET
    /* ---- OneNET 三要素完整性检查（放在最前面，原因见下）----
       ⚠️ 这条检查放在**初始化之前**，是为了让"三要素没填"这件事
          在日志的第一屏就出现。否则要等到 MQTT 反复重连之后
          才看到一堆 `MQTT disconnected`，而真正的原因在几百行之前。 */
    if (GW_ONENET_PRODUCT_ID[0] == '\0'
        || GW_ONENET_DEVICE_NAME[0] == '\0'
        || GW_MQTT_PASS[0] == '\0') {
        ESP_LOGE(TAG, "==================================================");
        ESP_LOGE(TAG, " OneNET 凭据没填全，MQTT 一定连不上：");
        ESP_LOGE(TAG, "   GW_MQTT_CLIENT_ID  (= 设备名称) %s",
                 (GW_MQTT_CLIENT_ID[0] != '\0') ? "已填" : "**空**");
        ESP_LOGE(TAG, "   GW_MQTT_USER       (= 产品ID)   %s",
                 (GW_MQTT_USER[0] != '\0') ? "已填" : "**空**");
        ESP_LOGE(TAG, "   GW_MQTT_PASS       (= token)    %s",
                 (GW_MQTT_PASS[0] != '\0') ? "已填" : "**空**");
        ESP_LOGE(TAG, "   GW_ONENET_PRODUCT_ID  %s",
                 (GW_ONENET_PRODUCT_ID[0] != '\0') ? "已填" : "**空**");
        ESP_LOGE(TAG, "   GW_ONENET_DEVICE_NAME %s",
                 (GW_ONENET_DEVICE_NAME[0] != '\0') ? "已填" : "**空**");
        ESP_LOGE(TAG, " token 用 tools/onenet_token.py 生成；");
        ESP_LOGE(TAG, " 详见 doc/OneNET接入说明.md");
        ESP_LOGE(TAG, "==================================================");
    }

    /* ⚠️ 一致性检查：鉴权用的 clientId/username，与 topic 里用的
       {设备名}/{产品ID}，**必须是同一对**。

       不一致时的现象特别难查：MQTT 照样能连上（鉴权只校验三要素
       彼此自洽，不管 topic），但发布/订阅会落到"另一个设备"上 ——
       表现为"订阅被拒"或"发布出去毫无反应"，而日志上
       看起来一切正常、连 `MQTT connected` 都打了。

       （两边都留空时 strcmp 相等，不会误报。） */
    if (strcmp(GW_MQTT_CLIENT_ID, GW_ONENET_DEVICE_NAME) != 0) {
        ESP_LOGE(TAG, "clientId(%s) 与 topic 里的设备名(%s) **不一致**",
                 GW_MQTT_CLIENT_ID, GW_ONENET_DEVICE_NAME);
    }
    if (strcmp(GW_MQTT_USER, GW_ONENET_PRODUCT_ID) != 0) {
        ESP_LOGE(TAG, "username(%s) 与 topic 里的产品ID(%s) **不一致**",
                 GW_MQTT_USER, GW_ONENET_PRODUCT_ID);
    }
#endif

    /* ---- NVS（Wi-Fi 驱动需要） ----
       ⚠️ 首次烧录后 NVS 可能有旧数据导致初始化失败，
          这时必须**擦除后重试**，否则设备会一直重启。 */
    {
        esp_err_t r = nvs_flash_init();
        if (r == ESP_ERR_NVS_NO_FREE_PAGES
            || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            (void)nvs_flash_erase();
            r = nvs_flash_init();
        }
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "nvs init failed: %d", (int)r);
        }
    }

    /* ---- UART ----
       ⚠️ pin 分配必须在 install 之前，且 TX/RX 要**交叉**接：
          ESP32_TX -> STM32_RX，ESP32_RX <- STM32_TX。
          接反了完全收不到数据（而现象是"两边都没报错"）。 */
    {
        uart_config_t cfg = {
            .baud_rate  = GW_UART_BAUD,
            .data_bits  = UART_DATA_8_BITS,
            .parity     = UART_PARITY_DISABLE,     /* 8N1 —— 与 STM32 一致 */
            .stop_bits  = UART_STOP_BITS_1,
            .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        ESP_ERROR_CHECK(uart_driver_install(GW_UART_NUM, GW_UART_RX_BUF, 0,
                                            0, 0, 0));
        ESP_ERROR_CHECK(uart_param_config(GW_UART_NUM, &cfg));
        ESP_ERROR_CHECK(uart_set_pin(GW_UART_NUM, GW_UART_TX_GPIO,
                                     GW_UART_RX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    }

    /* ---- Wi-Fi 驱动日志降噪 ----
       ⚠️ 驱动的 INFO 级日志在"连不上"时非常吵（实测 `wifi:sta is connecting`
          能刷到 44 KB/s）。正常运行不需要它；要排障就把这一行注释掉。
          压到 WARN 而不是 NONE —— 断线 / 认证失败这类**真正有用**的
          警告要留着，否则下次真出问题又变成"什么都看不见"。 */
    esp_log_level_set("wifi", ESP_LOG_WARN);

    /* ---- Wi-Fi ---- */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    (void)esp_netif_create_default_wifi_sta();

    /* ⚠️ 凭据还是占位符就**一次都不试**（理由见 wifi_creds_are_placeholder）。
       此时只跑 UART 桥接 —— STM32 侧的帧协议 / CRC / 补传仍能正常联调，
       只是不上云。这比让 Wi-Fi 自激刷屏有用得多。 */
    s_net_enabled = (wifi_creds_are_placeholder() == 0);

    if (s_net_enabled != 0) {
        wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
        wifi_config_t stacfg;

        ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, 0, 0));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, 0, 0));

        memset(&stacfg, 0, sizeof(stacfg));
        strncpy((char *)stacfg.sta.ssid, GW_WIFI_SSID,
                sizeof(stacfg.sta.ssid) - 1u);
        strncpy((char *)stacfg.sta.password, GW_WIFI_PASS,
                sizeof(stacfg.sta.password) - 1u);

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &stacfg));
        ESP_ERROR_CHECK(esp_wifi_start());

        /* ---- SNTP：拿真实时间 ----
           ⚠️ 为什么需要它（而不是直接用 esp_timer_get_time()）：
              后者是"上电以来的微秒数"，不是 unix 时间。OneNET 的
              `time` 字段语义是**属性生成时刻**，填上电毫秒会变成
              1970-01-01，控制台上的曲线时间轴全错 —— 而且看起来
              像"数据是准的，只是时间不对"，很容易被误判成平台时区问题。

           同步之前 now_ms_epoch() 返回 0，报文不带 time 字段，
           由平台盖接收时间（见 now_ms_epoch 的说明）。

           ⚠️ 用国内可达的 NTP 服务器。默认的 pool.ntp.org 在部分
              网络环境下解析不到，表现为"一直不同步"而没有任何报错。 */
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        sntp_set_time_sync_notification_cb(sntp_sync_cb);
        esp_sntp_init();
    } else {
        ESP_LOGW(TAG, "Wi-Fi 凭据仍是占位符（GW_WIFI_SSID=\"%s\"）"
                      " -> 不启动 Wi-Fi/MQTT，只跑 UART 桥接。"
                      "改 esp32/src/main.c 顶部的 GW_WIFI_SSID / GW_WIFI_PASS 后重烧。",
                 GW_WIFI_SSID);
    }

    /* ---- MQTT ---- */
    if (s_net_enabled != 0) {
        esp_mqtt_client_config_t mcfg;

        memset(&mcfg, 0, sizeof(mcfg));
        mcfg.broker.address.uri = GW_MQTT_BROKER;

        /* ⚠️ clientId 必须**显式**给（OneNET 要求它等于"设备名称"）。
           不给的话 esp-mqtt 会自己生成 `ESP32_xxxxxxxxxxxx` 这类随机串，
           OneNET 拿它去匹配设备 -> 匹配不到 -> 直接拒绝连接。
           通用 broker（EMQX 等）留空即可，走 esp-mqtt 的默认行为。 */
        if (GW_MQTT_CLIENT_ID[0] != '\0') {
            mcfg.credentials.client_id = GW_MQTT_CLIENT_ID;
        }
        if (GW_MQTT_USER[0] != '\0') {
            mcfg.credentials.username = GW_MQTT_USER;
            mcfg.credentials.authentication.password = GW_MQTT_PASS;
        }

        /* ⚠️ 专门拦一种最难查的现象："Wi-Fi 连上了，但一直不上云"。
           OneNET 的三要素缺任何一个，平台都只会回 CONNACK rc=4/5
           然后断开 —— 日志上看起来仅仅是反复 `MQTT disconnected`，
           完全看不出"是凭据没填"。与其让人去猜，不如直接把原因喊出来。 */
        if (strstr(GW_MQTT_BROKER, "hecoulds") != 0
            && (GW_MQTT_CLIENT_ID[0] == '\0'
                || GW_MQTT_USER[0] == '\0'
                || GW_MQTT_PASS[0] == '\0')) {
            ESP_LOGE(TAG, "OneNET 鉴权三要素不全（clientId=设备名称 / "
                          "username=产品ID / password=token），"
                          "MQTT 必然连不上。见 doc/OneNET接入说明.md");
        }
        /* ⚠️ KeepAlive 60s 与报告一致。太长会让"断线"发现得很慢
           （要等到下一次心跳超时才知道）。 */
        mcfg.session.keepalive = 60;
        /* MQTT 客户端自带的自动重连**开着**（默认行为）。
           ⚠️ 但注意分工：MQTT 客户端只负责"broker 掉了"的重连，
              **Wi-Fi 掉了**由我们自己的退避任务管（见 wifi_reconnect_task）。
              两层都做 Wi-Fi 重连会互相干扰 —— 客户端会在 Wi-Fi
              还没起来时疯狂重试，某些 AP 会把设备拉黑。 */
        mcfg.network.disable_auto_reconnect = false;   /* 保持默认：开 */

        s_mqtt = esp_mqtt_client_init(&mcfg);
        ESP_ERROR_CHECK(esp_mqtt_client_register_event(
            s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, 0));
        ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt));
    }

    /* ---- 任务 ----
       ⚠️ 重连任务只在网络启用时建：凭据是占位符时它没有任何事可做，
          建出来只会空转（而且它正是刷屏的入口）。 */
    xTaskCreate(uart_rx_task, "gw_uart", 4096, 0, 10, 0);
    xTaskCreate(housekeeping_task, "gw_house", 4096, 0, 5, 0);
    if (s_net_enabled != 0) {
        xTaskCreate(wifi_reconnect_task, "gw_wifi", 3072, 0, 4, 0);
    }

    ESP_LOGI(TAG, "gateway bridge started (dev_id=%u, net=%s, backend=%s)",
             (unsigned)GW_DEVICE_ID, (s_net_enabled != 0) ? "on" : "off",
#if GW_CLOUD_BACKEND == GW_CLOUD_ONENET
             "ONENET/OneJSON");
#else
             "GENERIC/flat-json");
#endif
}
