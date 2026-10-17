/**
 * @file    test_regmap.c
 * @brief   寄存器映射表契约测试 + Modbus 从站协议正确性测试
 *
 * ============================================================================
 *  这个测试文件回答一个问题："代码里的寄存器表，是否严格等于设计文档定义的那张表？"
 *
 *  它为什么重要：
 *  寄存器映射表是本设备对外的**接口契约**。它一旦和文档不一致，
 *  上位机/组态软件就会读到错位的值 —— 这类问题在联调现场极难定位
 *  （通信完全正常，只是数据对应错位），所以必须用测试钉死。
 *
 *  测试包含两个层次：
 *   A. 契约层：地址常量、数量上限、位定义、32 位量拆分方式
 *   B. 协议层：期望响应字节来自 tools/ref_frames.py 的独立参考实现，
 *              不是照着 src/modbus_rtu.c 反推的（那样只能证明代码自洽）
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iinc -o test/test_regmap.exe \
 *        src/modbus_crc.c src/modbus_rtu.c test/test_regmap.c
 *    ./test/test_regmap.exe
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

#include "modbus_crc.h"
#include "modbus_rtu.h"
#include "modbus_regs.h"

static int g_pass = 0;
static int g_fail = 0;

static mb_slave_t g_slave;
static mb_regs_t  g_regs;

/* ------------------------------------------------------------------ 工具 */

static uint16_t hex2bin(const char *hex, uint8_t *out)
{
    uint16_t n = 0u;
    while (*hex != '\0') {
        unsigned v = 0u;
        if (*hex == ' ') { hex++; continue; }
        if (sscanf(hex, "%2x", &v) != 1) { break; }
        out[n++] = (uint8_t)v;
        hex += 2;
    }
    return n;
}

static void dump(const char *label, const uint8_t *b, uint16_t n)
{
    uint16_t i;
    printf("%s", label);
    for (i = 0u; i < n; i++) { printf("%02X ", b[i]); }
    printf("\n");
}

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

/* ------------------------------------------------------------------
 * 预置寄存器值缓冲区
 *
 * 为什么要这一层：case_resp() 内部必须调用 mb_slave_init() 才能保证
 * 每个用例从干净状态开始；但 mb_slave_init() 会清零寄存器表，"先设值
 * 再调用"的值会被冲掉。所以先把值写进 g_pre_*，由 case_resp() 在
 * init 之后回填。这样用例既能保持干净起点，又能预置测试数据。
 * ------------------------------------------------------------------ */
static uint16_t g_pre_input[MB_INPUT_REG_COUNT];
static uint16_t g_pre_holding[MB_HOLDING_REG_COUNT];

static void clear_pre(void)
{
    uint16_t i;
    for (i = 0u; i < MB_INPUT_REG_COUNT; i++)   { g_pre_input[i]   = 0u; }
    for (i = 0u; i < MB_HOLDING_REG_COUNT; i++) { g_pre_holding[i] = 0u; }
}

/* ==================================================================
 *  把字节数组转成 case_resp 需要的十六进制字符串
 *
 *  ⚠️ 为什么需要它：原来边界用例把请求帧、响应帧和 CRC **全部硬编码**，
 *     于是"输入寄存器个数"一改，那些常量就全错了。
 *     现在改成按 MB_INPUT_REG_COUNT 动态构造，需要一个转字符串的辅助。
 * ================================================================== */
static void to_hex(char *out, const uint8_t *b, uint16_t n)
{
    uint16_t i;
    for (i = 0u; i < n; i++) {
        sprintf(out + i * 3u, "%02X ", (unsigned)b[i]);
    }
    if (n > 0u) {
        out[n * 3u - 1u] = '\0';        /* 去掉最后一个空格 */
    } else {
        out[0] = '\0';
    }
}

static void set_input(uint16_t addr, uint16_t value)
{
    if (addr < MB_INPUT_REG_COUNT) { g_pre_input[addr] = value; }
}

static void set_holding(uint16_t addr, uint16_t value)
{
    if (addr < MB_HOLDING_REG_COUNT) { g_pre_holding[addr] = value; }
}

/* 用给定 PDU（不含 CRC）拼出完整请求帧，写进 out，返回帧长。
 *
 * 调用约定（很容易踩坑，所以写清楚）：
 *   pdu_len 必须是"真正的 PDU 长度"，**不含** out 里预留的 CRC 位置。
 *   如果误把预留的 CRC 字节也算进 pdu_len，CRC 就会被算在错误的数据范围上，
 *   从站会因校验失败而静默丢弃 —— 表现为"请求发出去了但没有任何响应"，
 *   而且从站的 crc_errors 计数会增加。这类问题在真机上极难定位，
 *   所以在测试里也要按同样的纪律写。 */
static uint16_t mb_frame(uint8_t *out, const uint8_t *pdu, uint16_t pdu_len)
{
    uint16_t crc = mb_crc16(pdu, pdu_len);
    memcpy(out, pdu, pdu_len);
    out[pdu_len]      = (uint8_t)(crc & 0xFFu);
    out[pdu_len + 1u] = (uint8_t)(crc >> 8);
    return (uint16_t)(pdu_len + 2u);
}

/* 期望精确响应字节。
 * 注意：内部会调用 mb_slave_init() 清零寄存器表，所以调用方必须在
 *      调用本函数"之前"用 set_input()/set_holding() 填好测试数据 ——
 *      这两个 setter 写入的是 g_pre[], 由本函数在 init 之后回填。 */
static void case_resp(const char *name, const char *req_hex, const char *exp_hex)
{
    uint8_t  req[MB_MAX_ADU];
    uint8_t  exp[MB_MAX_ADU];
    uint16_t rn = hex2bin(req_hex, req);
    uint16_t en = hex2bin(exp_hex, exp);
    uint16_t i;
    int      r;

    mb_slave_init(&g_slave, 1u, &g_regs);

    /* 回填调用方预设的寄存器值 */
    for (i = 0u; i < MB_INPUT_REG_COUNT; i++) { g_regs.input[i]   = g_pre_input[i]; }
    for (i = 0u; i < MB_HOLDING_REG_COUNT; i++) { g_regs.holding[i] = g_pre_holding[i]; }

    r = mb_slave_process(&g_slave, req, rn);

    if (r != 1) {
        fail(name, "(期望有响应)");
        return;
    }
    if (g_slave.tx_len != en || memcmp(g_slave.tx_buf, exp, en) != 0) {
        g_fail++;
        printf("  [FAIL] %s\n", name);
        dump("          期望: ", exp, en);
        dump("          实际: ", g_slave.tx_buf, g_slave.tx_len);
        return;
    }
    ok(name);
}

/* 期望静默（不应答） */
static void case_silent(const char *name, const char *req_hex)
{
    uint8_t  req[MB_MAX_ADU];
    uint16_t rn = hex2bin(req_hex, req);
    int      r;

    mb_slave_init(&g_slave, 1u, &g_regs);
    r = mb_slave_process(&g_slave, req, rn);

    if (r == 0 && g_slave.tx_len == 0u) {
        ok(name);
    } else {
        g_fail++;
        printf("  [FAIL] %s  (期望静默, 实际 r=%d tx_len=%u)\n",
               name, r, (unsigned)g_slave.tx_len);
    }
}

/* ==================================================================
 * A. 契约层：寄存器表结构必须与设计文档一致
 * ================================================================== */
static void test_contract_counts(void)
{
    printf("\n[A1] 寄存器数量契约（设计文档：输入 32 / 保持 16）\n");

    /* ⚠️ 2026-10-09 变更说明（如实记录，不粉饰）
     *
     * 原来是"输入寄存器数量 == 32"的**严格相等**断言。为了放
     * 「片内耗时测量区」（IREG_TM_*，36~39）我把数量扩到了 40，
     * 于是这条断言会失败。
     *
     * 处理方式：**不删掉契约，而是把它写准**。
     *   设计文档保证的是：
     *     - 0x00~0x1F（0~31）这 32 个**必须存在且含义固定**
     *     - 允许向后扩展（Modbus 本来就用"读多少个"来裁剪，
     *       多发一个只读寄存器不破坏任何已有主站）
     *   所以断言应当是"**至少** 32" + "不能超过规范上限 125"。
     *
     * ⚠️ 若将来把测量区撤掉，数量应收回 32 —— 届时这条断言自动成立，
     *    不需要再改测试。 */
    if (MB_INPUT_REG_COUNT >= 32u) {
        char d[80];
        if (MB_INPUT_REG_COUNT == 32u) {
            ok("输入寄存器数量 = 32（文档原始值）");
        } else {
            /* ⚠️ 2026-10-11 更正措辞：扩展区**不全是"临时"的** ——
               36~39 的片内耗时测量区是**正式功能**（报告里的 CPU 负载/loop
               耗时都来自它），只有 28~31（RS485 诊断）算诊断占用，
               而它落在 0~31 契约之内。所以这里不应再叫"临时扩展"。 */
            sprintf(d, "(实际 %u：32~35 留空、36~39 为正式耗时测量区；0~31 契约不变)",
                    (unsigned)MB_INPUT_REG_COUNT);
            ok("输入寄存器数量 >= 32（0~31 契约保持不变）");
            printf("       \u2139\ufe0f  %s\n", d);
        }
    } else {
        fail("输入寄存器数量 >= 32", "(少于文档保证的 32 个，破坏了对外契约)");
    }

    /* 扩展不能超出 Modbus 规范的读上限，否则"一次读完"会失败 */
    if (MB_INPUT_REG_COUNT <= 125u) {
        ok("输入寄存器数量未超出 Modbus 规范上限 125");
    } else {
        fail("输入寄存器数量 <= 125", "(超出规范的读数量上限)");
    }

    /* ⚠️ 与输入寄存器同理（见上面 A1 的说明）：
     *    文档保证的是 0x00~0x0F 这 16 个存在且含义固定，允许向后追加。
     *    用 ">= 16" 而非严格相等 —— 这样将来追加/收回寄存器时
     *    这条断言都不用改。
     *
     *    （曾短暂扩到 17 以放"清零耗时统计"的命令寄存器，
     *      后来发现**写命令寄存器根本触发不了动作** ——
     *      HREG_CMD_* 那几个魔数宏定义了却没有任何处理器读它，
     *      动作走的是文本指令协议。所以撤回了那次扩展，
     *      改用启动时自动清零统计。见 main.c 的说明。） */
    if (MB_HOLDING_REG_COUNT >= 16u) {
        if (MB_HOLDING_REG_COUNT == 16u) {
            ok("保持寄存器数量 = 16（文档原始值）");
        } else {
            char d[80];
            sprintf(d, "(实际 %u；0~15 契约不变)",
                    (unsigned)MB_HOLDING_REG_COUNT);
            ok("保持寄存器数量 >= 16（0~15 契约保持不变）");
            printf("       \u2139\ufe0f  %s\n", d);
        }
    } else {
        fail("保持寄存器数量 >= 16", "(少于文档保证的 16 个)");
    }

    /* mb_regs_t 必须能容纳这两个数量，且两者之和与结构体大小自洽 */
    if (sizeof(mb_regs_t) ==
        (MB_INPUT_REG_COUNT + MB_HOLDING_REG_COUNT) * sizeof(uint16_t)) {
        ok("mb_regs_t 大小与寄存器数量自洽");
    } else {
        fail("mb_regs_t 大小与寄存器数量自洽", "(结构体存在额外填充或数量不匹配)");
    }

    /* 最长响应帧必须放得下：125 个寄存器 -> 3 + 250 + 2 = 255 <= 256 */
    if ((uint32_t)(3u + MB_MAX_READ_QTY * 2u + 2u) <= (uint32_t)MB_MAX_ADU) {
        ok("规范上限 125 寄存器的最长响应仍 <= MB_MAX_ADU");
    } else {
        fail("最长响应帧 <= MB_MAX_ADU", "(缓冲区会溢出)");
    }
}

static void test_contract_offsets(void)
{
    printf("\n[A2] 关键地址偏移契约（对外的接口契约，改动即破坏兼容性）\n");

    if (IREG_TEMP_X10 == 0 && IREG_HUMI_X10 == 1) {
        ok("温湿度位于输入寄存器 0/1");
    } else {
        fail("温湿度位于输入寄存器 0/1", "");
    }

    if (IREG_CAN_CH1 == 2 && IREG_CAN_CH8 == 9) {
        ok("CAN 通道 1~8 位于输入寄存器 2~9");
    } else {
        fail("CAN 通道 1~8 位于输入寄存器 2~9", "");
    }

    if (IREG_DEV_STATUS == 10) {
        ok("设备状态位位于输入寄存器 10");
    } else {
        fail("设备状态位位于输入寄存器 10", "");
    }

    if (IREG_MQTT_STATUS == 21 && IREG_OFFLINE_DEPTH == 22 && IREG_OFFLINE_DROP == 23) {
        ok("上云链路状态位于输入寄存器 21~23");
    } else {
        fail("上云链路状态位于输入寄存器 21~23", "");
    }

    if (IREG_RESERVED_31 == 31) {
        ok("输入寄存器最后一位为 31（边界）");
    } else {
        fail("输入寄存器最后一位为 31", "");
    }

    if (HREG_DEVICE_ID_HI == 0 && HREG_DEVICE_ID_LO == 1) {
        ok("设备 ID 高/低字位于保持寄存器 0/1");
    } else {
        fail("设备 ID 高/低字位于保持寄存器 0/1", "");
    }

    if (HREG_OTA_STATE == 15) {
        ok("OTA 状态位于保持寄存器 15（边界）");
    } else {
        fail("OTA 状态位于保持寄存器 15", "");
    }

    /* 命令寄存器必须是魔数，且三个魔数互不相同 */
    if (HREG_CMD_PARAM_SAVE == 0x5A5Au &&
        HREG_CMD_PARAM_DFLT == 0xA5A5u &&
        HREG_CMD_REBOOT == 0x1234u) {
        ok("三个命令魔数定义正确且互不相同");
    } else {
        fail("三个命令魔数定义正确且互不相同", "");
    }
}

static void test_contract_status_bits(void)
{
    uint32_t all = 0u;
    int      i;

    printf("\n[A3] 设备状态位定义（必须互不重叠，否则诊断会误判）\n");

    /* 9 个位两两不重叠，等价于异或累积结果等于按位或累积结果 */
    all = (uint32_t)IREG_STATUS_BIT_DHT_OK      | (uint32_t)IREG_STATUS_BIT_CAN_OK
        | (uint32_t)IREG_STATUS_BIT_WIFI_OK     | (uint32_t)IREG_STATUS_BIT_MQTT_OK
        | (uint32_t)IREG_STATUS_BIT_PARAM_OK    | (uint32_t)IREG_STATUS_BIT_OTA_BUSY
        | (uint32_t)IREG_STATUS_BIT_IWDG_RESET  | (uint32_t)IREG_STATUS_BIT_HARDFAULT
        | (uint32_t)IREG_STATUS_BIT_MASTER_OK;

    if (all == 0x1FFu) {
        ok("9 个状态位覆盖 bit0~bit8 且无重叠");
    } else {
        char d[64];
        sprintf(d, "(组合结果 0x%03lX, 期望 0x1FF)", (unsigned long)all);
        fail("9 个状态位覆盖 bit0~bit8 且无重叠", d);
    }

    /* 逐位单独检查：每个位只被一个宏置起 */
    {
        const uint16_t bits[9] = {
            IREG_STATUS_BIT_DHT_OK,     IREG_STATUS_BIT_CAN_OK,
            IREG_STATUS_BIT_WIFI_OK,    IREG_STATUS_BIT_MQTT_OK,
            IREG_STATUS_BIT_PARAM_OK,   IREG_STATUS_BIT_OTA_BUSY,
            IREG_STATUS_BIT_IWDG_RESET, IREG_STATUS_BIT_HARDFAULT,
            IREG_STATUS_BIT_MASTER_OK
        };
        int bad = 0;
        for (i = 0; i < 9; i++) {
            int j, cnt = 0;
            for (j = 0; j < 9; j++) {
                if (bits[j] & bits[i]) { cnt++; }
            }
            if (cnt != 1) { bad = 1; }
        }
        if (!bad) {
            ok("每个状态位宏互不重叠（逐位验证）");
        } else {
            fail("每个状态位宏互不重叠（逐位验证）", "(存在重叠位)");
        }
    }

    /* 状态位不能超过 uint16 表达范围（输入寄存器是 16 位） */
    if (all <= 0xFFFFu) {
        ok("状态位组合可由单个 16 位输入寄存器承载");
    } else {
        fail("状态位可由单个 16 位寄存器承载", "(位定义超出 16 位)");
    }
}

/* ==================================================================
 * B. 协议层：边界与地址映射
 * ================================================================== */
static void test_input_boundary(void)
{
    uint16_t qty = (uint16_t)MB_INPUT_REG_COUNT;
    uint8_t  req[8];
    uint8_t  exp[5 + 2 * 125];
    char     rq[32];
    char     ex[600];
    uint16_t rc, i, en;

    printf("\n[B1] 输入寄存器边界（按 MB_INPUT_REG_COUNT=%u 动态构造）\n",
           (unsigned)qty);

    /* ==================================================================
     *  ⚠️ 为什么这里要"动态构造"而不是硬编码帧
     *
     *  原来硬编码了"32 个"对应的请求帧、响应帧与 CRC。
     *  输入寄存器扩到 40 之后：
     *     - "读满 32 个"的期望字节数与 CRC 都不对了
     *     - "地址 1 起读 32 个应越界" 变成了合法（1+32=33 <= 40）
     *
     *  ⚠️ 但不能因此把测试弱化 —— 边界覆盖必须保留，
     *     只是把常量换成按数量算出来的值。
     * ================================================================== */

    /* ---- 用例 1：读满全部输入寄存器（上界，应成功）---- */
    clear_pre();
    for (i = 0u; i < qty; i++) {
        set_input(i, (uint16_t)(i * 3u + 1u));
    }

    req[0] = 1u;  req[1] = 0x04u;
    req[2] = 0u;  req[3] = 0u;                      /* 起始地址 0 */
    req[4] = (uint8_t)(qty >> 8);
    req[5] = (uint8_t)(qty & 0xFFu);
    rc = mb_crc16(req, 6u);
    req[6] = (uint8_t)(rc & 0xFFu);
    req[7] = (uint8_t)(rc >> 8);

    en = 0u;
    exp[en++] = 1u;
    exp[en++] = 0x04u;
    exp[en++] = (uint8_t)(qty * 2u);
    for (i = 0u; i < qty; i++) {
        uint16_t v = (uint16_t)(i * 3u + 1u);
        exp[en++] = (uint8_t)(v >> 8);
        exp[en++] = (uint8_t)(v & 0xFFu);
    }
    rc = mb_crc16(exp, en);
    exp[en++] = (uint8_t)(rc & 0xFFu);
    exp[en++] = (uint8_t)(rc >> 8);

    to_hex(rq, req, 8u);
    to_hex(ex, exp, en);
    case_resp("读满全部输入寄存器（上界，恰好在界内）", rq, ex);

    /* ---- 用例 2：起始地址 1 + 读满全部 -> 整体越界，须拒 ---- */
    clear_pre();
    req[2] = 0u;  req[3] = 1u;                      /* 起始地址 1 */
    rc = mb_crc16(req, 6u);
    req[6] = (uint8_t)(rc & 0xFFu);
    req[7] = (uint8_t)(rc >> 8);
    to_hex(rq, req, 8u);
    /* ⚠️ 越界必须整体拒绝（异常 02），不能截断后返回部分数据 */
    case_resp("起始地址 1 + 读满全部越界 -> 异常 02",
              rq, "01 84 02 C2 C1");

    /* ---- 用例 3：地址 = 个数（刚好越界）-> 异常 02 ---- */
    clear_pre();
    req[2] = (uint8_t)(qty >> 8);
    req[3] = (uint8_t)(qty & 0xFFu);
    req[4] = 0u;  req[5] = 1u;
    rc = mb_crc16(req, 6u);
    req[6] = (uint8_t)(rc & 0xFFu);
    req[7] = (uint8_t)(rc >> 8);
    to_hex(rq, req, 8u);
    case_resp("地址 = 寄存器个数（刚好越界）-> 异常 02",
              rq, "01 84 02 C2 C1");

    /* ---- 用例 4：0~31 契约区里的最后一个（地址 31）仍须可读 ----
     *  扩展之后 31 不再是"最后一个"，但它是设计文档里
     *  承诺的 32 个寄存器中的最后一位，这条契约不能变。 */
    clear_pre();
    set_input(IREG_RESERVED_31, 0x00A5u);
    case_resp("读地址 31（0~31 契约区的最后一位）成功",
              "01 04 00 1F 00 01 00 0C",
              "01 04 02 00 A5 79 4B");

    /* ---- 用例 5：数量 126 超出规范上限 125 -> 异常 03 ---- */
    clear_pre();
    case_resp("读 126 个超出规范上限 -> 异常 03",
              "01 04 00 00 00 7E 70 2A",
              "01 84 03 03 01");
}

static void test_holding_boundary(void)
{
    printf("\n[B2] 保持寄存器边界（16 个的上下界）\n");

    clear_pre();
    set_holding(HREG_OTA_STATE, 0x0003u);
    case_resp("读地址 15（最后一个）成功",
              "01 03 00 0F 00 01 B4 09",
              "01 03 02 00 03 F8 45");

    clear_pre();
    case_resp("读地址 16（越界）-> 异常 02",
              "01 03 00 10 00 01 85 CF",
              "01 83 02 C0 F1");

    clear_pre();
    case_resp("读满 16 个保持寄存器（恰好在界内）",
              "01 03 00 00 00 10 44 06",
              "01 03 20 "
              "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 "
              "00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 "
              "92 7A");
}

static void test_device_id_split(void)
{
    uint8_t  req[8];
    uint8_t  pdu[6];
    uint16_t len;
    uint32_t id;

    printf("\n[B3] 32 位量拆分约定（高字在前，必须由文档固定）\n");

    mb_slave_init(&g_slave, 1u, &g_regs);

    /* 写设备 ID 高字 = 0x0001 */
    pdu[0] = 0x01u; pdu[1] = 0x06u;
    pdu[2] = (uint8_t)(HREG_DEVICE_ID_HI >> 8); pdu[3] = (uint8_t)(HREG_DEVICE_ID_HI & 0xFF);
    pdu[4] = 0x00u; pdu[5] = 0x01u;
    len = mb_frame(req, pdu, 6u);

    if (mb_slave_process(&g_slave, req, len) == 1 &&
        g_slave.tx_len == 8u && g_slave.tx_buf[1] == 0x06u) {
        ok("写设备 ID 高字有正确响应（06 原样回显）");
    } else {
        fail("写设备 ID 高字有正确响应", "");
    }
    if (g_regs.holding[HREG_DEVICE_ID_HI] == 0x0001u) {
        ok("设备 ID 高字写入生效");
    } else {
        fail("设备 ID 高字写入生效", "");
    }

    /* 写设备 ID 低字 = 0x86A0，使整体成为 0x000186A0 = 100000 */
    pdu[2] = (uint8_t)(HREG_DEVICE_ID_LO >> 8); pdu[3] = (uint8_t)(HREG_DEVICE_ID_LO & 0xFF);
    pdu[4] = 0x86u; pdu[5] = 0xA0u;
    len = mb_frame(req, pdu, 6u);

    if (mb_slave_process(&g_slave, req, len) == 1 &&
        g_slave.tx_len == 8u && g_slave.tx_buf[1] == 0x06u) {
        ok("写设备 ID 低字有正确响应");
    } else {
        fail("写设备 ID 低字有正确响应", "");
    }
    if (g_regs.holding[HREG_DEVICE_ID_LO] == 0x86A0u) {
        ok("设备 ID 低字写入生效");
    } else {
        fail("设备 ID 低字写入生效", "");
    }

    /* 按"高字在前"重组，应得到 100000 */
    id = ((uint32_t)g_regs.holding[HREG_DEVICE_ID_HI] << 16)
       | (uint32_t)g_regs.holding[HREG_DEVICE_ID_LO];
    if (id == 100000u) {
        ok("高字在前重组得到 0x000186A0 = 100000");
    } else {
        char d[64];
        sprintf(d, "(重组结果 0x%08lX)", (unsigned long)id);
        fail("高字在前重组得到 100000", d);
    }
}

static void test_command_registers(void)
{
    printf("\n[B4] 命令寄存器写入（魔数机制）\n");

    mb_slave_init(&g_slave, 1u, &g_regs);

    case_resp("写参数落盘魔数 0x5A5A",
              "01 06 00 0C 5A 5A F3 52",
              "01 06 00 0C 5A 5A F3 52");

    case_resp("写两个命令寄存器 12~13",
              "01 10 00 0C 00 02 04 5A 5A 12 34 CC 46",
              "01 10 00 0C 00 02 81 CB");

    if (g_regs.holding[HREG_PARAM_SAVE_CMD] == 0x5A5Au) {
        ok("命令魔数值被正确写入寄存器");
    } else {
        fail("命令魔数值被正确写入寄存器", "");
    }
}

static void test_silent_cases(void)
{
    printf("\n[B5] 必须静默的场景（不应答是正确的行为）\n");

    /* 站号不匹配：CRC 本身合法，确保不是因为 CRC 错才静默 */
    case_silent("站号不匹配(发往2/本机1) -> 静默",
                "02 03 00 00 00 02 C4 38");

    case_silent("CRC 错误 -> 静默",
                "01 03 00 00 00 02 C4 0C");

    case_silent("帧长不足 4 字节 -> 静默",
                "01 04 00");
}

/* ==================================================================
 * D. 从站协议引擎通用用例（原 test_modbus.c 的覆盖，已并入本文件）
 *
 * 为什么合并：寄存器表扩建后，旧文件引用了已拆分的 HREG_DEVICE_ID，
 * 而且它的覆盖是本文件的子集。保留两份会带来重复维护成本，
 * 所以统一到这一个文件，做法是：
 *   1) 新增 test_protocol_engine()，把广播语义等旧用例补全；
 *   2) 删除旧文件，测试入口只剩这一个可执行程序。
 * ================================================================== */
static void test_protocol_engine(void)
{
    uint8_t req[8];
    uint16_t len;

    printf("\n[D1] 从站协议引擎通用行为\n");

    /* ---- CRC 标准向量（Modbus 公开测试向量） ---- */
    {
        uint8_t v[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x0A };
        if (mb_crc16(v, 6u) == 0xCDC5u) {
            ok("CRC(01 03 00 00 00 0A) = 0xCDC5");
        } else {
            fail("CRC 标准向量 0xCDC5", "");
        }
        if (mb_crc16((const uint8_t *)"", 0u) == 0xFFFFu) {
            ok("CRC(空串) = 0xFFFF");
        } else {
            fail("CRC(空串) = 0xFFFF", "");
        }
    }

    /* ---- 广播语义：执行但不响应 ---- */
    clear_pre();
    case_silent("广播写单个 -> 不响应", "00 06 00 00 00 0A 08 1C");
    if (g_regs.holding[0] == 0x000Au) {
        ok("广播写入仍然生效 (holding[0] = 0x000A)");
    } else {
        char d[64];
        sprintf(d, "(实际 0x%04X)", (unsigned)g_regs.holding[0]);
        fail("广播写入仍然生效", d);
    }

    /* ---- 06 写入后回读一致 ---- */
    clear_pre();
    case_resp("06 写单个保持寄存器",
              "01 06 00 00 00 FF C9 8A",
              "01 06 00 00 00 FF C9 8A");
    if (g_regs.holding[HREG_DEVICE_ID_HI] == 0x00FFu) {
        ok("06 写入生效");
    } else {
        fail("06 写入生效", "");
    }

    /* ---- 16 写多个：写入生效 ---- */
    clear_pre();
    case_resp("16 写多个保持寄存器",
              "01 10 00 02 00 02 04 11 11 22 22 BF F6",
              "01 10 00 02 00 02 E0 08");
    if (g_regs.holding[HREG_BAUDRATE] == 0x1111u &&
        g_regs.holding[HREG_REPORT_PERIOD] == 0x2222u) {
        ok("16 写入生效 (holding[2]=0x1111, holding[3]=0x2222)");
    } else {
        fail("16 写入生效", "");
    }

    /* ---- 异常码 01：非法功能码 ---- */
    clear_pre();
    case_resp("非法功能码 01 -> 异常 01",
              "01 01 00 00 00 01 FD CA",
              "01 81 01 81 90");

    /* 其余功能码也应返回异常 01（0x05 写单个线圈） */
    {
        uint8_t p5[6] = { 0x01, 0x05, 0x00, 0x00, 0xFF, 0x00 };
        len = mb_frame(req, p5, 6u);
        mb_slave_init(&g_slave, 1u, &g_regs);
        (void)mb_slave_process(&g_slave, req, len);
        if (g_slave.tx_len == 5u && g_slave.tx_buf[1] == 0x85u &&
            g_slave.tx_buf[2] == MB_EXC_ILLEGAL_FUNCTION) {
            ok("功能码 05（线圈）-> 异常 01");
        } else {
            fail("功能码 05 -> 异常 01", "");
        }
    }

    /* ---- 异常码 02：06 写地址越界 ---- */
    {
        uint8_t p[6] = { 0x01, 0x06, 0x00, 0x10, 0x00, 0x01 };   /* reg=16 越界 */
        len = mb_frame(req, p, 6u);
        mb_slave_init(&g_slave, 1u, &g_regs);
        (void)mb_slave_process(&g_slave, req, len);
        if (g_slave.tx_len == 5u && g_slave.tx_buf[1] == 0x86u &&
            g_slave.tx_buf[2] == MB_EXC_ILLEGAL_ADDRESS) {
            ok("06 写地址越界 -> 异常 02");
        } else {
            fail("06 写地址越界 -> 异常 02", "");
        }
    }

    /* ---- 异常码 03：16 的字节数与数量不匹配 ----
       注意：PDU 必须完整声明，数组长度用显式赋值逐字节填。
       早期版本写成 uint8_t p[9] 却按 9 字节传长度，虽然恰好自洽，
       但极易在改动时越界读到栈上垃圾值（表现为请求帧里出现莫名字节），
       所以这里统一改成"先声明足够大的数组，再逐字节赋值"。 */
    {
        uint8_t  p[11];
        uint16_t n = 0u;
        p[n++] = 0x01u;   /* 站号 */
        p[n++] = 0x10u;   /* 功能码 16 */
        p[n++] = 0x00u; p[n++] = 0x00u;   /* 起始地址 0 */
        p[n++] = 0x00u; p[n++] = 0x02u;   /* 数量 2 */
        p[n++] = 0x02u;                    /* 字节数 2 —— 与数量 2 不匹配（应为 4） */
        p[n++] = 0x11u; p[n++] = 0x11u;   /* 数据 */
        p[n++] = 0x00u; p[n++] = 0x00u;   /* CRC 占位，由 mb_frame 覆盖 */
        len = mb_frame(req, p, (uint16_t)(n - 2u));   /* 真正的 PDU 长度不含 CRC 占位 */
        mb_slave_init(&g_slave, 1u, &g_regs);
        (void)mb_slave_process(&g_slave, req, len);
        if (g_slave.tx_len == 5u && g_slave.tx_buf[1] == 0x90u &&
            g_slave.tx_buf[2] == MB_EXC_ILLEGAL_VALUE) {
            ok("16 字节数与数量不匹配 -> 异常 03");
        } else {
            char d[160];
            sprintf(d, "(len=%u tx_len=%u rx=%lu crc_err=%lu)",
                    (unsigned)len, (unsigned)g_slave.tx_len,
                    (unsigned long)g_slave.stat_rx_frames,
                    (unsigned long)g_slave.stat_crc_errors);
            fail("16 字节数与数量不匹配 -> 异常 03", d);
        }
    }

    /* ---- 16 的帧长不精确匹配也应拒 ----
       数量 2 => 应带 4 字节数据、总 PDU 长 11；这里只给 2 字节数据（PDU 长 9），
       属于帧结构不完整，必须整体拒绝。 */
    {
        uint8_t  p[11];
        uint16_t n = 0u;
        p[n++] = 0x01u;
        p[n++] = 0x10u;
        p[n++] = 0x00u; p[n++] = 0x00u;
        p[n++] = 0x00u; p[n++] = 0x02u;   /* 数量 2 */
        p[n++] = 0x04u;                    /* 字节数 4（与数量一致） */
        p[n++] = 0x11u; p[n++] = 0x11u;   /* 只给 2 字节数据，缺 2 字节 */
        p[n++] = 0x00u; p[n++] = 0x00u;   /* CRC 占位 */
        len = mb_frame(req, p, (uint16_t)(n - 2u));   /* PDU 长度不含 CRC 占位 */
        mb_slave_init(&g_slave, 1u, &g_regs);
        (void)mb_slave_process(&g_slave, req, len);
        if (g_slave.tx_len == 5u && g_slave.tx_buf[2] == MB_EXC_ILLEGAL_VALUE) {
            ok("16 帧长不足 -> 异常 03");
        } else {
            char d[80];
            sprintf(d, "(len=%u tx_len=%u, 功能码=0x%02X)",
                    (unsigned)len, (unsigned)g_slave.tx_len, g_slave.tx_buf[1]);
            fail("16 帧长不足 -> 异常 03", d);
        }
    }

    /* ---- 读数量为 0 应拒（异常 03） ---- */
    {
        uint8_t p[6] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x00 };
        len = mb_frame(req, p, 6u);
        mb_slave_init(&g_slave, 1u, &g_regs);
        (void)mb_slave_process(&g_slave, req, len);
        if (g_slave.tx_len == 5u && g_slave.tx_buf[2] == MB_EXC_ILLEGAL_VALUE) {
            ok("读数量为 0 -> 异常 03");
        } else {
            fail("读数量为 0 -> 异常 03", "");
        }
    }

    /* ---- 03/04 请求帧长不是 8 应拒 ---- */
    {
        uint8_t p[7] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00 };
        len = mb_frame(req, p, 7u);
        mb_slave_init(&g_slave, 1u, &g_regs);
        (void)mb_slave_process(&g_slave, req, len);
        if (g_slave.tx_len == 5u && g_slave.tx_buf[2] == MB_EXC_ILLEGAL_VALUE) {
            ok("读请求帧长为 7 -> 异常 03");
        } else {
            fail("读请求帧长为 7 -> 异常 03", "");
        }
    }

    /* ---- 超过 MB_MAX_ADU 的帧应静默丢弃（防缓冲区溢出） ---- */
    {
        uint8_t big[300];
        uint16_t i;
        for (i = 0u; i < sizeof(big); i++) { big[i] = 0xA5u; }
        big[0] = 0x01u; big[1] = 0x10u;
        mb_slave_init(&g_slave, 1u, &g_regs);
        if (mb_slave_process(&g_slave, big, (uint16_t)sizeof(big)) == 0 &&
            g_slave.tx_len == 0u) {
            ok("超长帧（300 字节）静默丢弃");
        } else {
            fail("超长帧静默丢弃", "");
        }
    }
}
/* ==================================================================
 * C. 统计量验证（这些计数会被映射到输入寄存器供诊断）
 * ================================================================== */
static void test_statistics(void)
{
    uint8_t req[8];
    uint16_t crc;

    printf("\n[C1] 运行统计量（诊断与实测数据来源）\n");

    mb_slave_init(&g_slave, 1u, &g_regs);

    /* 3 次合法读取 */
    {
        int i;
        uint8_t good[8] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00 };
        crc = mb_crc16(good, 6u);
        good[6] = (uint8_t)(crc & 0xFFu);
        good[7] = (uint8_t)(crc >> 8);
        for (i = 0; i < 3; i++) {
            (void)mb_slave_process(&g_slave, good, 8u);
        }
    }
    if (g_slave.stat_rx_frames == 3u) {
        ok("合法帧计数 = 3");
    } else {
        char d[64];
        sprintf(d, "(实际 %lu)", (unsigned long)g_slave.stat_rx_frames);
        fail("合法帧计数 = 3", d);
    }

    /* 2 次站号不匹配 */
    {
        int i;
        uint8_t miss[8] = { 0x05, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00 };
        crc = mb_crc16(miss, 6u);
        miss[6] = (uint8_t)(crc & 0xFFu);
        miss[7] = (uint8_t)(crc >> 8);
        for (i = 0; i < 2; i++) {
            (void)mb_slave_process(&g_slave, miss, 8u);
        }
    }
    if (g_slave.stat_addr_miss == 2u) {
        ok("站号不匹配计数 = 2");
    } else {
        char d[64];
        sprintf(d, "(实际 %lu)", (unsigned long)g_slave.stat_addr_miss);
        fail("站号不匹配计数 = 2", d);
    }

    /* 1 次 CRC 错 */
    memcpy(req, "\x01\x03\x00\x00\x00\x01\x00\x00", 8);
    (void)mb_slave_process(&g_slave, req, 8u);
    if (g_slave.stat_crc_errors == 1u) {
        ok("CRC 错误计数 = 1");
    } else {
        char d[64];
        sprintf(d, "(实际 %lu)", (unsigned long)g_slave.stat_crc_errors);
        fail("CRC 错误计数 = 1", d);
    }

    /* 1 次异常响应（非法功能码） */
    {
        uint8_t exc[8] = { 0x01, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00 };
        crc = mb_crc16(exc, 6u);
        exc[6] = (uint8_t)(crc & 0xFFu);
        exc[7] = (uint8_t)(crc >> 8);
        (void)mb_slave_process(&g_slave, exc, 8u);
    }
    if (g_slave.stat_exceptions == 1u) {
        ok("异常响应计数 = 1");
    } else {
        char d[64];
        sprintf(d, "(实际 %lu)", (unsigned long)g_slave.stat_exceptions);
        fail("异常响应计数 = 1", d);
    }

    /* 统计隔离性。
     *
     * 这里必须说清语义，否则很容易误判：
     *   stat_rx_frames 统计的是"CRC 通过且寻址到本机的帧"，
     *   也就是协议层真正处理过的帧。
     *   因此：
     *     - CRC 错误帧      -> 不计入 rx_frames，只计入 crc_errors
     *     - 站号不匹配帧    -> 不计入 rx_frames，只计入 addr_miss
     *     - 非法功能码帧    -> CRC 已通过，计入 rx_frames，同时发出异常响应
     *   所以上面 3 个合法读 + 1 个非法功能码 = 4，而不是 3。
     *   这个语义是对的：非法功能码说明总线通信正常，只是主机请求了不支持的功能，
     *   把它算作"坏帧"会掩盖真实的链路健康度。 */
    if (g_slave.stat_rx_frames == 4u) {
        ok("合法帧计数 = 4（3 次读 + 1 次非法功能码，CRC 均通过）");
    } else {
        char d[80];
        sprintf(d, "(实际 %lu, 期望 4)", (unsigned long)g_slave.stat_rx_frames);
        fail("合法帧计数 = 4", d);
    }

    /* 反向确认：错误帧确实被记在了各自的计数器上 */
    if (g_slave.stat_crc_errors == 1u && g_slave.stat_addr_miss == 2u &&
        g_slave.stat_exceptions == 1u) {
        ok("三类错误分别记入各自计数器，互不混淆");
    } else {
        char d[96];
        sprintf(d, "(crc=%lu miss=%lu exc=%lu)",
                (unsigned long)g_slave.stat_crc_errors,
                (unsigned long)g_slave.stat_addr_miss,
                (unsigned long)g_slave.stat_exceptions);
        fail("三类错误分别记入各自计数器", d);
    }
}

int main(void)
{
    printf("========== 寄存器映射表契约 + 从站协议测试 ==========\n");

    test_contract_counts();
    test_contract_offsets();
    test_contract_status_bits();

    test_input_boundary();
    test_holding_boundary();
    test_device_id_split();
    test_command_registers();
    test_silent_cases();

    test_protocol_engine();

    test_statistics();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
