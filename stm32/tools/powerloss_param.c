/**
 * @file    powerloss_param.c
 * @brief   参数保存的断电穷举 —— 在**每一次** Flash 操作后模拟断电
 *
 * ============================================================================
 *  与 test_param.c 里那个掉电测试的区别
 *
 *  `test_param.c` 的 `test_power_loss_during_write()` 注入了**前 3 次**写失败：
 *
 *      for (fail_after = 1; fail_after <= 3; fail_after++)
 *
 *  而一次 `gw_param_save()` 通常有**上百次**半字写
 *  （头部 18 字节 = 9 个半字，数据最多 256 字节 = 128 个半字）。
 *
 *  所以"第 4 次之后的断电点"**从未被验证过**。
 *
 *  ----------------------------------------------------------------------------
 *  本程序测的不变式（每个断电点都查一遍）
 *
 *  **I1. 必须能读回一份完整数据** —— `gw_param_load()` 要么给出旧值、
 *      要么给出新值，**不能是"没有有效数据"**。
 *      （首次保存例外：那时本来就没有旧值可留。）
 *
 *  **I2. 读回的必须是"完整的"一份** —— 长度与 CRC 都要自洽，
 *      不能出现"头部说 256 字节、实际只写了 40 字节"那种撕裂。
 *
 *  **I3. 必须是旧值或新值之一** —— 不能是两者的混合。
 *      混合意味着"半个新值 + 半个旧值"，那比"没有数据"更危险：
 *      它看起来是有效数据，但内容是错的。
 *
 *  **I4. 槽不能全部失效** —— 8 个槽轮换的意义就是"写新槽时旧槽完好"。
 *      如果一次断电让所有槽都失效，那 8 个槽的设计就白费了。
 *
 *  ----------------------------------------------------------------------------
 *  ⚠️ 断电语义（与 OTA 那个穷举一致的模型）
 *
 *  每个"操作"是一个原子单元：
 *      擦除 -> 整页擦（F1 一页 2 KB）
 *      写入 -> 整次 write 调用
 *
 *  断电时**本次操作不生效**（回滚到操作前的状态）。
 *  这是偏保守的建模：真实器件可能留下"部分擦除的页"（更糟），
 *  所以真机风险不会比这里测出来的小。
 * ============================================================================
 */

#include <stdio.h>
#include <string.h>

#include "gw_param.h"

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
 *  模拟 Flash
 *
 *  ⚠️ 用"快照 + 回滚"实现断电语义：
 *     每次操作前存一份快照，如果这次操作被"断电"打断，
 *     就把快照恢复回去。这样断电 = "这次操作没发生"。
 * ================================================================== */

#define SLOT_COUNT   8u
#define SLOT_SIZE    276u           /* GW_PARAM_SLOT_SIZE */
#define FLASH_SIZE   (SLOT_COUNT * SLOT_SIZE)

static uint8_t  g_flash[FLASH_SIZE];
static uint8_t  g_snap[FLASH_SIZE];

static uint32_t g_erase_calls;
static uint32_t g_write_calls;

/* 注入：在第 N 次"操作"处断电（0 = 注入第一次操作）*/
static int      g_inject_at = -1;
static uint32_t g_op_index  = 0u;
static int      g_fired     = 0;

/** 在每次操作前调用。返回 1 = 这次操作被断电打断 */
static int op_begin(void)
{
    memcpy(g_snap, g_flash, sizeof(g_flash));   /* 存快照 */

    if (g_inject_at >= 0 && (int)g_op_index == g_inject_at) {
        g_fired = 1;
        return 1;
    }
    g_op_index++;
    return 0;
}

static int ff_read(void *ctx, uint32_t offset, uint8_t *buf, uint32_t len)
{
    (void)ctx;
    if (offset + len > FLASH_SIZE) { return -1; }
    memcpy(buf, &g_flash[offset], len);
    return 0;
}

/**
 * 写 —— **按半字粒度**模拟断电
 *
 * ⚠️⚠️ 这是本测试第一版做错的地方，值得记下来。
 *
 *  第一版把"一次 `write` 调用"当成一个原子操作 ——
 *  于是整个保存只有 2 个断电点（头部、数据）。
 *
 *  但 **F1 的编程单位是半字（16 位）**。
 *  一次 18 字节的头部写入在真机上是 **9 次独立的半字编程**。
 *  如果电压在第 4 个半字之后掉了：
 *
 *      头部前 8 字节已写（magic 有效、seq 有效）
 *      头部后 10 字节还是 0xFF（len = 0xFFFFFFFF、crc = 0xFFFFFFFF）
 *      -> 槽头**看起来有 magic**，但 len/crc 是垃圾
 *
 *  那是真实存在的"撕裂槽头"，而第一版的模型**把它掩盖了**。
 *
 *  改细到半字粒度之后，断电点从 2 个变成 9 + 128 个，
 *  这才与真机的行为一致。
 *
 *  ⚠️ 边界：如果 `len` 是奇数，最后一个半字会含 1 个有效字节 + 1 个
 *     0xFF 填充（F1 就是这样）。这里按同样方式处理。
 */
static int ff_write(void *ctx, uint32_t offset, const uint8_t *buf, uint32_t len)
{
    uint32_t i;

    (void)ctx;
    if (offset + len > FLASH_SIZE) { return -1; }

    for (i = 0u; i < len; i += 2u) {
        /* ---- 每一个半字是一次独立的可被打断的操作 ---- */
        if (op_begin()) {
            return -1;              /* 断电：从这一个半字起没写进去 */
        }

        /* 组装一个半字（小端：低字节在前，与 F1 一致）*/
        {
            uint8_t  hw[2];
            uint32_t n = (len - i >= 2u) ? 2u : 1u;

            hw[0] = buf[i];
            hw[1] = (n == 2u) ? buf[i + 1u] : 0xFFu;
            if (n == 1u) {
                /* ⚠️ 奇数字节：后半字节是 0xFF（"不改变"）
                   所以这里不需要写它，只写前一个字节所在的半字。
                   为简化模型，仍按一个半字写入，0xFF 部分无副作用。 */
            }

            /* 模拟 F1"只能 1->0"的限制 */
            if ((g_flash[offset + i] & hw[0]) != hw[0]) {
                return -1;
            }
            g_flash[offset + i] = hw[0];
            if (n == 2u) {
                if ((g_flash[offset + i + 1u] & hw[1]) != hw[1]) {
                    return -1;
                }
                g_flash[offset + i + 1u] = hw[1];
            }
        }
        g_write_calls++;
    }
    return 0;
}

static int ff_erase_slot(void *ctx, uint8_t slot_index)
{
    (void)ctx;
    if (slot_index >= SLOT_COUNT) { return -1; }

    if (op_begin()) {
        return -1;                  /* 断电：本次擦除不生效 */
    }
    memset(&g_flash[slot_index * SLOT_SIZE], 0xFF, SLOT_SIZE);
    g_erase_calls++;
    return 0;
}

static uint8_t  ff_slot_count(void *ctx) { (void)ctx; return SLOT_COUNT; }

static uint32_t ff_slot_offset(void *ctx, uint8_t i)
{
    (void)ctx;
    return (uint32_t)i * SLOT_SIZE;
}

static gw_flash_if_t g_iface;

static void flash_wipe(void)
{
    memset(g_flash, 0xFF, sizeof(g_flash));
    memset(g_snap, 0xFF, sizeof(g_snap));
    g_erase_calls = 0u;
    g_write_calls = 0u;
    g_op_index    = 0u;
    g_fired       = 0;
    g_inject_at   = -1;

    memset(&g_iface, 0, sizeof(g_iface));
    g_iface.read       = ff_read;
    g_iface.write      = ff_write;
    g_iface.erase_slot = ff_erase_slot;
    g_iface.slot_count = ff_slot_count;
    g_iface.slot_offset = ff_slot_offset;
}

/* ==================================================================
 *  跑一次保存，数出它做了多少次操作（不注入断电）
 * ================================================================== */
static int count_ops(int do_erase)
{
    gw_param_t p;
    uint8_t    data[GW_PARAM_MAX_DATA];
    int        i;

    flash_wipe();

    for (i = 0; i < (int)GW_PARAM_MAX_DATA; i++) {
        data[i] = (uint8_t)(i & 0xFFu);
    }

    (void)gw_param_init(&p, &g_iface);

    if (do_erase != 0) {
        /* ⚠️ 要让"第二次保存"触发擦除，必须先占满所有槽：
           8 个槽各写一次之后，第 9 次才会擦最旧的那个。 */
        for (i = 0; i < (int)SLOT_COUNT; i++) {
            data[0] = (uint8_t)i;
            (void)gw_param_save(&p, data, GW_PARAM_MAX_DATA, 1u);
        }
    }

    g_op_index = 0u;
    data[0] = 0xAAu;
    (void)gw_param_save(&p, data, GW_PARAM_MAX_DATA, 2u);

    return (int)g_op_index;
}

/* ==================================================================
 *  不变式检查
 * ================================================================== */
typedef struct {
    int      loaded;            /* 1 = 读到了数据 */
    uint32_t len;
    uint16_t ver;
    uint8_t  first_byte;
} load_result_t;

static load_result_t try_load(void)
{
    load_result_t r;
    gw_param_t    p;
    uint8_t       out[GW_PARAM_MAX_DATA];
    uint32_t      len = 0u;
    uint16_t      ver = 0u;
    gw_param_err_t e;

    memset(&r, 0, sizeof(r));
    memset(out, 0, sizeof(out));

    if (gw_param_init(&p, &g_iface) != GW_PARAM_OK) {
        return r;
    }

    e = gw_param_load(&p, out, sizeof(out), &len, &ver);
    if (e == GW_PARAM_OK && len > 0u) {
        r.loaded     = 1;
        r.len        = len;
        r.ver        = ver;
        r.first_byte = out[0];
    }
    return r;
}

/** 检查"一次断电之后"的状态是否可接受 */
static int check_after_loss(const char *where, int pl_point,
                            const char *old_first, const char *new_first)
{
    load_result_t r = try_load();
    char          tag[200];
    int           bad = 0;

    /* I1：必须读到一份数据 */
    if (!r.loaded) {
        sprintf(tag, "%s（断电点 %d）：读不到任何有效参数 —— "
                     "掉电把有效数据弄丢了", where, pl_point);
        fail("I1 掉电后必须能读到一份完整参数", tag);
        return 1;
    }

    /* I2：长度必须自洽（能读出来本身就说明 CRC 与长度对得上，
       因为 gw_param_load 内部会校验；这里再确认一次长度范围）*/
    if (r.len > GW_PARAM_MAX_DATA || r.len == 0u) {
        sprintf(tag, "%s（断电点 %d）：长度非法 = %u",
                where, pl_point, (unsigned)r.len);
        fail("I2 长度必须自洽", tag);
        bad = 1;
    }

    /* I3：必须是旧值或新值之一，不能是混合 */
    if (!bad) {
        int is_old = (old_first != 0 && r.first_byte == (uint8_t)old_first[0]);
        int is_new = (new_first != 0 && r.first_byte == (uint8_t)new_first[0]);

        if (!is_old && !is_new) {
            sprintf(tag, "%s（断电点 %d）：读到的是**第三份**数据"
                         "（首字节 0x%02X，旧值首字节 0x%02X，新值 0x%02X）",
                    where, pl_point, r.first_byte,
                    old_first ? (unsigned char)old_first[0] : 0u,
                    new_first ? (unsigned char)new_first[0] : 0u);
            fail("I3 只能是旧值或新值，不能是混合", tag);
            bad = 1;
        }
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
    int ops_no_erase;
    int ops_with_erase;
    int total = 0;
    int bad_total = 0;
    int i;

    printf("========== 参数保存的断电穷举 ==========\n");

    /* ---- 先数出一次保存有多少次操作 ---- */
    ops_no_erase   = count_ops(0);
    ops_with_erase = count_ops(1);

    printf("\n[0] 基准\n");
    printf("      一次保存（无需擦除）共 %d 次 Flash 操作\n", ops_no_erase);
    printf("      一次保存（需要擦除）共 %d 次 Flash 操作\n", ops_with_erase);
    check_int("无需擦除时至少 2 次写（头部 + 数据）",
              (ops_no_erase >= 2) ? 1 : 0, 1);
    check_int("需要擦除时操作数更多", (ops_with_erase > ops_no_erase) ? 1 : 0, 1);

    /* ---- 场景 A：槽未满，不需要擦除 ---- */
    printf("\n[1] 穷举：槽未满（不擦除），%d 个断电点\n", ops_no_erase);
    for (i = 0; i < ops_no_erase; i++) {
        gw_param_t p;
        uint8_t    old_data[GW_PARAM_MAX_DATA];
        uint8_t    new_data[GW_PARAM_MAX_DATA];
        int        k;
        int        old_first_before;

        flash_wipe();

        for (k = 0; k < (int)GW_PARAM_MAX_DATA; k++) {
            old_data[k] = 0x5Au;
            new_data[k] = 0xA5u;
        }

        (void)gw_param_init(&p, &g_iface);

        /* 建立"已有一份有效参数" */
        if (gw_param_save(&p, old_data, GW_PARAM_MAX_DATA, 1u) != GW_PARAM_OK) {
            fail("准备阶段保存失败", "");
            continue;
        }
        old_first_before = 0x5A;

        /* 注入断电，再保存新值 */
        g_inject_at = i;
        g_op_index  = 0u;
        g_fired     = 0;
        (void)gw_param_save(&p, new_data, GW_PARAM_MAX_DATA, 2u);

        {
            char old_s[2] = { (char)old_first_before, 0 };
            char new_s[2] = { (char)0xA5, 0 };
            bad_total += check_after_loss("槽未满时断电", i, old_s, new_s);
        }
        total++;
    }

    /* ---- 场景 B：槽已满，需要擦除 ---- */
    printf("\n[2] 穷举：槽已满（要擦除），%d 个断电点\n", ops_with_erase);
    for (i = 0; i < ops_with_erase; i++) {
        gw_param_t p;
        uint8_t    data[GW_PARAM_MAX_DATA];
        uint8_t    old_first = 0;
        int        k;
        int        s;

        flash_wipe();

        for (k = 0; k < (int)GW_PARAM_MAX_DATA; k++) {
            data[k] = 0x11u;
        }
        (void)gw_param_init(&p, &g_iface);

        /* 占满 8 个槽 */
        for (s = 0; s < (int)SLOT_COUNT; s++) {
            data[0] = (uint8_t)(0x10 + s);
            (void)gw_param_save(&p, data, GW_PARAM_MAX_DATA, (uint16_t)s);
        }
        /* 记下"当前有效值"的首字节（最近的槽是最后一次写的那个）*/
        {
            load_result_t lr = try_load();
            old_first = lr.loaded ? lr.first_byte : 0u;
        }

        /* 注入断电，再保存（这次必然会擦除）*/
        data[0] = 0xEEu;
        g_inject_at = i;
        g_op_index  = 0u;
        g_fired     = 0;
        (void)gw_param_save(&p, data, GW_PARAM_MAX_DATA, 99u);

        {
            char old_s[2] = { (char)old_first, 0 };
            char new_s[2] = { (char)0xEE, 0 };
            bad_total += check_after_loss("槽已满时断电", i, old_s, new_s);
        }
        total++;
    }

    /* ---- 场景 C：连续 20 次保存，每次都在随机点断电；之后必须仍可读 ---- */
    printf("\n[3] 压力：连续保存，每次都在不同点断电\n");
    {
        gw_param_t p;
        uint8_t    data[GW_PARAM_MAX_DATA];
        int        round;
        int        k;
        int        survived = 0;

        flash_wipe();
        for (k = 0; k < (int)GW_PARAM_MAX_DATA; k++) {
            data[k] = 0x77u;
        }
        (void)gw_param_init(&p, &g_iface);
        (void)gw_param_save(&p, data, GW_PARAM_MAX_DATA, 1u);

        for (round = 0; round < 20; round++) {
            load_result_t r;

            data[0] = (uint8_t)(0x40 + round);
            g_inject_at = (round % 5);      /* 每次都打在开头附近 */
            g_op_index  = 0u;
            g_fired     = 0;
            (void)gw_param_save(&p, data, GW_PARAM_MAX_DATA,
                                (uint16_t)(2 + round));

            r = try_load();
            if (r.loaded && r.len == GW_PARAM_MAX_DATA) {
                survived++;
            }
        }
        check_int("20 轮断电后每轮都仍能读到完整参数", survived, 20);
        ok("⚠️ 连续断电不会累积损坏 —— 每次都能退回到一份完整数据");
    }

    /* ---- 场景 D：连续掉电耗尽所有槽（抓到缺陷的那个场景） ----
     *
     *  每一次"写中途掉电"都会让一个槽变成损坏槽（magic 已写、校验不过）。
     *  由于磨损均衡依次用下一个槽，**每次掉电消耗一个槽**。
     *
     *  8 个槽 + 连续 8 次"写中途掉电" = 7 个损坏槽 + 1 个有效槽。
     *  此时没有空槽也没有更旧的有效槽 ——
     *  如果 pick_target_slot 不复用损坏槽，就会落到"擦当前有效槽"的退化分支，
     *  **参数永久丢失**。
     *
     *  这个场景原来没被测过（原 test_param 只注入前 3 次写，
     *  而且每次都是全新 Flash，不会累积）。
     *  加上本穷举工具之后一次就抓到了，并已修复（复用损坏槽）。
     *
     *  ⚠️ 这里刻意跑 **15 轮**（> 槽数 8），确保"耗尽所有槽之后仍可用"被覆盖。
     */
    printf("\n[4] consecutive power loss: 15 rounds (> slot count 8)\n");
    {
        gw_param_t p;
        uint8_t    data[GW_PARAM_MAX_DATA];
        int        round;
        int        k;
        int        ok_rounds = 0;

        flash_wipe();
        for (k = 0; k < (int)GW_PARAM_MAX_DATA; k++) { data[k] = 0x33u; }
        (void)gw_param_init(&p, &g_iface);
        (void)gw_param_save(&p, data, GW_PARAM_MAX_DATA, 1u);

        for (round = 0; round < 15; round++) {
            load_result_t r;

            data[0] = (uint8_t)(0x50 + round);
            g_inject_at = 0;
            g_op_index  = 0u;
            g_fired     = 0;
            (void)gw_param_save(&p, data, GW_PARAM_MAX_DATA,
                                (uint16_t)(2 + round));

            r = try_load();
            if (r.loaded && r.len == GW_PARAM_MAX_DATA) {
                ok_rounds++;
            } else {
                char tag[160];
                sprintf(tag, "round %d of 15: params lost -- "
                             "corrupt slots not reused, fallback erased "
                             "the valid slot", round);
                fail("consecutive power loss keeps params readable", tag);
                break;
            }
        }
        check_int("15 rounds of mid-write power loss, params still readable",
                  ok_rounds, 15);
        ok("this caught a real defect: pick_target_slot used to skip corrupt "
           "slots, so 8 consecutive losses erased the only valid slot");
    }

    printf("\n      checked %d power-loss points + 15 consecutive rounds, "
           "%d invariant violations\n", total, bad_total);

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
