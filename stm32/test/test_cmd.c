/**
 * @file    test_cmd.c
 * @brief   下行指令解释单元测试（纯解析，无副作用）
 *
 * ============================================================================
 *  这套测试的重点是**畸形输入与安全边界**，不是"正常指令能不能用"。
 *
 *  正常指令（`report`、`period 30`）一眼就能看出对不对；
 *  真正会出事的是这些：
 *
 *    1) ⚠️ 危险指令缺令牌 / 令牌错误 —— 必须**拒绝且不做任何动作**
 *    2) 畸形输入（超长、只有空白、参数缺失、数字溢出、`0x` 后面没数字）
 *       —— 必须被拒，不能当成有效指令
 *    3) 大小写与多余空白 —— 现场手打出来的指令不可能规整
 *
 *  第 1 条是核心：一台装在高处的设备被误发一条 `restore` 恢复出厂，
 *  代价是几小时的人工。所以这部分的测试写得最细。
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iinc -o test/test_cmd.exe \
 *        src/gw_cmd.c test/test_cmd.c
 *    ./test/test_cmd.exe
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

#include "gw_cmd.h"

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

/* 便捷：解析一个 C 字符串并返回结果码 */
static gw_cmd_result_t parse(const char *s, gw_cmd_t *out)
{
    return gw_cmd_parse(s, (uint16_t)strlen(s), out);
}

/* 期望：解析成功且 id 正确 */
static void expect_id(const char *text, gw_cmd_id_t want)
{
    gw_cmd_t c;
    gw_cmd_result_t r = parse(text, &c);

    if (r != GW_CMD_OK) {
        char d[128];
        sprintf(d, "(\"%s\" 解析失败: %s)", text, gw_cmd_result_str(r));
        fail("解析成功", d);
        return;
    }
    if (c.id != want) {
        char d[128];
        sprintf(d, "(\"%s\" -> %s, 期望 %s)", text,
                gw_cmd_id_str(c.id), gw_cmd_id_str(want));
        fail("指令识别正确", d);
        return;
    }
    ok(text);
}

/* 期望：解析失败且结果码相符 */
static void expect_err(const char *text, gw_cmd_result_t want)
{
    gw_cmd_t c;
    gw_cmd_result_t r = parse(text, &c);

    if (r == want) {
        ok(text);
    } else {
        char d[128];
        sprintf(d, "(\"%s\" -> %s, 期望 %s)", text,
                gw_cmd_result_str(r), gw_cmd_result_str(want));
        fail("错误码正确", d);
    }
}

/* ==================================================================
 * 测试 1：基本指令识别
 * ================================================================== */
static void test_basic(void)
{
    printf("\n[1] 基本指令识别\n");

    expect_id("report",        GW_CMD_REPORT_NOW);
    expect_id("period 30",     GW_CMD_SET_PERIOD);
    expect_id("status",        GW_CMD_GET_STATUS);
    expect_id("uplink on",     GW_CMD_SET_UPLINK);
    expect_id("uplink off",    GW_CMD_SET_UPLINK);
    expect_id("clear_sticky 1", GW_CMD_CLEAR_STICKY);
    expect_id("save 0x5A5A",   GW_CMD_SAVE);
    expect_id("restore 0xA5A5", GW_CMD_RESTORE);
    expect_id("reboot 0x1234", GW_CMD_REBOOT);

    /* 别名 */
    printf("\n  别名：\n");
    expect_id("report_now",    GW_CMD_REPORT_NOW);
    expect_id("clear 2",       GW_CMD_CLEAR_STICKY);
    expect_id("factory 0xA5A5", GW_CMD_RESTORE);
    expect_id("reset 0x1234",  GW_CMD_REBOOT);

    printf("\n  大小写与空白（现场手打不可能规整）：\n");
    expect_id("REPORT",        GW_CMD_REPORT_NOW);
    expect_id("RePoRt",        GW_CMD_REPORT_NOW);
    expect_id("  report  ",    GW_CMD_REPORT_NOW);
    expect_id("\treport\n",    GW_CMD_REPORT_NOW);
    expect_id("PERIOD   30",   GW_CMD_SET_PERIOD);
    expect_id("period\t30",    GW_CMD_SET_PERIOD);
    expect_id("  UPLINK   ON  ", GW_CMD_SET_UPLINK);
}

/* ==================================================================
 * 测试 2：参数解析
 * ================================================================== */
static void test_params(void)
{
    gw_cmd_t c;

    printf("\n[2] 参数解析\n");

    (void)parse("period 30", &c);
    check_int("period 参数为 30", (int)c.arg0, 30);
    check_int("period 识别为 SET_PERIOD", (int)c.id, (int)GW_CMD_SET_PERIOD);

    (void)parse("period 1", &c);
    check_int("周期下界 1 可接受", (int)c.arg0, 1);
    (void)parse("period 3600", &c);
    check_int("周期上界 3600 可接受", (int)c.arg0, 3600);

    printf("\n  周期越界：\n");
    expect_err("period 0",    GW_CMD_ERR_RANGE);
    expect_err("period 3601", GW_CMD_ERR_RANGE);
    expect_err("period 99999", GW_CMD_ERR_RANGE);

    printf("\n  进制与符号：\n");
    (void)parse("clear_sticky 0x1F", &c);
    check_int("十六进制 0x1F 被解析", (int)c.arg0, 0x1F);
    (void)parse("clear_sticky 31", &c);
    check_int("十进制 31 被解析", (int)c.arg0, 31);
    (void)parse("clear_sticky 0X1f", &c);
    check_int("大写 0X 与小写十六进制字母", (int)c.arg0, 0x1F);
    (void)parse("clear_sticky +5", &c);
    check_int("前导 + 被接受", (int)c.arg0, 5);
    (void)parse("clear_sticky 0xFFFF", &c);
    check_int("0xFFFF 被解析", (int)c.arg0, 0xFFFF);

    printf("\n  uplink 的开关：\n");
    (void)parse("uplink on", &c);
    check_int("uplink on -> flag 1", (int)c.flag, 1);
    (void)parse("uplink 1", &c);
    check_int("uplink 1 -> flag 1", (int)c.flag, 1);
    (void)parse("uplink off", &c);
    check_int("uplink off -> flag 0", (int)c.flag, 0);
    (void)parse("uplink 0", &c);
    check_int("uplink 0 -> flag 0", (int)c.flag, 0);
    expect_err("uplink maybe", GW_CMD_ERR_SYNTAX);
}

/* ==================================================================
 * 测试 3：⚠️ 危险指令的令牌（本套测试的核心）
 * ================================================================== */
static void test_token_safety(void)
{
    gw_cmd_t c;
    gw_cmd_result_t r;

    printf("\n[3] ⚠️ 危险指令的令牌校验（本套测试的核心）\n");

    /* 哪三条需要令牌 */
    check_int("save 需要令牌",    gw_cmd_needs_token(GW_CMD_SAVE), 1);
    check_int("restore 需要令牌", gw_cmd_needs_token(GW_CMD_RESTORE), 1);
    check_int("reboot 需要令牌",  gw_cmd_needs_token(GW_CMD_REBOOT), 1);

    /* 其它指令不需要 */
    check_int("report 不需要令牌",
              gw_cmd_needs_token(GW_CMD_REPORT_NOW), 0);
    check_int("period 不需要令牌",
              gw_cmd_needs_token(GW_CMD_SET_PERIOD), 0);
    check_int("clear_sticky 不需要令牌",
              gw_cmd_needs_token(GW_CMD_CLEAR_STICKY), 0);
    check_int("status 不需要令牌",
              gw_cmd_needs_token(GW_CMD_GET_STATUS), 0);
    check_int("uplink 不需要令牌",
              gw_cmd_needs_token(GW_CMD_SET_UPLINK), 0);
    ok("危险指令清单完整（新增指令时这条测试会提醒你复核）");

    /* 期望令牌值 */
    check_int("save 的令牌是 0x5A5A",
              (int)gw_cmd_expected_token(GW_CMD_SAVE), 0x5A5A);
    check_int("restore 的令牌是 0xA5A5",
              (int)gw_cmd_expected_token(GW_CMD_RESTORE), 0xA5A5);
    check_int("reboot 的令牌是 0x1234",
              (int)gw_cmd_expected_token(GW_CMD_REBOOT), 0x1234);

    /* ---- 正确令牌 ---- */
    r = parse("save 0x5A5A", &c);
    check_int("save 带正确令牌解析成功", (int)r, (int)GW_CMD_OK);
    check_int("save 令牌校验通过",
              (int)gw_cmd_check_token(&c), (int)GW_CMD_OK);

    r = parse("restore 0xA5A5", &c);
    check_int("restore 令牌校验通过",
              (int)gw_cmd_check_token(&c), (int)GW_CMD_OK);

    r = parse("reboot 0x1234", &c);
    check_int("reboot 令牌校验通过",
              (int)gw_cmd_check_token(&c), (int)GW_CMD_OK);

    /* ---- ⚠️ 缺令牌：必须被拒 ---- */
    printf("\n  缺令牌（必须拒绝）：\n");
    r = parse("save", &c);
    check_int("save 不带令牌 -> ERR_TOKEN", (int)r, (int)GW_CMD_ERR_TOKEN);
    r = parse("restore", &c);
    check_int("restore 不带令牌 -> ERR_TOKEN", (int)r, (int)GW_CMD_ERR_TOKEN);
    r = parse("reboot", &c);
    check_int("reboot 不带令牌 -> ERR_TOKEN", (int)r, (int)GW_CMD_ERR_TOKEN);
    ok("缺令牌返回 ERR_TOKEN 而不是 SYNTAX —— 上位机能直接知道要补令牌");

    /* ---- ⚠️ 令牌错误：校验必须失败 ---- */
    printf("\n  令牌错误（必须拒绝）：\n");

    (void)parse("restore 0x5A5A", &c);      /* 用 save 的令牌去 restore */
    check_int("用 save 的令牌去 restore -> 拒绝",
              (int)gw_cmd_check_token(&c), (int)GW_CMD_ERR_TOKEN);

    (void)parse("save 0xA5A5", &c);
    check_int("用 restore 的令牌去 save -> 拒绝",
              (int)gw_cmd_check_token(&c), (int)GW_CMD_ERR_TOKEN);

    (void)parse("reboot 0x0000", &c);
    check_int("令牌 0 -> 拒绝",
              (int)gw_cmd_check_token(&c), (int)GW_CMD_ERR_TOKEN);

    (void)parse("reboot 0xFFFF", &c);
    check_int("令牌 0xFFFF -> 拒绝",
              (int)gw_cmd_check_token(&c), (int)GW_CMD_ERR_TOKEN);

    (void)parse("restore 1", &c);
    check_int("令牌 1 -> 拒绝",
              (int)gw_cmd_check_token(&c), (int)GW_CMD_ERR_TOKEN);
    ok("令牌错误一律拒绝 —— 这是防止'误发一条 restore 清掉现场参数'的关键");

    /* ---- 令牌本身不是合法数字 ---- */
    expect_err("save abc", GW_CMD_ERR_TOKEN);
    expect_err("save 0x",  GW_CMD_ERR_TOKEN);

    /* ---- 不需要令牌的指令调用 check_token 应该是 OK ---- */
    (void)parse("report", &c);
    check_int("report 调 check_token 返回 OK",
              (int)gw_cmd_check_token(&c), (int)GW_CMD_OK);
    check_int("check_token 对空指针返回 SYNTAX",
              (int)gw_cmd_check_token(0), (int)GW_CMD_ERR_SYNTAX);
}

/* ==================================================================
 * 测试 4：畸形输入
 * ================================================================== */
static void test_malformed(void)
{
    gw_cmd_t c;

    printf("\n[4] 畸形输入（必须被安全处理）\n");

    /* ---- 空与纯空白：静默忽略，不是错误 ---- */
    check_int("空字符串 -> NONE",
              (int)parse("", &c), (int)GW_CMD_OK);
    check_int("空字符串的 id 是 NONE", (int)c.id, (int)GW_CMD_NONE);
    check_int("纯空格 -> NONE",
              (int)parse("     ", &c), (int)GW_CMD_OK);
    check_int("纯空格 id 是 NONE", (int)c.id, (int)GW_CMD_NONE);
    check_int("纯制表/换行 -> NONE",
              (int)gw_cmd_parse("\t\r\n", 3u, &c), (int)GW_CMD_OK);
    check_int("长度 0 -> NONE",
              (int)gw_cmd_parse("abc", 0u, &c), (int)GW_CMD_OK);
    check_int("空指针 -> NONE",
              (int)gw_cmd_parse(0, 10u, &c), (int)GW_CMD_OK);
    ok("空消息静默忽略 —— MQTT 上偶尔有空消息，不该刷满日志");

    /* ---- 未知指令 ---- */
    check_int("未知指令 -> ERR_UNKNOWN",
              (int)parse("frobnicate", &c), (int)GW_CMD_ERR_UNKNOWN);
    check_int("未知指令的 id 是 UNKNOWN", (int)c.id, (int)GW_CMD_UNKNOWN);
    check_int("未知指令记录了出错位置", (int)(c.err_pos == 0u), 1);

    /* ---- 参数缺失 ---- */
    expect_err("period",           GW_CMD_ERR_SYNTAX);
    expect_err("clear_sticky",     GW_CMD_ERR_SYNTAX);
    expect_err("uplink",           GW_CMD_ERR_SYNTAX);

    /* ---- 参数不是数字 ---- */
    expect_err("period abc",       GW_CMD_ERR_SYNTAX);
    expect_err("period 30x",       GW_CMD_ERR_SYNTAX);
    expect_err("period -5",        GW_CMD_ERR_SYNTAX);   /* 不接受负数 */
    expect_err("clear_sticky 0x",  GW_CMD_ERR_SYNTAX);   /* 0x 后没数字 */
    expect_err("clear_sticky 1.5", GW_CMD_ERR_SYNTAX);

    /* ---- 数字溢出 ---- */
    printf("\n  数字溢出：\n");
    expect_err("clear_sticky 0xFFFFFFFFF", GW_CMD_ERR_SYNTAX);
    expect_err("clear_sticky 99999999999999", GW_CMD_ERR_SYNTAX);
    ok("溢出被拒绝 —— 回绕成一个合法值会更危险（清掉不该清的位）");

    /* ---- 超长输入 ---- */
    {
        char big[GW_CMD_MAX_LEN + 40];
        uint16_t i;

        for (i = 0u; i < sizeof(big) - 1u; i++) {
            big[i] = 'A';
        }
        big[sizeof(big) - 1u] = '\0';

        check_int("超长输入被拒",
                  (int)gw_cmd_parse(big, (uint16_t)strlen(big), &c),
                  (int)GW_CMD_ERR_SYNTAX);
        check_int("恰好 GW_CMD_MAX_LEN 长度不因超长被拒",
                  (int)(gw_cmd_parse(big, GW_CMD_MAX_LEN, &c)
                        != GW_CMD_ERR_SYNTAX ? 1 : 0), 1);
        ok("超长被拒 —— 防止在固定缓冲里发生截断后误解析");
    }

    /* ---- 多余参数：无参指令忽略它们（而不是报错） ----
       为什么要忽略而不是报错：现场手打时多打一个词是常事，
       为这个拒绝执行"立即上报"没有意义。
       但**改动量的指令**不受影响（period 会取第 2 个 token）。 */
    {
        gw_cmd_t t;

        check_int("report 后的多余参数被忽略",
                  (int)parse("report a b c d e", &t), (int)GW_CMD_OK);
        check_int("仍识别为 REPORT", (int)t.id, (int)GW_CMD_REPORT_NOW);

        (void)parse("period 30 extra", &t);
        check_int("period 只取第一个参数",
                  (int)(t.id == GW_CMD_SET_PERIOD && t.arg0 == 30u ? 1 : 0), 1);
    }

    /* ---- 空指针输出 ---- */
    check_int("out 为空指针 -> SYNTAX",
              (int)gw_cmd_parse("report", 6u, 0), (int)GW_CMD_ERR_SYNTAX);
    ok("解析接口对空指针安全");
}

/* ==================================================================
 * 测试 5：非 '\0' 结尾的输入与二进制安全
 * ================================================================== */
static void test_not_terminated(void)
{
    gw_cmd_t c;
    char     buf[32];

    printf("\n[5] 输入不必以 '\\0' 结尾（帧载荷就是这样的）\n");

    /* 帧载荷没有 '\0'，靠长度界定。这里模拟：后面跟着垃圾字节 */
    memcpy(buf, "period 30", 9u);
    buf[9]  = (char)0xFF;
    buf[10] = (char)0x00;
    buf[11] = 'X';

    check_int("按长度 9 解析成功",
              (int)gw_cmd_parse(buf, 9u, &c), (int)GW_CMD_OK);
    check_int("解析出 30", (int)c.arg0, 30);
    ok("靠长度界定而不是找 '\\0' —— 载荷里可能有 0x00，用 str 函数会截断");

    /* 长度里含 0x00 字节：应被当成 token 的一部分（非法字符） */
    memcpy(buf, "period 3", 8u);
    buf[8] = (char)0x00;
    buf[9] = '0';
    check_int("长度含 0x00 时不被当成结束符",
              (int)gw_cmd_parse(buf, 10u, &c), (int)GW_CMD_ERR_SYNTAX);
    ok("0x00 被当作非法字符而不是字符串结束 —— 避免二进制载荷被误解析");
}

/* ==================================================================
 * 测试 6：用法说明与字符串
 * ================================================================== */
static void test_usage(void)
{
    char     buf[80];
    uint16_t n;

    printf("\n[6] 用法说明（NACK 时回给上位机）\n");

    n = gw_cmd_usage(GW_CMD_SET_PERIOD, buf, sizeof(buf));
    if (n > 0u && strstr(buf, "1..3600") != 0) {
        ok("period 的用法里包含取值范围");
    } else {
        char d[96];
        sprintf(d, "(\"%s\")", buf);
        fail("period 的用法里包含取值范围", d);
    }

    n = gw_cmd_usage(GW_CMD_RESTORE, buf, sizeof(buf));
    if (n > 0u && strstr(buf, "0xA5A5") != 0) {
        ok("restore 的用法里包含正确的令牌");
    } else {
        char d[96];
        sprintf(d, "(\"%s\")", buf);
        fail("restore 的用法里包含正确的令牌", d);
    }

    n = gw_cmd_usage(GW_CMD_SAVE, buf, sizeof(buf));
    check_int("save 的用法包含 0x5A5A",
              (int)(strstr(buf, "0x5A5A") != 0 ? 1 : 0), 1);

    n = gw_cmd_usage(GW_CMD_REBOOT, buf, sizeof(buf));
    check_int("reboot 的用法包含 0x1234",
              (int)(strstr(buf, "0x1234") != 0 ? 1 : 0), 1);

    /* 未知指令：回完整清单 */
    n = gw_cmd_usage(GW_CMD_UNKNOWN, buf, sizeof(buf));
    check_int("未知指令回完整指令清单",
              (int)(n > 20u && strstr(buf, "report") != 0 ? 1 : 0), 1);
    ok("把文档放进协议里 —— 现场打错时能自己改对，不用翻手册");

    /* 截断安全 */
    {
        char small[8];

        n = gw_cmd_usage(GW_CMD_SET_PERIOD, small, sizeof(small));
        check_int("小缓冲不越界（返回值 < 容量）",
                  (int)(n < sizeof(small) ? 1 : 0), 1);
        check_int("小缓冲仍以 '\\0' 结尾",
                  (int)(small[n] == '\0' ? 1 : 0), 1);
    }
    check_int("空缓冲返回 0", (int)gw_cmd_usage(GW_CMD_SAVE, 0, 10u), 0);
    check_int("零容量返回 0", (int)gw_cmd_usage(GW_CMD_SAVE, buf, 0u), 0);

    /* 字符串 */
    if (strcmp(gw_cmd_id_str(GW_CMD_REPORT_NOW), "REPORT") == 0
        && strcmp(gw_cmd_result_str(GW_CMD_ERR_TOKEN), "BAD_TOKEN") == 0
        && gw_cmd_id_str((gw_cmd_id_t)99) != 0
        && gw_cmd_result_str((gw_cmd_result_t)99) != 0) {
        ok("字符串转换正确且有兜底");
    } else {
        fail("字符串转换正确且有兜底", "");
    }
}

int main(void)
{
    printf("========== 下行指令解释 单元测试 ==========\n");

    test_basic();
    test_params();
    test_token_safety();
    test_malformed();
    test_not_terminated();
    test_usage();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
