/**
 * @file    test_app_cmd.c
 * @brief   下行指令执行集成测试（通过应用层，验证副作用）
 *
 * ============================================================================
 *  这套测试关心的是**执行之后发生了什么**，不是"解析对不对"
 *  （解析已被 test_cmd.c 的 113 项覆盖）。
 *
 *  重点覆盖三类：
 *
 *   1) **危险指令被拒时，设备状态必须一点都没变**
 *      —— 这是最容易写错的地方：如果实现里"先执行再验令牌"，
 *         测试必须能发现。
 *
 *   2) **指令从帧进来、结果从帧出去**（端到端）
 *      —— 单测解析通过不代表接进帧分派之后也对。
 *
 *   3) **执行失败的回报**（Flash 写失败、移植层没提供回调）
 *      —— 假装成功比直接报错更危险。
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iinc -Idrivers -Iapp -o test/test_app_cmd.exe \
 *        src/modbus_crc.c src/modbus_rtu.c src/modbus_master.c src/gw_pipeline.c \
 *        src/gw_frame.c src/gw_uplink.c src/gw_can_core.c src/gw_timing.c \
 *        src/gw_sched.c src/gw_cmd.c src/gw_devinfo.c drivers/dht22.c \
 *        app/gw_app.c test/test_app_cmd.c
 *    ./test/test_app_cmd.exe
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

#include "gw_app.h"
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
 * 假环境
 * ================================================================== */
typedef struct {
    mb_regs_t regs;
    mb_slave_t slave;

    /* 假移植层动作计数 */
    int save_calls;
    int save_result;        /* 0 = 成功，<0 = 失败 */
    int restore_calls;
    int restore_result;
    int reboot_calls;

    /* 捕获发给 ESP32 的帧 */
    uint8_t  esp_out[GW_FRAME_MAX_TOTAL];
    uint16_t esp_out_len;
    int      esp_send_calls;
} env_t;

static env_t ENV;

static int p_take(void *c, uint8_t *b, uint16_t s, uint16_t *l)
{
    (void)c; (void)b; (void)s; (void)l;
    return -1;
}
static int p_send(void *c, const uint8_t *b, uint16_t l)
{
    (void)c; (void)b; (void)l;
    return 0;
}
static mb_regs_t *p_regs(void *c) { (void)c; return &ENV.regs; }

static int p_param_save(void *c)
{
    (void)c;
    ENV.save_calls++;
    return ENV.save_result;
}
static int p_param_restore(void *c)
{
    (void)c;
    ENV.restore_calls++;
    return ENV.restore_result;
}
static void p_reboot(void *c)
{
    (void)c;
    ENV.reboot_calls++;
    /* 不真的复位 —— 测试里只验证"被调用了" */
}

/* 上云移植层：捕获 ACK/NACK */
static int up_send(void *c, const uint8_t *buf, uint16_t len)
{
    (void)c;
    ENV.esp_send_calls++;
    if (len <= sizeof(ENV.esp_out)) {
        memcpy(ENV.esp_out, buf, len);
        ENV.esp_out_len = len;
    }
    return 0;
}

/* 假实现：数据由 up_take 直接提供，不需要收字节 */
static void up_tick(void *c)
{
    (void)c;
}

/* 待喂给应用层的一帧（模拟 ESP32 发来的业务帧） */
static uint8_t  FEED_TYPE;
static uint8_t  FEED_PAYLOAD[GW_FRAME_MAX_PAYLOAD];
static uint16_t FEED_LEN;
static uint8_t  FEED_READY;

static int up_take(void *c, uint8_t *out_type, uint8_t *payload,
                   uint16_t cap, uint16_t *out_len)
{
    (void)c;
    if (FEED_READY == 0u) {
        return -1;                  /* 没有待处理的帧 */
    }
    if (FEED_LEN > cap) {
        FEED_READY = 0u;
        return -1;
    }
    *out_type = FEED_TYPE;
    memcpy(payload, FEED_PAYLOAD, FEED_LEN);
    *out_len = FEED_LEN;
    FEED_READY = 0u;                /* 读走即消费 */
    return 0;
}

static gw_app_t APP;

static void setup(int with_param_callbacks)
{
    gw_port_t        port;
    gw_uplink_port_t up;

    memset(&ENV, 0, sizeof(ENV));
    memset(&APP, 0, sizeof(APP));
    FEED_READY = 0u;
    FEED_LEN   = 0u;

    mb_slave_init(&ENV.slave, 1u, &ENV.regs);

    memset(&port, 0, sizeof(port));
    port.rs485_take_frame = p_take;
    port.rs485_send       = p_send;
    port.get_regs         = p_regs;
    if (with_param_callbacks != 0) {
        port.param_save    = p_param_save;
        port.param_restore = p_param_restore;
        port.reboot        = p_reboot;
    }

    (void)gw_app_init(&APP, &ENV.slave, &port);

    /* ⚠️ 三个回调缺一个，gw_app_enable_uplink 就会把整个上云禁用
       （见它的实现：任何一个是 NULL 都直接返回 0 且 uplink_enabled = 0）。
       所以假实现也必须把三个都补上。 */
    memset(&up, 0, sizeof(up));
    up.esp_send       = up_send;
    up.esp_tick       = up_tick;
    up.esp_take_frame = up_take;
    (void)gw_app_enable_uplink(&APP, &up, 100000u);
}

/* 直接执行一条指令（走应用层接口） */
static gw_cmd_result_t exec(const char *s)
{
    return gw_app_exec_command(&APP, s, (uint16_t)strlen(s));
}

/**
 * 把一条 COMMAND 帧喂进应用层（模拟 ESP32 发来）
 *
 * ⚠️ 喂的是**类型 + 载荷**，不是整帧 ——
 *    帧的组帧/拆帧在移植层完成，应用层只拿到"类型 + 载荷"。
 *    这也是为什么测试必须提供 esp_take_frame 回调。
 */
static void feed_command_frame(const char *s)
{
    FEED_TYPE = (uint8_t)GW_FT_COMMAND;
    FEED_LEN  = (uint16_t)strlen(s);
    memcpy(FEED_PAYLOAD, s, FEED_LEN);
    FEED_READY = 1u;

    gw_app_poll(&APP, 0u);          /* 驱动应用层取帧并处理 */
}

/* 从捕获到的帧里取出载荷文本（帧结构：HEAD LEN2 TYPE PAYLOAD CRC） */
static void esp_payload_text(char *out, uint16_t cap)
{
    uint16_t plen = 0u;

    if (out == 0 || cap == 0u) {
        return;
    }
    out[0] = '\0';
    if (ENV.esp_out_len > 5u) {
        plen = (uint16_t)(ENV.esp_out_len - 5u);
        if (plen > (uint16_t)(cap - 1u)) {
            plen = (uint16_t)(cap - 1u);
        }
        memcpy(out, &ENV.esp_out[4], plen);
    }
    out[plen] = '\0';
}

/* ==================================================================
 * 测试 1：安全指令的副作用
 * ================================================================== */
static void test_safe_commands(void)
{
    printf("\n[1] 安全指令的副作用\n");

    /* ---- report：只置标志，不直接发 ---- */
    setup(1);
    check_int("初始没有上报请求", gw_app_take_report_now(&APP), 0);
    check_int("report 执行成功",
              (int)exec("report"), (int)GW_CMD_OK);
    check_int("report 置起了上报请求", gw_app_take_report_now(&APP), 1);
    check_int("读取后请求被消费（不会重复上报）",
              gw_app_take_report_now(&APP), 0);
    ok("report 只置标志 —— 什么时候发由上云策略决定，执行本身不阻塞");

    /* ---- period：写保持寄存器 ---- */
    setup(1);
    check_int("period 30 执行成功",
              (int)exec("period 30"), (int)GW_CMD_OK);
    check_int("HREG_REPORT_PERIOD 被写成 30",
              (int)ENV.regs.holding[HREG_REPORT_PERIOD], 30);

    /* ⚠️ 这一条覆盖一个真实踩过的坑：
       寄存器表**部分未初始化**（从站地址=0 等）时，
       改一个合法参数不该因为"别的字段非法"而被拒绝。 */
    check_int("寄存器表部分未初始化时改周期仍成功",
              (int)exec("period 45"), (int)GW_CMD_OK);
    check_int("周期被改成 45",
              (int)ENV.regs.holding[HREG_REPORT_PERIOD], 45);
    ok("按字段校验而不是整表体检 —— 否则上位机只写一个寄存器会被误拒");

    /* 越界必须被拒，且寄存器不变 */
    check_int("period 0 被拒",
              (int)exec("period 0"), (int)GW_CMD_ERR_RANGE);
    check_int("period 0 没有改动寄存器",
              (int)ENV.regs.holding[HREG_REPORT_PERIOD], 45);
    check_int("period 99999 被拒",
              (int)exec("period 99999"), (int)GW_CMD_ERR_RANGE);
    ok("参数越界时寄存器保持原值 —— 不留半改状态");

    /* ---- clear_sticky：清位 ---- */
    setup(1);
    APP.pipe.sticky_flags = 0x00FFu;
    check_int("clear_sticky 0x0F 执行成功",
              (int)exec("clear_sticky 0x0F"), (int)GW_CMD_OK);
    check_int("低 4 位被清掉", (int)APP.pipe.sticky_flags, 0x00F0);
    check_int("只清指定的位（高 4 位不受影响）",
              (int)((APP.pipe.sticky_flags & 0x00F0u) == 0x00F0u ? 1 : 0), 1);

    /* ---- uplink：开关上云 ---- */
    setup(1);
    check_int("初始上云是开的", (int)APP.uplink_enabled, 1);
    check_int("uplink off 执行成功",
              (int)exec("uplink off"), (int)GW_CMD_OK);
    check_int("上云被关闭", (int)APP.uplink_enabled, 0);
    check_int("uplink on 执行成功",
              (int)exec("uplink on"), (int)GW_CMD_OK);
    check_int("上云被打开", (int)APP.uplink_enabled, 1);

    /* ---- status：不改任何东西 ---- */
    setup(1);
    ENV.regs.holding[0] = 0x1234u;
    APP.pipe.sticky_flags = 0x0001u;
    check_int("status 执行成功",
              (int)exec("status"), (int)GW_CMD_OK);
    check_int("status 没改寄存器",
              (int)ENV.regs.holding[0], 0x1234);
    check_int("status 没改 sticky",
              (int)APP.pipe.sticky_flags, 1);
    ok("status 是只读操作 —— 不能有任何副作用");
}

/* ==================================================================
 * 测试 2：⚠️ 危险指令被拒时状态不能变（核心）
 * ================================================================== */
static void test_dangerous_rejected(void)
{
    printf("\n[2] ⚠️ 危险指令被拒时状态必须一点没变（核心）\n");

    /* ---- save 缺令牌 ---- */
    setup(1);
    check_int("save 不带令牌被拒",
              (int)exec("save"), (int)GW_CMD_ERR_TOKEN);
    check_int("**没有调用 param_save**（关键）", ENV.save_calls, 0);
    ok("缺令牌时绝不执行 —— 实现里令牌校验必须在执行之前");

    /* ---- save 令牌错误 ---- */
    check_int("save 令牌错误被拒",
              (int)exec("save 0xA5A5"), (int)GW_CMD_ERR_TOKEN);
    check_int("令牌错误时也没调用 param_save", ENV.save_calls, 0);

    /* ---- restore 令牌错误：这是最危险的一条 ---- */
    setup(1);
    check_int("restore 不带令牌被拒",
              (int)exec("restore"), (int)GW_CMD_ERR_TOKEN);
    check_int("restore 令牌错误被拒",
              (int)exec("restore 0x1234"), (int)GW_CMD_ERR_TOKEN);
    check_int("restore 令牌为 0 被拒",
              (int)exec("restore 0"), (int)GW_CMD_ERR_TOKEN);
    check_int("**restore 一次都没执行**（现场参数没被清）",
              ENV.restore_calls, 0);
    ok("restore 是最危险的指令（清掉现场标定参数），令牌保护必须生效");

    /* ---- reboot 令牌错误 ---- */
    setup(1);
    check_int("reboot 不带令牌被拒",
              (int)exec("reboot"), (int)GW_CMD_ERR_TOKEN);
    check_int("reboot 令牌错误被拒",
              (int)exec("reboot 0x5A5A"), (int)GW_CMD_ERR_TOKEN);
    check_int("**设备没有重启**", ENV.reboot_calls, 0);
    ok("令牌错误不会误重启 —— 误重启会中断现场正在进行的操作");

    /* ---- 正确令牌：应该执行 ---- */
    setup(1);
    check_int("save 带正确令牌成功",
              (int)exec("save 0x5A5A"), (int)GW_CMD_OK);
    check_int("param_save 被调用了一次", ENV.save_calls, 1);

    check_int("restore 带正确令牌成功",
              (int)exec("restore 0xA5A5"), (int)GW_CMD_OK);
    check_int("param_restore 被调用了一次", ENV.restore_calls, 1);

    check_int("reboot 带正确令牌成功",
              (int)exec("reboot 0x1234"), (int)GW_CMD_OK);
    check_int("reboot 回调被调用了一次", ENV.reboot_calls, 1);
    ok("正确令牌下三条危险指令都能执行 —— 保护没有把正常路径也挡住");
}

/* ==================================================================
 * 测试 3：执行失败的如实回报
 * ================================================================== */
static void test_failure_reporting(void)
{
    printf("\n[3] 执行失败必须如实回报（不能假装成功）\n");

    /* ---- Flash 写失败 ---- */
    setup(1);
    ENV.save_result = -1;
    check_int("Flash 写失败时返回 INTERNAL_ERROR",
              (int)exec("save 0x5A5A"), (int)GW_CMD_ERR_INTERNAL);
    check_int("但确实尝试了保存", ENV.save_calls, 1);
    ok("Flash 失败如实上报 —— 假装成功会让上位机以为参数已持久化");

    ENV.restore_result = -2;
    check_int("恢复出厂失败时返回 INTERNAL_ERROR",
              (int)exec("restore 0xA5A5"), (int)GW_CMD_ERR_INTERNAL);

    /* ---- 移植层没提供回调 ---- */
    setup(0);       /* 不注入 param_save / restore / reboot */
    check_int("移植层没提供保存 -> BAD_STATE（而不是假装成功）",
              (int)exec("save 0x5A5A"), (int)GW_CMD_ERR_STATE);
    check_int("移植层没提供恢复 -> BAD_STATE",
              (int)exec("restore 0xA5A5"), (int)GW_CMD_ERR_STATE);
    check_int("移植层没提供重启 -> BAD_STATE（比直接复位安全）",
              (int)exec("reboot 0x1234"), (int)GW_CMD_ERR_STATE);
    ok("能力缺失时明确回 BAD_STATE —— 上位机知道'本设备不支持'");

    /* ---- 未知指令 ---- */
    setup(1);
    check_int("未知指令返回 UNKNOWN_CMD",
              (int)exec("frobnicate"), (int)GW_CMD_ERR_UNKNOWN);

    /* ---- 空指令：不计数 ---- */
    {
        uint32_t before = APP.cmd_rx;

        check_int("空指令返回 OK",
                  (int)exec(""), (int)GW_CMD_OK);
        check_int("空指令不计入 cmd_rx",
                  (int)APP.cmd_rx, (int)before);
        ok("空消息静默忽略且不计数 —— 否则 MQTT 上的空消息会刷满统计");
    }

    /* ---- 统计量 ---- */
    setup(1);
    (void)exec("report");
    (void)exec("report");
    (void)exec("frobnicate");
    (void)exec("save");                 /* 缺令牌 */
    check_int("cmd_rx 计到 4 条", (int)APP.cmd_rx, 4);
    check_int("cmd_ok 为 2", (int)APP.cmd_ok, 2);
    check_int("cmd_err 为 2", (int)APP.cmd_err, 2);
    check_int("最后一条结果码是 BAD_TOKEN",
              (int)APP.cmd_last_result, (int)GW_CMD_ERR_TOKEN);
    ok("统计量可供寄存器上报（现场能看出'最近有没有指令失败'）");
}

/* ==================================================================
 * 测试 4：端到端（帧进 -> 执行 -> ACK 帧出）
 * ================================================================== */
static void test_end_to_end(void)
{
    char text[64];

    printf("\n[4] 端到端：指令帧进 -> 执行 -> ACK/NACK 帧出\n");

    /* ---- 成功：应回 ACK ---- */
    setup(1);
    feed_command_frame("report");
    check_int("收到指令帧后 cmd_rx = 1", (int)APP.cmd_rx, 1);
    check_int("向上位机回了一帧", ENV.esp_send_calls, 1);
    /* ⚠️ 指令确认走**专用**类型 GW_FT_CMD_ACK / GW_FT_CMD_NACK，
       不是 GW_FT_ACK / GW_FT_NACK。
       后者被上云链路占用（语义是"遥测送达/被拒"），且它只看类型
       不判载荷 —— 复用会让一次"指令令牌错误"被当成"遥测被拒"，
       进而误触发一次遥测重传。见 gw_frame.h 的说明。 */
    check_int("回的是 CMD_ACK 帧（不是遥测的 ACK）",
              (int)(ENV.esp_out_len > 4u
                    && ENV.esp_out[3] == GW_FT_CMD_ACK ? 1 : 0), 1);
    check_int("上报请求被置起（帧通路也生效）",
              gw_app_take_report_now(&APP), 1);
    ok("指令从帧进来能被正确执行 —— 单测解析通过不等于接进分派后也对");

    /* ---- 失败：应回 CMD_NACK ---- */
    setup(1);
    feed_command_frame("restore");
    check_int("缺令牌的指令也被受理（有统计）", (int)APP.cmd_rx, 1);
    check_int("回了 CMD_NACK 帧（不是遥测的 NACK）",
              (int)(ENV.esp_out_len > 4u
                    && ENV.esp_out[3] == GW_FT_CMD_NACK ? 1 : 0), 1);
    check_int("**没有执行 restore**", ENV.restore_calls, 0);
    ok("失败也回帧 —— 上位机能区分'执行了'与'丢了'");

    /* ---- ACK 载荷是给人看的文本 ---- */
    setup(1);
    feed_command_frame("period 45");
    esp_payload_text(text, sizeof(text));
    if (strstr(text, "PERIOD") != 0 && strstr(text, "OK") != 0) {
        ok("ACK 载荷是可读文本（现场不用查表解码）");
    } else {
        char d[96];
        sprintf(d, "(载荷 = \"%s\")", text);
        fail("ACK 载荷是可读文本", d);
    }
    if (strstr(text, "45") != 0) {
        ok("ACK 里回显了新的周期值");
    } else {
        fail("ACK 里回显了新的周期值", text);
    }

    /* ---- 状态请求：ACK 里带状态字 ---- */
    setup(1);
    APP.pipe.sticky_flags = 0x0003u;
    feed_command_frame("status");
    esp_payload_text(text, sizeof(text));
    if (strstr(text, "STATUS") != 0) {
        ok("状态请求的 ACK 里带回了指令名");
    } else {
        fail("状态请求的 ACK 里带回了指令名", text);
    }

    /* ---- 没有上云移植层时不能崩 ---- */
    {
        gw_port_t port;

        memset(&ENV, 0, sizeof(ENV));
        memset(&APP, 0, sizeof(APP));
        mb_slave_init(&ENV.slave, 1u, &ENV.regs);
        memset(&port, 0, sizeof(port));
        port.rs485_take_frame = p_take;
        port.rs485_send       = p_send;
        port.get_regs         = p_regs;
        (void)gw_app_init(&APP, &ENV.slave, &port);
        (void)gw_app_enable_uplink(&APP, 0, 1u);    /* 明确禁用上云 */

        check_int("没有上云时指令仍能执行",
                  (int)exec("report"), (int)GW_CMD_OK);
        check_int("没有上云时不尝试发帧", ENV.esp_send_calls, 0);
        ok("没有上云移植层时不会崩 —— 指令局部生效，只是回不了 ACK");
    }
}

/* ==================================================================
 * 测试 5：make_cmd_reply 的边界
 * ================================================================== */
static void test_reply_format(void)
{
    char     buf[64];
    uint16_t n;

    printf("\n[5] 应答载荷的生成\n");

    n = gw_app_make_cmd_reply((uint8_t)GW_CMD_REPORT_NOW,
                              (uint8_t)GW_CMD_OK, 0u, buf, sizeof(buf));
    if (n > 0u && strcmp(buf, "REPORT OK") == 0) {
        ok("无 detail 时不附加多余字符");
    } else {
        char d[96];
        sprintf(d, "(\"%s\")", buf);
        fail("无 detail 时不附加多余字符", d);
    }

    n = gw_app_make_cmd_reply((uint8_t)GW_CMD_SET_PERIOD,
                              (uint8_t)GW_CMD_OK, 45u, buf, sizeof(buf));
    if (strcmp(buf, "PERIOD OK 45") == 0) {
        ok("带 detail 时格式正确");
    } else {
        fail("带 detail 时格式正确", buf);
    }
    check_int("返回值 = 字符串长度", (int)n, (int)strlen(buf));

    /* 大数值 */
    (void)gw_app_make_cmd_reply((uint8_t)GW_CMD_GET_STATUS,
                                (uint8_t)GW_CMD_OK, 4294967295u,
                                buf, sizeof(buf));
    if (strstr(buf, "4294967295") != 0) {
        ok("32 位最大值能正确转成十进制");
    } else {
        fail("32 位最大值能正确转成十进制", buf);
    }

    /* 失败结果 */
    (void)gw_app_make_cmd_reply((uint8_t)GW_CMD_RESTORE,
                                (uint8_t)GW_CMD_ERR_TOKEN, 0u,
                                buf, sizeof(buf));
    if (strstr(buf, "BAD_TOKEN") != 0) {
        ok("失败结果码是自解释的字符串");
    } else {
        fail("失败结果码是自解释的字符串", buf);
    }

    /* 小缓冲截断安全 */
    {
        char small[12];

        n = gw_app_make_cmd_reply((uint8_t)GW_CMD_GET_STATUS,
                                  (uint8_t)GW_CMD_OK, 123456789u,
                                  small, sizeof(small));
        check_int("小缓冲不越界", (int)(n < sizeof(small) ? 1 : 0), 1);
        check_int("小缓冲仍以 '\\0' 结尾", (int)(small[n] == '\0' ? 1 : 0), 1);
        ok("截断时不会破坏内存（ACK 帧构造在栈上进行）");
    }

    check_int("空缓冲返回 0",
              (int)gw_app_make_cmd_reply(0u, 0u, 0u, 0, 32u), 0);
    check_int("容量过小返回 0",
              (int)gw_app_make_cmd_reply(0u, 0u, 0u, buf, 4u), 0);
}

int main(void)
{
    printf("========== 下行指令执行 集成测试 ==========\n");

    test_safe_commands();
    test_dangerous_rejected();
    test_failure_reporting();
    test_end_to_end();
    test_reply_format();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
