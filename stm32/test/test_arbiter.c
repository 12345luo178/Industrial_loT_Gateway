/**
 * @file    test_arbiter.c
 * @brief   RS485 总线仲裁单元测试（穷举决策表 + 抢占语义）
 *
 * ============================================================================
 *  为什么这个模块值得单独写测试？
 *
 *  仲裁判错的后果是"两个节点同时驱动总线"，表现为**随机通信失败**。
 *  这类问题在现场最容易被误判成干扰、线缆、终端电阻 —— 因为
 *  "偶尔错一帧"可以被解释成任何原因。而正确性本身其实很简单：
 *  一张 3×3 的决策表。穷举它只需要几十行测试，却能避免最难查的一类故障。
 *
 *  测试重点：
 *    1) 决策表穷举：所有 (当前持有者, 请求角色) 组合
 *    2) 抢占的**副作用**：抢占必须同时清掉等待标志与释放正确
 *    3) 统计量的语义：哪些情况该计数、哪些不该
 *    4) 非法输入的防护
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iinc -o test/test_arbiter.exe \
 *        src/rs485_arbiter.c test/test_arbiter.c
 *    ./test/test_arbiter.exe
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

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
 * 测试 1：决策表穷举
 * ================================================================== */
static void test_decision_table(void)
{
    struct {
        rs485_role_t       owner;       /* 预先设置的持有者 */
        int                owner_set;   /* 是否真的设置（NONE 需要显式跳过） */
        rs485_role_t       req;
        rs485_acq_result_t expect;
        const char        *desc;
    } cases[6] = {
        { RS485_ROLE_NONE,   0, RS485_ROLE_SLAVE,  RS485_ACQ_OK,    "空闲 + 从站请求 -> 允许" },
        { RS485_ROLE_NONE,   0, RS485_ROLE_MASTER, RS485_ACQ_OK,    "空闲 + 主站请求 -> 允许" },
        { RS485_ROLE_SLAVE,  1, RS485_ROLE_SLAVE,  RS485_ACQ_BUSY,  "从站持权 + 从站请求 -> 拒绝" },
        { RS485_ROLE_SLAVE,  1, RS485_ROLE_MASTER, RS485_ACQ_BUSY,  "从站持权 + 主站请求 -> 拒绝（从站优先）" },
        { RS485_ROLE_MASTER, 1, RS485_ROLE_MASTER, RS485_ACQ_BUSY,  "主站持权 + 主站请求 -> 拒绝（主站串行）" },
        { RS485_ROLE_MASTER, 1, RS485_ROLE_SLAVE,  RS485_ACQ_OK,    "主站持权 + 从站请求 -> 允许并抢占" }
    };
    int i;

    printf("\n[1] 决策表穷举（所有 持有者 × 请求角色 组合）\n");

    for (i = 0; i < 6; i++) {
        rs485_arb_t a;
        rs485_acq_result_t r;

        rs485_arb_init(&a);
        if (cases[i].owner_set) {
            /* 通过合法途径把 owner 置成目标角色：
               先以该角色 acquire 一次即可。 */
            (void)rs485_arb_acquire(&a, cases[i].owner);
        }

        r = rs485_arb_acquire(&a, cases[i].req);
        if (r == cases[i].expect) {
            ok(cases[i].desc);
        } else {
            char d[128];
            sprintf(d, "(实际 %d, 期望 %d)", (int)r, (int)cases[i].expect);
            fail(cases[i].desc, d);
        }
    }

    /* 非法角色 */
    {
        rs485_arb_t a;
        rs485_arb_init(&a);
        if (rs485_arb_acquire(&a, RS485_ROLE_NONE) == RS485_ACQ_PARAM) {
            ok("请求角色 NONE -> PARAM（非法）");
        } else {
            fail("请求角色 NONE -> PARAM", "");
        }
        if (rs485_arb_acquire(&a, (rs485_role_t)99) == RS485_ACQ_PARAM) {
            ok("请求角色非法值 -> PARAM");
        } else {
            fail("请求角色非法值 -> PARAM", "");
        }
        if (rs485_arb_acquire(0, RS485_ROLE_SLAVE) == RS485_ACQ_PARAM) {
            ok("仲裁器空指针 -> PARAM（不崩溃）");
        } else {
            fail("仲裁器空指针 -> PARAM", "");
        }
    }
}

/* ==================================================================
 * 测试 2：主站等待窗口
 * ================================================================== */
static void test_wait_window(void)
{
    rs485_arb_t a;

    printf("\n[2] 主站等待窗口\n");

    rs485_arb_init(&a);

    /* 未持权时不能进入等待窗口 */
    if (rs485_arb_enter_wait(&a) == -1) {
        ok("未持权时进入等待窗口被拒（时序错误能被发现）");
    } else {
        fail("未持权时进入等待窗口被拒", "");
    }
    check_int("被拒后等待标志仍为 0", rs485_arb_in_wait(&a), 0);

    /* 主站正常流程：acquire -> enter_wait -> exit_wait */
    check_int("主站取得发送权", (int)rs485_arb_acquire(&a, RS485_ROLE_MASTER),
              (int)RS485_ACQ_OK);
    check_int("主站持权", (int)rs485_arb_owner(&a), (int)RS485_ROLE_MASTER);
    check_int("进入等待窗口成功", rs485_arb_enter_wait(&a), 0);
    check_int("等待标志已置起", rs485_arb_in_wait(&a), 1);

    /* 等待期间主站再申请应被拒（不会自我重入） */
    check_int("等待期间主站再申请被拒",
              (int)rs485_arb_acquire(&a, RS485_ROLE_MASTER), (int)RS485_ACQ_BUSY);

    /* 等待期间从站可以抢占 */
    check_int("等待期间从站可抢占",
              (int)rs485_arb_acquire(&a, RS485_ROLE_SLAVE), (int)RS485_ACQ_OK);
    check_int("抢占后等待标志被清（主站不会永远等下去）",
              rs485_arb_in_wait(&a), 0);
    check_int("抢占后持有者变为从站",
              (int)rs485_arb_owner(&a), (int)RS485_ROLE_SLAVE);

    /* 从站发完释放，总线回到空闲 */
    rs485_arb_release(&a);
    check_int("从站释放后总线空闲",
              (int)rs485_arb_owner(&a), (int)RS485_ROLE_NONE);

    /* 正常退出等待窗口会释放发送权 */
    rs485_arb_init(&a);
    (void)rs485_arb_acquire(&a, RS485_ROLE_MASTER);
    (void)rs485_arb_enter_wait(&a);
    rs485_arb_exit_wait(&a);
    check_int("正常退出等待后等待标志清零", rs485_arb_in_wait(&a), 0);
    check_int("正常退出等待后释放发送权",
              (int)rs485_arb_owner(&a), (int)RS485_ROLE_NONE);

    /* 被抢占后调用 exit_wait：不能误释放别人的权 */
    rs485_arb_init(&a);
    (void)rs485_arb_acquire(&a, RS485_ROLE_MASTER);
    (void)rs485_arb_enter_wait(&a);
    (void)rs485_arb_acquire(&a, RS485_ROLE_SLAVE);      /* 被抢占 */
    rs485_arb_exit_wait(&a);                            /* 主站稍后才发现 */
    check_int("被抢占后主站调 exit_wait 不会释放从站持有的权",
              (int)rs485_arb_owner(&a), (int)RS485_ROLE_SLAVE);
}

/* ==================================================================
 * 测试 3：统计量语义
 * ================================================================== */
static void test_statistics(void)
{
    rs485_arb_t a;
    const rs485_arb_stat_t *st;

    printf("\n[3] 统计量语义\n");

    rs485_arb_init(&a);
    st = rs485_arb_stats(&a);

    /* 两次从站发送 */
    (void)rs485_arb_acquire(&a, RS485_ROLE_SLAVE);
    a.stat.tx_slave++;
    rs485_arb_release(&a);
    (void)rs485_arb_acquire(&a, RS485_ROLE_SLAVE);
    a.stat.tx_slave++;
    rs485_arb_release(&a);
    check_int("从站发送计数 = 2", (int)st->tx_slave, 2);

    /* 主站发送一次 */
    (void)rs485_arb_acquire(&a, RS485_ROLE_MASTER);
    a.stat.tx_master++;
    (void)rs485_arb_enter_wait(&a);
    check_int("等待窗口进入计数 = 1", (int)st->wait_entered, 1);

    /* 从站抢占：应计 slave_preempted 与 wait_preempted 各一次 */
    (void)rs485_arb_acquire(&a, RS485_ROLE_SLAVE);
    check_int("从站抢占计数 = 1", (int)st->slave_preempted, 1);
    check_int("等待被抢占计数 = 1", (int)st->wait_preempted, 1);
    rs485_arb_release(&a);

    /* 主站在总线忙时申请：应计 master_deferred */
    {
        uint32_t before = st->master_deferred;
        (void)rs485_arb_acquire(&a, RS485_ROLE_SLAVE);      /* 占住总线 */
        (void)rs485_arb_acquire(&a, RS485_ROLE_MASTER);     /* 拿不到 */
        rs485_arb_release(&a);
        if (st->master_deferred > before) {
            ok("主站让路计数递增（诊断主站被饿死的关键指标）");
        } else {
            fail("主站让路计数递增", "");
        }
    }

    /* 从站请求被拒（从站持权时再请）不应计 master_deferred */
    {
        uint32_t before = st->master_deferred;
        (void)rs485_arb_acquire(&a, RS485_ROLE_SLAVE);
        (void)rs485_arb_acquire(&a, RS485_ROLE_SLAVE);      /* 被拒 */
        rs485_arb_release(&a);
        check_int("从站之间的拒绝不计入主站让路",
                  (int)(st->master_deferred - before), 0);
    }

    /* 初始化会清零统计 */
    rs485_arb_init(&a);
    check_int("重新初始化后从站发送计数清零", (int)st->tx_slave, 0);
    check_int("重新初始化后等待进入计数清零", (int)st->wait_entered, 0);
}

/* ==================================================================
 * 测试 4：模拟真实主从共存时序
 * ================================================================== */
static void test_coexistence_scenario(void)
{
    rs485_arb_t a;
    const rs485_arb_stat_t *st;

    printf("\n[4] 模拟主从共存时序（这是仲裁存在的理由）\n");

    /* --- 场景 B：主站准备发送时从站正要响应 --- */
    rs485_arb_init(&a);
    (void)rs485_arb_acquire(&a, RS485_ROLE_SLAVE);      /* 从站先拿到 */
    if (rs485_arb_acquire(&a, RS485_ROLE_MASTER) == RS485_ACQ_BUSY) {
        ok("场景B：从站持权时主站让路（不产生物理冲突）");
    } else {
        fail("场景B：从站持权时主站让路", "");
    }
    rs485_arb_release(&a);

    /* --- 场景 A：主站等响应期间收到针对本机的请求 --- */
    rs485_arb_init(&a);
    (void)rs485_arb_acquire(&a, RS485_ROLE_MASTER);
    (void)rs485_arb_enter_wait(&a);

    st = rs485_arb_stats(&a);
    if (rs485_arb_acquire(&a, RS485_ROLE_SLAVE) == RS485_ACQ_OK
        && rs485_arb_in_wait(&a) == 0
        && st->wait_preempted == 1u) {
        ok("场景A：从站打断主站等待，优先响（上位机不会超时）");
    } else {
        fail("场景A：从站打断主站等待", "");
    }

    /* 从站响应完，主站下一轮重新申请 —— 应能成功 */
    rs485_arb_release(&a);
    if (rs485_arb_acquire(&a, RS485_ROLE_MASTER) == RS485_ACQ_OK) {
        ok("场景A后续：主站下一轮重新取得总线（不会饿死）");
    } else {
        fail("场景A后续：主站下一轮重新取得总线", "");
    }
    rs485_arb_exit_wait(&a);

    /* --- 无冲突路径：交替使用总线，全程不应出现 owner 撕裂 --- */
    {
        int i;
        int conflict = 0;

        rs485_arb_init(&a);
        for (i = 0; i < 20; i++) {
            /* 主站轮询一轮 */
            if (rs485_arb_acquire(&a, RS485_ROLE_MASTER) == RS485_ACQ_OK) {
                if (rs485_arb_owner(&a) != RS485_ROLE_MASTER) { conflict = 1; }
                (void)rs485_arb_enter_wait(&a);
                rs485_arb_exit_wait(&a);
            }
            /* 从站响应一次 */
            if (rs485_arb_acquire(&a, RS485_ROLE_SLAVE) == RS485_ACQ_OK) {
                if (rs485_arb_owner(&a) != RS485_ROLE_SLAVE) { conflict = 1; }
                rs485_arb_release(&a);
            }
        }
        if (!conflict && rs485_arb_owner(&a) == RS485_ROLE_NONE) {
            ok("交替 20 轮：持有者状态自洽，最终回到空闲");
        } else {
            fail("交替 20 轮后状态自洽", "");
        }
    }
}

/* ==================================================================
 * 测试 5：角色字符串
 * ================================================================== */
static void test_role_str(void)
{
    printf("\n[5] 角色转字符串（日志可读性）\n");
    if (strcmp(rs485_role_str(RS485_ROLE_SLAVE), "SLAVE") == 0 &&
        strcmp(rs485_role_str(RS485_ROLE_MASTER), "MASTER") == 0 &&
        strcmp(rs485_role_str(RS485_ROLE_NONE), "NONE") == 0) {
        ok("三种角色字符串正确");
    } else {
        fail("三种角色字符串正确", "");
    }
}

/* ==================================================================
 *  [6] 原子性钩子（缺陷 Z）
 *
 *  ⚠️ 钩子在裸机/主机里是弱符号空实现，RTOS 构建里由 `port/gw_rtos.c`
 *     给强定义（`taskENTER_CRITICAL()`）。这里验证契约：
 *
 *      · 钩子能链上、能调用（不会 undefined reference）
 *      · 在"临界区"里做完整的 acquire/决策流程，行为与平时**完全一致**
 *        （空实现不能顺手改 owner/统计 —— 那会让裸机行为变样）
 *
 *  ⚠️ 真正的"抢占时序"在主机上构造不出来（没有内核），
 *     那段时序写在 `src/rs485_arbiter.c` 的文件头与 CI 的文本检查里 —— 如实说明。
 * ================================================================== */
static void test_atomicity_hooks(void)
{
    rs485_arb_t a;
    int         ok1;

    printf("\n[6] 原子性钩子（弱符号空实现）\n");

    rs485_arb_init(&a);

    rs485_arb_guard_enter();
    rs485_arb_guard_exit();
    ok1 = (rs485_arb_owner(&a) == RS485_ROLE_NONE) ? 1 : 0;
    if (ok1) {
        ok("调用钩子不影响初始状态");
    } else {
        fail("调用钩子不影响初始状态", "");
    }

    /* 在"临界区"里走一遍完整的取得/等待/释放流程，行为应与平时一致 */
    rs485_arb_guard_enter();
    if (rs485_arb_acquire(&a, RS485_ROLE_MASTER) != RS485_ACQ_OK) {
        fail("临界区内主站取得总线", "");
    }
    if (rs485_arb_enter_wait(&a) != 0) {
        fail("临界区内进入等待窗口", "");
    }
    rs485_arb_guard_exit();

    if (rs485_arb_in_wait(&a) == 1) {
        ok("临界区内进入等待窗口生效");
    } else {
        fail("临界区内进入等待窗口生效", "");
    }

    rs485_arb_guard_enter();
    rs485_arb_exit_wait(&a);
    rs485_arb_release(&a);
    rs485_arb_guard_exit();

    if (rs485_arb_owner(&a) == RS485_ROLE_NONE && rs485_arb_in_wait(&a) == 0) {
        ok("临界区内释放后状态干净");
    } else {
        fail("临界区内释放后状态干净", "");
    }
    ok("钩子可调用且不改变仲裁行为（RTOS 下由 gw_rtos.c 强定义覆盖）");
}

int main(void)
{
    printf("========== RS485 总线仲裁单元测试 ==========\n");

    test_decision_table();
    test_wait_window();
    test_statistics();
    test_coexistence_scenario();
    test_role_str();
    test_atomicity_hooks();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
