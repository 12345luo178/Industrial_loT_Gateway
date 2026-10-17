/**
 * @file    test_ota.c
 * @brief   OTA 双分区升级与回滚单元测试
 *
 * ============================================================================
 *  这套测试的重点是**"升级失败后设备还能不能起来"**，而不是"升级能不能成功"。
 *
 *  成功路径相对好写（发完数据、CRC 对上、标记切换），
 *  真正难的是那些**出错之后**的路径：
 *
 *    1) 新固件完全起不来（连 main 都进不去）-> 必须能回滚
 *    2) 新固件能起来但很快崩 -> 也必须能回滚
 *    3) 升级写到一半断电 -> 旧固件必须还能用
 *    4) 元数据自身损坏（Flash 位翻转）-> 不能被误读成有效记录
 *
 *  第 1 条是本模块最重要的设计约束，测试里用"反复模拟启动、
 *  但从不调用 confirm"来覆盖它 —— 这正是"固件跑不起来"的本质特征。
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iinc -Iport -o test/test_ota.exe \
 *        src/gw_ota.c test/test_ota.c
 *    ./test/test_ota.exe
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include "gw_ota.h"

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
 * 假 Flash
 *
 * 只实现 APP 槽位 + 元数据区，按**偏移**索引，避免分配 512KB。
 * ================================================================== */
#define FAKE_APP_SIZE   GW_APP_SLOT_SIZE
#define FAKE_ERASED     0xFFu

typedef struct {
    uint8_t  app_a[FAKE_APP_SIZE];
    uint8_t  app_b[FAKE_APP_SIZE];
    /* 元数据：**两页**（双页 ping-pong）。按页索引，模拟真实布局。 */
    uint8_t  meta[GW_OTA_META_PAGES][GW_FLASH_PAGE_SIZE];

    /* 故障注入 */
    int      fail_erase_after_n;    /* >0：第 n 次擦除开始失败 */
    int      erase_calls;
    int      fail_write_after_n;
    int      write_calls;
    int      corrupt_verify;        /* >0：verify 失败 n 次 */
    int      corrupt_meta;          /* >0：meta 读取返回损坏的数据（按页注入） */
    /* 元数据注入（原来没有，导致"落盘失败"分支测不到） */
    int      meta_write_calls;      /* 元数据**写页**次数 */
    int      meta_erase_calls;      /* 元数据**擦页**次数 */
    int      fail_meta_write_after_n;
    int      fail_meta_erase_after_n;
    int      truncate_meta_write;   /* >0：这一页只写前 n 字节就"掉电" */
    int      corrupt_meta_page;     /* >=0：只让这一页读出损坏数据；-1 = 不注入 */
    uint8_t  last_erase_page;       /* 最近一次擦的是哪一页（断言"只擦旧页"用） */
    uint8_t  last_write_page;       /* 最近一次写的是哪一页 */
} fake_flash_t;

static fake_flash_t FL;

static uint8_t *flash_ptr(uint32_t addr)
{
    if (addr >= GW_APP_A_ADDR && addr < GW_APP_A_ADDR + FAKE_APP_SIZE) {
        return &FL.app_a[addr - GW_APP_A_ADDR];
    }
    if (addr >= GW_APP_B_ADDR && addr < GW_APP_B_ADDR + FAKE_APP_SIZE) {
        return &FL.app_b[addr - GW_APP_B_ADDR];
    }
    return 0;
}

static void flash_reset(void)
{
    memset(&FL, 0, sizeof(FL));
    memset(FL.app_a, FAKE_ERASED, sizeof(FL.app_a));
    memset(FL.app_b, FAKE_ERASED, sizeof(FL.app_b));
    memset(FL.meta,  FAKE_ERASED, sizeof(FL.meta));
    /* ⚠️ 0 对 corrupt_meta_page 是"有效页号"，会意外注入损坏 ——
       必须显式设成 -1（memset 之后设，别依赖默认值）。 */
    FL.corrupt_meta_page = -1;
}

/** 直接往某一页糊一条记录（模拟"上一版固件/异常状态留下的数据"） */
static void meta_plant(uint8_t page, const gw_ota_meta_t *m)
{
    if (page < GW_OTA_META_PAGES) {
        memcpy(FL.meta[page], m, sizeof(*m));
    }
}

static int ff_erase(void *ctx, uint32_t addr, uint32_t len)
{
    uint8_t *p;
    uint32_t i;

    (void)ctx;
    FL.erase_calls++;
    if (FL.fail_erase_after_n > 0
        && FL.erase_calls >= FL.fail_erase_after_n) {
        return -1;
    }

    p = flash_ptr(addr);
    if (p == 0 || (addr - GW_APP_A_ADDR) + len > FAKE_APP_SIZE * 2u) {
        return -1;
    }
    for (i = 0u; i < len; i++) {
        p[i] = FAKE_ERASED;
    }
    return 0;
}

static int ff_write(void *ctx, uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint8_t *p;
    uint32_t i;

    (void)ctx;
    FL.write_calls++;
    if (FL.fail_write_after_n > 0
        && FL.write_calls >= FL.fail_write_after_n) {
        return -1;
    }

    p = flash_ptr(addr);
    if (p == 0) {
        return -1;
    }

    /* 模拟 Flash 的"只能 1 变 0"限制：
       往未擦除的位置写会失败。这让"重复分片"的问题能被真实复现。 */
    for (i = 0u; i < len; i++) {
        if ((p[i] & data[i]) != data[i]) {
            return -2;              /* 目标不是全 1 */
        }
    }
    for (i = 0u; i < len; i++) {
        p[i] = data[i];
    }
    return 0;
}

static int ff_verify(void *ctx, uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint8_t *p;

    (void)ctx;
    if (FL.corrupt_verify > 0) {
        FL.corrupt_verify--;
        return -1;
    }
    p = flash_ptr(addr);
    if (p == 0) {
        return -1;
    }
    return (memcmp(p, data, len) == 0) ? 0 : -1;
}

/* ---------------- 元数据：按页读 / 擦 / 写 ----------------
 *
 * ⚠️ 这三个函数就是"能不能把掉电行为测出来"的关键：
 *    选页/选新在纯逻辑层，假介质只需如实反映"某一页被擦成 FF"、
 *    "某一页只写了一半"、"某一页读出来是坏的" —— 于是"任何瞬间至少一份有效"
 *    这条不变量可以被逐点枚举验证，而不是上板拔电碰运气。
 */

static int ff_meta_read_page(void *ctx, uint8_t page, uint8_t *buf, uint32_t len)
{
    (void)ctx;
    if (page >= GW_OTA_META_PAGES || len > GW_FLASH_PAGE_SIZE) {
        return -1;
    }
    memcpy(buf, FL.meta[page], len);
    if (FL.corrupt_meta > 0) {
        FL.corrupt_meta--;
        buf[8] ^= 0x5Au;            /* 翻转一位（落在 seq 上），模拟位翻转 */
    }
    if (FL.corrupt_meta_page >= 0
        && (uint8_t)FL.corrupt_meta_page == page) {
        buf[8] ^= 0x5Au;
    }
    return 0;
}

static int ff_meta_erase_page(void *ctx, uint8_t page)
{
    (void)ctx;
    if (page >= GW_OTA_META_PAGES) {
        return -1;
    }
    FL.meta_erase_calls++;
    if (FL.fail_meta_erase_after_n > 0
        && FL.meta_erase_calls >= FL.fail_meta_erase_after_n) {
        return -1;                  /* 注入"擦除失败" */
    }
    FL.last_erase_page = page;
    memset(FL.meta[page], FAKE_ERASED, GW_FLASH_PAGE_SIZE);
    return 0;
}

static int ff_meta_write_page(void *ctx, uint8_t page, const uint8_t *buf,
                              uint32_t len)
{
    (void)ctx;
    if (page >= GW_OTA_META_PAGES || len > GW_FLASH_PAGE_SIZE) {
        return -1;
    }

    FL.meta_write_calls++;
    FL.last_write_page = page;

    if (FL.fail_meta_write_after_n > 0
        && FL.meta_write_calls >= FL.fail_meta_write_after_n) {
        return -1;
    }

    /* 注入"写到一半掉电"：只落一半字节，其余保持 0xFF（未写完的页） */
    if (FL.truncate_meta_write > 0
        && (uint32_t)FL.truncate_meta_write < len) {
        memcpy(FL.meta[page], buf, (size_t)FL.truncate_meta_write);
        return 0;                   /* 硬件"以为写成功了" */
    }

    /* 真实实现不擦除（擦由上层显式调）；这里直接覆盖 */
    memcpy(FL.meta[page], buf, len);
    return 0;
}


static void make_io(gw_ota_io_t *io)
{
    memset(io, 0, sizeof(*io));
    io->erase            = ff_erase;
    io->write            = ff_write;
    io->verify           = ff_verify;
    io->meta_read_page   = ff_meta_read_page;
    io->meta_erase_page  = ff_meta_erase_page;
    io->meta_write_page  = ff_meta_write_page;
    io->ctx              = 0;
}

/* 计算一段数据的 CRC32（与 gw_ota.c 内部实现一致，独立算一遍） */
static uint32_t ref_crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;
    int      j;

    for (i = 0u; i < len; i++) {
        crc ^= data[i];
        for (j = 0; j < 8; j++) {
            crc = (crc & 1u) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

/* 造一段"固件"内容 */
static void make_firmware(uint8_t *buf, uint32_t len, uint8_t seed)
{
    uint32_t i;

    for (i = 0u; i < len; i++) {
        buf[i] = (uint8_t)(seed + (uint8_t)i);
    }
}

/* 完整走一遍升级流程 */
static int do_full_update(gw_ota_t *o, const uint8_t *fw, uint32_t len,
                          uint16_t major, uint16_t minor, uint32_t crc)
{
    uint32_t off = 0u;

    if (gw_ota_begin(o, major, minor, len, crc) != 0) {
        return -1;
    }
    while (off < len) {
        uint32_t n = len - off;

        if (n > GW_OTA_MAX_CHUNK) {
            n = GW_OTA_MAX_CHUNK;
        }
        if (gw_ota_chunk(o, off, fw + off, n) != 0) {
            return -2;
        }
        off += n;
    }
    return gw_ota_end(o);
}

/* ==================================================================
 * 测试 1：成功的升级流程
 * ================================================================== */
static void test_happy_path(void)
{
    gw_ota_t       o;
    gw_ota_io_t    io;
    gw_ota_meta_t  m;
    static uint8_t fw[2048];
    uint32_t       crc;

    printf("\n[1] 成功的升级流程\n");

    flash_reset();
    make_io(&io);
    make_firmware(fw, sizeof(fw), 0x11u);
    crc = ref_crc32(fw, sizeof(fw));

    /* 当前跑在 A，所以目标是 B */
    if (gw_ota_init(&o, &io, GW_OTA_SLOT_B) != 0) {
        fail("OTA 初始化", "");
        return;
    }
    check_int("初始状态 IDLE", (int)gw_ota_state(&o), (int)GW_OTA_ST_IDLE);

    if (do_full_update(&o, fw, sizeof(fw), 1u, 2u, crc) == 0) {
        ok("完整升级流程通过");
    } else {
        fail("完整升级流程通过", "");
        return;
    }
    check_int("结束后状态 READY", (int)gw_ota_state(&o), (int)GW_OTA_ST_READY);
    check_int("无错误", (int)gw_ota_err(&o), (int)GW_OTA_ERR_NONE);

    /* 数据真的写进 B 分区了 */
    check_int("固件已写入 B 分区",
              memcmp(FL.app_b, fw, sizeof(fw)) == 0 ? 1 : 0, 1);
    /* A 分区没被动过 */
    {
        int untouched = 1;
        uint32_t i;
        for (i = 0u; i < sizeof(fw); i++) {
            if (FL.app_a[i] != FAKE_ERASED) { untouched = 0; break; }
        }
        check_int("A 分区完全没被改动（旧固件仍完整）", untouched, 1);
        ok("这是双分区的核心价值：切换之前旧固件一直是完整的");
    }

    /* ---- 元数据必须已经切到 B 且 pending = 1 ---- */
    if (gw_ota_meta_load(&io, &m) == 0) {
        check_int("元数据 active 已切到 B", (int)m.active, (int)GW_OTA_SLOT_B);
        check_int("⚠️ pending 已置位（**跳转前就置**，回滚的前提）",
                  (int)m.pending, 1);
        check_int("boot_tries 从 0 开始", (int)m.boot_tries, 0);
        check_int("pending_len 记录了长度",
                  (int)(m.pending_len == sizeof(fw)), 1);
        check_int("pending_crc 记录了 CRC",
                  (int)(m.pending_crc == crc), 1);
        ok("这一步是本模块最重要的设计：pending 在重启之前就落盘了");
    } else {
        fail("元数据可读", "");
    }
}

/* ==================================================================
 * 测试 2：⚠️ 新固件完全起不来 -> 必须回滚（本套测试的核心）
 * ================================================================== */
static void test_rollback_not_booting(void)
{
    gw_ota_t       o;
    gw_ota_io_t    io;
    gw_ota_meta_t  m;
    static uint8_t fw[1024];
    uint32_t       crc;
    int            i;

    printf("\n[2] 新固件完全起不来 -> 自动回滚（核心场景）\n");

    /* ---- 前置：先在 A 上建立一个"已确认"的稳定版本 ---- */
    flash_reset();
    make_io(&io);
    gw_ota_meta_make_default(&m, GW_OTA_SLOT_A);
    m.confirmed_len = 4096u;
    m.confirmed_crc = 0xDEADBEEFu;
    gw_ota_meta_seal(&m);
    (void)gw_ota_meta_save(&io, &m);

    /* ---- 升级到 B ---- */
    make_firmware(fw, sizeof(fw), 0x22u);
    crc = ref_crc32(fw, sizeof(fw));
    (void)gw_ota_init(&o, &io, GW_OTA_SLOT_B);
    if (do_full_update(&o, fw, sizeof(fw), 2u, 0u, crc) != 0) {
        fail("升级到 B", "");
        return;
    }

    /* ---- 模拟"新固件完全起不来" ----
       特征：Bootloader 每次跳过去，但新固件从来跑不到
       gw_ota_meta_confirm()。所以这里**只调用 boot_decide，从不 confirm**。 */
    {
        int rolled_back_at = -1;

        for (i = 0; i < 6; i++) {
            gw_ota_slot_t booted;

            (void)gw_ota_meta_load(&io, &m);
            booted = gw_ota_boot_decide(&m);
            (void)gw_ota_meta_save(&io, &m);    /* 模拟 Bootloader 写回 */

            if (booted == GW_OTA_SLOT_A) {
                rolled_back_at = i;
                break;
            }
        }

        if (rolled_back_at < 0) {
            fail("最终回滚到 A", "(6 次启动都没回滚)");
        } else {
            /* 第 0/1/2 次启动 B（3 次机会），第 3 次回滚 */
            check_int("回滚发生在第 4 次启动（给了 3 次机会）",
                      rolled_back_at, 3);
            ok("新固件反复起不来时自动回滚 —— 设备不会变砖");
        }
    }

    /* ---- 回滚后的元数据状态 ---- */
    (void)gw_ota_meta_load(&io, &m);
    check_int("回滚后 active = A", (int)m.active, (int)GW_OTA_SLOT_A);
    check_int("回滚后 pending 清 0", (int)m.pending, 0);
    check_int("回滚后 boot_tries 清零", (int)m.boot_tries, 0);
    check_int("回滚后 confirmed_len 仍是旧版本",
              (int)(m.confirmed_len == 4096u), 1);
    ok("回滚不会丢失'上一个已确认版本'的信息（能判断回滚是否成功）");

    /* ---- 回滚之后应该是稳定的：再启动几次都停在 A ---- */
    {
        int stable = 1;
        for (i = 0; i < 5; i++) {
            (void)gw_ota_meta_load(&io, &m);
            if (gw_ota_boot_decide(&m) != GW_OTA_SLOT_A) {
                stable = 0;
                break;
            }
            (void)gw_ota_meta_save(&io, &m);
        }
        check_int("回滚后持续稳定启动 A（不会来回跳）", stable, 1);
    }
}

/* ==================================================================
 * 测试 3：能起来但很快崩 -> 也必须回滚
 * ================================================================== */
static void test_rollback_crashes_after_boot(void)
{
    gw_ota_io_t   io;
    gw_ota_meta_t m;

    printf("\n[3] 新固件能起来但很快崩 -> 也要回滚\n");

    flash_reset();
    make_io(&io);

    /* 场景：新固件每次都能跑到 main，但运行 5 秒就崩。
       因为从没跑够 GW_OTA_CONFIRM_UPTIME_S（30 秒），
       所以 pending 一直不清 —— 试够次数后回滚。 */
    gw_ota_meta_make_default(&m, GW_OTA_SLOT_B);
    m.pending    = 1u;
    m.boot_tries = 0u;
    m.confirmed_len = 8192u;        /* A 上的稳定版本 */
    gw_ota_meta_seal(&m);
    (void)gw_ota_meta_save(&io, &m);

    /* 第 1 次启动：跑到第 5 秒就崩（never confirm） */
    (void)gw_ota_meta_load(&io, &m);
    check_int("运行 5 秒还不该确认（阈值 30 秒）",
              gw_ota_should_confirm(&m, 5u, GW_OTA_SLOT_B), 0);
    check_int("B 被启动（第 1 次）",
              (int)gw_ota_boot_decide(&m), (int)GW_OTA_SLOT_B);
    (void)gw_ota_meta_save(&io, &m);

    /* 反复几次 */
    for (;;) {
        gw_ota_slot_t s;

        (void)gw_ota_meta_load(&io, &m);
        s = gw_ota_boot_decide(&m);
        (void)gw_ota_meta_save(&io, &m);
        if (s == GW_OTA_SLOT_A) {
            break;
        }
    }
    (void)gw_ota_meta_load(&io, &m);
    check_int("'起来就崩'的固件也被回滚到 A",
              (int)m.active, (int)GW_OTA_SLOT_A);
    ok("判据是**稳定运行时长**而不是启动次数 —— 只数次数救不了这种固件");
}

/* ==================================================================
 * 测试 4：升级中断电 -> 旧固件仍可用
 * ================================================================== */
static void test_power_loss_midway(void)
{
    gw_ota_t       o;
    gw_ota_io_t    io;
    gw_ota_meta_t  m;
    static uint8_t fw[1024];
    uint32_t       crc;

    printf("\n[4] 升级写到一半断电 -> 旧固件必须还能用\n");

    flash_reset();
    make_io(&io);

    /* 先在 A 上建立已确认版本 */
    gw_ota_meta_make_default(&m, GW_OTA_SLOT_A);
    m.confirmed_len = 1024u;
    gw_ota_meta_seal(&m);
    (void)gw_ota_meta_save(&io, &m);

    /* 往 B 写一半就"断电"（提前 return，不调 end） */
    make_firmware(fw, sizeof(fw), 0x33u);
    crc = ref_crc32(fw, sizeof(fw));
    (void)gw_ota_init(&o, &io, GW_OTA_SLOT_B);
    (void)gw_ota_begin(&o, 3u, 0u, sizeof(fw), crc);
    (void)gw_ota_chunk(&o, 0u, fw, 512u);
    /* ---- 断电 ---- */

    /* 重启后：元数据从未被修改（因为 end 没执行） */
    (void)gw_ota_meta_load(&io, &m);
    check_int("断电后 active 仍是 A（元数据没被动过）",
              (int)m.active, (int)GW_OTA_SLOT_A);
    check_int("断电后 pending 仍是 0", (int)m.pending, 0);

    {
        gw_ota_slot_t booted = gw_ota_boot_decide(&m);
        check_int("断电后启动的仍是 A（旧固件完好）",
                  (int)booted, (int)GW_OTA_SLOT_A);
    }
    ok("关键：元数据只在 END 成功后才改 —— 中途断电不影响旧固件");

    /* A 分区内容确实没变 */
    {
        int untouched = 1;
        uint32_t i;
        for (i = 0u; i < 1024u; i++) {
            if (FL.app_a[i] != FAKE_ERASED) { untouched = 0; break; }
        }
        check_int("A 分区内容未被触碰", untouched, 1);
    }
}

/* ==================================================================
 * 测试 5：分片异常处理
 * ================================================================== */
static void test_chunk_errors(void)
{
    gw_ota_t       o;
    gw_ota_io_t    io;
    static uint8_t fw[1024];
    uint32_t       crc;

    printf("\n[5] 分片异常：重复、错位、越界、超长\n");

    flash_reset();
    make_io(&io);
    make_firmware(fw, sizeof(fw), 0x44u);
    crc = ref_crc32(fw, sizeof(fw));

    /* ---- 没 BEGIN 就来分片 ---- */
    (void)gw_ota_init(&o, &io, GW_OTA_SLOT_B);
    check_int("没 BEGIN 就发分片被拒",
              gw_ota_chunk(&o, 0u, fw, 128u), -1);
    check_int("错误码为 NO_BEGIN",
              (int)gw_ota_err(&o), (int)GW_OTA_ERR_NO_BEGIN);

    /* ---- 正常流程中注入异常 ---- */
    (void)gw_ota_init(&o, &io, GW_OTA_SLOT_B);
    (void)gw_ota_begin(&o, 1u, 0u, sizeof(fw), crc);

    check_int("第 1 片正常写入", gw_ota_chunk(&o, 0u, fw, 128u), 0);
    check_int("第 2 片正常写入", gw_ota_chunk(&o, 128u, fw + 128, 128u), 0);

    /* ---- 重复分片：返回 1（已忽略），不是错误 ---- */
    check_int("重复分片返回 1（已忽略，无需重传）",
              gw_ota_chunk(&o, 0u, fw, 128u), 1);
    check_int("重复分片不改变状态（仍在 DOWNLOAD）",
              (int)gw_ota_state(&o), (int)GW_OTA_ST_DOWNLOAD);
    check_int("重复分片不破坏已写入的数据",
              memcmp(FL.app_b, fw, 256u) == 0 ? 1 : 0, 1);
    ok("显式识别重复分片 —— 否则会撞 Flash 的'指能 1 变 0'限制，报误导性的写失败");

    /* ---- 错位分片 ---- */
    check_int("偏移不连续被拒",
              gw_ota_chunk(&o, 512u, fw + 512, 128u), -1);
    check_int("错误码为 OFFSET",
              (int)gw_ota_err(&o), (int)GW_OTA_ERR_OFFSET);
    ok("严格要求偏移连续 —— 正好能检出丢片与错位");

    /* ---- 越界（超过声明长度） ---- */
    flash_reset();
    (void)gw_ota_init(&o, &io, GW_OTA_SLOT_B);
    (void)gw_ota_begin(&o, 1u, 0u, 256u, crc);      /* 声明只有 256 字节 */
    (void)gw_ota_chunk(&o, 0u, fw, 128u);
    (void)gw_ota_chunk(&o, 128u, fw + 128, 128u);
    check_int("写满声明长度后再写被拒",
              gw_ota_chunk(&o, 256u, fw, 128u), -1);
    ok("越界写会破坏分区外的日志区/参数区（不可恢复），必须拦住");

    /* ---- 超长分片 ---- */
    flash_reset();
    (void)gw_ota_init(&o, &io, GW_OTA_SLOT_B);
    (void)gw_ota_begin(&o, 1u, 0u, 1024u, crc);
    check_int("超过 GW_OTA_MAX_CHUNK 的分片被拒",
              gw_ota_chunk(&o, 0u, fw, GW_OTA_MAX_CHUNK + 1u), -1);
    check_int("零长度分片被拒", gw_ota_chunk(&o, 0u, fw, 0u), -1);

    /* ---- BEGIN 参数校验 ---- */
    flash_reset();
    (void)gw_ota_init(&o, &io, GW_OTA_SLOT_B);
    check_int("零长度固件被拒", gw_ota_begin(&o, 1u, 0u, 0u, crc), -1);
    check_int("超过分区容量被拒",
              gw_ota_begin(&o, 1u, 0u, GW_APP_SLOT_SIZE + 1u, crc), -1);
    check_int("错误码为 TOO_BIG",
              (int)gw_ota_err(&o), (int)GW_OTA_ERR_TOO_BIG);
    ok("超容量必须拒绝 —— 否则会写到分区外面");

    /* ---- init 参数校验 ---- */
    check_int("空指针被拒", gw_ota_init(0, &io, GW_OTA_SLOT_B), -1);
    {
        gw_ota_io_t bad;

        make_io(&bad);
        bad.verify = 0;
        check_int("缺少 verify 回调被拒（强制读回比对）",
                  gw_ota_init(&o, &bad, GW_OTA_SLOT_B), -2);
        make_io(&bad);
        bad.erase = 0;
        check_int("缺少 erase 回调被拒",
                  gw_ota_init(&o, &bad, GW_OTA_SLOT_B), -2);
    }
    check_int("非法的目标分区被拒（禁止升级）",
              gw_ota_init(&o, &io, GW_OTA_SLOT_NONE), -3);
    ok("GW_OTA_SLOT_NONE 表示禁止升级（安全模式下用）");
}

/* ==================================================================
 * 测试 6：校验失败与 Flash 故障
 * ================================================================== */
static void test_verify_failures(void)
{
    gw_ota_t       o;
    gw_ota_io_t    io;
    static uint8_t fw[512];
    uint32_t       crc;

    printf("\n[6] 校验失败与 Flash 故障\n");

    /* ---- CRC 不匹配 ---- */
    flash_reset();
    make_io(&io);
    make_firmware(fw, sizeof(fw), 0x55u);
    crc = ref_crc32(fw, sizeof(fw));

    (void)gw_ota_init(&o, &io, GW_OTA_SLOT_B);
    (void)gw_ota_begin(&o, 1u, 0u, sizeof(fw), crc ^ 1u);   /* 故意错的 CRC（用 ^ 而不是 +：+ 会触发整数提升） */
    {
        uint32_t off;
        for (off = 0u; off < sizeof(fw); off += GW_OTA_MAX_CHUNK) {
            (void)gw_ota_chunk(&o, off, fw + off, GW_OTA_MAX_CHUNK);
        }
    }
    check_int("CRC 不匹配时 END 失败", gw_ota_end(&o), -1);
    check_int("错误码为 CRC_MISMATCH",
              (int)gw_ota_err(&o), (int)GW_OTA_ERR_CRC_MISMATCH);
    check_int("状态为 FAILED", (int)gw_ota_state(&o), (int)GW_OTA_ST_FAILED);

    /* ---- 长度不匹配 ---- */
    flash_reset();
    (void)gw_ota_init(&o, &io, GW_OTA_SLOT_B);
    (void)gw_ota_begin(&o, 1u, 0u, 512u, crc);
    (void)gw_ota_chunk(&o, 0u, fw, 128u);       /* 只写了 128，声明的 512 没写完 */
    check_int("长度不足时 END 失败", gw_ota_end(&o), -1);
    check_int("错误码为 LEN_MISMATCH",
              (int)gw_ota_err(&o), (int)GW_OTA_ERR_LEN_MISMATCH);

    /* ---- 元数据没被改动（升级失败不该影响旧固件） ---- */
    {
        gw_ota_meta_t m;
        gw_ota_io_t   io2;
        gw_ota_t      o2;

        flash_reset();
        make_io(&io2);
        gw_ota_meta_make_default(&m, GW_OTA_SLOT_A);
        (void)gw_ota_meta_save(&io2, &m);

        /* ⚠️ 用**新的** OTA 对象：上面那个 o 已经在 FAILED 状态，
           处于 FAILED 的对象再 begin 会被 NO_BEGIN 拒绝 ——
           那样测的就不是"失败路径不动元数据"了。 */
        (void)gw_ota_init(&o2, &io2, GW_OTA_SLOT_B);
        (void)gw_ota_begin(&o2, 1u, 0u, sizeof(fw), crc ^ 1u);
        {
            uint32_t off;
            for (off = 0u; off < sizeof(fw); off += GW_OTA_MAX_CHUNK) {
                (void)gw_ota_chunk(&o2, off, fw + off, GW_OTA_MAX_CHUNK);
            }
        }
        (void)gw_ota_end(&o2);                  /* 会失败 */

        (void)gw_ota_meta_load(&io2, &m);
        check_int("升级失败后元数据没被改动（旧固件仍是 active）",
                  (int)m.active, (int)GW_OTA_SLOT_A);
        check_int("升级失败后 pending 仍为 0", (int)m.pending, 0);
        ok("失败路径绝不能碰元数据 —— 否则一次失败就能让设备变砖");
    }

    /* ---- Flash 擦除失败 ---- */
    flash_reset();
    (void)gw_ota_init(&o, &io, GW_OTA_SLOT_B);
    FL.fail_erase_after_n = 1;
    check_int("擦除失败时 BEGIN 失败",
              gw_ota_begin(&o, 1u, 0u, sizeof(fw), crc), -1);
    check_int("错误码为 FLASH_ERASE",
              (int)gw_ota_err(&o), (int)GW_OTA_ERR_FLASH_ERASE);
    FL.fail_erase_after_n = 0;

    /* ---- Flash 写失败 ---- */
    flash_reset();
    (void)gw_ota_init(&o, &io, GW_OTA_SLOT_B);
    (void)gw_ota_begin(&o, 1u, 0u, sizeof(fw), crc);
    FL.fail_write_after_n = 1;
    check_int("写失败时分片被拒", gw_ota_chunk(&o, 0u, fw, 128u), -1);
    check_int("错误码为 FLASH_WRITE",
              (int)gw_ota_err(&o), (int)GW_OTA_ERR_FLASH_WRITE);
    FL.fail_write_after_n = 0;

    /* ---- 读回比对失败（"写成功但内容错"） ---- */
    flash_reset();
    (void)gw_ota_init(&o, &io, GW_OTA_SLOT_B);
    (void)gw_ota_begin(&o, 1u, 0u, sizeof(fw), crc);
    FL.corrupt_verify = 1;
    check_int("读回比对失败时也算写失败", gw_ota_chunk(&o, 0u, fw, 128u), -1);
    check_int("错误码为 FLASH_WRITE（不掩盖）",
              (int)gw_ota_err(&o), (int)GW_OTA_ERR_FLASH_WRITE);
    ok("强制读回比对 —— 有些驱动写失败时返回成功");
}

/* ==================================================================
 * 测试 7：元数据的自保护
 * ================================================================== */
static void test_meta_integrity(void)
{
    gw_ota_meta_t m;
    gw_ota_io_t   io;

    printf("\n[7] 元数据自身的完整性保护\n");

    flash_reset();
    make_io(&io);

    /* ---- 全 0xFF（未初始化的 Flash）必须被判为无效 ---- */
    {
        gw_ota_meta_t bad;

        memset(&bad, 0xFF, sizeof(bad));
        check_int("全 0xFF 的元数据无效", gw_ota_meta_valid(&bad), 0);
    }
    /* ---- 全 0x00 ---- */
    {
        gw_ota_meta_t bad;

        memset(&bad, 0x00, sizeof(bad));
        check_int("全 0x00 的元数据无效", gw_ota_meta_valid(&bad), 0);
    }
    /* ---- 200 组随机位模式 ---- */
    {
        uint32_t seed = 0x12345678u;
        int      i;
        int      bad_count = 0;

        for (i = 0; i < 200; i++) {
            gw_ota_meta_t r;
            uint8_t      *p = (uint8_t *)&r;
            size_t        k;

            for (k = 0; k < sizeof(r); k++) {
                seed = seed * 1103515245u + 12345u;
                p[k] = (uint8_t)(seed >> 16);
            }
            if (gw_ota_meta_valid(&r) != 0) {
                bad_count++;
            }
        }
        check_int("200 组随机位模式全部被判为无效", bad_count, 0);
        ok("元数据损坏时宁可重新初始化，也不要按错误布局误读（那会直接变砖）");
    }

    /* ---- 正常元数据必须通过 ---- */
    gw_ota_meta_make_default(&m, GW_OTA_SLOT_A);
    check_int("默认元数据有效", gw_ota_meta_valid(&m), 1);

    /* ---- 改一个字节后必须失效 ---- */
    {
        gw_ota_meta_t t = m;

        t.active = (uint8_t)GW_OTA_SLOT_B;      /* 改了但没重算 CRC */
        check_int("改了字段但没重算 CRC -> 判为无效",
                  gw_ota_meta_valid(&t), 0);
    }
    {
        gw_ota_meta_t t = m;

        t.confirmed_crc ^= 0x01u;
        check_int("位翻转 -> 判为无效（CRC 保护生效）",
                  gw_ota_meta_valid(&t), 0);
    }
    /* ---- 非法 active ---- */
    {
        gw_ota_meta_t t;

        gw_ota_meta_make_default(&t, GW_OTA_SLOT_A);
        t.active = 0x7Fu;
        gw_ota_meta_seal(&t);                   /* CRC 是对的 */
        check_int("active 非法时仍判为无效（不只看 CRC）",
                  gw_ota_meta_valid(&t), 0);
    }
    /* ---- 版本不符 ---- */
    {
        gw_ota_meta_t t;

        gw_ota_meta_make_default(&t, GW_OTA_SLOT_A);
        t.version = (uint16_t)(GW_OTA_META_VERSION + 1u);
        gw_ota_meta_seal(&t);
        check_int("版本不符判为无效（不按旧布局误读）",
                  gw_ota_meta_valid(&t), 0);
    }

    /* ---- 损坏的元数据读取应报错 ---- */
    {
        gw_ota_meta_t out;

        flash_reset();
        make_io(&io);
        gw_ota_meta_make_default(&m, GW_OTA_SLOT_A);
        (void)gw_ota_meta_save(&io, &m);
        FL.corrupt_meta = 1;
        check_int("读取损坏的元数据返回错误",
                  gw_ota_meta_load(&io, &out) != 0 ? 1 : 0, 1);
    }

    /* ---- 空指针安全 ---- */
    check_int("meta_valid 对空指针返回 0", gw_ota_meta_valid(0), 0);
    gw_ota_meta_seal(0);
    gw_ota_meta_make_default(0, GW_OTA_SLOT_A);
    gw_ota_meta_confirm(0, 0u, 0u);
    check_int("should_confirm 对空指针返回 0",
              gw_ota_should_confirm(0, 9999u, GW_OTA_SLOT_B), 0);
    check_int("boot_decide 对空指针返回 NONE",
              (int)gw_ota_boot_decide(0), (int)GW_OTA_SLOT_NONE);
    check_int("meta_load 对空指针返回 -1", gw_ota_meta_load(0, 0), -1);
    ok("元数据相关接口对空指针安全");
}

/* ==================================================================
 * 测试 8：确认与状态机边界
 * ================================================================== */
static void test_confirm_and_bounds(void)
{
    gw_ota_io_t   io;
    gw_ota_meta_t m;

    printf("\n[8] 确认成功与状态机边界\n");

    flash_reset();
    make_io(&io);

    /* ---- 确认流程 ---- */
    gw_ota_meta_make_default(&m, GW_OTA_SLOT_B);
    m.pending      = 1u;
    m.pending_len  = 10000u;
    m.pending_crc  = 0xABCD1234u;
    m.boot_tries   = 1u;
    m.confirmed_len = 5000u;
    gw_ota_meta_seal(&m);

    /* 还没跑够时间 */
    check_int("运行 29 秒还不确认",
              gw_ota_should_confirm(&m, 29u, GW_OTA_SLOT_B), 0);
    /* 跑够了 */
    check_int("运行 30 秒该确认",
              gw_ota_should_confirm(&m, 30u, GW_OTA_SLOT_B), 1);
    check_int("运行 100 秒也该确认（上限不封顶）",
              gw_ota_should_confirm(&m, 100u, GW_OTA_SLOT_B), 1);
    /* ⚠️ 缺陷 P：跑够时间也**必须**是我这一槽才认（见下面 [N] 用例） */
    check_int("跑够 100 秒但跑错槽 -> 不确认",
              gw_ota_should_confirm(&m, 100u, GW_OTA_SLOT_A), 0);
    check_int("自定位失败(NONE) -> 不确认",
              gw_ota_should_confirm(&m, 100u, GW_OTA_SLOT_NONE), 0);

    /* 执行确认 */
    gw_ota_meta_confirm(&m, 10000u, 0xABCD1234u);
    check_int("确认后 pending 清 0", (int)m.pending, 0);
    check_int("确认后 boot_tries 清零", (int)m.boot_tries, 0);
    check_int("确认后 confirmed 记录新版本",
              (int)(m.confirmed_len == 10000u), 1);
    check_int("确认后 active 不变（还是 B）",
              (int)m.active, (int)GW_OTA_SLOT_B);
    check_int("确认后不该再确认",
              gw_ota_should_confirm(&m, 999u, GW_OTA_SLOT_B), 0);

    /*
     * ⚠️ 确认结果必须**落盘**。
     *
     * 踩过的坑：忘了这一步，然后下面那个循环每次调 gw_ota_meta_load()
     * 都会把内存里的确认结果**覆盖回磁盘上的旧值**（pending 还是 1），
     * 于是 boot_decide 每 3 次就"回滚"一次 —— 测试报"不能稳定启动 B"。
     *
     * 这个坑恰好演示了真实场景的关键一步：确认必须写进元数据区，
     * 否则下次复位时 Bootloader 看到的还是"待确认"，
     * 会把它当成一个起不来的新固件而回滚 —— 用户看到的现象是
     * "升级成功了，但重启之后版本又变回去了"。
     */
    (void)gw_ota_meta_save(&io, &m);

    /* 确认之后 boot_decide 应该稳定启动 B，不再回滚 */
    {
        int i;
        int stable = 1;

        for (i = 0; i < 10; i++) {
            (void)gw_ota_meta_load(&io, &m);
            if (gw_ota_boot_decide(&m) != GW_OTA_SLOT_B) {
                stable = 0;
                break;
            }
        }
        check_int("确认后持续稳定启动 B（不再回滚）", stable, 1);
        ok("确认之后禁止回滚 —— 否则'运行中偶发重启'会被误判成升级失败");
    }

    /* ---- abort ---- */
    {
        gw_ota_io_t io2;
        gw_ota_t    o2;
        static uint8_t fw[512];
        uint32_t crc;

        flash_reset();
        make_io(&io2);
        make_firmware(fw, sizeof(fw), 0x66u);
        crc = ref_crc32(fw, sizeof(fw));

        (void)gw_ota_init(&o2, &io2, GW_OTA_SLOT_B);
        (void)gw_ota_begin(&o2, 1u, 0u, sizeof(fw), crc);
        (void)gw_ota_chunk(&o2, 0u, fw, 128u);
        (void)gw_ota_chunk(&o2, 128u, fw + 128, 128u);
        check_int("进行中有进度", (int)(o2.written > 0u), 1);

        gw_ota_abort(&o2);
        check_int("abort 后状态回 IDLE", (int)gw_ota_state(&o2), (int)GW_OTA_ST_IDLE);
        check_int("abort 后进度清零", (int)o2.written, 0);
        check_int("abort 后错误码清零",
                  (int)gw_ota_err(&o2), (int)GW_OTA_ERR_NONE);
        ok("abort 只复位状态机，不碰元数据（放弃一次升级不该影响已确认固件）");
    }

    /* ---- 分区工具 ---- */
    check_int("A 分区地址正确",
              (int)(gw_ota_slot_addr(GW_OTA_SLOT_A) == GW_APP_A_ADDR), 1);
    check_int("B 分区地址正确",
              (int)(gw_ota_slot_addr(GW_OTA_SLOT_B) == GW_APP_B_ADDR), 1);
    check_int("A 的另一个是 B",
              (int)gw_ota_slot_other(GW_OTA_SLOT_A), (int)GW_OTA_SLOT_B);
    check_int("B 的另一个是 A",
              (int)gw_ota_slot_other(GW_OTA_SLOT_B), (int)GW_OTA_SLOT_A);
    check_int("两个分区不重叠（A + SIZE == B）",
              (int)(GW_APP_A_ADDR + GW_APP_SLOT_SIZE == GW_APP_B_ADDR), 1);

    /* ---- golden 出厂镜像（2026-10-11 新增）----
     *
     * ⚠️ 这一组守的是"三个镜像区等大且互不重叠"这条布局不变量。
     *    布局写错（漏改一个 .ld、或 gw_flash_map.h 算错）在这里就会暴露，
     *    而不是等烧进去才发现"某个槽的镜像跑到别人的区域里"。
     */
    check_int("GOLDEN 分区地址正确",
              (int)(gw_ota_slot_addr(GW_OTA_SLOT_GOLDEN) == GW_APP_GOLDEN_ADDR), 1);
    check_int("三个镜像区等大（A + SIZE == B）",
              (int)(GW_APP_A_ADDR + GW_APP_SLOT_SIZE == GW_APP_B_ADDR), 1);
    check_int("三个镜像区等大（B + SIZE == GOLDEN）",
              (int)(GW_APP_B_ADDR + GW_APP_SLOT_SIZE == GW_APP_GOLDEN_ADDR), 1);
    check_int("golden 区不越进日志区",
              (int)(GW_APP_GOLDEN_ADDR + GW_APP_SLOT_SIZE <= GW_LOG_ADDR), 1);
    check_int("golden 的'另一个槽'是 NONE（它不参与 A/B 互备）",
              (int)gw_ota_slot_other(GW_OTA_SLOT_GOLDEN), (int)GW_OTA_SLOT_NONE);
    check_int("NONE 没有地址",
              (int)(gw_ota_slot_addr(GW_OTA_SLOT_NONE) == 0u), 1);

    /* slot_is_image：把"只认 A/B"的那处硬编码收成一个可测函数 */
    check_int("A/B/GOLDEN 都是真实镜像区",
              gw_ota_slot_is_image(GW_OTA_SLOT_A)
              + gw_ota_slot_is_image(GW_OTA_SLOT_B)
              + gw_ota_slot_is_image(GW_OTA_SLOT_GOLDEN), 3);
    check_int("NONE 不是镜像区",
              gw_ota_slot_is_image(GW_OTA_SLOT_NONE), 0);
    check_int("越界枚举值不是镜像区",
              gw_ota_slot_is_image((gw_ota_slot_t)7), 0);

    /* ---- 字符串 ---- */
    if (strcmp(gw_ota_state_str(GW_OTA_ST_READY), "READY") == 0
        && strcmp(gw_ota_err_str(GW_OTA_ERR_CRC_MISMATCH), "CRC_MISMATCH") == 0
        && strcmp(gw_ota_slot_str(GW_OTA_SLOT_B), "B") == 0
        && strcmp(gw_ota_slot_str(GW_OTA_SLOT_GOLDEN), "GOLDEN") == 0
        && gw_ota_state_str((gw_ota_state_t)99) != 0
        && gw_ota_err_str((gw_ota_err_t)99) != 0
        && gw_ota_slot_str((gw_ota_slot_t)99) != 0) {
        ok("字符串转换正确且有兜底");
    } else {
        fail("字符串转换正确且有兜底", "");
    }

    /* ---- 空指针安全（运行时接口） ---- */
    check_int("state 对空指针有兜底",
              (int)gw_ota_state(0), (int)GW_OTA_ST_FAILED);
    check_int("err 对空指针有兜底",
              (int)gw_ota_err(0), (int)GW_OTA_ERR_BAD_PARAM);
    gw_ota_abort(0);
    check_int("chunk 对空指针返回 -1", gw_ota_chunk(0, 0u, 0, 1u), -1);
    check_int("end 对空指针返回 -1", gw_ota_end(0), -1);
    check_int("begin 对空指针返回 -1", gw_ota_begin(0, 1u, 0u, 1u, 0u), -1);
    ok("状态机接口对空指针安全");
}

/* ==================================================================
 *  ⚠️ 回归用例：高层确认封装 gw_ota_confirm_running()
 *
 *  背景（真实缺陷）：
 *      上面 [8] 测的是**原语**：should_confirm / meta_confirm，然后
 *      手动调 gw_ota_meta_save()。原语本身都对。
 *
 *      但**应用固件里一个调用者都没有** —— 因为调用方要自己记住
 *      "改完必须 save"，而漏掉 save 不会有任何报错：只是 pending 永远不清。
 *      后果是**一个完全正常的新固件会在重启 3 次后被回滚掉**。
 *
 *      所以新增了把"读-判-改-落盘"封在一起的一步式接口，
 *      本用例测它，重点是那些**手写顺序容易漏掉的分支**。
 * ================================================================== */
static void test_confirm_running_api(void)
{
    gw_ota_io_t   io;
    gw_ota_meta_t m;
    int           r;

    printf("\n[N] 一步式确认封装 gw_ota_confirm_running()\n");

    flash_reset();
    make_io(&io);

    /* ---- 1) io 为空：必须返回 ERR 且不解引用 ---- */
    check_int("io 为空返回 ERR",
              (int)gw_ota_confirm_running(0, GW_OTA_SLOT_A, 999u, 0u, 0u),
              (int)GW_OTA_CONFIRM_R_ERR);

    /* ---- 2) 元数据还没有（空白 Flash / 首次上电）----
     *
     * ⚠️ 这里的期望是 **ERR**，不是 NONE。
     *    理由：`gw_ota_meta_load()` 在空白 Flash 上会**返回非 0**
     *    （magic 是 0xFF...，读不出合法记录），
     *    所以走的是"读不出来 -> ERR"这条分支。
     *
     *    实际影响：设备第一次上电（还没升级过）时，
     *    调用方会拿到 ERR。那时**没有 pending 可确认**，
     *    所以 ERR 与 NONE 的行为后果相同（都不改动 Flash）。
     *    但调用方若把 ERR 当成"出错了"去重试，会白跑。
     *
     *    保留这个行为并在这里说明，而不是把它改成 NONE 来美化测试：
     *    "读不出来"与"读出来了但没 pending"是两件事，
     *    混在一起会丢掉诊断信息。 */
    check_int("空白 Flash（读不出元数据）返回 ERR",
              (int)gw_ota_confirm_running(&io, GW_OTA_SLOT_A, 999u, 0u, 0u),
              (int)GW_OTA_CONFIRM_R_ERR);

    /* ---- 3) 有元数据但没有 pending：NONE ---- */
    gw_ota_meta_make_default(&m, GW_OTA_SLOT_A);
    (void)gw_ota_meta_save(&io, &m);
    check_int("pending=0 时返回 NONE",
              (int)gw_ota_confirm_running(&io, GW_OTA_SLOT_A, 999u, 0u, 0u),
              (int)GW_OTA_CONFIRM_R_NONE);

    /* ---- 4) 有 pending 但时间不够：NONE，且**不能改动 Flash** ---- */
    m.state       = (uint8_t)GW_OTA_META_VALID;
    m.active      = (uint8_t)GW_OTA_SLOT_B;
    m.pending     = 1u;
    m.boot_tries  = 1u;
    m.pending_len = 2048u;
    m.pending_crc = 0x11223344u;
    gw_ota_meta_seal(&m);
    (void)gw_ota_meta_save(&io, &m);

    check_int("运行 29 秒返回 NONE",
              (int)gw_ota_confirm_running(&io, GW_OTA_SLOT_B, 29u,
                                          2048u, 0x11223344u),
              (int)GW_OTA_CONFIRM_R_NONE);

    /* 关键：时间不够时**一个字节都不该动**（包括 boot_tries） */
    {
        gw_ota_meta_t back;
        (void)gw_ota_meta_load(&io, &back);
        check_int("时间不够时 pending 仍是 1（没动 Flash）",
                  (int)back.pending, 1);
        check_int("时间不够时 boot_tries 未被清（没动 Flash）",
                  (int)back.boot_tries, 1);
    }

    /* ---- 5) 时间到了：DONE，并且**真的落盘了** ---- */
    check_int("运行 30 秒返回 DONE",
              (int)gw_ota_confirm_running(&io, GW_OTA_SLOT_B, 30u,
                                          2048u, 0x11223344u),
              (int)GW_OTA_CONFIRM_R_DONE);

    /* ⚠️ 这一步是本用例的核心：**重新从 Flash 读**，而不是看内存变量。
         如果实现漏了 save，下面第一条就会失败。 */
    {
        gw_ota_meta_t back;
        (void)gw_ota_meta_load(&io, &back);
        check_int("⚠️ 确认已落盘（重读 pending=0）", (int)back.pending, 0);
        check_int("确认已落盘（重读 boot_tries=0）", (int)back.boot_tries, 0);
        check_int("confirmed_len 被写入", (int)back.confirmed_len, 2048);
        check_int("confirmed_crc 被写入",
                  (int)(back.confirmed_crc == 0x11223344u), 1);
        check_int("active 保持 B（确认不切分区）",
                  (int)back.active, (int)GW_OTA_SLOT_B);
        check_int("重读后元数据仍有效",
                  gw_ota_meta_valid(&back), 1);
    }

    /* ---- 6) 幂等：再调一次返回 NONE，不重复写 ---- */
    check_int("重复调用返回 NONE（幂等）",
              (int)gw_ota_confirm_running(&io, GW_OTA_SLOT_B, 100u,
                                          2048u, 0x11223344u),
              (int)GW_OTA_CONFIRM_R_NONE);

    /* ---- 7) ⚠️ 真实场景回归：确认之后重启多次，**不许回滚** ---- */
    {
        int rolled_back = 0;
        int i;

        for (i = 0; i < 10; i++) {
            gw_ota_meta_t mm;

            if (gw_ota_meta_load(&io, &mm) != 0) {
                rolled_back = 1;
                break;
            }
            if (gw_ota_boot_decide(&mm) != GW_OTA_SLOT_B) {
                rolled_back = 1;        /* 离开 B 就是回滚了 */
                break;
            }
            (void)gw_ota_meta_save(&io, &mm);   /* 模拟 Bootloader 写回 */
        }
        check_int("⚠️ 确认后连续 10 次启动都留在 B（不回滚）",
                  rolled_back, 0);
    }

    /* ---- 8) 元数据损坏：ERR，且不崩、不误改 ---- */
    {
        gw_ota_meta_t bad;

        flash_reset();
        make_io(&io);
        gw_ota_meta_make_default(&bad, GW_OTA_SLOT_A);
        bad.pending = 1u;
        gw_ota_meta_seal(&bad);
        (void)gw_ota_meta_save(&io, &bad);

        /* 破坏一个字节让 CRC 失效。
           ⚠️ 双页布局下要**两页都破坏**：只破坏一页的话，另一页可能是有效
              记录，被测代码会正常选中它 —— 那样这条用例就测不到"损坏"分支了。 */
        FL.meta[0][8] ^= 0xFFu;
        FL.meta[1][8] ^= 0xFFu;

        check_int("元数据损坏返回 ERR",
                  (int)gw_ota_confirm_running(&io, GW_OTA_SLOT_A, 999u, 0u, 0u),
                  (int)GW_OTA_CONFIRM_R_ERR);
    }

    /* ---- 9) 落盘失败：ERR，且不谎报成功 ---- */
    {
        gw_ota_meta_t mm;

        flash_reset();
        make_io(&io);
        gw_ota_meta_make_default(&mm, GW_OTA_SLOT_A);
        mm.pending = 1u;
        gw_ota_meta_seal(&mm);
        (void)gw_ota_meta_save(&io, &mm);

        /* 让下一次**元数据**写失败。
           ⚠️ 必须用 meta_write 的注入点，不能用 ff_write 的：
              元数据走的是 io->meta_write，**不经过** io->write。
              第一版就是这样写错的，结果注入没生效、
              测试报了三条失败。 */
        FL.fail_meta_write_after_n = FL.meta_write_calls + 1;

        r = (int)gw_ota_confirm_running(&io, GW_OTA_SLOT_A, 999u, 0u, 0u);
        check_int("落盘失败返回 ERR（不谎报成功）",
                  r, (int)GW_OTA_CONFIRM_R_ERR);

        /* Flash 里应当**仍是 pending=1**（写失败了，没改成） */
        FL.fail_meta_write_after_n = 0;
        {
            gw_ota_meta_t back;
            (void)gw_ota_meta_load(&io, &back);
            check_int("写失败后 Flash 里仍是 pending=1",
                      (int)back.pending, 1);
        }

        /* 恢复后重试应当成功（幂等，可重入） */
        check_int("恢复后重试成功",
                  (int)gw_ota_confirm_running(&io, GW_OTA_SLOT_A, 999u, 0u, 0u),
                  (int)GW_OTA_CONFIRM_R_DONE);
    }

    /* ---- 10) 状态字符串 ---- */
    check_int("NONE 字符串正确",
              (int)(strcmp(gw_ota_confirm_r_str(GW_OTA_CONFIRM_R_NONE),
                           "NONE") == 0), 1);
    check_int("DONE 字符串正确",
              (int)(strcmp(gw_ota_confirm_r_str(GW_OTA_CONFIRM_R_DONE),
                           "DONE") == 0), 1);
    check_int("ERR 字符串正确",
              (int)(strcmp(gw_ota_confirm_r_str(GW_OTA_CONFIRM_R_ERR),
                           "ERR") == 0), 1);
    /* ⚠️ 这一档**必须**有自己的字符串，绝不能被并进 NONE/ERR：
       它是"有升级要确认，但它不是我在跑的那份"（缺陷 P 的症状）。 */
    check_int("WRONG_SLOT 字符串正确（不与 NONE/ERR 合并）",
              (int)(strcmp(gw_ota_confirm_r_str(GW_OTA_CONFIRM_R_WRONG_SLOT),
                           "WRONG_SLOT") == 0), 1);

    /* ==================================================================
     * ---- 11) ⚠️⚠️ 缺陷 P 回归：**不是我跑的那一份，就不许确认** ----
     *
     *  真实场景（2026-10-11 板上实测复现）：
     *      升级写到 B（active=B, pending=1），但 B 的镜像校验不过（空/坏）
     *      -> 引导程序**降级启动 A 里的旧固件**
     *      -> 旧固件跑够 30 秒后调 confirm
     *      -> 若不做归属检查：pending 被清 0、boot_tries 归 0
     *         -> **回滚计数永远到不了 GW_OTA_MAX_BOOT_TRIES**
     *         -> 试用期保护被绕过，元数据还停在 "active=坏槽"
     *
     *  所以本用例的核心断言不是"返回码是多少"，而是
     *  **"被拒绝时 Flash 里 pending / boot_tries 一个字节都没变"**。
     * ================================================================== */
    {
        gw_ota_meta_t bk;

        flash_reset();
        make_io(&io);

        gw_ota_meta_make_default(&m, GW_OTA_SLOT_B);    /* 待确认的是 B */
        m.pending    = 1u;
        m.boot_tries = 1u;
        gw_ota_meta_seal(&m);
        (void)gw_ota_meta_save(&io, &m);

        /* ---- 11.1) 归属判据本身 ---- */
        check_int("active=B 时从 B 问 -> 是我的",
                  gw_ota_confirm_is_mine(&m, GW_OTA_SLOT_B), 1);
        check_int("active=B 时从 A 问 -> 不是我的",
                  gw_ota_confirm_is_mine(&m, GW_OTA_SLOT_A), 0);
        check_int("自定位失败(NONE) -> 一律不是我的",
                  gw_ota_confirm_is_mine(&m, GW_OTA_SLOT_NONE), 0);
        /* ⚠️ golden 也永远没有确认权：它是"两槽都坏时的兜底"，
           绝不是"某个待确认升级的运行者"。 */
        check_int("从 golden 跑起来 -> 也没有确认权",
                  gw_ota_confirm_is_mine(&m, GW_OTA_SLOT_GOLDEN), 0);
        check_int("空指针 -> 不是我的",
                  gw_ota_confirm_is_mine(0, GW_OTA_SLOT_B), 0);

        /* ---- 11.2) ★ 跑够 30 秒、但从**错误的槽**来确认 -> 必须被拒绝 ---- */
        check_int("★ 从 A（降级槽）跑够 30s -> WRONG_SLOT",
                  (int)gw_ota_confirm_running(&io, GW_OTA_SLOT_A, 999u, 0u, 0u),
                  (int)GW_OTA_CONFIRM_R_WRONG_SLOT);
        check_int("自定位失败(NONE) -> 也拒绝（不假装知道）",
                  (int)gw_ota_confirm_running(&io, GW_OTA_SLOT_NONE, 999u, 0u, 0u),
                  (int)GW_OTA_CONFIRM_R_WRONG_SLOT);

        /* ---- 11.3) ★ 被拒绝时**一个字节都不能改**（本用例存在的意义） ---- */
        (void)gw_ota_meta_load(&io, &bk);
        check_int("★ 拒绝确认后 pending 仍是 1（试用期保护没被绕过）",
                  (int)bk.pending, 1);
        check_int("★ 拒绝确认后 boot_tries 仍是 1（回滚计数没被清零）",
                  (int)bk.boot_tries, 1);

        /* ---- 11.4) 从**正确的槽**来问就应当成功 ---- */
        check_int("从 B（active 那个槽）确认 -> DONE",
                  (int)gw_ota_confirm_running(&io, GW_OTA_SLOT_B, 999u, 0u, 0u),
                  (int)GW_OTA_CONFIRM_R_DONE);
        (void)gw_ota_meta_load(&io, &bk);
        check_int("（对照）正确槽确认后 pending 才变 0",
                  (int)bk.pending, 0);

        /* ---- 11.5) 归属检查必须在**时间检查之前** ----
         *  否则"不是我的"会被混进 NONE —— 那看起来像"还没到时候"，
         *  把真正的问题（我跑错槽了）藏起来，日志里什么都看不出来。 */
        flash_reset();
        make_io(&io);
        gw_ota_meta_make_default(&m, GW_OTA_SLOT_B);
        m.pending = 1u;
        gw_ota_meta_seal(&m);
        (void)gw_ota_meta_save(&io, &m);
        check_int("时间不够 + 错槽 -> 仍是 WRONG_SLOT（不混进 NONE）",
                  (int)gw_ota_confirm_running(&io, GW_OTA_SLOT_A, 1u, 0u, 0u),
                  (int)GW_OTA_CONFIRM_R_WRONG_SLOT);

        /* ---- 11.6) pending=0（常态）时跑错槽是 NONE，不是 WRONG_SLOT ----
         *  没有升级要确认，就谈不上"被拒绝" —— 这两种情况必须区分，
         *  否则日志里会把常态刷成告警。 */
        m.pending = 0u;
        gw_ota_meta_seal(&m);
        (void)gw_ota_meta_save(&io, &m);
        check_int("pending=0 + 错槽 -> NONE（常态，不是被拒绝）",
                  (int)gw_ota_confirm_running(&io, GW_OTA_SLOT_A, 999u, 0u, 0u),
                  (int)GW_OTA_CONFIRM_R_NONE);

        /* ---- 11.7) 自定位在主机上必须如实回答"我不知道" ----
         *  主机地址不在任何槽区间里。**不许猜**：
         *  猜错的后果就是替别人签字。 */
        check_int("主机上 gw_ota_running_slot() 返回 NONE（不假装知道）",
                  (int)gw_ota_running_slot(), (int)GW_OTA_SLOT_NONE);
    }
}

/* ==================================================================
 * 测试 10：**普通重启零擦写**（磨损回归）
 *
 * ⚠️ 守护 2026-10-10 修掉的一个真问题：
 *    原来的 Bootloader 模板在决策之后**无条件**调用 gw_ota_meta_save()，
 *    而"写元数据" = **擦一整页（2 KB） + 写 32 字节** —— 磨损来自擦除，
 *    STM32 内部 Flash 一页寿命约 1 万次。于是：
 *        正常设备 1 天重启 1 次  -> 1 万天 ≈ 27 年（看似没事，但纯属浪费）
 *        **复位循环** 1 次/秒     -> 86400 次/天
 *                                 -> **约 3 小时磨穿这一页** ❌
 *
 *    修法：改用 gw_ota_boot_persist()，它比对"决策前后有没有变"，
 *          没变就完全不碰 Flash。
 *
 * 本用例用假 Flash 的写调用计数把这个不变量钉住 ——
 * 光靠"看代码觉得没问题"是守不住这种回归的。
 * ================================================================== */
static void test_no_write_on_normal_boot(void)
{
    gw_ota_io_t   io;
    gw_ota_meta_t m;
    int           saved;
    int           i;
    int           base;

    printf("\n[10] 普通重启零擦写（磨损回归）\n");

    flash_reset();
    make_io(&io);

    /* 常态：A 已确认、没有 pending */
    gw_ota_meta_make_default(&m, GW_OTA_SLOT_A);
    m.confirmed_len = 4096u;
    m.confirmed_crc = 0x12345678u;
    gw_ota_meta_seal(&m);
    (void)gw_ota_meta_save(&io, &m);

    base = FL.meta_write_calls;

    /* ---- 反复"普通启动" 100 次：一次都不该写 Flash ---- */
    for (i = 0; i < 100; i++) {
        (void)gw_ota_meta_load(&io, &m);
        saved = 42;                     /* 故意给错值，验证出参确实被写 */
        if (gw_ota_boot_persist(&io, &m, &saved) != GW_OTA_SLOT_A) {
            fail("常态启动应启动 A", "");
            return;
        }
        if (saved != 0) {
            fail("常态启动 out_saved 应为 0", "(报告写了 Flash)");
            return;
        }
    }
    check_int("100 次普通启动的 Flash 写次数", FL.meta_write_calls - base, 0);

    /* ---- 对照：带 pending 时**必须**落盘 ----
       （否则试用计数不前进 -> 坏固件永远不回滚，
         这是"省 Flash"与"能回滚"之间的关键平衡） */
    (void)gw_ota_meta_load(&io, &m);
    m.active     = (uint8_t)GW_OTA_SLOT_B;    /* 升级到了 B，正在试用 */
    m.pending    = 1u;
    m.boot_tries = 0u;
    gw_ota_meta_seal(&m);
    (void)gw_ota_meta_save(&io, &m);
    base = FL.meta_write_calls;

    for (i = 0; i < (int)GW_OTA_MAX_BOOT_TRIES; i++) {
        (void)gw_ota_meta_load(&io, &m);
        if (gw_ota_boot_persist(&io, &m, &saved) != GW_OTA_SLOT_B) {
            fail("试用期应启动 B", "");
            return;
        }
        if (saved != 1) {
            fail("试用期每次启动都必须落盘", "");
            return;
        }
    }
    check_int("试用期 3 次启动各写 1 次", FL.meta_write_calls - base, 3);

    /* ---- 第 4 次：试够了 -> 回滚到 A（也要落盘）---- */
    (void)gw_ota_meta_load(&io, &m);
    if (gw_ota_boot_persist(&io, &m, &saved) != GW_OTA_SLOT_A) {
        fail("试够次数后应回滚到 A", "");
        return;
    }
    check_int("回滚那一次也落盘", saved, 1);
    check_int("回滚后 pending 已清", (int)m.pending, 0);
    check_int("回滚后 boot_tries 归零", (int)m.boot_tries, 0);

    /* ---- 回滚之后又回到"零擦写"常态 ---- */
    base = FL.meta_write_calls;
    for (i = 0; i < 50; i++) {
        (void)gw_ota_meta_load(&io, &m);
        (void)gw_ota_boot_persist(&io, &m, &saved);
    }
    check_int("回滚后 50 次启动的写次数", FL.meta_write_calls - base, 0);

    /* ---- 参数健壮性 ---- */
    check_int("m 为空 -> NONE",
              (int)gw_ota_boot_persist(&io, 0, &saved), (int)GW_OTA_SLOT_NONE);
    check_int("io 为空 -> 退化为纯决策（不崩、不写）",
              (int)gw_ota_boot_persist(0, &m, &saved), (int)GW_OTA_SLOT_A);
}

/* ==================================================================
 * 测试 11：元数据双页 ping-pong 与"掉电不丢唯一副本"
 *
 * ⚠️ 这一组用例守的是**不变量**：任何一瞬间至少有一份有效元数据。
 *    元数据原来是单份，"擦一页 + 写几十字节"的那 20~40ms 窗口里掉电，
 *    记录就没了 -> 引导程序按出厂默认走 -> **跑回旧固件**：
 *    用户看到的是"升级明明成功了，断电一次就变回旧版本"。
 *
 *    能确定性验证它，靠的是"选页/选新在纯逻辑层、端口只提供读/擦/写一页" ——
 *    于是假介质可以把"擦到一半""只写了一半""某一页读出是坏的"逐点重放。
 * ================================================================== */


static void test_meta_pingpong(void)
{
    gw_ota_io_t   io;
    gw_ota_meta_t m;
    gw_ota_meta_t got;
    int           e0;
    int           i;

    printf("\n[11] 元数据双页 ping-pong\n");

    /* ---- 布局自证 ----
       ⚠️ tools/make_ota_meta.py 按**固定偏移**造假记录用于板上验证。
          结构体一改，那个工具的偏移就全错，而它自己的 decode 会用同一套
          错约定"自洽" —— 只有固件能戳穿（本项目真踩过：CRC 覆盖范围写错，
          工具自洽但 gw_ota_meta_valid() 返回 0）。
          所以在这里把偏移钉死，让漂移在 CI 就暴露。 */
    check_int("sizeof(gw_ota_meta_t) = 40", (int)sizeof(gw_ota_meta_t), 40);
    check_int("offsetof(seq) = 8", (int)offsetof(gw_ota_meta_t, seq), 8);
    check_int("offsetof(active) = 13", (int)offsetof(gw_ota_meta_t, active), 13);
    check_int("offsetof(pending) = 14", (int)offsetof(gw_ota_meta_t, pending), 14);
    check_int("offsetof(boot_tries) = 15", (int)offsetof(gw_ota_meta_t, boot_tries), 15);
    check_int("offsetof(meta_crc) = 36", (int)offsetof(gw_ota_meta_t, meta_crc), 36);

    /* ---- 1) 首次使用：两页全 FF -> 没有有效记录 ---- */
    flash_reset();
    make_io(&io);
    check_int("空介质上 load 失败（无有效记录）",
              (gw_ota_meta_load(&io, &got) == 0) ? 1 : 0, 0);

    /* ---- 2) 第一次落盘：写页 0、seq=1、恰好 1 擦 1 写 ---- */
    gw_ota_meta_make_default(&m, GW_OTA_SLOT_A);
    e0 = FL.meta_erase_calls;
    check_int("首次 save 返回成功", gw_ota_meta_save(&io, &m), 0);
    check_int("首次 save 擦 1 页", FL.meta_erase_calls - e0, 1);
    check_int("首次 save 写在页 0", (int)FL.last_write_page, 0);
    check_int("首次 load 成功", gw_ota_meta_load(&io, &got), 0);
    check_int("首次落盘后 seq = 1", (int)got.seq, 1);
    check_int("出厂默认不写盘（make_default 自身不落盘）",
              (FL.meta_write_calls == 1) ? 1 : 0, 1);

    /* ---- 3) 内容没变：**一次擦写都不做**，且 seq 不动 ----
       ⚠️ 这条是"常态启动零擦写"在 ping-pong 下的落点：
          seq 若被算进比较，"内容没变"就永远不成立。 */
    e0 = FL.meta_erase_calls;
    i  = FL.meta_write_calls;
    for (int k = 0; k < 100; k++) {
        (void)gw_ota_meta_save(&io, &m);
    }
    check_int("100 次内容不变的 save：擦除次数", FL.meta_erase_calls - e0, 0);
    check_int("100 次内容不变的 save：写次数", FL.meta_write_calls - i, 0);
    check_int("load 后 seq 仍是 1（没有偷偷递增）",
              (gw_ota_meta_load(&io, &got) == 0) ? (int)got.seq : -1, 1);

    /* ---- 4) 业务字段真变了：写另一页，且**两页都有效** ----
       ⚠️ "两页都有效"就是强不变量的直接体现：写完还留着上一代记录。 */
    m.pending = 1u;
    check_int("变化后 save 成功", gw_ota_meta_save(&io, &m), 0);
    check_int("第二次写在页 1（两页交替）", (int)FL.last_write_page, 1);
    check_int("页 0 仍是有效记录",
              gw_ota_meta_valid((const gw_ota_meta_t *)FL.meta[0]), 1);
    check_int("页 1 是新记录",
              gw_ota_meta_valid((const gw_ota_meta_t *)FL.meta[1]), 1);
    check_int("load 取到新的那份（seq=2）",
              (gw_ota_meta_load(&io, &got) == 0) ? (int)got.seq : -1, 2);

    /* ---- 5) 模拟"擦完旧页、还没写新页就掉电" ----
       目标页（页 0）被擦成 FF，页 1 的老记录必须仍然有效。 */
    flash_reset();
    make_io(&io);
    {
        gw_ota_meta_t a;
        gw_ota_meta_t b;

        gw_ota_meta_make_default(&a, GW_OTA_SLOT_A);        /* seq=0 */
        (void)gw_ota_meta_save(&io, &a);                    /* 页 0, seq=1 */
        b = a;
        b.pending = 1u;
        (void)gw_ota_meta_save(&io, &b);                    /* 页 1, seq=2 */
        /* 现在"当前"在页 1；下一次写的目标是页 0 —— 只擦不写 */
        check_int("擦掉旧页（模拟掉电在写入前）",
                  ff_meta_erase_page(0, 0u), 0);
        check_int("掉电后仍能 load（取自页 1）",
                  gw_ota_meta_load(&io, &got), 0);
        check_int("掉电后取到的仍是 seq=2", (int)got.seq, 2);
        check_int("掉电后 pending 状态没丢", (int)got.pending, 1);
    }

    /* ---- 6) 模拟"新记录只写了一半就掉电" ----
       半写的记录 CRC 不过 -> 判无效 -> 仍需取到上一代有效记录。 */
    flash_reset();
    make_io(&io);
    {
        gw_ota_meta_t a;

        gw_ota_meta_make_default(&a, GW_OTA_SLOT_A);
        (void)gw_ota_meta_save(&io, &a);                    /* 页 0, seq=1 */
        a.pending = 1u;
        FL.truncate_meta_write = 20;        /* 只落前 20 字节 */
        (void)gw_ota_meta_save(&io, &a);                    /* 页 1 半写 */
        FL.truncate_meta_write = 0;
        check_int("半写的那页 CRC 不过（判无效）",
                  gw_ota_meta_valid((const gw_ota_meta_t *)FL.meta[1]), 0);
        check_int("半写后仍能 load（回落到页 0）",
                  gw_ota_meta_load(&io, &got), 0);
        check_int("回落取到 seq=1", (int)got.seq, 1);
    }

    /* ---- 7) 两页都无效 -> 出厂默认（引导程序走 boot A） ---- */
    flash_reset();
    make_io(&io);
    check_int("两页全 FF：load 失败", (gw_ota_meta_load(&io, &got) == 0) ? 1 : 0, 0);

    /* ---- 8) seq 相同但内容矛盾（协同损坏）-> 取"不处于试用"的那份 ----
       ⚠️ 宁可跑旧的**已确认**固件，也不要因为损坏去跑没确认过的新固件。 */
    flash_reset();
    make_io(&io);
    {
        gw_ota_meta_t x;
        gw_ota_meta_t y;

        gw_ota_meta_make_default(&x, GW_OTA_SLOT_B);
        x.seq = 5u;
        x.pending = 1u;                     /* B 还在试用 */
        gw_ota_meta_seal(&x);
        gw_ota_meta_make_default(&y, GW_OTA_SLOT_A);
        y.seq = 5u;
        y.pending = 0u;                     /* A 已确认 */
        gw_ota_meta_seal(&y);
        meta_plant(0u, &x);
        meta_plant(1u, &y);
        check_int("seq 相同：load 成功", gw_ota_meta_load(&io, &got), 0);
        check_int("seq 相同：取到已确认的那份（active=A）",
                  (int)got.active, (int)GW_OTA_SLOT_A);
    }

    /* ---- 9) 版本不符（旧固件写的记录）-> 判无效，不猜不迁移 ---- */
    flash_reset();
    make_io(&io);
    {
        gw_ota_meta_t old;
        gw_ota_meta_t nw;

        gw_ota_meta_make_default(&old, GW_OTA_SLOT_A);
        old.version = GW_OTA_META_VERSION - 1u;     /* 模拟旧版本 */
        gw_ota_meta_seal(&old);                     /* CRC 自洽，但版本不对 */
        meta_plant(0u, &old);
        check_int("版本不符的记录判无效",
                  gw_ota_meta_valid((const gw_ota_meta_t *)FL.meta[0]), 0);
        check_int("只有旧版本记录时 load 失败",
                  (gw_ota_meta_load(&io, &got) == 0) ? 1 : 0, 0);

        gw_ota_meta_make_default(&nw, GW_OTA_SLOT_B);
        nw.seq = 1u;
        gw_ota_meta_seal(&nw);
        meta_plant(1u, &nw);
        check_int("旧版本 + 新版本共存：取新版本",
                  (gw_ota_meta_load(&io, &got) == 0)
                  && ((int)got.active == (int)GW_OTA_SLOT_B) ? 1 : 0, 1);
    }

    /* ---- 10) 磨损账：连续 10 次真实落盘 -> 两页各擦 5 次 ---- */
    flash_reset();
    make_io(&io);
    {
        gw_ota_meta_t mm;
        int c0 = 0;
        int c1 = 0;

        gw_ota_meta_make_default(&mm, GW_OTA_SLOT_A);
        (void)gw_ota_meta_save(&io, &mm);           /* 页 0 */
        if (FL.last_erase_page == 0u) { c0++; } else { c1++; }
        for (i = 0; i < 9; i++) {
            mm.boot_tries = (uint8_t)(i + 1u);      /* 制造业务字段变化 */
            (void)gw_ota_meta_save(&io, &mm);
            if (FL.last_erase_page == 0u) { c0++; } else { c1++; }
        }
        check_int("10 次落盘：页 0 被擦 5 次", c0, 5);
        check_int("10 次落盘：页 1 被擦 5 次", c1, 5);
        check_int("页 0 是有效记录",
                  gw_ota_meta_valid((const gw_ota_meta_t *)FL.meta[0]), 1);
        check_int("页 1 是有效记录",
                  gw_ota_meta_valid((const gw_ota_meta_t *)FL.meta[1]), 1);
    }

    /* ---- 11) 擦除失败：报错，但**不破坏**当前有效记录 ---- */
    flash_reset();
    make_io(&io);
    {
        gw_ota_meta_t a;

        gw_ota_meta_make_default(&a, GW_OTA_SLOT_A);
        (void)gw_ota_meta_save(&io, &a);
        a.pending = 1u;
        FL.fail_meta_erase_after_n = FL.meta_erase_calls + 1;   /* 下一次擦失败 */
        check_int("擦除失败时 save 返回错误",
                  (gw_ota_meta_save(&io, &a) == 0) ? 1 : 0, 0);
        FL.fail_meta_erase_after_n = 0;
        check_int("擦除失败后仍能 load", gw_ota_meta_load(&io, &got), 0);
        check_int("擦除失败后记录未被破坏", (int)got.pending, 0);
    }
}

int main(void)
{
    printf("========== OTA 双分区升级与回滚 单元测试 ==========\n");

    test_happy_path();
    test_rollback_not_booting();
    test_rollback_crashes_after_boot();
    test_power_loss_midway();
    test_chunk_errors();
    test_verify_failures();
    test_meta_integrity();
    test_confirm_and_bounds();
    test_confirm_running_api();
    test_no_write_on_normal_boot();
    test_meta_pingpong();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
