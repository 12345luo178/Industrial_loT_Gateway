/**
 * @file    esp32_json.h
 * @brief   ESP32 侧 JSON 封装与解析（纯 C，可在 PC 上单元测试）
 *
 * ============================================================================
 *  ⚠️ 为什么手写 JSON 而不用 cJSON / ArduinoJson
 *
 *  本项目要生成的 JSON 是**固定形状**的：
 *
 *      {"dev_id":100000,"ts":1700000000,"seq":42,"temp":23.5,"humi":56.0,
 *       "can":[0,0,0,0],"status":17,"uptime":3600}
 *
 *  没有嵌套、没有数组可变长、没有需要省略的字段。
 *  这种情况下手写的代码量比接一个库还少，而且：
 *
 *    1) **能在 PC 上跑单元测试** —— 这是本项目最看重的性质。
 *       cJSON 也能在 PC 上跑，但它是个外部依赖，
 *       而本项目的 PC 测试环境是"裸 gcc，零依赖"。
 *    2) **浮点格式化可控** —— 温度 23.5 要输出 `23.5` 而不是
 *       `23.500000` 或 `23.499999`。手写能精确控制。
 *    3) **不需要动态内存** —— cJSON 会 malloc。ESP32 上有堆，
 *       但 MQTT 回调里 malloc 是有风险的（回调在协议栈任务里跑）。
 *
 *  ⚠️ 但手写 JSON 有个必须处理的陷阱：**字符串转义**。
 *     本模块生成的所有字符串里只有 `cmd`（来自云端），
 *     所以只有它需要转义 —— 见 `json_escape()`。
 *     不做转义的后果是**注入**：云端发来 `{"cmd":"a\"b"}`，
 *     拼出来的 JSON 就坏了，轻则解析失败，
 *     重则让云端把它当成两个字段。
 * ============================================================================
 */
#ifndef ESP32_JSON_H
#define ESP32_JSON_H

#include <stdint.h>

/* ==================================================================
 *  遥测 JSON
 * ================================================================== */

/**
 * 把 STM32 传来的 28 字节遥测载荷转成 JSON
 *
 * @param payload  GW_FT_TELEMETRY 的载荷（28 字节，大端）
 * @param len      载荷长度
 * @param ts_fix   时间戳修正：STM32 没有 RTC，它的 ts 是"上电秒数"。
 *                 ESP32 有 NTP，所以用**自己的**时间覆盖它。
 *                 传 0 表示不覆盖（保留 STM32 的值，用于测试）。
 * @param out      输出缓冲
 * @param cap      容量
 * @return 写入的字符数（不含 '\0'）；0 表示失败（载荷非法或缓冲不够）
 *
 * 输出示例：
 *   {"dev_id":100000,"ts":1700000000,"seq":42,"temp":23.5,"humi":56.0,
 *    "can":[0,0,0,0],"status":17,"uptime":3600}
 *
 * ⚠️ 温度是**有符号**的。DHT22 量程 -40~80℃，
 *    用无符号解析会把 -5℃ 变成 6553.1℃ 报给云端。
 */
uint16_t esp32_json_telemetry(const uint8_t *payload, uint16_t len,
                              uint32_t ts_fix, char *out, uint16_t cap);

/* ==================================================================
 *  链路状态 JSON（ESP32 -> 云端，与遥测分开的 topic）
 * ================================================================== */

/** 链路状态（与 STM32 的 gw_link_state_t 保持一致的语义） */
typedef enum {
    ESP32_LINK_DISCONNECTED = 0,
    ESP32_LINK_CONNECTING   = 1,
    ESP32_LINK_CONNECTED    = 2,
    ESP32_LINK_AUTH_FAILED  = 3
} esp32_link_state_t;

/**
 * 生成链路状态 JSON
 *
 * @param dev_id    设备 ID
 * @param wifi       Wi-Fi 状态
 * @param mqtt       MQTT 状态
 * @param rssi_dbm   Wi-Fi 信号强度（dBm，负值；0 表示未知）
 * @param backlog    STM32 侧待补传条数（让云端能看出"设备在补数据"）
 * @param out/cap    输出
 * @return 写入字符数；0 失败
 */
uint16_t esp32_json_link_status(uint32_t dev_id,
                                esp32_link_state_t wifi,
                                esp32_link_state_t mqtt,
                                int16_t rssi_dbm,
                                uint16_t backlog,
                                char *out, uint16_t cap);

/* ==================================================================
 *  下行指令 JSON（云端 -> ESP32）
 * ================================================================== */

/** 指令 JSON 解析结果 */
typedef struct {
    char     cmd[64];       /* 指令文本（已反转义），例如 "SET_PERIOD 30" */
    uint32_t req_id;        /* 云端给的请求 ID（用于回执关联）；无则 0 */
    uint8_t  has_req_id;
} esp32_cmd_t;

/**
 * 解析云端下发的指令 JSON
 *
 * @param json  输入（**不要求以 '\0' 结尾之外的处理**，内部按长度扫）
 * @param len   长度
 * @param out   输出
 * @return 0 成功；<0 失败
 *
 * 支持的形状（宽松解析，不要求字段顺序）：
 *   {"cmd":"SET_PERIOD 30"}                  最简形式
 *   {"cmd":"SET_PERIOD 30","id":12}          带请求 ID
 *   { "id" : 12 , "cmd" : "REPORT_NOW" }     带空格
 *
 * ⚠️ 为什么"宽松"很重要：云端可能是 Node-RED / EMQX 规则引擎 /
 *    手写的 Python 脚本，它们生成的 JSON 空格与字段顺序各不相同。
 *    严格的解析器在现场会莫名其妙地"收不到指令"。
 *
 * ⚠️ 但宽松不等于不设防：**必须拒绝超出缓冲的 cmd**，
 *    否则一个超长指令会截断执行 —— 截断后的 `SET_PERIOD 3`
 *    （原意 3000）会静默生效，而云端以为设置成功了。
 *    所以超长时返回错误，而不是截断。
 */
int esp32_json_parse_command(const char *json, uint16_t len,
                             esp32_cmd_t *out);

/**
 * 生成指令执行回执 JSON（ESP32 -> 云端）
 *
 * @param req_id    云端给的请求 ID（has_req_id 为 0 时忽略）
 * @param has_req_id 是否有请求 ID
 * @param ack_text  STM32 返回的原始确认文本，形如 "PERIOD OK"
 *                  （来自 GW_FT_ACK/NACK 的载荷）
 * @param ok        1 = ACK，0 = NACK
 * @param out/cap   输出
 * @return 写入字符数
 *
 * ⚠️ ack_text 来自 STM32，必须**转义**后再嵌入 ——
 *    虽然当前 STM32 只会生成 `[A-Z_]+ (OK|..._ERROR)` 这种安全字符，
 *    但依赖"对端只会发安全字符"是脆弱的假设。
 *    一旦将来命令词允许用户自定义，这里就成了注入点。
 */
uint16_t esp32_json_cmd_reply(uint32_t req_id, int has_req_id,
                              const char *ack_text, int ok,
                              char *out, uint16_t cap);

/* ==================================================================
 *  工具
 * ================================================================== */

/**
 * 把字符串转义后写入（供内部与测试使用）
 *
 * 转义：`"` -> `\"`，`\` -> `\\`，控制字符 -> `\u00XX`
 * @return 写入字符数；缓冲不足返回 0
 */
uint16_t esp32_json_escape(const char *in, uint16_t in_len,
                           char *out, uint16_t cap);

/**
 * 把 ×10 定点数格式化成带一位小数的字符串
 *
 * @param v    定点值（例如 235 表示 23.5）
 * @param is_signed 是否有符号（温度有、湿度没有）
 * @param out/cap 输出
 * @return 写入字符数
 *
 * ⚠️ 为什么要自己做而不是 `snprintf("%.1f", v/10.0)`：
 *    1) 浮点会链进浮点格式化代码（ESP32 上没关系，但本项目
 *       希望这段逻辑能在 PC 与 STM32 上共用）
 *    2) `235/10.0` 在二进制浮点里是 23.499999...，
 *       某些 printf 实现会输出 `23.5`，某些会输出 `23.499999`
 *    3) 负数的处理容易写错：-5 应当是 -0.5，
 *       而 C 的整数除法 -5/10 = 0（向零取整），会丢掉符号
 */
uint16_t esp32_json_fixed1(int32_t v, int is_signed, char *out, uint16_t cap);

/* ==================================================================
 *  ★ OneNET 物模型（OneJSON）
 *
 *  ⚠️ 为什么需要单独一套：OneNET Studio 不是通用 MQTT broker。
 *     官方原文：「暂不支持用户自定义 topic，仅限使用系统 topic」，
 *     而且上行报文必须是**物模型 OneJSON**，不是上面那种扁平 JSON。
 *
 *     两者形状完全不同：
 *
 *       扁平（EMQX 路径，esp32_json_telemetry）:
 *         {"dev_id":100000,"ts":1700000000,"temp":23.5,...}
 *
 *       物模型（OneNET 路径）:
 *         {"id":"1","version":"1.0",
 *          "params":{"CurrentTemperature":{"value":23.5,"time":1700000000000},
 *                    ...}}
 *
 *     ⚠️ 注意物模型里温度用的是平台标准功能点名 CurrentTemperature，
 *        不是 `temp` —— 见下面 ESP32_ONENET_ID_* 的说明。
 *
 *     所以不是"改几个字段名"，而是**换一种报文形状**。
 *     本组函数只负责拼/解报文，**不碰 topic** —— topic 在 main.c 里拼。
 * ================================================================== */

/** OneNET 物模型报文的建议缓冲大小。
 *
 *  ⚠️ 这个值不是拍脑袋来的：最坏情况（id 10 位 + 4 个 CAN 通道
 *     全 65535 + 13 位毫秒时间戳 + 负温度）实测约 445 字节，
 *     这里留了 ~200 字节余量。`test_esp32_json.c` 里有
 *     "最坏情况必须装得下"的断言，改字段时会被立刻发现。 */
#define ESP32_ONENET_JSON_MAX       640u

/** OneNET 物模型版本号（官方示例值，可选字段） */
#define ESP32_ONENET_VERSION        "1.0"

/* ------------------------------------------------------------------
 *  ★ OneNET 物模型属性标识符（**云端契约**）
 *
 *  ⚠️ 这 15 个字符串必须与控制台「物模型」里的标识符**逐字一致**，
 *     大小写敏感。拼错一个字母，平台回 code=2306（"identifier not exist"），
 *     而且**一次只报一个** —— 靠试错要来回十几轮。
 *     （实测见 doc/OneNET接入说明.md §8.7。）
 *
 *  ⚠️ 这里是**唯一**的 C 侧定义处：esp32_json.c 只引用这些宏，
 *     不再散落字符串字面量。tools/check_onenet_ids.py 会把它们与
 *     tools/onenet_probe.py 的 PROP_CHECKS、tools/gen_onenet_tsl.py 的
 *     PROPERTIES 三处比对，防止固件/探针/物模型悄悄漂移。
 *
 *  ⚠️ ESP32_ONENET_ID_TEMP 比较特殊：它用的是平台「环境监测」类目
 *     自带的**标准功能点** CurrentTemperature，**注意没有空格** ——
 *     控制台界面上因列宽折行看起来像有空格，以「导出物模型」的 JSON 为准。
 *     所以这个名字**不符合**本项目其余标识符的小写风格，是有意为之。
 *
 *  ⚠️ 而 STM32↔ESP32 那条链路上的字段名仍然是 `temp`（见 esp32_bridge.h
 *     的 gw_telemetry_t）—— 那是内部线格式，与云端契约无关，不要一起改。
 * ------------------------------------------------------------------ */
#define ESP32_ONENET_ID_TEMP        "CurrentTemperature"   /* 平台标准点 */
#define ESP32_ONENET_ID_HUMI        "humi"
#define ESP32_ONENET_ID_SEQ         "seq"
#define ESP32_ONENET_ID_STATUS      "status"
#define ESP32_ONENET_ID_UPTIME      "uptime"
#define ESP32_ONENET_ID_CAN1        "can1"
#define ESP32_ONENET_ID_CAN2        "can2"
#define ESP32_ONENET_ID_CAN3        "can3"
#define ESP32_ONENET_ID_CAN4        "can4"
#define ESP32_ONENET_ID_WIFI        "wifi"
#define ESP32_ONENET_ID_MQTT        "mqtt"
#define ESP32_ONENET_ID_RSSI        "rssi"
#define ESP32_ONENET_ID_BACKLOG     "backlog"
#define ESP32_ONENET_ID_CMD         "cmd"
#define ESP32_ONENET_ID_CMD_RESULT  "cmd_result"

/**
 * 生成 OneNET 物模型「属性上报」报文（遥测走这条）
 *
 * 输出形状（温度用平台标准点，**不是** "temp"）：
 *   {"id":"42","version":"1.0","params":{
 *      "CurrentTemperature":{"value":23.5,"time":1700000000000},
 *      "humi":{"value":56.0,"time":1700000000000},
 *      "seq":{"value":42,"time":1700000000000},
 *      "status":{"value":17,"time":1700000000000},
 *      "uptime":{"value":3600,"time":1700000000000},
 *      "can1":{"value":0,"time":1700000000000}, ... "can4":{...}}}
 *
 * @param payload  GW_FT_TELEMETRY 的载荷（28 字节，大端）
 * @param len      载荷长度
 * @param msg_id   报文 id（平台用它关联 post/reply）。数字字符串，≤13 位
 * @param ts_ms    属性时间戳（**毫秒**，unix 纪元）。传 0 则不带 time 字段
 * @param out/cap  输出
 * @return 写入字符数（不含 '\0'）；0 = 失败（载荷非法或缓冲不够）
 *
 * ⚠️ 为什么带 time：ESP32 侧的积压队列会在 MQTT 断线时**攒着**
 *    报文，恢复后一起补发。这时若靠平台的接收时间，
 *    补发的几十条会挤在同一秒 —— 曲线变成一根竖线。
 *    带上 time，平台才能还原真实时间轴。
 *
 * ⚠️ 温度是**有符号**的（DHT22 量程 -40~80℃）。
 *    按无符号解析会把 -5℃ 变成 6553.1℃。
 */
uint16_t esp32_json_onenet_property(const uint8_t *payload, uint16_t len,
                                    uint32_t msg_id, uint64_t ts_ms,
                                    char *out, uint16_t cap);

/**
 * 生成 OneNET 物模型「链路状态」属性上报
 *
 * 与遥测分开成两条报文，因为链路状态是 ESP32 自己的事、
 * 与 STM32 的遥测节奏无关（见 main.c 的 housekeeping_task）。
 *
 * @param rssi_dbm  信号强度（dBm，负值）
 * @param has_rssi  0 = 取不到 AP 信息，此时**不上报 rssi 属性**
 *                  （而不是上报 0 —— 0 dBm 是很强的信号，会误导）
 * @param backlog   待补传条数
 * @return 写入字符数；0 失败
 */
uint16_t esp32_json_onenet_link(uint32_t msg_id, uint64_t ts_ms,
                                esp32_link_state_t wifi,
                                esp32_link_state_t mqtt,
                                int16_t rssi_dbm, int has_rssi,
                                uint16_t backlog,
                                char *out, uint16_t cap);

/**
 * 解析 OneNET 下行「属性设置」报文，取出指令文本
 *
 * 输入形状（官方原文示例）：
 *   {"id":"123","version":"1.0","params":{"cmd":"REPORT_NOW"}}
 *
 * @param json      输入
 * @param len       长度
 * @param out       输出（`out->cmd` 为指令文本）
 * @param out_id    输出：平台的报文 id（**字符串**，原样拷出）。
 *                  ⚠️ 必须在 `set_reply` 里原样回填，否则平台无法关联，
 *                  会一直等超时。
 * @param id_cap    out_id 容量（建议 ≥ 16）
 * @return 0 成功；<0 失败
 *
 * ⚠️ OneNET 的 id 是**字符串**（`"id":"123"`），不是数字 ——
 *    与通用路径的 `{"cmd":"...","id":12}` 不一样。
 *    这里按字符串处理并原样回填，避免"数字化了再格式化回去"
 *    丢掉前导零或超长 id。
 */
int esp32_json_onenet_parse_set(const char *json, uint16_t len,
                                esp32_cmd_t *out,
                                char *out_id, uint16_t id_cap);

/**
 * 生成 OneNET「属性设置应答」（set_reply）
 *
 * 输出形状：`{"id":"123","code":200,"msg":"ok"}`
 *
 * ⚠️ 这个应答的含义是「**我收到了这条属性设置**」，
 *    不是「指令已经执行完」。平台等的是前者，且等得不久。
 *    执行结果另外用 `esp32_json_onenet_result()` 作为属性上报回去。
 *
 * @param msg_id  平台给的 id（原样回填）
 * @param code    200 = 成功；其余见 OneNET 文档
 * @param msg     说明文字（会被转义）
 * @return 写入字符数；0 失败
 */
uint16_t esp32_json_onenet_set_reply(const char *msg_id, int code,
                                     const char *msg,
                                     char *out, uint16_t cap);

/**
 * 生成 OneNET 物模型「指令执行结果」属性上报
 *
 * 输出形状：
 *   {"id":"7","version":"1.0","params":{
 *      "cmd_result":{"value":"REPORT OK","time":1700000000000}}}
 *
 * 需要在控制台建一个 string 类型的**只读**属性 `cmd_result`。
 *
 * @param result  STM32 返回的确认文本（形如 "REPORT OK"），会被转义
 * @param ok      1 = ACK，0 = NACK（NACK 时文本前缀加 "FAIL: "）
 * @return 写入字符数；0 失败
 */
uint16_t esp32_json_onenet_result(uint32_t msg_id, uint64_t ts_ms,
                                  const char *result, int ok,
                                  char *out, uint16_t cap);

#endif /* ESP32_JSON_H */
