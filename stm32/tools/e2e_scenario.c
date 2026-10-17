/**
 * @file    e2e_scenario.c
 * @brief   端到端场景运行器 —— 输出完整的线上字节流供独立验证
 *
 * ============================================================================
 *  这个工具解决什么问题
 *
 *  项目里已有 2228 项测试，但集成测试（`test_app.c`）是**"代码和自己比"**：
 *  期望字节是 C 代码自己算的。那只能证明"代码前后一致"，
 *  **不能证明"发出的字节符合 Modbus 规范"**。
 *
 *  `tools/ref_frames.py` 里有一份**独立的 Python Modbus 实现**
 *  （不是从 C 反推的），但集成测试一直没用它。
 *
 *  ----------------------------------------------------------------------------
 *  本工具的做法：把"验证"完全交给外部
 *
 *  它本身**不做任何断言**，只做两件事：
 *
 *    1) 按一个真实运行场景驱动整个应用
 *    2) 把经过两个串口的**每一个字节**按固定格式打到 stdout
 *
 *  然后由 `tools/e2e_verify.py` 独立解析这份 transcript，
 *  逐帧核对 Modbus 规范（CRC、功能码语义、字节数、异常码合理性、
 *  广播不应答……）。
 *
 *  这样：
 *    - transcript 是**稀疏、自描述**的（只有方向 + 十六进制字节）
 *    - Python 侧不需要硬编码期望值，它只需知道**规范**
 *    - C 侧改错了，Python 侧会独立发现（而不是"C 改了什么就期望什么"）
 *
 *  ⚠️ 一个刻意的取舍：Python 侧**不核对具体寄存器值**。
 *     因为那需要把寄存器映射表在 Python 里再实现一遍 ——
 *     那种"重复实现"迟早会与 C 侧不同步，而且同步的过程本身
 *     又变成"两边互相对照"，失去独立性的意义。
 *     值层面的正确性由 `test_regmap.c`（55 项）覆盖；
 *     本工具负责的是**线上格式与协议语义**。
 *
 *  ----------------------------------------------------------------------------
 *  用法（通常由 tools/e2e_run.ps1 调用，不手动跑）
 *
 *      e2e_scenario.exe > transcript.txt
 *      python tools/e2e_verify.py transcript.txt
 *
 *  transcript 格式（每行一条记录）：
 *
 *      RS485 TX <hex>     应用通过 RS485 发出的字节（从站响应 / 主站请求）
 *      RS485 RX <hex>     喂给应用的字节（模拟上位机 / 从站的应答）
 *      ESP   TX <hex>     应用通过 ESP32 串口发出的帧
 *      ESP   RX <hex>     喂给应用的帧
 *      NOTE  <text>       场景标注（仅供人读，验证器忽略）
 * ============================================================================
 */

#include <stdio.h>
#include <string.h>

#include "gw_app.h"
#include "gw_frame.h"
#include "modbus_regs.h"
#include "modbus_crc.h"
/* dht22 的类型经 gw_app.h 传递过来，不需要单独包含 */

/* ==================================================================
 *  假移植层
 * ================================================================== */

#define RX_Q_MAX    16

typedef struct {
    mb_regs_t regs;

    /* 待处理的上位机请求（模拟 RS485 收到的帧） */
    uint8_t  rx[RX_Q_MAX][MB_MAX_ADU];
    uint16_t rx_len[RX_Q_MAX];
    int      rx_n;

    /* 采样行为注入 */
    int      dht_fail;          /* 1 = 采样返回校验错误 */
    int      dht_absent;        /* 1 = 没有注入 dht_sample 回调 */
    int32_t  dht_temp_x10;
    uint16_t dht_humi_x10;
    int      dht_calls;

    /* 1 = 当前轮到主站发请求（用于区分出站帧的方向）*/
    int      master_busy;
} fake_rs485_t;

/* ESP32 侧 */
typedef struct {
    uint8_t  tx[RX_Q_MAX][GW_FRAME_MAX_TOTAL];
    uint16_t tx_len[RX_Q_MAX];
    int      tx_n;

    uint8_t  rx[RX_Q_MAX][GW_FRAME_MAX_TOTAL];
    uint16_t rx_len[RX_Q_MAX];
    int      rx_n;
} fake_esp_t;

static fake_rs485_t RS;
static fake_esp_t   ES;

/* ---------------- transcript 辅助 ---------------- */

static void dump(const char *dir, const char *who,
                 const uint8_t *buf, uint16_t len)
{
    uint16_t i;

    printf("%s %s ", who, dir);
    for (i = 0u; i < len; i++) {
        printf("%02X", buf[i]);
    }
    printf("\n");
}

static void note(const char *text)
{
    printf("NOTE %s\n", text);
}

/* ---------------- RS485 端口回调 ---------------- */

static int rs_take(void *ctx, uint8_t *buf, uint16_t cap, uint16_t *len)
{
    fake_rs485_t *f = (fake_rs485_t *)ctx;
    int i;

    (void)ctx;
    if (f->rx_n <= 0) {
        return -1;
    }
    if (f->rx_len[0] > cap) {
        return -2;
    }
    memcpy(buf, f->rx[0], f->rx_len[0]);
    *len = f->rx_len[0];
    dump("RX", "RS485", buf, *len);

    for (i = 1; i < f->rx_n; i++) {
        memcpy(f->rx[i - 1], f->rx[i], f->rx_len[i]);
        f->rx_len[i - 1] = f->rx_len[i];
    }
    f->rx_n--;
    return 0;
}

/**
 * ⚠️ 这里要区分两种出站帧，否则验证器分不清它们：
 *
 *     从站响应（SLAVERESP）—— 要严格按 Modbus 规范核对
 *                               （响应长度、字节数公式、异常格式）
 *     主站请求（MASTREQ）  —— 是**我们发出去的请求**，
 *                               按"请求"的规范核对（长度固定 8）
 *
 * 一开始我把两者都打成一类，验证器就把主站请求当成"字节数为 0 的
 * 读响应"而误报。标注清楚之后两边都能各按自己的规范验证。
 */
static int rs_send(void *ctx, const uint8_t *buf, uint16_t len)
{
    (void)ctx;
    if (RS.master_busy) {
        dump("TX", "MASTREQ", buf, len);
    } else {
        dump("TX", "SLAVERESP", buf, len);
    }
    return 0;
}

static mb_regs_t *rs_regs(void *ctx)
{
    return &((fake_rs485_t *)ctx)->regs;
}

static dht22_err_t rs_dht(void *ctx, dht22_filtered_t *filt, uint8_t retry)
{
    fake_rs485_t *f = (fake_rs485_t *)ctx;
    dht22_raw_t   raw;

    (void)retry;
    f->dht_calls++;

    if (f->dht_fail) {
        dht22_filter_push(filt, 0);
        return DHT22_ERR_CHECKSUM;
    }

    raw.temp_x10    = (int16_t)f->dht_temp_x10;
    raw.humi_x10    = f->dht_humi_x10;
    raw.checksum_ok = 1u;
    dht22_filter_push(filt, &raw);
    return DHT22_OK;
}

/* ---------------- ESP32 端口回调 ---------------- */

static int es_send(void *ctx, const uint8_t *buf, uint16_t len)
{
    (void)ctx;
    dump("TX", "ESP", buf, len);
    return 0;
}

static void es_tick(void *ctx)
{
    (void)ctx;
}

static int es_take(void *ctx, uint8_t *out_type, uint8_t *payload,
                   uint16_t cap, uint16_t *out_len)
{
    fake_esp_t *f = (fake_esp_t *)ctx;
    gw_frame_parser_t p;
    uint8_t  type = 0u;
    uint16_t plen = 0u;

    if (f->rx_n <= 0) {
        return -1;
    }

    /* 用真实的帧解析器解开它 —— 这样喂给应用层的载荷是"真解出来的"，
       而不是人工拼接的。 */
    gw_frame_parser_init(&p);
    gw_frame_feed(&p, f->rx[0], f->rx_len[0]);

    if (gw_frame_take(&p, &type, payload, cap, &plen) != GW_FRAME_OK) {
        return -2;
    }
    (void)out_type;
    *out_type  = type;
    *out_len   = plen;
    dump("RX", "ESP", f->rx[0], f->rx_len[0]);

    /* 出队 */
    {
        int i;
        for (i = 1; i < f->rx_n; i++) {
            memcpy(f->rx[i - 1], f->rx[i], f->rx_len[i]);
            f->rx_len[i - 1] = f->rx_len[i];
        }
        f->rx_n--;
    }
    return 0;
}

static uint32_t es_now(void *ctx)
{
    (void)ctx;
    return 1000u;
}

/* ==================================================================
 *  构造请求
 * ================================================================== */

/** 造一帧 Modbus 请求塞进接收队列（CRC 由本函数补上） */
static void push_req(const uint8_t *pdu, uint16_t pdu_len)
{
    uint8_t  *dst;
    uint16_t  crc;

    if (RS.rx_n >= RX_Q_MAX) {
        return;
    }
    dst = RS.rx[RS.rx_n];
    memcpy(dst, pdu, pdu_len);
    crc = mb_crc16(pdu, pdu_len);
    dst[pdu_len]      = (uint8_t)(crc & 0xFFu);
    dst[pdu_len + 1u] = (uint8_t)(crc >> 8);

    /* 记录"上位机发了什么"：验证器需要它来判断响应是否配得上请求 */
    printf("REQ RS485 ");
    {
        uint16_t i;
        for (i = 0u; i < pdu_len + 2u; i++) {
            printf("%02X", dst[i]);
        }
    }
    printf("\n");

    RS.rx_len[RS.rx_n] = (uint16_t)(pdu_len + 2u);
    RS.rx_n++;
}

/** 造一帧串口帧塞进 ESP32 接收队列 */
static void push_esp_frame(uint8_t type, const uint8_t *payload, uint16_t len)
{
    uint8_t  frame[GW_FRAME_MAX_TOTAL];
    uint16_t flen = 0u;

    if (ES.rx_n >= RX_Q_MAX) {
        return;
    }
    if (gw_frame_build(frame, (uint16_t)sizeof(frame), type, payload, len,
                       &flen) != GW_FRAME_OK) {
        return;
    }
    memcpy(ES.rx[ES.rx_n], frame, flen);
    ES.rx_len[ES.rx_n] = flen;
    ES.rx_n++;

    printf("REQ ESP ");
    {
        uint16_t i;
        for (i = 0u; i < flen; i++) {
            printf("%02X", frame[i]);
        }
    }
    printf("\n");
}

/* ==================================================================
 *  场景
 * ================================================================== */

int main(void)
{
    gw_app_t         app;
    gw_port_t        port;
    gw_uplink_port_t up;
    mb_slave_t       slave;
    uint16_t         dt = 100u;
    int              step;

    memset(&RS, 0, sizeof(RS));
    memset(&ES, 0, sizeof(ES));
    RS.dht_temp_x10 = 253;      /* 25.3 ℃ */
    RS.dht_humi_x10 = 612u;     /* 61.2 %RH */

    memset(&port, 0, sizeof(port));
    port.rs485_take_frame = rs_take;
    port.rs485_send       = rs_send;
    port.get_regs         = rs_regs;
    port.dht_sample       = rs_dht;
    port.ctx              = &RS;

    mb_slave_init(&slave, 1u, &RS.regs);
    (void)gw_app_init(&app, &slave, &port);

    memset(&up, 0, sizeof(up));
    up.esp_send       = es_send;
    up.esp_tick       = es_tick;
    up.esp_take_frame = es_take;
    up.now_ms         = es_now;
    up.ctx            = &ES;
    (void)gw_app_enable_uplink(&app, &up, 100000u);

    note("scenario start: slave+N master+ sensor+ uplink+");

    /* ================================================================
     * 阶段 1：从站基础读写
     * ================================================================ */
    note("phase1: slave read/write");

    {
        /* 读输入寄存器 0..3（温度/湿度/CAN1/CAN2）*/
        const uint8_t pdu[] = { 0x01u, 0x04u, 0x00u, 0x00u, 0x00u, 0x04u };
        push_req(pdu, sizeof(pdu));
    }
    gw_app_poll(&app, dt);

    {
        /* 读保持寄存器 0..3（设备 ID 高低 + 波特率 + 上报周期）*/
        const uint8_t pdu[] = { 0x01u, 0x03u, 0x00u, 0x00u, 0x00u, 0x04u };
        push_req(pdu, sizeof(pdu));
    }
    gw_app_poll(&app, dt);

    {
        /* 写单个保持寄存器：上报周期 = 30 */
        const uint8_t pdu[] = { 0x01u, 0x06u, 0x00u, 0x03u, 0x00u, 0x1Eu };
        push_req(pdu, sizeof(pdu));
    }
    gw_app_poll(&app, dt);

    {
        /* 写多个保持寄存器：CAN 滤波 ID 高低 = 0x0123 */
        const uint8_t pdu[] = { 0x01u, 0x10u, 0x00u, 0x05u, 0x00u, 0x02u,
                                0x04u, 0x01u, 0x23u, 0x00u, 0x00u };
        push_req(pdu, sizeof(pdu));
    }
    gw_app_poll(&app, dt);

    /* ================================================================
     * 阶段 2：异常路径
     * ================================================================ */
    note("phase2: exception paths");

    {
        /* 非法功能码 -> 异常 01 */
        const uint8_t pdu[] = { 0x01u, 0x63u, 0x00u, 0x00u, 0x00u, 0x01u };
        push_req(pdu, sizeof(pdu));
    }
    gw_app_poll(&app, dt);

    {
        /* 非法数据地址：输入寄存器只有 32 个，读 0x0020 开始 -> 异常 02 */
        const uint8_t pdu[] = { 0x01u, 0x04u, 0x00u, 0x20u, 0x00u, 0x01u };
        push_req(pdu, sizeof(pdu));
    }
    gw_app_poll(&app, dt);

    {
        /* 非法数据值：数量 = 0 -> 异常 03 */
        const uint8_t pdu[] = { 0x01u, 0x03u, 0x00u, 0x00u, 0x00u, 0x00u };
        push_req(pdu, sizeof(pdu));
    }
    gw_app_poll(&app, dt);

    {
        /* 地址不匹配（发给 0x09，本机是 1）-> 应当**完全不应答** */
        const uint8_t pdu[] = { 0x09u, 0x03u, 0x00u, 0x00u, 0x00u, 0x01u };
        push_req(pdu, sizeof(pdu));
    }
    gw_app_poll(&app, dt);

    {
        /* 广播（地址 0）读保持寄存器 -> 应当**执行但不应答** */
        const uint8_t pdu[] = { 0x00u, 0x03u, 0x00u, 0x00u, 0x00u, 0x02u };
        push_req(pdu, sizeof(pdu));
    }
    gw_app_poll(&app, dt);

    {
        /* CRC 错误的帧：手工构造一个坏 CRC（故意不用 push_req）*/
        const uint8_t bad[] = { 0x01u, 0x03u, 0x00u, 0x00u, 0x00u, 0x02u,
                                0x00u, 0x00u };
        if (RS.rx_n < RX_Q_MAX) {
            memcpy(RS.rx[RS.rx_n], bad, sizeof(bad));
            RS.rx_len[RS.rx_n] = sizeof(bad);
            RS.rx_n++;
            printf("REQ RS485 ");
            {
                uint16_t i;
                for (i = 0u; i < sizeof(bad); i++) {
                    printf("%02X", bad[i]);
                }
            }
            printf("\n");
        }
    }
    gw_app_poll(&app, dt);

    /* ================================================================
     * 阶段 3：传感器数据进入寄存器表
     * ================================================================ */
    note("phase3: sensor into registers");

    for (step = 0; step < 30; step++) {
        gw_app_poll(&app, 1000u);       /* 推进 30 秒，跨过采样节拍 */
    }

    {
        /* 再读一次输入寄存器，此时温湿度应已更新 */
        const uint8_t pdu[] = { 0x01u, 0x04u, 0x00u, 0x00u, 0x00u, 0x02u };
        push_req(pdu, sizeof(pdu));
    }
    gw_app_poll(&app, dt);

    /* ================================================================
     * 阶段 4：传感器故障时的行为
     * ================================================================ */
    note("phase4: sensor failure");

    RS.dht_fail = 1;
    for (step = 0; step < 30; step++) {
        gw_app_poll(&app, 1000u);
    }
    {
        /* 读设备状态位：DHT_OK 位应被清除 */
        const uint8_t pdu[] = { 0x01u, 0x04u, 0x00u, 0x0Au, 0x00u, 0x01u };
        push_req(pdu, sizeof(pdu));
    }
    gw_app_poll(&app, dt);

    RS.dht_fail = 0;

    /* ================================================================
     * 阶段 5：上云（遥测与下行指令）
     * ================================================================ */
    note("phase5: uplink telemetry and downlink command");

    {
        /* 链路状态：Wi-Fi 与 MQTT 都连上 */
        uint8_t pl[4];
        pl[0] = 1u;
        pl[1] = 2u;             /* GW_LINK_CONNECTED */
        pl[2] = 0u;
        pl[3] = 0u;
        push_esp_frame(GW_FT_LINK_STATUS, pl, 4u);
    }
    gw_app_poll(&app, dt);
    gw_app_poll(&app, 20000u);      /* 推进到上报节拍 */

    {
        /* 下行指令：立即上报 */
        const char *cmd = "REPORT_NOW";
        push_esp_frame(GW_FT_COMMAND, (const uint8_t *)cmd,
                       (uint16_t)strlen(cmd));
    }
    gw_app_poll(&app, dt);

    /* ⚠️ 下面用的是**线协议文本**，不是内部枚举名。
       这两者容易混淆 —— 我自己就在这里踩过一次：
       内部枚举叫 `GW_CMD_SET_PERIOD`，而线上要发的是 `period 45`。
       发 `SET_PERIOD 45` 会得到 `UNKNOWN_CMD`。

       完整词表（见 gw_cmd_usage 与 gw_cmd_parse）：
         report | period <1..3600> | save 0x5A5A | restore 0xA5A5
         reboot 0x1234 | clear_sticky <mask> | status | uplink <on|off>
       别名：REPORT_NOW=report, CLEAR=CLEAR_STICKY, FACTORY=RESET=restore */

    /* 覆盖全部 8 个命令 + 1 个别名 + 3 个错误路径 */

    {
        const char *cmd = "period 45";          /* 设置上报周期（合法）*/
        push_esp_frame(GW_FT_COMMAND, (const uint8_t *)cmd,
                       (uint16_t)strlen(cmd));
    }
    gw_app_poll(&app, dt);

    {
        const char *cmd = "period 0";           /* 周期 0 越界 -> RANGE_ERROR */
        push_esp_frame(GW_FT_COMMAND, (const uint8_t *)cmd,
                       (uint16_t)strlen(cmd));
    }
    gw_app_poll(&app, dt);

    {
        const char *cmd = "status";             /* 请求状态 */
        push_esp_frame(GW_FT_COMMAND, (const uint8_t *)cmd,
                       (uint16_t)strlen(cmd));
    }
    gw_app_poll(&app, dt);

    {
        const char *cmd = "clear_sticky 0x00FF";/* 清粘滞状态位 */
        push_esp_frame(GW_FT_COMMAND, (const uint8_t *)cmd,
                       (uint16_t)strlen(cmd));
    }
    gw_app_poll(&app, dt);

    {
        const char *cmd = "uplink on";          /* 上云开关（本来就是开的）*/
        push_esp_frame(GW_FT_COMMAND, (const uint8_t *)cmd,
                       (uint16_t)strlen(cmd));
    }
    gw_app_poll(&app, dt);

    {
        /* 危险指令但令牌错误 -> NACK（顺带验证"令牌错时什么都没做"）*/
        const char *cmd = "restore 0x0000";
        push_esp_frame(GW_FT_COMMAND, (const uint8_t *)cmd,
                       (uint16_t)strlen(cmd));
    }
    gw_app_poll(&app, dt);

    {
        const char *cmd = "save 0x5A5A";        /* 令牌正确的保存 */
        push_esp_frame(GW_FT_COMMAND, (const uint8_t *)cmd,
                       (uint16_t)strlen(cmd));
    }
    gw_app_poll(&app, dt);

    {
        const char *cmd = "FLY_TO_MOON";        /* 未知指令 -> NACK */
        push_esp_frame(GW_FT_COMMAND, (const uint8_t *)cmd,
                       (uint16_t)strlen(cmd));
    }
    gw_app_poll(&app, dt);

    {
        const char *cmd = "REPORT_NOW";         /* 别名，应等价于 report */
        push_esp_frame(GW_FT_COMMAND, (const uint8_t *)cmd,
                       (uint16_t)strlen(cmd));
    }
    gw_app_poll(&app, dt);

    /* ================================================================
     * 阶段 6：主站轮询
     *
     * ⚠️ 主站需要"从站应答"才能完成一次事务。这里的应答由**外部**
     *    喂进来（模拟总线上真实的从站），而不是由本程序生成 ——
     *    所以我们能验证"主站发出的请求帧"是否符合规范。
     * ================================================================ */
    note("phase6: master polling request frame");
    RS.master_busy = 1;

    {
        /* 加一台从机（地址 0x11）并开主站轮询 */
        mb_master_t *m = gw_app_master(&app);

        if (m != 0) {
            mb_m_poll_item_t it;
            memset(&it, 0, sizeof(it));
            it.slave = 0x11u;
            it.func  = MB_FC_READ_HOLDING;
            it.start = 0x006Bu;
            it.qty   = 3u;
            (void)mb_master_add_item(m, &it);
            printf("NOTE master item added (slave 0x11 fc03 addr 0x006B qty 3)\n");
        }
    }
    gw_app_poll(&app, dt);
    gw_app_poll(&app, dt);

    note("scenario end");

    /* 统计（供人参考，验证器不看这两行）*/
    printf("NOTE dht_calls=%d\n", RS.dht_calls);

    return 0;
}
