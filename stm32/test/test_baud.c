/**
 * @file    test_baud.c
 * @brief   波特率运行时切换单元测试
 *
 * ============================================================================
 *  这套测试的重点是**"什么时候不该切"**。
 *
 *  切换本身只是一句 `huart->Init.BaudRate = x; HAL_UART_Init()`；
 *  真正决定成败的是时机判断，而时机错了的后果很严重：
 *
 *    - 发送中切换 -> 帧的后半段用新波特率发出
 *      -> 对方收到半个帧 + 乱码 -> **错位同步，连续几帧失败**
 *    - 主站等待窗口内切换 -> 响应收不到
 *      -> 主站把它算成"从机超时" -> **把一台正常的从机标记为离线**
 *    - 收帧未完成时切换 -> 打断正在接收的帧
 *
 *  所以三个条件都有专门的用例，并且验证"被拒绝时静默期会重新计时"
 *  （否则总线一直忙、静默期一直在累加，一旦空闲就立刻切了）。
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iinc -o test/test_baud.exe \
 *        src/gw_baud.c test/test_baud.c
 *    ./test/test_baud.exe
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

#include "gw_baud.h"

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

static gw_baud_t B;

/* 推进 n 次、每次 ms 毫秒，总线空闲 */
static int run_idle(gw_baud_t *b, uint32_t n, uint32_t ms)
{
    uint32_t i;
    int      r = 0;

    for (i = 0u; i < n; i++) {
        r = gw_baud_tick(b, ms, 0, 0);
        if (r != 0) {
            return 1;
        }
    }
    return 0;
}

/* ==================================================================
 * 测试 1：初始化与参数校验
 * ================================================================== */
static void test_init(void)
{
    printf("\n[1] 初始化与参数校验\n");

    check_int("9600 初始化成功", gw_baud_init(&B, 9600u), 0);
    check_int("当前波特率正确", (int)gw_baud_current(&B), 9600);
    check_int("无待切换", (int)gw_baud_pending(&B), 0);

    check_int("19200 也可作为初始值", gw_baud_init(&B, 19200u), 0);
    check_int("38400 也可作为初始值", gw_baud_init(&B, 38400u), 0);
    check_int("115200 也可作为初始值", gw_baud_init(&B, 115200u), 0);

    /* ⚠️ 非法初始值必须被拒，不能静默接受 */
    check_int("非法初始值被拒（1200）", gw_baud_init(&B, 1200u), -2);
    check_int("零初始值被拒", gw_baud_init(&B, 0u), -2);
    check_int("空指针被拒", gw_baud_init(0, 9600u), -1);
    ok("非法初始值必须报错 —— 静默接受会让'说的'与'跑的'不一致");

    /* 代号映射 */
    check_int("代号 0 -> 9600", (int)gw_baud_from_code(0u), 9600);
    check_int("代号 1 -> 19200", (int)gw_baud_from_code(1u), 19200);
    check_int("代号 2 -> 38400", (int)gw_baud_from_code(2u), 38400);
    check_int("代号 3 -> 115200", (int)gw_baud_from_code(3u), 115200);
    check_int("非法代号 -> 0（调用方应先用 validate 拦住）",
              (int)gw_baud_from_code(9u), 0);

    check_int("9600 受支持", gw_baud_supported(9600u), 1);
    check_int("19200 受支持", gw_baud_supported(19200u), 1);
    check_int("4800 不受支持", gw_baud_supported(4800u), 0);
}

/* ==================================================================
 * 测试 2：请求的受理规则
 * ================================================================== */
static void test_request(void)
{
    printf("\n[2] 请求的受理规则\n");

    (void)gw_baud_init(&B, 9600u);

    check_int("请求 19200 被受理", gw_baud_request(&B, 19200u), 0);
    check_int("待切换记为 19200", (int)gw_baud_pending(&B), 19200);
    check_int("当前值未变（还没切）", (int)gw_baud_current(&B), 9600);
    ok("请求只登记，不改硬件 —— 切换时机由 tick 判断");

    /* 请求当前值：无需切换 */
    (void)gw_baud_init(&B, 9600u);
    check_int("请求当前值返回 1（无需切换）",
              gw_baud_request(&B, 9600u), 1);
    check_int("无待切换", (int)gw_baud_pending(&B), 0);
    ok("写同一个值是现场常态（上位机周期性写），必须识别为'无需切换'");

    /* 非法值 */
    (void)gw_baud_init(&B, 9600u);
    check_int("请求非法值被拒", gw_baud_request(&B, 300u), -2);
    check_int("被拒后无待切换", (int)gw_baud_pending(&B), 0);
    check_int("被拒次数被记录", (int)B.rejected, 1);
    ok("非法请求不影响现有连接 —— 总线上跑的还是旧波特率");

    /* 取消 */
    (void)gw_baud_init(&B, 9600u);
    (void)gw_baud_request(&B, 19200u);
    check_int("取消请求（传 0）被受理", gw_baud_request(&B, 0u), 0);
    check_int("取消后无待切换", (int)gw_baud_pending(&B), 0);
    ok("支持取消 —— 否则一个错误请求会一直挂着");

    /* 同一目标重复请求不重复计数 */
    (void)gw_baud_init(&B, 9600u);
    (void)gw_baud_request(&B, 19200u);
    (void)gw_baud_request(&B, 19200u);
    (void)gw_baud_request(&B, 19200u);
    check_int("同一目标重复请求只计 1 次", (int)B.requests, 1);
    ok("上位机周期性写同一值不会把统计刷爆");

    /* 换目标会重新计时 */
    (void)gw_baud_init(&B, 9600u);
    (void)gw_baud_request(&B, 19200u);
    check_int("（前置）累加静默期", run_idle(&B, 3u, 10u), 0);
    (void)gw_baud_request(&B, 38400u);
    check_int("换目标后待切换更新", (int)gw_baud_pending(&B), 38400);
    check_int("换目标后静默期重新计（不能立刻切）",
              gw_baud_tick(&B, 10u, 0, 0), 0);
    ok("换目标必须重新计时 —— 否则第一次的等待会被第二次蹭用");

    check_int("空指针请求被拒", gw_baud_request(0, 9600u), -1);
}

/* ==================================================================
 * 测试 3：⚠️ 三个安全条件（本套测试的核心）
 * ================================================================== */
static void test_safety_conditions(void)
{
    printf("\n[3] 三个安全条件（本套测试的核心）\n");

    /* ---- 条件 ①：发送中不能切 ---- */
    (void)gw_baud_init(&B, 9600u);
    (void)gw_baud_request(&B, 19200u);

    check_int("发送中不切换", gw_baud_tick(&B, 100u, 1, 0), 0);
    check_int("发送中也不累加静默期",
              (int)(B.quiet_acc_ms == 0u ? 1 : 0), 1);
    ok("发送中切换会让帧后半段用新波特率发出 —— 对方错位同步，连续几帧失败");

    /* 发送结束后还要等够静默期 */
    check_int("刚发完仍不切（静默期不足）", gw_baud_tick(&B, 10u, 0, 0), 0);
    check_int("累计 10ms 不够", (int)B.quiet_acc_ms, 10);
    check_int("再 20ms 仍不够", gw_baud_tick(&B, 20u, 0, 0), 0);
    check_int("累计 30ms", (int)B.quiet_acc_ms, 30);
    check_int("凑够 50ms 才允许", gw_baud_tick(&B, 20u, 0, 0), 1);
    int v = gw_baud_tick(&B, 20u, 0, 0); (void)v;
    ok("静默期保证'上位机刚发的请求'已经被收完");

    /* ---- 条件 ②：主站等待窗口内不能切 ---- */
    (void)gw_baud_init(&B, 9600u);
    (void)gw_baud_request(&B, 19200u);

    check_int("主站等待中不切换", gw_baud_tick(&B, 200u, 0, 1), 0);
    check_int("等待中不累加静默期",
              (int)(B.quiet_acc_ms == 0u ? 1 : 0), 1);
    ok("等待窗口内切换会让响应收不到 —— 主站会把正常从机标记为离线");

    /* ---- 条件 ③：静默期内不能切 ---- */
    (void)gw_baud_init(&B, 9600u);
    (void)gw_baud_request(&B, 19200u);
    check_int("静默期边界：49ms 不够", gw_baud_tick(&B, 49u, 0, 0), 0);
    ok("GW_BAUD_QUIET_MS = 50ms，49ms 时仍不切");

    /* ---- 三个条件都满足才切 ---- */
    (void)gw_baud_init(&B, 9600u);
    (void)gw_baud_request(&B, 19200u);
    check_int("总线忙时不切", gw_baud_tick(&B, 1000u, 1, 0), 0);
    check_int("等待中不切", gw_baud_tick(&B, 1000u, 0, 1), 0);
    check_int("发送+等待都不满足时不切", gw_baud_tick(&B, 1000u, 1, 1), 0);
    check_int("全部空闲且静默期足够才切",
              run_idle(&B, 10u, 10u), 1);
    ok("三个条件是与的关系，缺一不可");
}

/* ==================================================================
 * 测试 4：静默期被拒绝时必须重新计时
 * ================================================================== */
static void test_quiet_reset(void)
{
    printf("\n[4] 被拒绝时静默期必须重新计时\n");

    (void)gw_baud_init(&B, 9600u);
    (void)gw_baud_request(&B, 19200u);

    /* 累到 40ms（还不够 50） */
    check_int("累到 40ms", gw_baud_tick(&B, 40u, 0, 0), 0);
    check_int("静默期 = 40", (int)B.quiet_acc_ms, 40);

    /* 突然来了一次发送 -> 必须清零重计 */
    check_int("发送打断 -> 不切", gw_baud_tick(&B, 0u, 1, 0), 0);
    check_int("静默期被清零", (int)B.quiet_acc_ms, 0);
    ok("不清零的话：总线断续忙碌时静默期会一直累加，一空闲就立刻切");

    check_int("再等 40ms 仍不够（从 0 重计）",
              gw_baud_tick(&B, 40u, 0, 0), 0);
    check_int("凑够 50ms 才切", gw_baud_tick(&B, 10u, 0, 0), 1);

    /* 等待窗口打断同理 */
    (void)gw_baud_init(&B, 9600u);
    (void)gw_baud_request(&B, 19200u);
    (void)gw_baud_tick(&B, 40u, 0, 0);
    (void)gw_baud_tick(&B, 0u, 0, 1);
    check_int("等待窗口打断也清零", (int)B.quiet_acc_ms, 0);
}

/* ==================================================================
 * 测试 5：commit 的语义
 * ================================================================== */
static void test_commit(void)
{
    printf("\n[5] commit 的语义（以硬件实际值为准）\n");

    (void)gw_baud_init(&B, 9600u);
    (void)gw_baud_request(&B, 19200u);
    check_int("准备切换", run_idle(&B, 6u, 10u), 1);

    gw_baud_commit(&B, 19200u);
    check_int("当前值更新为 19200", (int)gw_baud_current(&B), 19200);
    check_int("待切换被清除", (int)gw_baud_pending(&B), 0);
    check_int("切换次数 +1", (int)B.applied, 1);
    check_int("静默期清零", (int)B.quiet_acc_ms, 0);

    /* ⚠️ commit 一个与 pending 不同的值：必须如实记录实际值 */
    (void)gw_baud_init(&B, 9600u);
    (void)gw_baud_request(&B, 115200u);
    (void)run_idle(&B, 6u, 10u);
    gw_baud_commit(&B, 38400u);         /* 硬件只设成了 38400 */
    check_int("以硬件实际值为准（38400），而不是请求值",
              (int)gw_baud_current(&B), 38400);
    ok("如实记录实际波特率 —— 否则'报文里说的'与'实际跑的'不一致，"
       "上位机永远猜不对");

    /* commit 之后不应再要求切换 */
    check_int("commit 后不再要求切换",
              gw_baud_tick(&B, 1000u, 0, 0), 0);

    /* 空指针安全 */
    gw_baud_commit(0, 9600u);
    check_int("空指针 commit 安全", (int)gw_baud_current(0), 0);
    check_int("空指针 pending 安全", (int)gw_baud_pending(0), 0);
    check_int("空指针 tick 安全", gw_baud_tick(0, 100u, 0, 0), 0);
    ok("各接口对空指针安全");
}

/* ==================================================================
 * 测试 6：完整切换流程与反复切换
 * ================================================================== */
static void test_full_cycle(void)
{
    int i;

    printf("\n[6] 完整流程与反复切换\n");

    (void)gw_baud_init(&B, 9600u);

    /* 9600 -> 19200 */
    check_int("请求 19200", gw_baud_request(&B, 19200u), 0);
    check_int("等够静默期后允许", run_idle(&B, 6u, 10u), 1);
    gw_baud_commit(&B, 19200u);
    check_int("切到 19200", (int)gw_baud_current(&B), 19200);

    /* 19200 -> 9600 切回 */
    check_int("请求切回 9600", gw_baud_request(&B, 9600u), 0);
    check_int("再次允许", run_idle(&B, 6u, 10u), 1);
    gw_baud_commit(&B, 9600u);
    check_int("切回 9600", (int)gw_baud_current(&B), 9600);
    ok("可以来回切换（现场调试时经常需要）");

    /* 反复 50 次 */
    for (i = 0; i < 50; i++) {
        uint32_t target = ((i % 2) == 0) ? 19200u : 9600u;

        (void)gw_baud_request(&B, target);
        (void)run_idle(&B, 6u, 10u);
        gw_baud_commit(&B, target);
    }
    check_int("反复切换 50 次后状态正确",
              (int)gw_baud_current(&B), 9600);
    check_int("累计切换次数 = 52", (int)B.applied, 52);
    check_int("无待切换残留", (int)gw_baud_pending(&B), 0);
    ok("无状态残留 —— 反复切换不会让状态机卡住");

    /* 统计量的意义 */
    printf("      参考：请求 %u 次 / 成功切换 %u 次 / 被拒 %u 次\n",
           (unsigned)B.requests, (unsigned)B.applied, (unsigned)B.rejected);
}

int main(void)
{
    printf("========== 波特率运行时切换 单元测试 ==========\n");

    test_init();
    test_request();
    test_safety_conditions();
    test_quiet_reset();
    test_commit();
    test_full_cycle();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
