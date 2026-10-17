/**
 * @file    bench.c
 * @brief   主机端性能基准（量化实测数据的一部分）
 *
 * ============================================================================
 *  这个工具测什么、不测什么
 *
 *  **测**：各模块的**算法效率** —— 用"每次操作耗时"与"每秒操作次数"表示。
 *         这部分与主频无关，所以：
 *           - 可以横向比较不同机器
 *           - 更重要的是**回归基线**：改动后重跑，变慢了立刻能看出来
 *
 *  **不测**：真实硬件上的绝对耗时。那需要上板用 DWT 测，
 *          见 `doc/量化实测与测试报告.md` 的测量方案。
 *
 *  ----------------------------------------------------------------------------
 *  ⚠️ 这个工具里有三个坑，都是第一版踩过并修掉的
 *
 *  **坑 1：优化器会把"结果没被使用"的代码整段删掉。**
 *     第一版测 CRC32 更新，输出 0.0 ns —— 编译器发现整个循环只往一个
 *     局部变量累加、而那个变量最后没人用，于是全删了。
 *     现在用 `g_blackhole`（volatile）承接所有结果。
 *
 *  **坑 2：单次耗时太短时，计时器分辨率会让数字完全失真。**
 *     `clock()` 分辨率 1ms。如果单次操作 2ns，那 100 万次才 2ms ——
 *     只有 2 个计时单位，误差可能 50%。
 *     现在的做法：**自适应批量计时** —— 先跑一小批，不够长就加倍，
 *     直到总耗时 >= 30ms（量化误差 < 1%）。
 *
 *  **坑 3：不同规模的输入不能混在一张表里比较绝对值。**
 *     第一版"处理 125 寄存器请求"显示 55ns，比"处理 4 寄存器请求"
 *     的 106ns **还快** —— 自相矛盾。原因是两次都测得太短，
 *     量化误差方向不同。现在所有条目都跑够 30ms，数字才可比。
 *
 *  ----------------------------------------------------------------------------
 *  怎么把 PC 数据用到 STM32 上（**必须谨慎**）
 *
 *  PC（x86，3 GHz+，乱序执行，大缓存）与 STM32（Cortex-M3，72 MHz，
 *  顺序执行，Flash 取指有等待周期）差异巨大。
 *
 *  可以跨平台比较的是**算法的工作量特征**：
 *    - "每字节耗时" —— CRC 这类逐位算法尤其适合这样比
 *    - "是 O(1) 还是 O(n)" —— 例如 CAN 收帧是常数时间
 *
 *  不能做的是"PC 上 5 纳秒，STM32 上大概也就几十纳秒"这种猜测。
 *  真实的 STM32 数字**必须上板用 DWT 实测**。
 *
 *  编译运行：
 *    gcc -O2 -std=c99 -Wall -Wextra -Iinc -Idrivers -Iapp -Iport \
 *        -o test/bench.exe <src/ drivers/ app/ 下全部 .c> test/bench.c
 *    ./test/bench.exe
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "modbus_crc.h"
#include "modbus_rtu.h"
#include "modbus_master.h"
#include "gw_frame.h"
#include "gw_log.h"
#include "gw_can_core.h"
#include "gw_pipeline.h"
#include "gw_timing.h"
#include "gw_sched.h"

/* ==================================================================
 *  防优化：黑盒
 *
 *  volatile 保证写入不会被删除，也不会被重排到计时区间之外。
 * ================================================================== */
static volatile uint32_t g_blackhole;

static void blackhole(uint32_t v)
{
    g_blackhole ^= v;
}

/* ==================================================================
 *  计时
 * ================================================================== */
static double now_sec(void)
{
    return (double)clock() / (double)CLOCKS_PER_SEC;
}

#define BENCH_TARGET_MS     30.0

/**
 * 自适应批量计时
 *
 * 先跑一小批，如果总耗时不足目标值就加倍重跑，
 * 直到测够 30ms（量化误差 < 1%）。
 *
 * @return 单次耗时（纳秒）；-1 表示这个操作快得测不出来
 */
static double bench_one(uint32_t (*fn)(uint32_t), uint32_t *out_iters)
{
    uint32_t iters = 1000u;
    int      round;

    for (round = 0; round < 24; round++) {
        uint32_t i;
        uint32_t acc = 0u;
        double   t0, t1, dt_ms;

        t0 = now_sec();
        for (i = 0u; i < iters; i++) {
            acc += fn(i);
        }
        t1 = now_sec();
        blackhole(acc);

        dt_ms = (t1 - t0) * 1000.0;

        if (dt_ms >= BENCH_TARGET_MS) {
            *out_iters = iters;
            return (t1 - t0) * 1e9 / (double)iters;
        }
        if (iters > 0x40000000u) {
            break;                      /* 再加就溢出了 */
        }
        iters *= 2u;
    }

    *out_iters = iters;
    return -1.0;
}

/* ==================================================================
 *  被测数据
 * ================================================================== */
static uint8_t  ADU[MB_MAX_ADU];
static uint16_t ADU_LEN;
static uint8_t  ADU_BIG[MB_MAX_ADU];
static uint16_t ADU_BIG_LEN;
static uint8_t  ADU_BAD[MB_MAX_ADU];
static uint8_t  ADU_BCAST[MB_MAX_ADU];

static mb_slave_t SLAVE;
static mb_regs_t  REGS;

static gw_frame_parser_t PARSER;
static uint8_t           FRAME_BUF[GW_FRAME_MAX_TOTAL];
static uint16_t          FRAME_LEN;

static gw_can_core_t   CAN;
static gw_can_frame_t  CAN_F;
static gw_can_frame_t  CAN_F_UNMAPPED;

static gw_pipeline_t   PIPE;
static gw_log_t        LOG;
static gw_timing_t     TIMING;

/* ---------- 被测函数 ---------- */

static uint32_t f_crc16_8(uint32_t i)
{
    (void)i;
    return (uint32_t)mb_crc16(ADU, 8u);
}

static uint32_t f_crc16_256(uint32_t i)
{
    (void)i;
    return (uint32_t)mb_crc16(ADU, 256u);
}

static uint32_t f_slave_read4(uint32_t i)
{
    (void)i;
    return (uint32_t)mb_slave_process(&SLAVE, ADU, ADU_LEN);
}

static uint32_t f_slave_read16(uint32_t i)
{
    (void)i;
    return (uint32_t)mb_slave_process(&SLAVE, ADU_BIG, ADU_BIG_LEN);
}

static uint32_t f_slave_bcast(uint32_t i)
{
    (void)i;
    return (uint32_t)mb_slave_process(&SLAVE, ADU_BCAST, 8u);
}

static uint32_t f_slave_badcrc(uint32_t i)
{
    (void)i;
    return (uint32_t)mb_slave_process(&SLAVE, ADU_BAD, 8u);
}

static uint32_t f_frame_build(uint32_t i)
{
    static const uint8_t payload[28] = {
        1,2,3,4,5,6,7,8,9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,26,27,28
    };
    uint16_t out_len = 0u;

    (void)i;
    return (uint32_t)gw_frame_build(FRAME_BUF, (uint16_t)sizeof(FRAME_BUF),
                                    GW_FT_TELEMETRY, payload,
                                    (uint16_t)sizeof(payload), &out_len);
}

static uint32_t f_frame_whole(uint32_t i)
{
    uint8_t  type;
    uint8_t  payload[GW_FRAME_MAX_PAYLOAD];
    uint16_t len;
    uint32_t n = 0u;

    (void)i;
    gw_frame_parser_init(&PARSER);
    gw_frame_feed(&PARSER, FRAME_BUF, FRAME_LEN);
    while (gw_frame_take(&PARSER, &type, payload,
                         (uint16_t)sizeof(payload), &len) == GW_FRAME_OK) {
        n++;
    }
    return n;
}

static uint32_t f_frame_bytewise(uint32_t i)
{
    uint8_t  type;
    uint8_t  payload[GW_FRAME_MAX_PAYLOAD];
    uint16_t len;
    uint16_t k;
    uint32_t n = 0u;

    (void)i;
    gw_frame_parser_init(&PARSER);
    for (k = 0u; k < FRAME_LEN; k++) {
        gw_frame_feed(&PARSER, &FRAME_BUF[k], 1u);
    }
    while (gw_frame_take(&PARSER, &type, payload,
                         (uint16_t)sizeof(payload), &len) == GW_FRAME_OK) {
        n++;
    }
    return n;
}

static uint32_t f_can_mapped(uint32_t i)
{
    (void)i;
    return (uint32_t)gw_can_core_on_frame(&CAN, &CAN_F);
}

static uint32_t f_can_unmapped(uint32_t i)
{
    (void)i;
    return (uint32_t)gw_can_core_on_frame(&CAN, &CAN_F_UNMAPPED);
}

static uint32_t f_pipe_sync(uint32_t i)
{
    (void)i;
    gw_pipeline_sync_regs(&PIPE, &REGS);
    return REGS.input[0];
}

static uint32_t f_status_word(uint32_t i)
{
    (void)i;
    return gw_pipeline_status_word(&PIPE);
}

static uint32_t f_log_write(uint32_t i)
{
    gw_log_write(&LOG, GW_LOG_DEBUG, GW_LOG_MOD_APP, 100u, i, 2u);
    return i;
}

static uint32_t f_log_filtered(uint32_t i)
{
    gw_log_write(&LOG, GW_LOG_DEBUG, GW_LOG_MOD_APP, 100u, i, 2u);
    return i;
}

static uint32_t f_timing_begin(uint32_t i)
{
    gw_timing_begin(&TIMING, (gw_phase_t)(i % (uint32_t)GW_PHASE_COUNT));
    return i;
}

/* ---- CRC32（OTA 整包校验的核心循环） ---- */
static uint32_t s_crc32_tab[256];
static uint8_t  CRC_BUF[128];

static void crc32_table_init(void)
{
    uint32_t i, j, c;

    for (i = 0u; i < 256u; i++) {
        c = i;
        for (j = 0u; j < 8u; j++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        s_crc32_tab[i] = c;
    }
}

static uint32_t f_crc32_128(uint32_t i)
{
    uint32_t c = 0xFFFFFFFFu;
    uint32_t k;

    for (k = 0u; k < (uint32_t)sizeof(CRC_BUF); k++) {
        c = s_crc32_tab[(c ^ CRC_BUF[k]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ i;
}

static uint32_t f_crc32_bitwise_128(uint32_t i)
{
    uint32_t c = 0xFFFFFFFFu;
    uint32_t k;
    int      j;

    for (k = 0u; k < (uint32_t)sizeof(CRC_BUF); k++) {
        c ^= CRC_BUF[k];
        for (j = 0; j < 8; j++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
    }
    return c ^ i;
}

/* ==================================================================
 *  造数据
 * ================================================================== */
static void build_all(void)
{
    uint16_t crc;

    ADU[0] = 0x01u; ADU[1] = 0x03u;
    ADU[2] = 0x00u; ADU[3] = 0x00u;
    ADU[4] = 0x00u; ADU[5] = 0x04u;
    crc = mb_crc16(ADU, 6u);
    ADU[6] = (uint8_t)(crc & 0xFFu);
    ADU[7] = (uint8_t)(crc >> 8);
    ADU_LEN = 8u;

    memcpy(ADU_BIG, ADU, 8u);
    ADU_BIG[4] = 0x00u; ADU_BIG[5] = 0x10u;   /* 16 = MB_HOLDING_REG_COUNT（表里全部） */
    crc = mb_crc16(ADU_BIG, 6u);
    ADU_BIG[6] = (uint8_t)(crc & 0xFFu);
    ADU_BIG[7] = (uint8_t)(crc >> 8);
    ADU_BIG_LEN = 8u;

    memcpy(ADU_BAD, ADU, 8u);
    ADU_BAD[7] ^= 0xFFu;

    memcpy(ADU_BCAST, ADU, 8u);
    ADU_BCAST[0] = 0x00u;
    crc = mb_crc16(ADU_BCAST, 6u);
    ADU_BCAST[6] = (uint8_t)(crc & 0xFFu);
    ADU_BCAST[7] = (uint8_t)(crc >> 8);

    {
        uint8_t  payload[28];
        uint16_t out_len = 0u;

        memset(payload, 0x5Au, sizeof(payload));
        (void)gw_frame_build(FRAME_BUF, (uint16_t)sizeof(FRAME_BUF),
                             GW_FT_TELEMETRY, payload,
                             (uint16_t)sizeof(payload), &out_len);
        FRAME_LEN = out_len;
    }

    memset(&CAN_F, 0, sizeof(CAN_F));
    CAN_F.id  = 0x100u;
    CAN_F.dlc = 2u;
    CAN_F.data[0] = 0x12u;
    CAN_F.data[1] = 0x34u;

    memset(&CAN_F_UNMAPPED, 0, sizeof(CAN_F_UNMAPPED));
    CAN_F_UNMAPPED.id  = 0x7FFu;
    CAN_F_UNMAPPED.dlc = 2u;

    memset(CRC_BUF, 0xA5u, sizeof(CRC_BUF));
}

#define HDR()                                                           \
    do {                                                                \
        printf("| %-32s | %10s | %12s | %14s |\n",                      \
               "操作", "迭代次数", "单次(ns)", "每秒次数");              \
        printf("|%s|%s|%s|%s|\n",                                       \
               "----------------------------------",                    \
               "------------", "--------------", "----------------");   \
    } while (0)

/* ==================================================================
 *  主程序
 * ================================================================== */
int main(void)
{
    printf("==============================================================\n");
    printf(" 主机端性能基准（算法效率，与主频无关）\n");
    printf("==============================================================\n");
    printf("\n");
    printf("计时器分辨率 : %.0f us\n", 1000000.0 / (double)CLOCKS_PER_SEC);
    printf("每个条目最少测 : %.0f ms（量化误差 < 1%%）\n", BENCH_TARGET_MS);
    printf("编译优化     : -O2（未开 -O2 的数字没有参考意义）\n");
    printf("\n");

    build_all();
    crc32_table_init();

    memset(&REGS, 0, sizeof(REGS));
    mb_slave_init(&SLAVE, 1u, &REGS);
    (void)gw_can_core_init(&CAN, 0u, 0u, 0, 0);
    (void)gw_can_core_set_map(&CAN, 0x100u, 0u);
    gw_pipeline_init(&PIPE, 0u, 4u);
    (void)gw_log_init(&LOG, 0, GW_LOG_DEBUG);
    (void)gw_timing_init(&TIMING, 0, 0);

    /* ---------------- 1) CRC16 ---------------- */
    printf("### 1) CRC16-Modbus（每帧都要算，最热的内循环）\n\n");
    HDR();
    {
        uint32_t it;
        double   a = bench_one(f_crc16_8,   &it);
        printf("| %-32s | %10u | %12.1f | %14.0f |\n",
               "CRC16 over 8 bytes", (unsigned)it, a,
               (a > 0.0) ? 1e9 / a : 0.0);
        a = bench_one(f_crc16_256, &it);
        printf("| %-32s | %10u | %12.1f | %14.0f |\n",
               "CRC16 over 256 bytes", (unsigned)it, a,
               (a > 0.0) ? 1e9 / a : 0.0);
        if (a > 0.0) {
            printf("\n> CRC16 是**逐位算法**（无查表），每字节耗时 = %.2f ns。\n",
                   a / 256.0);
            printf("> Cortex-M3 @72MHz 逐位 CRC16 约 8~12 周期/字节，\n");
            printf("> 即约 %.0f~%.0f us/KB。\n",
                   8.0 * 1024.0 / 72.0, 12.0 * 1024.0 / 72.0);
            printf("> Modbus 帧最长 256 字节 -> 单帧 CRC 约 %.1f~%.1f us。\n",
                   256.0 * 8.0 / 72.0, 256.0 * 12.0 / 72.0);
            printf("> 上板后请用 DWT 校准这几个数字。\n");
        }
    }

    /* ---------------- 2) Modbus 从站 ---------------- */
    printf("\n### 2) Modbus RTU 从站处理\n\n");
    HDR();
    {
        uint32_t it;
        double   a;

        a = bench_one(f_slave_read4, &it);
        printf("| %-32s | %10u | %12.1f | %14.0f |\n",
               "读 4 个保持寄存器(03)", (unsigned)it, a, (a > 0.0) ? 1e9 / a : 0.0);
        a = bench_one(f_slave_read16, &it);
        printf("| %-32s | %10u | %12.1f | %14.0f |\n",
               "读 16 个保持寄存器(全部)", (unsigned)it, a, (a > 0.0) ? 1e9 / a : 0.0);
        a = bench_one(f_slave_bcast, &it);
        printf("| %-32s | %10u | %12.1f | %14.0f |\n",
               "广播帧(执行但不应答)", (unsigned)it, a, (a > 0.0) ? 1e9 / a : 0.0);
        a = bench_one(f_slave_badcrc, &it);
        printf("| %-32s | %10u | %12.1f | %14.0f |\n",
               "CRC 错误帧(丢弃)", (unsigned)it, a, (a > 0.0) ? 1e9 / a : 0.0);
    }

    /* ---------------- 3) 串口帧 ---------------- */
    printf("\n### 3) 串口帧协议（STM32 ↔ ESP32）\n\n");
    HDR();
    {
        uint32_t it;
        double   a;

        a = bench_one(f_frame_build, &it);
        printf("| %-32s | %10u | %12.1f | %14.0f |\n",
               "组帧(遥测 28 字节载荷)", (unsigned)it, a, (a > 0.0) ? 1e9 / a : 0.0);
        a = bench_one(f_frame_whole, &it);
        printf("| %-32s | %10u | %12.1f | %14.0f |\n",
               "解析帧(整帧一次喂入)", (unsigned)it, a, (a > 0.0) ? 1e9 / a : 0.0);
        a = bench_one(f_frame_bytewise, &it);
        printf("| %-32s | %10u | %12.1f | %14.0f |\n",
               "解析帧(逐字节喂)", (unsigned)it, a, (a > 0.0) ? 1e9 / a : 0.0);
    }

    /* ---------------- 4) CAN ---------------- */
    printf("\n### 4) CAN 报文处理\n\n");
    HDR();
    {
        uint32_t it;
        double   a;

        a = bench_one(f_can_mapped, &it);
        printf("| %-32s | %10u | %12.1f | %14.0f |\n",
               "收帧(映射命中)", (unsigned)it, a, (a > 0.0) ? 1e9 / a : 0.0);
        a = bench_one(f_can_unmapped, &it);
        printf("| %-32s | %10u | %12.1f | %14.0f |\n",
               "收帧(未映射，仅计数)", (unsigned)it, a, (a > 0.0) ? 1e9 / a : 0.0);
    }

    /* ---------------- 5) 管道 ---------------- */
    printf("\n### 5) 数据管道与寄存器同步\n\n");
    HDR();
    {
        uint32_t it;
        double   a;

        a = bench_one(f_pipe_sync, &it);
        printf("| %-32s | %10u | %12.1f | %14.0f |\n",
               "同步到寄存器(48 个)", (unsigned)it, a, (a > 0.0) ? 1e9 / a : 0.0);
        a = bench_one(f_status_word, &it);
        printf("| %-32s | %10u | %12.1f | %14.0f |\n",
               "状态字计算", (unsigned)it, a, (a > 0.0) ? 1e9 / a : 0.0);
    }

    /* ---------------- 6) 日志 ---------------- */
    printf("\n### 6) 日志写入（应尽可能便宜）\n\n");
    HDR();
    {
        uint32_t it;
        double   a;

        (void)gw_log_init(&LOG, 0, GW_LOG_DEBUG);
        a = bench_one(f_log_write, &it);
        printf("| %-32s | %10u | %12.1f | %14.0f |\n",
               "写一条(DEBUG 通过)", (unsigned)it, a, (a > 0.0) ? 1e9 / a : 0.0);
        (void)gw_log_init(&LOG, 0, GW_LOG_ERROR);
        a = bench_one(f_log_filtered, &it);
        printf("| %-32s | %10u | %12.1f | %14.0f |\n",
               "写一条(被等级过滤)", (unsigned)it, a, (a > 0.0) ? 1e9 / a : 0.0);
    }

    /* ---------------- 7) 时间统计 ---------------- */
    printf("\n### 7) 时间统计（每轮 poll 调 6 次）\n\n");
    HDR();
    {
        uint32_t it;
        double   a = bench_one(f_timing_begin, &it);

        printf("| %-32s | %10u | %12.1f | %14.0f |\n",
               "gw_timing_begin(NULL 时间源)", (unsigned)it, a,
               (a > 0.0) ? 1e9 / a : 0.0);
    }

    /* ---------------- 8) CRC32 ---------------- */
    printf("\n### 8) CRC32（OTA 整包校验）\n\n");
    HDR();
    {
        uint32_t it1, it2;
        double   ns_tab = bench_one(f_crc32_128, &it1);
        double   ns_bit;

        printf("| %-32s | %10u | %12.1f | %14.0f |\n",
               "查表法 128 字节", (unsigned)it1, ns_tab,
               (ns_tab > 0.0) ? 1e9 / ns_tab : 0.0);
        ns_bit = bench_one(f_crc32_bitwise_128, &it2);
        printf("| %-32s | %10u | %12.1f | %14.0f |\n",
               "逐位法 128 字节", (unsigned)it2, ns_bit,
               (ns_bit > 0.0) ? 1e9 / ns_bit : 0.0);

        if (ns_tab > 0.0 && ns_bit > 0.0) {
            printf("\n> 查表法比逐位法快 **%.1f 倍**。\n", ns_bit / ns_tab);
            printf("> CRC32 查表法在 Cortex-M3 上约 10~15 周期/字节（含 Flash 取指等待），\n");
            printf("> 即 136 KB 整包校验约 **%.0f ms**（按 12 周期/字节估）。\n",
                   139264.0 * 12.0 / 72.0 / 1000.0);
            printf("> ⚠️ 这个换算是**估算**，不是实测 —— 上板后请用 DWT 校准。\n");
        }
    }

    printf("\n==============================================================\n");
    printf(" 怎么读这张表\n");
    printf("==============================================================\n");
    printf("\n");
    printf("1. **绝对耗时不能直接搬到 STM32 上。** PC 是 3GHz+ 乱序执行、\n");
    printf("   有大缓存；Cortex-M3 是 72MHz 顺序执行、Flash 取指有等待周期。\n");
    printf("\n");
    printf("2. **可以跨平台比较的是算法特征**：\n");
    printf("   - 每字节耗时（CRC 这类逐位算法）\n");
    printf("   - 是 O(1) 还是 O(n)（例如 CAN 收帧是常数时间）\n");
    printf("\n");
    printf("3. **本工具最重要的用途是回归基线。** 改动前后各跑一次：\n");
    printf("   如果某个操作变慢了一个量级，说明引入了性能退化 ——\n");
    printf("   这比\"绝对数字好看\"重要得多。\n");
    printf("\n");
    printf("4. 要写进报告的 STM32 数据必须上板用 DWT 实测：\n");
    printf("   测量方案与记录表格见 `doc/量化实测与测试报告.md`。\n");
    printf("\n");
    printf("blackhole = 0x%08X（防止优化器删除被测代码）\n",
           (unsigned)g_blackhole);

    return 0;
}
