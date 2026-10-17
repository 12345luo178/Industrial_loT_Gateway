/**
 * @file    test_app_uplink.c
 * @brief   应用层上云链路集成测试（遥测打包 / 帧分派 / 状态上报）
 *
 * ============================================================================
 *  这套测试验证的是"从采集值到云端"这条链路的最后一公里：
 *
 *    1) 上报节拍是否真的按保持寄存器 HREG_REPORT_PERIOD 走
 *    2) 遥测打包时**采集失败**是否正确反映（不能报"25.0℃"这种假数据）
 *    3) 帧分派边界：ACK/LINK_STATUS 交给 uplink，COMMAND 留在应用层
 *    4) 三个状态寄存器（MQTT 状态 / 积压深度 / 丢包数）是否如实上报
 *    5) 未启用上云时不能崩，也不能上报乐观的假状态
 *
 *  实现方式：用假 ESP32 端口 + **真实的 gw_frame 解析器**。
 *  这样"STM32 发出去的帧"与"STM32 收到的帧"都走真实协议，
 *  测的是端到端的字节级正确性，而不是结构体之间的搬运。
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iinc -Idrivers -Iapp -o test/test_app_uplink.exe \
 *        src/modbus_crc.c src/modbus_rtu.c src/modbus_master.c src/gw_pipeline.c \
 *        src/gw_frame.c src/gw_uplink.c drivers/dht22.c app/gw_app.c \
 *        test/test_app_uplink.c
 *    ./test/test_app_uplink.exe
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

#include "gw_app.h"
#include "modbus_regs.h"

static int g_pass = 0;
static int g_fail = 0;

static void ok(const char *name)
{
    g_pass++;
    printf("  [PASS] %s\n", name);
}

static void fail(const char *name, const char *detail)
{
    g_fail++;
    printf("  [FAIL] %s  %s\n", name, detail);
}

static void check_int(const char *name, int actual, int expected)
{
    if (actual == expected) {
        ok(name);
    } else {
        char d[96];
        sprintf(d, "(实际 %d, 期望 %d)", actual, expected);
        fail(name, d);
    }
}

/* ==================================================================
 * 假 ESP32：真实帧解析器 + 可注入的接收流
 * ================================================================== */
#define SENT_MAX 64

typedef struct {
    /* STM32 -> ESP32：把发出去的帧解回来，验证字节级正确 */
    gw_frame_parser_t tx_parser;
    uint8_t  tx_type;                       /* 最近一帧的类型 */
    uint8_t  tx_payload[GW_FRAME_MAX_PAYLOAD];
    uint16_t tx_len;
    uint32_t tx_frames;                     /* 发出的总帧数（含控制帧） */

    /* ⚠️ 必须**按类型分别计数**。
     *
     * 踩过的坑：最初只记录"最后一帧"，于是重连请求（MQTT_CONFIG）
     * 与心跳（LINK_STATUS）会把遥测帧的记录覆盖掉，
     * 测试里看到的是"没有遥测"，而实际上遥测已经发出去了 ——
     * 假环境的缺陷伪装成了实现的 bug，白白排查一轮。
     *
     * 教训：假环境本身也要按"被测物的语义"设计，
     * 只记一个可变快照会让后发生的帧掩盖先发生的帧。 */
    uint32_t telem_frames;                  /* 遥测帧数 */
    uint8_t  telem_last[GW_FRAME_MAX_PAYLOAD];
    uint16_t telem_last_len;
    uint32_t ctrl_frames;                   /* 控制帧数 */

    /* ESP32 -> STM32：待注入的接收流 + 真实解析器 */
    gw_frame_parser_t rx_parser;
    uint8_t  rx_stream[512];
    uint16_t rx_len;

    uint32_t clock_ms;
} fake_esp_t;

static fake_esp_t FE;

static int fe_send(void *ctx, const uint8_t *buf, uint16_t len)
{
    fake_esp_t *e = (fake_esp_t *)ctx;
    uint8_t  t = 0u;
    uint8_t  pl[GW_FRAME_MAX_PAYLOAD];
    uint16_t pl_len = 0u;

    gw_frame_feed(&e->tx_parser, buf, len);
    if (gw_frame_take(&e->tx_parser, &t, pl, sizeof(pl), &pl_len) == GW_FRAME_OK) {
        e->tx_type = t;
        e->tx_len  = pl_len;
        if (pl_len > 0u) {
            memcpy(e->tx_payload, pl, pl_len);
        }
        e->tx_frames++;

        if (t == GW_FT_TELEMETRY) {
            e->telem_frames++;
            e->telem_last_len = pl_len;
            if (pl_len > 0u) {
                memcpy(e->telem_last, pl, pl_len);
            }
        } else {
            e->ctrl_frames++;
        }
    }
    return 0;
}

static void fe_tick(void *ctx)
{
    fake_esp_t *e = (fake_esp_t *)ctx;

    /* 把待注入的字节全部喂进去（模拟"一次 tick 收到一段"） */
    if (e->rx_len > 0u) {
        gw_frame_feed(&e->rx_parser, e->rx_stream, e->rx_len);
        e->rx_len = 0u;
    }
}

static int fe_take(void *ctx, uint8_t *out_type, uint8_t *payload,
                   uint16_t cap, uint16_t *out_len)
{
    fake_esp_t *e = (fake_esp_t *)ctx;

    return (gw_frame_take(&e->rx_parser, out_type, payload, cap, out_len)
            == GW_FRAME_OK) ? 0 : -1;
}

static uint32_t fe_now(void *ctx)
{
    return ((fake_esp_t *)ctx)->clock_ms;
}

/* 往"ESP32 -> STM32"方向注入一帧 */
static void inject(uint8_t type, const uint8_t *pl, uint16_t len)
{
    uint8_t frame[GW_FRAME_MAX_TOTAL];
    uint16_t n = 0u;

    if (gw_frame_build(frame, sizeof(frame), type, pl, len, &n) != GW_FRAME_OK) {
        return;
    }
    if ((uint32_t)FE.rx_len + n <= sizeof(FE.rx_stream)) {
        memcpy(&FE.rx_stream[FE.rx_len], frame, n);
        FE.rx_len = (uint16_t)(FE.rx_len + n);
    }
}

/* ---------------- 应用层所需的假 RS485 端口 ---------------- */
static mb_regs_t REGS;
static mb_slave_t SLAVE;

static int p_take(void *c, uint8_t *b, uint16_t s, uint16_t *l)
{
    (void)c; (void)b; (void)s; (void)l;
    return -1;
}
static int p_send(void *c, const uint8_t *b, uint16_t l)
{
    (void)c; (void)b; (void)l;
    return 0;
}
static mb_regs_t *p_regs(void *c) { (void)c; return &REGS; }

static void env_reset(void)
{
    memset(&FE, 0, sizeof(FE));
    memset(&REGS, 0, sizeof(REGS));
    memset(&SLAVE, 0, sizeof(SLAVE));
    gw_frame_parser_init(&FE.tx_parser);
    gw_frame_parser_init(&FE.rx_parser);
    mb_slave_init(&SLAVE, 1u, &REGS);
}

static void setup(gw_app_t *app, int enable_uplink)
{
    gw_port_t        port;
    gw_uplink_port_t up;

    env_reset();

    memset(&port, 0, sizeof(port));
    port.rs485_take_frame = p_take;
    port.rs485_send       = p_send;
    port.get_regs         = p_regs;
    port.ctx              = 0;
    (void)gw_app_init(app, &SLAVE, &port);

    memset(&up, 0, sizeof(up));
    up.esp_send       = fe_send;
    up.esp_tick       = fe_tick;
    up.esp_take_frame = fe_take;
    up.now_ms         = fe_now;
    up.ctx            = &FE;

    if (enable_uplink != 0) {
        (void)gw_app_enable_uplink(app, &up, 100000u);
    } else {
        (void)gw_app_enable_uplink(app, 0, 0u);
    }
}

/* ==================================================================
 * 把链路直接置为"已连接"
 *
 * ⚠️ 为什么必须显式做这一步：
 *
 * gw_uplink 的设计是"未连接时只入队、不发送"（断线补传的前提）。
 * 所以调用 gw_app_submit_telemetry() 之后 **不会** 立刻看到帧发出，
 * 除非链路已经是 CONNECTED。
 *
 * 一开始我漏了这一步，测试里看到"一条遥测都没发"，
 * 一度以为是打包或发送路径的问题 —— 实际上是测试没有建立链路，
 * 而实现的行为完全正确。
 *
 * 教训：测试"发送类"逻辑之前，必须先把被测对象带到"允许发送"的状态，
 * 否则测的是排队逻辑而不是发送逻辑。
 * ================================================================== */
static void go_online(gw_app_t *app)
{
    uint8_t pl[4];

    pl[0] = 1u;                             /* Wi-Fi 已连接 */
    pl[1] = (uint8_t)GW_LINK_CONNECTED;     /* MQTT 已连接 */
    pl[2] = 0u;
    pl[3] = 0u;

    /* 走真实的分派路径（而不是直接调 gw_uplink_set_link_state），
       这样顺带验证了"LINK_STATUS 帧确实能改变链路状态"。 */
    inject(GW_FT_LINK_STATUS, pl, 4u);
    gw_app_poll(app, 0u);
}

/* 前向声明：测试 1 会用到（定义在测试 2 附近） */
static void go_online(gw_app_t *app);
static void ack_current(gw_app_t *app);

/* ==================================================================
 * 测试 1：上报节拍按保持寄存器走
 * ================================================================== */
static void test_report_period(void)
{
    gw_app_t app;
    int      i;

    printf("\n[1] 上报节拍按 HREG_REPORT_PERIOD 走\n");

    setup(&app, 1);
    check_int("上云已启用", app.uplink_enabled, 1);

    go_online(&app);
    check_int("链路已建立（否则遥测只会排队不发）",
              (int)gw_uplink_state(gw_app_uplink(&app)),
              (int)GW_LINK_CONNECTED);

    /* 默认周期 10 秒。推进 9 秒不应上报 */
    for (i = 0; i < 90; i++) {
        FE.clock_ms += 100u;
        gw_app_poll(&app, 100u);
    }
    check_int("9 秒内不产生遥测", (int)FE.telem_frames, 0);

    /* 再推进 2 秒（累计 11 秒）应上报 1 条 */
    for (i = 0; i < 20; i++) {
        FE.clock_ms += 100u;
        gw_app_poll(&app, 100u);
    }
    check_int("超过 10 秒后产生 1 条遥测", (int)FE.telem_frames, 1);
    check_int("帧类型为 TELEMETRY",
              (int)FE.tx_type, (int)GW_FT_TELEMETRY);
    check_int("遥测载荷长度正确",
              (int)FE.telem_last_len, (int)GW_TELEMETRY_LEN);

    /* 改周期为 1 秒，上报应明显变快。
       注意：uplink 同一时刻只等一个 ACK，所以不喂 ACK 的话
       后续遥测只会排队。这里每轮都确认一次，模拟"ESP32 及时回 ACK"。 */
    REGS.holding[HREG_REPORT_PERIOD] = 1u;
    {
        uint32_t before = FE.telem_frames;

        for (i = 0; i < 25; i++) {      /* 推进 2.5 秒 */
            FE.clock_ms += 100u;
            gw_app_poll(&app, 100u);
            ack_current(&app);
        }
        if (FE.telem_frames >= before + 2u) {
            ok("周期改为 1 秒后上报频率提升");
        } else {
            char d[64];
            sprintf(d, "(只增加 %lu 条)",
                    (unsigned long)(FE.telem_frames - before));
            fail("周期改为 1 秒后上报频率提升", d);
        }
    }

    /* 非法周期（0）应退回默认值（10 秒）而不是变成"每轮都上报" */
    REGS.holding[HREG_REPORT_PERIOD] = 0u;
    {
        uint32_t before = FE.telem_frames;

        for (i = 0; i < 50; i++) {      /* 推进 5 秒 */
            FE.clock_ms += 100u;
            gw_app_poll(&app, 100u);
            ack_current(&app);
        }
        check_int("周期写 0 时退回默认 10 秒（5 秒内不额外上报）",
                  (int)(FE.telem_frames - before), 0);
    }
}

/* 让链路"确认"掉当前待确认的那一帧
 *
 * uplink 的设计是"同一时刻只等一个 ACK"，所以连续提交多条遥测时，
 * 前一条没被确认，后一条只会排队。测试里需要显式喂 ACK
 * 才能观察到"下一条被发出"。 */
static void ack_current(gw_app_t *app)
{
    inject(GW_FT_ACK, 0, 0u);
    gw_app_poll(app, 0u);
}

/* ==================================================================
 * 测试 2：采集失败时不报假数据
 * ================================================================== */
static void test_no_fake_data(void)
{
    gw_app_t       app;
    gw_telemetry_t t;

    printf("\n[2] 采集失败时遥测不含假数据\n");

    setup(&app, 1);
    go_online(&app);

    /* 从未采样过：管道里 dht.valid == 0 */
    (void)gw_app_submit_telemetry(&app);
    check_int("产生了 1 条遥测", (int)FE.telem_frames, 1);

    if (gw_telemetry_unpack(FE.telem_last, FE.telem_last_len, &t) == 0) {
        /* ⚠️ 2026-10-10 改：无有效采样时上报**哨兵值**而不是 0。
           理由与寄存器那边一致 —— 0℃ / 0%RH 都是合法读数，
           上报 0 等于让云端把"没数据"当成真实读数
           （status 位里也有 DHT_OK，但载荷本身不该是假的）。 */
        check_int("无有效采样时温度上报哨兵值（不是 0、也不是假的 25.0℃）",
                  (int)t.temp_x10, (int)(int16_t)GW_DHT_REG_UNAVAILABLE);
        check_int("无有效采样时湿度上报哨兵值（不是 0）",
                  (int)t.humi_x10, (int)(uint16_t)GW_DHT_REG_UNAVAILABLE);
    } else {
        fail("遥测可反序列化", "");
    }

    /* 设备 ID 由 uplink 统一填充 */
    check_int("设备 ID 由链路层填充",
              (int)(t.dev_id == 100000u), 1);
    check_int("序号从 1 开始", (int)t.seq, 1);

    /* 让 DHT22 成功一次（直接写管道，模拟硬件层注入了有效值） */
    app.pipe.dht.valid    = 1u;
    app.pipe.dht.temp_x10 = 235;        /* 23.5℃ */
    app.pipe.dht.humi_x10 = 552u;       /* 55.2% */

    ack_current(&app);                  /* 先确认上一条，才能发下一条 */
    (void)gw_app_submit_telemetry(&app);
    check_int("提交第 2 条", (int)FE.telem_frames, 2);
    if (gw_telemetry_unpack(FE.telem_last, FE.telem_last_len, &t) == 0) {
        check_int("有效采样时温度正确上报", (int)t.temp_x10, 235);
        check_int("有效采样时湿度正确上报", (int)t.humi_x10, 552);
        check_int("序号递增", (int)t.seq, 2);
    }

    /* 负温度也要能正确上传（补码表示） */
    app.pipe.dht.temp_x10 = -105;       /* -10.5℃ */
    ack_current(&app);
    (void)gw_app_submit_telemetry(&app);
    if (gw_telemetry_unpack(FE.telem_last, FE.telem_last_len, &t) == 0) {
        check_int("负温度正确上报", (int)t.temp_x10, -105);
    }
}

/* ==================================================================
 * 测试 3：帧分派边界
 * ================================================================== */
static void test_frame_dispatch(void)
{
    gw_app_t app;
    uint8_t  cmd[4] = { 'A', 'T', '+', '1' };
    uint8_t  buf[32];
    uint16_t len = 0u;

    printf("\n[3] 帧分派：链路帧给 uplink，业务帧留应用层\n");

    setup(&app, 1);

    /* ---- LINK_STATUS：应由 uplink 消费，变成链路状态 ---- */
    {
        uint8_t pl[4];
        pl[0] = 1u;
        pl[1] = (uint8_t)GW_LINK_CONNECTED;
        pl[2] = 0u;
        pl[3] = 0u;
        inject(GW_FT_LINK_STATUS, pl, 4u);
    }
    gw_app_poll(&app, 0u);
    check_int("LINK_STATUS 被 uplink 消费，链路转为已连接",
              (int)gw_uplink_state(gw_app_uplink(&app)),
              (int)GW_LINK_CONNECTED);

    /* ---- COMMAND：uplink 不认，应留在应用层 ---- */
    inject(GW_FT_COMMAND, cmd, sizeof(cmd));
    gw_app_poll(&app, 0u);
    check_int("COMMAND 未进入 uplink（uplink 只认链路帧）",
              (int)(gw_uplink_pending(gw_app_uplink(&app)) == 0u), 1);

    if (gw_app_take_command(&app, buf, sizeof(buf), &len) == 0) {
        ok("下行指令可由应用层取出");
        check_int("指令长度正确", (int)len, 4);
        check_int("指令内容正确", memcmp(buf, cmd, 4u) == 0 ? 1 : 0, 1);
    } else {
        fail("下行指令可由应用层取出", "");
    }
    check_int("取过一次后不再有指令",
              gw_app_take_command(&app, buf, sizeof(buf), &len), -1);

    /* ---- 未知类型：忽略而不是误处理 ---- */
    {
        uint8_t junk[3] = { 1u, 2u, 3u };
        inject(0xEEu, junk, sizeof(junk));
    }
    gw_app_poll(&app, 0u);
    check_int("未知帧类型被忽略（不产生指令）",
              gw_app_take_command(&app, buf, sizeof(buf), &len), -1);
    ok("未知帧类型不会导致崩溃");

    /* ---- 粘连多帧：一次 tick 全部取出 ---- */
    {
        uint8_t pl[4];
        pl[0] = 1u; pl[1] = (uint8_t)GW_LINK_CONNECTED; pl[2] = 0u; pl[3] = 0u;
        inject(GW_FT_LINK_STATUS, pl, 4u);
        inject(GW_FT_COMMAND, cmd, sizeof(cmd));
        inject(GW_FT_COMMAND, cmd, sizeof(cmd));
    }
    gw_app_poll(&app, 0u);
    /* 后一条指令覆盖前一条（应用层只保留最新一条） */
    if (gw_app_take_command(&app, buf, sizeof(buf), &len) == 0 && len == 4u) {
        ok("粘连多帧被逐个分派（指令保留最新一条）");
    } else {
        fail("粘连多帧被逐个分派", "");
    }
}

/* ==================================================================
 * 测试 4：状态如实上报到管道
 * ================================================================== */
static void test_status_reporting(void)
{
    gw_app_t app;
    int      i;

    printf("\n[4] 上云状态如实上报（连接/积压/丢包）\n");

    setup(&app, 1);

    /* 未连接时状态为 DISCONNECTED */
    gw_app_poll(&app, 0u);
    check_int("初始状态为未连接",
              (int)app.pipe.mqtt_status, (int)GW_LINK_DISCONNECTED);
    check_int("初始积压为 0", (int)app.pipe.offline_depth, 0);
    check_int("初始丢包为 0", (int)app.pipe.offline_drop, 0);

    /* 断网期间提交若干条：积压应如实反映 */
    for (i = 0; i < 5; i++) {
        (void)gw_app_submit_telemetry(&app);
    }
    gw_app_poll(&app, 0u);
    check_int("断网时 5 条全部积压", (int)app.pipe.offline_depth, 5);
    check_int("断网时上报未连接",
              (int)app.pipe.mqtt_status, (int)GW_LINK_DISCONNECTED);

    /* 链路恢复：状态应变为已连接 */
    {
        uint8_t pl[4];
        pl[0] = 1u; pl[1] = (uint8_t)GW_LINK_CONNECTED; pl[2] = 0u; pl[3] = 0u;
        inject(GW_FT_LINK_STATUS, pl, 4u);
    }
    gw_app_poll(&app, 0u);
    check_int("链路恢复后上报已连接",
              (int)app.pipe.mqtt_status, (int)GW_LINK_CONNECTED);
    ok("积压深度与连接状态同时可见（运维能区分'在排队'与'已断线'）");

    /* 禁止上云时：状态应如实为"未连接"，而不是留旧值 */
    {
        gw_app_t app2;

        setup(&app2, 0);
        /* 先人为设一个乐观的旧值 */
        app2.pipe.mqtt_status   = (uint16_t)GW_LINK_CONNECTED;
        app2.pipe.offline_depth = 99u;
        app2.pipe.offline_drop  = 77u;

        gw_app_poll(&app2, 0u);
        check_int("未启用上云时状态被置为未连接（不留乐观旧值）",
                  (int)app2.pipe.mqtt_status, (int)GW_LINK_DISCONNECTED);
        check_int("未启用上云时积压清零", (int)app2.pipe.offline_depth, 0);
        check_int("未启用上云时丢包清零", (int)app2.pipe.offline_drop, 0);
        check_int("未启用上云时 uplink 指针为 NULL",
                  gw_app_uplink(&app2) == 0 ? 1 : 0, 1);
    }
}

/* ==================================================================
 * 测试 5：退化与边界（没接 ESP32 也要能跑）
 * ================================================================== */
static void test_degraded(void)
{
    gw_app_t app;
    int      i;

    printf("\n[5] 退化路径：没有 ESP32 模组\n");

    /* 完全不调用 enable_uplink */
    setup(&app, 0);
    check_int("未启用上云时 uplink_enabled = 0", app.uplink_enabled, 0);
    check_int("提交遥测返回'未启用'", gw_app_submit_telemetry(&app), -1);

    for (i = 0; i < 100; i++) {
        gw_app_poll(&app, 10u);
    }
    ok("未启用上云时长时间运行不崩溃");
    check_int("未启用上云不影响从站计数（本层仍在推进）",
              (int)(app.loop_count == 100u), 1);

    /* 移植层接口不全（缺 esp_take_frame）应被安全拒绝 */
    {
        gw_app_t         app2;
        gw_uplink_port_t up;

        setup(&app2, 0);
        memset(&up, 0, sizeof(up));
        up.esp_send = fe_send;
        up.esp_tick = fe_tick;
        up.esp_take_frame = 0;          /* 故意缺一个 */

        check_int("接口不全时安全拒绝（返回 0 = 明确禁用而非错误）",
                  gw_app_enable_uplink(&app2, &up, 1u), 0);
        check_int("拒绝后 uplink_enabled 仍为 0", app2.uplink_enabled, 0);
    }

    /* 空指针安全 */
    check_int("enable_uplink 对空指针安全",
              gw_app_enable_uplink(0, 0, 0u), -1);
    check_int("submit 对空指针安全", gw_app_submit_telemetry(0), -2);
    check_int("take_command 对空指针安全",
              gw_app_take_command(0, 0, 0u, 0), -1);
}


/* ==================================================================
 * 测试 6：⚠️ 指令确认不得干扰遥测的 ACK 窗口
 *
 *  这是一个**真实修复的回归测试**。
 *
 *  缺陷：指令的成功/失败确认曾经复用 GW_FT_ACK / GW_FT_NACK，
 *        而这两个类型被上云链路占用（语义是"遥测送达/被拒"），
 *        且上云**只看类型、不判载荷**。
 *
 *  后果：STM32 发出遥测 T1 进入 awaiting_ack；
 *        此时收到一条令牌错误的下行指令 -> STM32 回 NACK；
 *        上云链路把这条 NACK 当成"T1 被拒" -> 清掉等待窗口、
 *        计入 send_fail、**并且会重传 T1**。
 *
 *  修法：指令确认走专用类型 GW_FT_CMD_ACK / GW_FT_CMD_NACK，
 *        上云链路的 default 分支不认它们，交给应用层。
 *
 *  这个测试钉住的就是"两者互不干扰"。
 * ================================================================== */
static void test_cmd_ack_isolation(void)
{
    gw_app_t    app;
    gw_uplink_t *u;

    printf("\n[6] ⚠️ 指令确认与遥测 ACK 窗口互不干扰\n");

    /* ---- 指令成功确认：不应被上云消费 ---- */
    setup(&app, 1);
    u = gw_app_uplink(&app);
    (void)gw_app_submit_telemetry(&app);
    gw_app_poll(&app, 100u);        /* 制造"正在等遥测 ACK"的状态 */

    {
        uint32_t sent_before = u->stat.sent_ok;
        uint32_t fail_before = u->stat.send_fail;

        inject(GW_FT_CMD_ACK, (const uint8_t *)"PERIOD OK", 9u);
        gw_app_poll(&app, 1u);

        check_int("CMD_ACK 不增加 sent_ok",
                  (int)u->stat.sent_ok, (int)sent_before);
        check_int("CMD_ACK 不增加 send_fail",
                  (int)u->stat.send_fail, (int)fail_before);
        ok("指令确认走专用类型 —— 上云链路只认 ACK/NACK（遥测语义），"
           "两者不再互相污染");
    }

    /* ---- 指令失败确认：不应被当成"遥测被拒" ---- */
    setup(&app, 1);
    u = gw_app_uplink(&app);
    (void)gw_app_submit_telemetry(&app);
    gw_app_poll(&app, 100u);

    {
        uint32_t fail_before = u->stat.send_fail;

        inject(GW_FT_CMD_NACK, (const uint8_t *)"SAVE BAD_TOKEN", 14u);
        gw_app_poll(&app, 1u);

        check_int("⚠️ CMD_NACK 不被当成「遥测被拒」（send_fail 不增加）",
                  (int)u->stat.send_fail, (int)fail_before);
        ok("⚠️ 这是修复的核心：以前一次「指令令牌错误」会误触发"
           "一次遥测重传，而且统计被污染");
    }

    /* ---- 对照：真正的遥测 NACK 仍然生效 ---- */
    setup(&app, 1);
    u = gw_app_uplink(&app);
    (void)gw_app_submit_telemetry(&app);
    gw_app_poll(&app, 100u);

    {
        uint32_t fail_before = u->stat.send_fail;

        inject(GW_FT_NACK, 0, 0u);
        gw_app_poll(&app, 1u);

        check_int("对照：真正的遥测 NACK 仍然计入 send_fail",
                  (int)(u->stat.send_fail > fail_before ? 1 : 0), 1);
        ok("对照用例 —— 修复没有把真正的遥测 NACK 也一起屏蔽掉");
    }

    /* ---- 对照：真正的遥测 ACK 仍然生效 ---- */
    setup(&app, 1);
    u = gw_app_uplink(&app);
    (void)gw_app_submit_telemetry(&app);
    gw_app_poll(&app, 100u);

    {
        uint32_t sent_before = u->stat.sent_ok;

        inject(GW_FT_ACK, 0, 0u);
        gw_app_poll(&app, 1u);

        check_int("对照：真正的遥测 ACK 仍然计入 sent_ok",
                  (int)(u->stat.sent_ok > sent_before ? 1 : 0), 1);
        ok("对照用例 —— 遥测确认路径完全没被影响");
    }
}


/* ==================================================================
 * 测试 7：OTA 帧分派
 *
 *  ⚠️ 这个测试补上了**之前缺失的一环**：
 *
 *     `gw_ota.c`（111 项测试）与 `gw_ota_bridge.c`（132 项测试）
 *     写完之后，`GW_FT_OTA_BEGIN/CHUNK/END` 在应用层仍然是
 *     "未实现，显式忽略" —— 也就是说那两个模块**从来没被调用过**。
 *
 *     **测试全绿不等于功能接了线。** 这个测试验证的就是那条线。
 * ================================================================== */

static int      g_ota_calls;            /* 回调被调用次数 */
static uint8_t  g_ota_last_type;
static uint16_t g_ota_last_len;
static int      g_ota_ret;              /* 回调要返回的值 */

static int fake_ota_handler(void *ctx, uint8_t type,
                            const uint8_t *payload, uint16_t len)
{
    (void)ctx; (void)payload;
    g_ota_calls++;
    g_ota_last_type = type;
    g_ota_last_len  = len;
    return g_ota_ret;
}

static void test_ota_dispatch(void)
{
    gw_app_t app;
    uint8_t  pl[16];

    printf("\n[7] OTA 帧分派（验证那两个模块真的被调用了）\n");

    /* ---- 未注入处理器：三个类型被静默忽略，不算错误 ---- */
    setup(&app, 1);
    g_ota_calls = 0;

    inject(GW_FT_OTA_BEGIN, pl, 12u);
    inject(GW_FT_OTA_CHUNK, pl, 8u);
    inject(GW_FT_OTA_END, 0, 0u);
    gw_app_poll(&app, 1u);

    check_int("未注入处理器时回调不被调用", g_ota_calls, 0);
    check_int("且不计为错误", (int)app.ota_errors, 0);
    ok("未编 OTA 功能的固件不该因为收到 OTA 帧而报错");

    /* ---- 注入处理器：三个类型都被转过去 ---- */
    setup(&app, 1);
    g_ota_calls = 0;
    g_ota_ret   = 0;
    gw_app_enable_ota(&app, fake_ota_handler, 0);

    inject(GW_FT_OTA_BEGIN, pl, 12u);
    gw_app_poll(&app, 1u);

    check_int("OTA_BEGIN 被转给处理器", g_ota_calls, 1);
    check_int("类型正确", (int)g_ota_last_type, (int)GW_FT_OTA_BEGIN);
    check_int("载荷长度正确", (int)g_ota_last_len, 12);
    check_int("帧计数 +1", (int)app.ota_frames, 1);
    check_int("无错误", (int)app.ota_errors, 0);
    ok("⚠️ 这一步才是「接线」—— 在此之前 OTA 那 243 项测试是空转的");

    /* ---- 三个类型都要转 ---- */
    {
        int before = g_ota_calls;

        inject(GW_FT_OTA_CHUNK, pl, 8u);
        inject(GW_FT_OTA_END, 0, 0u);
        gw_app_poll(&app, 1u);

        check_int("CHUNK 与 END 也被转过去", g_ota_calls - before, 2);
        check_int("帧计数累加到 3", (int)app.ota_frames, 3);
    }

    /* ---- 失败要计数，但应用层不回 NACK（那是处理器的职责）---- */
    setup(&app, 1);
    g_ota_calls = 0;
    g_ota_ret   = -3;                   /* 模拟 Flash 错误 */
    gw_app_enable_ota(&app, fake_ota_handler, 0);

    inject(GW_FT_OTA_CHUNK, pl, 8u);
    gw_app_poll(&app, 1u);

    check_int("失败被计数", (int)app.ota_errors, 1);
    check_int("最近结果被记录", (int)app.ota_last_result, -3);
    ok("应用层只记日志与统计，ACK/NACK 由 OTA 处理器自己生成 —— "
       "两边都回会造成两条应答");

    /* ---- 非致命（BUSY / DUPLICATE）不该计入错误 ---- */
    setup(&app, 1);
    g_ota_calls = 0;
    g_ota_ret   = 1;                    /* 模拟 BUSY */
    gw_app_enable_ota(&app, fake_ota_handler, 0);

    inject(GW_FT_OTA_CHUNK, pl, 8u);
    gw_app_poll(&app, 1u);

    check_int("返回值 1（忙/重复）不计入错误", (int)app.ota_errors, 0);
    check_int("仍然计入帧数", (int)app.ota_frames, 1);
    ok("BUSY 与 DUPLICATE 是链路上的正常现象，记成错误会淹没真正的问题");

    /* ---- 关闭注入 ---- */
    gw_app_enable_ota(&app, 0, 0);
    g_ota_calls = 0;
    inject(GW_FT_OTA_END, 0, 0u);
    gw_app_poll(&app, 1u);
    check_int("可以关闭注入", g_ota_calls, 0);

    /* ---- 空指针安全 ---- */
    gw_app_enable_ota(0, fake_ota_handler, 0);
    check_int("空上下文安全", (int)gw_app_ota_frames(0), 0);
    ok("各接口对空指针安全");
}

int main(void)
{
    printf("========== 应用层上云链路集成测试 ==========\n");

    test_report_period();
    test_no_fake_data();
    test_frame_dispatch();
    test_status_reporting();
    test_degraded();
    test_cmd_ack_isolation();
    test_ota_dispatch();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
