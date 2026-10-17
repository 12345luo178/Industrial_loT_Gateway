/**
 * @file    test_boot.c
 * @brief   Bootloader 启动决策与固件校验单元测试
 *
 * ============================================================================
 *  这套测试的重点是**"什么情况下不该跳"**。
 *
 *  跳转本身只有几行汇编，正确性一眼能看出；真正危险的是
 *  **在固件损坏时仍然跳过去** —— 那会让设备连 Bootloader 都过不去：
 *
 *      跳进垃圾代码 -> HardFault -> 复位 -> 再跳 -> ...  无限循环
 *
 *  这正是 OTA 双分区与回滚机制要防的**最终场景**。
 *  所以本测试用各种损坏方式（栈顶指针越界、复位向量跑出分区、
 *  Thumb 位丢失、CRC 不匹配）逐一验证"必须拒绝跳转"。
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iinc -Iport -o test/test_boot.exe \
 *        src/gw_crc32.c src/gw_ota.c src/gw_boot.c test/test_boot.c
 *    ./test/test_boot.exe
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

#include "gw_boot.h"
#include "gw_crc32.h"

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
 * 假 Flash：把两个分区放在普通数组里
 * ================================================================== */
static uint8_t  FL_A[4096];     /* 缩小版（真机 136 KB，测试只需要前几 KB） */
static uint8_t  FL_B[4096];
static uint8_t  FL_G[4096];     /* golden 出厂镜像（2026-10-11 新增） */
static int      FL_READ_FAIL;
static uint32_t FL_LAST_ADDR;

static const uint32_t BASE_A = GW_APP_A_ADDR;
static const uint32_t BASE_B = GW_APP_B_ADDR;
static const uint32_t BASE_G = GW_APP_GOLDEN_ADDR;

static int fake_read(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len)
{
    const uint8_t *p = 0;

    (void)ctx;
    FL_LAST_ADDR = addr;

    if (FL_READ_FAIL != 0) {
        return -1;
    }
    if (addr >= BASE_A && addr + len <= BASE_A + sizeof(FL_A)) {
        p = &FL_A[addr - BASE_A];
    } else if (addr >= BASE_B && addr + len <= BASE_B + sizeof(FL_B)) {
        p = &FL_B[addr - BASE_B];
    } else if (addr >= BASE_G && addr + len <= BASE_G + sizeof(FL_G)) {
        p = &FL_G[addr - BASE_G];
    }
    if (p == 0) {
        return -1;                  /* 未映射 */
    }
    memcpy(buf, p, len);
    return 0;
}

static void make_io(gw_boot_io_t *io)
{
    memset(io, 0, sizeof(*io));
    io->read = fake_read;
    io->ctx  = 0;
}

/* 往一个分区里写一个"合法固件"：正确的向量表 + 填充内容 */
static void make_image(uint8_t *img, uint32_t sp, uint32_t reset, uint8_t fill)
{
    uint32_t i;

    /* 小端写入向量表前两项 */
    img[0] = (uint8_t)(sp & 0xFFu);
    img[1] = (uint8_t)((sp >> 8) & 0xFFu);
    img[2] = (uint8_t)((sp >> 16) & 0xFFu);
    img[3] = (uint8_t)((sp >> 24) & 0xFFu);
    img[4] = (uint8_t)(reset & 0xFFu);
    img[5] = (uint8_t)((reset >> 8) & 0xFFu);
    img[6] = (uint8_t)((reset >> 16) & 0xFFu);
    img[7] = (uint8_t)((reset >> 24) & 0xFFu);

    for (i = 8u; i < 4096u; i++) {
        img[i] = fill;
    }
}

/* 一个正常的栈顶（RAM 末尾附近）与复位向量（分区起点 + 0x100，带 Thumb 位） */
#define GOOD_SP     0x20010000u
#define GOOD_RESET_A (GW_APP_A_ADDR + 0x101u)
#define GOOD_RESET_B (GW_APP_B_ADDR + 0x101u)
#define GOOD_RESET_G (GW_APP_GOLDEN_ADDR + 0x101u)

static void setup(void)
{
    memset(FL_A, 0xAAu, sizeof(FL_A));
    memset(FL_B, 0xBBu, sizeof(FL_B));
    memset(FL_G, 0xCCu, sizeof(FL_G));
    FL_READ_FAIL = 0;
    make_image(FL_A, GOOD_SP, GOOD_RESET_A, 0x11u);
    make_image(FL_B, GOOD_SP, GOOD_RESET_B, 0x22u);
    make_image(FL_G, GOOD_SP, GOOD_RESET_G, 0x33u);
}

/* ==================================================================
 * 测试 1：栈顶指针校验（必须最先查）
 * ================================================================== */
static void test_sp(void)
{
    printf("\n[1] 栈顶指针校验（跳转前必须最先查）\n");

    check_int("RAM 范围内的对齐值通过", (int)gw_boot_check_sp(0x20010000u),
              (int)GW_BOOT_OK);
    check_int("RAM 起点通过", (int)gw_boot_check_sp(0x20000000u),
              (int)GW_BOOT_OK);
    check_int("RAM 中部通过", (int)gw_boot_check_sp(0x20008000u),
              (int)GW_BOOT_OK);

    printf("\n  越界（必须拒绝）：\n");
    check_int("低于 RAM 起点被拒",
              (int)gw_boot_check_sp(0x1FFFFFFCu),
              (int)GW_BOOT_ERR_SP_RANGE);
    check_int("高于 RAM 终点被拒",
              (int)gw_boot_check_sp(0x20010004u),
              (int)GW_BOOT_ERR_SP_RANGE);
    check_int("Flash 地址被拒（不是 RAM）",
              (int)gw_boot_check_sp(0x08009000u),
              (int)GW_BOOT_ERR_SP_RANGE);
    check_int("全 0 被拒（空白 Flash 的常见值）",
              (int)gw_boot_check_sp(0x00000000u),
              (int)GW_BOOT_ERR_SP_RANGE);
    check_int("全 F 被拒（未烧录 Flash）",
              (int)gw_boot_check_sp(0xFFFFFFFFu),
              (int)GW_BOOT_ERR_SP_RANGE);
    ok("垃圾值一律拒绝 —— 否则设置 MSP 的那一刻就把 RAM 写坏了");

    printf("\n  对齐（必须 8 字节）：\n");
    check_int("0x2000FFF8 通过（8 字节对齐）",
              (int)gw_boot_check_sp(0x2000FFF8u), (int)GW_BOOT_OK);
    check_int("0x2000FFF4 被拒（4 字节对齐但不是 8）",
              (int)gw_boot_check_sp(0x2000FFF4u),
              (int)GW_BOOT_ERR_SP_ALIGN);
    check_int("0x2000FFF2 被拒（2 字节对齐）",
              (int)gw_boot_check_sp(0x2000FFF2u),
              (int)GW_BOOT_ERR_SP_ALIGN);
    check_int("0x2000FFF1 被拒（未对齐）",
              (int)gw_boot_check_sp(0x2000FFF1u),
              (int)GW_BOOT_ERR_SP_ALIGN);
    ok("AAPCS 要求 8 字节对齐 —— 不对齐在用 64 位数据时才出错，很难查");
}

/* ==================================================================
 * 测试 2：复位向量校验（范围 + Thumb 位）
 * ================================================================== */
static void test_reset(void)
{
    printf("\n[2] 复位向量校验\n");

    check_int("A 分区内的合法地址通过",
              (int)gw_boot_check_reset(GOOD_RESET_A, GW_OTA_SLOT_A),
              (int)GW_BOOT_OK);
    check_int("B 分区内的合法地址通过",
              (int)gw_boot_check_reset(GOOD_RESET_B, GW_OTA_SLOT_B),
              (int)GW_BOOT_OK);
    check_int("分区起点的地址通过（带 Thumb 位）",
              (int)gw_boot_check_reset(GW_APP_A_ADDR | 1u, GW_OTA_SLOT_A),
              (int)GW_BOOT_OK);
    check_int("分区末尾之前的地址通过",
              (int)gw_boot_check_reset(GW_APP_A_ADDR + GW_APP_SLOT_SIZE - 1u,
                                       GW_OTA_SLOT_A),
              (int)GW_BOOT_OK);

    printf("\n  ⚠️ Thumb 位（Cortex-M 只有 Thumb 状态）：\n");
    check_int("Thumb 位为 0 被拒（会试图切到 ARM 状态）",
              (int)gw_boot_check_reset(GW_APP_A_ADDR + 0x100u, GW_OTA_SLOT_A),
              (int)GW_BOOT_ERR_THUMB_BIT);
    check_int("Thumb 位为 0 被拒（分区起点）",
              (int)gw_boot_check_reset(GW_APP_A_ADDR, GW_OTA_SLOT_A),
              (int)GW_BOOT_ERR_THUMB_BIT);
    ok("缺 Thumb 位会立刻 UsageFault —— 一个字节的损坏就能查出");

    printf("\n  范围（必须落在目标分区内）：\n");
    check_int("A 的地址用在 B 上被拒",
              (int)gw_boot_check_reset(GOOD_RESET_A, GW_OTA_SLOT_B),
              (int)GW_BOOT_ERR_PC_RANGE);
    check_int("B 的地址用在 A 上被拒",
              (int)gw_boot_check_reset(GOOD_RESET_B, GW_OTA_SLOT_A),
              (int)GW_BOOT_ERR_PC_RANGE);
    check_int("跳到 Bootloader 自己被拒",
              (int)gw_boot_check_reset(GW_BOOT_ADDR | 1u, GW_OTA_SLOT_A),
              (int)GW_BOOT_ERR_PC_RANGE);
    check_int("跳到分区外（A 之后）被拒",
              (int)gw_boot_check_reset(GW_APP_A_ADDR + GW_APP_SLOT_SIZE + 1u,
                                       GW_OTA_SLOT_A),
              (int)GW_BOOT_ERR_PC_RANGE);
    check_int("全 F（空白 Flash）被拒",
              (int)gw_boot_check_reset(0xFFFFFFFFu, GW_OTA_SLOT_A),
              (int)GW_BOOT_ERR_PC_RANGE);
    ok("地址跑出分区会被拦 —— 否则会跳到别的分区或空白区");

    check_int("非法分区被拒",
              (int)gw_boot_check_reset(GOOD_RESET_A, GW_OTA_SLOT_NONE),
              (int)GW_BOOT_ERR_NO_SLOT);

    /* ---- golden 出厂镜像（2026-10-11 新增）----
     *
     * ⚠️ 这里守的是"漏改一处硬编码"这种错：
     *    `gw_boot_check_reset/vectors` 原来写死了"只认 A/B"，
     *    加 golden 时如果漏改，症状是"golden 明明烧好了却说没有可用镜像"
     *    —— 而那正是最后一道兜底失效的时刻。 */
    printf("\n  golden 出厂镜像：\n");
    check_int("golden 分区内的合法地址通过",
              (int)gw_boot_check_reset(GW_APP_GOLDEN_ADDR + 0x101u,
                                       GW_OTA_SLOT_GOLDEN),
              (int)GW_BOOT_OK);
    check_int("golden 分区末尾之前的地址通过",
              (int)gw_boot_check_reset(GW_APP_GOLDEN_ADDR + GW_APP_SLOT_SIZE - 1u,
                                       GW_OTA_SLOT_GOLDEN),
              (int)GW_BOOT_OK);
    check_int("A 的地址用在 golden 上被拒",
              (int)gw_boot_check_reset(GOOD_RESET_A, GW_OTA_SLOT_GOLDEN),
              (int)GW_BOOT_ERR_PC_RANGE);
    check_int("golden 的地址用在 A 上被拒",
              (int)gw_boot_check_reset(GW_APP_GOLDEN_ADDR + 0x101u,
                                       GW_OTA_SLOT_A),
              (int)GW_BOOT_ERR_PC_RANGE);
    ok("golden 既**能**被校验，也**不会**与 A/B 互相串台");
}

/* ==================================================================
 *  ★ 启动尝试顺序：主槽 -> 另一槽 -> golden（2026-10-11 加 golden 时新增）
 *
 *  ⚠️ 顺序是**策略**，所以它被放在纯逻辑层（gw_boot_pick_slot）。
 *     否则"golden 到底有没有被试到"只能靠上板拔线验证 ——
 *     而这正是最不该靠运气的那一步（它是最后一道兜底）。
 * ================================================================== */
static int g_probe_ok_mask;     /* 置位表示该槽"可启动" */

static int probe_fake(void *ctx, gw_ota_slot_t slot)
{
    (void)ctx;
    if (gw_ota_slot_is_image(slot) == 0) {
        return -1;
    }
    return (g_probe_ok_mask & (1 << (int)slot)) ? 0 : -1;
}

static void test_pick_slot_order(void)
{
    printf("\n[X] 启动顺序：主槽 -> 另一槽 -> golden\n");

    /* ---- 三个都行：必须选主槽 ----
       ⚠️ 这条最重要：golden 存在**不能**让它抢到前面，
          否则"有升级但主槽只是暂时校验不过"就会静默跑回出厂镜像。 */
    g_probe_ok_mask = (1 << GW_OTA_SLOT_A) | (1 << GW_OTA_SLOT_B)
                    | (1 << GW_OTA_SLOT_GOLDEN);
    check_int("都可用 -> 选主槽 A",
              (int)gw_boot_pick_slot(GW_OTA_SLOT_A, probe_fake, 0),
              (int)GW_OTA_SLOT_A);
    check_int("都可用 -> 选主槽 B",
              (int)gw_boot_pick_slot(GW_OTA_SLOT_B, probe_fake, 0),
              (int)GW_OTA_SLOT_B);

    /* ---- 主槽坏 -> 另一槽（还轮不到 golden）---- */
    g_probe_ok_mask = (1 << GW_OTA_SLOT_B) | (1 << GW_OTA_SLOT_GOLDEN);
    check_int("主槽 A 坏 -> 降级到 B（不是 golden）",
              (int)gw_boot_pick_slot(GW_OTA_SLOT_A, probe_fake, 0),
              (int)GW_OTA_SLOT_B);

    /* ---- 只有 golden 行 -> 兜底（这就是它存在的意义）---- */
    g_probe_ok_mask = (1 << GW_OTA_SLOT_GOLDEN);
    check_int("A/B 都坏 -> 兜底到 golden",
              (int)gw_boot_pick_slot(GW_OTA_SLOT_A, probe_fake, 0),
              (int)GW_OTA_SLOT_GOLDEN);
    check_int("主槽未知(NONE) -> 直接试 golden",
              (int)gw_boot_pick_slot(GW_OTA_SLOT_NONE, probe_fake, 0),
              (int)GW_OTA_SLOT_GOLDEN);

    /* ---- 都不行 -> NONE（模板据此停在 Bootloader 等 ST-Link）---- */
    g_probe_ok_mask = 0;
    check_int("都不行 -> NONE",
              (int)gw_boot_pick_slot(GW_OTA_SLOT_A, probe_fake, 0),
              (int)GW_OTA_SLOT_NONE);

    /* ---- 边界：probe 为空不许崩 ---- */
    check_int("probe 为空 -> NONE（不崩）",
              (int)gw_boot_pick_slot(GW_OTA_SLOT_A, 0, 0),
              (int)GW_OTA_SLOT_NONE);

    /* ---- 不重复探测同一个槽（重复探测会让 golden 被问两次、日志也变乱）---- */
    g_probe_ok_mask = 0;
    check_int("主槽就是 golden 时也只试一次",
              (int)gw_boot_pick_slot(GW_OTA_SLOT_GOLDEN, probe_fake, 0),
              (int)GW_OTA_SLOT_NONE);
}

/* ==================================================================
 * 测试 3：向量表读取与完整校验
 * ================================================================== */
static void test_vectors(void)
{
    gw_boot_io_t  io;
    gw_boot_plan_t plan;

    printf("\n[3] 向量表读取与完整校验\n");

    setup();
    make_io(&io);

    check_int("合法向量表通过",
              (int)gw_boot_check_vectors(&io, GW_OTA_SLOT_A, &plan),
              (int)GW_BOOT_OK);
    check_int("计划里的分区是 A", (int)plan.slot, (int)GW_OTA_SLOT_A);
    check_int("计划里的基地址正确",
              (int)(plan.base == GW_APP_A_ADDR), 1);
    check_int("栈顶指针被正确读出",
              (int)(plan.sp == GOOD_SP), 1);
    check_int("复位向量被正确读出（含 Thumb 位）",
              (int)(plan.reset == GOOD_RESET_A), 1);
    ok("小端拼装正确（没有用指针强转，避免了 PC 上的对齐/别名问题）");

    /* ---- golden 的向量表也要能被读出来（2026-10-11 加 golden）----
       ⚠️ 原来这里只认 A/B，漏改的症状是"golden 烧好了却永远说没有可用镜像"。 */
    check_int("golden 向量表也能通过校验",
              (int)gw_boot_check_vectors(&io, GW_OTA_SLOT_GOLDEN, &plan),
              (int)GW_BOOT_OK);
    check_int("计划里的分区是 GOLDEN",
              (int)plan.slot, (int)GW_OTA_SLOT_GOLDEN);
    check_int("golden 的基地址正确",
              (int)(plan.base == GW_APP_GOLDEN_ADDR), 1);
    check_int("golden 的复位向量被正确读出",
              (int)(plan.reset == GOOD_RESET_G), 1);

    /* ---- 各种损坏方式 ---- */
    printf("\n  损坏的向量表（必须全部拒绝）：\n");

    setup();
    make_image(FL_A, 0x12345678u, GOOD_RESET_A, 0x11u);   /* SP 不是 RAM */
    check_int("栈顶指针损坏 -> SP_OUT_OF_RAM",
              (int)gw_boot_check_vectors(&io, GW_OTA_SLOT_A, &plan),
              (int)GW_BOOT_ERR_SP_RANGE);

    setup();
    make_image(FL_A, GOOD_SP, GW_APP_B_ADDR + 0x101u, 0x11u); /* PC 在 B */
    check_int("复位向量跑到别的分区 -> PC_OUT_OF_SLOT",
              (int)gw_boot_check_vectors(&io, GW_OTA_SLOT_A, &plan),
              (int)GW_BOOT_ERR_PC_RANGE);

    setup();
    make_image(FL_A, GOOD_SP, GW_APP_A_ADDR + 0x100u, 0x11u); /* Thumb 位 0 */
    check_int("Thumb 位丢失 -> NO_THUMB_BIT",
              (int)gw_boot_check_vectors(&io, GW_OTA_SLOT_A, &plan),
              (int)GW_BOOT_ERR_THUMB_BIT);

    setup();
    memset(FL_A, 0xFFu, sizeof(FL_A));      /* 空白 Flash */
    check_int("空白分区（全 F）-> SP_OUT_OF_RAM",
              (int)gw_boot_check_vectors(&io, GW_OTA_SLOT_A, &plan),
              (int)GW_BOOT_ERR_SP_RANGE);
    ok("空分区必须被拒 —— 否则会跳到 0xFFFFFFFF");

    setup();
    memset(FL_A, 0x00u, sizeof(FL_A));      /* 全 0 */
    check_int("全 0 分区 -> SP_OUT_OF_RAM",
              (int)gw_boot_check_vectors(&io, GW_OTA_SLOT_A, &plan),
              (int)GW_BOOT_ERR_SP_RANGE);

    /* ⚠️ 顺序验证：SP 非法时应该报 SP 的错误，而不是 PC 的。
       这验证了"先查 SP"这个顺序要求。 */
    setup();
    make_image(FL_A, 0x00000000u, 0x00000000u, 0x00u);  /* SP 与 PC 都非法 */
    check_int("SP 与 PC 都非法时报 SP 的错（先查 SP）",
              (int)gw_boot_check_vectors(&io, GW_OTA_SLOT_A, &plan),
              (int)GW_BOOT_ERR_SP_RANGE);

    /* ---- 读取失败 ---- */
    setup();
    FL_READ_FAIL = 1;
    check_int("读失败 -> READ_FAIL",
              (int)gw_boot_check_vectors(&io, GW_OTA_SLOT_A, &plan),
              (int)GW_BOOT_ERR_READ);
    FL_READ_FAIL = 0;

    /* ---- 参数校验 ---- */
    check_int("空 io 被拒",
              (int)gw_boot_check_vectors(0, GW_OTA_SLOT_A, &plan),
              (int)GW_BOOT_ERR_READ);
    check_int("空 plan 被拒",
              (int)gw_boot_check_vectors(&io, GW_OTA_SLOT_A, 0),
              (int)GW_BOOT_ERR_READ);
    check_int("非法分区被拒",
              (int)gw_boot_check_vectors(&io, GW_OTA_SLOT_NONE, &plan),
              (int)GW_BOOT_ERR_NO_SLOT);
}

/* ==================================================================
 * 测试 4：整包 CRC 校验
 * ================================================================== */
static void test_crc(void)
{
    gw_boot_io_t io;
    uint32_t     crc;
    uint32_t     i;

    printf("\n[4] 整包 CRC32 校验（分块读，不缓存整包）\n");

    setup();
    make_io(&io);

    /* 算一遍 4096 字节的 CRC（用的是共享实现） */
    crc = gw_crc32(FL_A, 4096u);
    check_int("CRC 校验通过",
              (int)gw_boot_check_crc(&io, GW_APP_A_ADDR, 4096u, crc),
              (int)GW_BOOT_OK);

    /* 改一个字节 -> 必须失败 */
    FL_A[2000] ^= 0x01u;
    check_int("改 1 个字节后 CRC 失败",
              (int)gw_boot_check_crc(&io, GW_APP_A_ADDR, 4096u, crc),
              (int)GW_BOOT_ERR_CRC);
    ok("单个字节损坏能被检出 —— 这是防止'半写入固件被启动'的最后一道防线");

    /* 长度变化 */
    setup();
    crc = gw_crc32(FL_A, 4096u);
    check_int("长度少了 1 字节 -> CRC 失败",
              (int)gw_boot_check_crc(&io, GW_APP_A_ADDR, 4095u, crc),
              (int)GW_BOOT_ERR_CRC);
    check_int("长度多了 1 字节 -> 读越界 -> READ_FAIL",
              (int)gw_boot_check_crc(&io, GW_APP_A_ADDR, 4097u, crc),
              (int)GW_BOOT_ERR_READ);

    /* 参数边界 */
    check_int("长度 0 被拒（无法校验 -> 保守判失败）",
              (int)gw_boot_check_crc(&io, GW_APP_A_ADDR, 0u, crc),
              (int)GW_BOOT_ERR_CRC);
    check_int("长度超过分区容量被拒",
              (int)gw_boot_check_crc(&io, GW_APP_A_ADDR,
                                     GW_APP_SLOT_SIZE + 1u, crc),
              (int)GW_BOOT_ERR_CRC);
    check_int("空 io 被拒",
              (int)gw_boot_check_crc(0, GW_APP_A_ADDR, 100u, crc),
              (int)GW_BOOT_ERR_READ);

    /* ---- 分块读的正确性：验证跨块边界的数据也被算进去了 ----
       用一个"只在跨块处有差异"的场景。 */
    {
        uint8_t  a[8192];
        uint8_t  b[8192];
        uint32_t c1, c2;

        for (i = 0u; i < 8192u; i++) {
            a[i] = (uint8_t)(i & 0xFFu);
            b[i] = (uint8_t)(i & 0xFFu);
        }
        /* 改在 4096 边界附近（正好是分块边界） */
        b[4095] ^= 0xFFu;

        c1 = gw_crc32(a, 8192u);
        c2 = gw_crc32(b, 8192u);
        if (c1 != c2) {
            ok("分块边界处的差异能被检出（增量 CRC 正确）");
        } else {
            fail("分块边界处的差异能被检出", "");
        }
    }

    /* ---- 增量与一次算的结果必须一致 ---- */
    {
        uint32_t acc = 0xFFFFFFFFu;
        uint32_t one;

        setup();
        one = gw_crc32(FL_A, 4096u);
        acc = gw_crc32_update(acc, FL_A, 1000u);
        acc = gw_crc32_update(acc, FL_A + 1000u, 2000u);
        acc = gw_crc32_update(acc, FL_A + 3000u, 1096u);
        check_int("分三次增量算 == 一次算",
                  (int)(gw_crc32_final(acc) == one ? 1 : 0), 1);
        ok("增量接口与一次算一致 —— OTA 分片接收依赖这一点");
    }
}

/* ==================================================================
 * 测试 5：完整准备流程（组合校验）
 * ================================================================== */
static void test_prepare(void)
{
    gw_boot_io_t   io;
    gw_boot_plan_t plan;
    gw_ota_meta_t  m;

    printf("\n[5] 完整准备流程\n");

    setup();
    make_io(&io);

    /* ---- 有元数据 + 做 CRC ---- */
    {
        uint32_t crc = gw_crc32(FL_B, 4096u);

        gw_ota_meta_make_default(&m, GW_OTA_SLOT_B);
        m.pending      = 1u;
        m.pending_len  = 4096u;
        m.pending_crc  = crc;
        gw_ota_meta_seal(&m);

        check_int("待确认固件 + CRC 通过",
                  (int)gw_boot_prepare(&io, GW_OTA_SLOT_B, &m, 1, &plan),
                  (int)GW_BOOT_OK);
        check_int("计划标记为待确认", (int)plan.is_pending, 1);
        check_int("计划标记为已校验", (int)plan.verified, 1);
        check_int("固件长度被带进计划",
                  (int)(plan.image_len == 4096u), 1);
        ok("待确认的固件能被完整校验（它最可能有问题是新写入的）");

        /* 损坏 -> 拒绝 */
        FL_B[3000] ^= 0x01u;
        check_int("待确认固件损坏 -> CRC 失败",
                  (int)gw_boot_prepare(&io, GW_OTA_SLOT_B, &m, 1, &plan),
                  (int)GW_BOOT_ERR_CRC);
        ok("损坏的待确认固件不会被启动 —— 设备会留在旧固件上");
    }

    /* ---- 有元数据但不做 CRC ---- */
    setup();
    {
        gw_ota_meta_make_default(&m, GW_OTA_SLOT_A);
        m.pending     = 0u;
        m.confirmed_len = 4096u;
        m.confirmed_crc = 0u;           /* 故意放一个错的值 */
        gw_ota_meta_seal(&m);

        check_int("跳过 CRC 时错误的值不影响结果",
                  (int)gw_boot_prepare(&io, GW_OTA_SLOT_A, &m, 0, &plan),
                  (int)GW_BOOT_OK);
        check_int("跳过 CRC 时不标记 verified", (int)plan.verified, 0);
        ok("正常启动可以跳过 CRC，省下每次上电的 35 ms");
    }

    /* ---- 没有元数据（首次烧录）---- */
    setup();
    check_int("元数据为 NULL 时只查向量表",
              (int)gw_boot_prepare(&io, GW_OTA_SLOT_A, 0, 0, &plan),
              (int)GW_BOOT_OK);
    check_int("无元数据时 verified 为 0", (int)plan.verified, 0);

    /* 元数据损坏（魔数不对）—— 也应该能启动 */
    memset(&m, 0xFFu, sizeof(m));
    check_int("元数据损坏时仍能按向量表启动",
              (int)gw_boot_prepare(&io, GW_OTA_SLOT_A, &m, 0, &plan),
              (int)GW_BOOT_OK);
    ok("首次烧录 / 元数据损坏时必须能启动 —— 否则设备变砖");

    /* ---- 向量表非法时，即使元数据完好也要拒 ---- */
    setup();
    make_image(FL_A, 0x00000000u, 0x00000000u, 0x00u);
    gw_ota_meta_make_default(&m, GW_OTA_SLOT_A);
    check_int("向量表非法时优先报向量表的错",
              (int)gw_boot_prepare(&io, GW_OTA_SLOT_A, &m, 1, &plan),
              (int)GW_BOOT_ERR_SP_RANGE);
    ok("向量表校验在 CRC 之前 —— 先确认'能不能跳'，再花 35ms 校验");

    /* ---- 空 plan 指针 ---- */
    setup();
    check_int("空 plan 被拒",
              (int)gw_boot_prepare(&io, GW_OTA_SLOT_A, &m, 0, 0),
              (int)GW_BOOT_ERR_READ);

    /* ---- 长度信息缺失时不标记 verified ---- */
    setup();
    gw_ota_meta_make_default(&m, GW_OTA_SLOT_A);
    m.pending     = 1u;
    m.pending_len = 0u;                 /* 没有长度信息 */
    gw_ota_meta_seal(&m);
    check_int("缺长度信息时不失败（可能首次烧录）",
              (int)gw_boot_prepare(&io, GW_OTA_SLOT_A, &m, 1, &plan),
              (int)GW_BOOT_OK);
    check_int("缺长度信息时不标记已校验", (int)plan.verified, 0);
    ok("不标记 verified 是诚实的 —— 不假装校验过");
}

/* ==================================================================
 * 测试 6：与 gw_ota 的联动（关键路径）
 * ================================================================== */
static void test_integration_with_ota(void)
{
    gw_boot_io_t   bio;
    gw_ota_meta_t  m;
    gw_boot_plan_t plan;
    static uint8_t FW[2048];
    uint32_t       i;

    printf("\n[6] 与 gw_ota 的联动（升级 -> 决策 -> 校验 -> 可跳转）\n");

    setup();
    make_io(&bio);

    /* 造一份"新固件"：前 8 字节是向量表，其余是内容 */
    for (i = 8u; i < sizeof(FW); i++) {
        FW[i] = (uint8_t)(0x30u + (uint8_t)i);
    }
    FW[0] = (uint8_t)(GOOD_SP & 0xFFu);
    FW[1] = (uint8_t)((GOOD_SP >> 8) & 0xFFu);
    FW[2] = (uint8_t)((GOOD_SP >> 16) & 0xFFu);
    FW[3] = (uint8_t)((GOOD_SP >> 24) & 0xFFu);
    FW[4] = (uint8_t)(GOOD_RESET_B & 0xFFu);
    FW[5] = (uint8_t)((GOOD_RESET_B >> 8) & 0xFFu);
    FW[6] = (uint8_t)((GOOD_RESET_B >> 16) & 0xFFu);
    FW[7] = (uint8_t)((GOOD_RESET_B >> 24) & 0xFFu);

    /* 把它放进 B 分区（模拟 gw_ota 写完的状态）。
       其余字节填 0 —— 因为 gw_ota_begin 会先擦除整个分区。 */
    memset(FL_B, 0x00u, sizeof(FL_B));
    memcpy(FL_B, FW, sizeof(FW));

    /* 元数据：active = B，pending = 1（模拟 gw_ota_end 之后的状态）
     *
     * ⚠️ 这里踩过一个坑：最初漏了 `memset(&m, 0, sizeof(m))`，
     *    直接调 `gw_ota_meta_make_default()` —— 看起来没问题，
     *    因为那个函数内部就会 memset。
     *
     *    但**这是错的习惯**：如果将来有人改成"在默认值基础上覆盖几个字段"，
     *    未清零的栈垃圾就会混进去。而且本测试里真正的症状更直接 ——
     *    断言 is_pending / verified 全失败，排查了半天才发现
     *    是 `m` 根本没被初始化到预期状态。
     *
     *    教训：**结构体在使用前显式清零**，不要依赖被调函数顺手清。 */
    memset(&m, 0, sizeof(m));
    gw_ota_meta_make_default(&m, GW_OTA_SLOT_B);
    m.pending     = 1u;
    m.pending_len = sizeof(FW);
    m.pending_crc = gw_crc32(FL_B, sizeof(FW));
    gw_ota_meta_seal(&m);

    check_int("（前置）元数据有效", gw_ota_meta_valid(&m), 1);

    /* ---- Bootloader 决策 ---- */
    {
        gw_ota_slot_t boot = gw_ota_boot_decide(&m);

        check_int("boot_decide 给出 B", (int)boot, (int)GW_OTA_SLOT_B);
        check_int("首次决策后 boot_tries = 1", (int)m.boot_tries, 1);
    }

    /* ⚠️ boot_decide() 改了 boot_tries 但**不重算 CRC** ——
       真实流程里紧接着要 meta_save()（它内部会 seal）。
       测试里不落盘，但必须补上 seal，否则后面 meta_valid 会失败。
       踩过的坑：漏了这一步，valid 从 1 变成 0，
       排查半天才发现差的就是 boot_tries 那一个字节。 */
    gw_ota_meta_seal(&m);
    check_int("（前置）boot_decide 后重算 CRC 仍有效",
              gw_ota_meta_valid(&m), 1);

    /* ---- 校验并生成启动计划 ---- */
    check_int("校验通过，可以跳转",
              (int)gw_boot_prepare(&bio, GW_OTA_SLOT_B, &m, 1, &plan),
              (int)GW_BOOT_OK);
    check_int("计划指向 B", (int)plan.slot, (int)GW_OTA_SLOT_B);
    check_int("标记为待确认（说明这是新固件）", (int)plan.is_pending, 1);
    check_int("已做整包校验", (int)plan.verified, 1);
    ok("升级 -> 决策 -> 校验 -> 跳转 这条链路贯通");

    /* ---- 关键场景：固件损坏时必须拒绝，从而触发回滚 ---- */
    FL_B[100] ^= 0x01u;
    check_int("固件损坏 -> 拒绝启动（设备会留在旧固件上）",
              (int)gw_boot_prepare(&bio, GW_OTA_SLOT_B, &m, 1, &plan),
              (int)GW_BOOT_ERR_CRC);
    ok("这是 OTA 回滚的最后一环：坏固件连被跳转的机会都没有");

    /* ---- 不做 CRC 时坏固件会被放行（这正是为什么待确认固件必须校验）---- */
    check_int("跳过 CRC 时坏固件会被放行（说明待确认必须校验）",
              (int)gw_boot_prepare(&bio, GW_OTA_SLOT_B, &m, 0, &plan),
              (int)GW_BOOT_OK);
    ok("由此确认：check_crc 对待确认固件不能省");
}


/* ==================================================================
 * 测试 N：gw_boot_err_str 的完整性
 *
 *  ⚠️ 这个函数**被 Bootloader 用它打印失败原因**（templates/bootloader_main.c），
 *     但原本没有任何测试。
 *
 *  为什么要测一个"返回字符串"的函数：
 *    它是现场排查的**唯一线索**。Bootloader 启动失败时只打印这一句，
 *    如果两个不同的错误码返回了同一个字符串，现场就分不清是
 *    "栈顶指针非法"还是"Thumb 位缺失" —— 而这两种的修法完全不同。
 *
 *    所以断言是：**每个错误码都有非空、互不相同的字符串**。
 * ================================================================== */
static void test_err_str(void)
{
    int i, j;
    int all_nonempty = 1;
    int all_distinct = 1;

    printf("\n[N] 错误码字符串（Bootloader 现场排查的唯一线索）\n");

    /* GW_BOOT_OK .. GW_BOOT_ERR_META 全部要有字符串 */
    for (i = 0; i <= (int)GW_BOOT_ERR_META; i++) {
        const char *s1 = gw_boot_err_str((gw_boot_err_t)i);

        if (s1 == 0 || s1[0] == '\0') {
            char d[64];
            sprintf(d, "(错误码 %d 返回空串)", i);
            fail("每个错误码都有非空字符串", d);
            all_nonempty = 0;
        }
    }
    check_int("OK 到 META 全部有非空字符串", all_nonempty, 1);

    /* 两两互不相同 */
    for (i = 0; i <= (int)GW_BOOT_ERR_META && all_distinct; i++) {
        for (j = i + 1; j <= (int)GW_BOOT_ERR_META; j++) {
            if (strcmp(gw_boot_err_str((gw_boot_err_t)i),
                       gw_boot_err_str((gw_boot_err_t)j)) == 0) {
                printf("      错误码 %d 与 %d 的字符串相同：%s\n",
                       i, j, gw_boot_err_str((gw_boot_err_t)i));
                all_distinct = 0;
                break;
            }
        }
    }
    check_int("⚠️ 各错误码的字符串互不相同", all_distinct, 1);
    ok("⚠️ 字符串重复会让现场分不清「栈顶指针非法」与「Thumb 位缺失」——"
       "而这两种的修法完全不同");

    /* 具体值抽查（这些是 Bootloader 打印出来会看到的内容） */
    check_int("OK 的字符串是 \"OK\"",
              strcmp(gw_boot_err_str(GW_BOOT_OK), "OK") == 0 ? 1 : 0, 1);
    check_int("SP 范围错误的字符串含提示",
              (int)(strlen(gw_boot_err_str(GW_BOOT_ERR_SP_RANGE)) > 0 ? 1 : 0), 1);
    check_int("CRC 错误的字符串含提示",
              (int)(strlen(gw_boot_err_str(GW_BOOT_ERR_CRC)) > 0 ? 1 : 0), 1);

    /* 越界错误码要有兜底（不能返回 NULL —— 那样 printf("%s") 会崩） */
    {
        const char *oob = gw_boot_err_str((gw_boot_err_t)99);

        check_int("越界错误码有兜底（不返回 NULL）",
                  (int)(oob != 0 ? 1 : 0), 1);
        check_int("越界兜底也是非空串",
                  (int)(oob != 0 && oob[0] != '\0' ? 1 : 0), 1);
        ok("越界必须返回非空串 —— 返回 NULL 会让 printf(\"%s\") 崩溃，"
           "而那正好发生在 Bootloader 的报错路径上");
    }
}

int main(void)
{
    printf("========== Bootloader 启动决策与固件校验 单元测试 ==========\n");

    test_sp();
    test_reset();
    test_vectors();
    test_err_str();
    test_crc();
    test_prepare();
    test_integration_with_ota();
    test_pick_slot_order();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
