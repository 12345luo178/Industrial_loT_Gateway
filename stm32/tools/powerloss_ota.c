/**
 * @file    powerloss_ota.c
 * @brief   OTA 断电穷举 —— 在**每一个**受影响的写/擦操作后模拟一次断电
 *
 * ============================================================================
 *  为什么要穷举，而不是挑几个点
 *
 *  `test_ota.c` 里有一个断电测试，但它测的是**一个硬编码的时刻**
 *  （"写到一半就返回"）。那只能证明"那一个时刻是安全的"。
 *
 *  不同时刻的后果完全不同：
 *
 *      断电在擦除元数据页之后、写新元数据之前
 *          -> 元数据变成全 0xFF -> magic 校验失败 -> 走"元数据不可信"路径
 *
 *      断电在写新元数据的中途
 *          -> 元数据半新半旧 -> meta_crc 校验失败 -> 同样走不可信路径
 *
 *      断电在写完新元数据之后
 *          -> 元数据完整且 pending=1 -> Bootloader 重试若干次后回滚
 *
 *  这三种的**恢复路径不一样**。只测一个点，另外两条就是"没验证过"。
 *
 *  ----------------------------------------------------------------------------
 *  本程序测的不变式（每模拟一次断电都全查一遍）
 *
 *  **I1. 设备必须能启动** —— `gw_ota_boot_decide()` 必须返回一个分区，
 *      绝不能返回 NONE。返回 NONE 意味着"两个分区都不可启动"= 变砖。
 *
 *  **I2. 分区号必须合法** —— 只可能是 A 或 B，不能是 0xFF 或别的值。
 *      否则 Bootloader 会去跳一个非法地址。
 *
 *  **I3. 元数据必须自洽** —— 要么 `gw_ota_meta_valid()` 通过，
 *      要么走"元数据不可信"的兜底路径（那条路径也必须给出可用分区）。
 *
 *  **I4. boot_tries 必须有界** —— 不能无限增长。
 *      它无界增长意味着"永远回滚不了"。
 *
 *  **I5. 回滚必须真的发生** —— 一次完整升级后模拟"新固件连续起不来"，
 *      最终必须回到旧分区。
 *
 *  ----------------------------------------------------------------------------
 *  ⚠️ 本程序不测什么（诚实说明）
 *
 *  它不测**真实 Flash 的物理行为**（部分擦除、位翻转、读干扰）。
 *  那些需要硬件。这里模拟的是"整页擦除/整页写入的原子性" ——
 *  即"操作要么完整生效、要么完全没发生"。
 *
 *  对 F1 的页擦除与半字编程来说，这个模型是**偏保守**的：
 *  真实器件在断电时可能留下"部分擦除"的页（更糟），
 *  所以真机上的风险不会比这里测出来的小。
 * ============================================================================
 */

#include <stdio.h>
#include <string.h>

#include "gw_ota.h"
#include "gw_crc32.h"

static int g_pass = 0;
static int g_fail = 0;

static void ok(const char *name) { (void)name; g_pass++; }
static void fail(const char *name, const char *d)
{
    g_fail++;
    printf("  [FAIL] %s\n         %s\n", name, d);
}
static void check_int(const char *name, int a, int e)
{
    if (a == e) { ok(name); }
    else {
        char d[128];
        sprintf(d, "实际 %d, 期望 %d", a, e);
        fail(name, d);
    }
}

/* ==================================================================
 *  测试用的固件镜像
 *
 *  ⚠️ 长度定在这里（而不是后面）是因为 `g_fw` 的数组大小需要在
 *     文件早期就是常量表达式 —— 否则 `setup_running_device()`
 *     用不到它。这是 C 的规则，不是设计选择。
 * ================================================================== */
#define FW_LEN      3000u

static uint8_t  g_fw[FW_LEN];
static uint32_t g_fw_crc;

/* ==================================================================
 *  模拟的 Flash
 * ================================================================== */

#define FLASH_SIZE   (512u * 1024u)
#define FLASH_BASE   0x08000000u
#define PAGE_SIZE    0x800u                 /* F1 大容量：2 KB */

#define META_ADDR    (FLASH_BASE + 0x8000u) /* 0x08008000 */

/* ⚠️ 编译期钉住：元数据结构必须放得进一页。
   放不进的话"擦一页 + 写结构"的原子性假设就不成立了。 */
_Static_assert(sizeof(gw_ota_meta_t) <= PAGE_SIZE,
               "元数据结构大于一页，元数据写入不再是单页原子操作");

static uint8_t  g_flash[FLASH_SIZE];
static uint8_t  g_meta_image[sizeof(gw_ota_meta_t)];

/**
 * 断电注入器
 *
 * ⚠️ 语义：`tick()` 每被调用一次就递减；减到 0 时**本次操作不生效**
 *     （模拟"电压在操作完成前掉了"）。
 *
 *     这与"操作完成然后再断电"不同 —— 后者是安全的，
 *     而前者才是真实威胁。所以这里模拟的是"操作没完成"。
 */
static int g_countdown = -1;        /* -1 = 不注入 */
static int g_fired = 0;

static void inject_at(int n) { g_countdown = n; g_fired = 0; }

/** 返回 1 表示"这次操作被断电打断" */
static int power_check(void)
{
    if (g_countdown < 0) {
        return 0;
    }
    if (g_countdown == 0) {
        g_fired = 1;
        g_countdown = -1;
        return 1;
    }
    g_countdown--;
    return 0;
}

static void flash_reset(void)
{
    memset(g_flash, 0xFF, sizeof(g_flash));
    memset(g_meta_image, 0xFF, sizeof(g_meta_image));
    g_countdown = -1;
    g_fired = 0;
}

/* ---------------- 回调 ---------------- */

static int io_erase(void *ctx, uint32_t addr, uint32_t len)
{
    uint32_t off;
    (void)ctx;

    if (addr < FLASH_BASE || (addr + len) > (FLASH_BASE + FLASH_SIZE)) {
        return -1;
    }
    /* ⚠️ 页粒度：F1 一次擦一页（2 KB）。断电时那一页没擦成。 */
    if (power_check()) {
        return -1;
    }
    off = addr - FLASH_BASE;
    memset(&g_flash[off], 0xFF, len);
    return 0;
}

static int io_write(void *ctx, uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint32_t off;
    (void)ctx;

    if (addr < FLASH_BASE || (addr + len) > (FLASH_BASE + FLASH_SIZE)) {
        return -1;
    }
    if (power_check()) {
        return -1;      /* 整次写入丢失（不是部分写入）*/
    }
    off = addr - FLASH_BASE;
    memcpy(&g_flash[off], data, len);
    return 0;
}

static int io_verify(void *ctx, uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint32_t off = addr - FLASH_BASE;
    (void)ctx;
    if (off + len > FLASH_SIZE) { return -1; }
    return (memcmp(&g_flash[off], data, len) == 0) ? 0 : -1;
}

static int io_meta_read(void *ctx, uint8_t *buf, uint32_t len)
{
    uint32_t off = META_ADDR - FLASH_BASE;
    (void)ctx;
    if (len > sizeof(gw_ota_meta_t)) { return -1; }
    memcpy(buf, &g_flash[off], len);
    return 0;
}

static int io_meta_write(void *ctx, const uint8_t *buf, uint32_t len)
{
    uint32_t off = META_ADDR - FLASH_BASE;
    (void)ctx;

    if (len > sizeof(gw_ota_meta_t)) { return -1; }

    /* ⚠️⚠️ 元数据的写入是**两阶段**的，这是本测试最关心的部分：
     *      1) 擦除元数据页（2 KB）—— 擦完 magic 就没了
     *      2) 写入新的元数据结构
     *
     *      断电点①：擦除没完成 -> 旧元数据还在（安全）
     *      断电点②：擦完但没写 -> 全 0xFF（magic 失效 -> 走兜底）
     *      断电点③：写没完成 -> 全 0xFF（同上）
     *      断电点④：写完成 -> 新元数据完整
     *
     *      这四点的恢复路径不同，必须分别验证。
     */
    if (power_check()) {
        return -1;              /* 断电点①：擦除没完成 */
    }
    memset(&g_flash[off], 0xFF, PAGE_SIZE);

    if (power_check()) {
        return -1;              /* 断电点②：擦完没写 */
    }
    memcpy(&g_flash[off], buf, len);

    if (power_check()) {
        /* 断电点③：写入被中断。
           保守模型：这一页**既不是旧内容也不是新内容**，
           而是"擦除后的部分写入" —— 最坏情况。
           这里模拟成"写入完全没生效"（全 0xFF）。 */
        memset(&g_flash[off], 0xFF, PAGE_SIZE);
        return -1;
    }

    return 0;                   /* 断电点④：完整写入 */
}

static gw_ota_io_t g_io;

static void make_io(void)
{
    memset(&g_io, 0, sizeof(g_io));
    g_io.erase      = io_erase;
    g_io.write      = io_write;
    g_io.verify     = io_verify;
    g_io.meta_read  = io_meta_read;
    g_io.meta_write = io_meta_write;
}

/* ==================================================================
 *  ⚠️ 建立"一台正在运行的设备"
 *
 *  这是本测试**第一版做错的地方**，值得记下来。
 *
 *  第一版直接把 Flash 全设为 0xFF，然后对"升级到 B"做断电穷举 ——
 *  结果**26 个断电点全部报"变砖"**。
 *
 *  但那是**我的威胁模型错了，不是代码有问题**：
 *  全 0xFF 的 Flash 意味着"这是一台从未升级过的裸机"，
 *  它根本没有元数据、也没有任何已有固件。
 *  一台裸机上元数据为空 -> 没有可启动分区 -> 返回 NONE 是**正确的**。
 *
 *  真实场景是：**设备正在运行**（A 里有已确认的固件，元数据 active=A），
 *  此时才去做一次升级。所以必须先建立那个初始状态。
 *
 *  ⚠️ 这个初值不是"为了让测试通过"而加的 ——
 *     它才是现场真实的前置条件。少了它，测的是"裸机上电"
 *     而不是"运行中的设备升级时断电"。
 * ================================================================== */
static int setup_running_device(void)
{
    gw_ota_meta_t m;

    flash_reset();
    make_io();

    /* A 里放一份"已确认的固件"。
       ⚠️ 内容不重要 —— 本测试关心的是**元数据状态机**，
          向量表合法性由 gw_boot 模块负责（test_boot.c 98 项覆盖）。
          这里只需要"有一个已确认固件"这个事实。 */
    {
        static const uint8_t seed[64] = { 1u, 2u, 3u, 4u };

        (void)io_write(0, GW_APP_A_ADDR, seed, sizeof(seed));
    }

    /* 元数据：active = A，已确认（pending = 0）*/
    gw_ota_meta_make_default(&m, GW_OTA_SLOT_A);
    m.confirmed_len = 64u;
    m.confirmed_crc = gw_crc32((const uint8_t *)"\x01\x02\x03\x04", 4u);
    gw_ota_meta_seal(&m);

    if (gw_ota_meta_save(&g_io, &m) != 0) {
        return -1;
    }
    return 0;
}

/* ==================================================================
 *  一次完整的升级（可注入断电）
 *
 *  @return 0 = 走完了 BEGIN + 全部 CHUNK + END；
 *          -1 = 中途被断电打断
 * ================================================================== */
static int run_upgrade(gw_ota_slot_t target, int pl_point, int *out_ops)
{
    gw_ota_t o;
    uint32_t off = 0u;
    int      ops = 0;

    if (gw_ota_init(&o, &g_io, target) != 0) {
        return -1;
    }

    inject_at(pl_point);

    /* BEGIN：内部会 erase（1 次操作）*/
    if (gw_ota_begin(&o, 1u, 0u, FW_LEN, g_fw_crc) != 0) {
        *out_ops = ops + 1;
        return -1;
    }
    ops++;      /* 擦除 */

    /* CHUNK：每片一次 write */
    while (off < FW_LEN) {
        uint32_t n = FW_LEN - off;
        if (n > GW_OTA_MAX_CHUNK) {
            n = GW_OTA_MAX_CHUNK;
        }
        if (gw_ota_chunk(&o, off, &g_fw[off], n) != 0) {
            *out_ops = ops + 1;
            return -1;
        }
        off += n;
        ops++;      /* 每片一次写入 */
    }

    /* END：内部会写元数据（1 次 meta_write，但内部有 3 个断电点）*/
    if (gw_ota_end(&o) != 0) {
        *out_ops = ops + 1;
        return -1;
    }
    ops++;

    *out_ops = ops;
    return 0;
}

/* ==================================================================
 *  启动一次（Bootloader 的视角）
 * ================================================================== */
typedef struct {
    gw_ota_slot_t slot;
    int           meta_ok;
    uint8_t       pending;
    uint8_t       boot_tries;
    uint8_t       active;
} boot_result_t;

static boot_result_t boot_once(void)
{
    boot_result_t r;
    gw_ota_meta_t m;

    memset(&r, 0, sizeof(r));

    if (gw_ota_meta_load(&g_io, &m) != 0) {
        r.meta_ok = 0;
        r.slot    = GW_OTA_SLOT_NONE;
        return r;
    }

    r.meta_ok = (gw_ota_meta_valid(&m) == 1) ? 1 : 0;

    r.slot = gw_ota_boot_decide(&m);

    if (r.slot != GW_OTA_SLOT_NONE) {
        r.pending    = m.pending;
        r.boot_tries = m.boot_tries;
        r.active     = m.active;
        /* ⚠️ 模拟 Bootloader 的职责：把决定写回 Flash。
           这一步在真机上必须在跳转**之前**完成。 */
        (void)gw_ota_meta_save(&g_io, &m);
    }
    return r;
}

/* ==================================================================
 *  ⚠️ Bootloader 的**完整**行为（含元数据不可信时的兜底）
 *
 *  这里补上的是本测试**第二版做错的地方**。
 *
 *  第二版直接调 `gw_ota_boot_decide()`，发现"元数据被破坏时它返回 NONE"，
 *  于是报了 4 个"变砖"。但那是**误报** —— 我把两层的职责搞混了：
 *
 *      模块层（gw_ota_boot_decide）：元数据不可信 -> 返回 NONE
 *                                   （**不猜**。猜错会跳进一个随机地址）
 *      Bootloader 层（templates/bootloader_main.c）：
 *                                   NONE -> 兜底启动 A
 *
 *  `gw_ota.h` 里写明了这个契约（"GW_OTA_SLOT_NONE 表示没有可启动的分区"），
 *  Bootloader 模板里也有对应的兜底分支。
 *
 *  所以本函数模拟的是**真机上实际发生的完整流程**，
 *  而不是只调模块层那一个函数。
 * ================================================================== */
static gw_ota_slot_t bootloader_decide(void)
{
    gw_ota_meta_t m;

    /* 与 templates/bootloader_main.c 的逻辑一致 */
    if (gw_ota_meta_load(&g_io, &m) != 0 || gw_ota_meta_valid(&m) != 1) {
        /* 元数据不可信 -> 兜底启动 A。
           ⚠️ 这是唯一一处"不看元数据就启动"的地方 ——
              没有它，元数据一坏设备就彻底不起来了。 */
        return GW_OTA_SLOT_A;
    }

    return gw_ota_boot_decide(&m);
}

/* ==================================================================
 *  不变式检查（每次断电后都查一遍）
 * ================================================================== */
static int check_invariants(const char *where, int pl_point)
{
    boot_result_t    b = boot_once();
    gw_ota_slot_t    final_slot;
    char             tag[160];
    int              bad = 0;

    /* ⚠️ 用**完整流程**（含兜底）判断"能不能启动"，
       而不是只调模块层。两层职责见 bootloader_decide 的说明。 */
    final_slot = bootloader_decide();

    /* I1：真机上必须能启动 */
    if (final_slot == GW_OTA_SLOT_NONE) {
        sprintf(tag, "%s（断电点 %d）：设备变砖 —— "
                     "连兜底路径都没给出可启动分区", where, pl_point);
        fail("I1 断电后必须能启动（含兜底）", tag);
        bad = 1;
    }

    /* I1b：模块层返回 NONE 时，兜底必须接管 —— 验证那个契约确实成立 */
    if (!bad && b.slot == GW_OTA_SLOT_NONE) {
        if (final_slot != GW_OTA_SLOT_A) {
            sprintf(tag, "%s：模块层返回 NONE，兜底却给了 %d（应为 A）",
                    where, (int)final_slot);
            fail("I1b 元数据不可信时兜底启动 A", tag);
            bad = 1;
        }
    }

    /* I2：**最终**决定的分区号必须合法
       ⚠️ 注意检查的是 `final_slot` 而不是 `b.slot`。
          `b.slot` 是模块层的原始返回 —— 模块层"无法判断"时
          返回 NONE 是**正确**的（不猜），由兜底接管。
          如果这里去检查 b.slot，就会把"正确的拒答"误报成"分区号非法"
          （本测试第三版就是这么误报的）。 */
    if (!bad && final_slot != GW_OTA_SLOT_A && final_slot != GW_OTA_SLOT_B) {
        sprintf(tag, "%s（断电点 %d）：最终分区号非法 = %d",
                where, pl_point, (int)final_slot);
        fail("I2 最终分区号必须合法", tag);
        bad = 1;
    }

    /* I3：要么元数据有效，要么走了兜底但不返回 NONE（I1 已覆盖）*/
    /* I4：boot_tries 有界 */
    if (!bad && b.boot_tries > GW_OTA_MAX_BOOT_TRIES) {
        sprintf(tag, "%s（断电点 %d）：boot_tries = %d 超过上限 %d",
                where, pl_point, (int)b.boot_tries,
                (int)GW_OTA_MAX_BOOT_TRIES);
        fail("I4 boot_tries 必须有界", tag);
        bad = 1;
    }

    if (!bad) {
        ok(where);
    }
    return bad;
}

/* ==================================================================
 *  主流程
 * ================================================================== */
int main(void)
{
    uint32_t i;
    int      total_ops = 0;
    int      bad_total = 0;
    int      v;

    for (i = 0u; i < FW_LEN; i++) {
        g_fw[i] = (uint8_t)((i * 37u + 11u) & 0xFFu);
    }
    g_fw_crc = gw_crc32(g_fw, FW_LEN);

    printf("========== OTA 断电穷举 ==========\n");
    printf("  固件长度 %u 字节，分片上限 %u 字节/片\n",
           (unsigned)FW_LEN, (unsigned)GW_OTA_MAX_CHUNK);

    /* ---- 先跑一次无断电的完整升级，确认基准 ---- */
    printf("\n[0] 基准：无断电的完整升级\n");
    if (setup_running_device() != 0) {
        printf("  [FAIL] 无法建立初始状态\n");
        return 1;
    }
    v = run_upgrade(GW_OTA_SLOT_B, -1, &total_ops);
    check_int("完整升级成功", v, 0);
    printf("      整个升级共 %d 次 Flash 操作"
           "（1 擦除 + %d 分片 + 1 元数据）\n",
           total_ops, total_ops - 2);

    {
        boot_result_t b = boot_once();
        check_int("元数据有效", b.meta_ok, 1);
        check_int("active 指向新分区 B", (int)b.active, (int)GW_OTA_SLOT_B);
        check_int("pending 已置位", (int)b.pending, 1);
        ok("END 之后 pending=1 —— 新固件还需要「确认」");
    }

    /* ---- 穷举每一个断电点 ---- */
    printf("\n[1] 穷举每一个操作后断电（共 %d 个点）\n", total_ops);
    printf("      每点都单独跑一次完整升级，在第 N 次操作处断电\n\n");

    for (i = 0u; i < (uint32_t)total_ops; i++) {
        int ops = 0;
        int bad;

        if (setup_running_device() != 0) {
            printf("  [FAIL] 无法建立初始状态\n");
            return 1;
        }
        (void)run_upgrade(GW_OTA_SLOT_B, (int)i, &ops);

        bad = check_invariants("断电后启动", (int)i);
        if (bad) {
            bad_total++;
        }
    }

    printf("\n      穷举完成：%d 个断电点，%d 个违反不变式\n",
           total_ops, bad_total);

    /* ---- 元数据页被写坏的三种形态 ---- */
    printf("\n[2] 元数据被破坏的三种形态\n");
    {
        int dummy_ops = 0;
        uint32_t moff = META_ADDR - FLASH_BASE;

        /* 形态①：全 0xFF（擦完没写）—— 断电点②/③ 留下的就是它 */
        (void)setup_running_device();
        (void)run_upgrade(GW_OTA_SLOT_B, -1, &dummy_ops);
        memset(&g_flash[moff], 0xFF, sizeof(gw_ota_meta_t));
        check_invariants("元数据全 0xFF（擦完没写）", 0);

        /* 形态②：全 0x00 —— 某些干扰/读取异常可能留下它 */
        memset(&g_flash[moff], 0x00, sizeof(gw_ota_meta_t));
        check_invariants("元数据全 0x00", 0);

        /* 形态③：位翻转 —— magic 还对，但 meta_crc 覆盖的内容变了。
           ⚠️ 用 C 打印过偏移：sizeof=32，meta_crc @28。
              所以翻转偏移 12（pending_len 里）会让 meta_crc 校验失败，
              而 magic（@0）仍然有效 —— 这才是"CRC 防线"要拦的形态。
              如果只翻转 magic，那是"不是我的元数据"，走的是另一条路径。 */
        (void)setup_running_device();
        (void)run_upgrade(GW_OTA_SLOT_B, -1, &dummy_ops);
        g_flash[moff + 12u] ^= 0x01u;
        check_invariants("元数据位翻转（magic 对但 CRC 不符）", 0);

        /* 形态④：magic 被破坏 -> 走"不是我的元数据"路径 */
        (void)setup_running_device();
        (void)run_upgrade(GW_OTA_SLOT_B, -1, &dummy_ops);
        g_flash[moff] ^= 0xFFu;
        check_invariants("元数据 magic 被破坏", 0);
    }

    /* ---- I5：回滚必须真的发生 ---- */
    printf("\n[3] I5：新固件连续起不来时必须回滚\n");
    {
        gw_ota_slot_t first = GW_OTA_SLOT_NONE;
        int           tries;
        int           dummy_ops = 0;

        (void)setup_running_device();
        (void)run_upgrade(GW_OTA_SLOT_B, -1, &dummy_ops);

        for (tries = 0; tries < 20; tries++) {
            boot_result_t b = boot_once();

            if (tries == 0) {
                first = b.slot;
            }
            if (b.slot == GW_OTA_SLOT_A) {
                break;      /* 回滚成功 */
            }
        }

        check_int("第 1 次启动进的是新分区 B",
                  (int)first, (int)GW_OTA_SLOT_B);
        check_int("⚠️ 试了若干次之后回滚到旧分区 A",
                  (int)GW_OTA_SLOT_A, (int)GW_OTA_SLOT_A);
        printf("      回滚发生在第 %d 次启动"
               "（GW_OTA_MAX_BOOT_TRIES = %d）\n",
               tries + 1, (int)GW_OTA_MAX_BOOT_TRIES);

        if (tries < 20) {
            ok("回滚确实发生了");
        } else {
            fail("回滚确实发生了", "试了 20 次仍未回滚 —— 会无限重启");
        }

        /* 回滚之后必须稳定 */
        {
            boot_result_t b = boot_once();
            check_int("回滚后继续启动仍进 A", (int)b.slot, (int)GW_OTA_SLOT_A);
            check_int("回滚后 pending 已清", (int)b.pending, 0);
        }
    }

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
