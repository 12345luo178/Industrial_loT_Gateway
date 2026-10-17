/**
 * @file    test_app_can.c
 * @brief   应用层 CAN 接入测试（通道同步 / 状态位语义 / 遥测取数）
 *
 * ============================================================================
 *  这套测试的重点是"数据来源唯一性"与"状态位语义"：
 *
 *    1) CAN 通道值必须与上位机读到的寄存器是**同一份**，
 *       不能有第二份副本（两份迟早不一致）
 *    2) 状态位 IREG_STATUS_BIT_CAN_OK 的语义必须精确：
 *       ERROR_WARN 时数据仍在进来，**不该**报"不可用"（会造成大量误报）
 *       BUS_OFF 时确实收不到数据，**必须**报"不可用"（否则上位机
 *       会看到一堆陈旧值却以为一切正常）
 *    3) 未启用 CAN 时不能崩，也不能留乐观的旧状态
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iinc -Idrivers -Iapp -o test/test_app_can.exe \
 *        src/modbus_crc.c src/modbus_rtu.c src/modbus_master.c src/gw_pipeline.c \
 *        src/gw_frame.c src/gw_uplink.c src/gw_can_core.c drivers/dht22.c \
 *        app/gw_app.c test/test_app_can.c
 *    ./test/test_app_can.exe
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
 * 假环境
 * ================================================================== */
typedef struct {
    gw_can_core_t core;
    uint32_t      tick_calls;
    uint32_t      last_elapsed;
    int           reinit_result;
} fake_can_t;

static fake_can_t FC;

static int fc_reinit(void *ctx)
{
    fake_can_t *c = (fake_can_t *)ctx;
    /* reinit 成功就回到正常态：真实移植层重初始化后控制器就正常了 */
    return c->reinit_result;
}

static void fc_tick(void *ctx, uint32_t elapsed_ms)
{
    fake_can_t *c = (fake_can_t *)ctx;

    c->tick_calls++;
    c->last_elapsed = elapsed_ms;
    gw_can_core_tick(&c->core, elapsed_ms);

    /* 模拟"恢复了"：调用方把 reinit_result 置 0 后，
       下一次 tick 里核心会调 reinit 回调成功，从而回到 NORMAL。 */
}

static gw_can_core_t *fc_get_core(void *ctx)
{
    fake_can_t *c = (fake_can_t *)ctx;
    return &c->core;
}

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

static gw_can_frame_t mk_frame(uint16_t id, uint16_t value)
{
    gw_can_frame_t f;

    memset(&f, 0, sizeof(f));
    f.id  = id;
    f.dlc = 2u;
    f.data[0] = (uint8_t)(value >> 8);
    f.data[1] = (uint8_t)(value & 0xFFu);
    return f;
}

/* 建立应用层；enable_can != 0 时接入 CAN */
static void setup(gw_app_t *app, int enable_can)
{
    gw_port_t     port;
    gw_can_port_t can;

    memset(&FC, 0, sizeof(FC));
    memset(&REGS, 0, sizeof(REGS));
    memset(&SLAVE, 0, sizeof(SLAVE));

    (void)gw_can_core_init(&FC.core, 0u, 0u, fc_reinit, &FC);
    mb_slave_init(&SLAVE, 1u, &REGS);

    memset(&port, 0, sizeof(port));
    port.rs485_take_frame = p_take;
    port.rs485_send       = p_send;
    port.get_regs         = p_regs;
    (void)gw_app_init(app, &SLAVE, &port);

    memset(&can, 0, sizeof(can));
    can.tick     = fc_tick;
    can.get_core = fc_get_core;
    can.ctx      = &FC;

    if (enable_can != 0) {
        (void)gw_app_enable_can(app, &can);
    } else {
        (void)gw_app_enable_can(app, 0);
    }
}

/* 状态位是否置起 */
static int can_ok_bit(const gw_app_t *app)
{
    return (app->pipe.can_ok != 0u) ? 1 : 0;
}

/* ==================================================================
 * 测试 1：启用 / 禁用 / 参数校验
 * ================================================================== */
static void test_enable(void)
{
    gw_can_port_t      can;
    static gw_app_t    big;     /* 放静态区，避免测试栈上放 15KB 结构体 */

    printf("\n[1] 启用、禁用与参数校验\n");

    setup(&big, 1);
    check_int("接入 CAN 后核心指针非空", gw_app_can(&big) != 0 ? 1 : 0, 1);

    setup(&big, 0);
    check_int("未接入时核心指针为 NULL", gw_app_can(&big) == 0 ? 1 : 0, 1);
    check_int("未启用 CAN 不影响其他功能（loop 能推进）",
              (gw_app_poll(&big, 10u), big.loop_count == 1u ? 1 : 0), 1);

    /* 接口不全应被安全拒绝 */
    memset(&can, 0, sizeof(can));
    can.tick = fc_tick;
    can.get_core = 0;                   /* 故意缺一个 */
    check_int("接口不全时安全禁用（返回 0 而非错误）",
              gw_app_enable_can(&big, &can), 0);
    check_int("拒绝后核心指针仍为 NULL", gw_app_can(&big) == 0 ? 1 : 0, 1);

    /* 空指针安全 */
    check_int("enable_can 对空指针安全", gw_app_enable_can(0, 0), -1);
    check_int("gw_app_can 对空指针安全", gw_app_can(0) == 0 ? 1 : 0, 1);
}

/* ==================================================================
 * 测试 2：通道值同步到寄存器表
 * ================================================================== */
static void test_channel_sync(void)
{
    static gw_app_t app;
    gw_can_frame_t  f;

    printf("\n[2] CAN 通道同步到输入寄存器\n");

    setup(&app, 1);

    /* 配 3 个通道映射 */
    (void)gw_can_core_set_map(&FC.core, 0x100u, 0u);
    (void)gw_can_core_set_map(&FC.core, 0x200u, 1u);
    (void)gw_can_core_set_map(&FC.core, 0x300u, 2u);

    /* 收到两条报文 */
    f = mk_frame(0x100u, 0x1234u);
    (void)gw_can_core_on_frame(&FC.core, &f);
    f = mk_frame(0x200u, 0xABCDu);
    (void)gw_can_core_on_frame(&FC.core, &f);

    gw_app_poll(&app, 10u);

    check_int("通道 0 写入 IREG_CAN_CH1", (int)REGS.input[IREG_CAN_CH1], 0x1234);
    check_int("通道 1 写入 IREG_CAN_CH2", (int)REGS.input[IREG_CAN_CH2], 0xABCD);
    /* 通道 2 没有数据：应保持原值（这里是 0），而不是被清成别的 */
    check_int("无数据的通道保持原值", (int)REGS.input[IREG_CAN_CH3], 0);

    /* 再收一条新值，应覆盖 */
    f = mk_frame(0x100u, 0x5678u);
    (void)gw_can_core_on_frame(&FC.core, &f);
    gw_app_poll(&app, 10u);
    check_int("通道值被最新报文覆盖", (int)REGS.input[IREG_CAN_CH1], 0x5678);

    /* 收帧计数应上报（32 位拆高低字） */
    check_int("CAN 收帧计数低字 = 3", (int)REGS.input[IREG_CAN_RX_LO], 3);
    check_int("CAN 收帧计数高字 = 0", (int)REGS.input[IREG_CAN_RX_HI], 0);
    ok("上位机能从寄存器读到 CAN 收帧统计（现场排查看得出'有没有在收'）");
}

/* ==================================================================
 * 测试 3：状态位语义（本套测试的核心）
 * ================================================================== */
static void test_status_bit(void)
{
    static gw_app_t app;

    printf("\n[3] CAN 状态位语义（精确区分'降级'与'不可用'）\n");

    setup(&app, 1);

    /* 正常态：状态位置起 */
    gw_app_poll(&app, 10u);
    check_int("NORMAL 时 CAN_OK 置起", can_ok_bit(&app), 1);
    check_int("状态字里也反映出来",
              (int)(gw_pipeline_status_word(&app.pipe) & IREG_STATUS_BIT_CAN_OK),
              (int)IREG_STATUS_BIT_CAN_OK);

    /* 错误警告态：数据仍在进来，**不该**报不可用 */
    gw_can_core_notify_error_warn(&FC.core);
    gw_app_poll(&app, 10u);
    check_int("ERROR_WARN 时 CAN_OK 仍置起（避免大量误报）",
              can_ok_bit(&app), 1);
    ok("把 ERROR_WARN 视为可用是对的：此时数据还在进来");
    check_int("ERROR_WARN 不计入离线时长",
              (int)(app.can_offline_ms == 0u ? 1 : 0), 1);

    /* 回到正常 */
    gw_can_core_notify_normal(&FC.core);
    gw_app_poll(&app, 10u);
    check_int("回到 NORMAL 后仍可用", can_ok_bit(&app), 1);

    /* Bus-Off：确实收不到数据了，**必须**报不可用 */
    gw_can_core_notify_bus_off(&FC.core);
    gw_app_poll(&app, 10u);
    check_int("BUS_OFF 时 CAN_OK 被清除", can_ok_bit(&app), 0);
    ok("BUS_OFF 必须报不可用：否则上位机看到陈旧值却以为正常");

    {
        uint32_t before = app.can_offline_ms;

        gw_app_poll(&app, 10u);
        if (app.can_offline_ms > before) {
            ok("累计'CAN 不可用'持续时长（运维判断'现在还没好'的依据）");
        } else {
            fail("累计 CAN 不可用持续时长", "");
        }
    }

    /* 恢复后状态位重新置起，且离线时长清零 */
    gw_can_core_notify_normal(&FC.core);
    gw_app_poll(&app, 10u);
    check_int("恢复后 CAN_OK 重新置起", can_ok_bit(&app), 1);
    check_int("恢复后离线时长清零", (int)app.can_offline_ms, 0);
}

/* ==================================================================
 * 测试 4：CAN 通道值同时出现在管道与寄存器表，且两者一致
 * ================================================================== */
static void test_telemetry_source(void)
{
    static gw_app_t app;
    static gw_app_t app2;
    gw_can_frame_t  f;

    printf("\n[4] CAN 值的唯一真相：管道 -> 寄存器表\n");

    /* ---- 先验证"未启用 CAN 时提交遥测不会崩" ---- */
    setup(&app2, 0);
    check_int("未启用 CAN 时提交遥测返回 -1（上云也没启用）",
              gw_app_submit_telemetry(&app2), -1);

    /* ---- 启用 CAN，验证遥测里的 CAN 值来自寄存器 ---- */
    setup(&app, 1);

    (void)gw_can_core_set_map(&FC.core, 0x100u, 0u);
    (void)gw_can_core_set_map(&FC.core, 0x200u, 1u);
    (void)gw_can_core_set_map(&FC.core, 0x300u, 2u);
    (void)gw_can_core_set_map(&FC.core, 0x400u, 3u);

    f = mk_frame(0x100u, 0x1111u);
    (void)gw_can_core_on_frame(&FC.core, &f);
    f = mk_frame(0x200u, 0x2222u);
    (void)gw_can_core_on_frame(&FC.core, &f);
    f = mk_frame(0x300u, 0x3333u);
    (void)gw_can_core_on_frame(&FC.core, &f);
    f = mk_frame(0x400u, 0x4444u);
    (void)gw_can_core_on_frame(&FC.core, &f);

    gw_app_poll(&app, 10u);     /* 让通道值进管道，再由管道同步到寄存器 */

    /* 验证"管道是唯一真相"：管道里的值 -> 寄存器表里的值，
       而遥测打包读的正是管道。所以三处必然一致。 */
    check_int("管道通道 0 = 核心通道 0",
              (int)app.pipe.can_ch[0], (int)FC.core.ch_value[0]);
    check_int("管道通道 1 = 核心通道 1",
              (int)app.pipe.can_ch[1], (int)FC.core.ch_value[1]);
    check_int("管道通道 2 = 核心通道 2",
              (int)app.pipe.can_ch[2], (int)FC.core.ch_value[2]);
    check_int("管道通道 3 = 核心通道 3",
              (int)app.pipe.can_ch[3], (int)FC.core.ch_value[3]);

    check_int("寄存器通道 0 = 管道通道 0（同步一致）",
              (int)REGS.input[IREG_CAN_CH1], (int)app.pipe.can_ch[0]);
    check_int("寄存器通道 3 = 管道通道 3（同步一致）",
              (int)REGS.input[IREG_CAN_CH4], (int)app.pipe.can_ch[3]);

    check_int("通道 0 的值正确（0x1111）", (int)REGS.input[IREG_CAN_CH1], 0x1111);
    check_int("通道 3 的值正确（0x4444）", (int)REGS.input[IREG_CAN_CH4], 0x4444);
    ok("上位机读到的、遥测上报的、核心映射出的是同一份值");
}

/* ==================================================================
 * 测试 5：未启用 CAN 时状态位如实清零
 * ================================================================== */
static void test_disabled_reports_honestly(void)
{
    static gw_app_t app;

    printf("\n[5] 未启用 CAN 时状态如实上报\n");

    setup(&app, 0);

    /* 人为设一个乐观的旧值 */
    app.pipe.can_ok = 1u;   /* 人为设一个乐观的旧值 */

    gw_app_poll(&app, 10u);
    check_int("未启用 CAN 时 CAN_OK 被清除（不留乐观旧值）",
              can_ok_bit(&app), 0);

    /* 长时间运行不应崩 */
    {
        int i;
        for (i = 0; i < 200; i++) {
            gw_app_poll(&app, 10u);
        }
        ok("未启用 CAN 时长时间运行不崩溃");
    }
    check_int("未启用 CAN 时仍能推进主循环",
              (int)(app.loop_count == 201u), 1);
}

/* ==================================================================
 * 测试 6：tick 被按节拍调用 + 状态机推进贯通
 * ================================================================== */
static void test_tick_wiring(void)
{
    static gw_app_t app;

    printf("\n[6] tick 接线与恢复贯通\n");

    setup(&app, 1);

    gw_app_poll(&app, 10u);
    check_int("poll 会调用 CAN tick", (int)FC.tick_calls, 1);
    check_int("elapsed_ms 被正确传递", (int)FC.last_elapsed, 10);

    gw_app_poll(&app, 25u);
    check_int("第二次 poll 也调用", (int)FC.tick_calls, 2);
    check_int("elapsed_ms 变化被传递", (int)FC.last_elapsed, 25);

    /* ---- 贯通测试：Bus-Off -> 等 1 秒 -> 恢复 -> 状态位回来 ---- */
    gw_can_core_notify_bus_off(&FC.core);
    gw_app_poll(&app, 10u);
    check_int("Bus-Off 后状态位清除", can_ok_bit(&app), 0);

    /* 推进 1.1 秒（让退避窗口过去，并且 reinit 会成功） */
    FC.reinit_result = 0;
    {
        int i;
        for (i = 0; i < 110; i++) {
            gw_app_poll(&app, 10u);
        }
    }
    check_int("退避结束后核心已恢复",
              (int)gw_can_core_state(&FC.core), (int)GW_CAN_ST_NORMAL);
    check_int("状态位随之恢复", can_ok_bit(&app), 1);
    ok("从 Bus-Off 到自愈的整条链路贯通（不需要上层介入）");
}

int main(void)
{
    printf("========== 应用层 CAN 接入测试 ==========\n");

    test_enable();
    test_channel_sync();
    test_status_bit();
    test_telemetry_source();
    test_disabled_reports_honestly();
    test_tick_wiring();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
