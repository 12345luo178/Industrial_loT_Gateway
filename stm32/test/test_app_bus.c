/**
 * @file    test_app_bus.c
 * @brief   应用层总线仲裁集成测试（主从共存的核心行为）
 *
 * ============================================================================
 *  这套测试验证的是"主从共存"最关键的四条性质：
 *
 *    1) 主站发请求前**必须先拿到总线** —— 拿不到就推迟，绝不硬发
 *    2) 推迟**不是错误** —— 同一个请求下一轮重发，轮询位置不前进
 *    3) 等待窗口被从站抢占后，主站放弃等待但
 *       **不把抢占算成从机超时**（否则正常的从机会被误判离线）
 *    4) 任何路径下总线都必须被释放 —— 泄漏一次就永久占用，
 *       整个 RS485 网络都瘫掉
 *
 *  实现方式：**用真实的 rs485_arb_t 做假移植层**。
 *
 *  这一点很重要：如果假移植层自己"假装"仲裁结果，
 *  那测的只是应用层的假设，而不是真实的仲裁交互。
 *  把真的仲裁器接进来，测试才覆盖了
 *  "应用层调用序列 × 仲裁器状态机"这个组合空间。
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iinc -Idrivers -Iapp -o test/test_app_bus.exe \
 *        src/modbus_crc.c src/modbus_rtu.c src/modbus_master.c src/gw_pipeline.c \
 *        src/rs485_arbiter.c drivers/dht22.c app/gw_app.c test/test_app_bus.c
 *    ./test/test_app_bus.exe
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

#include "gw_app.h"
#include "rs485_arbiter.h"

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
 * 假环境：真实的仲裁器 + 假总线 + 假从机应答
 * ================================================================== */
#define SENT_MAX 64

typedef struct {
    rs485_arb_t arb;                /* ★ 真实仲裁器 */

    mb_regs_t   regs;

    /* 捕获到的发送（按角色分别记） */
    uint8_t     sent_slave[SENT_MAX][MB_MAX_ADU];
    uint16_t    sent_slave_len[SENT_MAX];
    uint32_t    sent_slave_n;

    uint8_t     sent_master[SENT_MAX][MB_MAX_ADU];
    uint16_t    sent_master_len[SENT_MAX];
    uint32_t    sent_master_n;

    /* 收到的主站请求（供假从机应答） */
    uint8_t     last_req[MB_MAX_ADU];
    uint16_t    last_req_len;

    /* 假从机构造出的响应，交给测试主体喂回主站 */
    uint8_t     last_resp[MB_MAX_ADU];
    uint16_t    last_resp_len;
    uint8_t     resp_ready;

    uint32_t    release_master_calls;
    uint32_t    enter_wait_calls;
    uint32_t    exit_wait_calls;
} bus_env_t;

static bus_env_t ENV;

/* 假从机：模拟总线另一端的一台仪表 */
typedef struct {
    int      enabled;               /* 1 = 会应答 */
    uint8_t  addr;
    uint16_t regs[8];
} fake_slave_t;

static fake_slave_t FS;

/* ---------------- 移植层回调 ---------------- */

static int p_take_frame(void *ctx, uint8_t *buf, uint16_t buf_size, uint16_t *len)
{
    (void)ctx; (void)buf; (void)buf_size; (void)len;
    return -1;                      /* 本套测试不注入从站请求，抢占是显式发起的 */
}

/* 从站发送：内部申请总线（从站优先，会抢占主站等待窗口）-> 发送 -> 释放 */
static int p_send_as_slave(void *ctx, const uint8_t *buf, uint16_t len)
{
    bus_env_t *e = (bus_env_t *)ctx;

    /* 走真实仲裁：从站角色，会抢占主站的等待窗口 */
    if (rs485_arb_acquire(&e->arb, RS485_ROLE_SLAVE) != RS485_ACQ_OK) {
        return -1;
    }
    if (e->sent_slave_n < SENT_MAX) {
        memcpy(e->sent_slave[e->sent_slave_n], buf, len);
        e->sent_slave_len[e->sent_slave_n] = len;
        e->sent_slave_n++;
    }
    rs485_arb_release(&e->arb);     /* 从站发完立即释放 */
    return 0;
}

/* 主站申请总线 */
static int p_bus_acquire(void *ctx)
{
    bus_env_t *e = (bus_env_t *)ctx;
    return (rs485_arb_acquire(&e->arb, RS485_ROLE_MASTER) == RS485_ACQ_OK) ? 0 : -1;
}

/* 主站发送（已持权，不释放） */
static int p_send_held(void *ctx, const uint8_t *buf, uint16_t len)
{
    bus_env_t *e = (bus_env_t *)ctx;

    if (e->sent_master_n < SENT_MAX) {
        memcpy(e->sent_master[e->sent_master_n], buf, len);
        e->sent_master_len[e->sent_master_n] = len;
        e->sent_master_n++;
    }
    memcpy(e->last_req, buf, len);
    e->last_req_len = len;

    /* ---- 假从机立即应答 ----
       真实情况下响应会通过中断进来，这里直接构造并模拟"主站收到响应"。
       注意：应答内容用真实的从站引擎生成，保证帧格式合法。 */
    if (FS.enabled == 1) {
        mb_slave_t sl;
        mb_regs_t  sregs;
        uint8_t    resp[MB_MAX_ADU];
        uint16_t   rlen;
        uint16_t   i;

        memset(&sregs, 0, sizeof(sregs));
        for (i = 0u; i < 8u; i++) {
            sregs.holding[i] = FS.regs[i];
        }
        mb_slave_init(&sl, FS.addr, &sregs);
        if (mb_slave_process(&sl, buf, len) != 0) {
            rlen = sl.tx_len;
            memcpy(resp, sl.tx_buf, rlen);

            /* 响应先存起来，由测试主体在合适的时机喂回主站。
               为什么不在这里直接调 mb_master_on_response：
               本函数是被主站"发送过程"调用的，此时主站还没进入
               "等待响应"状态，直接喂回去会被当作无关帧丢弃。
               时序必须和真实中断路径一致：先发完、再收响应。 */
            memcpy(e->last_resp, resp, rlen);
            e->last_resp_len = rlen;
            e->resp_ready    = 1;
        }
    }
    return 0;
}

static void p_enter_wait(void *ctx)
{
    bus_env_t *e = (bus_env_t *)ctx;
    e->enter_wait_calls++;
    (void)rs485_arb_enter_wait(&e->arb);
}

static void p_exit_wait(void *ctx)
{
    bus_env_t *e = (bus_env_t *)ctx;
    e->exit_wait_calls++;
    rs485_arb_exit_wait(&e->arb);
}

static int p_release_master(void *ctx)
{
    bus_env_t *e = (bus_env_t *)ctx;

    e->release_master_calls++;
    if (rs485_arb_owner(&e->arb) != RS485_ROLE_MASTER) {
        return -1;
    }
    rs485_arb_release(&e->arb);
    return 0;
}

static int p_in_wait(void *ctx)
{
    bus_env_t *e = (bus_env_t *)ctx;
    return rs485_arb_in_wait(&e->arb);
}

static int p_master_holds(void *ctx)
{
    bus_env_t *e = (bus_env_t *)ctx;
    return (rs485_arb_owner(&e->arb) == RS485_ROLE_MASTER) ? 1 : 0;
}

static mb_regs_t *p_get_regs(void *ctx)
{
    bus_env_t *e = (bus_env_t *)ctx;
    return &e->regs;
}

static void env_reset(void)
{
    memset(&ENV, 0, sizeof(ENV));
    memset(&FS, 0, sizeof(FS));
    rs485_arb_init(&ENV.arb);
}

static void make_port(gw_port_t *p, bus_env_t *e)
{
    memset(p, 0, sizeof(*p));
    p->rs485_take_frame    = p_take_frame;
    p->rs485_send          = p_send_as_slave;
    p->rs485_bus_acquire   = p_bus_acquire;
    p->rs485_send_held     = p_send_held;
    p->rs485_enter_wait    = p_enter_wait;
    p->rs485_exit_wait     = p_exit_wait;
    p->rs485_release_master = p_release_master;
    p->rs485_in_wait       = p_in_wait;
    p->rs485_master_holds_bus = p_master_holds;
    p->get_regs            = p_get_regs;
    p->dht_sample          = 0;
    p->ctx                 = e;
}

/* 加一个轮询项：读从机 addr 的保持寄存器 0..3，映射到本地输入寄存器 0..3 */
static void add_poll(gw_app_t *app, uint8_t addr)
{
    mb_m_poll_item_t it;

    memset(&it, 0, sizeof(it));
    it.slave    = addr;
    it.func     = MB_FC_READ_HOLDING;
    it.start    = 0u;
    it.qty      = 4u;
    it.map_base = 0u;
    it.map_area = 0;                /* 写本地输入寄存器区 */
    (void)mb_master_add_item(gw_app_master(app), &it);
}


/* ==================================================================
 * 测试 1：主站拿到总线才发送，发完进入等待窗口
 * ================================================================== */
static void test_acquire_then_send(void)
{
    gw_app_t   app;
    gw_port_t  port;
    mb_slave_t slave;

    printf("\n[1] 主站：先拿总线，再发送，再进等待窗口\n");

    env_reset();
    make_port(&port, &ENV);
    memset(&slave, 0, sizeof(slave));

    mb_slave_init(&slave, 1u, &ENV.regs);
    if (gw_app_init(&app, &slave, &port) != 0) {
        fail("应用层初始化", "");
        return;
    }

    FS.enabled = 1;
    FS.addr    = 2u;
    FS.regs[0] = 0x1111u;
    add_poll(&app, 2u);

    gw_app_poll(&app, 0u);          /* 发请求 */

    check_int("主站发出了 1 帧", (int)ENV.sent_master_n, 1);
    check_int("发送后进入了等待窗口", (int)ENV.enter_wait_calls, 1);
    check_int("发送后总线由主站持有",
              (int)rs485_arb_owner(&ENV.arb), (int)RS485_ROLE_MASTER);
    check_int("等待窗口已置位", rs485_arb_in_wait(&ENV.arb), 1);

    /* 收到响应后应退出等待窗口并释放总线 */
    (void)mb_master_on_response(gw_app_master(&app),
                                ENV.last_resp, ENV.last_resp_len);
    gw_app_poll(&app, 0u);

    check_int("收到响应后退出等待窗口", (int)ENV.exit_wait_calls >= 1 ? 1 : 0, 1);
    check_int("收到响应后总线被释放",
              (int)rs485_arb_owner(&ENV.arb), (int)RS485_ROLE_NONE);
    check_int("等待窗口已清除", rs485_arb_in_wait(&ENV.arb), 0);
}

/* ==================================================================
 * 测试 2：总线被占时主站推迟，且不前进轮询位置
 * ================================================================== */
static void test_deferred_when_busy(void)
{
    gw_app_t   app;
    gw_port_t  port;
    mb_slave_t slave;
    uint8_t    idx_before;

    printf("\n[2] 总线被占：主站推迟，轮询位置不前进\n");

    env_reset();
    make_port(&port, &ENV);
    memset(&slave, 0, sizeof(slave));
    mb_slave_init(&slave, 1u, &ENV.regs);
    (void)gw_app_init(&app, &slave, &port);

    /* 从机 2、3 各一个轮询项，都为离线探测状态 */
    FS.enabled = 0;                 /* 不应答，让它超时 */
    add_poll(&app, 2u);
    add_poll(&app, 3u);

    /* 人为把总线占住（模拟另一台主站正在用，或从站正在发） */
    (void)rs485_arb_acquire(&ENV.arb, RS485_ROLE_SLAVE);

    idx_before = app.master.cur_index;
    gw_app_poll(&app, 0u);

    check_int("总线被占时主站不发送", (int)ENV.sent_master_n, 0);
    check_int("推迟被计数", (int)app.master_deferred >= 1 ? 1 : 0, 1);
    check_int("轮询位置没有前进（同一请求下次重发）",
              (int)app.master.cur_index, (int)idx_before);

    /* 释放总线后应能正常发出 */
    rs485_arb_release(&ENV.arb);
    gw_app_poll(&app, 0u);
    check_int("总线释放后主站发出请求", (int)ENV.sent_master_n >= 1 ? 1 : 0, 1);
}

/* ==================================================================
 * 测试 3：等待窗口被从站抢占 —— 不记从机失败（最关键的一条）
 * ================================================================== */
static void test_preempt_does_not_fail_slave(void)
{
    gw_app_t   app;
    gw_port_t  port;
    mb_slave_t slave;
    uint32_t   pre_before;
    uint32_t   sent_before_preempt;
    uint8_t    idx_before_preempt;

    printf("\n[3] 等待窗口被从站抢占：不把抢占算成从机超时\n");

    env_reset();
    make_port(&port, &ENV);
    memset(&slave, 0, sizeof(slave));
    mb_slave_init(&slave, 1u, &ENV.regs);
    (void)gw_app_init(&app, &slave, &port);

    FS.enabled = 0;                 /* 从机不应答：主站会一直等 */
    add_poll(&app, 2u);

    /* 主站发出请求并进入等待窗口 */
    gw_app_poll(&app, 0u);
    check_int("主站已持有总线",
              (int)rs485_arb_owner(&ENV.arb), (int)RS485_ROLE_MASTER);
    check_int("正在等待响应", app.master.awaiting, 1);

    /* ---- 上位机来读本机寄存器：以从站身份发送，抢占等待窗口 ---- */
    {
        uint8_t dummy[8] = { 1u, 3u, 0u, 0u, 0u, 4u, 0u, 0u };

        if (p_send_as_slave(&ENV, dummy, sizeof(dummy)) == 0) {
            ok("从站成功抢占并发送（从站优先级更高）");
        } else {
            fail("从站成功抢占并发送", "");
        }
    }
    check_int("从站发送后总线已释放（回到空闲）",
              (int)rs485_arb_owner(&ENV.arb), (int)RS485_ROLE_NONE);
    check_int("等待窗口已被抢占清除", rs485_arb_in_wait(&ENV.arb), 0);

    /* 此时主站引擎还以为自己在等 —— 下一步 poll 应检测到并放弃 */
    check_int("（前置）主站引擎仍以为在等待", app.master.awaiting, 1);

    pre_before = app.master_preempted;
    idx_before_preempt = app.master.cur_index;
    sent_before_preempt = ENV.sent_master_n;

    gw_app_poll(&app, 0u);

    check_int("应用层检测到抢占并计数",
              (int)(app.master_preempted - pre_before), 1);

    /* ⚠️ 这里不能断言 "awaiting == 0"。
     *
     * 抢占被检测到之后，同一轮 poll 里主站会**立刻重发同一个请求**
     * （这是期望行为：轮询位置不变，重发对从机无感），
     * 于是 awaiting 又变回 1。
     * 所以"awaiting 归零"只在检测的那一瞬间成立，不可观测。
     *
     * 真正该断言的是**重发是否发生**、**轮询位置是否不变**、
     * 以及**从机有没有被误判失败**。 */

    /* ---- 最关键的一条：不能把抢占算成从机失败 ---- */
    {
        const mb_m_slave_stat_t *ss = mb_master_slave_stat(&app.master, 2u);

        if (ss != 0) {
            check_int("从机失败计数未被污染（抢占不算从机失败）",
                      (int)ss->cnt_fail, 0);
            check_int("从机超时计数未被污染", (int)ss->cnt_timeout, 0);
            check_int("从机仍被视为在线（未被误判离线）",
                      (int)ss->online, 1);
        } else {
            fail("从机状态可查", "");
        }
    }

    /* 抢占后重发了**同一个**请求。
       注意：重发就发生在上面那次 poll 里，所以快照必须在那之前取。 */
    check_int("抢占后重发了请求",
              (int)(ENV.sent_master_n - sent_before_preempt), 1);
    check_int("重发的是同一个轮询项（位置未前进）",
              (int)app.master.cur_index, (int)idx_before_preempt);
    check_int("重发的请求内容与首次一致",
              memcmp(ENV.sent_master[ENV.sent_master_n - 1u],
                     ENV.sent_master[0], ENV.sent_master_len[0]) == 0 ? 1 : 0,
              1);
}

/* ==================================================================
 * 测试 4：总线不泄漏（任何路径都必须释放）
 *
 * ⚠️ 关于"怎么判定泄漏"
 *
 * 直觉写法是"poll 之后断言 owner == NONE"，但那是错的：
 * 主站发完请求后**本来就该持有总线**直到收到响应或超时。
 * 所以 poll 结束时 owner == MASTER 是**正常状态**，不是泄漏。
 *
 * 真正能判定泄漏的做法：让主站**无事可做**，再看总线是否被占。
 * 轮询表清空后 mb_master_poll 会立即返回，此时若 owner 仍是 MASTER，
 * 那就一定是前面某条路径忘了释放 —— 这才是泄漏。
 * ================================================================== */
static void test_no_bus_leak(void)
{
    gw_app_t   app;
    gw_port_t  port;
    mb_slave_t slave;
    int        i;

    printf("\n[4] 总线不泄漏：超时/重试/离线/抢占后都回到空闲\n");

    env_reset();
    make_port(&port, &ENV);
    memset(&slave, 0, sizeof(slave));
    mb_slave_init(&slave, 1u, &ENV.regs);
    (void)gw_app_init(&app, &slave, &port);

    FS.enabled = 0;                 /* 不应答，逼出超时与重试路径 */
    add_poll(&app, 2u);
    (void)mb_master_set_timing(gw_app_master(&app), 50u, 1u);

    /* 跑到从机被判离线（3 次连续失败） */
    for (i = 0; i < 40; i++) {
        gw_app_poll(&app, 50u);
    }

    check_int("从机已被判离线",
              (int)(mb_master_slave_stat(&app.master, 2u) != 0
                    && mb_master_slave_stat(&app.master, 2u)->online == 0u), 1);

    /* 再跑一段（退避、离线重试路径） */
    for (i = 0; i < 100; i++) {
        gw_app_poll(&app, 50u);
    }

    /* ---- 泄漏判定：清空轮询表，主站就无事可做了 ---- */
    mb_master_clear_items(gw_app_master(&app));
    gw_app_poll(&app, 50u);

    check_int("超时/重试/退避路径未泄漏总线（清表后回到空闲）",
              (int)rs485_arb_owner(&ENV.arb), (int)RS485_ROLE_NONE);
    check_int("超时/重试/退避路径未留下等待窗口",
              rs485_arb_in_wait(&ENV.arb), 0);

    /* ---- 抢占路径也不能泄漏 ---- */
    env_reset();
    make_port(&port, &ENV);
    memset(&slave, 0, sizeof(slave));
    mb_slave_init(&slave, 1u, &ENV.regs);
    (void)gw_app_init(&app, &slave, &port);

    FS.enabled = 1;
    FS.addr    = 2u;
    FS.regs[0] = 0x1234u;
    add_poll(&app, 2u);

    for (i = 0; i < 20; i++) {
        uint8_t dummy[8] = { 1u, 3u, 0u, 0u, 0u, 4u, 0u, 0u };

        gw_app_poll(&app, 50u);
        if (ENV.resp_ready != 0) {
            ENV.resp_ready = 0;
            (void)mb_master_on_response(gw_app_master(&app),
                                        ENV.last_resp, ENV.last_resp_len);
        }
        /* 每两轮插入一次从站发送（抢占） */
        if ((i % 2) == 0) {
            (void)p_send_as_slave(&ENV, dummy, sizeof(dummy));
        }
    }

    mb_master_clear_items(gw_app_master(&app));
    gw_app_poll(&app, 50u);

    check_int("抢占路径未泄漏总线（清表后回到空闲）",
              (int)rs485_arb_owner(&ENV.arb), (int)RS485_ROLE_NONE);
    check_int("抢占路径未留下等待窗口",
              rs485_arb_in_wait(&ENV.arb), 0);

    /* 仲裁器统计应反映真实发生过的抢占 */
    if (rs485_arb_stats(&ENV.arb)->slave_preempted >= 1u) {
        ok("仲裁器记录了抢占次数（可用于现场诊断）");
    } else {
        fail("仲裁器记录了抢占次数", "");
    }

    /* 从站全程都能发出（抢占保证上位机不被饿死） */
    if (ENV.sent_slave_n >= 10u) {
        ok("持续抢占期间从站始终能发出响应（上位机不被饿死）");
    } else {
        char d[64];
        sprintf(d, "(只发出 %lu 次)", (unsigned long)ENV.sent_slave_n);
        fail("持续抢占期间从站始终能发出响应", d);
    }
}

/* ==================================================================
 * 测试 5：混合负载下主站仍能完成轮询（抢占不导致饿死）
 * ================================================================== */
static void test_no_starvation(void)
{
    gw_app_t   app;
    gw_port_t  port;
    mb_slave_t slave;
    int        i;
    uint32_t   ok_before;

    printf("\n[5] 持续抢占下主站仍能完成轮询（不饿死）\n");

    env_reset();
    make_port(&port, &ENV);
    memset(&slave, 0, sizeof(slave));
    mb_slave_init(&slave, 1u, &ENV.regs);
    (void)gw_app_init(&app, &slave, &port);

    FS.enabled = 1;
    FS.addr    = 2u;
    FS.regs[0] = 0xABCDu;
    add_poll(&app, 2u);

    ok_before = mb_master_slave_stat(&app.master, 2u) != 0
                ? mb_master_slave_stat(&app.master, 2u)->cnt_ok : 0u;

    /* 上位机每轮都来读（持续抢占），主站仍应完成若干次成功轮询 */
    for (i = 0; i < 60; i++) {
        uint8_t dummy[8] = { 1u, 3u, 0u, 0u, 0u, 4u, 0u, 0u };

        /* 先让从站抢占（模拟上位机请求先到） */
        (void)p_send_as_slave(&ENV, dummy, sizeof(dummy));

        gw_app_poll(&app, 10u);

        if (ENV.resp_ready != 0) {
            ENV.resp_ready = 0;
            (void)mb_master_on_response(gw_app_master(&app),
                                        ENV.last_resp, ENV.last_resp_len);
        }
    }

    {
        const mb_m_slave_stat_t *ss = mb_master_slave_stat(&app.master, 2u);

        if (ss != 0 && ss->cnt_ok > ok_before) {
            char d[80];
            sprintf(d, "(成功 %lu 次)",
                    (unsigned long)(ss->cnt_ok - ok_before));
            printf("      [信息] 持续抢占下主站完成轮询 %s\n", d);
            ok("持续抢占下主站仍完成了成功轮询（未被饿死）");
        } else {
            fail("持续抢占下主站仍完成了成功轮询", "");
        }
    }

    check_int("从站始终可发送（抢占后本机仍能响应上位机）",
              (int)ENV.sent_slave_n >= 60 ? 1 : 0, 1);

    /* 泄漏判定：清空轮询表让主站无事可做，再看总线是否被占。
       直接断言"poll 后 owner == NONE"是错的 ——
       主站发完请求后本来就该持权等响应。 */
    mb_master_clear_items(gw_app_master(&app));
    gw_app_poll(&app, 10u);
    check_int("总线最终回到空闲",
              (int)rs485_arb_owner(&ENV.arb), (int)RS485_ROLE_NONE);
}

/* ==================================================================
 * 测试 6：退化路径 —— 移植层没提供仲裁回调时不应崩
 * ================================================================== */
static void test_degraded_port(void)
{
    gw_app_t   app;
    gw_port_t  port;
    mb_slave_t slave;

    printf("\n[6] 退化路径：移植层未提供仲裁回调\n");

    env_reset();
    make_port(&port, &ENV);

    /* 模拟"只做从站"阶段（L4）：没有主站仲裁回调 */
    port.rs485_bus_acquire    = 0;
    port.rs485_send_held      = 0;
    port.rs485_enter_wait     = 0;
    port.rs485_exit_wait      = 0;
    port.rs485_release_master = 0;
    port.rs485_in_wait        = 0;
    port.rs485_master_holds_bus = 0;

    memset(&slave, 0, sizeof(slave));
    mb_slave_init(&slave, 1u, &ENV.regs);

    if (gw_app_init(&app, &slave, &port) == 0) {
        ok("缺少仲裁回调时仍能初始化（只做从站的阶段可用）");
    } else {
        fail("缺少仲裁回调时仍能初始化", "");
        return;
    }

    FS.enabled = 0;
    add_poll(&app, 2u);

    /* 主站会走 rs485_send 退化路径（内部自行申请/释放） */
    gw_app_poll(&app, 0u);
    check_int("退化路径下主站仍能发出请求",
              (int)ENV.sent_slave_n >= 1 ? 1 : 0, 1);
    check_int("退化路径下总线未泄漏",
              (int)rs485_arb_owner(&ENV.arb), (int)RS485_ROLE_NONE);

    /* 再跑一段确认不崩 */
    {
        int i;
        for (i = 0; i < 50; i++) {
            gw_app_poll(&app, 50u);
        }
        ok("退化路径下长时间运行不崩溃");
    }
    check_int("退化路径下总线仍未泄漏",
              (int)rs485_arb_owner(&ENV.arb), (int)RS485_ROLE_NONE);
}

int main(void)
{
    printf("========== 应用层总线仲裁集成测试 ==========\n");

    test_acquire_then_send();
    test_deferred_when_busy();
    test_preempt_does_not_fail_slave();
    test_no_bus_leak();
    test_no_starvation();
    test_degraded_port();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
