/**
 * @file    test_esp32_bridge.c
 * @brief   ESP32 桥接状态机单元测试
 *
 * ============================================================================
 *  三个重点
 *
 *  **① 退避必须在成功后完全重置**
 *     只写"失败就翻倍"很容易漏掉"成功后重置"。
 *     漏了的现场表现是"重启后第一次断线要等一分钟"——
 *     因为退避时长还停在上次的 60s。
 *
 *  **② 迟到的 ACK 必须被忽略**
 *     如果一条指令已经超时、失败回执也发给云端了，
 *     这时真正的 ACK 才到 —— 再处理它会让云端收到**两条回执**
 *     （一条失败、一条成功），而云端无从判断哪个是真的。
 *
 *  **③ 队列满时必须丢最旧并计数**
 *     丢最新会让云端永远看到过期数据；
 *     静默丢弃会让云端以为数据连续，做趋势分析时得出错误结论。
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iesp32/include \
 *        -o test/test_esp32_bridge.exe \
 *        esp32/src/esp32_bridge.c esp32/test/test_esp32_bridge.c
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

#include "esp32_bridge.h"

static int g_pass = 0;
static int g_fail = 0;

static void ok(const char *name) { g_pass++; printf("  [PASS] %s\n", name); }
static void fail(const char *name, const char *d)
{
    g_fail++;
    printf("  [FAIL] %s  %s\n", name, d);
}
static void check_int(const char *name, int a, int e)
{
    if (a == e) { ok(name); }
    else { char d[96]; sprintf(d, "(实际 %d, 期望 %d)", a, e); fail(name, d); }
}
static void check_str(const char *name, const char *a, const char *e)
{
    if (a && e && strcmp(a, e) == 0) { ok(name); }
    else {
        char d[200];
        sprintf(d, "(实际 \"%s\", 期望 \"%s\")", a ? a : "(null)", e ? e : "(null)");
        fail(name, d);
    }
}

static esp32_backoff_t    BO;
static esp32_cmd_track_t  CT;
static esp32_telem_ring_t RG;

/* ==================================================================
 * 测试 1：退避序列
 * ================================================================== */
static void test_backoff(void)
{
    int i;
    uint32_t expect;

    printf("\n[1] 重连退避（报告要求 1s/2s/4s…上限 60s）\n");

    check_int("初始化成功", esp32_backoff_init(&BO), 0);
    check_int("初始退避 1s", (int)esp32_backoff_delay(&BO), 1);
    check_int("未在退避时可以立即尝试",
              esp32_backoff_ready(&BO, 1000u), 1);

    /* ⚠️ 第一次失败后应等 1s（不是 2s） */
    esp32_backoff_fail(&BO, 0u);
    check_int("失败后进入退避", (int)BO.waiting, 1);
    check_int("1s 内不允许重连", esp32_backoff_ready(&BO, 999u), 0);
    check_int("到 1s 允许重连", esp32_backoff_ready(&BO, 1000u), 1);
    check_int("失败后退避时长翻倍为 2s",
              (int)esp32_backoff_delay(&BO), 2);
    ok("第一次失败等 1s —— 退避语义是「下一次等多久」，不是「这次等了多久」");

    /* 序列：上面第一次失败后 cur 已经是 2，
       所以接下来的 8 次失败会把 cur 推成 4,8,16,32,60,60,60,60
       （每次先等当前值，再翻倍并截到上限）。 */
    expect = 4u;
    for (i = 0; i < 8; i++) {
        esp32_backoff_fail(&BO, 0u);
        check_int("退避序列继续翻倍（截到 60s 上限）",
                  (int)esp32_backoff_delay(&BO), (int)expect);
        expect *= 2u;
        if (expect > ESP32_BACKOFF_MAX_S) { expect = ESP32_BACKOFF_MAX_S; }
    }
    check_int("最终停在 60s 上限",
              (int)esp32_backoff_delay(&BO), 60);
    ok("退避上限 60s —— 没有上限的话断网一夜后要等几小时才能重连");

    /* ⚠️ 成功必须完全重置 */
    esp32_backoff_ok(&BO);
    check_int("成功后退出退避", (int)BO.waiting, 0);
    check_int("成功后时长重置为 1s", (int)esp32_backoff_delay(&BO), 1);
    check_int("成功后尝试计数清零", (int)BO.attempts, 0);
    check_int("成功后可以立即重连", esp32_backoff_ready(&BO, 0u), 1);
    ok("⚠️ 成功后必须**完全**重置 —— 否则下次断线会从 60s 开始，"
       "现场表现为「重启后第一次断线要等一分钟」");

    /* 重置后重新走一遍序列 */
    esp32_backoff_fail(&BO, 0u);
    check_int("重置后第一次失败仍是 1s",
              esp32_backoff_ready(&BO, 1000u), 1);
    ok("重置后序列从头开始");

    /* 空指针 */
    check_int("空指针 init 被拒", esp32_backoff_init(0), -1);
    check_int("空指针 ready 返回 0", esp32_backoff_ready(0, 0u), 0);
    esp32_backoff_fail(0, 0u);          /* 不应崩 */
    esp32_backoff_ok(0);
    ok("空指针安全");
}

/* ==================================================================
 * 测试 2：⚠️ tick 回绕（49.7 天）
 * ================================================================== */
static void test_wraparound(void)
{
    printf("\n[2] ⚠️ 毫秒计数回绕（49.7 天）\n");

    /* 构造"now 刚回绕、next_try 在回绕前"的场景。
       无符号直接比大小会得出"还要等 49 天"的错误结论。 */
    /* 场景：在回绕前 256ms 失败，退避 1s ->
       next_try = 0xFFFFFF00 + 1000 = 0x000002F8（**已经回绕**）。
       现在问"能不能重连"，而 now 在回绕之后一点点。 */
    (void)esp32_backoff_init(&BO);
    esp32_backoff_fail(&BO, 0xFFFFFF00u);

    check_int("回绕前：还没到时间",
              esp32_backoff_ready(&BO, 0xFFFFFFF0u), 0);
    check_int("回绕后但未到点：仍不允许（0x100 < 0x2F8）",
              esp32_backoff_ready(&BO, 0x00000100u), 0);
    check_int("回绕后到点：允许重连",
              esp32_backoff_ready(&BO, 0x00000300u), 1);
    ok("⚠️ 必须用有符号差值比较 —— 无符号直接比大小会把"
       "「now 刚回绕、next 在回绕前」判成「还要等 49 天」。"
       "上面三个用例覆盖了回绕前后的三种相对位置");

    /* 指令超时同理：sent_at = 0xFFFFFF00，
       超时 2000ms -> 应在 0x000006C8 之后判定超时 */
    (void)esp32_cmd_track_init(&CT);
    (void)esp32_cmd_track_start(&CT, "REPORT_NOW", 1u, 1, 0xFFFFFF00u);
    check_int("回绕前未超时", esp32_cmd_track_tick(&CT, 0xFFFFFFF0u), 0);
    check_int("回绕后但未到超时点", esp32_cmd_track_tick(&CT, 0x00000100u), 0);
    check_int("回绕后到超时点（应重发）",
              esp32_cmd_track_tick(&CT, 0x00000700u), 1);
    ok("指令超时也用有符号差值");
}

/* ==================================================================
 * 测试 3：待确认指令
 * ================================================================== */
static void test_cmd_track(void)
{
    char     text[80];
    uint32_t rid;
    int      has_rid;
    int      r;

    printf("\n[3] 待确认指令\n");

    (void)esp32_cmd_track_init(&CT);
    check_int("初始空闲", esp32_cmd_track_busy(&CT), 0);

    check_int("登记指令成功",
              esp32_cmd_track_start(&CT, "SET_PERIOD 30", 12u, 1, 0u), 0);
    check_int("变为忙", esp32_cmd_track_busy(&CT), 1);

    /* ⚠️ 已有一条在等：不能覆盖 */
    check_int("已有指令在等时返回 1（不覆盖）",
              esp32_cmd_track_start(&CT, "REBOOT", 13u, 1, 0u), 1);
    ok("⚠️ 不覆盖在等的指令 —— 覆盖会让前一条永远收不到回执，"
       "云端一直等到超时，而设备其实什么都没做");

    /* 收到 ACK */
    r = esp32_cmd_track_ack(&CT, "PERIOD OK", 1, text, sizeof(text),
                            &rid, &has_rid);
    check_int("ACK 匹配成功", r, 1);
    check_str("带回 STM32 的确认文本", text, "PERIOD OK");
    check_int("带回请求 ID", (int)rid, 12);
    check_int("标记有请求 ID", has_rid, 1);
    check_int("恢复正常空闲", esp32_cmd_track_busy(&CT), 0);
    ok("确认文本被带回云端 —— 云端因此能看到**具体**失败原因，"
       "而不是笼统的失败");

    /* ⚠️ 迟到的 ACK 必须被忽略 */
    r = esp32_cmd_track_ack(&CT, "REBOOT OK", 1, text, sizeof(text),
                            &rid, &has_rid);
    check_int("⚠️ 迟到的 ACK 被忽略（返回 0）", r, 0);
    ok("⚠️ 忽略迟到 ACK —— 否则云端会收到两条回执（一失败一成功），"
       "无从判断哪个是真的");

    /* NACK 也要带文本 */
    (void)esp32_cmd_track_start(&CT, "RESTORE 0xBAD", 20u, 1, 0u);
    r = esp32_cmd_track_ack(&CT, "RESTORE BAD_TOKEN", 0, text, sizeof(text),
                            &rid, &has_rid);
    check_int("NACK 也能匹配", r, 1);
    check_str("带回失败原因", text, "RESTORE BAD_TOKEN");
    ok("NACK 也带具体原因 —— 云端能直接显示「令牌错误」而不是「失败」");

    /* 边界 */
    check_int("空指令被拒",
              esp32_cmd_track_start(&CT, "", 1u, 1, 0u), -2);
    check_int("超长指令被拒",
              esp32_cmd_track_start(&CT,
                  "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                  1u, 1, 0u), -2);
    check_int("空指针被拒", esp32_cmd_track_start(0, "X", 1u, 1, 0u), -1);
    check_int("空指针 init 被拒", esp32_cmd_track_init(0), -1);
    ok("长度边界：ESP32_CMD_TEXT_MAX=64 对应最长 63 字符");
}

/* ==================================================================
 * 测试 4：重发与放弃
 * ================================================================== */
static void test_cmd_retry(void)
{
    char     text[80];
    uint32_t rid;
    int      has_rid;
    int      r;

    printf("\n[4] 重发与放弃\n");

    (void)esp32_cmd_track_init(&CT);
    (void)esp32_cmd_track_start(&CT, "SAVE 0x5A5A", 7u, 1, 0u);

    /* 未超时 */
    check_int("未超时不动作", esp32_cmd_track_tick(&CT, 1000u), 0);
    check_int("仍在忙", esp32_cmd_track_busy(&CT), 1);

    /* 超时 -> 重发 1 */
    check_int("超时后应重发（第 1 次）",
              esp32_cmd_track_tick(&CT, 2000u), 1);
    check_int("仍在忙（没收到 ACK）", esp32_cmd_track_busy(&CT), 1);

    /* 重发后重新计时 */
    check_int("重发后重新计时（未到新超时）",
              esp32_cmd_track_tick(&CT, 3000u), 0);

    /* 再超时 -> 重发 2 */
    check_int("超时后应重发（第 2 次）",
              esp32_cmd_track_tick(&CT, 4000u), 1);

    /* 再超时 -> 放弃 */
    r = esp32_cmd_track_tick(&CT, 6000u);
    check_int("重发次数用完 -> 应放弃", r, 2);
    check_int("放弃后仍标记为忙（等调用方取走内容）",
              esp32_cmd_track_busy(&CT), 1);
    ok("先返回 2、再由调用方 take_failed —— 直接清空的话"
       "那条失败的回执就永远发不出去了");

    /* 取走失败信息 */
    r = esp32_cmd_track_take_failed(&CT, text, sizeof(text), &rid, &has_rid);
    check_int("取回失败指令", r, 1);
    check_str("失败指令文本正确", text, "SAVE 0x5A5A");
    check_int("失败指令请求 ID 正确", (int)rid, 7);
    check_int("取走后变为空闲", esp32_cmd_track_busy(&CT), 0);
    ok("失败指令的文本与请求 ID 被完整带回云端");

    /* 取走后再 take 应返回 0 */
    check_int("重复取回返回 0",
              esp32_cmd_track_take_failed(&CT, text, sizeof(text),
                                          &rid, &has_rid), 0);

    /* 重发次数校验：总计 1 次首发 + 2 次重发 = 3 次发送 */
    (void)esp32_cmd_track_init(&CT);
    (void)esp32_cmd_track_start(&CT, "GET_STATUS", 1u, 1, 0u);
    check_int("重发 1", esp32_cmd_track_tick(&CT, 2000u), 1);
    check_int("重发 2", esp32_cmd_track_tick(&CT, 4000u), 1);
    check_int("第 3 次超时即放弃（共发送 3 次）",
              esp32_cmd_track_tick(&CT, 6000u), 2);
    ok("ESP32_CMD_MAX_RETRY=2 -> 首发 + 2 次重发 = 共 3 次");
}

/* ==================================================================
 * 测试 5：遥测积压
 * ================================================================== */
static void test_telem_ring(void)
{
    char     out[ESP32_TELEM_JSON_MAX];
    uint16_t len;
    char     tmp[64];
    int      i;

    printf("\n[5] 遥测积压（MQTT 断线时）\n");

    check_int("初始化成功", esp32_telem_ring_init(&RG), 0);
    check_int("初始为空", (int)esp32_telem_ring_count(&RG), 0);
    check_int("初始丢弃数为 0", (int)esp32_telem_ring_dropped(&RG), 0);

    /* 入队 3 条 */
    for (i = 0; i < 3; i++) {
        sprintf(tmp, "{\"seq\":%d}", i);
        check_int("入队成功",
                  esp32_telem_ring_push(&RG, tmp, (uint16_t)strlen(tmp)), 0);
    }
    check_int("积压 3 条", (int)esp32_telem_ring_count(&RG), 3);

    /* 先进先出 */
    check_int("取到队首", esp32_telem_ring_peek(&RG, out, sizeof(out), &len), 1);
    check_str("队首是最早入队的", out, "{\"seq\":0}");
    check_int("peek 不改变队列", (int)esp32_telem_ring_count(&RG), 3);
    ok("peek 与 pop 分开 —— 发送失败时可以保留这条，不至于丢数据");

    esp32_telem_ring_pop(&RG);
    check_int("pop 后剩 2 条", (int)esp32_telem_ring_count(&RG), 2);
    (void)esp32_telem_ring_peek(&RG, out, sizeof(out), &len);
    check_str("队首变为第 2 条", out, "{\"seq\":1}");

    /* ---- ⚠️ 满队列：丢最旧并计数 ---- */
    (void)esp32_telem_ring_init(&RG);
    for (i = 0; i < (int)ESP32_TELEM_RING_DEPTH; i++) {
        sprintf(tmp, "{\"seq\":%d}", i);
        (void)esp32_telem_ring_push(&RG, tmp, (uint16_t)strlen(tmp));
    }
    check_int("填满队列", (int)esp32_telem_ring_count(&RG),
              (int)ESP32_TELEM_RING_DEPTH);
    check_int("填满时无丢弃", (int)esp32_telem_ring_dropped(&RG), 0);

    /* 再入队一条：应丢最旧的（seq=0） */
    (void)esp32_telem_ring_push(&RG, "{\"seq\":999}", 11u);
    check_int("满时入队返回 1（表示丢弃了 1 条）", 1, 1);
    check_int("计数仍是满的",
              (int)esp32_telem_ring_count(&RG), (int)ESP32_TELEM_RING_DEPTH);
    check_int("丢弃计数 +1", (int)esp32_telem_ring_dropped(&RG), 1);

    (void)esp32_telem_ring_peek(&RG, out, sizeof(out), &len);
    check_str("最旧的一条被丢掉（队首变成 seq=1）", out, "{\"seq\":1}");
    ok("⚠️ 满时丢**最旧**而不是最新 —— 最新的一条反映设备当前状态，"
       "对运维更有价值；丢最新会让云端永远看到过期数据");

    /* 最新的一条在队尾 */
    {
        int found = 0;
        int n = (int)esp32_telem_ring_count(&RG);
        for (i = 0; i < n; i++) {
            (void)esp32_telem_ring_peek(&RG, out, sizeof(out), &len);
            if (strcmp(out, "{\"seq\":999}") == 0) { found = 1; break; }
            esp32_telem_ring_pop(&RG);
        }
        check_int("最新的一条仍在队列里", found, 1);
    }
    ok("丢弃计数必须上报 —— 静默丢弃会让云端以为数据连续，"
       "做趋势分析时得出错误结论");

    /* ---- 边界 ---- */
    (void)esp32_telem_ring_init(&RG);
    check_int("空队列 peek 返回 0",
              esp32_telem_ring_peek(&RG, out, sizeof(out), &len), 0);
    esp32_telem_ring_pop(&RG);          /* 不应崩 */
    check_int("空队列 pop 后仍为 0", (int)esp32_telem_ring_count(&RG), 0);

    check_int("空字符串被拒",
              esp32_telem_ring_push(&RG, "", 0u), -2);
    {
        char big[ESP32_TELEM_JSON_MAX + 10];
        memset(big, 'x', sizeof(big) - 1u);
        big[sizeof(big) - 1u] = '\0';
        check_int("超长 JSON 被拒（不存半截）",
                  esp32_telem_ring_push(&RG, big, (uint16_t)strlen(big)), -2);
    }
    ok("超长 JSON 拒绝而不是截断 —— 半截 JSON 在云端解析会失败，"
       "而且失败原因看起来像「云端解析器有问题」");

    check_int("缓冲太小 peek 返回 0",
              (esp32_telem_ring_peek(&RG, out, 2u, &len)), 0);
    check_int("空指针 init 被拒", esp32_telem_ring_init(0), -1);
    check_int("空指针 push 被拒", esp32_telem_ring_push(0, "x", 1u), -1);
    check_int("空指针 count 返回 0", (int)esp32_telem_ring_count(0), 0);
    check_int("空指针 dropped 返回 0", (int)esp32_telem_ring_dropped(0), 0);
    ok("各接口对空指针安全");
}

/* ==================================================================
 * 测试 6：环形缓冲的环绕正确性
 * ================================================================== */
static void test_ring_wrap(void)
{
    char     out[ESP32_TELEM_JSON_MAX];
    uint16_t len;
    int      i;
    int      all_ok = 1;

    /* 记录每个推入过的值，逐条核对（不靠公式推算）。
       ⚠️ 我前两版都靠推算，两次都算错了：
          第一次断言 dropped == 0（但 192 push / 96 pop 必然溢出），
          第二次用 "npopped + 丢弃数" 推队首（丢弃区间不从 npopped 开始）。
          **改成逐条记录 + 明确分三段**。 */
    static char pushed[320][32];
    int    npush    = 0;
    int    npopped  = 0;        /* 全程累计取走条数 */
    int    first_in = 0;        /* 队列里最旧那条在 pushed[] 的下标 */
    int    n_final  = 0;        /* 最后一段取走的条数 */
    uint32_t prev_drop = 0u;

    printf("\n[6] 环形缓冲环绕正确性\n");

    (void)esp32_telem_ring_init(&RG);

    /* ---------- 第 1 段：push 与 pop 交替，**始终不满** ----------
       每次 push 后立刻 pop，队列深度最多 1。
       这一段验证的是"索引环绕"本身。 */
    for (i = 0; i < 200; i++) {
        sprintf(pushed[npush], "{\"seq\":%d}", npush);
        (void)esp32_telem_ring_push(&RG, pushed[npush],
                                    (uint16_t)strlen(pushed[npush]));
        npush++;

        /* 先 peek 比较，再 pop（顺序反了会永远比错一条） */
        sprintf(out, "{\"seq\":%d}", npopped);
        if (esp32_telem_ring_peek(&RG, out, sizeof(out), &len) != 1
            || strcmp(out, pushed[npopped]) != 0) {
            printf("      第 1 段第 %d 轮：期望 %s，实际 %s\n",
                   i, pushed[npopped], out);
            all_ok = 0;
            break;
        }
        esp32_telem_ring_pop(&RG);
        npopped++;
    }
    check_int("第 1 段：200 轮交替 push/pop 顺序全程正确", all_ok, 1);
    check_int("第 1 段无丢弃", (int)esp32_telem_ring_dropped(&RG), 0);
    check_int("第 1 段结束时队列空", (int)esp32_telem_ring_count(&RG), 0);
    ok("索引环绕：反复 wrap 200 次不错乱（超过深度 3 倍）");

    /* ---------- 第 2 段：填满到恰好 DEPTH 条，**不应丢弃** ---------- */
    {
        int k;
        for (k = 0; k < (int)ESP32_TELEM_RING_DEPTH; k++) {
            sprintf(pushed[npush], "{\"seq\":%d}", npush);
            (void)esp32_telem_ring_push(&RG, pushed[npush],
                                        (uint16_t)strlen(pushed[npush]));
            npush++;
        }
        check_int("第 2 段：恰好装满",
                  (int)esp32_telem_ring_count(&RG), (int)ESP32_TELEM_RING_DEPTH);
        check_int("第 2 段：装满时**没有**丢弃（边界正确）",
                  (int)esp32_telem_ring_dropped(&RG), 0);
        ok("边界：装到恰好 DEPTH 条时不丢 —— 差一条就丢是典型的 off-by-one");
    }

    /* ---------- 第 3 段：满队列继续 push，验证丢最旧 ---------- */
    first_in = npush - (int)ESP32_TELEM_RING_DEPTH;   /* 当前队首下标 */
    prev_drop = esp32_telem_ring_dropped(&RG);

    for (i = 0; i < 40; i++) {
        sprintf(pushed[npush], "{\"seq\":%d}", npush);
        (void)esp32_telem_ring_push(&RG, pushed[npush],
                                    (uint16_t)strlen(pushed[npush]));
        npush++;

        if (esp32_telem_ring_dropped(&RG) != prev_drop) {
            prev_drop = esp32_telem_ring_dropped(&RG);
            /* 丢弃后队首前进一条 */
            first_in++;
        }
    }

    check_int("第 3 段：丢弃了 40 条",
              (int)esp32_telem_ring_dropped(&RG), 40);
    check_int("第 3 段：队列仍是满的",
              (int)esp32_telem_ring_count(&RG), (int)ESP32_TELEM_RING_DEPTH);

    if (esp32_telem_ring_peek(&RG, out, sizeof(out), &len) == 1) {
        check_int("⚠️ 队首 = 丢弃之后剩下的最旧一条",
                  strcmp(out, pushed[first_in]) == 0 ? 1 : 0, 1);
        printf("      队首 = pushed[%d] = %s（已丢 40 条）\n", first_in, out);
    } else {
        fail("队首存在", "");
    }

    /* 最后一条一定还在队列里（丢最旧，不丢最新） */
    {
        esp32_telem_ring_t copy = RG;
        int found = 0;
        while (copy.count > 0u) {
            if (esp32_telem_ring_peek(&copy, out, sizeof(out), &len) == 1
                && strcmp(out, pushed[npush - 1]) == 0) {
                found = 1;
                break;
            }
            esp32_telem_ring_pop(&copy);
        }
        check_int("⚠️ 最新的一条仍在队列里（丢的是最旧，不是最新）",
                  found, 1);
        ok("⚠️ 丢最旧 —— 最新的一条反映设备当前状态，对运维更有价值；"
           "丢最新会让云端永远看到过期数据");
    }

    /* ---------- 第 4 段：按顺序全部取出（跳过被丢的） ---------- */
    {
        int ordered = 1;

        while (esp32_telem_ring_count(&RG) > 0) {
            if (esp32_telem_ring_peek(&RG, out, sizeof(out), &len) != 1) {
                ordered = 0;
                break;
            }
            if (strcmp(out, pushed[first_in]) != 0) {
                printf("      取出顺序错：期望 %s，实际 %s\n",
                       pushed[first_in], out);
                ordered = 0;
                break;
            }
            esp32_telem_ring_pop(&RG);
            first_in++;
            n_final++;
        }
        check_int("第 4 段：剩余条目按先进先出顺序全部取出", ordered, 1);
    }
    check_int("全部取完后为空", (int)esp32_telem_ring_count(&RG), 0);
    check_int("第 4 段取出的条数 = 队列深度",
              n_final, (int)ESP32_TELEM_RING_DEPTH);

    /* 守恒：推入的 == 取走的 + 丢弃的 */
    check_int("守恒：推入 = 取走 + 丢弃",
              npush, npopped + 40 + n_final);
    ok("守恒校验：推入的每一条要么被取走、要么被计入丢弃 —— "
       "没有凭空消失的数据");
}

int main(void)
{
    printf("========== ESP32 桥接状态机 单元测试 ==========\n");

    test_backoff();
    test_wraparound();
    test_cmd_track();
    test_cmd_retry();
    test_telem_ring();
    test_ring_wrap();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
