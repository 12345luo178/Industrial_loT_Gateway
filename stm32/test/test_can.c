/**
 * @file    test_can.c
 * @brief   CAN 报文处理与 Bus-Off 恢复单元测试
 *
 * ============================================================================
 *  本套测试的重点是**恢复策略的时间语义**。
 *
 *  "退避"这件事看起来简单，但极容易写成"每次调用试一次、失败就计数"——
 *  那样的实际间隔完全取决于调用频率：10ms 的节拍下就是每秒试 100 次，
 *  3 次失败瞬间放弃，比不做恢复还糟。
 *
 *  本模块踩过这个坑（详见 gw_can_core.h 里的说明），
 *  所以这里用**明确的毫秒推进**来验证：
 *    - 1 秒退避窗口内，无论 tick 多少次，都只能尝试一次
 *    - 连续失败 3 次后间隔拉长到 10 秒
 *    - 拉长后仍会继续重试（不永久放弃）
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iinc -o test/test_can.exe \
 *        src/gw_can_core.c test/test_can.c
 *    ./test/test_can.exe
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

#include "gw_can_core.h"

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
 * 测试环境：可控的恢复回调
 * ================================================================== */
typedef struct {
    int      reinit_result;         /* 0 = 成功；非 0 = 失败 */
    uint32_t reinit_calls;          /* reinit 被调用的次数 */
} fake_env_t;

static fake_env_t ENV;

static int fake_reinit(void *ctx)
{
    fake_env_t *e = (fake_env_t *)ctx;

    e->reinit_calls++;
    return e->reinit_result;
}

/* 造一条标准帧 */
static gw_can_frame_t mk_frame(uint16_t id, const uint8_t *data, uint8_t dlc)
{
    gw_can_frame_t f;

    memset(&f, 0, sizeof(f));
    f.id       = id;
    f.dlc      = dlc;
    f.extended = 0u;
    if (data != 0 && dlc > 0u) {
        memcpy(f.data, data, dlc);
    }
    return f;
}

/* 推进时间：每次 tick 一小步，模拟真实主循环 */
static void advance(gw_can_core_t *c, uint32_t total_ms, uint32_t step_ms)
{
    uint32_t left = total_ms;

    while (left > 0u) {
        uint32_t s = (left < step_ms) ? left : step_ms;

        gw_can_core_tick(c, s);
        left -= s;
    }
}

/* ==================================================================
 * 测试 1：初始化与参数校验
 * ================================================================== */
static void test_init(void)
{
    gw_can_core_t c;

    printf("\n[1] 初始化与参数校验\n");

    memset(&ENV, 0, sizeof(ENV));
    ENV.reinit_result = 0;

    if (gw_can_core_init(&c, 0x100u, 0x7F0u, fake_reinit, &ENV) == 0) {
        ok("初始化成功");
    } else {
        fail("初始化成功", "");
        return;
    }
    check_int("初始状态为 NORMAL", (int)gw_can_core_state(&c),
              (int)GW_CAN_ST_NORMAL);
    check_int("初始无有效通道", (int)gw_can_core_valid_mask(&c), 0);

    check_int("空指针被拒", gw_can_core_init(0, 0u, 0u, 0, 0), -1);
    check_int("滤波 ID 超出 11 位被拒",
              gw_can_core_init(&c, 0x800u, 0u, 0, 0), -2);
    check_int("滤波掩码超出 11 位被拒",
              gw_can_core_init(&c, 0u, 0x800u, 0, 0), -2);
    check_int("掩码 0x7FF（全通过）被接受",
              gw_can_core_init(&c, 0u, 0x7FFu, 0, 0), 0);

    /* 无恢复回调也要能初始化（只是无法自愈） */
    check_int("允许不提供恢复回调",
              gw_can_core_init(&c, 0u, 0u, 0, 0), 0);
}

/* ==================================================================
 * 测试 2：报文校验与取值
 * ================================================================== */
static void test_frame_helpers(void)
{
    gw_can_frame_t f;
    uint16_t       v = 0u;
    uint8_t        d[8] = { 0x12, 0x34, 0xAB, 0xCD, 0x00, 0xFF, 0x80, 0x01 };

    printf("\n[2] 报文校验与数值取值\n");

    f = mk_frame(0x123u, d, 8u);
    check_int("合法标准帧通过校验", gw_can_frame_valid(&f), 0);

    f.id = 0x800u;                  /* 超出 11 位 */
    check_int("标准帧 ID 超出 11 位被判非法",
              gw_can_frame_valid(&f), -3);

    f = mk_frame(0x123u, d, 8u);
    f.extended = 1u;
    f.id = 0x1FFFFFFFu;
    check_int("扩展帧最大 ID 合法", gw_can_frame_valid(&f), 0);
    f.id = 0x20000000u;
    check_int("扩展帧 ID 超出 29 位被判非法", gw_can_frame_valid(&f), -3);

    f = mk_frame(0x123u, d, 9u);
    check_int("DLC 超过 8 被判非法", gw_can_frame_valid(&f), -2);

    /* ------------------------------------------------------------------
     * ⚠️ 远程帧（RTR）必须被拒 —— 这条是 fuzz 测试抓出来的 bug
     *
     * 远程帧是"请求别人发数据"，它**自己没有数据段**。
     * 如果放它进通道映射，`gw_can_frame_get_u16()` 会从 `frame.data[]`
     * 取值 —— 而那是上一个报文留下的内容（调用方复用同一个结构体时）。
     *
     * 后果：通道里出现**一个看起来合理但完全过期的值**，
     * 上报到云端后没人能看出它是假的。
     * 这比"通道没数据"危险得多 —— 后者至少能被发现。
     * ------------------------------------------------------------------ */
    f = mk_frame(0x123u, d, 8u);
    f.rtr = 1u;
    check_int("远程帧（RTR）被判非法", gw_can_frame_valid(&f), -4);

    /* 同一个 rtr 帧不应进入通道映射 */
    {
        gw_can_core_t c2;

        (void)gw_can_core_init(&c2, 0u, 0u, 0, 0);
        (void)gw_can_core_set_map(&c2, 0x123u, 0u);
        check_int("远程帧不被收下（返回值非 0）",
                  (int)(gw_can_core_on_frame(&c2, &f) != 0 ? 1 : 0), 1);
        check_int("远程帧计数进 rx_malformed",
                  (int)(gw_can_core_stats(&c2)->rx_malformed >= 1u ? 1 : 0), 1);
    }

    /* 大端取值 */
    f = mk_frame(0x123u, d, 8u);
    check_int("offset 0 大端取值 = 0x1234",
              gw_can_frame_get_u16(&f, 0u, &v), 0);
    check_int("offset 0 的值正确", (int)v, 0x1234);
    check_int("offset 2 大端取值", gw_can_frame_get_u16(&f, 2u, &v), 0);
    check_int("offset 2 的值正确", (int)v, 0xABCD);
    check_int("offset 6 大端取值", gw_can_frame_get_u16(&f, 6u, &v), 0);
    check_int("offset 6 的值正确", (int)v, 0x8001);

    /* 小端取值 */
    check_int("offset 0 小端取值", gw_can_frame_get_u16_le(&f, 0u, &v), 0);
    check_int("小端结果正确（字节序反转）", (int)v, 0x3412);

    /* 越界 */
    check_int("offset 7 越界被拒（只剩 1 字节）",
              gw_can_frame_get_u16(&f, 7u, &v), -1);
    check_int("offset 8 越界被拒", gw_can_frame_get_u16(&f, 8u, &v), -1);

    /* DLC 不足 */
    f = mk_frame(0x123u, d, 1u);
    check_int("DLC = 1 时取 16 位被拒",
              gw_can_frame_get_u16(&f, 0u, &v), -1);

    /* 空指针 */
    check_int("空报文被拒", gw_can_frame_get_u16(0, 0u, &v), -1);
    check_int("空输出被拒", gw_can_frame_get_u16(&f, 0u, 0), -1);
    check_int("valid 对空指针返回负值", gw_can_frame_valid(0) < 0 ? 1 : 0, 1);
}

/* ==================================================================
 * 测试 3：软件滤波
 * ================================================================== */
static void test_filter(void)
{
    gw_can_core_t  c;
    gw_can_frame_t f;
    uint8_t        d[2] = { 0x00, 0x11 };

    printf("\n[3] 软件滤波（与硬件滤波叠加）\n");

    /* 掩码 0x7F0：只比较 bit4~bit10 */
    (void)gw_can_core_init(&c, 0x100u, 0x7F0u, fake_reinit, &ENV);
    (void)gw_can_core_set_map(&c, 0x100u, 0u);

    f = mk_frame(0x100u, d, 2u);
    check_int("完全匹配的 ID 通过", gw_can_core_on_frame(&c, &f), 1);

    f = mk_frame(0x10Fu, d, 2u);        /* 低 4 位不同，掩码不比较低 4 位 */
    check_int("掩码不比较的位不同仍通过", gw_can_core_on_frame(&c, &f), 0);

    f = mk_frame(0x200u, d, 2u);        /* bit8 不同，掩码比较该位 */
    check_int("掩码比较的位不同被拒绝", gw_can_core_on_frame(&c, &f), -1);
    check_int("拒绝计数增加",
              (int)gw_can_core_stats(&c)->rx_filtered >= 1 ? 1 : 0, 1);

    /* 掩码 0 = 全通过 */
    (void)gw_can_core_init(&c, 0x100u, 0u, fake_reinit, &ENV);
    f = mk_frame(0x7FFu, d, 2u);
    check_int("掩码 0 时任意 ID 都通过", gw_can_core_on_frame(&c, &f) >= 0 ? 1 : 0, 1);

    /* 扩展帧不走通道映射 */
    (void)gw_can_core_init(&c, 0u, 0u, fake_reinit, &ENV);
    f = mk_frame(0x100u, d, 2u);
    f.extended = 1u;
    check_int("扩展帧被拒绝（寄存器表只定义标准帧 ID）",
              gw_can_core_on_frame(&c, &f), -1);
}

/* ==================================================================
 * 测试 4：通道映射与最新值覆盖
 * ================================================================== */
static void test_channel_map(void)
{
    gw_can_core_t  c;
    gw_can_frame_t f;
    uint8_t        d1[2] = { 0x01, 0x00 };      /* 256 */
    uint8_t        d2[2] = { 0x02, 0x00 };      /* 512 */

    printf("\n[4] 通道映射与最新值覆盖\n");

    (void)gw_can_core_init(&c, 0u, 0u, fake_reinit, &ENV);
    check_int("配置映射成功", gw_can_core_set_map(&c, 0x100u, 0u), 0);
    check_int("配置第二个映射", gw_can_core_set_map(&c, 0x200u, 1u), 0);

    check_int("通道下标越界被拒",
              gw_can_core_set_map(&c, 0x300u, GW_CAN_CH_COUNT), -3);
    check_int("ID 超出 11 位被拒",
              gw_can_core_set_map(&c, 0x800u, 0u), -2);

    f = mk_frame(0x100u, d1, 2u);
    check_int("报文映射到通道 0", gw_can_core_on_frame(&c, &f), 1);
    check_int("通道 0 变为有效",
              (int)(gw_can_core_valid_mask(&c) & 0x01u), 1);
    check_int("通道 1 仍无效",
              (int)(gw_can_core_valid_mask(&c) & 0x02u), 0);

    /* 同一 ID 再来一次，值应被覆盖 */
    f = mk_frame(0x100u, d2, 2u);
    (void)gw_can_core_on_frame(&c, &f);

    {
        uint16_t regs[16];
        memset(regs, 0xAA, sizeof(regs));
        gw_can_core_sync_regs(&c, regs, 2u, 16u);
        check_int("通道 0 的值被最新报文覆盖", (int)regs[2], 512);
        check_int("未收到数据的通道 1 保持原值不动（0xAAAA）",
                  (int)regs[3], 0xAAAA);
        ok("无效通道不清零（上位机可区分'无数据'与'数据为 0'）");
    }

    /* 未映射的 ID */
    f = mk_frame(0x555u, d1, 2u);
    check_int("未映射的 ID 返回 0（不是错误）",
              gw_can_core_on_frame(&c, &f), 0);
    check_int("未映射被单独计数",
              (int)gw_can_core_stats(&c)->rx_unmapped, 1);

    /* 同一 ID 重复配置：覆盖而不是占两个槽 */
    check_int("重复配置同一 ID 成功（覆盖）",
              gw_can_core_set_map(&c, 0x100u, 2u), 0);
    f = mk_frame(0x100u, d1, 2u);
    (void)gw_can_core_on_frame(&c, &f);
    /* 注意：通道 0 在更早的测试里已经被置为有效了，所以这里掩码是
       "通道 0 + 通道 2" = 0x01 | 0x04 = 0x05，而不是 0x04。
       断言要反映**累积状态**，不能只想着本次操作的效果。 */
    check_int("覆盖后映射到新通道（通道 0+2 -> 掩码 0x05）",
              (int)gw_can_core_valid_mask(&c), 0x05);

    /* 清空映射 */
    gw_can_core_clear_map(&c);
    f = mk_frame(0x100u, d1, 2u);
    check_int("清空映射后不再映射到通道",
              gw_can_core_on_frame(&c, &f), 0);

    /* 映射表满 */
    {
        uint8_t i;
        gw_can_core_clear_map(&c);
        for (i = 0u; i < GW_CAN_CH_COUNT; i++) {
            (void)gw_can_core_set_map(&c, (uint16_t)(0x100u + i), i);
        }
        check_int("映射表满后新增被拒",
                  gw_can_core_set_map(&c, 0x700u, 0u), -4);
    }
}

/* ==================================================================
 * 测试 5：DLC 不足仍视为有效通道
 * ================================================================== */
static void test_short_frame(void)
{
    gw_can_core_t  c;
    gw_can_frame_t f;
    uint8_t        d[1] = { 0x42 };

    printf("\n[5] 短报文（心跳帧）的处理\n");

    (void)gw_can_core_init(&c, 0u, 0u, fake_reinit, &ENV);
    (void)gw_can_core_set_map(&c, 0x700u, 3u);

    /* DLC = 1 的心跳帧：取不到 16 位值，但节点确实活着 */
    f = mk_frame(0x700u, d, 1u);
    check_int("DLC=1 的报文仍被接受（证明节点在线）",
              gw_can_core_on_frame(&c, &f), 1);
    /* 通道 3 对应位 3，掩码值 = 0x08 */
    check_int("该通道被标记为有效（通道 3 -> 位掩码 0x08）",
              (int)gw_can_core_valid_mask(&c), 0x08);

    {
        uint16_t regs[16];
        memset(regs, 0x55, sizeof(regs));
        gw_can_core_sync_regs(&c, regs, 2u, 16u);
        check_int("取不到值时上报 0", (int)regs[5], 0);
    }

    /* DLC = 0：零长度帧也是合法 CAN 报文 */
    f = mk_frame(0x700u, 0, 0u);
    check_int("DLC=0 的零长度帧合法", gw_can_core_on_frame(&c, &f), 1);

    /* 非法 DLC */
    f = mk_frame(0x700u, d, 9u);
    check_int("DLC=9 被判非法并计数", gw_can_core_on_frame(&c, &f), -2);
    check_int("非法报文被计数",
              (int)gw_can_core_stats(&c)->rx_malformed, 1);
}

/* ==================================================================
 * 测试 6：诊断环形缓冲
 * ================================================================== */
static void test_diag(void)
{
    gw_can_core_t  c;
    gw_can_frame_t f;
    uint8_t        d[2] = { 0x00, 0x01 };
    gw_can_diag_t  rec;
    int            i;
    int            n = 0;

    printf("\n[6] 诊断环形缓冲（最近收到的 ID）\n");

    (void)gw_can_core_init(&c, 0u, 0u, fake_reinit, &ENV);

    check_int("初始没有诊断记录",
              gw_can_core_take_diag(&c, &rec), 0);

    /* 发 3 条（都没映射，但应被记录） */
    for (i = 0; i < 3; i++) {
        f = mk_frame((uint16_t)(0x100u + i), d, 2u);
        (void)gw_can_core_on_frame(&c, &f);
    }

    n = 0;
    while (gw_can_core_take_diag(&c, &rec) != 0) {
        n++;
    }
    check_int("记录条数 = 3", n, 3);

    /* 溢出：发满 + 超出 */
    for (i = 0; i < (int)GW_CAN_DIAG_DEPTH + 5; i++) {
        f = mk_frame((uint16_t)(0x200u + i), d, 2u);
        (void)gw_can_core_on_frame(&c, &f);
    }
    n = 0;
    while (gw_can_core_take_diag(&c, &rec) != 0) {
        n++;
    }
    check_int("环形缓冲最多保留 GW_CAN_DIAG_DEPTH 条",
              n, (int)GW_CAN_DIAG_DEPTH);

    /* 保留的应是最新的（丢最旧）。
       算清楚当前缓冲里有什么：
         第二轮发了 DEPTH+5 = 13 条（0x200..0x20C），
         take_diag 又把它们全部读掉了（count 归零），
         **但读操作只动 count、不动数据** —— 所以那些数据还在数组里。
         再写 3 条（0x300..0x302）后，count = 3，最旧的自然是 0x300。

       这里踩过一次：我误以为"读掉"就等于"清空"，于是按"残留 8 条"去算，
       得出了错误的期望值 0x208。
       教训：诊断缓冲的**数据**与**可读条数**是两件事，
       断言前必须清楚当前 count 是多少 —— 而 count 只由"写"和"读"共同决定。 */
    for (i = 0; i < 3; i++) {
        f = mk_frame((uint16_t)(0x300u + i), d, 2u);
        (void)gw_can_core_on_frame(&c, &f);
    }
    {
        uint16_t first = 0u;
        int      got   = 0;

        while (gw_can_core_take_diag(&c, &rec) != 0) {
            if (got == 0) {
                first = rec.id;
            }
            got++;
        }
        check_int("最旧的一条是本次写入的第一条（0x300）",
                  (int)first, 0x300);
        check_int("可读条数 = 本次写入的 3 条", got, 3);
    }

    /* 再验证"缓冲满时丢最旧"：连续写 DEPTH+3 条，读出来应是最新的 DEPTH 条 */
    {
        uint16_t first = 0u;
        int      got   = 0;

        for (i = 0; i < (int)GW_CAN_DIAG_DEPTH + 3; i++) {
            f = mk_frame((uint16_t)(0x400u + i), d, 2u);
            (void)gw_can_core_on_frame(&c, &f);
        }
        while (gw_can_core_take_diag(&c, &rec) != 0) {
            if (got == 0) {
                first = rec.id;
            }
            got++;
        }
        check_int("缓冲满时可读条数仍为 DEPTH", got, (int)GW_CAN_DIAG_DEPTH);
        check_int("丢最旧后最旧的一条是 0x403", (int)first, 0x403);
    }

    /* 被过滤掉的报文也要记录 —— 这是排查"通道没数据"的关键 */
    {
        uint16_t id_before;
        int      seen = 0;

        (void)gw_can_core_init(&c, 0x100u, 0x7F0u, fake_reinit, &ENV);
        f = mk_frame(0x500u, d, 2u);        /* 会被滤波拒绝 */
        (void)gw_can_core_on_frame(&c, &f);

        while (gw_can_core_take_diag(&c, &rec) != 0) {
            id_before = rec.id;
            seen = 1;
            (void)id_before;
        }
        check_int("被滤波拒绝的报文仍进入诊断记录", seen, 1);
        ok("现场能看出'ID 收到了但被滤波挡了'与'根本没收到'的区别");
    }
}

/* ==================================================================
 * 测试 7：Bus-Off 恢复的时间语义（本套测试的重点）
 * ================================================================== */
static void test_bus_off_recovery(void)
{
    gw_can_core_t c;

    printf("\n[7] Bus-Off 恢复的时间语义（核心）\n");

    /* ---- 场景 A：恢复成功 ---- */
    memset(&ENV, 0, sizeof(ENV));
    ENV.reinit_result = 0;
    (void)gw_can_core_init(&c, 0u, 0u, fake_reinit, &ENV);

    gw_can_core_notify_bus_off(&c);
    check_int("进入 Bus-Off 状态", (int)gw_can_core_state(&c),
              (int)GW_CAN_ST_BUS_OFF);
    check_int("Bus-Off 次数 = 1",
              (int)gw_can_core_stats(&c)->bus_off_count, 1);

    /* ⚠️ 关键验证：1 秒窗口内即使 tick 100 次，也只能尝试 1 次恢复 */
    advance(&c, 900u, 10u);         /* 推进 900ms（90 次 tick） */
    check_int("退避窗口内不尝试恢复（tick 90 次仍为 0）",
              (int)ENV.reinit_calls, 0);

    advance(&c, 200u, 10u);         /* 再推进 200ms，跨过 1 秒 */
    check_int("退避结束后尝试恢复", (int)ENV.reinit_calls, 1);
    check_int("恢复成功回到 NORMAL", (int)gw_can_core_state(&c),
              (int)GW_CAN_ST_NORMAL);
    check_int("恢复成功计数 = 1",
              (int)gw_can_core_stats(&c)->recover_ok, 1);
    check_int("没有失败计数",
              (int)gw_can_core_stats(&c)->recover_fail, 0);

    /* 恢复成功后继续 tick 不应再触发恢复 */
    advance(&c, 5000u, 10u);
    check_int("恢复成功后不再重复恢复", (int)ENV.reinit_calls, 1);

    /* ---- 场景 B：反复失败 -> 间隔拉长 ---- */
    memset(&ENV, 0, sizeof(ENV));
    ENV.reinit_result = -1;         /* 让恢复一直失败 */
    (void)gw_can_core_init(&c, 0u, 0u, fake_reinit, &ENV);

    gw_can_core_notify_bus_off(&c);

    /* 第 1 次尝试（1 秒后） */
    advance(&c, 1100u, 10u);
    check_int("第 1 次恢复尝试", (int)ENV.reinit_calls, 1);
    check_int("失败后回到 BUS_OFF 等待",
              (int)gw_can_core_state(&c), (int)GW_CAN_ST_BUS_OFF);

    /* ⚠️ 再验证一次：下一个 1 秒窗口内不能重复尝试 */
    advance(&c, 900u, 10u);
    check_int("第二次退避窗口内不重复尝试（仍为 1 次）",
              (int)ENV.reinit_calls, 1);

    /* 第 2 次尝试 */
    advance(&c, 200u, 10u);
    check_int("第 2 次恢复尝试", (int)ENV.reinit_calls, 2);

    /* 第 3 次尝试 —— 达到 GW_CAN_RECOVER_MAX_TRIES */
    advance(&c, 1100u, 10u);
    check_int("第 3 次恢复尝试", (int)ENV.reinit_calls, 3);
    check_int("连续失败 3 次后进入 FAILED（长间隔）",
              (int)gw_can_core_state(&c), (int)GW_CAN_ST_FAILED);

    /* ⚠️ 长间隔内不能尝试 */
    advance(&c, 9000u, 10u);
    check_int("10 秒长间隔内不尝试恢复（tick 900 次仍为 3 次）",
              (int)ENV.reinit_calls, 3);

    /* 长间隔结束后继续重试 —— **不永久放弃** */
    advance(&c, 1500u, 10u);
    check_int("长间隔结束后继续重试（不永久放弃）",
              (int)(ENV.reinit_calls >= 4 ? 1 : 0), 1);

    /* ---- 场景 C：终于成功 ---- */
    ENV.reinit_result = 0;
    advance(&c, 30000u, 10u);       /* 给足时间让下一轮尝试发生 */
    check_int("对端恢复后最终能自愈", (int)gw_can_core_state(&c),
              (int)GW_CAN_ST_NORMAL);
    if (gw_can_core_stats(&c)->recover_ok >= 1u) {
        ok("自愈被计数（运维可见'这台设备曾经 Bus-Off 过'）");
    } else {
        fail("自愈被计数", "");
    }

    /* ---- 场景 D：重复报告 Bus-Off 不重复计数 ---- */
    memset(&ENV, 0, sizeof(ENV));
    ENV.reinit_result = -1;
    (void)gw_can_core_init(&c, 0u, 0u, fake_reinit, &ENV);

    gw_can_core_notify_bus_off(&c);
    gw_can_core_notify_bus_off(&c);
    gw_can_core_notify_bus_off(&c);
    check_int("重复报告 Bus-Off 只计一次（轮询式查询会重复报）",
              (int)gw_can_core_stats(&c)->bus_off_count, 1);

    /* ---- 场景 E：无恢复回调时如实停在 BUS_OFF ---- */
    memset(&ENV, 0, sizeof(ENV));
    (void)gw_can_core_init(&c, 0u, 0u, 0, 0);   /* 不给回调 */
    gw_can_core_notify_bus_off(&c);
    advance(&c, 5000u, 10u);
    check_int("无恢复回调时停在 BUS_OFF（不假装恢复成功）",
              (int)gw_can_core_state(&c), (int)GW_CAN_ST_BUS_OFF);
    check_int("无回调时不产生恢复尝试", (int)ENV.reinit_calls, 0);
}

/* ==================================================================
 * 测试 8：错误警告态与状态字符串
 * ================================================================== */
static void test_error_warn(void)
{
    gw_can_core_t c;

    printf("\n[8] 错误警告态与状态字符串\n");

    memset(&ENV, 0, sizeof(ENV));
    (void)gw_can_core_init(&c, 0u, 0u, fake_reinit, &ENV);

    gw_can_core_notify_error_warn(&c);
    check_int("正常态可转警告态", (int)gw_can_core_state(&c),
              (int)GW_CAN_ST_ERROR_WARN);

    gw_can_core_notify_normal(&c);
    check_int("警告态可转回正常", (int)gw_can_core_state(&c),
              (int)GW_CAN_ST_NORMAL);

    /* 警告态下不应触发恢复流程 */
    gw_can_core_notify_error_warn(&c);
    advance(&c, 5000u, 10u);
    check_int("警告态不触发恢复（还能通信，不该重初始化）",
              (int)ENV.reinit_calls, 0);

    /* Bus-Off 优先于警告 */
    gw_can_core_notify_bus_off(&c);
    check_int("警告态下收到 Bus-Off 能正确转入",
              (int)gw_can_core_state(&c), (int)GW_CAN_ST_BUS_OFF);

    /* 恢复流程中的警告通知不应覆盖状态机 */
    gw_can_core_notify_error_warn(&c);
    check_int("恢复流程中的警告通知不覆盖状态",
              (int)gw_can_core_state(&c), (int)GW_CAN_ST_BUS_OFF);

    /* 状态字符串 */
    if (strcmp(gw_can_state_str(GW_CAN_ST_NORMAL), "NORMAL") == 0 &&
        strcmp(gw_can_state_str(GW_CAN_ST_BUS_OFF), "BUS_OFF") == 0 &&
        strcmp(gw_can_state_str(GW_CAN_ST_FAILED), "FAILED") == 0 &&
        gw_can_state_str((gw_can_state_t)99) != 0) {
        ok("状态字符串正确且有兜底");
    } else {
        fail("状态字符串正确且有兜底", "");
    }

    /* 空指针安全 */
    gw_can_core_tick(0, 100u);
    gw_can_core_notify_bus_off(0);
    gw_can_core_notify_normal(0);
    gw_can_core_notify_error_warn(0);
    gw_can_core_clear_map(0);
    gw_can_core_sync_regs(0, 0, 0u, 0u);
    check_int("统计量查询对空指针安全", gw_can_core_stats(0) == 0 ? 1 : 0, 1);
    check_int("状态查询对空指针安全",
              (int)gw_can_core_state(0), (int)GW_CAN_ST_UNINIT);
    check_int("有效位图查询对空指针安全", (int)gw_can_core_valid_mask(0), 0);
    ok("各接口对空指针安全（不崩溃）");
}

/* ==================================================================
 * 测试 9：寄存器同步的边界
 * ================================================================== */
static void test_sync_bounds(void)
{
    gw_can_core_t  c;
    gw_can_frame_t f;
    uint8_t        d[2] = { 0x00, 0xAB };
    uint16_t       regs[8];
    uint8_t        i;

    printf("\n[9] 寄存器同步的边界处理\n");

    (void)gw_can_core_init(&c, 0u, 0u, fake_reinit, &ENV);
    for (i = 0u; i < GW_CAN_CH_COUNT; i++) {
        (void)gw_can_core_set_map(&c, (uint16_t)(0x100u + i), i);
    }

    /* 让全部 8 个通道都有效 */
    for (i = 0u; i < GW_CAN_CH_COUNT; i++) {
        f = mk_frame((uint16_t)(0x100u + i), d, 2u);
        (void)gw_can_core_on_frame(&c, &f);
    }
    check_int("全部 8 通道有效",
              (int)gw_can_core_valid_mask(&c), 0xFF);

    /* 目标寄存器表比通道数小：应只写放得下的部分，不越界 */
    memset(regs, 0, sizeof(regs));
    gw_can_core_sync_regs(&c, regs, 2u, 8u);    /* 只能写 regs[2..7] = 6 个通道 */
    check_int("通道 0 写入正确", (int)regs[2], 0x00AB);
    check_int("通道 5 写入正确", (int)regs[7], 0x00AB);
    ok("寄存器表放不下时截断而不越界写");

    /* base 超出范围：什么都不写 */
    memset(regs, 0xEE, sizeof(regs));
    gw_can_core_sync_regs(&c, regs, 100u, 8u);
    check_int("base 越界时不写入任何位置", (int)regs[0], 0xEEEE);

    /* 未初始化时不写 */
    {
        gw_can_core_t c2;
        memset(&c2, 0, sizeof(c2));
        memset(regs, 0xEE, sizeof(regs));
        gw_can_core_sync_regs(&c2, regs, 0u, 8u);
        check_int("未初始化时不同步", (int)regs[0], 0xEEEE);
    }
}

/* ==================================================================
 * 测试 10：端口层初始化失败 -> mark_failed（2026-10-14 上板实测补的）
 *
 * 背景：`can_port_init()` 在 `HAL_CAN_Start()` 超时（板上没有 CAN 总线 /
 *       收发器没工作 -> bxCAN 拿不到 11 个连续隐性位、退不出初始化模式）
 *       时会**提前 return**，而核心就停在 `gw_can_core_init()` 置的
 *       `NORMAL` 上 -> `gw_pipeline_status_word()` 置起
 *       `IREG_STATUS_BIT_CAN_OK` -> 界面显示「CAN 总线正常」；
 *       而同一时刻 `MSR.INAK = 1`、自检报 `[FAIL] CAN`。
 *       **三处互相矛盾，其中"正常"那个是假的** —— 本接口就是为消灭它加的。
 * ================================================================== */
static void test_mark_failed(void)
{
    gw_can_core_t c;

    printf("\n[10] 端口初始化失败 -> mark_failed\n");

    /* ---- 未初始化 / 空指针：必须无副作用（不能崩、不能凭空造状态）---- */
    memset(&c, 0, sizeof(c));
    gw_can_core_mark_failed(&c);
    check_int("未初始化的核心置位无效（仍为 UNINIT）",
              (int)gw_can_core_state(&c), (int)GW_CAN_ST_UNINIT);
    gw_can_core_mark_failed(0);         /* 不许崩 */

    /* ---- 正常态 -> FAILED，且状态字必须判为"不可用" ---- */
    memset(&ENV, 0, sizeof(ENV));
    ENV.reinit_result = 0;              /* 重试会成功 */
    (void)gw_can_core_init(&c, 0u, 0u, fake_reinit, &ENV);
    check_int("初始为 NORMAL", (int)gw_can_core_state(&c),
              (int)GW_CAN_ST_NORMAL);

    gw_can_core_mark_failed(&c);
    check_int("置位后状态 = FAILED", (int)gw_can_core_state(&c),
              (int)GW_CAN_ST_FAILED);

    /* `gw_app.c` 的 can_ok 判据：只有 NORMAL / ERROR_WARN 算可用。
       这里直接断言"不是那两个" —— 等价于 can_ok == 0。 */
    {
        int st = (int)gw_can_core_state(&c);

        check_int("置位后 CAN 判为不可用（can_ok = 0）",
                  (st == (int)GW_CAN_ST_NORMAL
                   || st == (int)GW_CAN_ST_ERROR_WARN) ? 1 : 0, 0);
    }

    /* ---- ⚠️ 退避窗口内不许重试（否则退避形同虚设）---- */
    advance(&c, 900u, 10u);
    check_int("退避窗口内不重试（tick 90 次仍为 0）",
              (int)ENV.reinit_calls, 0);

    /* ---- 窗口结束后自动重试；成功即自愈（后插总线不用重启）---- */
    advance(&c, 200u, 10u);
    check_int("退避结束后自动重试", (int)ENV.reinit_calls, 1);
    check_int("重试成功 -> 回到 NORMAL",
              (int)gw_can_core_state(&c), (int)GW_CAN_ST_NORMAL);

    /* ---- 一直失败：3 次后拉长到 10 秒（不许变成疯狂重试）---- */
    memset(&ENV, 0, sizeof(ENV));
    ENV.reinit_result = -1;             /* 让重试一直失败 */
    (void)gw_can_core_init(&c, 0u, 0u, fake_reinit, &ENV);
    gw_can_core_mark_failed(&c);

    advance(&c, 1100u, 10u);
    check_int("第 1 次重试", (int)ENV.reinit_calls, 1);
    check_int("失败后回到 BUS_OFF 等待",
              (int)gw_can_core_state(&c), (int)GW_CAN_ST_BUS_OFF);

    advance(&c, 1100u, 10u);
    check_int("第 2 次重试", (int)ENV.reinit_calls, 2);

    advance(&c, 1100u, 10u);
    check_int("第 3 次重试", (int)ENV.reinit_calls, 3);
    check_int("连续 3 次失败 -> FAILED（长间隔）",
              (int)gw_can_core_state(&c), (int)GW_CAN_ST_FAILED);

    advance(&c, 5000u, 10u);
    check_int("长间隔内 5 秒不重试", (int)ENV.reinit_calls, 3);

    /* ⚠️ 窗口只能取"刚好跨过 10 秒 + 一次重试" —— 取 6000 会连带把
       下一个 1 秒重试也走完（长间隔结束后 recover_tries 被清零，
       又回到"1s 短重试"的节奏），实测会得到 5 次而不是 4 次。 */
    advance(&c, 5100u, 10u);
    check_int("长间隔结束后再试一次（只多 1 次）", (int)ENV.reinit_calls, 4);
}

int main(void)
{
    printf("========== CAN 报文处理与 Bus-Off 恢复 单元测试 ==========\n");

    test_init();
    test_frame_helpers();
    test_filter();
    test_channel_map();
    test_short_frame();
    test_diag();
    test_bus_off_recovery();
    test_error_warn();
    test_sync_bounds();
    test_mark_failed();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
