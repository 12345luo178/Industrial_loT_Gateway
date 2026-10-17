/**
 * @file    gw_ota.c
 * @brief   OTA 双分区升级与回滚实现（纯逻辑，可在 PC 上单元测试）
 */
#include "gw_ota.h"

#include "gw_crc32.h"

#include <string.h>

/* ==================================================================
 *  CRC32
 *
 *  ⚠️ 实现已抽到 `gw_crc32.c`，这里只做一层薄封装。
 *
 *  为什么必须共用而不是各写一份：
 *    Bootloader 跳转前也要校验固件完整性。如果两边各写一份，
 *    就可能出现"OTA 写入时用 A 算法、Bootloader 校验时用 B 算法"——
 *    症状是"每次升级都在最后一步失败"，而且两份代码各自看都对，
 *    极难定位。
 *
 *  所以：**算法只有一份**。
 * ================================================================== */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    return gw_crc32_update(crc, data, len);
}

/* 计算一段数据的 CRC32（含初值与末尾取反） */
static uint32_t crc32_calc(const uint8_t *data, uint32_t len)
{
    return gw_crc32(data, len);
}

/* ==================================================================
 *  内部：状态迁移
 * ================================================================== */
static void set_failed(gw_ota_t *o, gw_ota_err_t e)
{
    o->state = GW_OTA_ST_FAILED;
    o->err   = e;
}

/* ==================================================================
 *  分区工具
 * ================================================================== */

uint32_t gw_ota_slot_addr(gw_ota_slot_t slot)
{
    if (slot == GW_OTA_SLOT_A) {
        return GW_APP_A_ADDR;
    }
    if (slot == GW_OTA_SLOT_B) {
        return GW_APP_B_ADDR;
    }
    if (slot == GW_OTA_SLOT_GOLDEN) {
        return GW_APP_GOLDEN_ADDR;
    }
    return 0u;                      /* NONE：非法地址，调用方应检查 */
}

int gw_ota_slot_is_image(gw_ota_slot_t slot)
{
    return (slot == GW_OTA_SLOT_A
            || slot == GW_OTA_SLOT_B
            || slot == GW_OTA_SLOT_GOLDEN) ? 1 : 0;
}

gw_ota_slot_t gw_ota_slot_other(gw_ota_slot_t slot)
{
    if (slot == GW_OTA_SLOT_A) {
        return GW_OTA_SLOT_B;
    }
    if (slot == GW_OTA_SLOT_B) {
        return GW_OTA_SLOT_A;
    }
    return GW_OTA_SLOT_NONE;
}

/* ==================================================================
 *  元数据
 * ================================================================== */

void gw_ota_meta_seal(gw_ota_meta_t *m)
{
    if (m == 0) {
        return;
    }
    /* 先把自己清零再算，避免把上一次的 crc 也算进去 */
    m->meta_crc = 0u;
    m->meta_crc = crc32_calc((const uint8_t *)m, (uint32_t)sizeof(*m));
}

int gw_ota_meta_valid(const gw_ota_meta_t *m)
{
    gw_ota_meta_t tmp;
    uint32_t      crc;

    if (m == 0) {
        return 0;
    }
    if (m->magic != GW_OTA_META_MAGIC) {
        return 0;
    }
    if (m->version != GW_OTA_META_VERSION) {
        /* 版本不符：宁可当成无效重新初始化，也不要按旧布局误读。
           误读的后果可能是"active 指向一个随机分区" -> 直接变砖。 */
        return 0;
    }
    if (m->state != (uint8_t)GW_OTA_META_VALID) {
        return 0;
    }
    if (m->active != (uint8_t)GW_OTA_SLOT_A
        && m->active != (uint8_t)GW_OTA_SLOT_B) {
        return 0;
    }

    /* 自身 CRC 校验 */
    tmp = *m;
    tmp.meta_crc = 0u;
    crc = crc32_calc((const uint8_t *)&tmp, (uint32_t)sizeof(tmp));
    return (crc == m->meta_crc) ? 1 : 0;
}

/* ==================================================================
 *  元数据：双页 ping-pong 的选新与落盘
 *
 *  不变量（本文件所有分支都必须在它之下）：**任何一瞬间至少一份有效记录**。
 *  做法是"永远只擦 seq 较小的那一页"，于是每次写完两页都有效（一新一旧），
 *  不存在"正在写新记录、旧记录已经没了"的窗口。
 * ================================================================== */

_Static_assert(GW_OTA_META_PAGES == 2u,
               "ping-pong 的写目标按 cur_page ^ 1 推导，只适用于 2 页");
_Static_assert(sizeof(gw_ota_meta_t) == 40u,
               "元数据结构布局变了：必须递增 GW_OTA_META_VERSION 并同步文档");

/**
 * 规范化一条记录（强制标识字段 + 重算 CRC），并可指定 seq
 *
 * 用途：① 落盘前把调用方给的记录补齐（调用方不必关心 magic/version）
 *       ② **业务字段比对**时先把两边都规范化，才有可比性
 */
static void meta_normalize(gw_ota_meta_t *m, uint32_t seq)
{
    m->magic   = GW_OTA_META_MAGIC;
    m->version = GW_OTA_META_VERSION;
    m->state   = (uint8_t)GW_OTA_META_VALID;
    m->seq     = seq;
    gw_ota_meta_seal(m);
}

/**
 * 只比较**业务字段**（即除 seq / meta_crc 外的一切）
 *
 * ⚠️ 为什么必须排除 seq：seq 是"落盘代数"，只在真落盘时递增。
 *    如果把它算进比较，"内容没变"就永远不成立 -> 每次 save 都擦一页
 *    -> 常态启动每轮擦一页 -> 复位循环 1 次/秒 **约 3 小时磨穿一页**，
 *    正是 2026-10-10 刚修掉的那个缺陷会重新出现。
 *    （所以这里把 seq 归零后再各自 seal，然后比对整块字节。）
 */
static int meta_same_business(const gw_ota_meta_t *a, const gw_ota_meta_t *b)
{
    gw_ota_meta_t x;
    gw_ota_meta_t y;

    x = *a;
    y = *b;
    meta_normalize(&x, 0u);
    meta_normalize(&y, 0u);

    return (memcmp(&x, &y, sizeof(x)) == 0) ? 1 : 0;
}

/**
 * 读两页、各自校验、选出"当前有效"的那一份
 *
 * @param out      选出的记录（调用方保证非空）
 * @param out_page 选出的页号（可为 0）
 * @return 0 = 找到；<0 = 两页都无效（调用方按出厂默认处理）
 *
 * 选新规则：
 *   1) seq 大的赢
 *   2) seq 相同（只可能来自位翻转等协同损坏）-> 取 pending==0（已确认）的那份
 *      —— 宁可跑旧的**已确认**固件，也不要因为损坏去跑一个没确认过的新固件，
 *         后者才可能变砖
 *   3) 仍无法区分 -> 取页号小的（确定性行为，便于测试复现）
 */
static int meta_pick(const gw_ota_io_t *io, gw_ota_meta_t *out, uint8_t *out_page)
{
    gw_ota_meta_t m[GW_OTA_META_PAGES];
    int           ok[GW_OTA_META_PAGES];
    int           best = -1;
    int           i;

    for (i = 0; i < (int)GW_OTA_META_PAGES; i++) {
        ok[i] = 0;
        if (io->meta_read_page(io->ctx, (uint8_t)i,
                               (uint8_t *)&m[i],
                               (uint32_t)sizeof(m[i])) != 0) {
            continue;               /* 读失败：当成无效 */
        }
        if (gw_ota_meta_valid(&m[i]) == 0) {
            continue;               /* 读到但内容无效（空页/CRC 不过/版本不符） */
        }
        ok[i] = 1;
    }

    for (i = 0; i < (int)GW_OTA_META_PAGES; i++) {
        if (ok[i] == 0) {
            continue;
        }
        if (best < 0) {
            best = i;
            continue;
        }
        if (m[i].seq > m[best].seq) {
            best = i;
        } else if (m[i].seq == m[best].seq
                   && m[i].pending == 0u && m[best].pending != 0u) {
            best = i;
        }
    }

    if (best < 0) {
        return -1;
    }
    *out = m[best];
    if (out_page != 0) {
        *out_page = (uint8_t)best;
    }
    return 0;
}

void gw_ota_meta_make_default(gw_ota_meta_t *m, gw_ota_slot_t active)
{
    if (m == 0) {
        return;
    }
    memset(m, 0, sizeof(*m));
    m->active  = (uint8_t)((active == GW_OTA_SLOT_B)
                           ? GW_OTA_SLOT_B : GW_OTA_SLOT_A);
    m->pending = 0u;
    m->boot_tries = 0u;
    /* seq = 0 表示"还没有真实落盘过"；第一次 save 会把它变成 1。
       出厂默认本身**不写盘**（写了反而平白擦一次）。 */
    meta_normalize(m, 0u);
}

int gw_ota_meta_load(const gw_ota_io_t *io, gw_ota_meta_t *out)
{
    if (io == 0 || out == 0 || io->meta_read_page == 0) {
        return -1;
    }
    if (meta_pick(io, out, 0) != 0) {
        return -3;              /* 两页都无效：调用方按出厂默认处理 */
    }
    return 0;
}

int gw_ota_meta_save(const gw_ota_io_t *io, const gw_ota_meta_t *m)
{
    gw_ota_meta_t cur;
    gw_ota_meta_t tmp;
    uint8_t       cur_page = 0u;
    uint8_t       target;
    int           have;
    uint32_t      next_seq;

    if (io == 0 || m == 0
        || io->meta_read_page == 0
        || io->meta_erase_page == 0
        || io->meta_write_page == 0) {
        return -1;
    }

    have = meta_pick(io, &cur, &cur_page);

    /* ---- 主防线：业务字段没变就一次擦写都不做，且**不递增 seq** ----
       这条是"常态启动零擦写"的关键，也是端口层幂等兜底的前提。 */
    if (have == 0 && meta_same_business(&cur, m) != 0) {
        return 0;               /* 无操作即成功 */
    }

    next_seq = (have == 0) ? (cur.seq + 1u) : 1u;
    if (next_seq == 0u) {
        next_seq = 1u;          /* seq 回绕（4e9 次写，实际不可能）：跳过 0 */
    }

    tmp = *m;
    meta_normalize(&tmp, next_seq);

    /* ---- 写目标 = 另一页 ----
       它必然是"旧的那一份"所在页（或空页），所以擦掉它不会破坏唯一副本。 */
    target = (have == 0) ? (uint8_t)(cur_page ^ 1u) : 0u;

    if (io->meta_erase_page(io->ctx, target) != 0) {
        return -2;              /* 擦失败：当前有效记录仍在另一页，未受损 */
    }
    if (io->meta_write_page(io->ctx, target, (const uint8_t *)&tmp,
                            (uint32_t)sizeof(tmp)) != 0) {
        return -3;              /* 写失败/回读不一致：同上，仍是安全状态 */
    }
    return 0;
}

void gw_ota_meta_confirm(gw_ota_meta_t *m, uint32_t len, uint32_t crc)
{
    if (m == 0) {
        return;
    }

    /* 把"待确认"转成"已确认" */
    m->confirmed_len = len;
    m->confirmed_crc = crc;

    m->pending    = 0u;
    m->boot_tries = 0u;

    /* pending_len/crc 保留不清：它们记录了"上一个被确认的版本是什么"，
       对排查"为什么回滚了"有用。 */

    gw_ota_meta_seal(m);
}

/* ==================================================================
 *  我是谁：自定位（缺陷 P）
 *
 *  ⚠️ 只在**编译期事实**上下结论：本编译单元被链接到哪个槽。
 *     用"同编译单元里一个静态对象的地址"来问这个问题 ——
 *     它一定和本文件的代码在同一个镜像区间里。
 *
 *  ⚠️ 不用 `SCB->VTOR`：VTOR 说的是"向量表在哪"，它会被改
 *     （引导程序跳转前会设、应用也可能重定位），而"我这份代码被链接到哪"
 *     才是"我是谁"的可靠答案。
 *
 *  ⚠️ 判断不出来时返回 NONE（**如实回答"我不知道"**）：
 *     主机测试、旧单镜像构建（0x08000000）、与槽位化不一致的链接脚本。
 *     调用方必须把 NONE 当"无权确认"，不能当"大概是我"。
 * ================================================================== */
static const uint32_t s_slot_probe = 0u;    /* 只用来取地址 */

gw_ota_slot_t gw_ota_running_slot(void)
{
    const uint32_t here = (uint32_t)(uintptr_t)&s_slot_probe;

    if (here >= (uint32_t)GW_APP_A_ADDR
        && here < (uint32_t)(GW_APP_A_ADDR + GW_APP_SLOT_SIZE)) {
        return GW_OTA_SLOT_A;
    }
    if (here >= (uint32_t)GW_APP_B_ADDR
        && here < (uint32_t)(GW_APP_B_ADDR + GW_APP_SLOT_SIZE)) {
        return GW_OTA_SLOT_B;
    }
    /* ⚠️ 从 golden 跑起来时如实返回 GOLDEN（不是 NONE）：
       现场需要能看出"这台设备正在跑出厂兜底镜像"。
       ⚠️ 而归属于它的"确认权"仍然是 0 —— 见 gw_ota_confirm_is_mine()。 */
    if (here >= (uint32_t)GW_APP_GOLDEN_ADDR
        && here < (uint32_t)(GW_APP_GOLDEN_ADDR + GW_APP_SLOT_SIZE)) {
        return GW_OTA_SLOT_GOLDEN;
    }
    return GW_OTA_SLOT_NONE;
}

int gw_ota_confirm_is_mine(const gw_ota_meta_t *m, gw_ota_slot_t running)
{
    /* ⚠️⚠️ 缺陷 P 的判据（2026-10-11，板上实测复现过）
     *
     *    "待确认的升级"指的是元数据里 `active` 那个槽；而**我**是不是那个槽，
     *    只有一个可靠答案：我这份代码被链接到哪（`gw_ota_running_slot()`）。
     *
     *    为什么必须有这一条：引导程序在"目标槽镜像校验不过"时会
     *    **降级启动另一个槽**（`bootloader_main.c` 的 fallback，是设计内行为）。
     *    此时我 = 旧固件，`active` = 新槽。若旧固件替新槽确认：
     *        pending 被清 0、boot_tries 归 0
     *        -> **回滚计数永远到不了 GW_OTA_MAX_BOOT_TRIES**
     *        -> 试用期保护被绕过，元数据还会停在"active=坏槽"
     *
     *    `running == NONE` 也**必须**返回 0：
     *    宁可永不确认，也不能替一个不知道是谁的固件签字。 */
    if (m == 0 || gw_ota_meta_valid(m) == 0) {
        return 0;
    }
    if (running == GW_OTA_SLOT_NONE) {
        return 0;
    }
    /* ⚠️ golden 永远没有确认权（2026-10-11 加 golden 时补）：
       元数据里不可能出现 active=GOLDEN（meta_valid 会拒绝），所以下面那条
       比较本来就会返回 0 —— 但这里**显式**再挡一次：将来若有人放宽了
       meta_valid 的 active 范围，也不至于让"出厂兜底镜像"替别人签字。 */
    if (running == GW_OTA_SLOT_GOLDEN) {
        return 0;
    }
    if ((gw_ota_slot_t)m->active != running) {
        return 0;
    }
    return 1;
}

int gw_ota_should_confirm(const gw_ota_meta_t *m, uint32_t uptime_s,
                          gw_ota_slot_t running)
{
    if (m == 0 || gw_ota_meta_valid(m) == 0) {
        return 0;
    }
    if (m->pending == 0u) {
        return 0;                   /* 没有待确认的升级 */
    }
    /* ⚠️ 归属先于时间：**不是我的事，等多久都不确认**（缺陷 P） */
    if (gw_ota_confirm_is_mine(m, running) == 0) {
        return 0;
    }
    /* 判据是**稳定运行时长**，不是启动次数 ——
       只数次数救不了"能起来但起来就崩"的固件。 */
    return (uptime_s >= GW_OTA_CONFIRM_UPTIME_S) ? 1 : 0;
}

/* ==================================================================
 *  高层封装：跑够时间就把这次的升级确认为可用
 *
 *  ⚠️ 为什么需要它（这是一个真实缺陷的修复）
 *
 *    上面那三个原语各管一段：
 *        gw_ota_meta_load()      读
 *        gw_ota_should_confirm() 判
 *        gw_ota_meta_confirm()   改内存里的结构
 *        gw_ota_meta_save()      落盘
 *
 *    而**没有任何调用方**把它们串起来（应用固件里一处都没有）。
 *    后果：升级到 B 之后 pending 永远是 1，于是每重启一次 boot_tries 加一，
 *    累计 3 次就**把一个完全正常的新固件回滚掉**。
 *
 *    根因不是"忘了写代码"，而是**接口层次不对**：
 *    调用方要自己记住"改完必须 save"，而漏掉 save 不会有任何报错 ——
 *    只是 pending 永远不清。这种"漏一步就静默失效"的接口迟早被漏掉。
 *
 *    所以这里提供一个**一步到位**的函数，调用方只知道
 *    "我跑够时间了，把升级确认掉"，不需要了解内部有几步。
 *
 *  ----------------------------------------------------------------------------
 *  ⚠️ 关于 len / crc 参数（如实说明）
 *
 *    它们被写进元数据的 confirmed_len / confirmed_crc。
 *    **当前没有任何代码读这两个字段做判断**（它们只是诊断信息）：
 *      回滚靠的是 pending + boot_tries + active，
 *      校验靠的是 pending_len / pending_crc。
 *
 *    所以传"本分区声明的长度与 CRC"（也就是升级时写进去的那对值）
 *    在功能上是正确的。传 0 也不影响回滚逻辑，但会让诊断信息失去意义。
 *
 *    ⚠️ 如果将来要让"确认"具备防篡改含义（确认的必须正是此刻在跑的
 *       那份二进制），就需要一份"运行中固件自身的长度与 CRC"——
 *       那要用链接期符号（如 `_etext`）或构建期生成的常量。
 *       当前项目**没有**提供它，因为编译期算不出自己的大小。
 *       这一点写在 README 的已知限制里，此处不再假装具备。
 * ================================================================== */
gw_ota_confirm_r_t gw_ota_confirm_running(const gw_ota_io_t *io,
                                          gw_ota_slot_t running,
                                          uint32_t uptime_s,
                                          uint32_t len, uint32_t crc)
{
    gw_ota_meta_t m;
    int           r;

    if (io == 0) {
        return GW_OTA_CONFIRM_R_ERR;
    }

    r = gw_ota_meta_load(io, &m);
    if (r != 0) {
        return GW_OTA_CONFIRM_R_ERR;    /* 读不出来：不动它 */
    }
    if (gw_ota_meta_valid(&m) != 1) {
        return GW_OTA_CONFIRM_R_ERR;    /* 元数据不可信：不动它 */
    }
    if (m.pending == 0u) {
        return GW_OTA_CONFIRM_R_NONE;   /* 没有待确认的升级 —— 这是常态 */
    }

    /* ⚠️⚠️ 归属检查必须在**时间检查之前**，而且必须**单独成一档返回**（缺陷 P）
     *
     *    放在时间检查之后也行不通：那样"不是我的"会被混进 NONE，
     *    看起来像"还没到时候"，把真正的问题（我跑错槽了）藏起来。
     *    单独一档才能让日志/上位机说出"确认被拒绝：这不是我在跑的那份"。 */
    if (gw_ota_confirm_is_mine(&m, running) == 0) {
        return GW_OTA_CONFIRM_R_WRONG_SLOT;
    }

    if (gw_ota_should_confirm(&m, uptime_s, running) == 0) {
        return GW_OTA_CONFIRM_R_NONE;   /* 时间还不够，下次再问 */
    }

    /* ---- 确认 ---- */
    gw_ota_meta_confirm(&m, len, crc);

    if (gw_ota_meta_save(io, &m) != 0) {
        /* ⚠️ 落盘失败：如实报告。此刻内存里的 m 已经是"已确认"，
           但 Flash 里仍是 pending —— 下一次启动会再走一遍确认流程。
           这是安全的（幂等），但不能谎报成功。 */
        return GW_OTA_CONFIRM_R_ERR;
    }

    return GW_OTA_CONFIRM_R_DONE;
}

const char *gw_ota_confirm_r_str(gw_ota_confirm_r_t r)
{
    switch (r) {
    case GW_OTA_CONFIRM_R_NONE: return "NONE";
    case GW_OTA_CONFIRM_R_DONE: return "DONE";
    /* ⚠️ 单独一档，绝不与 NONE 合并：这一档代表"**有**待确认的升级，
       但它不是我在跑的那份" —— 是缺陷 P 的症状，必须能被日志/上位机看见。 */
    case GW_OTA_CONFIRM_R_WRONG_SLOT: return "WRONG_SLOT";
    default:                    return "ERR";
    }
}

gw_ota_slot_t gw_ota_boot_decide(gw_ota_meta_t *m)
{
    if (m == 0 || gw_ota_meta_valid(m) == 0) {
        return GW_OTA_SLOT_NONE;
    }

    if (m->pending == 0u) {
        /* 常态：启动已确认的分区 */
        return (gw_ota_slot_t)m->active;
    }

    /* ---- 有待确认的新固件 ---- */

    if (m->boot_tries < (uint8_t)GW_OTA_MAX_BOOT_TRIES) {
        /* 还给它机会。

           ⚠️ 计数在**返回之前**就已经加过了 —— 调用方负责把它写回 Flash。
              这是"必须在跳转前写回"的体现：如果等新固件跑起来再写，
              一个"连跳转都过不去"的坏固件会让计数永远停在 0。 */
        m->boot_tries++;
        return (gw_ota_slot_t)m->active;
    }

    /* ---- 试够了，回滚 ----
       把 active 切回另一个分区。切换之后 pending 清 0，
       因为此时"待确认的固件"已经不打算再启动了。 */
    m->active     = (uint8_t)gw_ota_slot_other((gw_ota_slot_t)m->active);
    m->pending    = 0u;
    m->boot_tries = 0u;
    gw_ota_meta_seal(m);

    return (gw_ota_slot_t)m->active;
}

/* ==================================================================
 *  启动决策 + 只在必要时落盘
 *
 *  ⚠️ 这个函数存在的唯一理由是**磨损**：元数据的一次"写"是
 *     "擦一整页 + 写 32 字节"，而 Flash 一页寿命约 1 万次。
 *     如果无条件落盘，复位循环（1 次/秒）会在约 3 小时内磨穿这一页。
 *     详见头文件里 gw_ota_boot_persist() 的说明。
 *
 *  判据用"调用前 memcmp"而不是让 decide 报告 ——
 *  这样 decide 以后新增任何字段修改都自动被覆盖。
 * ================================================================== */
gw_ota_slot_t gw_ota_boot_persist(const gw_ota_io_t *io, gw_ota_meta_t *m,
                                  int *out_saved)
{
    gw_ota_meta_t before;
    gw_ota_slot_t target;
    int           changed;

    if (out_saved != 0) {
        *out_saved = 0;
    }
    if (m == 0) {
        return GW_OTA_SLOT_NONE;
    }

    /* 没有落盘能力（io 或分页写回调为空）：退化成纯决策。
       调用方要自己承担"计数没写回"的后果 —— 见 gw_ota.h 的说明。 */
    if (io == 0 || io->meta_write_page == 0 || io->meta_erase_page == 0) {
        return gw_ota_boot_decide(m);
    }

    before = *m;
    target = gw_ota_boot_decide(m);

    changed = (memcmp(&before, m, sizeof(before)) != 0) ? 1 : 0;
    if (changed == 0) {
        /* 常态：pending == 0，一个字段都没改 -> 一次擦写都不做。
           这是"普通重启零磨损"的落点。 */
        return target;
    }

    if (gw_ota_meta_save(io, m) != 0) {
        if (out_saved != 0) {
            *out_saved = -1;        /* 如实报告：回滚计数这次不可靠 */
        }
        return target;
    }

    if (out_saved != 0) {
        *out_saved = 1;
    }
    return target;
}

/* ==================================================================
 *  下载状态机
 * ================================================================== */

int gw_ota_init(gw_ota_t *o, const gw_ota_io_t *io, gw_ota_slot_t target)
{
    if (o == 0 || io == 0) {
        return -1;
    }
    /* 这些都是必须的：
       erase / write / verify（镜像读写）
       meta_read_page / meta_erase_page / meta_write_page（元数据双页 ping-pong） */
    if (io->erase == 0 || io->write == 0 || io->verify == 0
        || io->meta_read_page == 0 || io->meta_erase_page == 0
        || io->meta_write_page == 0) {
        return -2;
    }
    if (target != GW_OTA_SLOT_A && target != GW_OTA_SLOT_B) {
        return -3;                  /* 目标分区非法：禁止升级 */
    }

    memset(o, 0, sizeof(*o));
    o->io     = *io;
    o->target = target;
    o->state  = GW_OTA_ST_IDLE;
    o->err    = GW_OTA_ERR_NONE;
    o->inited = 1u;
    return 0;
}

gw_ota_state_t gw_ota_state(const gw_ota_t *o)
{
    return (o == 0) ? GW_OTA_ST_FAILED : o->state;
}

gw_ota_err_t gw_ota_err(const gw_ota_t *o)
{
    return (o == 0) ? GW_OTA_ERR_BAD_PARAM : o->err;
}

void gw_ota_abort(gw_ota_t *o)
{
    if (o == 0 || o->inited == 0u) {
        return;
    }
    /* 只复位状态机，**不碰元数据** ——
       已确认的固件必须保持可用，放弃一次升级不该影响它。 */
    o->state       = GW_OTA_ST_IDLE;
    o->err         = GW_OTA_ERR_NONE;
    o->written     = 0u;
    o->running_crc = 0xFFFFFFFFu;
    o->chunks      = 0u;
    o->retries     = 0u;
    o->total_len   = 0u;
    o->expect_crc  = 0u;
}

int gw_ota_begin(gw_ota_t *o, uint16_t major, uint16_t minor,
                 uint32_t total_len, uint32_t crc32)
{
    uint32_t addr;

    if (o == 0 || o->inited == 0u) {
        return -1;
    }

    /* ---- 参数校验 ----
       0 长度的固件一定是错的；
       超过分区容量的必须拒绝 —— 否则会写到分区外面，
       把后面的日志区或参数区破坏掉（那是不可恢复的）。 */
    if (total_len == 0u || total_len > GW_APP_SLOT_SIZE) {
        set_failed(o, (total_len > GW_APP_SLOT_SIZE)
                      ? GW_OTA_ERR_TOO_BIG : GW_OTA_ERR_BAD_PARAM);
        return -1;
    }

    addr = gw_ota_slot_addr(o->target);
    if (addr == 0u) {
        set_failed(o, GW_OTA_ERR_BAD_PARAM);
        return -1;
    }

    /* ---- 擦除目标分区 ----
       ⚠️ 擦除放在 BEGIN 而不是 END：
          136 KB 要擦 68 页，F1 一页 20~40ms，合计 1.4~2.7 秒。
          如果放在 END，那 1.4~2.7 秒发生在"已经收完、就等 ACK"的时刻，
          上位机会以为超时。放在 BEGIN 则可以在 ACK 之前完成，
          而且此时还没有数据丢失风险。 */
    if (o->io.erase(o->io.ctx, addr, GW_APP_SLOT_SIZE) != 0) {
        set_failed(o, GW_OTA_ERR_FLASH_ERASE);
        return -1;
    }

    o->state       = GW_OTA_ST_DOWNLOAD;
    o->err         = GW_OTA_ERR_NONE;
    o->fw_major    = major;
    o->fw_minor    = minor;
    o->total_len   = total_len;
    o->expect_crc  = crc32;
    o->written     = 0u;
    o->chunks      = 0u;
    o->retries     = 0u;
    o->running_crc = 0xFFFFFFFFu;   /* CRC32 的初值 */

    return 0;
}

int gw_ota_chunk(gw_ota_t *o, uint32_t offset, const uint8_t *data, uint32_t len)
{
    uint32_t addr;

    if (o == 0 || o->inited == 0u || data == 0) {
        return -1;
    }
    if (o->state != GW_OTA_ST_DOWNLOAD) {
        set_failed(o, GW_OTA_ERR_NO_BEGIN);
        return -1;
    }
    if (len == 0u || len > GW_OTA_MAX_CHUNK) {
        set_failed(o, GW_OTA_ERR_BAD_PARAM);
        return -1;
    }

    /* ---- 重复分片：显式忽略并返回 1 ----
       ⚠️ 为什么必须显式处理，不能让它去撞 Flash 的写入限制：
          串口链路上偶尔会重传。同一片收到两次时，第二次会写到
          同一个地址 —— 而对已经写过的 Flash 再写同样的值，
          在硬件上是"1 变 0"的操作，写同样的值理论上允许，
          但实际会让"目标必须已擦除"的检查失败，
          从而报一个**误导性的 Flash 写失败**，
          掩盖了"其实只是重传"这个真相。

       判据：offset + len <= written 说明整片都已经写过了。 */
    if (offset + len <= o->written) {
        o->retries++;
        return 1;
    }

    /* ---- 偏移必须连续 ----
       要求 offset 恰好等于已写入的字节数。
       这比"允许任意偏移"更严格，但正好能检出丢片与错位 ——
       那两种情况都会让固件损坏，而 CRC 要到很晚才发现。 */
    if (offset != o->written) {
        set_failed(o, GW_OTA_ERR_OFFSET);
        return -1;
    }

    /* ---- 不允许越界 ----
       越界会写到分区外面，破坏日志区或参数区（不可恢复）。 */
    if (o->written + len > o->total_len
        || o->written + len > GW_APP_SLOT_SIZE) {
        set_failed(o, GW_OTA_ERR_TOO_BIG);
        return -1;
    }

    addr = gw_ota_slot_addr(o->target) + o->written;

    if (o->io.write(o->io.ctx, addr, data, len) != 0) {
        set_failed(o, GW_OTA_ERR_FLASH_WRITE);
        return -1;
    }
    /* 读回比对：防止"驱动报成功但内容错" */
    if (o->io.verify(o->io.ctx, addr, data, len) != 0) {
        set_failed(o, GW_OTA_ERR_FLASH_WRITE);
        return -1;
    }

    o->running_crc = crc32_update(o->running_crc, data, len);
    o->written    += len;
    o->chunks++;

    return 0;
}

int gw_ota_end(gw_ota_t *o)
{
    gw_ota_meta_t m;
    uint32_t      final_crc;

    if (o == 0 || o->inited == 0u) {
        return -1;
    }
    if (o->state != GW_OTA_ST_DOWNLOAD) {
        set_failed(o, GW_OTA_ERR_NO_BEGIN);
        return -1;
    }

    o->state = GW_OTA_ST_VERIFY;

    /* ---- 长度比对 ---- */
    if (o->written != o->total_len) {
        set_failed(o, GW_OTA_ERR_LEN_MISMATCH);
        return -1;
    }

    /* ---- CRC32 比对 ---- */
    final_crc = o->running_crc ^ 0xFFFFFFFFu;
    if (final_crc != o->expect_crc) {
        set_failed(o, GW_OTA_ERR_CRC_MISMATCH);
        return -1;
    }

    /* ---- 写元数据：active = 新分区，pending = 1 ----
       ⚠️⚠️ 这是整个回滚机制的关键一步，**必须在重启之前完成**。
           详见 gw_ota.h 文件头的说明：
           如果等新固件跑起来再置 pending，那么"跑不起来的新固件"
           会让设备无限重启（因为 Bootloader 看到没有 pending，
           每次都跳进那个坏固件）。

       ⚠️ active 与 pending 必须**在同一次写入里**落盘。
          分两次写的话，中间断电会留下"active 已切、pending 未置"
          的状态 —— 那正是上面描述的无限重启场景。 */
    if (gw_ota_meta_load(&o->io, &m) != 0) {
        /* 元数据不存在（首次升级）或已损坏：建一个默认的。
           ⚠️ 默认的 active 要设成**当前正在运行的那个分区**，
              否则会把"应该启动哪个"搞错。 */
        gw_ota_meta_make_default(&m, gw_ota_slot_other(o->target));
    }

    m.state        = (uint8_t)GW_OTA_META_VALID;
    m.active       = (uint8_t)o->target;
    m.pending      = 1u;
    m.boot_tries   = 0u;            /* 从 0 开始数 */
    m.pending_len  = o->total_len;
    m.pending_crc  = o->expect_crc;
    /* confirmed_* 保持不变：它记录的是"上一个被确认的固件"，
       也就是回滚时的目标。清掉它会让回滚后无法判断回滚是否成功。 */
    gw_ota_meta_seal(&m);

    if (gw_ota_meta_save(&o->io, &m) != 0) {
        set_failed(o, GW_OTA_ERR_FLASH_WRITE);
        return -1;
    }

    o->state = GW_OTA_ST_READY;
    o->err   = GW_OTA_ERR_NONE;
    return 0;
}

/* ==================================================================
 *  字符串
 * ================================================================== */

const char *gw_ota_state_str(gw_ota_state_t st)
{
    switch (st) {
    case GW_OTA_ST_IDLE:     return "IDLE";
    case GW_OTA_ST_DOWNLOAD: return "DOWNLOAD";
    case GW_OTA_ST_VERIFY:   return "VERIFY";
    case GW_OTA_ST_READY:    return "READY";
    case GW_OTA_ST_FAILED:   return "FAILED";
    default:                 return "?";
    }
}

const char *gw_ota_err_str(gw_ota_err_t e)
{
    switch (e) {
    case GW_OTA_ERR_NONE:         return "NONE";
    case GW_OTA_ERR_NO_BEGIN:     return "NO_BEGIN";
    case GW_OTA_ERR_BAD_PARAM:    return "BAD_PARAM";
    case GW_OTA_ERR_TOO_BIG:      return "TOO_BIG";
    case GW_OTA_ERR_OFFSET:       return "OFFSET";
    case GW_OTA_ERR_FLASH_WRITE:  return "FLASH_WRITE";
    case GW_OTA_ERR_FLASH_ERASE:  return "FLASH_ERASE";
    case GW_OTA_ERR_LEN_MISMATCH: return "LEN_MISMATCH";
    case GW_OTA_ERR_CRC_MISMATCH: return "CRC_MISMATCH";
    case GW_OTA_ERR_SAME_SLOT:    return "SAME_SLOT";
    default:                      return "?";
    }
}

const char *gw_ota_slot_str(gw_ota_slot_t s)
{
    switch (s) {
    case GW_OTA_SLOT_A:      return "A";
    case GW_OTA_SLOT_B:      return "B";
    case GW_OTA_SLOT_GOLDEN: return "GOLDEN";
    case GW_OTA_SLOT_NONE:   return "NONE";
    default:                 return "?";
    }
}
