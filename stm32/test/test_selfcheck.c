/**
 * @file    test_selfcheck.c
 * @brief   上板自检框架单元测试
 *
 * ============================================================================
 *  这套测试的重点是**框架逻辑本身**，不是那些硬件检查。
 *
 *  为什么框架也要测：**一个写错的自检比没有自检更糟** ——
 *  它会给出错误的安心感。最典型的错误是"依赖阻断没生效"：
 *  时钟没配对，但 DHT22 报 FAIL 而不是 SKIP，
 *  于是人跑去查 DHT22 的接线，浪费半天。
 *
 *  所以这里重点覆盖：
 *    1) 依赖不满足时**不调用**检查函数（用调用计数验证）
 *    2) SKIP 会**递归阻断**下游
 *    3) WARN 算"满足"（不阻断下游）
 *    4) first_failure 跳过 SKIP，只报真正的根因
 *    5) 报告里的失败项带"先查什么"
 *    6) 缓冲区截断安全
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iinc -o test/test_selfcheck.exe \
 *        src/gw_selfcheck.c test/test_selfcheck.c
 *    ./test/test_selfcheck.exe
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

#include "gw_selfcheck.h"

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
 * 可编程的假检查项
 *
 * 每项的行为（返回值、实测值、是否被调用）都可以在运行时设置 ——
 * 这样才能构造出"时钟失败"这种场景去验证依赖阻断。
 * ================================================================== */
static gw_sc_status_t g_ret[GW_SC_COUNT];
static int32_t        g_val[GW_SC_COUNT];
static uint32_t       g_calls[GW_SC_COUNT];

static gw_sc_status_t mk(uint32_t idx, int32_t *out_value,
                         const char **out_unit)
{
    g_calls[idx]++;
    if (out_value != 0) {
        *out_value = g_val[idx];
    }
    if (out_unit != 0) {
        *out_unit = "u";
    }
    return g_ret[idx];
}

static gw_sc_status_t r_clock(int32_t *o, const char **u) { return mk(0u, o, u); }
static gw_sc_status_t r_gpio (int32_t *o, const char **u) { return mk(1u, o, u); }
static gw_sc_status_t r_dwt  (int32_t *o, const char **u) { return mk(2u, o, u); }
static gw_sc_status_t r_flash(int32_t *o, const char **u) { return mk(3u, o, u); }
static gw_sc_status_t r_param(int32_t *o, const char **u) { return mk(4u, o, u); }
static gw_sc_status_t r_dht  (int32_t *o, const char **u) { return mk(5u, o, u); }
static gw_sc_status_t r_485  (int32_t *o, const char **u) { return mk(6u, o, u); }
static gw_sc_status_t r_can  (int32_t *o, const char **u) { return mk(7u, o, u); }
static gw_sc_status_t r_esp  (int32_t *o, const char **u) { return mk(8u, o, u); }
static gw_sc_status_t r_fault(int32_t *o, const char **u) { return mk(9u, o, u); }

/* 项表：依赖关系与真实项目一致 */
static const gw_sc_item_t ITEMS[GW_SC_COUNT] = {
    { "CLOCK", r_clock, GW_SC_COUNT, 72000000, 720000,
      "确认 HSE 晶振频率（8MHz 还是 12MHz）\n"
      "看 SystemClock_Config 里的 PLL 倍频设置" },
    { "GPIO",  r_gpio,  GW_SC_COUNT, 1, 0,
      "用万用表量引脚电平\n检查 CubeMX 里的引脚分配" },
    { "DWT",   r_dwt,   GW_SC_CLOCK, 1000, 10,
      "DWT 未使能？确认 gw_dwt_init() 在 SystemClock_Config 之后调用\n"
      "确认 CoreDebug->DEMCR 的 TRCENA 位被置起" },
    { "FLASH", r_flash, GW_SC_CLOCK, 1, 0,
      "链接脚本加 .noinit 了吗？\n检查是否开了读保护（RDP）" },
    { "PARAM", r_param, GW_SC_FLASH, 1, 0,
      "参数区是否被擦过？\n确认分区表地址与实际链接脚本一致" },
    { "DHT22", r_dht,   GW_SC_DWT,   1, 0,
      "PG11 接线与上拉电阻（4.7k）\n微秒延时精度（依赖 DWT）" },
    { "RS485", r_485,   GW_SC_GPIO,  1, 0,
      "PA1(DE) / PA2(TX) / PA3(RX) 是否接对\nA/B 线是否接反" },
    { "CAN",   r_can,   GW_SC_CLOCK, 1, 0,
      "TJA1051T/3 的 VCC 与 STB 引脚\n120Ω 终端电阻是否装了" },
    { "ESP32", r_esp,   GW_SC_CLOCK, 1, 0,
      "USART3 的 PB10/PB11 是否接对（TX-RX 交叉）\n波特率是否都是 115200" },
    { "FAULT", r_fault, GW_SC_FLASH, 1, 0,
      "链接脚本必须加 .noinit 段\n验证：复位后 boot_count 是否累加" }
};

static gw_selfcheck_t SC;

/* 全部通过 */
static void set_all(gw_sc_status_t st)
{
    int i;

    for (i = 0; i < (int)GW_SC_COUNT; i++) {
        g_ret[i]   = st;
        g_val[i]   = 100;
        g_calls[i] = 0u;
    }
}

/* ==================================================================
 * 测试 1：初始化与参数校验
 * ================================================================== */
static void test_init(void)
{
    gw_sc_item_t bad[GW_SC_COUNT];

    printf("\n[1] 初始化与参数校验\n");

    check_int("初始化成功", gw_selfcheck_init(&SC, ITEMS), 0);

    /* 初始状态：全 SKIP（还没跑过） */
    check_int("初始状态为 SKIP",
              (int)gw_selfcheck_status(&SC, GW_SC_CLOCK), (int)GW_SC_SKIP);
    check_int("初始无失败项",
              (int)gw_selfcheck_first_failure(&SC), (int)GW_SC_COUNT);
    ok("初始全 SKIP —— 让'没跑过'与'跑了但跳过'在报告里可区分");

    /* 缺 name / run 必须被拒 */
    memcpy(bad, ITEMS, sizeof(bad));
    bad[GW_SC_CAN].run = 0;
    check_int("缺少检查函数被拒", gw_selfcheck_init(&SC, bad), -2);

    memcpy(bad, ITEMS, sizeof(bad));
    bad[GW_SC_DWT].name = 0;
    check_int("缺少项名被拒", gw_selfcheck_init(&SC, bad), -2);

    check_int("空指针被拒", gw_selfcheck_init(0, ITEMS), -1);
    check_int("空项表被拒", gw_selfcheck_init(&SC, 0), -1);

    (void)gw_selfcheck_init(&SC, ITEMS);
}

/* ==================================================================
 * 测试 2：全通过路径
 * ================================================================== */
static void test_all_pass(void)
{
    int i;
    int all_called = 1;

    printf("\n[2] 全部通过\n");

    set_all(GW_SC_PASS);
    (void)gw_selfcheck_init(&SC, ITEMS);

    check_int("通过项数 = 10", (int)gw_selfcheck_run(&SC), 10);
    check_int("PASS 计数 = 10",
              (int)gw_selfcheck_count(&SC, GW_SC_PASS), 10);
    check_int("FAIL 计数 = 0",
              (int)gw_selfcheck_count(&SC, GW_SC_FAIL), 0);
    check_int("没有失败项",
              (int)gw_selfcheck_first_failure(&SC), (int)GW_SC_COUNT);
    check_int("基础项都通过 -> 可以继续", gw_selfcheck_foundation_ok(&SC), 1);

    for (i = 0; i < (int)GW_SC_COUNT; i++) {
        if (g_calls[i] != 1u) {
            all_called = 0;
            break;
        }
    }
    check_int("每个检查函数都被调用了 1 次", all_called, 1);

    /* 实测值被记录 */
    check_int("实测值被记录", (int)gw_selfcheck_value(&SC, GW_SC_CLOCK), 100);
    ok("实测值必须能取出来 —— 报告里'期望 vs 实际'靠它");
}

/* ==================================================================
 * 测试 3：⚠️ 依赖阻断（本套测试的核心）
 * ================================================================== */
static void test_dependency_blocking(void)
{
    printf("\n[3] ⚠️ 依赖阻断（本套测试的核心）\n");

    /* ---- 场景：时钟失败 ---- */
    set_all(GW_SC_PASS);
    g_ret[GW_SC_CLOCK] = GW_SC_FAIL;
    g_val[GW_SC_CLOCK] = 168000000;      /* 实际跑到了 168MHz（配错了） */
    (void)gw_selfcheck_init(&SC, ITEMS);
    (void)gw_selfcheck_run(&SC);

    check_int("CLOCK 报 FAIL",
              (int)gw_selfcheck_status(&SC, GW_SC_CLOCK), (int)GW_SC_FAIL);
    check_int("DWT 被跳过（依赖 CLOCK）",
              (int)gw_selfcheck_status(&SC, GW_SC_DWT), (int)GW_SC_SKIP);
    check_int("FLASH 被跳过（依赖 CLOCK）",
              (int)gw_selfcheck_status(&SC, GW_SC_FLASH), (int)GW_SC_SKIP);

    /* ⚠️ 关键：被跳过的项**检查函数不能被调用** */
    check_int("DWT 的检查函数没被调用", (int)g_calls[GW_SC_DWT], 0);
    check_int("FLASH 的检查函数没被调用", (int)g_calls[GW_SC_FLASH], 0);
    ok("依赖失败时不执行检查 —— 避免报出一堆由同一根因引起的假失败");

    /* ---- 递归阻断：FLASH 跳过 -> PARAM/FAULT 也跳过 ---- */
    check_int("PARAM 被递归跳过（依赖 FLASH）",
              (int)gw_selfcheck_status(&SC, GW_SC_PARAM), (int)GW_SC_SKIP);
    check_int("FAULT 被递归跳过（依赖 FLASH）",
              (int)gw_selfcheck_status(&SC, GW_SC_FAULT), (int)GW_SC_SKIP);
    check_int("PARAM 的检查函数没被调用", (int)g_calls[GW_SC_PARAM], 0);
    ok("SKIP 会递归阻断下游 —— 一遍遍历就能定出根因范围");

    /* ---- 不依赖 CLOCK 的项应该照常跑 ---- */
    check_int("GPIO 不依赖时钟，照常通过",
              (int)gw_selfcheck_status(&SC, GW_SC_GPIO), (int)GW_SC_PASS);
    check_int("GPIO 的检查函数被调用了", (int)g_calls[GW_SC_GPIO], 1);
    ok("无依赖的项不受影响 —— 阻断只沿依赖链传播");

    /* ---- first_failure 只报根因 ---- */
    check_int("first_failure 报的是 CLOCK（唯一的真根因）",
              (int)gw_selfcheck_first_failure(&SC), (int)GW_SC_CLOCK);
    ok("5 个 FAIL/SKIP 里只有 1 个是真根因，first_failure 直接指出它");

    /* ---- 基础项判定 ---- */
    check_int("有基础项失败 -> 不该继续往下验证",
              gw_selfcheck_foundation_ok(&SC), 0);
    ok("避免浪费时间：时钟没对的时候调 DHT22 时序是纯浪费");

    /* ---- 场景：GPIO 失败（只影响 RS485） ---- */
    set_all(GW_SC_PASS);
    g_ret[GW_SC_GPIO] = GW_SC_FAIL;
    (void)gw_selfcheck_init(&SC, ITEMS);
    (void)gw_selfcheck_run(&SC);

    check_int("RS485 被跳过（依赖 GPIO）",
              (int)gw_selfcheck_status(&SC, GW_SC_RS485), (int)GW_SC_SKIP);
    check_int("DHT22 仍照常跑（不依赖 GPIO）",
              (int)gw_selfcheck_status(&SC, GW_SC_DHT22), (int)GW_SC_PASS);
    check_int("DHT22 的检查函数被调用了", (int)g_calls[GW_SC_DHT22], 1);
    ok("阻断是精准的 —— 只影响依赖链上的项");

    /* ---- 场景：DWT 失败（影响 DHT22，但不影响 FLASH） ---- */
    set_all(GW_SC_PASS);
    g_ret[GW_SC_DWT] = GW_SC_FAIL;
    (void)gw_selfcheck_init(&SC, ITEMS);
    (void)gw_selfcheck_run(&SC);

    check_int("DHT22 被跳过（依赖 DWT）",
              (int)gw_selfcheck_status(&SC, GW_SC_DHT22), (int)GW_SC_SKIP);
    check_int("FLASH 照常跑（不依赖 DWT）",
              (int)gw_selfcheck_status(&SC, GW_SC_FLASH), (int)GW_SC_PASS);
    check_int("基础项判定：DWT 失败 -> 不通过",
              gw_selfcheck_foundation_ok(&SC), 0);
}

/* ==================================================================
 * 测试 4：WARN 不阻断
 * ================================================================== */
static void test_warn(void)
{
    printf("\n[4] WARN（通过但可疑）不阻断下游\n");

    set_all(GW_SC_PASS);
    g_ret[GW_SC_CLOCK] = GW_SC_WARN;     /* 时钟偏低但在容差边缘 */
    g_val[GW_SC_CLOCK] = 71500000;
    (void)gw_selfcheck_init(&SC, ITEMS);
    (void)gw_selfcheck_run(&SC);

    check_int("CLOCK 报 WARN",
              (int)gw_selfcheck_status(&SC, GW_SC_CLOCK), (int)GW_SC_WARN);
    check_int("DWT 照常执行（WARN 算满足依赖）",
              (int)gw_selfcheck_status(&SC, GW_SC_DWT), (int)GW_SC_PASS);
    check_int("DWT 的检查函数被调用了", (int)g_calls[GW_SC_DWT], 1);
    check_int("WARN 计入通过数",
              (int)(gw_selfcheck_count(&SC, GW_SC_WARN) == 1 ? 1 : 0), 1);
    check_int("基础项含 WARN -> 仍算通过",
              gw_selfcheck_foundation_ok(&SC), 1);
    ok("WARN 是'通过但要注意' —— 它不该阻断下游，只该在报告里被看见");
}

/* ==================================================================
 * 测试 5：报告生成
 * ================================================================== */
static void test_report(void)
{
    char     buf[2048];
    uint16_t n;

    printf("\n[5] 报告生成\n");

    set_all(GW_SC_PASS);
    g_ret[GW_SC_DWT]  = GW_SC_FAIL;
    g_val[GW_SC_DWT]  = 3400;            /* 期望 1000 */
    g_ret[GW_SC_CAN]  = GW_SC_WARN;
    g_val[GW_SC_CAN]  = 55;
    (void)gw_selfcheck_init(&SC, ITEMS);
    (void)gw_selfcheck_run(&SC);

    n = gw_selfcheck_report(&SC, buf, sizeof(buf));
    if (n > 0u) {
        ok("报告生成成功");
    } else {
        fail("报告生成成功", "");
        return;
    }

    /* 汇总行 */
    if (strstr(buf, "[SELFCHECK]") != 0 && strstr(buf, "通过") != 0) {
        ok("报告有汇总行");
    } else {
        fail("报告有汇总行", "");
    }

    /* 失败项带实测值与期望值 */
    if (strstr(buf, "FAIL") != 0 && strstr(buf, "DWT") != 0
        && strstr(buf, "3400") != 0 && strstr(buf, "1000") != 0) {
        ok("失败项带实测值与期望值（不是只有'失败'两个字）");
    } else {
        fail("失败项带实测值与期望值", "");
    }

    /* ⚠️ 失败项带"先查什么" —— 这是本模块最有价值的部分 */
    if (strstr(buf, "DWT 未使能") != 0) {
        ok("失败项带「先查什么」提示");
    } else {
        fail("失败项带「先查什么」提示", "");
    }

    /* SKIP 项带原因 */
    if (strstr(buf, "SKIP") != 0 && strstr(buf, "DHT22") != 0) {
        ok("被跳过的项也出现在报告里（不能静默省略）");
    } else {
        fail("被跳过的项也出现在报告里", "");
    }

    /* WARN 项出现在报告里 */
    if (strstr(buf, "WARN") != 0 && strstr(buf, "CAN") != 0) {
        ok("WARN 项出现在报告里");
    } else {
        fail("WARN 项出现在报告里", "");
    }

    printf("\n  ---- 报告样例 ----\n");
    printf("%s", buf);
    printf("  -------------------\n");

    /* ---- 截断安全 ---- */
    {
        char small[100];
        uint16_t sn = gw_selfcheck_report(&SC, small, sizeof(small));

        check_int("小缓冲不越界", (int)(sn < sizeof(small) ? 1 : 0), 1);
        check_int("小缓冲仍以 '\\0' 结尾",
                  (int)(small[sn] == '\0' ? 1 : 0), 1);
        ok("截断时不会破坏内存（报告缓冲在栈上）");
    }
    check_int("缓冲过小返回 0",
              (int)gw_selfcheck_report(&SC, buf, 16u), 0);
    check_int("空缓冲返回 0",
              (int)gw_selfcheck_report(&SC, 0, 100u), 0);
    check_int("空上下文返回 0",
              (int)gw_selfcheck_report(0, buf, sizeof(buf)), 0);
}

/* ==================================================================
 * 测试 6：单独重测某一项
 * ================================================================== */
static void test_run_one(void)
{
    printf("\n[6] 单独重测某一项\n");

    /* 先让 DWT 失败 -> DHT22 被跳过 */
    set_all(GW_SC_PASS);
    g_ret[GW_SC_DWT] = GW_SC_FAIL;
    (void)gw_selfcheck_init(&SC, ITEMS);
    (void)gw_selfcheck_run(&SC);

    check_int("（前置）DHT22 被跳过",
              (int)gw_selfcheck_status(&SC, GW_SC_DHT22), (int)GW_SC_SKIP);

    /* 修好 DWT 后单独重测 */
    g_ret[GW_SC_DWT] = GW_SC_PASS;
    check_int("单独重测 DWT 通过",
              (int)gw_selfcheck_run_one(&SC, GW_SC_DWT), (int)GW_SC_PASS);
    check_int("单独重测 DHT22 通过（依赖已满足）",
              (int)gw_selfcheck_run_one(&SC, GW_SC_DHT22), (int)GW_SC_PASS);
    ok("支持单独重测 —— 修一项之后不必重跑全部");

    /* 越界与空指针 */
    check_int("越界项返回 SKIP",
              (int)gw_selfcheck_run_one(&SC, (gw_sc_id_t)99), (int)GW_SC_SKIP);
    check_int("空指针返回 SKIP",
              (int)gw_selfcheck_run_one(0, GW_SC_CLOCK), (int)GW_SC_SKIP);
    check_int("状态查询越界返回 SKIP",
              (int)gw_selfcheck_status(&SC, (gw_sc_id_t)99), (int)GW_SC_SKIP);
    check_int("取实测值越界返回 0",
              (int)gw_selfcheck_value(&SC, (gw_sc_id_t)99), 0);
    check_int("计数对空指针返回 0",
              (int)gw_selfcheck_count(0, GW_SC_PASS), 0);
    check_int("first_failure 对空指针返回 COUNT",
              (int)gw_selfcheck_first_failure(0), (int)GW_SC_COUNT);
    check_int("foundation_ok 对空指针返回 0",
              gw_selfcheck_foundation_ok(0), 0);
    check_int("run 对空指针返回 0", (int)gw_selfcheck_run(0), 0);
    ok("各接口对空指针与越界安全（自检工具自己不能崩）");
}

/* ==================================================================
 * 测试 7：字符串与边界
 * ================================================================== */
static void test_strings(void)
{
    printf("\n[7] 字符串与边界\n");

    check_int("CLOCK 名称正确",
              strcmp(gw_selfcheck_id_str(GW_SC_CLOCK), "CLOCK") == 0 ? 1 : 0, 1);
    check_int("DHT22 名称正确",
              strcmp(gw_selfcheck_id_str(GW_SC_DHT22), "DHT22") == 0 ? 1 : 0, 1);
    check_int("越界 ID 有兜底",
              gw_selfcheck_id_str((gw_sc_id_t)99) != 0 ? 1 : 0, 1);

    {
        gw_sc_status_t st;
        int all_ok = 1;

        for (st = GW_SC_PASS; st <= GW_SC_WARN; st = (gw_sc_status_t)(st + 1)) {
            if (gw_selfcheck_status_str(st) == 0) {
                all_ok = 0;
                break;
            }
        }
        check_int("所有状态都有字符串", all_ok, 1);
    }
    check_int("越界状态有兜底",
              gw_selfcheck_status_str((gw_sc_status_t)99) != 0 ? 1 : 0, 1);

    /* 全失败：报告不该崩 */
    {
        char buf[2048];
        uint32_t nfail;
        uint32_t nskip;

        set_all(GW_SC_FAIL);
        (void)gw_selfcheck_init(&SC, ITEMS);
        (void)gw_selfcheck_run(&SC);
        check_int("全失败时报告生成成功",
                  (int)(gw_selfcheck_report(&SC, buf, sizeof(buf)) > 0u), 1);
        check_int("全失败时 first_failure 是 CLOCK",
                  (int)gw_selfcheck_first_failure(&SC), (int)GW_SC_CLOCK);

        /* ⚠️ 注意这里**不是** 10 个 FAIL。
           即使所有检查函数都返回失败，依赖链也会生效：
             CLOCK 失败          -> GPIO 无依赖，照跑 -> FAIL（共 2 个真失败）
             其余 8 项依赖 CLOCK/GPIO/FLASH -> 全部 SKIP
           这正是框架的价值：**10 个检查函数都坏，报告里只留 2 个真根因**。 */
        nfail = gw_selfcheck_count(&SC, GW_SC_FAIL);
        nskip = gw_selfcheck_count(&SC, GW_SC_SKIP);

        check_int("全失败时只有 2 个真 FAIL（CLOCK 与 GPIO）",
                  (int)nfail, 2);
        check_int("其余 8 项被依赖链阻断为 SKIP", (int)nskip, 8);
        ok("依赖链把'10 个都坏'收敛成'2 个根因' —— 这是框架最重要的作用");

        check_int("全失败时基础项判定为不通过",
                  gw_selfcheck_foundation_ok(&SC), 0);
    }

    /* 全跳过（依赖链第一项失败）也要能出报告 */
    {
        char buf[2048];

        set_all(GW_SC_PASS);
        g_ret[GW_SC_CLOCK] = GW_SC_FAIL;
        g_ret[GW_SC_GPIO]  = GW_SC_FAIL;
        (void)gw_selfcheck_init(&SC, ITEMS);
        (void)gw_selfcheck_run(&SC);
        check_int("大范围跳过时报告生成成功",
                  (int)(gw_selfcheck_report(&SC, buf, sizeof(buf)) > 0u), 1);
    }
}

int main(void)
{
    printf("========== 上板自检框架 单元测试 ==========\n");

    test_init();
    test_all_pass();
    test_dependency_blocking();
    test_warn();
    test_report();
    test_run_one();
    test_strings();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
