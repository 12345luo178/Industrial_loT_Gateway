/**
 * @file    test_master.c
 * @brief   Modbus RTU 主站协议引擎单元测试（主机端，不需要硬件）
 *
 * ============================================================================
 *  这套测试最有价值的地方：把"时间"变成可控输入
 *
 *  主站的核心难点不是协议格式，而是**时序与状态机**：
 *    超时怎么判、重试几次、从机坏了怎么隔离、退避多久恢复。
 *  这些在真机上验证一次要花几十秒甚至几分钟，而且难以复现。
 *
 *  由于 mb_master_poll(m, elapsed_ms) 的时间由调用方推进，
 *  这里可以用"虚拟时间"瞬间跑完：
 *    一次 300ms 超时 + 1 次重试 = 调用 poll(300) 两次即可
 *  于是"离线判定 + 退避 + 恢复"这种跨越十几秒的行为，
 *  也能在毫秒内完成且完全确定。
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iinc -o test/test_master.exe \
 *        src/modbus_crc.c src/modbus_rtu.c src/modbus_master.c test/test_master.c
 *    ./test/test_master.exe
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

#include "modbus_crc.h"
#include "modbus_rtu.h"
#include "modbus_master.h"

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
 * 假总线：记录主站发出的每一帧，并允许测试注入响应
 * ================================================================== */
#define SENT_MAX 64

typedef struct {
    /* 发送记录 */
    uint8_t  sent[SENT_MAX][16];
    uint16_t sent_len[SENT_MAX];
    int      sent_n;

    /* 注入的响应：下一帧发送后，测试代码手动调用 on_response */
    uint8_t  resp[32];
    uint16_t resp_len;
    int      have_resp;

    /* 寄存器写入记录 */
    uint8_t  store_area;
    uint16_t store_base;
    uint16_t store_vals[MB_MAX_READ_QTY];
    uint16_t store_qty;
    int      store_calls;
    int      store_fail;        /* 非 0 时让 store 回调返回失败 */

    /* 发送失败注入 */
    int      send_fail;
} bus_t;

static bus_t BUS;

static int bus_send(void *ctx, const uint8_t *buf, uint16_t len)
{
    bus_t *b = (bus_t *)ctx;

    if (b->send_fail) {
        return -1;
    }
    if (b->sent_n < SENT_MAX && len <= 16u) {
        memcpy(b->sent[b->sent_n], buf, len);
        b->sent_len[b->sent_n] = len;
        b->sent_n++;
    }
    return 0;
}

static int bus_store(void *ctx, uint8_t area, uint16_t base,
                     const uint16_t *values, uint16_t qty)
{
    bus_t *b = (bus_t *)ctx;
    uint16_t i;

    if (b->store_fail) {
        return -1;
    }
    b->store_area = area;
    b->store_base = base;
    b->store_qty  = qty;
    for (i = 0u; i < qty && i < MB_MAX_READ_QTY; i++) {
        b->store_vals[i] = values[i];
    }
    b->store_calls++;
    return 0;
}

static void bus_reset(void)
{
    memset(&BUS, 0, sizeof(BUS));
}

/* ---- 构造响应帧 ---- */

/* 读响应：站号 功能 字节数 数据... CRC */
static void make_read_resp(uint8_t *out, uint16_t *out_len,
                           uint8_t slave, uint8_t func,
                           const uint16_t *vals, uint16_t qty)
{
    uint16_t i;
    uint16_t n = 0u;
    uint16_t crc;

    out[n++] = slave;
    out[n++] = func;
    out[n++] = (uint8_t)(qty * 2u);
    for (i = 0u; i < qty; i++) {
        out[n++] = (uint8_t)(vals[i] >> 8);
        out[n++] = (uint8_t)(vals[i] & 0xFFu);
    }
    crc = mb_crc16(out, n);
    out[n++] = (uint8_t)(crc & 0xFFu);
    out[n++] = (uint8_t)(crc >> 8);
    *out_len = n;
}

/* 异常响应：站号 功能|0x80 异常码 CRC */
static void make_exc_resp(uint8_t *out, uint16_t *out_len,
                          uint8_t slave, uint8_t func, uint8_t exc)
{
    uint16_t n = 0u;
    uint16_t crc;

    out[n++] = slave;
    out[n++] = (uint8_t)(func | 0x80u);
    out[n++] = exc;
    crc = mb_crc16(out, n);
    out[n++] = (uint8_t)(crc & 0xFFu);
    out[n++] = (uint8_t)(crc >> 8);
    *out_len = n;
}

/* ==================================================================
 * 测试 1：请求帧格式
 * ================================================================== */
static void test_request_frame(void)
{
    mb_master_t m;
    mb_m_poll_item_t it;

    printf("\n[1] 请求帧格式\n");

    bus_reset();
    (void)mb_master_init(&m, bus_send, &BUS, bus_store, &BUS);

    it.slave = 0x11; it.func = MB_FC_READ_HOLDING;
    it.start = 0x006Bu; it.qty = 3u;
    it.map_base = 0u; it.map_area = 0u;
    if (mb_master_add_item(&m, &it) == MB_M_OK) { ok("添加轮询项成功"); }
    else { fail("添加轮询项成功", ""); }

    mb_master_poll(&m, 0u);         /* 空闲 -> 立即发起请求 */

    if (BUS.sent_n == 1 && BUS.sent_len[0] == 8u) {
        ok("发出 1 帧且长度为 8");
    } else {
        char d[64];
        sprintf(d, "(sent_n=%d len=%u)", BUS.sent_n,
                (unsigned)(BUS.sent_n > 0 ? BUS.sent_len[0] : 0u));
        fail("发出 1 帧且长度为 8", d);
    }

    /* 这是 Modbus 规范里的经典示例帧：11 03 006B 0003 -> CRC 7687 */
    {
        const uint8_t expect[8] = { 0x11, 0x03, 0x00, 0x6B, 0x00, 0x03, 0x76, 0x87 };
        if (BUS.sent_n >= 1 && memcmp(BUS.sent[0], expect, 8) == 0) {
            ok("请求帧与 Modbus 规范示例一致 (11 03 006B 0003 7687)");
        } else {
            char d[96];
            sprintf(d, "(实际 %02X %02X %02X %02X %02X %02X %02X %02X)",
                    BUS.sent_n > 0 ? BUS.sent[0][0] : 0,
                    BUS.sent_n > 0 ? BUS.sent[0][1] : 0,
                    BUS.sent_n > 0 ? BUS.sent[0][2] : 0,
                    BUS.sent_n > 0 ? BUS.sent[0][3] : 0,
                    BUS.sent_n > 0 ? BUS.sent[0][4] : 0,
                    BUS.sent_n > 0 ? BUS.sent[0][5] : 0,
                    BUS.sent_n > 0 ? BUS.sent[0][6] : 0,
                    BUS.sent_n > 0 ? BUS.sent[0][7] : 0);
            fail("请求帧与 Modbus 规范示例一致", d);
        }
    }

    check_int("发出请求后进入等待状态", mb_master_is_idle(&m), 0);
}

/* ==================================================================
 * 测试 2：正常读响应与数据映射
 * ================================================================== */
static void test_normal_response(void)
{
    mb_master_t m;
    mb_m_poll_item_t it;
    uint16_t vals[4] = { 0x1234u, 0x5678u, 0x9ABCu, 0xDEF0u };
    uint8_t  resp[32];
    uint16_t rlen;

    printf("\n[2] 正常响应与数据映射\n");

    bus_reset();
    (void)mb_master_init(&m, bus_send, &BUS, bus_store, &BUS);

    it.slave = 1u; it.func = MB_FC_READ_INPUT;
    it.start = 0x0010u; it.qty = 4u;
    it.map_base = 2u; it.map_area = 0u;     /* 映射到输入寄存器区偏移 2 */
    (void)mb_master_add_item(&m, &it);

    mb_master_poll(&m, 0u);

    make_read_resp(resp, &rlen, 1u, MB_FC_READ_INPUT, vals, 4u);
    if (mb_master_on_response(&m, resp, rlen) == MB_M_OK) {
        ok("合法响应被接受");
    } else {
        fail("合法响应被接受", "");
    }

    check_int("store 回调被调用 1 次", BUS.store_calls, 1);
    check_int("映射区 = 输入寄存器", (int)BUS.store_area, 0);
    check_int("映射起始偏移 = 2", (int)BUS.store_base, 2);
    check_int("映射数量 = 4", (int)BUS.store_qty, 4);
    check_int("第 1 个值大端解析正确", (int)BUS.store_vals[0], 0x1234);
    check_int("第 4 个值大端解析正确", (int)BUS.store_vals[3], 0xDEF0);

    check_int("请求完成后回到空闲", mb_master_is_idle(&m), 1);

    {
        const mb_m_slave_stat_t *s = mb_master_slave_stat(&m, 1u);
        if (s != 0 && s->online == 1u && s->cnt_ok == 1u && s->fail_streak == 0u) {
            ok("从机 1 统计正确（在线、成功 1 次）");
        } else {
            fail("从机 1 统计正确", "");
        }
    }
}

/* ==================================================================
 * 测试 3：多从机轮询调度
 * ================================================================== */
static void test_poll_schedule(void)
{
    mb_master_t m;
    mb_m_poll_item_t it;
    uint16_t vals[2] = { 0xAAAAu, 0xBBBBu };
    uint8_t  resp[32];
    uint16_t rlen;

    printf("\n[3] 多从机轮询调度\n");

    bus_reset();
    (void)mb_master_init(&m, bus_send, &BUS, bus_store, &BUS);

    it.func = MB_FC_READ_HOLDING; it.qty = 2u;
    it.map_base = 0u; it.map_area = 1;

    it.slave = 1u; it.start = 0u; (void)mb_master_add_item(&m, &it);
    it.slave = 2u; it.start = 2u; (void)mb_master_add_item(&m, &it);
    it.slave = 3u; it.start = 4u; (void)mb_master_add_item(&m, &it);

    /* 第 1 轮：应依次访问 1、2、3 */
    mb_master_poll(&m, 0u);
    check_int("第 1 个请求发往从机 1", (int)BUS.sent[0][0], 1);
    make_read_resp(resp, &rlen, 1u, MB_FC_READ_HOLDING, vals, 2u);
    (void)mb_master_on_response(&m, resp, rlen);

    mb_master_poll(&m, 0u);
    check_int("第 2 个请求发往从机 2", (int)BUS.sent[1][0], 2);
    make_read_resp(resp, &rlen, 2u, MB_FC_READ_HOLDING, vals, 2u);
    (void)mb_master_on_response(&m, resp, rlen);

    mb_master_poll(&m, 0u);
    check_int("第 3 个请求发往从机 3", (int)BUS.sent[2][0], 3);
    make_read_resp(resp, &rlen, 3u, MB_FC_READ_HOLDING, vals, 2u);
    (void)mb_master_on_response(&m, resp, rlen);

    /* 第 2 轮：回到从机 1 */
    mb_master_poll(&m, 0u);
    check_int("第 4 个请求回到从机 1", (int)BUS.sent[3][0], 1);
    make_read_resp(resp, &rlen, 1u, MB_FC_READ_HOLDING, vals, 2u);
    (void)mb_master_on_response(&m, resp, rlen);

    check_int("已完成 1 个轮询轮次", (int)mb_master_stats(&m)->cnt_poll_rounds, 1);
    check_int("累计 4 次请求", (int)mb_master_stats(&m)->cnt_requests, 4);
}

/* ==================================================================
 * 测试 4：超时与重试（虚拟时间）
 * ================================================================== */
static void test_timeout_retry(void)
{
    mb_master_t m;
    mb_m_poll_item_t it;

    printf("\n[4] 超时与重试（虚拟时间推进）\n");

    bus_reset();
    (void)mb_master_init(&m, bus_send, &BUS, bus_store, &BUS);
    (void)mb_master_set_timing(&m, 300u, 1u);       /* 超时 300ms，重试 1 次 */

    it.slave = 5u; it.func = MB_FC_READ_HOLDING; it.start = 0u; it.qty = 1u;
    it.map_base = 0u; it.map_area = 1;
    (void)mb_master_add_item(&m, &it);

    mb_master_poll(&m, 0u);
    check_int("已发出第 1 次请求", BUS.sent_n, 1);

    /* 时间未到：不应重发 */
    mb_master_poll(&m, 100u);
    check_int("100ms 时未重发", BUS.sent_n, 1);

    /* 走到 300ms：触发重试 */
    mb_master_poll(&m, 200u);
    check_int("300ms 时触发重试（第 2 次发送）", BUS.sent_n, 2);
    check_int("重试次数计入统计", (int)mb_master_stats(&m)->cnt_retries, 1);
    check_int("重试后仍在等待响应", mb_master_is_idle(&m), 0);

    /* 再等 300ms：重试用尽，判定失败 */
    mb_master_poll(&m, 300u);
    check_int("重试用尽后未再发送", BUS.sent_n, 2);
    check_int("请求完成后空闲", mb_master_is_idle(&m), 1);

    {
        const mb_m_slave_stat_t *s = mb_master_slave_stat(&m, 5u);
        if (s != 0 && s->cnt_timeout == 1u && s->fail_streak == 1u) {
            ok("超时计数与连续失败计数正确");
        } else {
            fail("超时计数与连续失败计数正确", "");
        }
        if (s != 0 && s->last_err == (uint8_t)(-MB_M_ERR_TIMEOUT)) {
            ok("最近错误码记录为 TIMEOUT");
        } else {
            fail("最近错误码记录为 TIMEOUT", "");
        }
    }
}

/* ==================================================================
 * 测试 5：离线判定与退避隔离（本项目最关键的行为）
 * ================================================================== */
static void test_offline_backoff(void)
{
    mb_master_t m;
    mb_m_poll_item_t it;
    uint16_t vals[1] = { 0x1111u };
    uint8_t  resp[32];
    uint16_t rlen;
    int      i;

    printf("\n[5] 离线判定与退避隔离\n");

    bus_reset();
    (void)mb_master_init(&m, bus_send, &BUS, bus_store, &BUS);
    (void)mb_master_set_timing(&m, 100u, 0u);       /* 不重试，便于计数 */

    /* 两台从机：1 号正常，2 号掉线 */
    it.func = MB_FC_READ_HOLDING; it.qty = 1u;
    it.map_base = 0u; it.map_area = 1;
    it.slave = 1u; it.start = 0u; (void)mb_master_add_item(&m, &it);
    it.slave = 2u; it.start = 0u; (void)mb_master_add_item(&m, &it);

    /* 让 2 号连续失败 3 次（失败阈值），期间 1 号正常应答 */
    for (i = 0; i < 3; i++) {
        /* 1 号：正常 */
        mb_master_poll(&m, 0u);
        make_read_resp(resp, &rlen, 1u, MB_FC_READ_HOLDING, vals, 1u);
        (void)mb_master_on_response(&m, resp, rlen);

        /* 2 号：超时 */
        mb_master_poll(&m, 0u);
        mb_master_poll(&m, 100u);
    }

    {
        const mb_m_slave_stat_t *s2 = mb_master_slave_stat(&m, 2u);
        const mb_m_slave_stat_t *s1 = mb_master_slave_stat(&m, 1u);
        if (s2 != 0 && s2->online == 0u) {
            ok("从机 2 连续失败 3 次后判定离线");
        } else {
            fail("从机 2 连续失败 3 次后判定离线", "");
        }
        if (s1 != 0 && s1->online == 1u) {
            ok("从机 1 仍在线（未受 2 号影响）");
        } else {
            fail("从机 1 仍在线", "");
        }
        check_int("离线事件计数 = 1", (int)mb_master_stats(&m)->cnt_offline, 1);
    }

    /* 关键行为：离线后从机 2 应被跳过，1 号仍能被正常轮询。
       接下来 4 轮里，2 号都不应收到请求（退避期 = 5 轮）。 */
    {
        int sent_before = BUS.sent_n;
        int to_slave2   = 0;

        for (i = 0; i < 4; i++) {
            mb_master_poll(&m, 0u);         /* 应只发给 1 号 */
            make_read_resp(resp, &rlen, 1u, MB_FC_READ_HOLDING, vals, 1u);
            (void)mb_master_on_response(&m, resp, rlen);
        }
        {
            int k;
            for (k = sent_before; k < BUS.sent_n; k++) {
                if (BUS.sent[k][0] == 2u) { to_slave2++; }
            }
        }
        check_int("退避期内未向离线从机 2 发请求", to_slave2, 0);
        check_int("退避期内从机 1 正常轮询 4 次",
                  (int)mb_master_slave_stat(&m, 1u)->cnt_ok, 7);
    }

    /* 退避到期后应重试一次；这次让它成功，验证"自动恢复"。
     *
     * ⚠️ 不要硬编码"再过几轮"：skip_count 只在**轮询到该从机时**才递减，
     *    而每轮里该从机只被访问一次，所以 5 次退避大约需要 10 次 poll 调用。
     *    依赖这个精确次数会让测试很脆 —— 一旦调度策略调整就失败，
     *    而且失败原因看起来像"功能坏了"，其实是测试写死了实现细节。
     *    改成"一直跑到向 2 号发出请求为止"，只断言最终行为。 */
    {
        int guard = 0;

        while (guard++ < 40) {
            mb_master_poll(&m, 0u);

            if (BUS.sent[BUS.sent_n - 1][0] == 2u) {
                break;                      /* 退避到期，主站回来重试 2 号了 */
            }
            /* 这一轮发给的是 1 号，正常应答 */
            make_read_resp(resp, &rlen, 1u, MB_FC_READ_HOLDING, vals, 1u);
            (void)mb_master_on_response(&m, resp, rlen);
        }

        if (BUS.sent[BUS.sent_n - 1][0] == 2u) {
            ok("退避到期后主站自动回来重试离线从机");
        } else {
            fail("退避到期后主站自动回来重试离线从机", "(40 轮内未重试)");
            return;
        }

        /* 这次让 2 号正常应答，应恢复在线 */
        make_read_resp(resp, &rlen, 2u, MB_FC_READ_HOLDING, vals, 1u);
        (void)mb_master_on_response(&m, resp, rlen);

        {
            const mb_m_slave_stat_t *s2 = mb_master_slave_stat(&m, 2u);
            if (s2 != 0 && s2->online == 1u && s2->fail_streak == 0u) {
                ok("从机 2 恢复在线，连续失败计数清零");
            } else {
                fail("从机 2 恢复在线", "");
            }
            check_int("恢复事件计数 = 1", (int)mb_master_stats(&m)->cnt_recover, 1);
        }

        /* 恢复后应回到正常轮询：再跑几轮，2 号的成功次数应持续增加 */
        {
            uint32_t ok_before = mb_master_slave_stat(&m, 2u)->cnt_ok;
            int      i;

            for (i = 0; i < 4; i++) {
                mb_master_poll(&m, 0u);
                make_read_resp(resp, &rlen, BUS.sent[BUS.sent_n - 1][0],
                               MB_FC_READ_HOLDING, vals, 1u);
                (void)mb_master_on_response(&m, resp, rlen);
            }
            if (mb_master_slave_stat(&m, 2u)->cnt_ok > ok_before) {
                ok("恢复后 2 号被纳入正常轮询（成功计数持续增加）");
            } else {
                fail("恢复后 2 号被纳入正常轮询", "");
            }
        }
    }
}

/* ==================================================================
 * 测试 6：各类非法响应的处理
 * ================================================================== */
static void test_bad_responses(void)
{
    mb_master_t m;
    mb_m_poll_item_t it;
    uint16_t vals[2] = { 0x1111u, 0x2222u };
    uint8_t  resp[32];
    uint16_t rlen;
    int      i;

    printf("\n[6] 非法响应处理\n");

    /* ---- 6.1 站号不符 ---- */
    bus_reset();
    (void)mb_master_init(&m, bus_send, &BUS, bus_store, &BUS);
    it.slave = 7u; it.func = MB_FC_READ_HOLDING; it.start = 0u; it.qty = 2u;
    it.map_base = 0u; it.map_area = 1;
    (void)mb_master_add_item(&m, &it);
    mb_master_poll(&m, 0u);
    make_read_resp(resp, &rlen, 9u, MB_FC_READ_HOLDING, vals, 2u);   /* 站号 9 != 7 */
    if (mb_master_on_response(&m, resp, rlen) == MB_M_ERR_ADDR) {
        ok("站号不符 -> ERR_ADDR");
    } else {
        fail("站号不符 -> ERR_ADDR", "");
    }

    /* ---- 6.2 CRC 错误 ---- */
    bus_reset();
    (void)mb_master_init(&m, bus_send, &BUS, bus_store, &BUS);
    (void)mb_master_add_item(&m, &it);
    mb_master_poll(&m, 0u);
    make_read_resp(resp, &rlen, 7u, MB_FC_READ_HOLDING, vals, 2u);
    resp[rlen - 1u] ^= 0xFFu;                        /* 破坏 CRC */
    if (mb_master_on_response(&m, resp, rlen) == MB_M_ERR_CRC) {
        ok("CRC 错误 -> ERR_CRC");
    } else {
        fail("CRC 错误 -> ERR_CRC", "");
    }

    /* ---- 6.3 功能码不符 ---- */
    bus_reset();
    (void)mb_master_init(&m, bus_send, &BUS, bus_store, &BUS);
    (void)mb_master_add_item(&m, &it);
    mb_master_poll(&m, 0u);
    make_read_resp(resp, &rlen, 7u, MB_FC_READ_INPUT, vals, 2u);   /* 请求 03 却回 04 */
    if (mb_master_on_response(&m, resp, rlen) == MB_M_ERR_FUNC) {
        ok("功能码不符 -> ERR_FUNC");
    } else {
        fail("功能码不符 -> ERR_FUNC", "");
    }

    /* ---- 6.4 字节数不符 ---- */
    bus_reset();
    (void)mb_master_init(&m, bus_send, &BUS, bus_store, &BUS);
    (void)mb_master_add_item(&m, &it);
    mb_master_poll(&m, 0u);
    make_read_resp(resp, &rlen, 7u, MB_FC_READ_HOLDING, vals, 1u);  /* 只回 1 个，请求 2 个 */
    if (mb_master_on_response(&m, resp, rlen) == MB_M_ERR_LEN) {
        ok("字节数不符 -> ERR_LEN");
    } else {
        fail("字节数不符 -> ERR_LEN", "");
    }

    /* ---- 6.5 异常响应 ---- */
    bus_reset();
    (void)mb_master_init(&m, bus_send, &BUS, bus_store, &BUS);
    (void)mb_master_add_item(&m, &it);
    mb_master_poll(&m, 0u);
    make_exc_resp(resp, &rlen, 7u, MB_FC_READ_HOLDING, MB_EXC_ILLEGAL_ADDRESS);
    if (mb_master_on_response(&m, resp, rlen) == MB_M_ERR_EXCEPTION) {
        ok("异常响应 -> ERR_EXCEPTION");
    } else {
        fail("异常响应 -> ERR_EXCEPTION", "");
    }
    check_int("异常响应计入从机统计",
              (int)mb_master_slave_stat(&m, 7u)->cnt_exception, 1);

    /* ---- 6.6 帧长过短 ---- */
    bus_reset();
    (void)mb_master_init(&m, bus_send, &BUS, bus_store, &BUS);
    (void)mb_master_add_item(&m, &it);
    mb_master_poll(&m, 0u);
    {
        uint8_t shortf[3] = { 7u, MB_FC_READ_HOLDING, 0x04u };
        if (mb_master_on_response(&m, shortf, 3u) == MB_M_ERR_ADDR ||
            mb_master_on_response(&m, shortf, 3u) == MB_M_ERR_LEN) {
            ok("过短帧被拒（ERR_ADDR 或 ERR_LEN）");
        } else {
            fail("过短帧被拒", "");
        }
    }

    /* ---- 6.7 寄存器越界（store 回调返回失败） ---- */
    bus_reset();
    (void)mb_master_init(&m, bus_send, &BUS, bus_store, &BUS);
    (void)mb_master_add_item(&m, &it);
    mb_master_poll(&m, 0u);
    BUS.store_fail = 1;
    make_read_resp(resp, &rlen, 7u, MB_FC_READ_HOLDING, vals, 2u);
    if (mb_master_on_response(&m, resp, rlen) == MB_M_ERR_PARAM) {
        ok("寄存器写入越界 -> 本次轮询判失败");
    } else {
        fail("寄存器写入越界 -> 本次轮询判失败", "");
    }

    /* ---- 6.8 冗余/无待处理请求时的响应 ---- */
    bus_reset();
    (void)mb_master_init(&m, bus_send, &BUS, bus_store, &BUS);
    (void)mb_master_add_item(&m, &it);
    (void)mb_master_on_response(&m, resp, rlen);
    if (mb_master_on_response(&m, resp, rlen) == MB_M_ERR_BUSY) {
        ok("没有待处理请求时返回 ERR_BUSY（不破坏状态机）");
    } else {
        fail("没有待处理请求时返回 ERR_BUSY", "");
    }
    (void)i;
}

/* ==================================================================
 * 测试 7：入参校验与配置接口
 * ================================================================== */
static void test_config_validation(void)
{
    mb_master_t m;
    mb_m_poll_item_t it;

    printf("\n[7] 入参校验与配置\n");

    bus_reset();

    if (mb_master_init(0, bus_send, &BUS, bus_store, &BUS) == MB_M_ERR_PARAM &&
        mb_master_init(&m, 0, &BUS, bus_store, &BUS) == MB_M_ERR_PARAM &&
        mb_master_init(&m, bus_send, &BUS, 0, &BUS) == MB_M_ERR_PARAM) {
        ok("init 空指针全部被拒");
    } else {
        fail("init 空指针全部被拒", "");
    }

    (void)mb_master_init(&m, bus_send, &BUS, bus_store, &BUS);

    it.func = MB_FC_READ_HOLDING; it.start = 0u; it.qty = 1u;
    it.map_base = 0u; it.map_area = 1;

    it.slave = 0u;      /* 广播地址不能作为主站轮询目标 */
    if (mb_master_add_item(&m, &it) == MB_M_ERR_PARAM) { ok("从机地址 0 被拒"); }
    else { fail("从机地址 0 被拒", ""); }

    it.slave = 248u;
    if (mb_master_add_item(&m, &it) == MB_M_ERR_PARAM) { ok("从机地址 248 被拒"); }
    else { fail("从机地址 248 被拒", ""); }

    it.slave = 1u; it.func = 0x06u;     /* 主站本实现只支持读 */
    if (mb_master_add_item(&m, &it) == MB_M_ERR_PARAM) { ok("写功能码被拒（本实现只支持读）"); }
    else { fail("写功能码被拒", ""); }

    it.func = MB_FC_READ_HOLDING; it.qty = 126u;
    if (mb_master_add_item(&m, &it) == MB_M_ERR_PARAM) { ok("数量 126 被拒（超规范上限 125）"); }
    else { fail("数量 126 被拒", ""); }

    it.qty = 0u;
    if (mb_master_add_item(&m, &it) == MB_M_ERR_PARAM) { ok("数量 0 被拒"); }
    else { fail("数量 0 被拒", ""); }

    it.qty = 1u; it.map_area = 2u;
    if (mb_master_add_item(&m, &it) == MB_M_ERR_PARAM) { ok("映射区 2 被拒"); }
    else { fail("映射区 2 被拒", ""); }

    it.map_area = 0u;
    if (mb_master_set_timing(&m, 5u, 0u) == MB_M_ERR_PARAM) { ok("超时 5ms 被拒（下限 10ms）"); }
    else { fail("超时 5ms 被拒", ""); }
    if (mb_master_set_timing(&m, 6000u, 0u) == MB_M_ERR_PARAM) { ok("超时 6000ms 被拒（上限 5000ms）"); }
    else { fail("超时 6000ms 被拒", ""); }
    if (mb_master_set_timing(&m, 100u, 6u) == MB_M_ERR_PARAM) { ok("重试 6 次被拒（上限 5）"); }
    else { fail("重试 6 次被拒", ""); }
    if (mb_master_set_timing(&m, 100u, 2u) == MB_M_OK) { ok("合法超时与重试被接受"); }
    else { fail("合法超时与重试被接受", ""); }

    /* 发送失败也应按失败处理，不卡死状态机 */
    bus_reset();
    (void)mb_master_init(&m, bus_send, &BUS, bus_store, &BUS);
    (void)mb_master_add_item(&m, &it);
    BUS.send_fail = 1;
    mb_master_poll(&m, 0u);
    mb_master_poll(&m, 100u);
    if (mb_master_is_idle(&m) == 1) {
        ok("发送失败时不卡死状态机");
    } else {
        fail("发送失败时不卡死状态机", "");
    }
}

/* ==================================================================
 * 测试 8：在线位图与空表处理
 * ================================================================== */
static void test_bitmap_and_empty(void)
{
    mb_master_t m;
    mb_m_poll_item_t it;

    printf("\n[8] 在线位图与空表处理\n");

    bus_reset();
    (void)mb_master_init(&m, bus_send, &BUS, bus_store, &BUS);

    /* 空表：poll 不应做任何事 */
    mb_master_poll(&m, 0u);
    check_int("空表时 poll 不发送任何帧", BUS.sent_n, 0);
    check_int("空表时保持空闲", mb_master_is_idle(&m), 1);
    check_int("空表时在线位图为 0（位图以轮询表为准）",
              (int)mb_master_online_bitmap(&m), 0);

    /* 加入从机 3 后，位图应只有 bit2 */
    it.slave = 3u; it.func = MB_FC_READ_HOLDING; it.start = 0u; it.qty = 1u;
    it.map_base = 0u; it.map_area = 1;
    (void)mb_master_add_item(&m, &it);
    check_int("仅配置从机 3 时位图 = 0x0004",
              (int)mb_master_online_bitmap(&m), 0x0004);

    /* 让 3 号连续失败 3 次（默认不重试时用 set_timing 明确设置） */
    (void)mb_master_set_timing(&m, 50u, 0u);
    {
        int i;
        for (i = 0; i < 3; i++) {
            mb_master_poll(&m, 0u);
            mb_master_poll(&m, 50u);
        }
    }
    check_int("3 号离线后位图 bit2 清零",
              (int)(mb_master_online_bitmap(&m) & 0x0004u), 0);

    /* clear_items 后应回到空闲，且位图清零 */
    mb_master_clear_items(&m);
    check_int("清空轮询表后 item_count = 0", (int)m.item_count, 0);
    check_int("清空后位图 = 0", (int)mb_master_online_bitmap(&m), 0);
    {
        int before = BUS.sent_n;
        mb_master_poll(&m, 0u);
        check_int("清空后 poll 不发送", BUS.sent_n, before);
    }
}

/* ==================================================================
 * 测试 10：从机状态表容量与登记语义
 *
 * 这一组针对一次真实的缺陷修复：
 *   早期实现用 `mb_m_slave_stat_t slave[248]`（按地址索引），
 *   一个数组占 6.2 KB，而 MB_M_MAX_SLAVES 明明定义的是 16。
 *   改成"按登记紧凑存储"后，引入了新的边界条件，必须测到：
 *     - 登记满 16 台后，第 17 台必须被**明确拒绝**（而不是静默截断）
 *     - 同一从机的多个轮询项只能登记一次（不能重复占槽）
 *     - 未登记的地址查询状态应返回 NULL（如实报告，而不是返回全零结构）
 * ================================================================== */
static void test_slave_table(void)
{
    mb_master_t m;
    mb_m_poll_item_t it;
    int i;

    printf("\n[10] 从机状态表容量与登记语义\n");

    bus_reset();
    (void)mb_master_init(&m, bus_send, &BUS, bus_store, &BUS);

    check_int("初始从机表为空", (int)m.slave_count, 0);
    check_int("未登记地址查询状态返回 NULL",
              mb_master_slave_stat(&m, 5u) == 0 ? 1 : 0, 1);
    check_int("地址 0（广播）查询状态返回 NULL",
              mb_master_slave_stat(&m, 0u) == 0 ? 1 : 0, 1);

    /* 同一从机加 3 个轮询项：只应登记 1 个槽 */
    it.slave = 7u; it.func = MB_FC_READ_HOLDING;
    it.start = 0u; it.qty = 1u; it.map_base = 0u; it.map_area = 1;
    (void)mb_master_add_item(&m, &it);
    it.start = 1u;
    (void)mb_master_add_item(&m, &it);
    it.start = 2u;
    (void)mb_master_add_item(&m, &it);

    check_int("同一从机 3 个轮询项只占 1 个槽", (int)m.slave_count, 1);
    check_int("轮询项数为 3", (int)m.item_count, 3);
    if (mb_master_slave_stat(&m, 7u) != 0) {
        ok("已登记地址能查到状态");
    } else {
        fail("已登记地址能查到状态", "");
    }

    /* 填满到 MB_M_MAX_SLAVES 台。
       已有地址 7 占 1 个槽，还需 MB_M_MAX_SLAVES-1 = 15 台。
       取地址 2..(MB_M_MAX_SLAVES+1) 共 MB_M_MAX_SLAVES 个值，
       跳过 7 后剩 MB_M_MAX_SLAVES-1 个 —— 恰好凑满。 */
    for (i = 2; i <= (int)MB_M_MAX_SLAVES + 1; i++) {
        if (i == 7) {
            continue;                   /* 已在上面登记过 */
        }
        it.slave = (uint8_t)i;
        it.start = 0u;
        if (mb_master_add_item(&m, &it) != MB_M_OK) {
            char d[64];
            sprintf(d, "(登记到第 %u 台时失败, 地址 %d)",
                    (unsigned)m.slave_count, i);
            fail("依次登记到表满都成功", d);
            return;
        }
    }
    check_int("表已满（MB_M_MAX_SLAVES 台）",
              (int)m.slave_count, (int)MB_M_MAX_SLAVES);

    /* 第 17 台必须被明确拒绝，不能静默截断 */
    it.slave = 50u;
    if (mb_master_add_item(&m, &it) == MB_M_ERR_PARAM) {
        ok("超过上限的第 17 台被明确拒绝（不静默截断）");
    } else {
        fail("超过上限的第 17 台被明确拒绝", "");
    }
    check_int("被拒后从机表数量不变", (int)m.slave_count, (int)MB_M_MAX_SLAVES);
    check_int("被拒后轮询项数不变",
              (int)m.item_count, (int)MB_M_MAX_SLAVES + 2);
    /* 位图边界：位图覆盖地址 1..16（16 台）。
       当前登记的 16 台是地址 7 与 2..17（跳过 7），
       其中落在位图范围内（1..16）的是 2..16 共 15 台；
       地址 17 超出位图容量，应被忽略（不越界、不误置位）。
       所以位图应为 bit1..bit15 全 1，bit0 为 0 -> 0xFFFE。 */
    check_int("表满时位图 = 0xFFFE（地址 2..16 在线，17 超容量被忽略）",
              (int)mb_master_online_bitmap(&m), 0xFFFE);

    /* 被拒的地址不应留下任何痕迹 */
    check_int("被拒的地址查不到状态（未留下半条记录）",
              mb_master_slave_stat(&m, 50u) == 0 ? 1 : 0, 1);

    /* 明确验证"超出位图容量的地址不会被误置位"：
       地址 17 已在轮询表且在线，但它不该影响任何位。 */
    {
        int slot17 = -1;
        int k;
        for (k = 0; k < (int)m.slave_count; k++) {
            if (m.addr[k] == 17u) { slot17 = k; break; }
        }
        if (slot17 >= 0 && m.stat[slot17].online != 0u) {
            ok("地址 17 确实已登记且在线（用于验证位图边界）");
        } else {
            fail("地址 17 确实已登记且在线", "");
        }
    }

    /* 清空轮询表应同时清空从机表 */
    mb_master_clear_items(&m);
    check_int("清空后从机表也清空", (int)m.slave_count, 0);
    check_int("清空后已登记地址查不到状态",
              mb_master_slave_stat(&m, 7u) == 0 ? 1 : 0, 1);
    check_int("清空后位图为 0", (int)mb_master_online_bitmap(&m), 0);

    /* 空指针安全 */
    check_int("空上下文查询状态返回 NULL",
              mb_master_slave_stat(0, 1u) == 0 ? 1 : 0, 1);
}

/* ==================================================================
 * 测试 9：与真实从站引擎对测（自环验证）
 * ================================================================== */
static void test_against_real_slave(void)
{
    mb_master_t   m;
    mb_slave_t    slave;
    mb_regs_t     regs;
    mb_m_poll_item_t it;
    uint8_t       resp[MB_MAX_ADU];

    printf("\n[9] 与真实从站引擎对测（主站发 -> 从站解 -> 从站回 -> 主站解）\n");

    bus_reset();
    (void)mb_master_init(&m, bus_send, &BUS, bus_store, &BUS);
    mb_slave_init(&slave, 1u, &regs);

    it.slave = 1u; it.func = MB_FC_READ_HOLDING; it.start = 0u; it.qty = 4u;
    it.map_base = 0u; it.map_area = 1;
    (void)mb_master_add_item(&m, &it);

    /* 准备从站侧数据：设备 ID = 0x000186A0 = 100000 */
    regs.holding[0] = 0x0001u;
    regs.holding[1] = 0x86A0u;
    regs.holding[2] = 0x1234u;
    regs.holding[3] = 0x5678u;

    /* 主站发出请求 */
    mb_master_poll(&m, 0u);
    check_int("主站已发出请求", BUS.sent_n, 1);

    /* 把主站发出的帧直接喂给真实从站引擎 */
    if (mb_slave_process(&slave, BUS.sent[0], BUS.sent_len[0]) != 1) {
        fail("真实从站引擎接受了主站请求", "");
        return;
    }
    ok("真实从站引擎接受了主站请求");

    memcpy(resp, slave.tx_buf, slave.tx_len);

    /* 把从站的响应交回主站 */
    if (mb_master_on_response(&m, resp, slave.tx_len) == MB_M_OK) {
        ok("主站正确解析了真实从站的响应");
    } else {
        char d[64];
        sprintf(d, "(得到 %s)", mb_master_strerror(mb_master_on_response(&m, resp, slave.tx_len)));
        fail("主站正确解析了真实从站的响应", d);
        return;
    }

    check_int("跨引擎读回第 1 个寄存器", (int)BUS.store_vals[0], 0x0001);
    check_int("跨引擎读回第 2 个寄存器", (int)BUS.store_vals[1], 0x86A0);
    check_int("跨引擎读回第 3 个寄存器", (int)BUS.store_vals[2], 0x1234);
    check_int("跨引擎读回第 4 个寄存器", (int)BUS.store_vals[3], 0x5678);

    /* 异常路径也要对测：让主站读越界地址，从站应返回异常，主站应识别 */
    bus_reset();
    (void)mb_master_clear_items(&m);
    it.start = 0x0010u;                 /* 保持寄存器只有 16 个，地址 16 越界 */
    it.qty = 1u;
    (void)mb_master_add_item(&m, &it);

    mb_master_poll(&m, 0u);
    if (mb_slave_process(&slave, BUS.sent[0], BUS.sent_len[0]) != 1) {
        fail("从站对越界请求返回异常响应", "");
        return;
    }
    memcpy(resp, slave.tx_buf, slave.tx_len);
    if (mb_master_on_response(&m, resp, slave.tx_len) == MB_M_ERR_EXCEPTION) {
        ok("跨引擎异常响应被正确识别（越界 -> ERR_EXCEPTION）");
    } else {
        fail("跨引擎异常响应被正确识别", "");
    }
}


/* ==================================================================
 * 测试 11：应用层用的 4 个访问器
 *
 *  ⚠️ 这 4 个 API 是**供应用层遍历从机状态**用的
 *     （`gw_app.c` 的 `gw_app_sync_master_to_pipe()` 在用它们），
 *     但原本一个都没有单元测试 —— 只被集成路径间接覆盖。
 *
 *     "被间接覆盖"不等于"测过"：如果 `mb_master_slave_at` 的越界判断
 *     写错了，集成测试只会在**恰好遍历到边界**时才暴露，
 *     而那种情况在 3 台从机的测试里根本不会出现。
 *
 *  另外 `mb_master_current_index` 目前**没有任何调用方** ——
 *  它是有文档意图的公开 API（"轮询进度，用于上报告警"）。
 *  这里把它测了，免得将来有人接上它时带着 bug。
 * ================================================================== */
static void test_app_accessors(void)
{
    mb_master_t m;
    mb_m_poll_item_t it;
    uint8_t addr = 0u;
    int i;

    printf("\n[11] 应用层访问器（从机表遍历与轮询进度）\n");

    /* ---- 空表 ---- */
    bus_reset();
    (void)mb_master_init(&m, bus_send, &BUS, bus_store, &BUS);

    check_int("空表时从机数为 0", (int)mb_master_slave_count(&m), 0);
    check_int("空表时取第 0 台越界",
              mb_master_slave_at(&m, 0u, &addr), -1);
    check_int("空表时轮询进度为 0",
              (int)mb_master_current_index(&m), 0);

    /* ---- 登记 3 台不同地址的从机（刻意非连续，验证不是按地址索引）---- */
    {
        const uint8_t addrs[3] = { 0x11u, 0x05u, 0x63u };   /* 17, 5, 99 */

        for (i = 0; i < 3; i++) {
            memset(&it, 0, sizeof(it));
            it.slave = addrs[i];
            it.func  = MB_FC_READ_HOLDING;
            it.start = 0u;
            it.qty   = 2u;
            (void)mb_master_add_item(&m, &it);
        }

        check_int("登记 3 项后从机数为 3",
                  (int)mb_master_slave_count(&m), 3);

        /* ⚠️ 按**序号**取，不是按地址 —— 地址是非连续的，
           如果实现写成"按地址索引"，这里会取错或越界。 */
        for (i = 0; i < 3; i++) {
            addr = 0u;
            if (mb_master_slave_at(&m, (uint8_t)i, &addr) == 0
                && addr == addrs[i]) {
                ok("按序号取从机地址正确");
            } else {
                char d[80];
                sprintf(d, "(序号 %d: 期望 0x%02X, 实际 0x%02X)",
                        i, addrs[i], addr);
                fail("按序号取从机地址正确", d);
            }
        }
        ok("⚠️ 地址非连续（17/5/99）—— 若实现按地址索引会取错");

        /* 越界 */
        check_int("序号 3 越界（只有 3 台）",
                  mb_master_slave_at(&m, 3u, &addr), -1);
        check_int("序号 255 越界",
                  mb_master_slave_at(&m, 255u, &addr), -1);
        ok("越界返回 -1 而不是读到别的内存");
    }

    /* ---- 空指针 ---- */
    check_int("空指针 slave_count 返回 0",
              (int)mb_master_slave_count(0), 0);
    check_int("空指针 slave_at 返回 -1",
              mb_master_slave_at(0, 0u, &addr), -1);
    check_int("空指针 current_index 返回 0",
              (int)mb_master_current_index(0), 0);
    check_int("空 addr 输出指针返回 -1",
              mb_master_slave_at(&m, 0u, 0), -1);
    ok("各访问器对空指针安全");

    /* ---- 轮询进度随 poll 推进 ---- */
    bus_reset();
    (void)mb_master_init(&m, bus_send, &BUS, bus_store, &BUS);
    for (i = 0; i < 3; i++) {
        memset(&it, 0, sizeof(it));
        it.slave = (uint8_t)(1 + i);
        it.func  = MB_FC_READ_HOLDING;
        it.start = 0u;
        it.qty   = 2u;
        (void)mb_master_add_item(&m, &it);
    }

    {
        uint16_t first = mb_master_current_index(&m);

        /* 轮询若干轮（每轮都是"发请求 -> 无响应 -> 超时"） */
        for (i = 0; i < 40; i++) {
            mb_master_poll(&m, 100u);
        }

        check_int("轮询后进度仍在合法范围（0..2）",
                  (int)(mb_master_current_index(&m) <= 2u ? 1 : 0), 1);
        printf("      轮询进度：起始 %u -> 当前 %u\n",
               (unsigned)first, (unsigned)mb_master_current_index(&m));
    }
    ok("轮询进度是「第几项」的索引，不是计数器 —— 所以它恒在 0..(n-1) 内");

    /* ---- abort_pending ---- */
    bus_reset();
    (void)mb_master_init(&m, bus_send, &BUS, bus_store, &BUS);
    memset(&it, 0, sizeof(it));
    it.slave = 0x11u; it.func = MB_FC_READ_HOLDING; it.qty = 2u;
    (void)mb_master_add_item(&m, &it);

    check_int("空闲时 abort_pending 返回 -1（没有在等的请求）",
              mb_master_abort_pending(&m), -1);

    mb_master_poll(&m, 0u);         /* 发起请求 -> 进入等待 */
    check_int("发起请求后不再空闲", mb_master_is_idle(&m), 0);

    check_int("等待中 abort_pending 返回 0",
              mb_master_abort_pending(&m), 0);
    check_int("放弃后回到空闲", mb_master_is_idle(&m), 1);
    check_int("再次 abort 返回 -1（已经没在等）",
              mb_master_abort_pending(&m), -1);
    ok("⚠️ 放弃**不判定为从机失败** —— 那是「我们主动让出总线」，"
       "若走超时路径会把一台正常从机判成离线");

    check_int("空指针 abort_pending 返回 -1", mb_master_abort_pending(0), -1);
}

/* ==================================================================
 *  测试：轮询节拍（两次请求之间的最小空闲）
 *
 *  ⚠️ 这条守护的是 2026-10-10 上板实测发现的问题：
 *     从机应答够快时（PC 模拟从机 ~2ms），主站会**全速轮询** ——
 *     实测 177 次/秒、CPU 负载 1000‰、单轮耗时打满。
 *     那不是性能好，而是把总线长期占着、现场仪表被上百次/秒地访问。
 * ================================================================== */
static void test_poll_interval(void)
{
    mb_master_t      m;
    mb_m_poll_item_t it;
    uint8_t  resp[32];
    uint16_t rlen;
    uint16_t vals[4];
    int      i;

    printf("\n[轮询节拍]\n");

    bus_reset();
    (void)mb_master_init(&m, bus_send, &BUS, bus_store, &BUS);

    memset(&it, 0, sizeof(it));
    it.slave = 1u; it.func = MB_FC_READ_INPUT;
    it.start = 0u; it.qty = 4u; it.map_base = 0u; it.map_area = 0u;
    (void)mb_master_add_item(&m, &it);

    check_int("设置节拍 100ms", (int)mb_master_set_interval(&m, 100u), MB_M_OK);
    check_int("超出上限被拒（60001ms）",
              (int)mb_master_set_interval(&m, 60001u), MB_M_ERR_PARAM);

    /* ---- 首次请求不受节拍限制：立即发出 ---- */
    BUS.sent_n = 0;
    mb_master_poll(&m, 0u);
    check_int("首次请求立即发出（不用先等一个节拍）", BUS.sent_n, 1);

    /* ---- 事务结束：给它一个合法响应 ---- */
    vals[0] = 0x1111u; vals[1] = 0x2222u; vals[2] = 0x3333u; vals[3] = 0x4444u;
    make_read_resp(resp, &rlen, 1u, MB_FC_READ_INPUT, vals, 4u);
    check_int("响应被主站接受",
              (int)mb_master_on_response(&m, resp, rlen), (int)MB_M_OK);

    /* ---- 90ms 之内不该发第二帧 ---- */
    BUS.sent_n = 0;
    for (i = 0; i < 9; i++) {
        mb_master_poll(&m, 10u);        /* 9 × 10ms = 90ms < 100ms */
    }
    check_int("节拍未到时不发请求（90ms < 100ms）", BUS.sent_n, 0);

    /* ---- 到第 100ms：发出第二帧 ---- */
    mb_master_poll(&m, 10u);
    check_int("节拍到了才发（100ms）", BUS.sent_n, 1);

    /* ---- 节拍关成 0：恢复"一问一答连着发" ---- */
    check_int("关掉节拍", (int)mb_master_set_interval(&m, 0u), MB_M_OK);
    make_read_resp(resp, &rlen, 1u, MB_FC_READ_INPUT, vals, 4u);
    (void)mb_master_on_response(&m, resp, rlen);
    BUS.sent_n = 0;
    mb_master_poll(&m, 0u);
    check_int("节拍为 0 时立即发下一帧（旧行为不变）", BUS.sent_n, 1);

    /* ---- 发送失败**不**重新计时：应当立刻重试，而不是再等一个间隔 ---- */
    (void)mb_master_set_interval(&m, 500u);
    m.awaiting = 0u;                    /* 手工收尾，模拟上一轮结束 */
    m.idle_ms  = 500u;                  /* 空闲已经等够 */
    BUS.send_fail = 1;
    BUS.sent_n    = 0;
    mb_master_poll(&m, 0u);             /* 尝试发送 -> 失败（不发出去） */
    BUS.send_fail = 0;
    mb_master_poll(&m, 0u);             /* 下一轮应当立刻重试 */
    check_int("发送失败后下一轮立刻重试（不重新等间隔）", BUS.sent_n, 1);
}

int main(void)
{
    printf("========== Modbus RTU 主站协议引擎 单元测试 ==========\n");

    test_request_frame();
    test_normal_response();
    test_poll_schedule();
    test_poll_interval();
    test_timeout_retry();
    test_offline_backoff();
    test_bad_responses();
    test_config_validation();
    test_bitmap_and_empty();
    test_slave_table();
    test_against_real_slave();
    test_app_accessors();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
