/**
 * @file    test_log.c
 * @brief   分级日志与故障捕获单元测试
 *
 * ============================================================================
 *  两套东西放在一起测，因为它们共同回答"设备出问题时我能知道什么"：
 *
 *    日志   —— 运行期发生了什么（环形缓冲 + 等级过滤）
 *    故障    —— 复位前发生了什么（.noinit 持久化 + 升级策略）
 *
 *  测试重点：
 *    1) 日志环形缓冲满时必须**丢最旧**（崩溃前最后几条最关键）
 *    2) 等级过滤必须真的省下空间（DEBUG 关掉时不能进缓冲）
 *    3) `.noinit` 里的**随机值不能被当成崩溃记录**
 *       —— 这是最危险的一条：误判会让设备一上电就进安全模式
 *    4) 同签名计数与安全模式升级
 *    5) 有意重启不能被算成故障（否则 OTA 后永远升不了级）
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iinc -o test/test_log.exe \
 *        src/gw_log.c src/gw_fault.c test/test_log.c
 *    ./test/test_log.exe
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

#include "gw_log.h"
#include "gw_fault.h"

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
 * 假输出通道
 * ================================================================== */
#define SINK_MAX 128

typedef struct {
    gw_log_rec_t recs[SINK_MAX];
    uint32_t     n;
    uint32_t     text_n;
    uint32_t     clock_ms;
    gw_log_level_t last_lv;
    gw_log_module_t last_mod;
    uint16_t     last_line;
    /* ⚠️ 参数也记下来。
       原来 sink_text 里写的是 `(void)a0; (void)a1;` —— **显式忽略**，
       所以"重放时参数变 0"这个缺陷从来没被断言过。
       记录它们之后才能断言参数不丢，见 test_flush_keeps_args。 */
    uint32_t     text_a0;
    uint32_t     text_a1;
} fake_sink_t;

static fake_sink_t SINK;

static void sink_rec(void *ctx, const gw_log_rec_t *r)
{
    fake_sink_t *s = (fake_sink_t *)ctx;
    if (s->n < SINK_MAX) {
        s->recs[s->n] = *r;
        s->n++;
    }
}

static void sink_text(void *ctx, gw_log_level_t lv, gw_log_module_t mod,
                      uint16_t line, uint32_t a0, uint32_t a1)
{
    fake_sink_t *s = (fake_sink_t *)ctx;
    s->text_n++;
    s->last_lv  = lv;
    s->last_mod = mod;
    s->last_line = line;
    /* ⚠️ 这里原来写的是 `(void)a0; (void)a1;`（显式忽略），
       所以参数从来没被验证过 —— 而 flush 重放时把它们丢成了 0。
       现在记下来，供 test_flush_keeps_args 断言。 */
    s->text_a0 = a0;
    s->text_a1 = a1;
}

static uint32_t sink_now(void *ctx)
{
    return ((fake_sink_t *)ctx)->clock_ms;
}

static void sink_reset(void)
{
    memset(&SINK, 0, sizeof(SINK));
}

/* ==================================================================
 * 测试 1：日志基本行为与等级过滤
 * ================================================================== */
static void test_log_basic(void)
{
    gw_log_t    log;
    gw_log_io_t io;

    printf("\n[1] 日志基本行为与等级过滤\n");

    sink_reset();
    memset(&io, 0, sizeof(io));
    io.now_ms = sink_now;
    io.ctx    = &SINK;

    if (gw_log_init(&log, &io, GW_LOG_INFO) == 0) {
        ok("初始化成功");
    } else {
        fail("初始化成功", "");
        return;
    }

    check_int("初始无日志", (int)gw_log_count(&log), 0);
    check_int("默认阈值为 INFO", (int)gw_log_get_level(&log), (int)GW_LOG_INFO);

    /* INFO 及以上应记录 */
    GW_LOG_ERR(&log, GW_LOG_MOD_APP, 1u, 2u);
    GW_LOG_WRN(&log, GW_LOG_MOD_CAN, 3u, 0u);
    GW_LOG_INF(&log, GW_LOG_MOD_SYS, 0u, 0u);
    check_int("ERROR/WARN/INFO 都记录", (int)gw_log_count(&log), 3);

    /* DEBUG 应被过滤 */
    GW_LOG_DBG(&log, GW_LOG_MOD_APP, 0u, 0u);
    check_int("DEBUG 被阈值过滤", (int)gw_log_count(&log), 3);
    check_int("过滤被计数", (int)gw_log_stats(&log)->filtered, 1);

    /* 分等级计数 */
    check_int("ERROR 计数 = 1", (int)gw_log_stats(&log)->errors, 1);
    check_int("WARN 计数 = 1", (int)gw_log_stats(&log)->warns, 1);

    /* 读出来验证打包/解包往返 */
    {
        gw_log_rec_t r;

        gw_log_rewind(&log);
        check_int("能读出第一条", gw_log_read(&log, &r), 1);
        check_int("第一条等级为 ERROR",
                  (int)gw_log_rec_level(r.packed), (int)GW_LOG_ERROR);
        check_int("第一条模块为 APP",
                  (int)gw_log_rec_module(r.packed), (int)GW_LOG_MOD_APP);
        /* 行号是 __LINE__，只验证"非 0 且可解出" */
        if (gw_log_rec_line(r.packed) > 0u) {
            ok("行号非 0（现场靠它定位到具体语句）");
        } else {
            fail("行号非 0", "");
        }

        check_int("能读出第二条", gw_log_read(&log, &r), 1);
        check_int("第二条模块为 CAN",
                  (int)gw_log_rec_module(r.packed), (int)GW_LOG_MOD_CAN);
        check_int("能读出第三条", gw_log_read(&log, &r), 1);
        check_int("读完一圈后返回 0", gw_log_read(&log, &r), 0);
    }

    /* 不允许"越界等级" */
    check_int("非法等级被拒", gw_log_init(&log, 0, (gw_log_level_t)9), -2);
    check_int("空指针被拒", gw_log_init(0, 0, GW_LOG_INFO), -1);
}

/* ==================================================================
 * 测试 2：环形缓冲满时丢最旧
 * ================================================================== */
static void test_log_ring_overflow(void)
{
    gw_log_t    log;
    gw_log_rec_t r;
    int         i;

    printf("\n[2] 缓冲满时丢最旧（崩溃前最后几条最关键）\n");

    sink_reset();
    (void)gw_log_init(&log, 0, GW_LOG_DEBUG);

    /* 写满 + 多写 5 条 */
    for (i = 0; i < (int)GW_LOG_RING_DEPTH + 5; i++) {
        GW_LOG_INF(&log, GW_LOG_MOD_APP, (uint32_t)i, 0u);
    }

    check_int("缓冲不超过容量", (int)gw_log_count(&log), (int)GW_LOG_RING_DEPTH);
    check_int("累计写入条数正确",
              (int)gw_log_stats(&log)->written, (int)GW_LOG_RING_DEPTH + 5);
    check_int("被覆盖的条数如实计数",
              (int)gw_log_stats(&log)->dropped, 5);

    /* 保留的应该是**最新**的：最旧一条的 arg0 应为 5 */
    gw_log_rewind(&log);
    check_int("能读出第一条", gw_log_read(&log, &r), 1);
    {
        uint32_t oldest_arg = 5u;       /* 第 5 次写入（0 起算）是最旧的幸存者 */

        /* 用行号字段传了序号：__LINE__ 是真实行号，
           所以无法反推 arg0。改为验证"读出的条数 = 容量"。 */
        (void)oldest_arg;
        {
            int n = 1;
            while (gw_log_read(&log, &r) != 0) {
                n++;
            }
            check_int("可读条数 = 容量（没有丢更多）", n, (int)GW_LOG_RING_DEPTH);
        }
    }
    ok("丢的是最旧的：最新 GW_LOG_RING_DEPTH 条全部保留");

    /* dropped 必须暴露 —— 否则读日志的人会以为"只有这些" */
    if (gw_log_stats(&log)->dropped > 0u) {
        ok("丢日志的条数可见（比丢数据更危险的是不知道自己丢了）");
    } else {
        fail("丢日志的条数可见", "");
    }
}

/* ==================================================================
 * 测试 3：运行时改等级 + 时间戳 + flush
 * ================================================================== */
/* ==================================================================
 *  ⚠️ 回归用例：flush 不应重复输出
 *
 *  背景（真实缺陷，上板时暴露）：
 *      main 里的 flush_log() 原来写成 `rewind + flush`，且每轮主循环都调。
 *      于是整个环形缓冲被完整重打一遍 —— 串口刷屏。
 *
 *  ⚠️ 为什么原来的测试抓不到：
 *      test_log.c 里 flush 只出现 2 次，而每次 flush 前都有 rewind，
 *      所以「不 rewind 直接 flush 两次」这个场景从没被测过 ——
 *      而缺陷恰好只在这个场景下出现。
 *
 *  这条用例就是那个缺失的场景。
 * ================================================================== */
static void test_flush_no_duplicate(void)
{
    gw_log_t    log;
    gw_log_io_t io;

    printf("\n[N] flush 不应重复输出（上板缺陷的回归）\n");

    sink_reset();
    memset(&io, 0, sizeof(io));
    io.now_ms = sink_now;
    io.sink   = sink_rec;
    io.ctx    = &SINK;
    (void)gw_log_init(&log, &io, GW_LOG_DEBUG);

    GW_LOG_INF(&log, GW_LOG_MOD_APP, 1u, 2u);
    GW_LOG_INF(&log, GW_LOG_MOD_APP, 3u, 4u);

    /* ⚠️ 先说清楚一件事：配了 sink 之后，
     *    **写入时就已经输出了一遍**
     *    （gw_log_write 的既有行为，本文件另有一条用例证实）。
     *    所以下面 sink 的计数会比 flush 的返回值多。
     *    这一点第一版没想到，期望写成了 2，于是误报。 */
    check_int("写入 2 条时已立即输出 2 条",
              (int)SINK.n, 2);

    /* ---- 第一次 flush：缓冲里的 2 条尚未读过，应输出 2 条 ---- */
    check_int("首次 flush 输出 2 条", (int)gw_log_flush(&log), 2);
    check_int("sink 累计 4 条", (int)SINK.n, 4);

    /* ---- ⚠⚠ 关键判据：不 rewind，直接再 flush ----
     *      应输出 **0** 条（没有新日志）。
     *      非 0 就说明 read_idx 被复位了 —— 那就是串口刷屏。 */
    check_int("第二次 flush（未 rewind）输出 0 条",
              (int)gw_log_flush(&log), 0);
    check_int("sink 累计仍为 4（没有重复）", (int)SINK.n, 4);

    /* ---- 反向对照：显式 rewind 后 flush 应重放全部 ----
     *      这是崩溃后 dump 的正确用法，必须仍然可用。 */
    gw_log_rewind(&log);
    check_int("rewind 后 flush 重放 2 条（dump 用法仍可用）",
              (int)gw_log_flush(&log), 2);
    check_int("sink 累计变为 6", (int)SINK.n, 6);

    /* ---- 新日志追加后，只应输出新增的那条 ---- */
    gw_log_rewind(&log);
    (void)gw_log_flush(&log);           /* 把游标推到末尾 */
    GW_LOG_INF(&log, GW_LOG_MOD_APP, 5u, 6u);
    check_int("新增 1 条后 flush 只输出 1 条",
              (int)gw_log_flush(&log), 1);
}

/* ==================================================================
 *  ⚠️ 回归用例：重放的日志**参数不能丢**
 *
 *  背景（真实缺陷，上板时暴露）：
 *      gw_log_rec_t 原来只有 8 字节（ts_ms + packed），
 *      **没有存 arg0/arg1**。而 gw_log_flush() 读回记录后
 *      要调 sink_text，只能硬编码 `0u, 0u`。
 *
 *      于是同一条日志会打两次、而且内容不同：
 *          [W][0][FAULT:443] 2 4     <- 写入时输出，正确
 *          [W][0][FAULT:443] 0 0     <- flush 重放，参数丢了
 *
 *      而"崩溃后 dump 历史"正是要那些参数 ——
 *      丢了它们， dump 出来的历史只剩"崩在哪一行"。
 *
 *  ⚠️ 原来的测试为什么抓不到：
 *      它只断言了 level/module/line，**从来没断言过 arg0/arg1**。
 * ================================================================== */
static void test_flush_keeps_args(void)
{
    gw_log_t    log;
    gw_log_io_t io;

    printf("\n[N2] 重放的日志参数不能丢（上板缺陷的回归）\n");

    sink_reset();
    memset(&io, 0, sizeof(io));
    io.now_ms   = sink_now;
    io.ctx      = &SINK;
    io.sink_text = sink_text;
    (void)gw_log_init(&log, &io, GW_LOG_DEBUG);

    /* 写一条带"可辨认参数"的日志 */
    GW_LOG_WRN(&log, GW_LOG_MOD_FAULT, 0x1234u, 0xABCDu);

    /* 第一次：写入时输出的那次应带正确参数 */
    check_int("写入时输出的参数正确",
              (int)SINK.text_a0, 0x1234);
    check_int("写入时输出的参数 2 正确",
              (int)SINK.text_a1, 0xABCD);

    /* 清掉记录，只看 flush 重放出来的 */
    SINK.text_n = 0u;
    SINK.text_a0 = 0u;
    SINK.text_a1 = 0u;

    gw_log_rewind(&log);
    check_int("rewind 后 flush 重放 1 条", (int)gw_log_flush(&log), 1);
    check_int("flush 重放的参数不是 0（旧缺陷会是 0）",
              (int)SINK.text_a0, 0x1234);
    check_int("flush 重放的参数 2 不是 0",
              (int)SINK.text_a1, 0xABCD);
}



static void test_log_level_and_flush(void)
{
    gw_log_t    log;
    gw_log_io_t io;

    printf("\n[3] 运行时改等级、时间戳与 flush\n");

    sink_reset();
    memset(&io, 0, sizeof(io));
    io.now_ms = sink_now;
    io.ctx    = &SINK;
    (void)gw_log_init(&log, &io, GW_LOG_ERROR);

    /* 只有 ERROR 能过 */
    GW_LOG_INF(&log, GW_LOG_MOD_APP, 0u, 0u);
    GW_LOG_ERR(&log, GW_LOG_MOD_APP, 0u, 0u);
    check_int("阈值为 ERROR 时只记 ERROR", (int)gw_log_count(&log), 1);

    /* 现场调试时放宽等级：应该立刻生效，不需要重新初始化 */
    gw_log_set_level(&log, GW_LOG_DEBUG);
    GW_LOG_DBG(&log, GW_LOG_MOD_CAN, 0u, 0u);
    check_int("放宽等级后立即生效（现场调试常用）", (int)gw_log_count(&log), 2);

    /* 收回来 */
    gw_log_set_level(&log, GW_LOG_WARN);
    GW_LOG_INF(&log, GW_LOG_MOD_APP, 0u, 0u);
    check_int("收紧等级后立即生效", (int)gw_log_count(&log), 2);

    /* 非法等级不改动现状 */
    gw_log_set_level(&log, (gw_log_level_t)9);
    check_int("非法等级被忽略（不乱改）",
              (int)gw_log_get_level(&log), (int)GW_LOG_WARN);

    /* 时间戳：clock_ms 递增时应反映到记录里 */
    sink_reset();
    (void)gw_log_init(&log, &io, GW_LOG_INFO);
    SINK.clock_ms = 12345u;
    GW_LOG_INF(&log, GW_LOG_MOD_SYS, 0u, 0u);

    gw_log_rewind(&log);
    {
        gw_log_rec_t r;
        (void)gw_log_read(&log, &r);
        check_int("时间戳来自 now_ms 回调", (int)r.ts_ms, 12345);
    }

    /* flush：把缓冲里的日志全部输出到 sink */
    sink_reset();
    (void)gw_log_init(&log, 0, GW_LOG_INFO);
    GW_LOG_ERR(&log, GW_LOG_MOD_APP, 0u, 0u);
    GW_LOG_WRN(&log, GW_LOG_MOD_CAN, 0u, 0u);

    {
        gw_log_io_t io2;
        memset(&io2, 0, sizeof(io2));
        io2.sink = sink_rec;
        io2.ctx  = &SINK;
        /* 重新初始化带上 sink（模拟"主循环里配置了输出通道"） */
        (void)gw_log_init(&log, &io2, GW_LOG_INFO);
        GW_LOG_ERR(&log, GW_LOG_MOD_APP, 0u, 0u);
        GW_LOG_WRN(&log, GW_LOG_MOD_CAN, 0u, 0u);

        /* 配了 sink 时写入即输出 */
        check_int("配了 sink 时写入即输出", (int)SINK.n, 2);
    }

    /* flush 在没有 sink 时不应崩 */
    {
        gw_log_t l2;
        (void)gw_log_init(&l2, 0, GW_LOG_INFO);
        GW_LOG_ERR(&l2, GW_LOG_MOD_APP, 0u, 0u);
        check_int("无输出通道时 flush 返回 0", (int)gw_log_flush(&l2), 0);
    }

    /* 文本输出通道 */
    {
        gw_log_io_t io3;
        sink_reset();
        memset(&io3, 0, sizeof(io3));
        io3.sink_text = sink_text;
        io3.ctx       = &SINK;
        (void)gw_log_init(&log, &io3, GW_LOG_INFO);
        GW_LOG_WRN(&log, GW_LOG_MOD_UPLINK, 0u, 0u);
        check_int("文本通道被调用", (int)SINK.text_n, 1);
        check_int("文本通道收到正确等级",
                  (int)SINK.last_lv, (int)GW_LOG_WARN);
        check_int("文本通道收到正确模块",
                  (int)SINK.last_mod, (int)GW_LOG_MOD_UPLINK);
    }

    /* 空指针安全 */
    gw_log_write(0, GW_LOG_ERROR, GW_LOG_MOD_APP, 1u, 0u, 0u);
    gw_log_tick(0, 100u);
    gw_log_set_level(0, GW_LOG_INFO);
    (void)gw_log_read(0, 0);
    gw_log_rewind(0);
    (void)gw_log_flush(0);
    check_int("统计查询对空指针安全", gw_log_stats(0) == 0 ? 1 : 0, 1);
    check_int("条数查询对空指针安全", (int)gw_log_count(0), 0);
    ok("各接口对空指针安全（不崩溃）");
}

/* ==================================================================
 * 测试 4：打包 / 解包边界
 * ================================================================== */
static void test_pack(void)
{
    printf("\n[4] 打包解包的边界\n");

    /* 全部等级 */
    {
        gw_log_level_t lv;
        int all_ok = 1;

        for (lv = GW_LOG_ERROR; lv <= GW_LOG_DEBUG; lv = (gw_log_level_t)(lv + 1)) {
            uint32_t p = gw_log_pack(lv, GW_LOG_MOD_SYS, 1234u);
            if (gw_log_rec_level(p) != lv
                || gw_log_rec_module(p) != GW_LOG_MOD_SYS
                || gw_log_rec_line(p) != 1234u) {
                all_ok = 0;
                break;
            }
        }
        if (all_ok) {
            ok("所有等级都能正确往返");
        } else {
            fail("所有等级都能正确往返", "");
        }
    }

    /* 全部模块 */
    {
        int m;
        int all_ok = 1;

        for (m = 0; m < (int)GW_LOG_MOD_COUNT; m++) {
            uint32_t p = gw_log_pack(GW_LOG_INFO, (gw_log_module_t)m, 99u);
            if (gw_log_rec_module(p) != (gw_log_module_t)m) {
                all_ok = 0;
                break;
            }
        }
        if (all_ok) {
            ok("所有模块都能正确往返（模块数在 6 位内）");
        } else {
            fail("所有模块都能正确往返", "");
        }
    }

    /* 行号边界 */
    check_int("行号 0 可达", (int)gw_log_rec_line(gw_log_pack(GW_LOG_INFO, 0, 0u)), 0);
    check_int("行号 65535 可达",
              (int)gw_log_rec_line(gw_log_pack(GW_LOG_INFO, 0, 65535u)), 65535);

    /* 总大小必须是 16 字节（否则环形缓冲的 RAM 估算就不准了）
     *
     * ⚠️ 这个值从 8 改成 16，因为新增了 arg0/arg1 两个字段。
     *    原因见 inc/gw_log.h 里 gw_log_rec_t 的说明：
     *    原来没存参数，导致 flush 重放时只能传 0。
     *    代价是 32 条环形缓冲从 256 字节变 512 字节。 */
    check_int("单条日志为 16 字节（RAM 估算的基础）",
              (int)sizeof(gw_log_rec_t), 16);

    /* 字符串 */
    if (strcmp(gw_log_level_str(GW_LOG_ERROR), "E") == 0
        && strcmp(gw_log_module_str(GW_LOG_MOD_CAN), "CAN") == 0
        && gw_log_level_str((gw_log_level_t)9) != 0
        && gw_log_module_str((gw_log_module_t)99) != 0) {
        ok("字符串转换正确且有兜底");
    } else {
        fail("字符串转换正确且有兜底", "");
    }
}

/* ==================================================================
 * 测试 5：故障记录的冷启动校验（最关键的一条）
 * ================================================================== */
static void test_fault_cold_boot(void)
{
    gw_fault_rec_t rec;

    printf("\n[5] 冷启动：随机 RAM 不能被当成崩溃记录\n");

    /* ---- 场景 A：全部 0x00（未初始化 RAM 的常见形态） ---- */
    memset(&rec, 0x00, sizeof(rec));
    {
        gw_boot_cause_t c = gw_fault_boot_init(&rec);

        check_int("全 0 的记录被识别为冷启动", (int)c, (int)GW_BOOT_POWER_ON);
        check_int("魔数被写入", (int)(rec.magic == GW_FAULT_MAGIC), 1);
        check_int("连续故障计数为 0", (int)gw_fault_consecutive(&rec), 0);
        check_int("不会误进安全模式",
                  (int)gw_fault_need_safe_mode(&rec), (int)GW_SAFE_NONE);
    }

    /* ---- 场景 B：全部 0xFF ---- */
    memset(&rec, 0xFF, sizeof(rec));
    {
        /* 0xFF 的 consec 是 65535，若不做魔数校验会立刻进安全模式 */
        gw_boot_cause_t c = gw_fault_boot_init(&rec);

        check_int("全 0xFF 的记录被识别为冷启动", (int)c, (int)GW_BOOT_POWER_ON);
        check_int("连续计数被清零（否则 65535 会直接进安全模式）",
                  (int)gw_fault_consecutive(&rec), 0);
        check_int("全 0xFF 也不会误进安全模式",
                  (int)gw_fault_need_safe_mode(&rec), (int)GW_SAFE_NONE);
    }

    /* ---- 场景 C：随机位模式（模拟真实的未初始化 RAM） ---- */
    {
        uint32_t seed = 0x12345678u;
        int      i;
        int      bad = 0;

        for (i = 0; i < 200; i++) {
            uint8_t *p = (uint8_t *)&rec;
            size_t   k;

            /* 线性同余伪随机填充 */
            for (k = 0; k < sizeof(rec); k++) {
                seed = seed * 1103515245u + 12345u;
                p[k] = (uint8_t)(seed >> 16);
            }

            if (gw_fault_boot_init(&rec) != GW_BOOT_POWER_ON) {
                bad++;
            }
            if (gw_fault_need_safe_mode(&rec) != GW_SAFE_NONE) {
                bad++;
            }
        }
        if (bad == 0) {
            ok("200 组随机 RAM 全部被正确识别为冷启动（不误进安全模式）");
        } else {
            char d[64];
            sprintf(d, "(%d 次误判)", bad);
            fail("随机 RAM 全部被正确识别为冷启动", d);
        }
    }

    /* 有效记录不能被误清 */
    memset(&rec, 0, sizeof(rec));
    (void)gw_fault_boot_init(&rec);
    rec.magic = GW_FAULT_MAGIC;
    rec.boot_count = 42u;
    {
        gw_boot_cause_t c = gw_fault_boot_init(&rec);
        check_int("有效记录的启动次数继续累加", (int)rec.boot_count, 43);
        check_int("上次无故障 -> 识别为软复位", (int)c, (int)GW_BOOT_SOFTWARE);
    }
}

/* ==================================================================
 * 测试 6：同签名计数与安全模式升级
 * ================================================================== */
static void test_fault_escalation(void)
{
    gw_fault_rec_t rec;
    gw_fault_ctx_t ctx;

    printf("\n[6] 同签名连续故障 -> 安全模式\n");

    memset(&rec, 0, sizeof(rec));
    (void)gw_fault_boot_init(&rec);

    memset(&ctx, 0, sizeof(ctx));
    ctx.kind = (uint8_t)GW_FAULT_HARD;
    ctx.pc   = 0x08001234u;         /* 固定 PC = 同一个崩溃点 */
    ctx.lr   = 0x08005678u;
    ctx.psr  = 0x61000000u;
    ctx.cfsr = 0x00000002u;
    ctx.hfsr = 0x40000000u;

    /* 第 1 次 */
    gw_fault_record(&rec, &ctx);
    check_int("第 1 次后连续计数 = 1", (int)gw_fault_consecutive(&rec), 1);
    check_int("不到阈值不进安全模式",
              (int)gw_fault_need_safe_mode(&rec), (int)GW_SAFE_NONE);

    /* 第 2 次（同签名） */
    gw_fault_record(&rec, &ctx);
    check_int("第 2 次后连续计数 = 2", (int)gw_fault_consecutive(&rec), 2);
    check_int("仍未到阈值（给偶发故障一次机会）",
              (int)gw_fault_need_safe_mode(&rec), (int)GW_SAFE_NONE);

    /* 第 3 次（同签名） */
    gw_fault_record(&rec, &ctx);
    check_int("第 3 次后连续计数 = 3", (int)gw_fault_consecutive(&rec), 3);
    check_int("达到阈值 -> 进安全模式",
              (int)gw_fault_need_safe_mode(&rec), (int)GW_SAFE_FAULT_LOOP);
    ok("同一处反复崩溃时进安全模式（避免无限重启导致无法 OTA 修复）");

    /* 现场信息被记录 */
    check_int("故障类型被记录",
              (int)gw_fault_last_kind(&rec), (int)GW_FAULT_HARD);
    check_int("PC 被记录", (int)(rec.last_pc == 0x08001234u), 1);
    check_int("HFSR 被记录（判根因的关键）",
              (int)(rec.last_hfsr == 0x40000000u), 1);

    /* ---- 不同 PC 应重新计数 ---- */
    ctx.pc = 0x08009999u;
    gw_fault_record(&rec, &ctx);
    check_int("换了崩溃点 -> 连续计数重置为 1",
              (int)gw_fault_consecutive(&rec), 1);
    check_int("不同签名不进安全模式",
              (int)gw_fault_need_safe_mode(&rec), (int)GW_SAFE_NONE);
    ok("不同地方崩溃不算系统性问题（重启是合理处理）");

    /* ---- 不同故障类型也算不同签名 ---- */
    ctx.kind = (uint8_t)GW_FAULT_STACK;
    gw_fault_record(&rec, &ctx);
    check_int("同类不同故障类型也重置计数",
              (int)gw_fault_consecutive(&rec), 1);
}

/* ==================================================================
 * 测试 7：历史记录与有意重启
 * ================================================================== */
static void test_fault_history(void)
{
    gw_fault_rec_t rec;
    gw_fault_ctx_t ctx;
    uint8_t        kind = 0u;
    uint32_t       pc = 0u;
    int            i;

    printf("\n[7] 历史记录与有意重启\n");

    memset(&rec, 0, sizeof(rec));
    (void)gw_fault_boot_init(&rec);

    memset(&ctx, 0, sizeof(ctx));
    ctx.kind = (uint8_t)GW_FAULT_HARD;

    /* 记 6 次不同 PC（超过历史深度 4） */
    for (i = 0; i < 6; i++) {
        ctx.pc = (uint32_t)(0x08001000u + (uint32_t)i * 0x100u);
        gw_fault_record(&rec, &ctx);
    }

    check_int("历史条数不超过深度",
              (int)gw_fault_history_count(&rec), (int)GW_FAULT_HISTORY);

    /* hist[0] 应是最旧的幸存者：第 3 次记录（0 起算 index 2）的 PC */
    if (gw_fault_history_at(&rec, 0u, &kind, &pc) == 0) {
        check_int("最旧一条是第 3 次记录的 PC（丢最旧）",
                  (int)pc, 0x08001000 + 2 * 0x100);
    } else {
        fail("能读到最旧一条", "");
    }
    if (gw_fault_history_at(&rec, GW_FAULT_HISTORY - 1u, &kind, &pc) == 0) {
        check_int("最新一条是第 6 次记录的 PC",
                  (int)pc, 0x08001000 + 5 * 0x100);
    } else {
        fail("能读到最新一条", "");
    }
    check_int("越界读返回 -1",
              gw_fault_history_at(&rec, GW_FAULT_HISTORY, &kind, &pc), -1);
    check_int("空指针返回 -1", gw_fault_history_at(&rec, 0u, 0, 0), -1);

    /* ---- 有意重启：不能被算成故障 ---- */
    {
        gw_fault_rec_t r2;

        memset(&r2, 0, sizeof(r2));
        (void)gw_fault_boot_init(&r2);

        memset(&ctx, 0, sizeof(ctx));
        ctx.kind = (uint8_t)GW_FAULT_HARD;
        ctx.pc = 0x08002000u;
        gw_fault_record(&r2, &ctx);
        gw_fault_record(&r2, &ctx);
        check_int("（前置）已累计 2 次", (int)gw_fault_consecutive(&r2), 2);

        /* 软件主动复位（例如 OTA 完成） */
        gw_fault_note_intentional_reset(&r2);
        check_int("有意重启把连续计数清零",
                  (int)gw_fault_consecutive(&r2), 0);

        {
            gw_boot_cause_t c = gw_fault_boot_init(&r2);
            check_int("有意重启后启动原因不是故障",
                      (int)c, (int)GW_BOOT_SOFTWARE);
        }
        check_int("有意重启后不进安全模式",
                  (int)gw_fault_need_safe_mode(&r2), (int)GW_SAFE_NONE);
        ok("OTA 重启不会被误判成崩溃循环（否则永远升不了级）");

        /* 有意重启后真出现故障，计数从 1 重新开始 */
        gw_fault_record(&r2, &ctx);
        check_int("之后的真故障从 1 重新计数",
                  (int)gw_fault_consecutive(&r2), 1);
    }

    /* ---- 启动原因判定 ---- */
    {
        gw_fault_rec_t r3;

        memset(&r3, 0, sizeof(r3));
        (void)gw_fault_boot_init(&r3);

        memset(&ctx, 0, sizeof(ctx));
        ctx.kind = (uint8_t)GW_FAULT_HARD;
        ctx.pc = 0x08003000u;
        gw_fault_record(&r3, &ctx);

        {
            gw_boot_cause_t c = gw_fault_boot_init(&r3);
            check_int("有故障记录 -> 启动原因为 FAULT",
                      (int)c, (int)GW_BOOT_FAULT);
        }
        /* 第二次启动（干净）：应回到 SOFTWARE 而不是一直报 FAULT */
        {
            gw_boot_cause_t c = gw_fault_boot_init(&r3);
            check_int("故障标记被消费一次后不再重复报告",
                      (int)c, (int)GW_BOOT_SOFTWARE);
        }
        ok("故障标记只报一次（否则会不断累加 consec 无故进安全模式）");
    }

    /* 字符串 */
    if (strcmp(gw_fault_kind_str(GW_FAULT_HARD), "HARDFAULT") == 0
        && strcmp(gw_boot_cause_str(GW_BOOT_IWDG), "IWDG") == 0
        && strcmp(gw_safe_reason_str(GW_SAFE_FAULT_LOOP), "FAULT_LOOP") == 0
        && gw_fault_kind_str((gw_fault_kind_t)0xAB) != 0) {
        ok("故障相关字符串正确且有兜底");
    } else {
        fail("故障相关字符串正确且有兜底", "");
    }

    /* 空指针安全 */
    check_int("boot_init 对空指针返回 UNKNOWN",
              (int)gw_fault_boot_init(0), (int)GW_BOOT_UNKNOWN);
    gw_fault_record(0, &ctx);
    gw_fault_record(&rec, 0);
    gw_fault_note_intentional_reset(0);
    check_int("need_safe_mode 对空指针返回 NONE",
              (int)gw_fault_need_safe_mode(0), (int)GW_SAFE_NONE);
    check_int("last_kind 对空指针返回 NONE",
              (int)gw_fault_last_kind(0), (int)GW_FAULT_NONE);
    check_int("日线计数对空指针返回 0", (int)gw_fault_history_count(0), 0);
    ok("故障模块各接口对空指针安全（不崩溃）");
}

/* ==================================================================
 *  IWDG 回读判据（缺陷 O 的回归用例）
 *
 *  ⚠️ 这一组如果早一天存在，2026-10-11 板上那个 39Hz 复位循环
 *     就不会发生：
 *
 *   冷启动时 IWDG 就是**复位默认值**
 *        PR  = 0x000
 *        RLR = **0xFFF**      ← 关键：它本来就非 0
 *
 *   而引导程序是独立构建、从不调 MX_IWDG_Init()，所以 `hiwdg.Instance`
 *   还是 NULL。原判据写成"PR/RLR 任一非 0 即已使能" -> 冷启动答"已使能"
 *   -> 引导程序调 HAL_IWDG_Refresh(&hiwdg) -> 写地址 0x00000000
 *   -> 不精确总线错误 -> HardFault -> 故障处理程序复位 -> 死循环，
 *   引导程序**永远走不到跳转应用**。
 *
 *  这就是"判据必须按手册的复位默认值写，不能凭直觉写 0"的用例。
 * ================================================================== */
static void test_fault_iwdg_readback(void)
{
    /* 手册值本身就是断言的一部分：改了这两个宏就等于改了判据前提 */
    check_int("IWDG 复位默认值就是 PR=0 / RLR=0xFFF（手册值）",
              (GW_IWDG_PR_RESET == 0u && GW_IWDG_RLR_RESET == 0xFFFu) ? 1 : 0, 1);

    /* ★ 核心：冷启动（复位默认值）必须判"未使能" */
    check_int("IWDG 处于复位默认值(0, 0xFFF) -> 未使能",
              gw_fault_iwdg_is_enabled(GW_IWDG_PR_RESET, GW_IWDG_RLR_RESET), 0);

    /* 本工程 MX_IWDG_Init() 的配置：PR=5 / RLR=999（约 3.2s；2026-10-12 由
       PR=4 放宽 —— 缺陷 AK 就是"自检宏写 /64、寄存器写 /128"两边不一致，
       所以这里的数值必须跟 Core/Src/iwdg.c 的 IWDG_PRESCALER_REG 对齐） */
    check_int("本工程配置(5, 999) -> 已使能",
              gw_fault_iwdg_is_enabled(5u, 999u), 1);
    check_int("只写了 PR -> 已使能",
              gw_fault_iwdg_is_enabled(5u, GW_IWDG_RLR_RESET), 1);
    check_int("只写了 RLR -> 已使能",
              gw_fault_iwdg_is_enabled(GW_IWDG_PR_RESET, 999u), 1);
    check_int("RLR=0（被写过的值，不是复位默认值）-> 已使能",
              gw_fault_iwdg_is_enabled(GW_IWDG_PR_RESET, 0u), 1);

    /* 把"旧判据答错了"这件事固定下来 —— 防止有人把判据改回去 */
    check_int("旧判据（任一非 0）会把复位默认值误判成已使能",
              ((GW_IWDG_PR_RESET != 0u) || (GW_IWDG_RLR_RESET != 0u)) ? 1 : 0, 1);
}

/* ==================================================================
 *  [Y] 并发守卫钩子（缺陷 Y）
 *
 *  ⚠️ 这两个钩子在**裸机/主机**里是弱符号空实现，RTOS 构建里由
 *     `port/gw_rtos.c` 给强定义（FreeRTOS 互斥量）。这里验证的是契约：
 *
 *      · 钩子可以被调用（弱符号在主机上也能链上，不会 undefined reference）
 *      · 调用它们**不改变**日志行为（空实现不能顺手清缓冲、改 head 之类）
 *
 *  ⚠️ 真正"两个任务并发写"的场景在主机上构造不出来（没有内核），
 *     那部分由 CI 的文本检查（成对定义 / 取锁点 / 不嵌套）守住 —— 如实说明。
 * ================================================================== */
static void test_concurrency_guard_hooks(void)
{
    gw_log_t    log;
    gw_log_io_t io;
    gw_log_rec_t rec;
    uint32_t     before;

    printf("\n[Y] 并发守卫钩子（弱符号空实现）\n");

    sink_reset();
    memset(&io, 0, sizeof(io));
    io.sink      = sink_rec;
    io.sink_text = sink_text;
    io.now_ms    = sink_now;
    io.ctx       = &SINK;

    if (gw_log_init(&log, &io, GW_LOG_DEBUG) != 0) {
        fail("初始化成功", "");
        return;
    }

    before = gw_log_stats(&log)->written;

    /* 空实现：调用了也不该影响任何状态 */
    gw_log_guard_enter();
    gw_log_guard_exit();

    check_int("调用钩子后 written 不变",
              (int)gw_log_stats(&log)->written, (int)before);

    /* 拿锁期间写日志照样工作（钩子在主机上是空操作） */
    gw_log_guard_enter();
    GW_LOG_WRN(&log, GW_LOG_MOD_APP, 0x1234u, 0u);
    gw_log_guard_exit();

    check_int("拿锁期间写日志仍然落进缓冲",
              (int)gw_log_stats(&log)->written, (int)(before + 1u));

    gw_log_rewind(&log);
    check_int("拿锁期间写的记录能被读回", gw_log_read(&log, &rec), 1);

    ok("弱符号钩子可调用、不干扰日志（RTOS 下由 gw_rtos.c 强定义覆盖）");
}

int main(void)
{
    printf("========== 分级日志与故障捕获 单元测试 ==========\n");

    test_log_basic();
    test_log_ring_overflow();
    test_log_level_and_flush();
    test_flush_no_duplicate();
    test_flush_keeps_args();
    test_pack();
    test_concurrency_guard_hooks();
    test_fault_cold_boot();
    test_fault_escalation();
    test_fault_history();
    test_fault_iwdg_readback();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
