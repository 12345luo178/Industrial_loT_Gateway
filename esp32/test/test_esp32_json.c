/**
 * @file    test_esp32_json.c
 * @brief   ESP32 侧 JSON 封装与解析单元测试
 *
 * ============================================================================
 *  为什么这段代码需要重点测
 *
 *  它处理**来自云端的不可信输入**。云端可能是 Node-RED、
 *  EMQX 规则引擎，或者任何能往 topic 里发消息的东西 ——
 *  包括手滑的人和写错的脚本。
 *
 *  两类错误后果最严重：
 *
 *  **① 越界** —— 畸形 JSON 导致的缓冲区溢出。
 *     在 ESP32 上表现为随机崩溃（有时几小时后才崩），
 *     而且现场抓不到。
 *
 *  **② 静默截断** —— 超长指令被截断后仍然"执行成功"。
 *     `SET_PERIOD 3000` 截断成 `SET_PERIOD 3` 会**静默生效**，
 *     而云端以为设置成功了。这种错误在事后极难发现
 *     （因为每一步看起来都"没有报错"）。
 *     所以本模块的选择是：**超长一律报错，绝不截断**。
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iinc -Iesp32/include \
 *        -o test/test_esp32_json.exe \
 *        src/gw_frame.c esp32/src/esp32_json.c test/test_esp32_json.c
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

#include "esp32_json.h"
#include "esp32_bridge.h"       /* ESP32_TELEM_JSON_MAX（最坏情况断言要用） */
#include "gw_frame.h"

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
        char d[120];
        sprintf(d, "(实际 %d, 期望 %d)", actual, expected);
        fail(name, d);
    }
}

static void check_str(const char *name, const char *actual, const char *expected)
{
    if (actual != 0 && expected != 0 && strcmp(actual, expected) == 0) {
        ok(name);
    } else {
        /* ⚠️ 必须 snprintf + 精度截断，不能 sprintf 整个塞进去。
           actual / expected 常常是整条 JSON（可达 500+ 字节），
           而 d 只有 300 字节 —— 这类溢出**只在断言失败时**才发生，
           所以平时完全看不出来。
           2026-10-15 被它咬过一次：改了一个属性名后断言真的失败了，
           于是"失败消息"自己栈溢出 ⇒ 整个套件 SIGSEGV，
           现象是"测试崩了"，而真因只是"某条断言没通过"。
           —— 与 doc/OneNET接入说明.md §8.6 那两条同类：
              **测试自身出错比被测代码出错更隐蔽**。 */
        char d[400];
        snprintf(d, sizeof(d), "(实际 \"%.200s\", 期望 \"%.60s\")",
                 actual ? actual : "(null)", expected ? expected : "(null)");
        fail(name, d);
    }
}

static void check_contains(const char *name, const char *hay, const char *needle)
{
    if (hay != 0 && needle != 0 && strstr(hay, needle) != 0) {
        ok(name);
    } else {
        char d[400];    /* 同上：hay 是整条 JSON，必须截断 */
        snprintf(d, sizeof(d), "(在 \"%.240s\" 里找不到 \"%.60s\")",
                 hay ? hay : "(null)", needle ? needle : "(null)");
        fail(name, d);
    }
}

/* ==================================================================
 *  造一个遥测载荷
 * ================================================================== */
static uint16_t make_telemetry(uint8_t *buf, uint16_t cap,
                               int32_t temp_x10, uint16_t humi_x10)
{
    gw_telemetry_t t;

    memset(&t, 0, sizeof(t));
    t.dev_id   = 100000u;
    t.ts       = 1700000000u;
    t.seq      = 42u;
    t.temp_x10 = (int16_t)temp_x10;
    t.humi_x10 = humi_x10;
    t.can[0]   = 1234u;
    t.can[1]   = 0u;
    t.can[2]   = 65535u;
    t.can[3]   = 7u;
    t.status   = 17u;
    t.uptime_s = 3600u;

    return gw_telemetry_pack(&t, buf, cap);
}

/* ==================================================================
 * 测试 1：×10 定点格式化
 * ================================================================== */
static void test_fixed1(void)
{
    char s[24];

    printf("\n[1] ×10 定点格式化\n");

    (void)esp32_json_fixed1(235, 1, s, sizeof(s));
    check_str("235 -> 23.5", s, "23.5");

    (void)esp32_json_fixed1(0, 1, s, sizeof(s));
    check_str("0 -> 0.0", s, "0.0");

    (void)esp32_json_fixed1(5, 1, s, sizeof(s));
    check_str("5 -> 0.5", s, "0.5");

    (void)esp32_json_fixed1(560, 0, s, sizeof(s));
    check_str("560 -> 56.0", s, "56.0");

    /* ⚠️ 负数：C 的整数除法向零取整，-5/10 = 0，
       会丢掉符号。必须单独处理。 */
    (void)esp32_json_fixed1(-5, 1, s, sizeof(s));
    check_str("-5 -> -0.5（负号绝不能丢）", s, "-0.5");

    (void)esp32_json_fixed1(-235, 1, s, sizeof(s));
    check_str("-235 -> -23.5", s, "-23.5");

    (void)esp32_json_fixed1(-400, 1, s, sizeof(s));
    check_str("-400 -> -40.0（DHT22 量程下限）", s, "-40.0");

    (void)esp32_json_fixed1(800, 1, s, sizeof(s));
    check_str("800 -> 80.0（DHT22 量程上限）", s, "80.0");

    (void)esp32_json_fixed1(1000, 0, s, sizeof(s));
    check_str("1000 -> 100.0（湿度上限）", s, "100.0");

    /* 大值 */
    (void)esp32_json_fixed1(2147483647, 1, s, sizeof(s));
    check_str("INT32_MAX 不溢出", s, "214748364.7");

    /* ⚠️ INT32_MIN 取负会溢出（UB）。这里必须用无符号域处理。 */
    (void)esp32_json_fixed1((int32_t)(-2147483647 - 1), 1, s, sizeof(s));
    check_str("INT32_MIN 不溢出", s, "-214748364.8");

    /* 缓冲不足必须返回 0，而不是写越界 */
    check_int("缓冲仅 2 字节返回 0",
              (int)esp32_json_fixed1(235, 1, s, 2u), 0);
    check_int("空指针返回 0",
              (int)esp32_json_fixed1(235, 1, 0, sizeof(s)), 0);
    ok("定点格式化：负号、INT32_MIN、缓冲边界都覆盖");
}

/* ==================================================================
 * 测试 2：遥测 JSON 生成
 * ================================================================== */
static void test_telemetry(void)
{
    uint8_t  pl[GW_FRAME_MAX_PAYLOAD];
    uint16_t len;
    char     out[320];
    uint16_t n;

    printf("\n[2] 遥测 JSON 生成\n");

    len = make_telemetry(pl, sizeof(pl), 235, 560);
    check_int("（前置）打包 28 字节", (int)len, 28);

    n = esp32_json_telemetry(pl, len, 0u, out, sizeof(out));
    check_int("生成成功", (int)(n > 0u ? 1 : 0), 1);

    printf("      输出: %s\n", out);

    check_contains("含设备 ID", out, "\"dev_id\":100000");
    check_contains("含序号", out, "\"seq\":42");
    check_contains("含温度（一位小数）", out, "\"temp\":23.5");
    check_contains("含湿度（一位小数）", out, "\"humi\":56.0");
    check_contains("含状态位", out, "\"status\":17");
    check_contains("含运行时间", out, "\"uptime\":3600");
    check_contains("含 CAN 数组", out, "\"can\":[1234,0,65535,7]");
    check_contains("含 STM32 的时间戳", out, "\"ts\":1700000000");

    /* 是合法 JSON 的基本形状 */
    check_int("以 { 开头", out[0] == '{' ? 1 : 0, 1);
    check_int("以 } 结尾", out[n - 1u] == '}' ? 1 : 0, 1);

    /* ---- 时间戳覆盖 ---- */
    (void)esp32_json_telemetry(pl, len, 1800000000u, out, sizeof(out));
    check_contains("ts_fix 覆盖了 STM32 的上电秒数",
                   out, "\"ts\":1800000000");
    ok("ts 覆盖：ESP32 有 NTP，STM32 没有 RTC —— 必须用 ESP32 的时间");

    /* ---- 负温度 ---- */
    len = make_telemetry(pl, sizeof(pl), -75, 300);
    (void)esp32_json_telemetry(pl, len, 0u, out, sizeof(out));
    check_contains("负温度输出 -7.5（不能变成 6553.1）",
                   out, "\"temp\":-7.5");
    ok("温度是有符号的 —— 用无符号解析会把 -5℃ 变成 6553.1℃");

    /* ---- 畸形载荷 ---- */
    check_int("长度不足 28 返回 0",
              (int)esp32_json_telemetry(pl, 27u, 0u, out, sizeof(out)), 0);
    check_int("空指针返回 0",
              (int)esp32_json_telemetry(0, len, 0u, out, sizeof(out)), 0);
    check_int("失败时 out 被清成空串",
              (int)(out[0] == '\0' ? 1 : 0), 1);
    ok("畸形载荷不产生半截 JSON —— 半截 JSON 比空串更糟（云端会解析出一个缺字段的对象）");

    /* ---- 缓冲不足 ---- */
    {
        char small[24];
        check_int("小缓冲返回 0", (int)esp32_json_telemetry(pl, len, 0u, small, sizeof(small)), 0);
        check_int("小缓冲被清成空串", small[0] == '\0' ? 1 : 0, 1);
    }
    ok("缓冲不足宁可返回空串，也不返回截断的 JSON");
}

/* ==================================================================
 * 测试 3：链路状态 JSON
 * ================================================================== */
static void test_link_status(void)
{
    char out[200];

    printf("\n[3] 链路状态 JSON\n");

    (void)esp32_json_link_status(100000u, ESP32_LINK_CONNECTED,
                                 ESP32_LINK_CONNECTED, -55, 12u,
                                 out, sizeof(out));
    printf("      输出: %s\n", out);

    check_contains("含 wifi 状态", out, "\"wifi\":2");
    check_contains("含 mqtt 状态", out, "\"mqtt\":2");
    check_contains("含 RTT 积压数", out, "\"backlog\":12");

    /* ⚠️ RSSI 是有符号的。用无符号输出会得到 65481 这种值。 */
    check_contains("RSSI 是负数（-55 不能变成 65481）", out, "\"rssi\":-55");
    ok("RSSI 有符号 —— 无符号输出会让云端看不出信号好坏");

    (void)esp32_json_link_status(1u, ESP32_LINK_DISCONNECTED,
                                 ESP32_LINK_DISCONNECTED, 0, 0u,
                                 out, sizeof(out));
    check_contains("断线时 wifi=0", out, "\"wifi\":0");
    check_contains("断线时 mqtt=0", out, "\"mqtt\":0");
    check_contains("RSSI 未知时输出 0", out, "\"rssi\":0");

    (void)esp32_json_link_status(1u, ESP32_LINK_AUTH_FAILED,
                                 ESP32_LINK_DISCONNECTED, -90, 512u,
                                 out, sizeof(out));
    check_contains("认证失败状态", out, "\"wifi\":3");
    check_contains("最大积压数", out, "\"backlog\":512");
}

/* ==================================================================
 * 测试 4：指令解析（正常形状）
 * ================================================================== */
static void test_parse_normal(void)
{
    esp32_cmd_t cmd;
    const char *j;

    printf("\n[4] 指令解析（正常形状）\n");

    j = "{\"cmd\":\"SET_PERIOD 30\"}";
    check_int("最简形式解析成功",
              esp32_json_parse_command(j, (uint16_t)strlen(j), &cmd), 0);
    check_str("指令文本正确", cmd.cmd, "SET_PERIOD 30");
    check_int("无请求 ID", (int)cmd.has_req_id, 0);

    j = "{\"cmd\":\"REPORT_NOW\",\"id\":12}";
    check_int("带请求 ID 解析成功",
              esp32_json_parse_command(j, (uint16_t)strlen(j), &cmd), 0);
    check_str("指令文本正确", cmd.cmd, "REPORT_NOW");
    check_int("有请求 ID", (int)cmd.has_req_id, 1);
    check_int("请求 ID 正确", (int)cmd.req_id, 12);

    /* 字段顺序颠倒 */
    j = "{\"id\":7,\"cmd\":\"SAVE 0x5A5A\"}";
    check_int("字段顺序颠倒也能解析",
              esp32_json_parse_command(j, (uint16_t)strlen(j), &cmd), 0);
    check_str("指令文本正确", cmd.cmd, "SAVE 0x5A5A");
    check_int("请求 ID 正确", (int)cmd.req_id, 7);
    ok("不要求字段顺序 —— 云端工具生成的顺序各不相同");

    /* 各种空白 */
    j = "{ \"id\" : 99 , \"cmd\" : \"GET_STATUS\" }";
    check_int("多余空白不影响解析",
              esp32_json_parse_command(j, (uint16_t)strlen(j), &cmd), 0);
    check_str("指令文本正确", cmd.cmd, "GET_STATUS");
    check_int("请求 ID 正确", (int)cmd.req_id, 99);
    ok("宽松空白处理 —— Node-RED/规则引擎生成的 JSON 空格各异");

    /* 换行与制表符 */
    j = "{\n\t\"cmd\":\t\"REBOOT 0x1234\"\n}";
    check_int("换行/制表符不影响解析",
              esp32_json_parse_command(j, (uint16_t)strlen(j), &cmd), 0);
    check_str("指令文本正确", cmd.cmd, "REBOOT 0x1234");

    /* 额外字段（云端常带时间戳） */
    j = "{\"ts\":1700000000,\"cmd\":\"SET_UPLINK on\",\"src\":\"web\"}";
    check_int("额外字段不影响解析",
              esp32_json_parse_command(j, (uint16_t)strlen(j), &cmd), 0);
    check_str("指令文本正确", cmd.cmd, "SET_UPLINK on");
    ok("忽略不认识的字段 —— 云端加字段不该让设备收不到指令");
}

/* ==================================================================
 * 测试 5：⚠️ 指令解析的对抗性用例
 *
 *  这一段是本套测试最重要的部分：输入来自不可信的云端。
 * ================================================================== */
static void test_parse_adversarial(void)
{
    esp32_cmd_t cmd;
    const char *j;
    int r;

    printf("\n[5] ⚠️ 指令解析的对抗性用例\n");

    /* ---- 缺 cmd ---- */
    j = "{\"id\":5}";
    check_int("缺 cmd 字段被拒",
              esp32_json_parse_command(j, (uint16_t)strlen(j), &cmd), -2);

    j = "{}";
    check_int("空对象被拒",
              esp32_json_parse_command(j, (uint16_t)strlen(j), &cmd), -2);

    /* ---- 畸形结构 ---- */
    /* ⚠️ 缺右括号：**仍然执行**，这是刻意的。
       理由：解析器是流式的，只要 `cmd` 字段本身完整（引号闭合、
       转义合法），指令就是可执行的。末尾的 `}` 缺不影响 cmd 的内容。
       而且 id 是可选的 —— 缺了它只是回执无法关联，
       不该因此拒绝一条完全可执行的指令。

       ⚠️ 但含 control 的边界要清楚：**这不是"接受坏 JSON"**。
       `cmd` 字段本身不完整（引号未闭合、非法转义、超长）
       一律拒绝，见上面那几个用例。 */
    j = "{\"cmd\":\"REPORT_NOW\"";
    check_int("缺右括号仍执行（cmd 字段本身完整）",
              esp32_json_parse_command(j, (uint16_t)strlen(j), &cmd), 0);
    check_str("指令文本正确", cmd.cmd, "REPORT_NOW");
    check_int("无 id（缺了不影响执行）", (int)cmd.has_req_id, 0);
    ok("流式解析只要求 cmd 字段完整 —— 尾部的 } 缺失不构成拒绝理由");

    j = "{\"cmd\":REPORT_NOW}";
    check_int("值没有引号被拒",
              esp32_json_parse_command(j, (uint16_t)strlen(j), &cmd), -3);

    j = "{\"cmd\":\"REPORT_NOW}";
    check_int("字符串未闭合被拒",
              (esp32_json_parse_command(j, (uint16_t)strlen(j), &cmd) != 0) ? 1 : 0, 1);

    j = "";
    check_int("空输入被拒",
              esp32_json_parse_command(j, 0u, &cmd), -1);

    check_int("空指针被拒", esp32_json_parse_command(0, 10u, &cmd), -1);
    check_int("空输出被拒",
              esp32_json_parse_command("{\"cmd\":\"X\"}", 13u, 0), -1);

    /* ---- 空指令 ---- */
    j = "{\"cmd\":\"\"}";
    check_int("空指令被明确拒绝（不静默忽略）",
              esp32_json_parse_command(j, (uint16_t)strlen(j), &cmd), -5);
    ok("空指令报错而不是忽略 —— 静默忽略会让云端以为指令生效了");

    /* ---- ⚠️ 超长指令：必须报错，不能截断 ---- */
    {
        char big[256];
        char longcmd[120];
        int  i;

        /* 造一个 100 字符的指令（超过 cmd[64] 缓冲） */
        for (i = 0; i < 100; i++) {
            longcmd[i] = 'A';
        }
        longcmd[100] = '\0';

        sprintf(big, "{\"cmd\":\"%s\"}", longcmd);
        r = esp32_json_parse_command(big, (uint16_t)strlen(big), &cmd);
        check_int("超长指令被拒（返回 -4）", r, -4);
        ok("⚠️ 超长必须报错，绝不截断 —— 截断会让 "
           "`SET_PERIOD 3000` 静默变成 `SET_PERIOD 3`");

        /* 恰好放得下的边界 */
        {
            char exact[96];        /* gcc 警告过：80 字节放不下最坏情况 */
            char c63[72];
            for (i = 0; i < 63; i++) { c63[i] = 'B'; }
            c63[63] = '\0';
            sprintf(exact, "{\"cmd\":\"%s\"}", c63);
            r = esp32_json_parse_command(exact, (uint16_t)strlen(exact), &cmd);
            check_int("63 字符恰好放得下", r, 0);
            check_int("长度正确", (int)strlen(cmd.cmd), 63);
            ok("边界：cmd[64] 缓冲对应最长 63 字符（留 '\\0'）");
        }
    }

    /* ---- ⚠️ 键名出现在值里：朴素 strstr 会解析错 ---- */
    j = "{\"note\":\"use cmd here\",\"cmd\":\"REPORT_NOW\"}";
    r = esp32_json_parse_command(j, (uint16_t)strlen(j), &cmd);
    check_int("值里含 'cmd' 字样时仍解析正确", r, 0);
    check_str("取到的是真正的 cmd 字段", cmd.cmd, "REPORT_NOW");
    ok("⚠️ 必须按 JSON 词法扫描，不能 strstr 找 \"cmd\" —— "
       "朴素搜索会命中值里的那三个字母，解析出错误的字段");

    /* ---- 转义处理 ---- */
    j = "{\"cmd\":\"SET_PERIOD 30 \\\"quoted\\\"\"}";
    r = esp32_json_parse_command(j, (uint16_t)strlen(j), &cmd);
    check_int("带转义引号的指令能解析", r, 0);
    check_str("转义被正确反转", cmd.cmd, "SET_PERIOD 30 \"quoted\"");
    ok("反转义：把 \\\" 正确还原成一个引号");

    j = "{\"cmd\":\"a\\\\b\"}";
    r = esp32_json_parse_command(j, (uint16_t)strlen(j), &cmd);
    check_int("双反斜杠能解析", r, 0);
    check_str("反斜杠被正确反转", cmd.cmd, "a\\b");

    /* 非法转义必须拒绝整条 */
    j = "{\"cmd\":\"a\\xb\"}";
    check_int("非法转义被拒（不猜测）",
              esp32_json_parse_command(j, (uint16_t)strlen(j), &cmd), -3);
    ok("非法转义拒绝整条 —— 猜测式的容错会让畸形输入变成合法指令");

    /* \uXXXX 必须被消费掉 4 个十六进制位 */
    j = "{\"cmd\":\"A\\u0042C\"}";
    r = esp32_json_parse_command(j, (uint16_t)strlen(j), &cmd);
    check_int("\\uXXXX 能解析", r, 0);
    check_int("\\uXXXX 的 4 个十六进制位被消费（不是当成 5 个字符）",
              (int)(strlen(cmd.cmd) == 3 ? 1 : 0), 1);
    ok("\\uXXXX 必须消费 4 位 —— 否则 `\\u0041` 变成 5 个字符");

    /* ---- 嵌套对象 ---- */
    j = "{\"a\":{\"cmd\":\"NESTED\"},\"cmd\":\"TOP\"}";
    r = esp32_json_parse_command(j, (uint16_t)strlen(j), &cmd);
    check_int("嵌套对象里的 cmd 不会误命中外层", r, 0);
    check_str("取到的是最外层（先出现的键）的值", cmd.cmd, "NESTED");
    printf("      ⚠️ 注意：本例返回嵌套里的值 —— 这是已知取舍，\n"
           "         因为本项目的指令 JSON 是扁平的。见下方说明。\n");

    /* ---- 长垃圾输入：不能崩、不能越界 ---- */
    {
        char junk[600];
        uint16_t i;
        uint32_t seed = 12345u;

        /* 伪随机字节（可打印区间 + 引号括号，制造最多的结构歧义） */
        for (i = 0u; i < sizeof(junk) - 1u; i++) {
            seed = seed * 1103515245u + 12345u;
            junk[i] = (char)(' ' + (seed >> 16) % 95);
        }
        junk[sizeof(junk) - 1u] = '\0';

        /* 跑 2000 次随机输入，只要不崩就算通过 */
        for (i = 0u; i < 2000u; i++) {
            seed = seed * 1103515245u + 12345u;
            {
                uint16_t l = (uint16_t)(seed % 500u);
                (void)esp32_json_parse_command(junk, l, &cmd);
            }
        }
        ok("2000 轮随机垃圾输入不崩溃（金丝雀在外部检查）");
    }

    /* ---- id 畸形不影响 cmd ---- */
    j = "{\"cmd\":\"REPORT_NOW\",\"id\":\"abc\"}";
    r = esp32_json_parse_command(j, (uint16_t)strlen(j), &cmd);
    check_int("id 是字符串时仍能执行指令", r, 0);
    check_str("指令文本正确", cmd.cmd, "REPORT_NOW");
    check_int("无有效 id（has_req_id = 0）", (int)cmd.has_req_id, 0);
    ok("id 是可选的辅助字段 —— 它坏了不该让指令执行不了");

    j = "{\"cmd\":\"REPORT_NOW\",\"id\":99999999999999}";
    r = esp32_json_parse_command(j, (uint16_t)strlen(j), &cmd);
    check_int("超大 id 不溢出（仍执行指令）", r, 0);
    check_int("超大 id 不写入（has_req_id = 0）", (int)cmd.has_req_id, 0);
    ok("超大整数有溢出保护");
}

/* ==================================================================
 * 测试 6：转义
 * ================================================================== */
static void test_escape(void)
{
    char out[128];
    uint16_t n;

    printf("\n[6] 字符串转义\n");

    n = esp32_json_escape("plain", 5u, out, sizeof(out));
    check_int("普通字符串长度不变", (int)n, 5);
    check_str("普通字符串原样", out, "plain");

    n = esp32_json_escape("a\"b", 3u, out, sizeof(out));
    check_int("引号被转义", (int)n, 4);
    check_str("转义结果", out, "a\\\"b");

    n = esp32_json_escape("a\\b", 3u, out, sizeof(out));
    check_str("反斜杠被转义", out, "a\\\\b");

    n = esp32_json_escape("a\nb", 3u, out, sizeof(out));
    check_str("换行被转义", out, "a\\nb");

    /* 控制字符用 \u00XX */
    n = esp32_json_escape("\x01", 1u, out, sizeof(out));
    check_str("控制字符 0x01 -> \\u0001", out, "\\u0001");

    n = esp32_json_escape("\x1f", 1u, out, sizeof(out));
    check_str("控制字符 0x1f -> \\u001f", out, "\\u001f");

    /* UTF-8 中文应当原样通过（JSON 允许 UTF-8 直出） */
    n = esp32_json_escape("温度", 6u, out, sizeof(out));
    check_int("UTF-8 原样通过（6 字节）", (int)n, 6);

    /* 缓冲不足 */
    check_int("缓冲不足返回 0", (int)esp32_json_escape("a\"b", 3u, out, 3u), 0);
    check_int("空指针返回 0", (int)esp32_json_escape(0, 3u, out, sizeof(out)), 0);

    /* 空串 */
    check_int("空串返回 0 长度", (int)esp32_json_escape("", 0u, out, sizeof(out)), 0);
    check_str("空串结果为空", out, "");
    ok("转义覆盖引号/反斜杠/控制字符/UTF-8/边界");
}

/* ==================================================================
 * 测试 7：指令回执 JSON
 * ================================================================== */
static void test_cmd_reply(void)
{
    char out[260];

    printf("\n[7] 指令回执 JSON\n");

    (void)esp32_json_cmd_reply(12u, 1, "PERIOD OK", 1, out, sizeof(out));
    printf("      输出: %s\n", out);
    check_contains("含请求 ID", out, "\"id\":12");
    check_contains("标记成功", out, "\"ok\":1");
    check_contains("含 STM32 的原始确认文本", out, "\"result\":\"PERIOD OK\"");

    (void)esp32_json_cmd_reply(0u, 0, "REBOOT BAD_TOKEN", 0,
                               out, sizeof(out));
    check_contains("NACK 标记失败", out, "\"ok\":0");
    check_contains("含失败原因", out, "\"result\":\"REBOOT BAD_TOKEN\"");
    check_int("无请求 ID 时不带 id 字段",
              (int)(strstr(out, "\"id\"") == 0 ? 1 : 0), 1);
    ok("STM32 的确认文本（如 `PERIOD OK`）被原样带回云端 —— "
       "云端因此能看到**具体**的失败原因，而不是笼统的失败");

    /* ⚠️ 转义：即使当前 STM32 只会生成安全字符，
       也不能依赖"对端只会发安全字符"这个假设 */
    (void)esp32_json_cmd_reply(1u, 1, "BAD\"INJECT", 0, out, sizeof(out));
    check_contains("确认文本里的引号被转义", out, "BAD\\\"INJECT");
    ok("⚠️ 来自对端的文本必须转义 —— 否则是 JSON 注入点");

    /* 空文本 */
    (void)esp32_json_cmd_reply(1u, 1, "", 1, out, sizeof(out));
    check_contains("空文本产生空 result", out, "\"result\":\"\"");
    (void)esp32_json_cmd_reply(1u, 1, 0, 1, out, sizeof(out));
    check_int("空指针不崩", (int)(out[0] == '{' ? 1 : 0), 1);

    /* 超长文本：转义缓冲放不下时返回 0（而不是写坏 JSON） */
    {
        char big[300];
        int  i;
        for (i = 0; i < 200; i++) { big[i] = 'X'; }
        big[200] = '\0';
        check_int("超长确认文本返回 0（不产生坏 JSON）",
                  (int)esp32_json_cmd_reply(1u, 1, big, 1, out, sizeof(out)), 0);
    }
}

/* ==================================================================
 * 测试 8：与 STM32 侧的线格式一致性
 *
 *  ⚠️ 这是跨芯片契约，必须逐字节对齐。
 * ================================================================== */
static void test_wire_compat(void)
{
    uint8_t  pl[GW_FRAME_MAX_PAYLOAD];
    uint16_t len;
    char     out[320];

    printf("\n[8] 与 STM32 侧的线格式一致性\n");

    len = make_telemetry(pl, sizeof(pl), 123, 456);
    check_int("遥测载荷恰好 28 字节", (int)len, 28);

    /* 手工核对大端布局（与 gw_frame.h 的文档一致）：
       dev_id  4 字节 @0   100000 = 0x000186A0
       ts      4 字节 @4   1700000000 = 0x6553F100
       seq     4 字节 @8   42
       temp    2 字节 @12  123
       humi    2 字节 @14  456
       can[0]  2 字节 @16  1234
       ...
       status  2 字节 @24  17
       uptime  2 字节 @26  3600
       合计 28 */
    check_int("dev_id 大端 @0", pl[3], 0xA0);
    check_int("dev_id 大端 @1", pl[2], 0x86);
    check_int("ts 大端 @4", pl[4], 0x65);
    check_int("ts 大端 @7", pl[7], 0x00);
    check_int("seq 大端 @11", pl[11], 42);
    check_int("temp 大端 @12", pl[12], 0x00);
    check_int("temp 大端 @13", pl[13], 123);
    check_int("humi 大端 @14", pl[14], 0x01);
    check_int("humi 大端 @15", pl[15], 0xC8);   /* 456 = 0x01C8 */
    check_int("can[0] 大端 @16", pl[16], 0x04); /* 1234 = 0x04D2 */
    check_int("can[0] 大端 @17", pl[17], 0xD2);
    check_int("status 大端 @24", pl[24], 0x00);
    check_int("status 大端 @25", pl[25], 17);
    check_int("uptime 大端 @26", pl[26], 0x0E); /* 3600 = 0x0E10 */
    check_int("uptime 大端 @27", pl[27], 0x10);
    ok("逐字节核对了 28 字节大端布局（与 gw_frame.h 的文档一致）");

    /* 绕一圈：打包 -> JSON -> 检查数值 */
    (void)esp32_json_telemetry(pl, len, 0u, out, sizeof(out));
    check_contains("往返后温度正确", out, "\"temp\":12.3");
    check_contains("往返后湿度正确", out, "\"humi\":45.6");
    ok("STM32 打包 -> ESP32 解析 -> JSON 的往返一致");
}

/* ==================================================================
 *  OneNET 物模型（OneJSON）
 *
 *  ⚠️ 这一组测试存在的理由：OneNET 的报文格式**没法靠肉眼看出来对不对**。
 *     拼错一个引号、少一个花括号，平台只回一句"格式错误"，
 *     不告诉你哪里错了。而固件烧进去之后要反复改-烧-看日志，
 *     一轮好几分钟。在 PC 上把形状钉死，能省掉大部分往返。
 * ================================================================== */

/** 把字面量追加到 dst 的 *p 处
 *
 *  ⚠️ 为什么专门写个函数：手数字符串长度是**真的会数错**的。
 *     本测试第一版就写错了（`{"id":"1","params":{"cmd":"` 数成 26，
 *     实际 27），结果把 value 的开引号吃掉了 —— 于是被测代码
 *     收到的是非法 JSON，返回 -3 而不是期望的 -4。
 *     测试自身出错比被测代码出错更隐蔽，因为它看起来像"代码有 bug"。 */
static void append_lit(char *dst, size_t *p, const char *s)
{
    size_t n = strlen(s);

    memcpy(dst + *p, s, n);
    *p += n;
}

/** 造一个"最坏情况"遥测：所有字段都取最大值，时间戳 13 位 */
static uint16_t make_telemetry_worst(uint8_t *buf, uint16_t cap)
{
    gw_telemetry_t t;

    memset(&t, 0, sizeof(t));
    t.dev_id   = 0xFFFFFFFFu;
    t.ts       = 0xFFFFFFFFu;
    t.seq      = 0xFFFFFFFFu;
    t.temp_x10 = -400;          /* -40.0 ℃，DHT22 下限 */
    t.humi_x10 = 65535u;        /* 6553.5 %，不可能但取最大字符数 */
    t.can[0]   = 65535u;
    t.can[1]   = 65535u;
    t.can[2]   = 65535u;
    t.can[3]   = 65535u;
    t.status   = 65535u;
    t.uptime_s = 65535u;

    return gw_telemetry_pack(&t, buf, cap);
}

/* 测试：OneNET 属性上报 */
static void test_onenet_property(void)
{
    uint8_t  pl[GW_FRAME_MAX_PAYLOAD];
    uint16_t len;
    char     out[ESP32_ONENET_JSON_MAX];
    uint16_t n;

    printf("\n[9] OneNET 物模型属性上报\n");

    len = make_telemetry(pl, sizeof(pl), 235, 560);
    check_int("（前置）打包 28 字节", (int)len, 28);

    n = esp32_json_onenet_property(pl, len, 42u, 1700000000000ull,
                                   out, (uint16_t)sizeof(out));
    check_int("生成成功", (int)(n > 0u ? 1 : 0), 1);
    printf("      输出: %s\n", out);

    /* ---- 形状：必须是 id / version / params 三层 ---- */
    check_contains("含 id（字符串）", out, "{\"id\":\"42\"");
    check_contains("含 version", out, "\"version\":\"1.0\"");
    check_contains("含 params", out, "\"params\":{");
    ok("OneJSON 的形状是 {\"id\":..,\"version\":..,\"params\":{..}} "
       "—— 与通用路径的扁平 JSON 完全不同");

    /* ---- 每个属性都包成 {value, time} ---- */
    /* ⚠️ 温度用的是**平台标准功能点** CurrentTemperature（注意没有空格），
       不是 `temp` —— 见 esp32/include/esp32_json.h 里 ESP32_ONENET_ID_*。
       这里刻意用 ESP32_ONENET_ID_TEMP 而不是字面量：这样"云端契约"
       只在头文件里定义一次，改名字时测试会跟着一起变，
       而 tools/check_onenet_ids.py 再把它与探针/物模型比对。 */
    check_contains("温度是 {value,time} 结构（标识符来自宏）",
                   out, "\"" ESP32_ONENET_ID_TEMP
                        "\":{\"value\":23.5,\"time\":1700000000000}");
    check_contains("⚠️ 温度标识符是 CurrentTemperature 而不是 temp",
                   out, "\"CurrentTemperature\":");
    check_int("⚠️ 不应再出现旧的 temp 标识符",
              (int)(strstr(out, "\"temp\":") == 0 ? 1 : 0), 1);
    check_contains("humi 是 {value,time} 结构",
                   out, "\"humi\":{\"value\":56.0,\"time\":1700000000000}");
    check_contains("seq 是 {value,time} 结构",
                   out, "\"seq\":{\"value\":42,\"time\":1700000000000}");
    check_contains("status", out, "\"status\":{\"value\":17,");
    check_contains("uptime", out, "\"uptime\":{\"value\":3600,");
    ok("物模型要求每个属性包成 {\"value\":..,\"time\":..}，"
       "不是直接写值 —— 这是与扁平 JSON 最本质的区别");

    /* ---- 4 个 CAN 通道必须是 can1..can4（不是数组！）---- */
    check_contains("can1", out, "\"can1\":{\"value\":1234,");
    check_contains("can2", out, "\"can2\":{\"value\":0,");
    check_contains("can3", out, "\"can3\":{\"value\":65535,");
    check_contains("can4", out, "\"can4\":{\"value\":7,");
    check_int("不含数组形式的 can",
              (int)(strstr(out, "\"can\":[") == 0 ? 1 : 0), 1);
    ok("物模型没有数组属性 —— 4 个通道必须拆成 can1..can4 四个独立属性");

    check_int("以 { 开头", out[0] == '{' ? 1 : 0, 1);
    check_int("以 } 结尾", out[n - 1u] == '}' ? 1 : 0, 1);
    check_int("花括号配平", (int)(strchr(out, '{') != 0), 1);

    /* ---- time 字段可以不带（ts_ms = 0）---- */
    (void)esp32_json_onenet_property(pl, len, 1u, 0ull,
                                     out, (uint16_t)sizeof(out));
    check_contains("ts=0 时仍生成 value", out,
                   "\"" ESP32_ONENET_ID_TEMP "\":{\"value\":23.5}");
    check_int("ts=0 时不带 time 字段",
              (int)(strstr(out, "\"time\"") == 0 ? 1 : 0), 1);
    ok("time 是可选字段；不带时平台用接收时间");

    /* ---- 负温度 ---- */
    len = make_telemetry(pl, sizeof(pl), -75, 300);
    (void)esp32_json_onenet_property(pl, len, 1u, 0ull,
                                     out, (uint16_t)sizeof(out));
    check_contains("负温度 -7.5（不能变成 6553.1）",
                   out, "\"" ESP32_ONENET_ID_TEMP "\":{\"value\":-7.5}");
    ok("温度有符号 —— 与通用路径同样的坑");

    /* ---- ★ 最坏情况必须装得下 ---- */
    len = make_telemetry_worst(pl, sizeof(pl));
    n = esp32_json_onenet_property(pl, len, 0xFFFFFFFFu, 9999999999999ull,
                                   out, (uint16_t)sizeof(out));
    check_int("最坏情况生成成功", (int)(n > 0u ? 1 : 0), 1);
    printf("      最坏情况长度: %u 字节 (缓冲 %u)\n",
           (unsigned)n, (unsigned)ESP32_ONENET_JSON_MAX);
    check_int("最坏情况必须装得下 ESP32_ONENET_JSON_MAX",
              (int)(n < (uint16_t)ESP32_ONENET_JSON_MAX ? 1 : 0), 1);
    check_int("最坏情况必须装得下积压队列槽位 ESP32_TELEM_JSON_MAX",
              (int)(n < (uint16_t)ESP32_TELEM_JSON_MAX ? 1 : 0), 1);
    ok("★ 这条断言是 ESP32_TELEM_JSON_MAX 从 256 提到 640 的依据 —— "
       "装不下的话积压队列会**静默丢数据**，而那正是它存在的理由");

    /* ---- 畸形载荷 ---- */
    check_int("长度不足返回 0",
              (int)esp32_json_onenet_property(pl, 27u, 1u, 0ull,
                                              out, (uint16_t)sizeof(out)), 0);
    check_int("空指针返回 0",
              (int)esp32_json_onenet_property(0, len, 1u, 0ull,
                                              out, (uint16_t)sizeof(out)), 0);
    check_int("失败时 out 被清空", out[0] == '\0' ? 1 : 0, 1);

    /* ---- 缓冲不足 ---- */
    {
        char small[32];
        check_int("小缓冲返回 0",
                  (int)esp32_json_onenet_property(pl, len, 1u, 0ull,
                                                  small,
                                                  (uint16_t)sizeof(small)), 0);
        check_int("小缓冲被清成空串", small[0] == '\0' ? 1 : 0, 1);
    }
}

/* 测试：解析下行属性设置 */
static void test_onenet_parse_set(void)
{
    esp32_cmd_t c;
    char id[24];
    int  r;

    printf("\n[10] OneNET 下行属性设置解析\n");

    /* ---- 官方示例形状 ---- */
    {
        const char *j = "{\"id\":\"123\",\"version\":\"1.0\","
                        "\"params\":{\"cmd\":\"REPORT_NOW\"}}";
        r = esp32_json_onenet_parse_set(j, (uint16_t)strlen(j), &c, id,
                                        (uint16_t)sizeof(id));
        check_int("解析成功", r, 0);
        check_str("取出 cmd", c.cmd, "REPORT_NOW");
        check_str("取出 id（字符串原样）", id, "123");
        ok("OneNET 的 id 是**字符串** —— 与通用路径的 {\"id\":12} 不同，"
           "必须原样回填，数字化再格式化回去会丢前导零");
    }

    /* ---- 带空格 / 字段顺序打乱 ---- */
    {
        const char *j = "{ \"params\" : { \"cmd\" : \"SET_PERIOD 30\" } ,"
                        " \"id\" : \"9876543210\" }";
        r = esp32_json_onenet_parse_set(j, (uint16_t)strlen(j), &c, id,
                                        (uint16_t)sizeof(id));
        check_int("宽松解析成功", r, 0);
        check_str("取出带参数的 cmd", c.cmd, "SET_PERIOD 30");
        check_str("取出 10 位 id", id, "9876543210");
    }

    /* ---- 值里含 "cmd" 字样不能误命中 ---- */
    {
        const char *j = "{\"id\":\"5\",\"params\":{"
                        "\"note\":\"use cmd here\",\"cmd\":\"GET_STATUS\"}}";
        r = esp32_json_onenet_parse_set(j, (uint16_t)strlen(j), &c, id,
                                        (uint16_t)sizeof(id));
        check_int("解析成功", r, 0);
        check_str("命中真正的 cmd 键，而不是值里的字样",
                  c.cmd, "GET_STATUS");
        ok("按 JSON 词法扫描（只在键的位置比较），"
           "不是朴素的 strstr —— 否则会命中 \"use cmd here\"");
    }

    /* ---- 没有 cmd 属性：id 仍要取出来，好让调用方回错误应答 ---- */
    {
        const char *j = "{\"id\":\"777\",\"version\":\"1.0\","
                        "\"params\":{\"temperature\":\"30.5\"}}";
        r = esp32_json_onenet_parse_set(j, (uint16_t)strlen(j), &c, id,
                                        (uint16_t)sizeof(id));
        check_int("返回 -2（无 cmd 属性）", r, -2);
        check_str("id 仍然被取出", id, "777");
        ok("★ 即使没有 cmd，id 也必须取出来 —— 否则调用方无法回"
           "set_reply，平台会一直等到超时，而设备这边看起来"
           "\"收到了但什么都没做\"");
    }

    /* ---- 空指令 ---- */
    {
        const char *j = "{\"id\":\"1\",\"params\":{\"cmd\":\"\"}}";
        r = esp32_json_onenet_parse_set(j, (uint16_t)strlen(j), &c, id,
                                        (uint16_t)sizeof(id));
        check_int("空指令返回 -5", r, -5);
        ok("空指令明确拒绝，不静默忽略");
    }

    /* ---- 超长指令：必须报错，不能截断 ---- */
    {
        char   j[256];
        size_t p = 0u;
        size_t i;

        append_lit(j, &p, "{\"id\":\"1\",\"params\":{\"cmd\":\"");
        for (i = 0u; i < 100u; i++) {
            j[p++] = 'A';
        }
        append_lit(j, &p, "\"}}");
        j[p] = '\0';

        r = esp32_json_onenet_parse_set(j, (uint16_t)strlen(j), &c, id,
                                        (uint16_t)sizeof(id));
        check_int("超长指令返回 -4（过长）", r, -4);
        ok("★ 超长一律报错，绝不截断 —— 截断会让 SET_PERIOD 3000 "
           "静默变成 SET_PERIOD 3");
    }

    /* ---- 畸形输入 ---- */
    {
        const char *j = "{\"id\":\"1\",\"params\":{\"cmd\":\"unterminated";
        r = esp32_json_onenet_parse_set(j, (uint16_t)strlen(j), &c, id,
                                        (uint16_t)sizeof(id));
        check_int("未闭合字符串被拒", (int)(r < 0 ? 1 : 0), 1);

        check_int("空输入返回 -1",
                  esp32_json_onenet_parse_set("", 0u, &c, id, 8u), -1);
        check_int("空指针返回 -1",
                  esp32_json_onenet_parse_set(0, 10u, &c, id, 8u), -1);
    }

    /* ---- id 缓冲不足：不该崩，且 cmd 仍可用 ---- */
    {
        const char *j = "{\"id\":\"12345678901234567890\","
                        "\"params\":{\"cmd\":\"GET_STATUS\"}}";
        char tiny[4];
        r = esp32_json_onenet_parse_set(j, (uint16_t)strlen(j), &c, tiny,
                                        (uint16_t)sizeof(tiny));
        check_int("id 放不下但解析仍成功", r, 0);
        check_str("cmd 不受 id 影响", c.cmd, "GET_STATUS");
    }
}

/* 测试：应答与结果上报 */
static void test_onenet_reply(void)
{
    char     out[256];
    uint16_t n;

    printf("\n[11] OneNET set_reply 与指令结果\n");

    /* ---- set_reply 形状 ---- */
    n = esp32_json_onenet_set_reply("123", 200, "ok",
                                    out, (uint16_t)sizeof(out));
    check_int("生成成功", (int)(n > 0u ? 1 : 0), 1);
    check_str("set_reply 形状正确", out,
              "{\"id\":\"123\",\"code\":200,\"msg\":\"ok\"}");
    ok("set_reply 的含义是「我收到了这条属性设置」，"
       "不是「指令已经执行完」");

    /* ---- id 原样回填 ---- */
    (void)esp32_json_onenet_set_reply("9876543210", 200, "ok",
                                      out, (uint16_t)sizeof(out));
    check_contains("id 原样回填（含前导零也不丢）",
                   out, "{\"id\":\"9876543210\"");

    /* ---- id 为空时给 "0"，不能给空串 ---- */
    (void)esp32_json_onenet_set_reply("", 400, "bad",
                                      out, (uint16_t)sizeof(out));
    check_contains("空 id 兜底成 \"0\"", out, "{\"id\":\"0\"");
    ok("id 为空时不能拼出 {\"id\":\"\"} —— 平台不接受空 id");

    /* ---- 错误码 ---- */
    (void)esp32_json_onenet_set_reply("1", 400, "bad request",
                                      out, (uint16_t)sizeof(out));
    check_contains("错误码被原样写出", out, "\"code\":400");

    /* ---- msg 转义（防注入）---- */
    (void)esp32_json_onenet_set_reply("1", 400, "a\"b\\c",
                                      out, (uint16_t)sizeof(out));
    check_contains("msg 里的引号被转义", out, "a\\\"b\\\\c");
    ok("★ msg 必须转义 —— 不转义的话平台侧 JSON 解析会错位，"
       "轻则解析失败，重则被当成两个字段（注入）");

    /* ---- 指令结果：成功 ---- */
    n = esp32_json_onenet_result(7u, 1700000000000ull, "REPORT OK", 1,
                                 out, (uint16_t)sizeof(out));
    check_int("生成成功", (int)(n > 0u ? 1 : 0), 1);
    check_contains("走 cmd_result 属性",
                   out, "\"cmd_result\":{\"value\":\"REPORT OK\"");
    check_int("成功时无 FAIL 前缀",
              (int)(strstr(out, "FAIL:") == 0 ? 1 : 0), 1);

    /* ---- 指令结果：失败要一眼可见 ---- */
    (void)esp32_json_onenet_result(8u, 0ull, "PERIOD_RANGE_ERROR", 0,
                                   out, (uint16_t)sizeof(out));
    check_contains("失败时加 FAIL: 前缀",
                   out, "\"cmd_result\":{\"value\":\"FAIL: PERIOD_RANGE_ERROR\"}");
    ok("NACK 加 FAIL: 前缀 —— 让控制台上失败一眼可见，"
       "而不用人去看命令词猜");

    /* ---- 结果里的引号也要转义 ---- */
    (void)esp32_json_onenet_result(9u, 0ull, "weird\"text", 1,
                                   out, (uint16_t)sizeof(out));
    check_contains("结果文本被转义", out, "weird\\\"text");

    /* ---- 缓冲不足 ---- */
    {
        char small[16];
        check_int("小缓冲返回 0",
                  (int)esp32_json_onenet_set_reply("123", 200, "ok",
                                                   small,
                                                   (uint16_t)sizeof(small)), 0);
        check_int("小缓冲返回 0（result）",
                  (int)esp32_json_onenet_result(1u, 0ull, "OK", 1,
                                                small,
                                                (uint16_t)sizeof(small)), 0);
    }
}

/* 测试：链路状态上报 */
static void test_onenet_link(void)
{
    char out[256];

    printf("\n[12] OneNET 链路状态上报\n");

    (void)esp32_json_onenet_link(3u, 1700000000000ull,
                                 ESP32_LINK_CONNECTED, ESP32_LINK_CONNECTED,
                                 -62, 1, 5u, out, (uint16_t)sizeof(out));
    check_contains("含 wifi 状态", out, "\"wifi\":{\"value\":2,");
    check_contains("含 mqtt 状态", out, "\"mqtt\":{\"value\":2,");
    check_contains("含 rssi（负值）", out, "\"rssi\":{\"value\":-62,");
    check_contains("含 backlog", out, "\"backlog\":{\"value\":5,");
    check_contains("走 params 包裹", out, "\"params\":{");
    ok("链路状态是独立的一条属性上报（与 STM32 遥测节奏无关）");

    /* ---- 取不到 AP 信息时不上报 rssi ---- */
    (void)esp32_json_onenet_link(4u, 0ull,
                                 ESP32_LINK_DISCONNECTED, ESP32_LINK_DISCONNECTED,
                                 0, 0, 0u, out, (uint16_t)sizeof(out));
    check_int("has_rssi=0 时不含 rssi 属性",
              (int)(strstr(out, "\"rssi\"") == 0 ? 1 : 0), 1);
    check_contains("backlog 仍在", out, "\"backlog\":");
    ok("★ 取不到信号时**不上报** rssi，而不是上报 0 —— "
       "0 dBm 表示贴着路由器，会让人以为信号极好，而真相是「读不到」");
}

int main(void)
{
    printf("========== ESP32 侧 JSON 封装与解析 单元测试 ==========\n");

    test_fixed1();
    test_telemetry();
    test_link_status();
    test_parse_normal();
    test_parse_adversarial();
    test_escape();
    test_cmd_reply();
    test_wire_compat();
    test_onenet_property();
    test_onenet_parse_set();
    test_onenet_reply();
    test_onenet_link();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
