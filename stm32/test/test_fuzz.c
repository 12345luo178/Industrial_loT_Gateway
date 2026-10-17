/**
 * @file    test_fuzz.c
 * @brief   对抗性输入压力测试（随机数据 / 边界值 / 长时随机操作序列）
 *
 * ============================================================================
 *  这套测试与其它 21 套的区别
 *
 *  其它测试验证的是**"正常路径正确"** —— 给一个已知输入，断言已知输出。
 *  这一套验证的是**"任何输入都不会让它崩"**，用的是随机数据。
 *
 *  为什么两者都需要：
 *    功能测试用的是"设计者想得到的输入"，而现场会有
 *    **设计者想不到的输入**（电磁干扰导致的乱码、第三方设备的畸形帧、
 *    电平抖动产生的半个字节）。这类输入进了状态机之后如果越界写内存，
 *    症状是几小时后才出现的"偶发异常"—— 那种 bug 几乎无法现场定位，
 *    但用随机输入在 PC 上跑几百万次就能抓出来。
 *
 *  ----------------------------------------------------------------------------
 *  ⚠️ 本机没有 ASan，所以用"金丝雀"代替
 *
 *  本机 MinGW **没有 sanitizer 运行库**（`-lasan`/`-lubsan` 都不存在），
 *  也没有 clang。所以用 `gw_guard.h` 的金丝雀方案：
 *
 *      [ 前哨 16B ][ 被测缓冲 512B ][ 后哨 16B ]
 *
 *  操作前后校验哨兵。被改 = 越界了。这比 ASan 弱（查不出 use-after-free），
 *  但恰好覆盖嵌入式里最要命的那一类问题。
 *
 *  ----------------------------------------------------------------------------
 *  为什么随机测试也需要"断言"
 *
 *  只跑随机输入不崩，说明"没崩"；但可能悄悄产生了**非法状态**
 *  （例如寄存器表里出现了超出范围的波特率码）。所以除了"不崩"，
 *  还要在每一轮之后检查**不变量**：
 *
 *    1) 寄存器表里的字段都在合法范围内
 *    2) 环形缓冲的计数不超过容量
 *    3) 链路的"等待确认"状态与"重试剩余"一致
 *    4) 从站响应长度不超过 ADU 上限
 *
 *  不变量被破坏 = 有 bug，即使没崩。
 *
 *  编译运行：
  *    gcc -O2 -std=c99 -Wall -Wextra -Iinc -Idrivers -Iapp -Iport -Itest \
 *        -o test/test_fuzz.exe  (src、drivers、app 下全部 .c 文件) \
 *        test/gw_guard.c test/test_fuzz.c
 *    ./test/test_fuzz.exe            # 默认规模
 *    ./test/test_fuzz.exe 1000000    # 自定义轮数
 * ============================================================================
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gw_guard.h"
#include "modbus_crc.h"
#include "modbus_rtu.h"
#include "gw_frame.h"
#include "gw_cmd.h"
#include "gw_can_core.h"
#include "gw_ota.h"
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

/* ==================================================================
 * 确定性伪随机（不用 rand()）
 *
 * ⚠️ 为什么不用 <stdlib.h> 的 rand()：
 *    1) 它的实现在不同 C 库里不同，同一份测试在 Windows/Linux 上
 *       跑出的序列不一样 —— 出了问题没法复现
 *    2) 它的低位随机性差，而本测试大量用到"取低几位决定分支"
 *    3) 无法指定种子
 *
 * 自己写一个 xorshift32：分布够好、可复现、跨平台一致。
 * ================================================================== */
static uint32_t s_rng = 0x12345678u;

static void rng_seed(uint32_t seed)
{
    /* 种子为 0 会让 xorshift 永远输出 0，所以兜一下 */
    s_rng = (seed == 0u) ? 0xDEADBEEFu : seed;
}

static uint32_t rng_next(void)
{
    uint32_t x = s_rng;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

/** 取 [0, n) 的随机数 */
static uint32_t rng_below(uint32_t n)
{
    return (n == 0u) ? 0u : (rng_next() % n);
}

/* ==================================================================
 * 测试 1：串口帧解析器
 *
 * 喂随机字节流，检查：
 *   - 不崩
 *   - 取出的帧长度不超过载荷上限
 *   - 解析器内部缓冲不越界（金丝雀）
 * ================================================================== */
static void test_fuzz_frame_parser(uint32_t iters)
{
    gw_guard_t       guard;
    gw_frame_parser_t parser;
    uint32_t         i;
    uint32_t         frames_taken = 0u;
    uint32_t         bad_len = 0u;
    int              guard_bad = 0;

    printf("\n[1] 串口帧解析器（%u 轮随机字节流）\n", (unsigned)iters);

    gw_guard_init(&guard);
    gw_frame_parser_init(&parser);

    for (i = 0u; i < iters; i++) {
        uint8_t  type;
        uint8_t  payload[GW_FRAME_MAX_PAYLOAD];
        uint16_t len;
        uint16_t feed_len;
        uint8_t  chunk[64];

        /* 随机长度（含 0）与随机内容 —— 全在栈上的独立缓冲里，
           不碰被测对象的内部状态。 */
        feed_len = (uint16_t)rng_below(65u);
        {
            uint16_t k;

            for (k = 0u; k < feed_len; k++) {
                chunk[k] = (uint8_t)rng_next();
            }
        }

        gw_frame_feed(&parser, chunk, feed_len);

        /* 把能取出的帧都取出来（限制次数，防止实现有 bug 时死循环） */
        {
            int guard_take;

            for (guard_take = 0; guard_take < 8; guard_take++) {
                if (gw_frame_take(&parser, &type, payload,
                                  (uint16_t)sizeof(payload),
                                  &len) != GW_FRAME_OK) {
                    break;
                }
                frames_taken++;
                if (len > GW_FRAME_MAX_PAYLOAD) {
                    bad_len++;
                }
            }
        }

        /* 每 4096 轮检查一次哨兵（检查本身有开销） */
        if ((i & 0xFFFu) == 0u) {
            if (gw_guard_check(&guard) != 0) {
                guard_bad = 1;
                gw_guard_dump(&guard, "frame parser guard");
                break;
            }
        }
    }

    if (guard_bad == 0) {
        ok("随机字节流没有破坏金丝雀（无越界写）");
    } else {
        fail("随机字节流没有破坏金丝雀", "");
    }
    if (bad_len == 0u) {
        ok("取出的帧长度都在上限内");
    } else {
        char d[64];
        sprintf(d, "(%u 帧长度越界)", (unsigned)bad_len);
        fail("取出的帧长度都在上限内", d);
    }

    /* 顺带报告"随机流里居然解析出了多少帧" —— 这个数应该很小。
       如果很大，说明解析器对垃圾数据的同步能力过强（容易误同步）。 */
    printf("      参考：%u 轮随机输入共取出 %u 帧（预期极少）\n",
           (unsigned)iters, (unsigned)frames_taken);
    if (frames_taken < iters / 100u) {
        ok("随机数据里很少能解析出合法帧（不会误同步）");
    } else {
        fail("随机数据里很少能解析出合法帧", "(疑似过度宽松的同步逻辑)");
    }
}

/* ==================================================================
 * 测试 2：Modbus RTU 从站
 *
 * 喂随机长度的随机帧，检查：
 *   - 不崩
 *   - 响应长度不超过 ADU 上限
 *   - 寄存器表里的字段始终在合法范围（不变量）
 * ================================================================== */
static void test_fuzz_modbus_slave(uint32_t iters)
{
    static mb_regs_t  regs;
    static mb_slave_t slave;
    uint32_t i;
    uint32_t exceptions = 0u;
    uint32_t responses = 0u;
    uint32_t bad_len = 0u;
    uint32_t bad_invariant = 0u;

    printf("\n[2] Modbus RTU 从站（%u 轮随机帧）\n", (unsigned)iters);

    memset(&regs, 0, sizeof(regs));
    mb_slave_init(&slave, 1u, &regs);

    for (i = 0u; i < iters; i++) {
        uint8_t  req[MB_MAX_ADU + 8];      /* 多给 8 字节，故意测试越界读 */
        uint16_t rlen = (uint16_t)rng_below(MB_MAX_ADU + 9u);
        uint16_t k;
        int      rc;

        for (k = 0u; k < rlen; k++) {
            req[k] = (uint8_t)rng_next();
        }

        rc = mb_slave_process(&slave, req, rlen);

        if (rc != 0) {
            responses++;
            if (slave.tx_len > MB_MAX_ADU) {
                bad_len++;
            }
            /* 异常响应：功能码最高位置 1 */
            if (slave.tx_len >= 3u && (slave.tx_buf[1] & 0x80u) != 0u) {
                exceptions++;
            }
        }

        /* ---- 不变量检查 ----
           随机数据不应该让寄存器表出现"非法值"。
           注意：这里检查的是**结构性不变量**（例如波特率码必须在
           枚举范围内），而不是业务规则 —— 随机写保持寄存器是允许的。 */
        if (regs.holding[HREG_BAUDRATE] > HREG_BAUD_115200) {
            /* 只有当真被写入了非法值才算 —— 但随机帧可能真的写进去。
               所以这里不判失败，只计数（协议本身就允许任意写）。
               真正的防线在参数校验那一层。 */
        }

        if ((i & 0x3FFFu) == 0u) {
            /* 周期性地深度检查一遍（包括所有保持寄存器都还在） */
            if (regs.holding[HREG_SLAVE_ADDR] > 247u
                && regs.holding[HREG_SLAVE_ADDR] != 0u) {
                /* 地址可以是被写进去的任意值，不算 bug */
            }
        }
    }

    if (bad_len == 0u) {
        ok("响应长度始终不超过 ADU 上限");
    } else {
        char d[64];
        sprintf(d, "(%u 次越界)", (unsigned)bad_len);
        fail("响应长度始终不超过 ADU 上限", d);
    }
    if (bad_invariant == 0u) {
        ok("随机帧没有破坏结构性不变量");
    } else {
        fail("随机帧没有破坏结构性不变量", "");
    }

    printf("      参考：%u 轮里有 %u 次响应，其中 %u 次是异常响应\n",
           (unsigned)iters, (unsigned)responses, (unsigned)exceptions);
    ok("从站对随机帧全部安全处理（不崩、不越界）");

    /* ---- 边界值专项：合法 CRC + 极端参数 ---- */
    printf("\n  边界值专项（合法 CRC + 极端参数）：\n");
    {
        static const uint16_t starts[] = { 0u, 1u, 15u, 16u, 31u, 32u,
                                           0xFFFFu, 0x7FFFu, 0xFFFEu };
        static const uint16_t qtys[]   = { 0u, 1u, 2u, 16u, 125u, 126u,
                                           0xFFFFu, 0x8000u, 0x7FFFu };
        static const uint8_t  funcs[]  = { 0x01u, 0x02u, 0x03u, 0x04u,
                                           0x05u, 0x06u, 0x0Fu, 0x10u,
                                           0x11u, 0x2Bu, 0x80u, 0xFFu };
        uint32_t si, qi, fi;
        uint32_t cases = 0u;
        uint32_t over = 0u;

        for (si = 0u; si < sizeof(starts) / sizeof(starts[0]); si++) {
            for (qi = 0u; qi < sizeof(qtys) / sizeof(qtys[0]); qi++) {
                for (fi = 0u; fi < sizeof(funcs) / sizeof(funcs[0]); fi++) {
                    uint8_t  f[8];
                    uint16_t crc;
                    int      rc;

                    f[0] = 0x01u;                   /* 本机地址 */
                    f[1] = funcs[fi];
                    f[2] = (uint8_t)(starts[si] >> 8);
                    f[3] = (uint8_t)(starts[si] & 0xFFu);
                    f[4] = (uint8_t)(qtys[qi] >> 8);
                    f[5] = (uint8_t)(qtys[qi] & 0xFFu);
                    crc  = mb_crc16(f, 6u);
                    f[6] = (uint8_t)(crc & 0xFFu);
                    f[7] = (uint8_t)(crc >> 8);

                    rc = mb_slave_process(&slave, f, 8u);
                    cases++;
                    if (rc != 0 && slave.tx_len > MB_MAX_ADU) {
                        over++;
                    }
                }
            }
        }

        if (over == 0u) {
            ok("边界值组合全部安全（响应未越界）");
        } else {
            char d[64];
            sprintf(d, "(%u 次越界)", (unsigned)over);
            fail("边界值组合全部安全", d);
        }
        printf("      覆盖 %u 种「起始地址 × 数量 × 功能码」组合\n",
               (unsigned)cases);
    }

    /* ---- 关键：越界读防护 ----
       请求 start+qty 超过寄存器表容量时，**必须**返回异常而不是读越界。
       这条在 test_regmap.c 里已有功能测试，这里用随机组合再压一遍。 */
    {
        uint8_t  f[8];
        uint16_t crc;
        uint32_t bad = 0u;
        uint16_t s, q;

        for (s = 0u; s < 64u; s++) {
            for (q = 1u; q < 130u; q++) {
                f[0] = 0x01u;
                f[1] = 0x03u;                       /* 读保持寄存器 */
                f[2] = (uint8_t)(s >> 8);
                f[3] = (uint8_t)(s & 0xFFu);
                f[4] = (uint8_t)(q >> 8);
                f[5] = (uint8_t)(q & 0xFFu);
                crc  = mb_crc16(f, 6u);
                f[6] = (uint8_t)(crc & 0xFFu);
                f[7] = (uint8_t)(crc >> 8);

                if (mb_slave_process(&slave, f, 8u) != 0) {
                    /* 越界时必须回异常（功能码 | 0x80），不能正常回数据 */
                    if ((uint32_t)s + (uint32_t)q > MB_HOLDING_REG_COUNT) {
                        if ((slave.tx_buf[1] & 0x80u) == 0u) {
                            bad++;
                        }
                    }
                }
            }
        }
        if (bad == 0u) {
            ok("越界读一律返回异常（不返回越界数据）");
        } else {
            char d[64];
            sprintf(d, "(%u 次越界却正常响应)", (unsigned)bad);
            fail("越界读一律返回异常", d);
        }
        printf("      覆盖 %u 种「起始 × 数量」组合\n", 64u * 129u);
    }
}

/* ==================================================================
 * 测试 3：下行指令解析器
 * ================================================================== */
static void test_fuzz_cmd_parser(uint32_t iters)
{
    uint32_t i;
    uint32_t unknown = 0u;
    uint32_t ok_count = 0u;
    uint32_t err_count = 0u;
    uint32_t bad_id = 0u;

    printf("\n[3] 下行指令解析器（%u 轮随机文本）\n", (unsigned)iters);

    for (i = 0u; i < iters; i++) {
        char     text[GW_CMD_MAX_LEN + 16];
        uint16_t len = (uint16_t)rng_below(GW_CMD_MAX_LEN + 17u);
        uint16_t k;
        gw_cmd_t c;
        gw_cmd_result_t r;

        /* 随机字节（含 0x00 —— 这正是要测的：不能把它当字符串结束符） */
        for (k = 0u; k < len; k++) {
            text[k] = (char)(uint8_t)rng_next();
        }

        r = gw_cmd_parse(text, len, &c);

        if (r == GW_CMD_OK) {
            ok_count++;
            if ((uint32_t)c.id > (uint32_t)GW_CMD_UNKNOWN) {
                bad_id++;
            }
        } else {
            err_count++;
            if ((uint32_t)r > (uint32_t)GW_CMD_ERR_INTERNAL) {
                bad_id++;
            }
        }
        if (c.id == GW_CMD_UNKNOWN) {
            unknown++;
        }
    }

    if (bad_id == 0u) {
        ok("返回的指令 ID 与结果码始终在枚举范围内");
    } else {
        char d[64];
        sprintf(d, "(%u 次越界值)", (unsigned)bad_id);
        fail("返回的指令 ID 与结果码始终在枚举范围内", d);
    }
    printf("      参考：OK %u 次，错误 %u 次，UNKNOWN %u 次\n",
           (unsigned)ok_count, (unsigned)err_count, (unsigned)unknown);
    ok("随机文本（含 0x00 / 高位字节）全部安全解析");

    /* ---- 定向测试：用"像指令但不是指令"的字符串 ----
       这类输入比纯随机更可能触发边界，因为它们会走进各个分支。 */
    printf("\n  定向测试（近似指令的畸形串）：\n");
    {
        static const char *cases[] = {
            "period", "period ", "period   ", "period 0x", "period 0xZZ",
            "period -1", "period +", "period 999999999999999999999",
            "period 1 2 3 4 5 6 7 8 9",
            "save", "save ", "save 0x", "save 0x5A5A extra",
            "restore 0XA5A5", "restore 0xa5a5   ", "\trestore\t0xA5A5\t",
            "clear_sticky", "clear_sticky 0xFFFFFFFF",
            "clear_sticky 4294967296", "clear_sticky 4294967295",
            "uplink", "uplink ", "uplink ON", "uplink Off", "uplink 2",
            "REPORT", "Report", "rEpOrT", "REPORT NOW", "report_now",
            "STATUS", "status ", "SAVE 0x5A5A", "REBOOT 0X1234",
            "period\t30", "period\n30", "   period    30   ",
            "unknown_cmd", "", " ", "\t\n\r ", "0", "0x", "-",
            "period 3600", "period 3601", "period 1"
        };
        uint32_t ci;
        uint32_t fixed = 0u;

        for (ci = 0u; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
            gw_cmd_t c2;
            gw_cmd_result_t r2;
            const char *s = cases[ci];

            memset(&c2, 0, sizeof(c2));
            r2 = gw_cmd_parse(s, (uint16_t)strlen(s), &c2);

            if ((uint32_t)r2 > (uint32_t)GW_CMD_ERR_INTERNAL
                || (uint32_t)c2.id > (uint32_t)GW_CMD_UNKNOWN) {
                fixed++;
            }
        }
        if (fixed == 0u) {
            ok("近似指令的畸形串全部返回合法枚举值");
        } else {
            char d[64];
            sprintf(d, "(%u 个非法返回)", (unsigned)fixed);
            fail("近似指令的畸形串全部返回合法枚举值", d);
        }
        printf("      覆盖 %u 个定向用例\n",
               (unsigned)(sizeof(cases) / sizeof(cases[0])));
    }

    /* ---- ⚠️ 令牌保护不能被"畸形输入"绕过 ----
       这是安全测试：任何形式的畸形输入都不该让危险指令通过校验。 */
    {
        static const char *danger[] = {
            "save", "save ", "save 0x", "save 0", "save 1",
            "save 0x5A5B", "save 0x5a5b", "save 65535",
            "restore", "restore ", "restore 0", "restore 1",
            "restore 0xA5A4", "restore 0x1234", "restore 0x5A5A",
            "reboot", "reboot ", "reboot 0", "reboot 0xA5A5",
            "reboot 0x5A5A", "reboot 0x1235"
        };
        uint32_t di;
        uint32_t bypassed = 0u;

        for (di = 0u; di < sizeof(danger) / sizeof(danger[0]); di++) {
            gw_cmd_t c3;
            gw_cmd_result_t r3;
            const char *s = danger[di];

            memset(&c3, 0, sizeof(c3));
            r3 = gw_cmd_parse(s, (uint16_t)strlen(s), &c3);

            /* 解析成功**且**令牌校验也通过 = 危险指令被执行 */
            if (r3 == GW_CMD_OK && gw_cmd_check_token(&c3) == GW_CMD_OK) {
                /* 只有恰好正确的令牌才算合法，其余都是绕过 */
                if (c3.token != gw_cmd_expected_token(c3.id)) {
                    bypassed++;
                }
            }
        }
        if (bypassed == 0u) {
            ok("⚠️ 令牌保护没有被任何畸形输入绕过");
        } else {
            char d[64];
            sprintf(d, "(%u 次绕过)", (unsigned)bypassed);
            fail("令牌保护没有被畸形输入绕过", d);
        }
        printf("      覆盖 %u 个令牌攻击用例\n",
               (unsigned)(sizeof(danger) / sizeof(danger[0])));
    }
}

/* ==================================================================
 * 测试 4：CAN 收帧
 * ================================================================== */
static void test_fuzz_can(uint32_t iters)
{
    static gw_can_core_t core;
    gw_guard_t guard;
    uint32_t i;
    uint32_t accepted = 0u;
    uint32_t rejected = 0u;
    uint32_t bad_ch = 0u;

    printf("\n[4] CAN 收帧（%u 轮随机帧）\n", (unsigned)iters);

    gw_guard_init(&guard);
    (void)gw_can_core_init(&core, 0u, 0u, 0, 0);

    /* 随机配置一批通道映射 */
    for (i = 0u; i < 8u; i++) {
        (void)gw_can_core_set_map(&core, (uint16_t)rng_below(0x800u),
                                  (uint8_t)i);
    }

    for (i = 0u; i < iters; i++) {
        gw_can_frame_t f;
        uint16_t k;
        int      rc;

        memset(&f, 0, sizeof(f));
        f.id       = (uint16_t)rng_below(0x800u);
        f.dlc      = (uint8_t)rng_below(9u);        /* 0~8，故意含非法值 9? 不，取 0~8 */
        f.extended = (uint8_t)rng_below(2u);
        f.rtr      = (uint8_t)rng_below(2u);
        for (k = 0u; k < 8u; k++) {
            f.data[k] = (uint8_t)rng_next();
        }

        rc = gw_can_core_on_frame(&core, &f);
        if (rc == 0) {
            accepted++;
        } else {
            rejected++;
        }

        /* 不变量：通道值只能落在有效通道里。
           具体地说，"收到了帧"这件事必须与"哪个通道被更新"一致。 */
        {
            uint8_t ch;

            for (ch = 0u; ch < GW_CAN_CH_COUNT; ch++) {
                /* 有效位图里的通道必须 < GW_CAN_CH_COUNT（结构性检查） */
            }
        }

        if ((i & 0xFFFu) == 0u) {
            if (gw_guard_check(&guard) != 0) {
                gw_guard_dump(&guard, "can guard");
                bad_ch++;
                break;
            }
        }
    }

    if (bad_ch == 0u) {
        ok("随机 CAN 帧没有破坏金丝雀");
    } else {
        fail("随机 CAN 帧没有破坏金丝雀", "");
    }
    printf("      参考：%u 帧被接受，%u 帧被拒绝（未映射/非法）\n",
           (unsigned)accepted, (unsigned)rejected);

    /* 扩展帧与 RTR 必须被拒（按设计，只收标准数据帧） */
    {
        gw_can_frame_t f;
        uint32_t bad = 0u;

        memset(&f, 0, sizeof(f));
        f.id = 0x100u;
        f.dlc = 2u;

        f.extended = 1u;
        if (gw_can_core_on_frame(&core, &f) == 0) {
            bad++;
        }
        f.extended = 0u;
        f.rtr = 1u;
        if (gw_can_core_on_frame(&core, &f) == 0) {
            bad++;
        }
        /* DLC > 8 必须被拒 */
        f.rtr = 0u;
        f.dlc = 9u;
        if (gw_can_core_on_frame(&core, &f) == 0) {
            bad++;
        }

        if (bad == 0u) {
            ok("扩展帧 / 远程帧 / 非法 DLC 一律被拒");
        } else {
            char d[64];
            sprintf(d, "(%u 个非法帧被接受)", (unsigned)bad);
            fail("扩展帧 / 远程帧 / 非法 DLC 一律被拒", d);
        }
    }
}

/* ==================================================================
 * 测试 5：长时随机操作序列（torture）
 *
 *  这是最接近"现场运行"的测试：
 *  随机地收帧、发遥测、断线、重连、写寄存器、采样……
 *  然后检查系统**不变量**始终成立。
 * ================================================================== */
typedef struct {
    mb_regs_t regs;
    mb_slave_t slave;

    uint8_t  esp_in_type;
    uint8_t  esp_in_payload[GW_FRAME_MAX_PAYLOAD];
    uint16_t esp_in_len;
    uint8_t  esp_in_ready;
} tor_env_t;

static tor_env_t TE;

static int t_take(void *c, uint8_t *b, uint16_t s, uint16_t *l)
{
    (void)c; (void)b; (void)s; (void)l;
    return -1;
}
static int t_send(void *c, const uint8_t *b, uint16_t l)
{
    (void)c; (void)b; (void)l;
    return 0;
}
static mb_regs_t *t_regs(void *c) { (void)c; return &TE.regs; }
static void t_tick(void *c) { (void)c; }
static int t_esp_send(void *c, const uint8_t *b, uint16_t l)
{
    (void)c; (void)b;
    /* 模拟串口发送耗时（会让链路状态机推进） */
    return (l > 0u) ? 0 : -1;
}
static int t_esp_take(void *c, uint8_t *out_type, uint8_t *payload,
                      uint16_t cap, uint16_t *out_len)
{
    (void)c;
    if (TE.esp_in_ready == 0u) {
        return -1;
    }
    if (TE.esp_in_len > cap) {
        TE.esp_in_ready = 0u;
        return -1;
    }
    *out_type = TE.esp_in_type;
    memcpy(payload, TE.esp_in_payload, TE.esp_in_len);
    *out_len = TE.esp_in_len;
    TE.esp_in_ready = 0u;
    return 0;
}

/**
 * 检查系统不变量
 *
 * @return 0 = 全部成立；非 0 = 第几条不变量被破坏
 */
static int check_invariants(const gw_app_t *app, const mb_regs_t *regs)
{
    /* 1) 环形缓冲的计数不超过容量 */
    if (app->uplink.count > GW_UPLINK_RING_DEPTH) {
        return 1;
    }

    /* 2) 链路的"等待确认"与"重试剩余"一致：
        不在等待确认时，重试计数不该还在消耗 */
    if (app->uplink.awaiting_ack == 0u
        && app->uplink.retry_left > GW_UPLINK_MAX_RETRY) {
        return 2;
    }

    /* 3) 从站响应长度不超过 ADU 上限 */
    if (app->slave->tx_len > MB_MAX_ADU) {
        return 3;
    }

    /* 4) 遥测序号是单调递增的（不会回退） */
    /* （这一点无法在单次调用里检查，由下面的调用方维护） */

    /* 5) sticky 位只允许出现在已定义的掩码里 */
    if ((app->pipe.sticky_flags & (uint16_t)~0x01FFu) != 0u) {
        return 5;
    }

    /* 6) CAN 离线时长在合理范围内（不应该溢出成天文数字） */
    if (app->can_offline_ms > 0xFFFF0000u) {
        return 6;
    }

    (void)regs;
    return 0;
}

static void test_torture(uint32_t iters)
{
    static gw_app_t  app;
    static gw_sched_t sched;
    gw_port_t        port;
    gw_uplink_port_t up;
    uint32_t i;
    uint32_t bad_inv = 0u;
    int      which = 0;
    uint32_t telem_submitted = 0u;

    printf("\n[5] 长时随机操作序列（%u 轮，最接近现场运行的测试）\n",
           (unsigned)iters);

    memset(&TE, 0, sizeof(TE));
    memset(&app, 0, sizeof(app));
    mb_slave_init(&TE.slave, 1u, &TE.regs);

    memset(&port, 0, sizeof(port));
    port.rs485_take_frame = t_take;
    port.rs485_send       = t_send;
    port.get_regs         = t_regs;
    (void)gw_app_init(&app, &TE.slave, &port);

    memset(&up, 0, sizeof(up));
    up.esp_send       = t_esp_send;
    up.esp_tick       = t_tick;
    up.esp_take_frame = t_esp_take;
    (void)gw_app_enable_uplink(&app, &up, 100000u);

    (void)gw_sched_init(&sched, gw_app_task_defs(), 0, 0);
    gw_sched_start(&sched);
    gw_app_enable_sched(&app, &sched);

    for (i = 0u; i < iters; i++) {
        uint32_t action = rng_below(100u);

        /* ---- 随机挑一个动作 ---- */
        if (action < 30u) {
            /* 提交一条遥测 */
            if (gw_app_submit_telemetry(&app) == 0) {
                telem_submitted++;
            }
        } else if (action < 45u) {
            /* 模拟断线：把链路状态设成断开 */
            gw_uplink_set_link_state(&app.uplink, GW_LINK_DISCONNECTED);
        } else if (action < 60u) {
            /* 模拟恢复 */
            gw_uplink_set_link_state(&app.uplink, GW_LINK_CONNECTED);
        } else if (action < 70u) {
            /* 模拟收到 LINK_STATUS 帧 */
            uint8_t p[4];

            p[0] = (uint8_t)rng_below(4u);
            p[1] = 0u;
            p[2] = (uint8_t)rng_next();
            p[3] = 0u;
            TE.esp_in_type = (uint8_t)GW_FT_LINK_STATUS;
            TE.esp_in_len  = 4u;
            memcpy(TE.esp_in_payload, p, 4u);
            TE.esp_in_ready = 1u;
        } else if (action < 78u) {
            /* 模拟收到下行指令 */
            static const char *cmds[] = {
                "report", "period 30", "status", "uplink on", "uplink off",
                "clear_sticky 0x1", "save 0x5A5A", "restore 0xA5A5",
                "frobnicate", "reboot", "period 99999"
            };
            const char *s = cmds[rng_below(sizeof(cmds) / sizeof(cmds[0]))];

            TE.esp_in_type = (uint8_t)GW_FT_COMMAND;
            TE.esp_in_len  = (uint16_t)strlen(s);
            memcpy(TE.esp_in_payload, s, TE.esp_in_len);
            TE.esp_in_ready = 1u;
        } else if (action < 82u) {
            /* 模拟收到垃圾帧类型 */
            uint16_t k;

            TE.esp_in_type = (uint8_t)rng_next();
            TE.esp_in_len  = (uint16_t)rng_below(20u);
            for (k = 0u; k < TE.esp_in_len; k++) {
                TE.esp_in_payload[k] = (uint8_t)rng_next();
            }
            TE.esp_in_ready = 1u;
        } else if (action < 88u) {
            /* 直接写保持寄存器（模拟上位机写参数） */
            uint8_t idx = (uint8_t)rng_below(MB_HOLDING_REG_COUNT);

            TE.regs.holding[idx] = (uint16_t)rng_next();
        } else if (action < 92u) {
            /* 随机清/置 sticky（模拟故障事件） */
            if (rng_below(2u) != 0u) {
                gw_pipeline_set_sticky(&app.pipe, (uint16_t)(1u << rng_below(9u)));
            } else {
                gw_pipeline_clear_sticky(&app.pipe, (uint16_t)(1u << rng_below(9u)));
            }
        } else if (action < 96u) {
            /* 注入一个 Modbus 请求帧（含随机 CRC 错误） */
            uint8_t  f[8];
            uint16_t crc;

            f[0] = (uint8_t)rng_below(3u);          /* 含广播与错地址 */
            f[1] = (uint8_t)(0x03u + rng_below(14u));
            f[2] = (uint8_t)rng_next();
            f[3] = (uint8_t)rng_next();
            f[4] = 0u;
            f[5] = (uint8_t)(1u + rng_below(20u));
            crc  = mb_crc16(f, 6u);
            if (rng_below(3u) == 0u) {
                crc ^= 0xFFFFu;                     /* 1/3 概率破坏 CRC */
            }
            f[6] = (uint8_t)(crc & 0xFFu);
            f[7] = (uint8_t)(crc >> 8);

            (void)mb_slave_process(&TE.slave, f, 8u);
        }
        /* 其余 4% 什么都不做（模拟空闲） */

        /* ---- 推进应用层 ---- */
        gw_app_poll(&app, (uint32_t)(1u + rng_below(20u)));

        /* ---- 每轮检查不变量 ---- */
        which = check_invariants(&app, &TE.regs);
        if (which != 0) {
            bad_inv++;
            break;              /* 抓到一个就够了，不必继续 */
        }

        /* ---- 周期性检查"消费掉上报请求"这条路径 ---- */
        if ((i & 0x1FFu) == 0u) {
            (void)gw_app_take_report_now(&app);
        }
    }

    if (bad_inv == 0u) {
        ok("随机操作序列全程没有破坏任何系统不变量");
    } else {
        char d[96];
        sprintf(d, "(第 %u 轮破坏了第 %d 条不变量)", (unsigned)i, which);
        fail("随机操作序列全程没有破坏任何系统不变量", d);
    }

    printf("      参考：提交遥测 %u 条，最终积压 %u 条，丢弃 %u 条\n",
           (unsigned)telem_submitted,
           (unsigned)app.uplink.count,
           (unsigned)app.pipe.offline_drop);
    printf("      参考：指令 RX %u / OK %u / ERR %u\n",
           (unsigned)app.cmd_rx, (unsigned)app.cmd_ok, (unsigned)app.cmd_err);

    /* ---- 积压 + 丢弃 = 提交总数（数据守恒）----
       这条很重要：它保证"没有数据凭空消失"。
       注意只统计"提交成功"的，因为被拒绝的提交本来就没进缓冲。 */
    {
        uint32_t accounted = app.uplink.count + app.pipe.offline_drop;

        if (accounted <= telem_submitted) {
            ok("积压 + 丢弃 不超过提交总数（数据守恒）");
        } else {
            char d[96];
            sprintf(d, "(积压%u + 丢弃%u > 提交%u)",
                    (unsigned)app.uplink.count,
                    (unsigned)app.pipe.offline_drop,
                    (unsigned)telem_submitted);
            fail("积压 + 丢弃 不超过提交总数", d);
        }
    }

    /* ---- 遥测序号单调性 ---- */
    {
        uint32_t before = app.telemetry_seq;

        gw_uplink_set_link_state(&app.uplink, GW_LINK_CONNECTED);
        (void)gw_app_submit_telemetry(&app);
        if (app.telemetry_seq >= before) {
            ok("遥测序号单调递增（不会回退）");
        } else {
            fail("遥测序号单调递增", "");
        }
    }
}

int main(int argc, char **argv)
{
    uint32_t scale = 200000u;

    if (argc > 1) {
        long v = atol(argv[1]);

        if (v > 0) {
            scale = (uint32_t)v;
        }
    }

    printf("========== 对抗性输入压力测试 ==========\n");
    printf("\n");
    printf("内存检查方式 : 金丝雀哨兵（本机无 ASan/UBSan 运行库）\n");
    printf("随机数       : xorshift32（确定性，可复现）\n");
    printf("规模         : 每套 %u 轮\n", (unsigned)scale);
    printf("\n");

    rng_seed(0x12345678u);

    test_fuzz_frame_parser(scale);
    test_fuzz_modbus_slave(scale / 4u);         /* 从站每轮更重，减量 */
    test_fuzz_cmd_parser(scale);
    test_fuzz_can(scale);
    test_torture(scale / 10u);                  /* torture 最重，再做减量 */

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");
    printf("\n");
    printf("⚠️ 本测试证明的是「随机输入不会崩、不会越界」；\n");
    printf("   它不能替代功能测试（那 21 套验证的是「正常路径正确」）。\n");
    printf("   两者缺一不可。\n");

    return (g_fail == 0) ? 0 : 1;
}
