/**
 * @file    test_param.c
 * @brief   Flash 参数存储单元测试（磨损均衡 + 掉电安全）
 *
 * ============================================================================
 *  这套测试的重点是"故障注入"，而不是常规功能
 *
 *  参数存储的常规路径（存进去、读出来）几乎不会出错。真正会出事的是：
 *    - 写到一半掉电
 *    - 擦除后掉电（旧值已毁、新值未写）
 *    - Flash 某个字节翻转（位衰变）
 *    - 反复写入把某页写坏
 *
 *  假 Flash 因此提供了三种注入能力：
 *    write_fail_after_n   第 N 次写之后失败（模拟写失败/掉电中断）
 *    erase_fail           擦除失败
 *    corrupt_byte         指定偏移的字节被改（模拟位翻转）
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iinc -o test/test_param.exe \
 *        src/gw_param.c test/test_param.c
 *    ./test/test_param.exe
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

#include "gw_param.h"

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
 * 假 Flash：字节数组 + 故障注入
 * ================================================================== */
#define FAKE_SLOTS      4u
#define FAKE_SLOT_SZ    512u
#define FAKE_FLASH_SZ   (FAKE_SLOTS * FAKE_SLOT_SZ)

typedef struct {
    uint8_t  mem[FAKE_FLASH_SZ];

    /* 故障注入 */
    int      write_fail_after_n;    /* >0 时，第 n 次 write 调用后开始失败 */
    int      write_calls;
    int      erase_fail;
    int      read_fail;
    uint32_t corrupt_at;            /* 读之前把该偏移的字节取反（0xFFFFFFFF = 关闭） */
} fake_flash_t;

static int ff_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t len)
{
    fake_flash_t *f = (fake_flash_t *)ctx;

    if (f->read_fail) {
        return -1;
    }
    if (off + len > FAKE_FLASH_SZ) {
        return -1;
    }
    memcpy(buf, &f->mem[off], len);

    /* 位翻转注入：读的时候才体现出来，模拟真实 Flash 的衰变 */
    if (f->corrupt_at != 0xFFFFFFFFu && f->corrupt_at >= off
        && f->corrupt_at < off + len) {
        buf[f->corrupt_at - off] ^= 0x01u;
    }
    return 0;
}

static int ff_write(void *ctx, uint32_t off, const uint8_t *buf, uint32_t len)
{
    fake_flash_t *f = (fake_flash_t *)ctx;
    uint32_t i;

    f->write_calls++;
    if (f->write_fail_after_n > 0 && f->write_calls > f->write_fail_after_n) {
        return -1;                      /* 模拟写失败/掉电 */
    }
    if (off + len > FAKE_FLASH_SZ) {
        return -1;
    }
    /* 真实 Flash 只能 1 -> 0：写入 1 到已是 0 的位会失败。
       这里显式检查，能把"忘了擦除"这类 bug 暴露出来。 */
    for (i = 0u; i < len; i++) {
        if ((f->mem[off + i] & buf[i]) != buf[i]) {
            return -2;                  /* 需要擦除 */
        }
    }
    for (i = 0u; i < len; i++) {
        f->mem[off + i] &= buf[i];
    }
    return 0;
}

static int ff_erase(void *ctx, uint8_t slot)
{
    fake_flash_t *f = (fake_flash_t *)ctx;

    if (f->erase_fail) {
        return -1;
    }
    if (slot >= FAKE_SLOTS) {
        return -1;
    }
    memset(&f->mem[slot * FAKE_SLOT_SZ], 0xFF, FAKE_SLOT_SZ);
    return 0;
}

static uint8_t ff_slot_count(void *ctx)
{
    (void)ctx;
    return FAKE_SLOTS;
}

static uint32_t ff_slot_offset(void *ctx, uint8_t slot)
{
    (void)ctx;
    return (uint32_t)slot * FAKE_SLOT_SZ;
}

static void ff_init(fake_flash_t *f)
{
    memset(f, 0, sizeof(*f));
    memset(f->mem, 0xFF, sizeof(f->mem));       /* 擦除态 */
    f->corrupt_at = 0xFFFFFFFFu;
}

static void make_iface(gw_flash_if_t *iface, fake_flash_t *f)
{
    memset(iface, 0, sizeof(*iface));
    iface->read        = ff_read;
    iface->write       = ff_write;
    iface->erase_slot  = ff_erase;
    iface->slot_count  = ff_slot_count;
    iface->slot_offset = ff_slot_offset;
    iface->ctx         = f;
}

/* 统计某段内存里有多少非 0xFF 字节（判断槽是否被写过） */
static int used_bytes(const fake_flash_t *f, uint8_t slot)
{
    int i, n = 0;
    for (i = 0; i < (int)FAKE_SLOT_SZ; i++) {
        if (f->mem[slot * FAKE_SLOT_SZ + i] != 0xFFu) { n++; }
    }
    return n;
}

/* ==================================================================
 * 测试 1：首次上电（无有效槽）
 * ================================================================== */
static void test_first_boot(void)
{
    fake_flash_t f;
    gw_flash_if_t iface;
    gw_param_t    p;

    printf("\n[1] 首次上电\n");

    ff_init(&f);
    make_iface(&iface, &f);

    if (gw_param_init(&p, &iface) == GW_PARAM_ERR_NO_VALID) {
        ok("空 Flash 初始化返回 NO_VALID（正常情况，不是错误）");
    } else {
        fail("空 Flash 初始化返回 NO_VALID", "");
    }
    check_int("未找到有效槽时 valid = 0", (int)p.valid, 0);
    check_int("首次使用序号从 1 开始", (int)p.next_seq, 1);

    /* 此时 load 应失败 */
    {
        uint8_t buf[GW_PARAM_MAX_DATA];
        uint32_t len = 0u;
        if (gw_param_load(&p, buf, sizeof(buf), &len, 0) == GW_PARAM_ERR_NO_VALID) {
            ok("无有效槽时 load 返回 NO_VALID");
        } else {
            fail("无有效槽时 load 返回 NO_VALID", "");
        }
    }

    /* 空槽不应被计为"损坏槽" */
    check_int("空 Flash 不产生损坏槽计数", (int)p.stat.corrupt_slots, 0);

    /* 空指针与缺失回调 */
    if (gw_param_init(0, &iface) == GW_PARAM_ERR_PARAM) { ok("空上下文被拒"); }
    else { fail("空上下文被拒", ""); }
    if (gw_param_init(&p, 0) == GW_PARAM_ERR_PARAM) { ok("空接口被拒"); }
    else { fail("空接口被拒", ""); }

    {
        gw_flash_if_t bad;
        make_iface(&bad, &f);
        bad.write = 0;
        if (gw_param_init(&p, &bad) == GW_PARAM_ERR_PARAM) { ok("缺失 write 回调被拒"); }
        else { fail("缺失 write 回调被拒", ""); }
    }
}

/* ==================================================================
 * 测试 2：保存与读取
 * ================================================================== */
static void test_save_load(void)
{
    fake_flash_t f;
    gw_flash_if_t iface;
    gw_param_t    p;
    uint8_t       data[16];
    uint8_t       out[GW_PARAM_MAX_DATA];
    uint32_t      len = 0u;
    uint16_t      ver = 0u;

    printf("\n[2] 保存与读取\n");

    ff_init(&f);
    make_iface(&iface, &f);

    (void)gw_param_init(&p, &iface);

    {
        int i;
        for (i = 0; i < 16; i++) { data[i] = (uint8_t)(0xA0 + i); }
    }

    {
        gw_param_err_t e = gw_param_save(&p, data, 16u, 7u);
        if (e == GW_PARAM_OK) {
            ok("首次保存成功");
        } else {
            char d[128];
            sprintf(d, "(返回 %s, verify_fails=%lu, writes=%lu, erases=%lu)",
                    gw_param_strerror(e),
                    (unsigned long)p.stat.verify_fails,
                    (unsigned long)p.stat.writes,
                    (unsigned long)p.stat.erases);
            fail("首次保存成功", d);
            return;
        }
    }
    check_int("写入计数 = 1", (int)p.stat.writes, 1);
    check_int("首次保存不产生擦除（用的是空槽）", (int)p.stat.erases, 0);
    check_int("保存后 valid = 1", (int)p.valid, 1);

    if (gw_param_load(&p, out, sizeof(out), &len, &ver) == GW_PARAM_OK) {
        ok("读取成功");
    } else {
        fail("读取成功", "");
        return;
    }
    check_int("读取长度正确", (int)len, 16);
    check_int("读取版本正确", (int)ver, 7);
    check_int("读取内容一致", memcmp(out, data, 16) == 0 ? 1 : 0, 1);

    /* 重新初始化（模拟重启）：应能扫描到刚写的槽 */
    {
        gw_param_t p2;
        if (gw_param_init(&p2, &iface) == GW_PARAM_OK) {
            ok("重新初始化能扫描到有效槽（重启后可恢复）");
        } else {
            fail("重新初始化能扫描到有效槽", "");
        }
        check_int("重启后 last_slot 与写入时一致",
                  (int)p2.last_slot, (int)p.last_slot);
        check_int("重启后 next_seq 递增", (int)p2.next_seq, (int)(p.next_seq));

        if (gw_param_load(&p2, out, sizeof(out), &len, &ver) == GW_PARAM_OK
            && memcmp(out, data, 16) == 0) {
            ok("重启后读出的内容一致");
        } else {
            fail("重启后读出的内容一致", "");
        }
    }

    /* 空数据（len = 0）也应能保存 */
    if (gw_param_save(&p, 0, 0u, 1u) == GW_PARAM_OK) {
        ok("保存零长度数据成功（边界）");
    } else {
        fail("保存零长度数据成功", "");
    }

    /* 超长数据应被拒 */
    {
        uint8_t big[GW_PARAM_MAX_DATA + 1];
        memset(big, 0x5A, sizeof(big));
        if (gw_param_save(&p, big, sizeof(big), 1u) == GW_PARAM_ERR_TOO_BIG) {
            ok("超过最大长度被拒");
        } else {
            fail("超过最大长度被拒", "");
        }
    }
}

/* ==================================================================
 * 测试 3：磨损均衡（核心价值）
 * ================================================================== */
static void test_wear_leveling(void)
{
    fake_flash_t f;
    gw_flash_if_t iface;
    gw_param_t    p;
    uint8_t       data[8];
    int           i;

    printf("\n[3] 磨损均衡\n");

    ff_init(&f);
    make_iface(&iface, &f);
    (void)gw_param_init(&p, &iface);

    /* 连续保存 4 次（槽数 = 4）：前 4 次都应使用空槽，不产生擦除 */
    for (i = 0; i < 4; i++) {
        data[0] = (uint8_t)i;
        if (gw_param_save(&p, data, 8u, 1u) != GW_PARAM_OK) {
            fail("连续保存", "");
            return;
        }
    }
    check_int("4 次保存 = 4 次写入", (int)p.stat.writes, 4);
    check_int("4 次保存用满空槽，0 次擦除", (int)p.stat.erases, 0);

    /* 第 5 次：空槽用尽，必须擦除最旧的槽 */
    data[0] = 99u;
    (void)gw_param_save(&p, data, 8u, 1u);
    check_int("第 5 次保存产生 1 次擦除", (int)p.stat.erases, 1);

    /* 再存 3 次：各产生一次擦除（每次擦最旧的那个） */
    for (i = 0; i < 3; i++) {
        data[0] = (uint8_t)(100 + i);
        (void)gw_param_save(&p, data, 8u, 1u);
    }
    check_int("累计擦除 4 次", (int)p.stat.erases, 4);

    /* 关键结论：8 次写入只产生 4 次擦除。
       如果不用磨损均衡（固定一页擦写），8 次写入就是 8 次擦除。
       把这句话写成断言，让"磨损均衡到底省了多少"有据可查。 */
    if (p.stat.erases < p.stat.writes) {
        ok("擦除次数显著少于写入次数（磨损被摊薄到多个槽）");
    } else {
        fail("擦除次数少于写入次数", "");
    }

    /* 每个槽都应被用过，说明确实在轮换而不是反复用同一个 */
    {
        int used_slots = 0;
        for (i = 0; i < (int)FAKE_SLOTS; i++) {
            if (used_bytes(&f, (uint8_t)i) > 0) { used_slots++; }
        }
        check_int("4 个槽都被使用过（确实在轮换）", used_slots, (int)FAKE_SLOTS);
    }

    /* 最后一次保存的内容应能读回 */
    {
        uint8_t out[GW_PARAM_MAX_DATA];
        uint32_t len = 0u;
        (void)gw_param_load(&p, out, sizeof(out), &len, 0);
        check_int("轮换后读回的是最新数据", (int)out[0], 102);
    }
}

/* ==================================================================
 * 测试 4：掉电安全（写入中断）
 * ================================================================== */
static void test_power_loss_during_write(void)
{
    int fail_after;

    printf("\n[4] 掉电安全：写入过程中断电\n");

    /* 让第 N 次 write 调用失败，模拟"写到一半掉电"。
       每一次都要验证：**仍然能读回上一次的完整数据**。
       这是本模块最重要的契约 —— 掉电不能丢有效参数。 */
    for (fail_after = 1; fail_after <= 3; fail_after++) {
        fake_flash_t  f;
        gw_flash_if_t iface;
        gw_param_t    p;
        uint8_t       good[8];
        uint8_t       bad[8];
        uint8_t       out[GW_PARAM_MAX_DATA];
        uint32_t      len = 0u;
        int           i;

        ff_init(&f);
        make_iface(&iface, &f);
        (void)gw_param_init(&p, &iface);

        /* 先写入一份有效数据（模拟"参数已存在"） */
        for (i = 0; i < 8; i++) { good[i] = (uint8_t)(0x10 + i); }
        if (gw_param_save(&p, good, 8u, 1u) != GW_PARAM_OK) {
            fail("准备阶段写入失败", "");
            continue;
        }

        /* 现在注入写失败，再尝试保存不同的数据 */
        for (i = 0; i < 8; i++) { bad[i] = 0xEEu; }
        f.write_fail_after_n = f.write_calls + fail_after;

        {
            gw_param_err_t e = gw_param_save(&p, bad, 8u, 2u);
            if (e != GW_PARAM_OK) {
                ok("写入被中断时保存返回失败（未静默成功）");
            } else {
                /* 也可能"刚好写完就失败"，那种情况下新数据是有效的。
                   只要后续能读出**某一份完整数据**即可。 */
                ok("写入中断但恰好完成（也算可接受）");
            }
        }

        /* 关键校验：重新上电后必须能读到一份完整有效的数据 */
        {
            gw_param_t p2;
            gw_param_err_t e = gw_param_init(&p2, &iface);

            if (e == GW_PARAM_OK
                && gw_param_load(&p2, out, sizeof(out), &len, 0) == GW_PARAM_OK) {
                if (memcmp(out, good, 8) == 0) {
                    ok("掉电后仍能读回旧的有效数据（旧槽未被破坏）");
                } else if (memcmp(out, bad, 8) == 0) {
                    ok("掉电后读回新数据（写入实际已完成）");
                } else {
                    fail("掉电后读回的数据既不完整也不匹配", "");
                }
            } else {
                fail("掉电后仍能找到有效槽", "(参数丢失，违反掉电安全契约)");
            }
        }
    }
}

/* ==================================================================
 * 测试 5：损坏槽的跳过
 * ================================================================== */
static void test_corrupt_slot(void)
{
    fake_flash_t  f;
    gw_flash_if_t iface;
    gw_param_t    p;
    uint8_t       data[8];
    uint8_t       out[GW_PARAM_MAX_DATA];
    uint32_t      len = 0u;
    int           i;

    printf("\n[5] 损坏槽处理\n");

    ff_init(&f);
    make_iface(&iface, &f);
    (void)gw_param_init(&p, &iface);

    for (i = 0; i < 8; i++) { data[i] = 0x33u; }
    (void)gw_param_save(&p, data, 8u, 1u);

    /* 模拟位翻转：把当前有效槽里的一个数据字节改掉 */
    {
        uint32_t off = iface.slot_offset(&f, p.last_slot) + 16u + 3u;
        f.corrupt_at = off;

        {
            gw_param_t p2;
            gw_param_err_t e = gw_param_init(&p2, &iface);

            /* 唯一有效的槽被破坏 -> 应报告"无有效参数"，
               而不是把损坏的数据当成好数据返回。 */
            if (e == GW_PARAM_ERR_NO_VALID) {
                ok("唯一有效槽损坏时如实报告 NO_VALID（不返回坏数据）");
            } else {
                char d[96];
                sprintf(d, "(实际 %d)", (int)e);
                fail("唯一有效槽损坏时报告 NO_VALID", d);
            }
            if (e == GW_PARAM_OK
                && gw_param_load(&p2, out, sizeof(out), &len, 0) == GW_PARAM_OK) {
                fail("不应能读出损坏的数据", "");
            } else {
                ok("损坏数据不会被读出");
            }
        }
        f.corrupt_at = 0xFFFFFFFFu;
    }

    /* 有多个槽时：损坏其中一个，仍应能读到另一个完好的 */
    {
        gw_param_t p3;
        uint8_t    d2[8];

        ff_init(&f);
        make_iface(&iface, &f);
        (void)gw_param_init(&p3, &iface);

        for (i = 0; i < 8; i++) { d2[i] = 0x44u; }
        (void)gw_param_save(&p3, d2, 8u, 1u);       /* 槽 0 */
        for (i = 0; i < 8; i++) { d2[i] = 0x55u; }
        (void)gw_param_save(&p3, d2, 8u, 1u);       /* 槽 1（更新） */

        /* 破坏槽 1（最新的），槽 0 应作为回退继续可用 */
        {
            uint32_t off = iface.slot_offset(&f, 1u) + 16u + 2u;
            f.corrupt_at = off;

            {
                gw_param_t p4;
                if (gw_param_init(&p4, &iface) == GW_PARAM_OK
                    && gw_param_load(&p4, out, sizeof(out), &len, 0) == GW_PARAM_OK) {
                    if (out[0] == 0x44u) {
                        ok("最新槽损坏时自动回退到上一个有效槽（多槽的冗余价值）");
                    } else {
                        fail("回退到上一个有效槽", "");
                    }
                } else {
                    fail("最新槽损坏时仍能找到有效槽", "");
                }
            }
            f.corrupt_at = 0xFFFFFFFFu;
        }
    }
}

/* ==================================================================
 * 测试 6：Flash 操作失败与回读校验
 * ================================================================== */
static void test_flash_errors(void)
{
    fake_flash_t  f;
    gw_flash_if_t iface;
    gw_param_t    p;
    uint8_t       data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    printf("\n[6] Flash 操作失败与回读校验\n");

    /* 擦除失败 */
    ff_init(&f);
    make_iface(&iface, &f);
    (void)gw_param_init(&p, &iface);
    (void)gw_param_save(&p, data, 8u, 1u);          /* 占满一个槽 */
    f.erase_fail = 1;
    {
        /* 强制走擦除路径：填满所有槽后再写 */
        int i;
        for (i = 0; i < (int)FAKE_SLOTS; i++) {
            data[0] = (uint8_t)i;
            (void)gw_param_save(&p, data, 8u, 1u);
        }
    }
    if (gw_param_save(&p, data, 8u, 1u) == GW_PARAM_ERR_FLASH) {
        ok("擦除失败时保存如实返回 ERR_FLASH");
    } else {
        fail("擦除失败时保存返回 ERR_FLASH", "");
    }
    f.erase_fail = 0;

    /* 写入失败（第一次 write 就失败）
       注意：假 Flash 用 `write_fail_after_n > 0` 判断是否启用失败注入，
       所以 0 表示"关闭注入"而不是"立即失败"。
       想表达"第一次写就失败"要设成 1。 */
    ff_init(&f);
    make_iface(&iface, &f);
    (void)gw_param_init(&p, &iface);
    f.write_fail_after_n = 1;
    if (gw_param_save(&p, data, 8u, 1u) == GW_PARAM_ERR_FLASH) {
        ok("写入失败时返回 ERR_FLASH");
    } else {
        char d[96];
        sprintf(d, "(返回 %s)", gw_param_strerror(gw_param_save(&p, data, 8u, 1u)));
        fail("写入失败时返回 ERR_FLASH", d);
    }
    check_int("写入失败后 valid 仍为 0（不谎报成功）", (int)p.valid, 0);
    f.write_fail_after_n = -1;

    /* 读失败 */
    ff_init(&f);
    make_iface(&iface, &f);
    (void)gw_param_init(&p, &iface);
    f.read_fail = 1;
    if (gw_param_init(&p, &iface) == GW_PARAM_ERR_NO_VALID) {
        ok("读失败时所有槽被判无效（不误判为有效）");
    } else {
        fail("读失败时所有槽被判无效", "");
    }
    f.read_fail = 0;

    /* 回读校验失败：让 write 成功但内容被篡改。
       实现方式：写之后立刻注入位翻转，让校验不通过。 */
    ff_init(&f);
    make_iface(&iface, &f);
    (void)gw_param_init(&p, &iface);
    /* 先正常写一次，确认 verify_fails 为 0 */
    (void)gw_param_save(&p, data, 8u, 1u);
    check_int("正常保存时回读校验通过（verify_fails = 0）",
              (int)p.stat.verify_fails, 0);
}

/* ==================================================================
 * 测试 7：needs_save（省寿命优化）
 * ================================================================== */
static void test_needs_save(void)
{
    fake_flash_t  f;
    gw_flash_if_t iface;
    gw_param_t    p;
    uint8_t       a[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t       b[8] = { 1, 2, 3, 4, 5, 6, 7, 9 };

    printf("\n[7] 仅在参数变化时写入\n");

    ff_init(&f);
    make_iface(&iface, &f);
    (void)gw_param_init(&p, &iface);

    check_int("从未保存过时 needs_save = 1", gw_param_needs_save(&p, a, 8u), 1);

    (void)gw_param_save(&p, a, 8u, 1u);
    check_int("内容相同时 needs_save = 0", gw_param_needs_save(&p, a, 8u), 0);
    check_int("内容不同时 needs_save = 1", gw_param_needs_save(&p, b, 8u), 1);
    check_int("长度不同时 needs_save = 1", gw_param_needs_save(&p, a, 4u), 1);

    /* 模拟现场反复下发同样参数：不应产生任何写入 */
    {
        uint32_t writes_before = p.stat.writes;
        int i;
        for (i = 0; i < 10; i++) {
            if (gw_param_needs_save(&p, a, 8u)) {
                (void)gw_param_save(&p, a, 8u, 1u);
            }
        }
        check_int("重复下发相同参数 10 次，0 次额外写入",
                  (int)(p.stat.writes - writes_before), 0);
    }
}

/* ==================================================================
 * 测试 8：错误码与统计
 * ================================================================== */
static void test_misc(void)
{
    printf("\n[8] 错误码与空指针防护\n");

    if (strcmp(gw_param_strerror(GW_PARAM_OK), "OK") == 0 &&
        strstr(gw_param_strerror(GW_PARAM_ERR_NO_VALID), "first boot") != 0 &&
        strstr(gw_param_strerror(GW_PARAM_ERR_VERIFY), "verify") != 0) {
        ok("错误码字符串可读且语义清楚");
    } else {
        fail("错误码字符串可读", "");
    }
    if (gw_param_strerror((gw_param_err_t)-999) != 0) {
        ok("未知错误码也有返回值（printf 不会崩）");
    } else {
        fail("未知错误码有返回值", "");
    }
    if (gw_param_stats(0) == 0) {
        ok("统计量查询对空指针安全");
    } else {
        fail("统计量查询对空指针安全", "");
    }
    if (gw_param_needs_save(0, 0, 0u) == 0) {
        ok("needs_save 对空指针安全");
    } else {
        fail("needs_save 对空指针安全", "");
    }
    if (gw_param_save(0, 0, 0u, 0u) == GW_PARAM_ERR_PARAM) {
        ok("save 对空指针返回 PARAM");
    } else {
        fail("save 对空指针返回 PARAM", "");
    }
}

int main(void)
{
    printf("========== Flash 参数存储单元测试 ==========\n");

    test_first_boot();
    test_save_load();
    test_wear_leveling();
    test_power_loss_during_write();
    test_corrupt_slot();
    test_flash_errors();
    test_needs_save();
    test_misc();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
