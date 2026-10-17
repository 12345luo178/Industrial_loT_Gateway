/**
 * @file    test_uplink.c
 * @brief   上云链路单元测试（断线补传 / 退避 / 拥塞策略）
 *
 * ============================================================================
 *  本模块的核心承诺是"断网期间不丢数据"，所以测试的重点是**故障注入**：
 *
 *    - 断网后提交数据，恢复后能否按序补传完
 *    - 缓存满了丢的是最旧的还是最新的（策略必须明确且被验证）
 *    - 丢了多少条是否被如实计数（假称"一条没丢"比丢数据更糟）
 *    - 重连退避是否真的增长、是否有上限、鉴权失败是否停止重试
 *    - ACK 超时后是否正确重发同一帧（而不是重发别的帧）
 *    - 发送回调失败时数据是否还在队列里（绝不能静默丢弃）
 *
 *  时间由假时钟推进，所以"断网 10 分钟"能在毫秒内跑完且完全确定。
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iinc -o test/test_uplink.exe \
 *        src/gw_frame.c src/gw_uplink.c test/test_uplink.c
 *    ./test/test_uplink.exe
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

#include "gw_uplink.h"

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
 * 假环境：假时钟 + 发送捕获
 * ================================================================== */
#define SENT_MAX 2048

typedef struct {
    uint32_t clock_ms;

    /* 捕获到的遥测（解析后的结构体，便于断言序号与内容） */
    gw_telemetry_t sent[SENT_MAX];
    uint32_t       sent_count;

    /* 捕获到的控制帧类型 */
    uint8_t        ctrl[SENT_MAX];
    uint32_t       ctrl_count;

    int            send_fail;
    uint32_t       send_calls;
} fake_env_t;

static fake_env_t ENV;

static uint32_t fe_now(void *ctx)
{
    fake_env_t *e = (fake_env_t *)ctx;
    return e->clock_ms;
}

static int fe_send(void *ctx, const uint8_t *buf, uint16_t len)
{
    fake_env_t *e = (fake_env_t *)ctx;

    e->send_calls++;
    if (e->send_fail) {
        return -1;
    }

    /* 用真实的帧解析器把发出去的帧再解回来 ——
       这样测试同时验证了"组帧正确"与"载荷内容正确"，
       而不是只看字节数组。 */
    {
        gw_frame_parser_t p;
        uint8_t  type = 0u;
        uint8_t  payload[GW_FRAME_MAX_PAYLOAD];
        uint16_t plen = 0u;

        gw_frame_parser_init(&p);
        gw_frame_feed(&p, buf, len);
        if (gw_frame_take(&p, &type, payload, sizeof(payload), &plen) != GW_FRAME_OK) {
            return -1;                  /* 组帧有问题，直接判失败 */
        }

        if (type == GW_FT_TELEMETRY) {
            gw_telemetry_t t;
            if (gw_telemetry_unpack(payload, plen, &t) != 0) {
                return -1;
            }
            if (e->sent_count < SENT_MAX) {
                e->sent[e->sent_count] = t;
                e->sent_count++;
            }
        } else {
            if (e->ctrl_count < SENT_MAX) {
                e->ctrl[e->ctrl_count] = type;
                e->ctrl_count++;
            }
        }
    }
    return 0;
}

static void fe_init(fake_env_t *e)
{
    memset(e, 0, sizeof(*e));
}

static void make_io(gw_uplink_io_t *io, fake_env_t *e)
{
    memset(io, 0, sizeof(*io));
    io->send   = fe_send;
    io->now_ms = fe_now;
    io->ctx    = e;
}

/* 造一条遥测（只填数据字段，seq/dev_id 由模块填） */
static gw_telemetry_t mk_telem(int marker)
{
    gw_telemetry_t t;
    memset(&t, 0, sizeof(t));
    t.temp_x10 = (int16_t)marker;
    t.humi_x10 = (uint16_t)(600 + marker);
    t.ts       = (uint32_t)marker;
    return t;
}

/* 把链路置为已连接。
 *
 * ⚠️ 载荷第 2 字节必须用 GW_LINK_CONNECTED（值 2），不能随手写 1 ——
 *    1 是 CONNECTING。早期测试里写成了 {1,1,0,0}，结果链路一直停在
 *    "连接中"，所有依赖"已连接"的断言全挂。
 *    这类"魔数写错"的问题用常量就没机会发生。 */
static void go_online(gw_uplink_t *u, fake_env_t *e)
{
    uint8_t pl[4];
    pl[0] = 1u;                             /* Wi-Fi 已连接 */
    pl[1] = (uint8_t)GW_LINK_CONNECTED;     /* MQTT 已连接 */
    pl[2] = 0u;
    pl[3] = 0u;
    (void)gw_uplink_on_frame(u, GW_FT_LINK_STATUS, pl, 4u);
    (void)e;
}

/* 把链路置为断开 */
static void go_offline(gw_uplink_t *u)
{
    uint8_t pl[4];
    pl[0] = 0u;
    pl[1] = (uint8_t)GW_LINK_DISCONNECTED;
    pl[2] = 0u;
    pl[3] = 0u;
    (void)gw_uplink_on_frame(u, GW_FT_LINK_STATUS, pl, 4u);
}

/* 确认当前待确认的那一帧 */
static void ack(gw_uplink_t *u)
{
    (void)gw_uplink_on_frame(u, GW_FT_ACK, 0, 0u);
}

/* ==================================================================
 * 测试 1：初始化与入参
 * ================================================================== */
static void test_init(void)
{
    gw_uplink_t    u;
    gw_uplink_io_t io;

    printf("\n[1] 初始化与入参校验\n");

    fe_init(&ENV);
    make_io(&io, &ENV);

    if (gw_uplink_init(&u, &io, 100000u) == GW_UPLINK_OK) {
        ok("初始化成功");
    } else {
        fail("初始化成功", "");
        return;
    }

    check_int("初始状态为断开", (int)gw_uplink_state(&u), (int)GW_LINK_DISCONNECTED);
    check_int("初始积压为 0", (int)gw_uplink_pending(&u), 0);
    check_int("初始退避为 1 秒", (int)gw_uplink_backoff_s(&u), 1);

    if (gw_uplink_init(0, &io, 1u) == GW_UPLINK_ERR_PARAM &&
        gw_uplink_init(&u, 0, 1u) == GW_UPLINK_ERR_PARAM) {
        ok("空指针被拒");
    } else {
        fail("空指针被拒", "");
    }

    {
        gw_uplink_io_t bad;
        make_io(&bad, &ENV);
        bad.send = 0;
        if (gw_uplink_init(&u, &bad, 1u) == GW_UPLINK_ERR_PARAM) {
            ok("缺少 send 回调被拒");
        } else {
            fail("缺少 send 回调被拒", "");
        }
        make_io(&bad, &ENV);
        bad.now_ms = 0;
        if (gw_uplink_init(&u, &bad, 1u) == GW_UPLINK_ERR_PARAM) {
            ok("缺少 now_ms 回调被拒");
        } else {
            fail("缺少 now_ms 回调被拒", "");
        }
    }

    if (gw_uplink_init(&u, &io, 0u) == GW_UPLINK_ERR_PARAM) {
        ok("设备 ID 为 0 被拒（避免上线后无法区分设备）");
    } else {
        fail("设备 ID 为 0 被拒", "");
    }
}

/* ==================================================================
 * 测试 2：在线时立即上报
 * ================================================================== */
static void test_online_send(void)
{
    gw_uplink_t     u;
    gw_uplink_io_t  io;
    gw_telemetry_t  t;

    printf("\n[2] 链路正常时立即上报\n");

    fe_init(&ENV);
    make_io(&io, &ENV);
    (void)gw_uplink_init(&u, &io, 0x000186A0u);
    go_online(&u, &ENV);

    t = mk_telem(1);
    (void)gw_uplink_submit(&u, &t);

    check_int("提交后立即发出 1 帧", (int)ENV.sent_count, 1);
    check_int("模块填充了设备 ID",
              (int)(ENV.sent[0].dev_id == 0x000186A0u), 1);
    check_int("序号从 1 开始", (int)ENV.sent[0].seq, 1);
    check_int("数据内容正确", (int)ENV.sent[0].temp_x10, 1);
    check_int("提交后队列为空（已被取走待确认）", (int)gw_uplink_pending(&u), 0);

    /* 收到 ACK 后计入成功 */
    ack(&u);
    check_int("ACK 后成功计数 = 1", (int)gw_uplink_stats(&u)->sent_ok, 1);

    /* 序号应递增且不重复 */
    t = mk_telem(2);
    (void)gw_uplink_submit(&u, &t);
    check_int("第二条序号 = 2", (int)ENV.sent[1].seq, 2);
    ack(&u);

    /* 未 ACK 时不应继续发下一条（保证顺序与去重语义） */
    t = mk_telem(3);
    (void)gw_uplink_submit(&u, &t);     /* 发出，进入待确认 */
    check_int("第三条已发出", (int)ENV.sent_count, 3);
    t = mk_telem(4);
    (void)gw_uplink_submit(&u, &t);     /* 上一条还没 ACK，这条只能入队 */
    check_int("上一条未确认时新数据只入队不发", (int)ENV.sent_count, 3);
    check_int("队列里有 1 条积压", (int)gw_uplink_pending(&u), 1);
}

/* ==================================================================
 * 测试 3：断网缓存 + 恢复补传（本模块的核心承诺）
 * ================================================================== */
static void test_offline_then_flush(void)
{
    gw_uplink_t    u;
    gw_uplink_io_t io;
    int            i;
    uint32_t       sent_before;

    printf("\n[3] 断网缓存与恢复补传（核心承诺）\n");

    fe_init(&ENV);
    make_io(&io, &ENV);
    (void)gw_uplink_init(&u, &io, 1u);

    /* 初始为断开状态：提交 20 条，应全部进队列、一条都不发 */
    for (i = 0; i < 20; i++) {
        gw_telemetry_t t = mk_telem(i);
        (void)gw_uplink_submit(&u, &t);
    }
    check_int("断网时提交 20 条均不入发送通道", (int)ENV.sent_count, 0);
    check_int("断网时 20 条全部积压在队列", (int)gw_uplink_pending(&u), 20);
    check_int("入队计数 = 20", (int)gw_uplink_stats(&u)->queued, 20);

    /* 恢复链路 */
    sent_before = ENV.sent_count;
    go_online(&u, &ENV);

    /* 推进时间并逐条 ACK —— 模拟补传过程。
       每批最多 GW_UPLINK_FLUSH_BATCH 条，所以需要多次 tick。 */
    for (i = 0; i < 200 && gw_uplink_pending(&u) > 0; i++) {
        ENV.clock_ms += 100u;
        gw_uplink_tick(&u, 100u);
        if (u.awaiting_ack != 0u) {
            ack(&u);
        }
    }

    check_int("恢复后队列被清空（全部补传完）", (int)gw_uplink_pending(&u), 0);
    check_int("实际发出 20 条", (int)(ENV.sent_count - sent_before), 20);

    /* 补传必须按序号顺序，云端才能正确排序与去重 */
    {
        int order_ok = 1;
        uint32_t k;
        for (k = 1u; k < 20u; k++) {
            if (ENV.sent[sent_before + k].seq
                != ENV.sent[sent_before + k - 1u].seq + 1u) {
                order_ok = 0;
                break;
            }
        }
        if (order_ok) {
            ok("补传顺序严格按序号递增（云端可按序去重）");
        } else {
            fail("补传顺序严格按序号递增", "");
        }
    }

    check_int("无溢出丢包", (int)gw_uplink_stats(&u)->dropped_overflow, 0);
    check_int("成功计数 = 20", (int)gw_uplink_stats(&u)->sent_ok, 20);
}

/* ==================================================================
 * 测试 4：缓存溢出策略（丢最旧 + 如实计数）
 * ================================================================== */
static void test_overflow(void)
{
    gw_uplink_t    u;
    gw_uplink_io_t io;
    int            i;

    printf("\n[4] 缓存溢出策略\n");

    fe_init(&ENV);
    make_io(&io, &ENV);
    (void)gw_uplink_init(&u, &io, 1u);

    /* 断网状态下填满缓存 + 多提交 10 条 */
    for (i = 0; i < (int)GW_UPLINK_RING_DEPTH + 10; i++) {
        gw_telemetry_t t = mk_telem(i);
        (void)gw_uplink_submit(&u, &t);
    }

    check_int("缓存不会超过容量",
              (int)gw_uplink_pending(&u), (int)GW_UPLINK_RING_DEPTH);
    check_int("溢出丢弃 10 条并如实计数",
              (int)gw_uplink_stats(&u)->dropped_overflow, 10);

    /* 恢复后补传，验证保留的是**最新**的数据（丢最旧策略） */
    go_online(&u, &ENV);
    for (i = 0; i < 2000 && gw_uplink_pending(&u) > 0; i++) {
        ENV.clock_ms += 100u;
        gw_uplink_tick(&u, 100u);
        if (u.awaiting_ack != 0u) {
            ack(&u);
        }
    }
    check_int("恢复后全部补传", (int)gw_uplink_pending(&u), 0);

    /* 最早被保留的那条应该是第 10 条（前 10 条被丢弃） */
    if (ENV.sent_count > 0u) {
        check_int("保留的是较新的数据（第 10 条开始）",
                  (int)ENV.sent[0].temp_x10, 10);
        check_int("最后一条是最新的",
                  (int)ENV.sent[ENV.sent_count - 1u].temp_x10,
                  (int)(GW_UPLINK_RING_DEPTH + 9));
    }
}

/* ==================================================================
 * 测试 5：重连退避
 * ================================================================== */
static void test_backoff(void)
{
    gw_uplink_t    u;
    gw_uplink_io_t io;
    uint32_t       tries_before;
    int            i;

    printf("\n[5] 重连退避\n");

    fe_init(&ENV);
    make_io(&io, &ENV);
    (void)gw_uplink_init(&u, &io, 1u);

    /* 断网 + 不回应重连请求：应产生一系列重连尝试，且间隔逐渐拉长 */
    tries_before = gw_uplink_stats(&u)->reconnect_tries;
    check_int("初始退避为 1 秒", (int)gw_uplink_backoff_s(&u), 1);

    /* 推进 10 秒（每次 100ms） */
    for (i = 0; i < 100; i++) {
        ENV.clock_ms += 100u;
        gw_uplink_tick(&u, 100u);
    }

    if (gw_uplink_stats(&u)->reconnect_tries > tries_before) {
        ok("断网期间产生了重连尝试");
    } else {
        fail("断网期间产生了重连尝试", "");
    }

    /* 退避应已增长（从 1 秒涨上去） */
    if (gw_uplink_backoff_s(&u) > 1u) {
        ok("连续失败后退避时长增长");
    } else {
        fail("连续失败后退避时长增长", "");
    }

    /* 退避不应超过上限 */
    for (i = 0; i < 3000; i++) {
        ENV.clock_ms += 1000u;
        gw_uplink_tick(&u, 1000u);
    }
    if (gw_uplink_backoff_s(&u) <= GW_UPLINK_BACKOFF_MAX_S) {
        ok("退避时长不超过上限（避免恢复后迟迟接不回）");
    } else {
        char d[64];
        sprintf(d, "(实际 %lu 秒)", (unsigned long)gw_uplink_backoff_s(&u));
        fail("退避时长不超过上限", d);
    }

    /* 恢复后退避应重置 */
    go_online(&u, &ENV);
    check_int("恢复后退避重置为 1 秒", (int)gw_uplink_backoff_s(&u), 1);
    if (gw_uplink_stats(&u)->reconnects >= 1u) {
        ok("恢复被计入重连成功次数");
    } else {
        fail("恢复被计入重连成功次数", "");
    }

    /* 鉴权失败：不应继续自动重试 */
    {
        uint32_t tries_now;
        uint8_t  pl[4] = { 1u, (uint8_t)GW_LINK_AUTH_FAILED, 0u, 0u };

        (void)gw_uplink_on_frame(&u, GW_FT_LINK_STATUS, pl, 4u);
        check_int("鉴权失败状态被识别",
                  (int)gw_uplink_state(&u), (int)GW_LINK_AUTH_FAILED);

        tries_now = gw_uplink_stats(&u)->reconnect_tries;
        for (i = 0; i < 600; i++) {         /* 推进 60 秒 */
            ENV.clock_ms += 100u;
            gw_uplink_tick(&u, 100u);
        }
        check_int("鉴权失败后不再自动重试（重试也不会成功，只会耗电）",
                  (int)gw_uplink_stats(&u)->reconnect_tries, (int)tries_now);

        /* 上层更新密钥后显式恢复 */
        go_online(&u, &ENV);
        check_int("上层显式恢复后回到已连接",
                  (int)gw_uplink_state(&u), (int)GW_LINK_CONNECTED);
    }
}

/* ==================================================================
 * 测试 6：ACK 超时与重发
 * ================================================================== */
static void test_ack_timeout_retry(void)
{
    gw_uplink_t    u;
    gw_uplink_io_t io;
    gw_telemetry_t t;
    uint32_t       sent_before;
    int            i;

    printf("\n[6] ACK 超时与重发\n");

    fe_init(&ENV);
    make_io(&io, &ENV);
    (void)gw_uplink_init(&u, &io, 1u);
    go_online(&u, &ENV);

    /* 清空基线。
       注意这里不用 ack()：go_online 期间若正在等 ACK，链路状态通知会被
       **延迟**（这是刻意的设计，避免把已发出的请求作废），
       而那只是为了保护"已发出但未确认"的那个请求，与本用例无关。
       所以直接显式设置链路状态并清计数，得到干净起点。 */
    gw_uplink_set_link_state(&u, GW_LINK_CONNECTED);
    u.awaiting_ack = 0u;
    ENV.sent_count = 0u;
    ENV.ctrl_count = 0u;

    /* 基线必须在 submit **之前**记录 —— 因为链路正常时 submit 会立即发送，
       在它之后记录基线就永远算出差值为 0。 */
    sent_before = ENV.sent_count;

    t = mk_telem(42);
    (void)gw_uplink_submit(&u, &t);
    check_int("首次发出 1 帧", (int)(ENV.sent_count - sent_before), 1);

    /* 未到超时时间：不应重发 */
    ENV.clock_ms += 500u;
    gw_uplink_tick(&u, 500u);
    check_int("未到 ACK 超时前不重发",
              (int)(ENV.sent_count - sent_before), 1);

    /* 超过超时：应重发**同一帧**（序号与内容都不变） */
    ENV.clock_ms += (GW_UPLINK_ACK_TIMEOUT_MS + 100u);
    gw_uplink_tick(&u, GW_UPLINK_ACK_TIMEOUT_MS + 100u);
    check_int("超时后重发（第 2 次）", (int)(ENV.sent_count - sent_before), 2);

    if (ENV.sent_count >= 2u
        && ENV.sent[sent_before].seq == ENV.sent[sent_before + 1u].seq
        && ENV.sent[sent_before].temp_x10 == ENV.sent[sent_before + 1u].temp_x10) {
        ok("重发的是同一帧（序号与内容一致，云端可去重）");
    } else {
        fail("重发的是同一帧", "");
    }

    /* 继续超时直到重试用尽 */
    for (i = 0; i < (int)GW_UPLINK_MAX_RETRY + 2; i++) {
        ENV.clock_ms += (GW_UPLINK_ACK_TIMEOUT_MS + 100u);
        gw_uplink_tick(&u, GW_UPLINK_ACK_TIMEOUT_MS + 100u);
    }
    check_int("重试用尽后不再重发",
              (int)(ENV.sent_count - sent_before),
              (int)GW_UPLINK_MAX_RETRY + 1);
    check_int("本条被计为发送失败",
              (int)gw_uplink_stats(&u)->send_fail, 1);
    check_int("单条失败不影响链路状态（仍为已连接）",
              (int)gw_uplink_state(&u), (int)GW_LINK_CONNECTED);
}

/* ==================================================================
 * 测试 7：发送失败不丢数据
 * ================================================================== */
static void test_send_failure_keeps_data(void)
{
    gw_uplink_t    u;
    gw_uplink_io_t io;
    gw_telemetry_t t;
    int            i;

    printf("\n[7] 发送回调失败时不丢数据\n");

    fe_init(&ENV);
    make_io(&io, &ENV);
    (void)gw_uplink_init(&u, &io, 1u);
    go_online(&u, &ENV);

    /* 让发送回调一直失败 */
    ENV.send_fail = 1;

    t = mk_telem(7);
    (void)gw_uplink_submit(&u, &t);
    check_int("发送失败时数据仍在队列里", (int)gw_uplink_pending(&u), 1);
    check_int("发送失败被计数", (int)gw_uplink_stats(&u)->send_fail, 1);

    /* tick 多次也不应把数据丢掉 */
    for (i = 0; i < 20; i++) {
        ENV.clock_ms += 100u;
        gw_uplink_tick(&u, 100u);
    }
    check_int("反复 tick 后数据仍未丢失", (int)gw_uplink_pending(&u), 1);

    /* 串口恢复正常：数据应被补传出去 */
    ENV.send_fail = 0;
    for (i = 0; i < 50 && gw_uplink_pending(&u) > 0; i++) {
        ENV.clock_ms += 100u;
        gw_uplink_tick(&u, 100u);
        if (u.awaiting_ack != 0u) {
            ack(&u);
        }
    }
    check_int("串口恢复后数据被补传", (int)gw_uplink_pending(&u), 0);
    if (ENV.sent_count >= 1u && ENV.sent[0].temp_x10 == 7) {
        ok("补传的数据内容正确");
    } else {
        fail("补传的数据内容正确", "");
    }
}

/* ==================================================================
 * 测试 8：心跳与断网时长统计
 * ================================================================== */
static void test_heartbeat_and_offline_time(void)
{
    gw_uplink_t    u;
    gw_uplink_io_t io;
    uint32_t       ctrl_before;
    int            i;

    printf("\n[8] 心跳与断网时长统计\n");

    fe_init(&ENV);
    make_io(&io, &ENV);
    (void)gw_uplink_init(&u, &io, 1u);
    go_online(&u, &ENV);

    /* 无遥测时也应按周期发心跳 */
    ctrl_before = ENV.ctrl_count;
    for (i = 0; i < 400; i++) {         /* 推进 40 秒 */
        ENV.clock_ms += 100u;
        gw_uplink_tick(&u, 100u);
    }
    if (gw_uplink_stats(&u)->heartbeats >= 1u) {
        ok("无遥测时仍按周期发送心跳（双向确认链路存活）");
    } else {
        fail("无遥测时仍按周期发送心跳", "");
    }
    (void)ctrl_before;

    /* 断网 -> 恢复，应记录断网持续时长 */
    {
        go_offline(&u);

        for (i = 0; i < 300; i++) {     /* 断网 30 秒 */
            ENV.clock_ms += 100u;
            gw_uplink_tick(&u, 100u);
        }
        go_online(&u, &ENV);

        if (gw_uplink_stats(&u)->last_offline_s >= 29u
            && gw_uplink_stats(&u)->last_offline_s <= 31u) {
            ok("记录了断网持续时长（现场排查的关键数字）");
        } else {
            char d[64];
            sprintf(d, "(实际 %lu 秒, 期望约 30)",
                    (unsigned long)gw_uplink_stats(&u)->last_offline_s);
            fail("记录断网持续时长", d);
        }
    }
}

/* ==================================================================
 * 测试 9：帧分派边界（业务帧交回上层）
 * ================================================================== */
static void test_frame_dispatch(void)
{
    gw_uplink_t    u;
    gw_uplink_io_t io;
    uint8_t        cmd[3] = { 'A', 'T', '+' };

    printf("\n[9] 帧分派边界\n");

    fe_init(&ENV);
    make_io(&io, &ENV);
    (void)gw_uplink_init(&u, &io, 1u);

    /* 本模块处理的类型 */
    {
        uint8_t pl[4] = { 1u, 2u, 3u, 4u };
        if (gw_uplink_on_frame(&u, GW_FT_LINK_STATUS, pl, 4u) == GW_UPLINK_OK) {
            ok("LINK_STATUS 由本模块处理");
        } else {
            fail("LINK_STATUS 由本模块处理", "");
        }
        if (gw_uplink_on_frame(&u, GW_FT_ACK, 0, 0u) == GW_UPLINK_OK) {
            ok("ACK 由本模块处理");
        } else {
            fail("ACK 由本模块处理", "");
        }
        if (gw_uplink_on_frame(&u, GW_FT_NACK, 0, 0u) == GW_UPLINK_OK) {
            ok("NACK 由本模块处理");
        } else {
            fail("NACK 由本模块处理", "");
        }
    }

    /* 业务类型应返回 ERR_PARAM，交回上层 —— 保持"只负责链路"的边界 */
    if (gw_uplink_on_frame(&u, GW_FT_COMMAND, cmd, sizeof(cmd))
        == GW_UPLINK_ERR_PARAM) {
        ok("COMMAND 交回上层（本模块不解释业务指令）");
    } else {
        fail("COMMAND 交回上层", "");
    }
    if (gw_uplink_on_frame(&u, GW_FT_LOG, cmd, sizeof(cmd))
        == GW_UPLINK_ERR_PARAM) {
        ok("LOG 交回上层");
    } else {
        fail("LOG 交回上层", "");
    }

    /* 长度不足的 LINK_STATUS 不应崩溃 */
    if (gw_uplink_on_frame(&u, GW_FT_LINK_STATUS, 0, 0u) == GW_UPLINK_OK) {
        ok("空载荷 LINK_STATUS 被安全忽略");
    } else {
        fail("空载荷 LINK_STATUS 被安全忽略", "");
    }

    /* 链路状态字符串 */
    if (strcmp(gw_link_state_str(GW_LINK_CONNECTED), "CONNECTED") == 0
        && strcmp(gw_link_state_str(GW_LINK_AUTH_FAILED), "AUTH_FAILED") == 0
        && gw_link_state_str((gw_link_state_t)99) != 0) {
        ok("链路状态字符串正确且有兜底");
    } else {
        fail("链路状态字符串正确", "");
    }

    if (strcmp(gw_uplink_strerror(GW_UPLINK_OK), "OK") == 0
        && gw_uplink_stats(0) == 0) {
        ok("错误码与空指针防护正确");
    } else {
        fail("错误码与空指针防护正确", "");
    }
}

int main(void)
{
    printf("========== 上云链路单元测试 ==========\n");

    test_init();
    test_online_send();
    test_offline_then_flush();
    test_overflow();
    test_backoff();
    test_ack_timeout_retry();
    test_send_failure_keeps_data();
    test_heartbeat_and_offline_time();
    test_frame_dispatch();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
