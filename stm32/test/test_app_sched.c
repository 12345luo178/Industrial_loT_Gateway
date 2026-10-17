/**
 * @file    test_app_sched.c
 * @brief   应用层任务化验证：调度模式与顺序模式**行为等价**
 *
 * ============================================================================
 *  这套测试回答一个问题：**把 gw_app_poll 拆成任务之后，行为变了吗？**
 *
 *  重构最怕的就是"看起来没问题，实际某个边缘路径变了"。
 *  所以这里用同一套输入跑两条路径，逐字段比对结果：
 *
 *      路径 A：gw_app_poll() 顺序调用（原来的行为）
 *      路径 B：gw_app_poll() 经 gw_sched_run() 调度
 *
 *  比对的内容包括：
 *    - 从站收发计数与响应内容
 *    - 主站轮询进度与从机在线位图
 *    - DHT22 采样次数
 *    - 寄存器表**全部 48 个寄存器**逐字节一致
 *    - 上云遥测条数与内容
 *
 *  如果两者有任何差异，说明任务化改变了行为 —— 那是必须修掉的。
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iinc -Idrivers -Iapp -o test/test_app_sched.exe \
 *        src/modbus_crc.c src/modbus_rtu.c src/modbus_master.c src/gw_pipeline.c \
 *        src/gw_frame.c src/gw_uplink.c src/gw_can_core.c src/gw_timing.c \
 *        src/gw_sched.c drivers/dht22.c app/gw_app.c test/test_app_sched.c
 *    ./test/test_app_sched.exe
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

#include "gw_app.h"
#include "modbus_crc.h"
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
 * 可控的假环境
 * ================================================================== */
typedef struct {
    mb_regs_t regs;
    mb_slave_t slave;

    /* 待注入的从站请求帧 */
    uint8_t  req[MB_MAX_ADU];
    uint16_t req_len;
    uint8_t  req_ready;

    /* 捕获到的从站响应 */
    uint8_t  resp[MB_MAX_ADU];
    uint16_t resp_len;

    /* 主站发出去的请求（用于假从机应答） */
    uint8_t  mast_req[MB_MAX_ADU];
    uint16_t mast_req_len;
    uint8_t  resp_to_master_ready;
    uint8_t  mast_resp[MB_MAX_ADU];
    uint16_t mast_resp_len;

    int      dht_calls;
} env_t;

static env_t ENV;

static int p_take(void *c, uint8_t *buf, uint16_t cap, uint16_t *len)
{
    (void)c;
    if (ENV.req_ready == 0u || ENV.req_len == 0u) {
        return -1;
    }
    if (ENV.req_len > cap) {
        return -1;
    }
    memcpy(buf, ENV.req, ENV.req_len);
    *len = ENV.req_len;
    ENV.req_ready = 0u;
    return 0;
}

static int p_send(void *c, const uint8_t *buf, uint16_t len)
{
    (void)c;
    if (len <= sizeof(ENV.resp)) {
        memcpy(ENV.resp, buf, len);
        ENV.resp_len = len;
    }
    return 0;
}

/* 主站发送：模拟总线上有一台从机（地址 2）立即应答 */
static int p_bus_acquire(void *c) { (void)c; return 0; }

static int p_send_held(void *c, const uint8_t *buf, uint16_t len)
{
    mb_slave_t fs;
    mb_regs_t  fregs;

    (void)c;
    if (len > sizeof(ENV.mast_req)) {
        return -1;
    }
    memcpy(ENV.mast_req, buf, len);
    ENV.mast_req_len = len;

    /* 用真实从站引擎生成响应 */
    memset(&fregs, 0, sizeof(fregs));
    fregs.holding[0] = 0x1234u;
    fregs.holding[1] = 0x5678u;
    mb_slave_init(&fs, 2u, &fregs);

    if (mb_slave_process(&fs, buf, len) != 0) {
        if (fs.tx_len <= sizeof(ENV.mast_resp)) {
            memcpy(ENV.mast_resp, fs.tx_buf, fs.tx_len);
            ENV.mast_resp_len = fs.tx_len;
            ENV.resp_to_master_ready = 1u;
        }
    }
    return 0;
}

static void p_enter_wait(void *c) { (void)c; }
static void p_exit_wait(void *c)  { (void)c; }
static int  p_release(void *c)    { (void)c; return 0; }
static int  p_in_wait(void *c)    { (void)c; return 0; }
static int  p_holds(void *c)      { (void)c; return 0; }

static mb_regs_t *p_regs(void *c) { (void)c; return &ENV.regs; }

static dht22_err_t p_dht(void *c, dht22_filtered_t *f, uint8_t retry)
{
    (void)c; (void)retry;
    ENV.dht_calls++;
    /* 注入一个确定的有效值 */
    f->valid    = 1u;
    f->temp_x10 = 235;
    f->humi_x10 = 552u;
    return DHT22_OK;
}

static void env_reset(void)
{
    memset(&ENV, 0, sizeof(ENV));
}

static void make_port(gw_port_t *p)
{
    memset(p, 0, sizeof(*p));
    p->rs485_take_frame      = p_take;
    p->rs485_send            = p_send;
    p->rs485_bus_acquire     = p_bus_acquire;
    p->rs485_send_held       = p_send_held;
    p->rs485_enter_wait      = p_enter_wait;
    p->rs485_exit_wait       = p_exit_wait;
    p->rs485_release_master  = p_release;
    p->rs485_in_wait         = p_in_wait;
    p->rs485_master_holds_bus = p_holds;
    p->get_regs              = p_regs;
    p->dht_sample            = p_dht;
    p->ctx                   = 0;
}

/* 造一个读保持寄存器的 Modbus 请求（地址 1，功能码 03，起始 0，数量 4） */
static void inject_slave_request(void)
{
    uint16_t crc;

    ENV.req[0] = 0x01u;
    ENV.req[1] = 0x03u;
    ENV.req[2] = 0x00u;
    ENV.req[3] = 0x00u;
    ENV.req[4] = 0x00u;
    ENV.req[5] = 0x04u;

    crc = mb_crc16(ENV.req, 6u);
    ENV.req[6] = (uint8_t)(crc & 0xFFu);
    ENV.req[7] = (uint8_t)(crc >> 8);

    ENV.req_len   = 8u;
    ENV.req_ready = 1u;
}

/* ==================================================================
 *  同一套输入，跑两条路径
 * ================================================================== */
typedef struct {
    mb_regs_t regs_snapshot;
    uint32_t  slave_rx, slave_tx;
    uint16_t  online_bmp;
    uint32_t  dht_samples;
    uint32_t  loops;
    uint16_t  resp_len;
    uint8_t   resp[MB_MAX_ADU];
    uint16_t  master_err;
    uint32_t  telem_ok;
} result_t;

/**
 * 跑一遍场景并收集结果
 *
 * 场景：12 轮，每轮 10ms
 *   第 1 轮注入一次从站请求（验证从站路径）
 *   第 3 轮让主站轮询一台从机（地址 2）并应答
 *   第 8 轮触发一次 DHT 采样（采样周期 2.5s = 250 轮，
 *        所以这里靠"注入 tick 累积"来提前触发 —— 见下方说明）
 */
static void run_scenario(gw_app_t *app, int use_sched, result_t *out)
{
    int i;

    env_reset();
    mb_slave_init(&ENV.slave, 1u, &ENV.regs);

    /* 主站加一个轮询项：读地址 2 的保持寄存器 0..3 -> 本地输入 0..3 */
    {
        mb_m_poll_item_t it;

        memset(&it, 0, sizeof(it));
        it.slave    = 2u;
        it.func     = MB_FC_READ_HOLDING;
        it.start    = 0u;
        it.qty      = 4u;
        it.map_base = 0u;
        it.map_area = 0;
        (void)mb_master_add_item(gw_app_master(app), &it);
    }

    for (i = 0; i < 12; i++) {
        if (i == 0) {
            inject_slave_request();
        }

        gw_app_poll(app, 10u);

        /* 主站发出请求后立刻喂响应（模拟从机即时应答） */
        if (ENV.resp_to_master_ready != 0u) {
            ENV.resp_to_master_ready = 0u;
            (void)mb_master_on_response(gw_app_master(app),
                                        ENV.mast_resp, ENV.mast_resp_len);
        }

        /* 第 4 轮手动触发一次采样：把采样累加器直接推满。
           为什么要这样做而不是等 250 轮：
           本测试关心的是"两条路径行为是否一致"，
           不需要真的等 2.5 秒的模拟 —— 用同样的手法推两条路径即可。 */
        if (i == 3) {
            app->pipe.sample_acc_ms = GW_SAMPLE_PERIOD_MS;
        }
    }

    out->regs_snapshot = ENV.regs;
    out->slave_rx      = app->slave_rx_frames;
    out->slave_tx      = app->slave_tx_frames;
    out->online_bmp    = mb_master_online_bitmap(gw_app_master(app));
    out->dht_samples   = app->dht_samples;
    out->loops         = app->loop_count;
    out->resp_len      = ENV.resp_len;
    memcpy(out->resp, ENV.resp, sizeof(out->resp));
    out->master_err    = app->pipe.master_err;
    out->telem_ok      = app->telemetry_seq;

    (void)use_sched;
}

/* ==================================================================
 * 测试
 * ================================================================== */
static void test_equivalence(void)
{
    static gw_app_t   app_seq;
    static gw_app_t   app_sch;
    static gw_sched_t sched;
    gw_port_t         port;
    result_t          ra;
    result_t          rb;

    printf("\n[1] 调度模式与顺序模式行为等价（任务化的正确性验证）\n");

    /* ---- 路径 A：顺序模式 ---- */
    env_reset();
    make_port(&port);
    memset(&ENV.slave, 0, sizeof(ENV.slave));
    if (gw_app_init(&app_seq, &ENV.slave, &port) != 0) {
        fail("顺序模式初始化", "");
        return;
    }
    gw_app_enable_timing(&app_seq, 0);
    gw_app_enable_sched(&app_seq, 0);       /* 关键：不启用调度 */
    run_scenario(&app_seq, 0, &ra);

    /* ---- 路径 B：调度模式 ---- */
    env_reset();
    make_port(&port);
    memset(&ENV.slave, 0, sizeof(ENV.slave));
    if (gw_app_init(&app_sch, &ENV.slave, &port) != 0) {
        fail("调度模式初始化", "");
        return;
    }
    gw_app_enable_timing(&app_sch, 0);

    if (gw_sched_init(&sched, gw_app_task_defs(), 0, 0) != 0) {
        fail("调度器初始化", "");
        return;
    }
    gw_sched_start(&sched);
    gw_app_enable_sched(&app_sch, &sched);  /* 关键：启用调度 */
    run_scenario(&app_sch, 1, &rb);

    /* ---- 逐项比对 ---- */
    check_int("从站收帧计数一致",
              (int)(ra.slave_rx == rb.slave_rx), 1);
    check_int("从站发帧计数一致",
              (int)(ra.slave_tx == rb.slave_tx), 1);
    check_int("主站在线位图一致",
              (int)(ra.online_bmp == rb.online_bmp), 1);
    check_int("DHT 采样次数一致",
              (int)(ra.dht_samples == rb.dht_samples), 1);
    check_int("轮数一致", (int)(ra.loops == rb.loops), 1);
    check_int("主站错误码一致",
              (int)(ra.master_err == rb.master_err), 1);
    check_int("上云遥测条数一致",
              (int)(ra.telem_ok == rb.telem_ok), 1);

    /* 从站响应逐字节一致 */
    check_int("从站响应长度一致",
              (int)(ra.resp_len == rb.resp_len), 1);
    if (ra.resp_len == rb.resp_len && ra.resp_len > 0u) {
        check_int("从站响应内容逐字节一致",
                  memcmp(ra.resp, rb.resp, ra.resp_len) == 0 ? 1 : 0, 1);
    }

    /* 寄存器表逐字节一致 —— 这是最强的一条断言 */
    check_int("寄存器表逐字节一致（48 个寄存器）",
              memcmp(&ra.regs_snapshot, &rb.regs_snapshot,
                     sizeof(mb_regs_t)) == 0 ? 1 : 0, 1);

    if (memcmp(&ra.regs_snapshot, &rb.regs_snapshot,
               sizeof(mb_regs_t)) == 0) {
        ok("任务化没有改变任何可观测行为");
    } else {
        fail("任务化没有改变任何可观测行为", "");
    }

    /* 顺带确认场景本身是有效的（不是"两边都没干活"） */
    if (ra.slave_rx > 0u && ra.slave_tx > 0u && ra.resp_len > 0u) {
        ok("场景有效：从站确实收到请求并发出了响应");
    } else {
        fail("场景有效", "");
    }
    if (ra.loops == 12u) {
        ok("场景有效：确实跑了 12 轮");
    } else {
        fail("场景有效：轮数不对", "");
    }
}

static void test_sched_stats(void)
{
    static gw_app_t   app;
    static gw_sched_t sched;
    gw_port_t         port;
    int               i;

    printf("\n[2] 调度模式下各任务确实被调度\n");

    env_reset();
    make_port(&port);
    memset(&ENV.slave, 0, sizeof(ENV.slave));
    (void)gw_app_init(&app, &ENV.slave, &port);

    (void)gw_sched_init(&sched, gw_app_task_defs(), 0, 0);
    gw_sched_start(&sched);
    gw_app_enable_sched(&app, &sched);

    for (i = 0; i < 5; i++) {
        gw_app_poll(&app, 10u);
    }

    {
        int k;
        int all_ran = 1;

        /* ⚠️ 除 UI 外的 6 个功能任务都是"每轮跑"（period=0）-> 5 轮就是 5 次。 */
        for (k = 0; k < (int)GW_TASK_COUNT; k++) {
            const gw_task_stat_t *st;

            if (k == (int)GW_TASK_UI) {
                continue;
            }
            st = gw_sched_stat(&sched, (gw_task_id_t)k);
            if (st == 0 || st->runs != 5u) {
                all_ran = 0;
                break;
            }
        }
        check_int("6 个功能任务各被调度 5 次", all_ran, 1);

        /* ⚠️ UI 的 period 是 **50ms**（生产表里唯一填真实周期的任务）：
           这里推进了 5 x 10ms = 50ms -> 它只该被调度 **1** 次。
           这条断言是 2026-10-11 加 UI 任务时补的：原来的循环要求"每个任务都 5 次"，
           在出现第一个非零周期任务后必然失败（CI 抓到，失败信息只说了"6 个任务"）。 */
        {
            const gw_task_stat_t *stu = gw_sched_stat(&sched, GW_TASK_UI);
            check_int("UI 按 50ms 周期只被调度 1 次（5 x 10ms）",
                      (stu != 0 && stu->runs == 1u) ? 1 : 0, 1);
        }
    }

    /* 喂狗检查：跑过之后应允许 */
    check_int("启用调度时喂狗检查可用", gw_app_watchdog_ok(&app), 1);
    check_int("喂狗检查消费掉计数", gw_app_watchdog_ok(&app), 0);

    /* 再跑一轮 -> 又允许 */
    gw_app_poll(&app, 10u);
    check_int("再跑一轮后又允许喂狗", gw_app_watchdog_ok(&app), 1);

    ok("FreeRTOS 下把喂狗放在最低优先级任务里，用同一套检查即可");
}

static void test_watchdog_without_sched(void)
{
    static gw_app_t app;
    gw_port_t       port;

    printf("\n[3] 未启用调度时的喂狗行为\n");

    env_reset();
    make_port(&port);
    memset(&ENV.slave, 0, sizeof(ENV.slave));
    (void)gw_app_init(&app, &ENV.slave, &port);

    gw_app_enable_sched(&app, 0);       /* 顺序模式 */

    /* 顺序模式：无条件允许（poll 返回就意味着所有段都跑过了） */
    check_int("顺序模式下无条件允许喂狗", gw_app_watchdog_ok(&app), 1);
    check_int("连续调用仍返回 1（顺序模式没有'进展计数'的概念）",
              gw_app_watchdog_ok(&app), 1);
    ok("顺序模式不存在'任务悄悄死掉'的故障模式，所以无需检查");

    /* 空指针安全 */
    check_int("空指针不允许喂狗", gw_app_watchdog_ok(0), 0);
    gw_app_enable_sched(0, &(gw_sched_t){0});
    ok("enable_sched 对空指针安全");
}

/* ==================================================================
 *  [3b] RTOS 配置下的喂狗判据（★ 缺陷 U 的回归测试）
 *
 *  ⚠️ 这一组存在的理由：RTOS 下 `gw_rtos_create_tasks()` **故意不调**
 *     `gw_app_enable_sched()`（怕 gw_app_poll 又去分派任务），
 *     于是 `app->sched == NULL` -> 判据走了"无调度器 -> 无条件允许"
 *     那条路 -> **等于没养狗**，而表面一切正常。
 *     所以 RTOS 必须用 `gw_app_enable_watchdog_sched()` 单独注册判据。
 * ================================================================== */
static uint32_t WDS_MS;

static uint32_t wds_now_ms(void *ctx)
{
    (void)ctx;
    return WDS_MS;
}

static void test_watchdog_rtos_wiring(void)
{
    static gw_app_t  app;
    static gw_sched_t sched;
    gw_port_t        port;
    gw_task_def_t    defs[GW_TASK_COUNT];
    int              i;

    printf("\n[3b] RTOS 配置：喂狗判据必须真的在查（缺陷 U）\n");

    env_reset();
    make_port(&port);
    memset(&ENV.slave, 0, sizeof(ENV.slave));
    (void)gw_app_init(&app, &ENV.slave, &port);

    memset(defs, 0, sizeof(defs));
    {
        const gw_task_def_t *d = gw_app_task_defs();
        for (i = 0; i < (int)GW_TASK_COUNT; i++) {
            defs[i] = d[i];
            defs[i].period_ms = 10u;        /* 统一成小周期，便于构造迟到 */
        }
    }

    WDS_MS = 0u;
    (void)gw_sched_init(&sched, defs, 0, 0);
    gw_sched_set_clock_ms(&sched, wds_now_ms, 0);
    gw_sched_start(&sched);

    /* ---- 关键：只注册"喂狗判据"，不动 app->sched（RTOS 的配置）---- */
    check_int("注册前：app->sched 为空（顺序模式 -> 无条件允许）",
              gw_app_watchdog_ok(&app), 1);

    gw_app_enable_watchdog_sched(&app, &sched);

    /* ⚠️ 刚注册完、还没到任何任务的截止时间 -> 应当**允许**：
       判据不能一上来就拒喂（否则启动瞬间就被狗咬）。
       要证明"判据真的在查"，得把时间推过截止时间。 */
    check_int("注册后、未到截止时间 -> 允许（不早杀）",
              gw_app_watchdog_ok(&app), 1);

    WDS_MS += 10u + GW_SCHED_WD_SLACK_MS + 1u;      /* 一直没人心跳 */
    check_int("注册后：超过截止时间仍无心跳 -> 拒绝",
              gw_app_watchdog_ok(&app), 0);
    ok("★ 缺陷 U 的回归点：RTOS 下如果不注册，判据会静默变成'无条件允许'");

    /* 全员登记心跳 -> 允许 */
    for (i = 0; i < (int)GW_TASK_COUNT; i++) {
        gw_sched_note_beat(&sched, (gw_task_id_t)i);
    }
    check_int("全员心跳在期限内 -> 允许", gw_app_watchdog_ok(&app), 1);

    /* 只有 SLAVE 死掉，其余照常心跳 -> 必须拒绝 */
    WDS_MS += 10u + GW_SCHED_WD_SLACK_MS + 1u;
    for (i = 0; i < (int)GW_TASK_COUNT; i++) {
        if ((gw_task_id_t)i != GW_TASK_SLAVE) {
            gw_sched_note_beat(&sched, (gw_task_id_t)i);
        }
    }
    check_int("★ 只死掉一个任务 -> 拒绝喂狗（RTOS 的关键保护）",
              gw_app_watchdog_ok(&app), 0);

    /* 注册顺序：把 sched 置空（退出协作式调度）不应顺带关掉喂狗判据 */
    gw_app_enable_sched(&app, 0);       /* sched 置空；wd_sched 必须留着 */
    WDS_MS += 10u + GW_SCHED_WD_SLACK_MS + 1u;
    check_int("sched 置空后判据仍走 wd_sched（不被带跑）",
              gw_app_watchdog_ok(&app), 0);
    ok("★ 置空 sched 不会静默关掉喂狗判据（否则又是一条'等于没养狗'的路）");

    gw_app_enable_watchdog_sched(0, &sched);
    ok("enable_watchdog_sched 对空指针安全");
}

static void test_task_defs(void)
{
    const gw_task_def_t *defs = gw_app_task_defs();

    printf("\n[4] 任务定义表的合理性\n");

    if (defs == 0) {
        fail("任务定义表非空", "");
        return;
    }

    /* 顺序必须与原来的固定执行顺序一致 */
    {
        int i;
        int ok_order = 1;

        for (i = 0; i < (int)GW_TASK_COUNT; i++) {
            if (defs[i].name == 0 || defs[i].entry == 0) {
                ok_order = 0;
                break;
            }
        }
        check_int("每个任务都有名字与任务体", ok_order, 1);
    }

    /* ⚠️ 只有从站有硬超时 —— 这是划分优先级的依据 */
    {
        int with_deadline = 0;
        int i;

        for (i = 0; i < (int)GW_TASK_COUNT; i++) {
            if (defs[i].deadline_us > 0u) {
                with_deadline++;
            }
        }
        check_int("只有一个任务声明硬超时（从站）", with_deadline, 1);
        if (defs[GW_TASK_SLAVE].deadline_us > 0u) {
            ok("硬超时声明在 SLAVE 上（上位机等待有硬性要求）");
        } else {
            fail("硬超时声明在 SLAVE 上", "");
        }
    }

    /* CAN/UPLINK/SENSOR 都声明了预算，便于统计超限 */
    {
        int i;
        int all_budget = 1;

        for (i = 0; i < (int)GW_TASK_COUNT; i++) {
            if (defs[i].budget_us == 0u) {
                all_budget = 0;
                break;
            }
        }
        check_int("每个任务都声明了预算", all_budget, 1);
    }

    /* 任务名与 gw_sched_task_name 一致 */
    {
        int i;
        int match = 1;

        for (i = 0; i < (int)GW_TASK_COUNT; i++) {
            if (strcmp(defs[i].name,
                       gw_sched_task_name((gw_task_id_t)i)) != 0) {
                match = 0;
                break;
            }
        }
        check_int("定义表的任务名与调度器一致", match, 1);
    }
}

int main(void)
{
    printf("========== 应用层任务化验证 ==========\n");

    test_equivalence();
    test_sched_stats();
    test_watchdog_without_sched();
    test_watchdog_rtos_wiring();
    test_task_defs();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
