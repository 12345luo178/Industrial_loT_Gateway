/**
 * @file    test_master_dispatch.c
 * @brief   回归测试：从站任务必须把"主站的响应"派发给主站
 *
 * ============================================================================
 *  这个测试守护一个**上板才暴露的真实缺陷**
 *
 *  现象（上板）：启用主站后，从站不再应答，而主站对外表现为
 *      "错误码 0、在线位图 0、从不发请求" —— 整条总线被自己锁死。
 *
 *  真因：`mb_master_on_response()` **在生产代码里从来没被调用过**。
 *      全工程搜它只有两处：`modbus_master.c` 的定义，
 *      以及 `test/` 目录下**手工喂响应**的测试。
 *      应用层一个调用点都没有。
 *
 *      于是主站发完请求后没人把响应交给它，只能一路超时；
 *      而超时期间必须持有总线，从站也就收不进来了。
 *
 *  ⚠️ 为什么原来 2275 项测试全过却漏掉了它 —— 这正是本测试存在的理由：
 *
 *      `test_app_bus.c` 的 `p_take_frame()` 直接 **return -1**
 *      （注释写着"本套测试不注入从站请求"），
 *      而其它测试干脆**手工调** `mb_master_on_response()`。
 *      两条路都**绕过了**"从站任务把帧交给主站"这段真实代码。
 *
 *      也就是说：测试验证的是"主站收到响应后行为正确"，
 *      却从没验证"响应对不对**送得到**主站那里"。
 *      —— 缺的是**接线**，不是逻辑。
 *
 * ============================================================================
 *  本测试怎么做
 *
 *      1) 让主站发一个请求（gw_app_poll 推一轮）-> 进入 awaiting
 *      2) 往"接收队列"里放一帧**合法的响应**（就是刚才那个请求的应答）
 *      3) 再推一轮 gw_app_poll —— 此时从站任务会 take_frame 到那一帧
 *      4) 断言：主站不再是 awaiting，且从机被标记为在线
 *
 *      ✅ 修复前：第 4 步失败（帧被从站引擎当成"地址不匹配"丢掉，主站一直等）
 *      ✅ 修复后：通过
 * ============================================================================
 */

#include <stdio.h>
#include <string.h>

#include "gw_app.h"
#include "gw_pipeline.h"
#include "modbus_rtu.h"
#include "modbus_crc.h"

static int g_pass = 0;
static int g_fail = 0;

static void ok(const char *n) { g_pass++; printf("  [PASS] %s\n", n); }
static void fail(const char *n, const char *d)
{
    g_fail++;
    printf("  [FAIL] %s  %s\n", n, d);
}
static void check_int(const char *n, int actual, int expected)
{
    if (actual == expected) {
        ok(n);
    } else {
        char d[96];
        sprintf(d, "(实际 %d, 期望 %d)", actual, expected);
        fail(n, d);
    }
}

/* ==================================================================
 *  测试环境
 * ================================================================== */
static mb_regs_t  REGS;
static mb_slave_t SLAVE;

/* 收到的（要交给主站的）帧队列 */
static uint8_t  g_rx[64];
static uint16_t g_rx_len = 0u;

/* 主站发出的请求 */
static uint8_t  g_tx[64];
static uint16_t g_tx_len = 0u;
static int      g_tx_n = 0;

/* 主站写回本机寄存器表的次数 */

static int  p_take(void *ctx, uint8_t *buf, uint16_t sz, uint16_t *len)
{
    (void)ctx;
    if (g_rx_len == 0u) {
        return -1;                      /* 没有帧：正常返回"没收到" */
    }
    if (g_rx_len > sz) {
        return -1;
    }
    memcpy(buf, g_rx, g_rx_len);
    *len = g_rx_len;
    g_rx_len = 0u;                      /* 取走 */
    return 0;
}

static int p_send(void *ctx, const uint8_t *buf, uint16_t len)
{
    (void)ctx;
    if (len <= sizeof(g_tx)) {
        memcpy(g_tx, buf, len);
        g_tx_len = len;
        g_tx_n++;
    }
    return 0;
}

/* 总线辅助：本测试不关心仲裁细节，一律成功 */
static uint8_t g_owner = 0u;
static uint8_t g_in_wait = 0u;

/* ⚠️ 单独一个开关来模拟"申请成功、但持有的不是主站身份"的移植层
   （用例 5 用它复现上板那个配错角色的缺陷）。
   默认 1 = 正常移植层；老用例不受影响。 */
static uint8_t g_holds_ok = 1u;

/* ⚠️ 模拟"总线已被占用、再申请会失败"的移植层（用例 6 用）。
   置 1 后 p_acq 返回失败 —— 用于验证"已持总线时不该二次申请"。 */
static uint8_t g_acq_fail = 0u;

static int p_acq(void *ctx)
{
    (void)ctx;
    if (g_acq_fail != 0u) {
        return -1;
    }
    g_owner = 1u;
    return 0;
}
static int p_send_held(void *ctx, const uint8_t *b, uint16_t l)
{ return p_send(ctx, b, l); }
static void p_enter(void *ctx)        { (void)ctx; g_in_wait = 1u; }
static void p_exit(void *ctx)         { (void)ctx; g_in_wait = 0u; }
static int  p_in_wait(void *ctx)      { (void)ctx; return g_in_wait ? 1 : 0; }
static int  p_holds(void *ctx)        { (void)ctx; return (g_owner && g_holds_ok) ? 1 : 0; }
static mb_regs_t *p_get(void *ctx)    { (void)ctx; return &REGS; }

static void make_port(gw_port_t *p)
{
    memset(p, 0, sizeof(*p));
    p->rs485_take_frame       = p_take;
    p->rs485_send             = p_send;
    p->rs485_bus_acquire      = p_acq;
    p->rs485_send_held        = p_send_held;
    p->rs485_enter_wait       = p_enter;
    p->rs485_exit_wait        = p_exit;
    p->rs485_in_wait          = p_in_wait;
    p->rs485_master_holds_bus = p_holds;
    p->get_regs               = p_get;
}

static void reset_env(void)
{
    memset(&REGS, 0, sizeof(REGS));
    memset(&SLAVE, 0, sizeof(SLAVE));
    mb_slave_init(&SLAVE, 1u, &REGS);
    g_rx_len = 0u; g_tx_len = 0u; g_tx_n = 0;
    g_owner = 0u; g_in_wait = 0u; g_holds_ok = 1u; g_acq_fail = 0u;
}

/* 组一个"读输入寄存器 0..3"的合法响应（从机 2 号） */
static uint16_t build_response(uint8_t *out, uint8_t addr,
                               const uint16_t *vals, uint16_t qty)
{
    uint16_t n = 0u, i, crc;

    out[n++] = addr;
    out[n++] = 0x04u;
    out[n++] = (uint8_t)(qty * 2u);
    for (i = 0u; i < qty; i++) {
        out[n++] = (uint8_t)(vals[i] >> 8);
        out[n++] = (uint8_t)(vals[i] & 0xFFu);
    }
    crc = mb_crc16(out, n);
    out[n++] = (uint8_t)(crc & 0xFFu);
    out[n++] = (uint8_t)(crc >> 8);
    return n;
}

/* ==================================================================
 *  用例 1：主站发出请求后，从站任务必须把响应派发给它
 * ================================================================== */
static void test_response_reaches_master(void)
{
    gw_app_t  app;
    gw_port_t port;
    mb_m_poll_item_t it;
    static const uint16_t want[4] = { 1000u, 1003u, 1006u, 1009u };

    printf("\n[1] 从站任务把主站的响应派发回去（本测试守护的缺陷）\n");

    reset_env();
    make_port(&port);
    if (gw_app_init(&app, &SLAVE, &port) != 0) {
        fail("gw_app_init 成功", "");
        return;
    }

    memset(&it, 0, sizeof(it));
    it.slave    = 2u;                   /* 从机 2 号 */
    it.func     = 0x04u;
    it.start    = 0u;
    it.qty      = 4u;
    it.map_base = 8u;
    it.map_area = 1u;
    if (mb_master_add_item(gw_app_master(&app), &it) != MB_M_OK) {
        fail("登记轮询项成功", "");
        return;
    }

    /* ---- 第一轮：主站发请求 ---- */
    gw_app_poll(&app, 0u);
    check_int("主站发出了 1 帧", g_tx_n, 1);
    check_int("主站进入等待响应的状态",
              (int)app.master.awaiting, 1);

    /* ⚠️ 关键：这里**不手工调 mb_master_on_response**。
       我们要验证的正是"应用层会自己派发"。
       所以只把响应放进接收队列，然后推进一轮。 */
    g_rx_len = build_response(g_rx, 2u, want, 4u);
    gw_app_poll(&app, 0u);

    /* ==============================================================
     *  ⚠️ 断言要盯**持久的事实**，不能盯 transient 状态
     *
     *  我第一版断言的是 `app.master.awaiting == 0`，它失败了 —— 但
     *  **不是代码错，是断言错**：
     *
     *      mb_master_poll 处理完响应后会**立刻发出下一个请求**，
     *      于是 awaiting 在同一轮里又变回 1。
     *      它是"当前是否在等"，而不是"刚才是否收到过"。
     *      （真实系统里它的节拍由主循环决定，约 1ms 一次；
     *        而这个测试每轮 poll 都推进它，所以它一直在发。）
     *
     *  真正持久、且**只有派发成功才会发生**的是下面两件事：
     *      1) 主站把读回的数据写进了寄存器表（store 回调被调用）
     *      2) 从机被标记为在线（只有收到合法响应才会置位）
     * ============================================================== */
    /* ==============================================================
     *  ⚠️ 断言**寄存器表本身**，不要断言回调计数
     *
     *  我第一版挂了一个 p_store 回调去数调用次数，永远是 0 ——
     *  但那是**观测点错了**，不是代码错：
     *      `app_master_store` 是 gw_app.c 里的**静态**函数，
     *      由 gw_app 自己传给 mb_master_init，**不经过 port**。
     *      所以我的回调根本不可能被调用。
     *
     *  正确的观测点是"它写出来的效果"：寄存器表里的值。
     *  这既不依赖内部接线，也是**用户真正看得到的东西**。
     * ============================================================== */
    check_int("⚠️ 主站读回的值写进了本机保持寄存器[8]（1000）",
              (int)REGS.holding[8], 1000);
    check_int("主站读回的值写进了本机保持寄存器[9]（1003）",
              (int)REGS.holding[9], 1003);
    check_int("主站读回的值写进了本机保持寄存器[10]（1006）",
              (int)REGS.holding[10], 1006);
    check_int("主站读回的值写进了本机保持寄存器[11]（1009）",
              (int)REGS.holding[11], 1009);

    {
        const mb_m_slave_stat_t *ss = mb_master_slave_stat(
            gw_app_master(&app), 2u);
        /* 在线位只在收到合法响应后置位 —— 这是"派发成功"的独立证据 */
        check_int("⚠️ 从机 2 被标记为在线（只有收到合法响应才会）",
                  (ss != 0 && ss->online != 0u) ? 1 : 0, 1);
    }

    check_int("主站发出了不止一个请求（收到响应后继续下一轮）",
              g_tx_n >= 2 ? 1 : 0, 1);
}

/* ==================================================================
 *  用例 2：不是给主站的帧，不能吞掉 —— 必须继续交给从站引擎
 *
 *  ⚠️ 这条防的是"派发写成一刀切"：如果收到任何帧都喂给主站，
 *     那么 PC 在 STM32 主站等待期间发来的**从站请求会被丢掉**。
 * ================================================================== */
static void test_slave_request_not_swallowed(void)
{
    gw_app_t  app;
    gw_port_t port;
    mb_m_poll_item_t it;
    uint8_t  req[8];
    uint16_t crc;
    const mb_m_slave_stat_t *ss;

    printf("\n[2] 主站等待期间的从站请求不能被吞掉\n");

    reset_env();
    make_port(&port);
    if (gw_app_init(&app, &SLAVE, &port) != 0) {
        fail("gw_app_init 成功", "");
        return;
    }

    memset(&it, 0, sizeof(it));
    it.slave    = 2u;                   /* 主站轮询的是 2 号 */
    it.func     = 0x04u;
    it.qty      = 4u;
    it.map_base = 8u;
    it.map_area = 1u;
    (void)mb_master_add_item(gw_app_master(&app), &it);

    gw_app_poll(&app, 0u);
    check_int("主站进入等待", (int)app.master.awaiting, 1);

    /* ⚠️ 放一帧**发给本机从站（地址 1）**的请求。
       主站等的是 2 号，所以这帧不该被主站消费。 */
    req[0] = 1u; req[1] = 0x03u;
    req[2] = 0u; req[3] = 0u; req[4] = 0u; req[5] = 1u;
    crc = mb_crc16(req, 6u);
    req[6] = (uint8_t)(crc & 0xFFu);
    req[7] = (uint8_t)(crc >> 8);
    memcpy(g_rx, req, 8u);
    g_rx_len = 8u;

    g_tx_n = 0;                         /* 清掉，只看这一轮有没有从站响应 */
    gw_app_poll(&app, 0u);

    check_int("从站引擎仍然处理了这一帧（发出了响应）",
              g_tx_n >= 1 ? 1 : 0, 1);

    /* ==================================================================
     *  回归：站号不符的帧**不能**被记成"轮询目标从机的失败"
     *
     *  ⚠️ 这是 2026-10-10 上板实测抓出来的缺陷（主从共存时的关键一条）：
     *     派发会把这种帧先交给 mb_master_on_response()，
     *     而它原来在"站号不符"这条路径上也走 fail -> note_fail + advance，
     *     于是**上位机每读一次板子，就给下游从机记一次失败** ——
     *     连续 3 次（MB_M_FAIL_TO_OFFLINE）就把一台正常的从机判成离线。
     *     现场表现：在线位图永远是 0、错误码固定 4，而保持寄存器 8~11 明明是对的。
     * ================================================================== */
    ss = mb_master_slave_stat(gw_app_master(&app), 2u);
    check_int("(回归)站号不符的帧不计入 2 号从机的失败次数",
              (ss != 0) ? (int)ss->cnt_fail : -1, 0);
    check_int("(回归)主站仍在等自己的响应，没被这帧带偏",
              (int)app.master.awaiting, 1);
}

/* ==================================================================
 *  用例 3：主站未在等待时，收到的帧必须直接交给从站
 * ================================================================== */
static void test_normal_slave_path_unaffected(void)
{
    gw_app_t  app;
    gw_port_t port;
    uint8_t  req[8];
    uint16_t crc;

    printf("\n[3] 主站没在等待时，从站路径不受影响\n");

    reset_env();
    make_port(&port);
    if (gw_app_init(&app, &SLAVE, &port) != 0) {
        fail("gw_app_init 成功", "");
        return;
    }
    /* 故意**不登记**任何轮询项 */

    req[0] = 1u; req[1] = 0x03u;
    req[2] = 0u; req[3] = 0u; req[4] = 0u; req[5] = 1u;
    crc = mb_crc16(req, 6u);
    req[6] = (uint8_t)(crc & 0xFFu);
    req[7] = (uint8_t)(crc >> 8);
    memcpy(g_rx, req, 8u);
    g_rx_len = 8u;

    gw_app_poll(&app, 0u);

    check_int("从站正常响应", g_tx_n >= 1 ? 1 : 0, 1);
    check_int("主站没有等待窗口（没登记轮询项）",
              (int)app.master.awaiting, 0);
}

/* ==================================================================
 *  用例 4：轮询地址 == 本机从站地址 —— 必须在登记时就被拦下
 *
 *  ⚠️ 这条防的是一个**只在配置层出现**的缺陷，而本文件原来的用例
 *     恰好绕开了它：用例 2 里主站轮询 2 号、本机从站是 1 号，
 *     两者从来不相同。
 *
 *     上板验证时现场恰恰把两者都设成了 1（固件常量 + PC 模拟从机），
 *     于是：上位机发给板子**本机从站**的读请求，会先被派发喂给
 *     mb_master_on_response() —— 站号对、CRC 对、功能码对，
 *     只有"字节数"对不上，于是它被记成**一次从机响应失败**并跳过
 *     一个轮询项。现象是每读一次板子，主站就误记一次失败，
 *     fail_streak 累积后从机被判离线，在线位图永远是 0 ——
 *     看起来像"主站根本没工作"。
 *
 *  两段断言：
 *     (a) 说明危害：同地址时那条请求确实被记成了从机失败
 *     (b) 配置入口把它拦下：返回 GW_APP_ITEM_ERR_LOCAL，一项都不登记
 * ================================================================== */
static void test_local_addr_collision_rejected(void)
{
    gw_app_t app;
    gw_port_t port;
    mb_m_poll_item_t it;
    uint8_t  req[8];
    uint16_t crc;
    const mb_m_slave_stat_t *ss;

    printf("\n[4] 轮询地址 = 本机从站地址：必须先被拦下\n");

    /* ---- (a) 先看"不拦会怎样"：用引擎接口直接登记，模拟以前的做法 ---- */
    reset_env();
    make_port(&port);
    if (gw_app_init(&app, &SLAVE, &port) != 0) {
        fail("gw_app_init 成功", "");
        return;
    }

    memset(&it, 0, sizeof(it));
    it.slave    = 1u;                   /* ⚠️ 与本机从站地址 1 相同 */
    it.func     = 0x04u;
    it.qty      = 4u;
    it.map_base = 8u;
    it.map_area = 1u;
    (void)mb_master_add_item(gw_app_master(&app), &it);   /* 故意绕过守卫 */

    gw_app_poll(&app, 0u);
    check_int("(说明)主站已在等 1 号从机的响应",
              (int)app.master.awaiting, 1);

    /* 上位机（地址 1）发来的**读请求**：站号 / CRC / 功能码全部"对得上" */
    req[0] = 1u; req[1] = 0x04u;
    req[2] = 0u; req[3] = 0u; req[4] = 0u; req[5] = 4u;
    crc = mb_crc16(req, 6u);
    req[6] = (uint8_t)(crc & 0xFFu);
    req[7] = (uint8_t)(crc >> 8);
    memcpy(g_rx, req, 8u);
    g_rx_len = 8u;

    gw_app_poll(&app, 0u);

    ss = mb_master_slave_stat(gw_app_master(&app), 1u);
    check_int("(说明)这条请求被记成了 1 号从机的失败",
              (ss != 0) ? (int)ss->cnt_fail : -1, 1);

    /* ---- (b) 正门：配置入口必须拒绝这个组合 ---- */
    reset_env();
    make_port(&port);
    if (gw_app_init(&app, &SLAVE, &port) != 0) {
        fail("gw_app_init 成功", "");
        return;
    }

    it.slave = 1u;                      /* 同地址：必须被拒 */
    check_int("同地址配置被拒绝", gw_app_master_add_item(&app, &it),
              GW_APP_ITEM_ERR_LOCAL);
    check_int("被拒后一项都没登记上", (int)app.master.item_count, 0);

    it.slave = 2u;                      /* 正确配置：接受 */
    check_int("换成不同地址即可正常登记",
              gw_app_master_add_item(&app, &it), GW_APP_ITEM_OK);
    check_int("登记后轮询项 = 1", (int)app.master.item_count, 1);

    check_int("空指针被拒", gw_app_master_add_item(&app, 0),
              GW_APP_ITEM_ERR_PARAM);
}

/* ==================================================================
 *  用例 5：移植层"申请到了，但持有的不是主站身份"
 *
 *  ⚠️ 这条守护的是 2026-10-10 用 SWD 读内存才查出来的真缺陷：
 *
 *     模板把 vtable 的 rs485_bus_acquire 配成了申请**从站**角色，
 *     而 rs485_send_held 按**主站**身份发送。两者叠加就是"申请两次"：
 *     第二次（主站申请）只有在总线完全空闲时才成功 —— 必然 BUSY。
 *
 *     上板实测的后果：主站重试 46873 次、移植层 tx 计数**一直是 0**
 *     （一帧都没上总线），仲裁器的 owner 永久停在 SLAVE，
 *     于是从站自己发响应也拿不到总线 —— **启用主站后从站也不再应答**；
 *     主循环每轮都在重试，CPU 拉到 1000‰。
 *
 *     为什么原来的单测抓不到：假移植层 acquire 和 send_held 都返回成功，
 *     "两次申请"这件事根本没有被建模。
 *
 *  判据：应用层必须**察觉**并计数（master_not_held），而不是假装发送成功。
 * ================================================================== */
static void test_master_role_mismatch_detected(void)
{
    gw_app_t app;
    gw_port_t port;
    mb_m_poll_item_t it;

    printf("\n[5] 申请到的是从站身份 -> 必须计数，不能静默\n");

    reset_env();
    make_port(&port);
    if (gw_app_init(&app, &SLAVE, &port) != 0) {
        fail("gw_app_init 成功", "");
        return;
    }

    memset(&it, 0, sizeof(it));
    it.slave    = 2u;
    it.func     = 0x04u;
    it.qty      = 4u;
    it.map_base = 8u;
    it.map_area = 1u;
    (void)gw_app_master_add_item(&app, &it);

    g_holds_ok = 0u;                /* 模拟"配错角色"的移植层 */
    g_tx_n     = 0;

    gw_app_poll(&app, 0u);

    check_int("识别出未持有主站身份", (int)app.master_not_held, 1);
    check_int("不假装发送成功（一帧都没出去）", g_tx_n, 0);
}

/* ==================================================================
 *  用例 6：超时**重发**时不能再申请一次总线（否则重发永远发不出去）
 *
 *  ⚠️ 这是 2026-10-10 从**总线上的帧间隔**抓到的缺陷：
 *
 *     实测帧间隔恒为 1.30 秒 = 300ms(超时) + 1000ms(轮询节拍)，
 *     而按设计应当出现"重发帧在 300ms 后再来一次"的成对形态。
 *     也就是说 max_retry=1 从来没生效过。
 *
 *     原因：重发发生在**同一次事务内**，此时主站还持着总线
 *     （等响应期间不能放权），而仲裁规则是"主站只有总线完全空闲才拿得到"
 *     —— 同一个 owner 再申请一样 BUSY，于是重发在移植层被判失败。
 *
 *  ⚠️ 判据必须是"事务还没结束（awaiting）"，**不能**是"当前是否持有总线"：
 *     一轮正常轮询结束到下一轮开始之间总线可能还握在手里，
 *     用后者会把正常请求也当成重发，于是**总线永远不释放**
 *     （test_app_bus 的 3 条断言当场抓到了这个写法）。
 * ================================================================== */
static void test_resend_while_holding_bus(void)
{
    gw_app_t app;
    gw_port_t port;
    mb_m_poll_item_t it;

    printf("\n[6] 超时重发不该二次申请总线\n");

    reset_env();
    make_port(&port);
    if (gw_app_init(&app, &SLAVE, &port) != 0) {
        fail("gw_app_init 成功", "");
        return;
    }

    memset(&it, 0, sizeof(it));
    it.slave    = 2u;
    it.func     = 0x04u;
    it.qty      = 4u;
    it.map_base = 8u;
    it.map_area = 1u;
    (void)gw_app_master_add_item(&app, &it);

    /* 第一次发送：正常申请到总线并进入等待 */
    gw_app_poll(&app, 0u);
    check_int("第一次发送成功", g_tx_n, 1);
    check_int("已进入等待响应", (int)app.master.awaiting, 1);

    /* ⚠️ 模拟"仲裁层仍认为总线归主站所有，再申请必然失败"的状态 */
    g_acq_fail = 1u;
    g_tx_n     = 0;

    /* > timeout_ms(默认 300ms) -> 触发超时重发 */
    gw_app_poll(&app, 350u);

    check_int("重发帧正常发出（没有再去申请一次）", g_tx_n, 1);
    check_int("没有把它记成「总线被占而推迟」", (int)app.master_deferred, 0);
}

int main(void)
{
    printf("========== 主站响应派发（上板缺陷的回归） ==========\n");

    test_response_reaches_master();
    test_slave_request_not_swallowed();
    test_normal_slave_path_unaffected();
    test_local_addr_collision_rejected();
    test_master_role_mismatch_detected();
    test_resend_while_holding_bus();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
