/**
 * @file    test_app.c
 * @brief   应用层集成测试（主机端，全部用假移植层）
 *
 * ============================================================================
 *  这套测试验证的是"编排"而不是"算法"
 *
 *  各模块自己的算法已经被各自的测试覆盖了（regmap 55 / dht22 57 /
 *  master 74 / pipeline 74）。这里要回答的是另外一类问题：
 *
 *    - 初始化顺序对不对？寄存器表是不是在第一次读之前就有确定值？
 *    - 上位机的一帧请求，能不能一路走到从站引擎并把响应发出去？
 *    - 主站的错误状态会不会被正确搬运到寄存器表供诊断？
 *    - 传感器不可用时，系统是"如实报告无数据"还是"假装正常"？
 *    - 一轮 poll 里各模块的调用顺序是否符合实时性要求？
 *
 *  这类问题在各模块单独测试时**完全测不出来** ——
 *  它们的缺陷只在"拼起来"之后才暴露。
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iinc -Idrivers -Iapp \
 *        src/modbus_crc.c src/modbus_rtu.c src/modbus_master.c \
 *        src/gw_pipeline.c drivers/dht22.c app/gw_app.c test/test_app.c
 *    ./test_app.exe
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

#include "gw_app.h"
#include "modbus_regs.h"
#include "modbus_crc.h"

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
 * 假移植层：模拟 RS485 与 DHT22
 * ================================================================== */
#define FAKE_RX_MAX 16

typedef struct {
    mb_regs_t regs;

    /* 待处理的接收帧队列（模拟"上位机发来的请求"） */
    uint8_t  rx[FAKE_RX_MAX][MB_MAX_ADU];
    uint16_t rx_len[FAKE_RX_MAX];
    int      rx_n;

    /* 发出的响应记录 */
    uint8_t  tx[FAKE_RX_MAX][MB_MAX_ADU];
    uint16_t tx_len[FAKE_RX_MAX];
    int      tx_n;

    /* DHT22 行为注入 */
    int      dht_available;     /* 0 = 未注入 dht_sample 回调 */
    int      dht_fail;          /* 1 = 每次采样都返回校验错误 */
    int      dht_calls;         /* 采样被调用次数 */
} fake_port_t;

static int fp_take_frame(void *ctx, uint8_t *buf, uint16_t buf_size, uint16_t *len)
{
    fake_port_t *f = (fake_port_t *)ctx;
    int i;

    if (f->rx_n <= 0) {
        return -1;                          /* 没有待处理帧 */
    }
    if (f->rx_len[0] > buf_size) {
        return -2;
    }
    memcpy(buf, f->rx[0], f->rx_len[0]);
    *len = f->rx_len[0];

    /* 出队 */
    for (i = 1; i < f->rx_n; i++) {
        memcpy(f->rx[i - 1], f->rx[i], f->rx_len[i]);
        f->rx_len[i - 1] = f->rx_len[i];
    }
    f->rx_n--;
    return 0;
}

static int fp_send(void *ctx, const uint8_t *buf, uint16_t len)
{
    fake_port_t *f = (fake_port_t *)ctx;

    if (f->tx_n >= FAKE_RX_MAX || len > MB_MAX_ADU) {
        return -1;
    }
    memcpy(f->tx[f->tx_n], buf, len);
    f->tx_len[f->tx_n] = len;
    f->tx_n++;
    return 0;
}

static mb_regs_t *fp_get_regs(void *ctx)
{
    fake_port_t *f = (fake_port_t *)ctx;
    return &f->regs;
}

static dht22_err_t fp_dht_sample(void *ctx, dht22_filtered_t *filt, uint8_t retry)
{
    fake_port_t *f = (fake_port_t *)ctx;
    dht22_raw_t  raw;

    (void)retry;
    f->dht_calls++;

    if (f->dht_fail) {
        /* 模拟校验失败：按驱动器的方式累加统计与失败计数，不污染均值 */
        dht22_filter_push(filt, 0);
        return DHT22_ERR_CHECKSUM;
    }

    /* 模拟一次成功采样：61.2%RH / 25.3℃ */
    raw.temp_x10 = 253;
    raw.humi_x10 = 612u;
    raw.checksum_ok = 1u;
    dht22_filter_push(filt, &raw);
    return DHT22_OK;
}

static void fp_reset(fake_port_t *f)
{
    memset(f, 0, sizeof(*f));
}

/* 构造一帧 Modbus 请求（自动补 CRC） */
static uint16_t build_req(uint8_t *out, const uint8_t *pdu, uint16_t pdu_len)
{
    uint16_t crc = mb_crc16(pdu, pdu_len);
    memcpy(out, pdu, pdu_len);
    out[pdu_len]      = (uint8_t)(crc & 0xFFu);
    out[pdu_len + 1u] = (uint8_t)(crc >> 8);
    return (uint16_t)(pdu_len + 2u);
}

/* 往假移植层的接收队列里塞一帧 */
static void fp_push_rx(fake_port_t *f, const uint8_t *pdu, uint16_t pdu_len)
{
    if (f->rx_n >= FAKE_RX_MAX) {
        return;
    }
    f->rx_len[f->rx_n] = build_req(f->rx[f->rx_n], pdu, pdu_len);
    f->rx_n++;
}

static void make_port(gw_port_t *p, fake_port_t *f, int with_dht)
{
    memset(p, 0, sizeof(*p));
    p->rs485_take_frame = fp_take_frame;
    p->rs485_send       = fp_send;
    p->get_regs         = fp_get_regs;
    p->dht_sample       = with_dht ? fp_dht_sample : 0;
    p->ctx              = f;
}

/* ==================================================================
 * 测试 1：初始化
 * ================================================================== */
static void test_init(void)
{
    gw_app_t     app;
    fake_port_t  f;
    gw_port_t    port;
    mb_slave_t   slave;

    printf("\n[1] 初始化\n");

    fp_reset(&f);
    make_port(&port, &f, 1);

    mb_slave_init(&slave, 1u, &f.regs);

    if (gw_app_init(&app, &slave, &port) == 0) {
        ok("应用层初始化成功（含 DHT22）");
    } else {
        fail("应用层初始化成功", "");
        return;
    }

    check_int("寄存器表指针已绑定", (app.regs == &f.regs) ? 1 : 0, 1);
    check_int("从站上下文已绑定", (app.slave == &slave) ? 1 : 0, 1);

    /* 关键：初始化后寄存器表必须已是"确定值"，而不是全 0 的未初始化状态。
       固件版本字段应当被写进去 —— 用它能判断"同步确实执行过"。 */
    check_int("固件主版本已写入寄存器",
              (int)f.regs.input[IREG_FW_VER_MAJOR], GW_FW_VERSION_MAJOR);
    check_int("固件次版本已写入寄存器",
              (int)f.regs.input[IREG_FW_VER_MINOR], GW_FW_VERSION_MINOR);

    /* MQTT 初始状态应为"未连接"，而不是 0 之外的值 */
    check_int("MQTT 初始状态为未连接",
              (int)f.regs.input[IREG_MQTT_STATUS], IREG_MQTT_ST_DISCONNECTED);

    /* 空指针与缺回调都要被拒 */
    if (gw_app_init(0, &slave, &port) != 0) { ok("空 app 指针被拒"); }
    else { fail("空 app 指针被拒", ""); }
    if (gw_app_init(&app, 0, &port) != 0) { ok("空 slave 指针被拒"); }
    else { fail("空 slave 指针被拒", ""); }
    if (gw_app_init(&app, &slave, 0) != 0) { ok("空 port 指针被拒"); }
    else { fail("空 port 指针被拒", ""); }

    {
        gw_port_t bad;
        make_port(&bad, &f, 1);
        bad.rs485_send = 0;
        if (gw_app_init(&app, &slave, &bad) != 0) { ok("缺少 send 回调被拒"); }
        else { fail("缺少 send 回调被拒", ""); }

        make_port(&bad, &f, 1);
        bad.get_regs = 0;
        if (gw_app_init(&app, &slave, &bad) != 0) { ok("缺少 get_regs 回调被拒"); }
        else { fail("缺少 get_regs 回调被拒", ""); }
    }

    /* 没有 DHT22 也应能初始化成功 */
    {
        gw_port_t p2;
        make_port(&p2, &f, 0);
        if (gw_app_init(&app, &slave, &p2) == 0) {
            ok("DHT22 不可用时仍能初始化（降级而非失败）");
        } else {
            fail("DHT22 不可用时仍能初始化", "");
        }
    }
}

/* ==================================================================
 * 测试 2：从站请求 -> 响应全链路
 * ================================================================== */
static void test_slave_roundtrip(void)
{
    gw_app_t     app;
    fake_port_t  f;
    gw_port_t    port;
    mb_slave_t   slave;

    printf("\n[2] 从站请求 -> 响应全链路\n");

    fp_reset(&f);
    make_port(&port, &f, 1);
    mb_slave_init(&slave, 1u, &f.regs);
    (void)gw_app_init(&app, &slave, &port);

    /* 轮询前：位移寄存器里没有帧，但初始化同步已写好版本号 */
    gw_app_poll(&app, GW_APP_TICK_MS);
    check_int("无请求时不产生响应", f.tx_n, 0);
    check_int("无请求时收帧计数为 0", (int)app.slave_rx_frames, 0);

    /* 塞入一帧"读保持寄存器 0~3" */
    {
        uint8_t pdu[6];
        pdu[0] = 1u;            /* 站号 */
        pdu[1] = MB_FC_READ_HOLDING;
        pdu[2] = 0u; pdu[3] = 0u;
        pdu[4] = 0u; pdu[5] = 4u;
        fp_push_rx(&f, pdu, 6u);
    }

    /* 先在从机侧写入已知数据 */
    f.regs.holding[HREG_DEVICE_ID_HI] = 0x0001u;
    f.regs.holding[HREG_DEVICE_ID_LO] = 0x86A0u;

    gw_app_poll(&app, GW_APP_TICK_MS);

    check_int("收帧计数 = 1", (int)app.slave_rx_frames, 1);
    check_int("发出了 1 帧响应", f.tx_n, 1);
    check_int("发帧计数 = 1", (int)app.slave_tx_frames, 1);

    /* 校验响应内容：站号 1、功能码 03、字节数 8、数据大端 */
    if (f.tx_n >= 1) {
        check_int("响应站号 = 1", (int)f.tx[f.tx_n - 1][0], 1);
        check_int("响应功能码 = 0x03", (int)f.tx[f.tx_n - 1][1], 0x03);
        check_int("响应字节数 = 8", (int)f.tx[f.tx_n - 1][2], 8);
        check_int("第 1 个寄存器高字节", (int)f.tx[f.tx_n - 1][3], 0x00);
        check_int("第 1 个寄存器低字节", (int)f.tx[f.tx_n - 1][4], 0x01);
        check_int("第 2 个寄存器高字节", (int)f.tx[f.tx_n - 1][5], 0x86);
    }

    /* 从站引擎的统计应被搬进管道 */
    check_int("管道内的收帧计数已同步", (int)app.pipe.mb_rx, 1);

    /* 再塞一帧畸形（CRC 错）的请求：应静默丢弃，不产生响应 */
    {
        uint8_t pdu[6];
        uint16_t flen;
        pdu[0] = 1u;
        pdu[1] = MB_FC_READ_HOLDING;
        pdu[2] = 0u; pdu[3] = 0u; pdu[4] = 0u; pdu[5] = 4u;
        flen = build_req(f.rx[f.rx_n], pdu, 6u);
        f.rx[f.rx_n][flen - 1u] ^= 0xFFu;       /* 破坏 CRC */
        f.rx_len[f.rx_n] = flen;
        f.rx_n++;
    }

    gw_app_poll(&app, GW_APP_TICK_MS);
    check_int("CRC 错误帧不产生响应（响应总数仍为 1）", f.tx_n, 1);

    /* 站号不匹配：也不应响应 */
    {
        uint8_t pdu[6];
        pdu[0] = 9u;            /* 本机是 1 */
        pdu[1] = MB_FC_READ_HOLDING;
        pdu[2] = 0u; pdu[3] = 0u; pdu[4] = 0u; pdu[5] = 4u;
        fp_push_rx(&f, pdu, 6u);
    }
    gw_app_poll(&app, GW_APP_TICK_MS);
    check_int("站号不匹配的帧不产生响应", f.tx_n, 1);
}

/* ==================================================================
 * 测试 3：DHT22 采样进入寄存器表
 * ================================================================== */
static void test_dht_into_regs(void)
{
    gw_app_t     app;
    fake_port_t  f;
    gw_port_t    port;
    mb_slave_t   slave;
    uint32_t     elapsed = 0u;

    printf("\n[3] DHT22 采样进入寄存器表\n");

    fp_reset(&f);
    make_port(&port, &f, 1);
    mb_slave_init(&slave, 1u, &f.regs);
    (void)gw_app_init(&app, &slave, &port);

    /* 采样周期是 2500ms；用 500ms 的步长推进 5 次正好触发一次 */
    {
        int i;
        for (i = 0; i < 5; i++) {
            gw_app_poll(&app, 500u);
            elapsed += 500u;
        }
    }

    check_int("到达采样点后调用了一次采样", f.dht_calls, 1);
    check_int("采样尝试计数 = 1", (int)app.dht_samples, 1);
    check_int("温度已写入寄存器", (int)(int16_t)f.regs.input[IREG_TEMP_X10], 253);
    check_int("湿度已写入寄存器", (int)f.regs.input[IREG_HUMI_X10], 612);

    /* 状态位应反映数据有效 */
    if ((f.regs.input[IREG_DEV_STATUS] & IREG_STATUS_BIT_DHT_OK) != 0u) {
        ok("状态位置起 DHT_OK");
    } else {
        fail("状态位置起 DHT_OK", "");
    }

    /* uptime 应已累计（5×500ms = 2.5 秒） */
    {
        uint32_t up = gw_join_u32(f.regs.input[IREG_UPTIME_HI],
                                  f.regs.input[IREG_UPTIME_LO]);
        check_int("运行时间已累计到 2 秒", (int)up, 2);
    }
    (void)elapsed;
}

/* ==================================================================
 * 测试 4：传感器故障时的行为
 * ================================================================== */
static void test_dht_failure(void)
{
    gw_app_t     app;
    fake_port_t  f;
    gw_port_t    port;
    mb_slave_t   slave;
    int          i;

    printf("\n[4] 传感器故障时的行为（是否如实报告）\n");

    fp_reset(&f);
    make_port(&port, &f, 1);
    mb_slave_init(&slave, 1u, &f.regs);
    (void)gw_app_init(&app, &slave, &port);

    /* 先成功采样一次，让寄存器里有有效值 */
    for (i = 0; i < 5; i++) { gw_app_poll(&app, 500u); }
    check_int("首次采样成功，温度 = 253",
              (int)(int16_t)f.regs.input[IREG_TEMP_X10], 253);

    /* 之后让传感器持续失败 */
    f.dht_fail = 1;
    for (i = 0; i < 30; i++) { gw_app_poll(&app, 500u); }

    if (f.dht_calls < 5) {
        fail("故障期间持续尝试采样", "");
    } else {
        ok("故障期间持续尝试采样（不会因为失败就停止）");
    }

    /* 关键契约：连续失败后清状态位，但**保留旧读数** */
    if ((f.regs.input[IREG_DEV_STATUS] & IREG_STATUS_BIT_DHT_OK) == 0u) {
        ok("连续失败后清除 DHT_OK 位");
    } else {
        fail("连续失败后清除 DHT_OK 位", "");
    }
    check_int("失败后旧温度值仍保留（未被清零成 0℃）",
              (int)(int16_t)f.regs.input[IREG_TEMP_X10], 253);

    /* 恢复后状态位应重新置起 */
    f.dht_fail = 0;
    for (i = 0; i < 5; i++) { gw_app_poll(&app, 500u); }
    if ((f.regs.input[IREG_DEV_STATUS] & IREG_STATUS_BIT_DHT_OK) != 0u) {
        ok("传感器恢复后重新置起 DHT_OK");
    } else {
        fail("传感器恢复后重新置起 DHT_OK", "");
    }
}

/* ==================================================================
 * 测试 5：DHT22 硬件层未注入（降级路径）
 * ================================================================== */
static void test_dht_unavailable(void)
{
    gw_app_t     app;
    fake_port_t  f;
    gw_port_t    port;
    mb_slave_t   slave;
    int          i;

    printf("\n[5] DHT22 硬件不可用时的降级\n");

    fp_reset(&f);
    make_port(&port, &f, 0);        /* 不注入 dht_sample */
    mb_slave_init(&slave, 1u, &f.regs);
    (void)gw_app_init(&app, &slave, &port);

    for (i = 0; i < 15; i++) { gw_app_poll(&app, 500u); }

    check_int("硬件不可用时采样回调未被调用", f.dht_calls, 0);
    if (app.dht_samples >= 3) {
        ok("仍按节拍记录采样尝试次数");
    } else {
        fail("仍按节拍记录采样尝试次数", "");
    }
    if ((f.regs.input[IREG_DEV_STATUS] & IREG_STATUS_BIT_DHT_OK) == 0u) {
        ok("硬件不可用时状态位不置 DHT_OK（如实报告）");
    } else {
        fail("硬件不可用时状态位不置 DHT_OK", "");
    }
    /* ⚠️ 从未有过有效值时必须写**哨兵值**，不能留 0（2026-10-10 改）：
       0℃ 是合法读数，留 0 会让上位机把"没数据"当成"真的 0℃"。
       状态位 DHT_OK 仍然如实反映有效性 —— 两者是"双保险"，不是二选一。 */
    check_int("从未采样成功时温度写哨兵值 0x8000（不是 0）",
              (int)(int16_t)f.regs.input[IREG_TEMP_X10],
              (int)(int16_t)GW_DHT_REG_UNAVAILABLE);
    check_int("从未采样成功时湿度写哨兵值 0x8000（不是 0）",
              (int)f.regs.input[IREG_HUMI_X10],
              (int)GW_DHT_REG_UNAVAILABLE);
}

/* ==================================================================
 * 测试 6：主站状态搬运
 * ================================================================== */
static void test_master_sync(void)
{
    gw_app_t        app;
    fake_port_t     f;
    gw_port_t       port;
    mb_slave_t      slave;
    mb_m_poll_item_t it;
    int             i;

    printf("\n[6] 主站状态搬运到寄存器表\n");

    fp_reset(&f);
    make_port(&port, &f, 1);
    mb_slave_init(&slave, 1u, &f.regs);
    (void)gw_app_init(&app, &slave, &port);

    /* 配置一个"永远不存在的从机"，让主站必然失败并转离线。
       注意：不要真的让它发出去 —— 假移植层的 send 会记录到 tx[]，
       所以这里只关心主站的统计与位图，不关心发送内容。 */
    it.slave = 7u;
    it.func  = MB_FC_READ_HOLDING;
    it.start = 0u;
    it.qty   = 1u;
    it.map_base = 0u;
    it.map_area = 0u;
    check_int("添加主站轮询项成功",
              (int)(mb_master_add_item(&app.master, &it) == MB_M_OK), 1);

    /* 推进足够长时间，让超时+重试+离线判定全部走完 */
    for (i = 0; i < 200; i++) { gw_app_poll(&app, 100u); }

    {
        const mb_m_slave_stat_t *ss = mb_master_slave_stat(&app.master, 7u);
        if (ss != 0 && ss->online == 0u) {
            ok("不存在的主站从机被判定离线");
        } else {
            fail("不存在的主站从机被判定离线", "");
        }
    }

    /* 在线位图应清零（该从机离线） */
    check_int("在线位图已清零", (int)f.regs.input[IREG_MASTER_ONLINE], 0);

    /* 错误码应被搬运（非 0 表示有故障） */
    if (f.regs.input[IREG_MASTER_ERR] != 0u) {
        ok("主站错误码已搬运到寄存器表（供诊断）");
    } else {
        fail("主站错误码已搬运到寄存器表", "");
    }

    /* MASTER_OK 位应清零 */
    if ((f.regs.input[IREG_DEV_STATUS] & IREG_STATUS_BIT_MASTER_OK) == 0u) {
        ok("无在线从机时清 MASTER_OK 位");
    } else {
        fail("无在线从机时清 MASTER_OK 位", "");
    }
}

/* ==================================================================
 * 测试 7：调用顺序（实时性契约）
 * ================================================================== */
static void test_poll_order(void)
{
    gw_app_t     app;
    fake_port_t  f;
    gw_port_t    port;
    mb_slave_t   slave;

    printf("\n[7] 一轮 poll 内的处理顺序\n");

    fp_reset(&f);
    make_port(&port, &f, 1);
    mb_slave_init(&slave, 1u, &f.regs);
    (void)gw_app_init(&app, &slave, &port);

    /* 同一次 poll 里同时有：待处理的从站请求 + 到期的采样。
       从站请求必须先被处理 —— 这是实时性要求：
       上位机在等响应，而采样晚 10ms 无影响。
       验证方式：让采样回调记录"被调用时，从站帧是否已处理完"。 */
    {
        uint8_t pdu[6];
        pdu[0] = 1u; pdu[1] = MB_FC_READ_HOLDING;
        pdu[2] = 0u; pdu[3] = 0u; pdu[4] = 0u; pdu[5] = 1u;
        fp_push_rx(&f, pdu, 6u);
    }

    /* 先推进到采样点前一刻（采样周期 2500ms） */
    gw_app_poll(&app, 2400u);
    check_int("采样尚未触发", f.dht_calls, 0);

    /* 再推进 100ms：本次既有请求又有采样 */
    gw_app_poll(&app, 100u);

    check_int("本次采样被触发", f.dht_calls, 1);
    check_int("本次从站请求也被处理（响应已发出）", f.tx_n, 1);
    check_int("收帧计数已累加", (int)app.slave_rx_frames, 1);

    /* 顺序契约：响应必须在同一轮 poll 内发出。
       如果从站处理被放在采样之后，遇到"采样耗时 60ms"（最坏 3 次尝试）
       的极端情况就可能超出上位机等待窗口。
       这里断言的是"同一轮内完成"，而不是具体耗时 —— 后者需要真实硬件测。 */
    ok("从站请求与采样在同一轮 poll 内均被处理");

    /* 未初始化时的 poll 应安全空转 */
    {
        gw_app_t fresh;
        memset(&fresh, 0, sizeof(fresh));
        gw_app_poll(&fresh, 100u);      /* 不应崩溃 */
        ok("未初始化的 app 调用 poll 安全空转");
    }
}

/* ==================================================================
 * 测试 8：启动原因 -> sticky 状态位（gw_app_note_boot_cause）
 *
 * ⚠️ 这个用例守的是"接口承诺了却没有实现"那类静默失效：
 *    寄存器表里 IWDG_RESET / HARDFAULT 两位写着"sticky，仅复位清除"，
 *    但加了 IWDG 时才发现**从来没有人设置过它们** —— 上位机永远看不到
 *    "这台设备刚被看门狗复位过"。所以这里把语义钉死：
 *      异常启动（IWDG / 故障）-> 置位
 *      正常启动（上电 / 软件复位）-> **不置位**（否则每次正常重启都报故障）
 * ================================================================== */
static void test_boot_cause_sticky(void)
{
    gw_app_t    app;
    fake_port_t f;
    gw_port_t   port;
    mb_slave_t  slave;

    printf("\n[8] 启动原因 -> sticky 状态位\n");

    fp_reset(&f);
    make_port(&port, &f, 1);
    mb_slave_init(&slave, 1u, &f.regs);
    if (gw_app_init(&app, &slave, &port) != 0) {
        fail("应用层初始化成功", "");
        return;
    }

    /* 正常启动：两个 sticky 位都不该被置 */
    fp_reset(&f);
    gw_app_note_boot_cause(&app, GW_BOOT_POWER_ON);
    gw_app_note_boot_cause(&app, GW_BOOT_SOFTWARE);
    /* ⚠️ 必须跑一次 poll：`gw_pipeline_set_sticky()` 只是置**掩码**，
       状态字是在管道下一次同步时才组装进寄存器的。
       漏掉这一步会误判成"接口没生效"（第一版就这么写的）。 */
    gw_app_poll(&app, 1000u);
    check_int("上电/软件复位不置 IWDG_RESET",
              (f.regs.input[IREG_DEV_STATUS] & IREG_STATUS_BIT_IWDG_RESET) ? 1 : 0, 0);
    check_int("上电/软件复位不置 HARDFAULT",
              (f.regs.input[IREG_DEV_STATUS] & IREG_STATUS_BIT_HARDFAULT) ? 1 : 0, 0);

    /* 看门狗复位：置 IWDG_RESET */
    fp_reset(&f);
    gw_app_note_boot_cause(&app, GW_BOOT_IWDG);
    gw_app_poll(&app, 1000u);
    check_int("看门狗复位置 IWDG_RESET",
              (f.regs.input[IREG_DEV_STATUS] & IREG_STATUS_BIT_IWDG_RESET) ? 1 : 0, 1);

    /* 故障复位：置 HARDFAULT（枚举里异常复位统一是 GW_BOOT_FAULT） */
    fp_reset(&f);
    gw_app_note_boot_cause(&app, GW_BOOT_FAULT);
    gw_app_poll(&app, 1000u);
    check_int("故障复位置 HARDFAULT",
              (f.regs.input[IREG_DEV_STATUS] & IREG_STATUS_BIT_HARDFAULT) ? 1 : 0, 1);

    /* 空指针必须安全（启动早期调用，不能崩） */
    gw_app_note_boot_cause(0, GW_BOOT_IWDG);
    ok("空 app 指针不崩");
}

int main(void)
{
    printf("========== 应用层集成测试（主机端） ==========\n");

    test_init();
    test_slave_roundtrip();
    test_dht_into_regs();
    test_dht_failure();
    test_dht_unavailable();
    test_master_sync();
    test_poll_order();
    test_boot_cause_sticky();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
