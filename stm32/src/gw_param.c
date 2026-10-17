/**
 * @file    gw_param.c
 * @brief   Flash 参数掉电保存实现（纯逻辑，可在 PC 上单元测试）
 */
#include "gw_param.h"

#include <string.h>

/* ------------------------------------------------------------------
 * CRC32（标准多项式 0xEDB88320 反射形式，与 zlib/PNG 一致）
 *
 * 为什么用 CRC32 而不是简单的累加和：
 *   参数区比 Modbus 帧长得多，累加和对"两个字节互相抵消"这类错误
 *   完全不敏感（例如 0x01+0x02 与 0x02+0x01 得到同一个值）。
 *   CRC32 对这种错误敏感，而且本项目里 OTA 也用 CRC32，
 *   保持一致少记一套算法。
 * ------------------------------------------------------------------ */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    uint32_t i;
    uint8_t  b;

    for (i = 0u; i < len; i++) {
        crc ^= (uint32_t)data[i];
        for (b = 0u; b < 8u; b++) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/* ------------------------------------------------------------------
 * 槽头的序列化 / 反序列化
 *
 * 显式按字节拼装，而不是把结构体直接写入 Flash。原因：
 *   1) 结构体有对齐填充，填充字节值不确定，会让"同一份数据"的 CRC 不一致
 *   2) 直接写结构体依赖编译器的内存布局，换编译器/换优化级别就变
 *   3) 显式拼装让格式在代码里一目了然，便于和文档对照
 *
 * ⚠️ 字段布局踩过的坑：早期版本把 LEN 放在偏移 8（32 位，占 8..11），
 *    又把 VERSION 放在偏移 10（16 位，占 10..11）—— 两者**重叠**。
 *    写 LEN=16 之后再写 VERSION=7，会把 LEN 的低字节覆盖成 0x07，
 *    于是读回来 len 变成 7。
 *    现象是"写入成功但读取校验必然失败"，而且槽头 dump 出来的字节
 *    看起来"几乎正确"（magic/seq/crc 都对，只有 len 错），极易误判。
 *
 *    现在的布局严格按"字段互不重叠 + 4 字节对齐方便阅读"排布。
 * ------------------------------------------------------------------ */
#define SLOT_OFF_MAGIC   0u      /* uint32 */
#define SLOT_OFF_SEQ     4u      /* uint32 */
#define SLOT_OFF_VERSION 8u      /* uint16 */
#define SLOT_OFF_LEN     10u     /* uint32 */
#define SLOT_OFF_CRC     14u     /* uint32 */
#define SLOT_OFF_DATA    18u     /* 数据起始 */

/* ---- "槽头长度" 与 "数据起始偏移" 是两个不同的概念，必须分开 ----
 *
 * 踩过的坑：写槽头时用 GW_PARAM_SLOT_OVERHEAD(20) 当长度，
 * 但真正的槽头只有 18 字节 —— 多写的 2 字节把数据区前两字节写成了 0x00。
 * 之后写数据时，Flash 的"只能 1->0"特性让 0x00 无法变成 0xA0，
 * 于是数据写入被拒，表现为"save 返回 ERR_FLASH 且没有任何数据落盘"。
 *
 *   SLOT_HDR_LEN   = 槽头实际字节数（写入用）
 *   SLOT_OFF_DATA  = 数据相对槽起始的偏移（对齐用，可以 > SLOT_HDR_LEN）
 *
 * 两者之间若有间隙，那些字节保持擦除态 0xFF，不会被写入 ——
 * 这正是我们要的（保证间隙永远不会出现"0->1"的写入需求）。 */
#define SLOT_HDR_LEN     SLOT_OFF_DATA      /* 18 字节 */

/* 编译期检查：字段区间不得重叠；槽头长度必须与数据偏移一致或更小。 */
_Static_assert(SLOT_OFF_MAGIC + 4u <= SLOT_OFF_SEQ,   "magic 与 seq 重叠");
_Static_assert(SLOT_OFF_SEQ + 4u <= SLOT_OFF_VERSION, "seq 与 version 重叠");
_Static_assert(SLOT_OFF_VERSION + 2u <= SLOT_OFF_LEN, "version 与 len 重叠");
_Static_assert(SLOT_OFF_LEN + 4u <= SLOT_OFF_CRC,     "len 与 crc 重叠");
_Static_assert(SLOT_OFF_CRC + 4u <= SLOT_OFF_DATA,    "crc 与 data 重叠");
_Static_assert(SLOT_HDR_LEN <= SLOT_OFF_DATA, "槽头长度不得超过数据起始偏移");
_Static_assert(SLOT_OFF_DATA <= GW_PARAM_SLOT_OVERHEAD, "数据偏移超出预留槽头空间");
_Static_assert(SLOT_OFF_DATA + GW_PARAM_MAX_DATA <= GW_PARAM_SLOT_SIZE,
               "槽总长超出预留空间");

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* 计算槽的 CRC：覆盖"magic..len"全部槽头字段 + 数据。
   刻意不含 crc 字段本身（否则成了自指），也不含 data 之后的填充区。 */
static uint32_t slot_crc(const uint8_t *hdr, const uint8_t *data, uint32_t len)
{
    uint32_t crc = crc32_update(0xFFFFFFFFu, hdr, SLOT_OFF_CRC);
    crc = crc32_update(crc, data, len);
    return crc ^ 0xFFFFFFFFu;
}

/* ==================================================================
 * 内部：槽访问
 * ================================================================== */

static uint8_t iface_slot_count(const gw_param_t *p)
{
    uint8_t n = p->iface.slot_count(p->iface.ctx);

    if (n == 0u || n > GW_PARAM_MAX_SLOTS) {
        n = (n == 0u) ? 0u : GW_PARAM_MAX_SLOTS;
    }
    return n;
}

static gw_param_err_t slot_read_header(const gw_param_t *p, uint8_t slot,
                                       uint8_t *hdr /* 16 字节 */)
{
    uint32_t off = p->iface.slot_offset(p->iface.ctx, slot);

    if (p->iface.read(p->iface.ctx, off, hdr, GW_PARAM_SLOT_OVERHEAD) != 0) {
        return GW_PARAM_ERR_FLASH;
    }
    return GW_PARAM_OK;
}

/**
 * 判断一个槽是否有效，并取出它的序号
 *
 * 三关校验：magic、len 合理、CRC32。任一不过都视为无效。
 * 检查顺序刻意如此：先判 magic（最便宜，能快速排除空槽），
 * 再判 len（防止越界读），最后算 CRC（最贵）。
 * 这与从站引擎"先查站号再算 CRC"是同一个思路。
 */
static int slot_is_valid(const gw_param_t *p, uint8_t slot,
                         const uint8_t *hdr, uint32_t *out_seq,
                         uint32_t *out_len, uint16_t *out_ver)
{
    uint32_t len;
    uint32_t seq;
    uint32_t crc_stored;
    uint32_t crc_calc;
    uint8_t  data[GW_PARAM_MAX_DATA];
    uint32_t off;

    if (get_u32(&hdr[SLOT_OFF_MAGIC]) != GW_PARAM_MAGIC) {
        return 0;                       /* 空槽（0xFFFFFFFF）或未写入 */
    }
    len = get_u32(&hdr[SLOT_OFF_LEN]);
    if (len > GW_PARAM_MAX_DATA) {
        return 0;                       /* 长度非法：可能写了一半 */
    }
    seq = get_u32(&hdr[SLOT_OFF_SEQ]);
    crc_stored = get_u32(&hdr[SLOT_OFF_CRC]);

    /* 读数据并算 CRC */
    off = p->iface.slot_offset(p->iface.ctx, slot) + SLOT_OFF_DATA;
    if (len > 0u) {
        if (p->iface.read(p->iface.ctx, off, data, len) != 0) {
            return 0;
        }
    }
    crc_calc = slot_crc(hdr, (len > 0u) ? data : 0, len);
    if (crc_calc != crc_stored) {
        return 0;                       /* 数据损坏或写入中断 */
    }

    if (out_seq != 0) { *out_seq = seq; }
    if (out_len != 0) { *out_len = len; }
    if (out_ver != 0) { *out_ver = get_u16(&hdr[SLOT_OFF_VERSION]); }
    return 1;
}

/* ==================================================================
 * 对外接口
 * ================================================================== */

gw_param_err_t gw_param_init(gw_param_t *p, const gw_flash_if_t *iface)
{
    uint8_t  n;
    uint8_t  i;
    uint8_t  best_slot = 0u;
    uint32_t best_seq  = 0u;
    int      found = 0;

    if (p == 0 || iface == 0) {
        return GW_PARAM_ERR_PARAM;
    }
    if (iface->read == 0 || iface->write == 0 || iface->erase_slot == 0
        || iface->slot_count == 0 || iface->slot_offset == 0) {
        return GW_PARAM_ERR_PARAM;
    }

    memset(p, 0, sizeof(*p));
    p->iface = *iface;

    n = iface_slot_count(p);
    if (n == 0u) {
        return GW_PARAM_ERR_PARAM;
    }

    for (i = 0u; i < n; i++) {
        uint8_t  hdr[GW_PARAM_SLOT_OVERHEAD];
        uint32_t seq = 0u;

        if (slot_read_header(p, i, hdr) != GW_PARAM_OK) {
            continue;                   /* Flash 读失败：跳过该槽 */
        }
        if (slot_is_valid(p, i, hdr, &seq, 0, 0)) {
            if (!found || seq > best_seq) {
                /* 用 > 而不是 >=：序号相同时保留先扫到的（行为确定，
                   避免"同一份数据每次上电选出不同槽"这种诡异现象） */
                best_seq  = seq;
                best_slot = i;
                found     = 1;
            }
        } else {
            /* 记录"看到过无效槽"，用于判断是否有过写入中断 */
            if (get_u32(&hdr[SLOT_OFF_MAGIC]) != 0xFFFFFFFFu) {
                p->stat.corrupt_slots++;
            }
        }
    }

    if (!found) {
        /* 首次上电或全部损坏。next_seq 从 0 开始，last_slot 用一个
           不可能的初值（0xFF），这样第一次保存一定会写入槽 0。 */
        p->next_seq  = 1u;
        p->last_slot = 0xFFu;
        p->valid     = 0u;
        return GW_PARAM_ERR_NO_VALID;
    }

    p->next_seq  = best_seq + 1u;
    p->last_slot = best_slot;
    p->valid     = 1u;
    return GW_PARAM_OK;
}

gw_param_err_t gw_param_load(gw_param_t *p, uint8_t *buf, uint32_t buf_size,
                             uint32_t *out_len, uint16_t *out_ver)
{
    uint8_t  hdr[GW_PARAM_SLOT_OVERHEAD];
    uint32_t len = 0u;
    uint32_t off;

    if (p == 0 || buf == 0 || p->valid == 0u) {
        return GW_PARAM_ERR_NO_VALID;
    }
    if (slot_read_header(p, p->last_slot, hdr) != GW_PARAM_OK) {
        return GW_PARAM_ERR_FLASH;
    }
    if (!slot_is_valid(p, p->last_slot, hdr, 0, &len, out_ver)) {
        p->valid = 0u;
        return GW_PARAM_ERR_NO_VALID;
    }
    if (len > buf_size) {
        return GW_PARAM_ERR_TOO_BIG;
    }

    off = p->iface.slot_offset(p->iface.ctx, p->last_slot) + SLOT_OFF_DATA;
    if (len > 0u && p->iface.read(p->iface.ctx, off, buf, len) != 0) {
        return GW_PARAM_ERR_FLASH;
    }
    if (out_len != 0) {
        *out_len = len;
    }
    return GW_PARAM_OK;
}

/**
 * 选目标槽：优先空槽，其次最旧的槽。
 *
 * "空槽"的判定用 magic 不匹配 —— 擦除后是 0xFF，天然不匹配。
 * 这样不必额外维护"哪个槽空着"的状态，避免状态与实际不一致。
 */
static uint8_t pick_target_slot(gw_param_t *p, int *is_erase)
{
    uint8_t  n = iface_slot_count(p);
    uint8_t  i;
    uint8_t  oldest = 0u;
    uint32_t oldest_seq = 0xFFFFFFFFu;
    int      have_oldest = 0;

    /* ⚠️ 损坏槽的候选（见下面"修复"的说明） */
    uint8_t  bad_slot  = 0u;
    uint32_t bad_seq   = 0xFFFFFFFFu;
    int      have_bad  = 0;

    *is_erase = 0;

    for (i = 0u; i < n; i++) {
        uint8_t  hdr[GW_PARAM_SLOT_OVERHEAD];
        uint32_t seq = 0u;

        if (i == p->last_slot) {
            continue;                   /* 不覆盖当前有效槽（掉电安全的关键） */
        }
        if (slot_read_header(p, i, hdr) != GW_PARAM_OK) {
            continue;
        }
        if (!slot_is_valid(p, i, hdr, &seq, 0, 0)) {
            /* magic 不匹配 = 空槽，直接用 */
            if (get_u32(&hdr[SLOT_OFF_MAGIC]) != GW_PARAM_MAGIC) {
                return i;
            }
            /* ---- 损坏槽（magic 对但校验不过）----
             *
             * ⚠️⚠️ 这里原本是 `continue`（直接跳过），那是个**真实的健壮性缺陷**。
             *
             * 场景：一次"写中途掉电"会让一个槽变成损坏槽（magic 已写入，
             *       但 len/crc 还是 0xFF，或数据没写完）。
             *       由于磨损均衡会依次用下一个槽，**每次掉电消耗一个槽**。
             *
             *       8 个槽、连续 8 次"写中途掉电"之后：
             *           7 个损坏槽 + 1 个当前有效槽
             *       -> 没有空槽、也没有"更旧的有效槽"
             *       -> 落到下面的退化分支，**擦掉当前有效槽**
             *       -> 参数丢失（而那是唯一的有效数据）
             *
             *       原来的注释写的是"实践中不该走到这里" ——
             *       但那只是**没有测过**。加上断电穷举之后，
             *       11 次连续掉电就能走到（本项目的 powerloss_param 抓到）。
             *
             * 修复：损坏槽同样是**可复用的** —— 它本来就没有有效数据，
             *       擦掉它不会造成任何损失。用它比用退化分支安全得多。
             *
             * ⚠️ 选哪个损坏槽：取 seq 最小的那个。
             *    "写中途掉电"发生在写 seq 之后，所以损坏槽的 seq 是可信的；
             *    按 seq 最小来选可以**延续磨损均衡**，
             *    而不是每次都盯着同一个槽反复擦（那会加速那个槽的损耗）。 */
            if (have_bad == 0 || seq < bad_seq) {
                bad_seq  = seq;
                bad_slot = i;
                have_bad = 1;
            }
            continue;
        }
        if (!have_oldest || seq < oldest_seq) {
            oldest_seq  = seq;
            oldest     = i;
            have_oldest = 1;
        }
    }

    if (have_oldest) {
        *is_erase = 1;
        return oldest;
    }

    /* ---- 优先复用损坏槽（而不是擦当前有效槽）---- */
    if (have_bad) {
        *is_erase = 1;
        return bad_slot;
    }

    /* ---- 真正的退化情况：所有槽都是当前有效槽 ----
     *
     * ⚠️ 这**只可能在槽数 == 1** 时发生（只有一个槽，而它就是有效槽）。
     *    槽数 >= 2 时，上面必然能返回：要么有空槽、要么有损坏槽可复用、
     *    要么有更旧的有效槽。
     *
     * ⚠️ 走到这里只能擦当前有效槽 —— 那会丢数据。
     *    但**不擦就保存不了任何东西**，而"完全不能保存参数"
     *    对现场设备更糟（设备再也无法配置）。
     *    所以这里仍然擦，但调用方能看到返回值判断风险。
     *
     *    实际部署用 8 个槽，所以这条路走不到；
     *    真走到说明槽数配置有问题（GW_PARAM_FLASH_SLOT_COUNT 太小）。 */
    *is_erase = 1;
    return p->last_slot;
}

gw_param_err_t gw_param_save(gw_param_t *p, const uint8_t *data, uint32_t len,
                             uint16_t version)
{
    uint8_t  hdr[GW_PARAM_SLOT_OVERHEAD];
    uint8_t  verify[GW_PARAM_MAX_DATA];
    uint8_t  slot;
    uint32_t off;
    uint32_t crc;
    int      is_erase = 0;

    if (p == 0 || (len > 0u && data == 0)) {
        return GW_PARAM_ERR_PARAM;
    }
    if (len > GW_PARAM_MAX_DATA) {
        return GW_PARAM_ERR_TOO_BIG;
    }
    if (iface_slot_count(p) == 0u) {
        return GW_PARAM_ERR_PARAM;
    }

    /* ---- 1) 选目标槽 ---- */
    slot = pick_target_slot(p, &is_erase);
    off  = p->iface.slot_offset(p->iface.ctx, slot);

    /* ---- 2) 擦除（只有这一步会破坏数据，且破坏的是最旧的那份） ---- */
    if (is_erase) {
        if (p->iface.erase_slot(p->iface.ctx, slot) != 0) {
            return GW_PARAM_ERR_FLASH;
        }
        p->stat.erases++;
    }

    /* ---- 3) 组帧并写入 ---- */
    memset(hdr, 0, sizeof(hdr));
    put_u32(&hdr[SLOT_OFF_MAGIC], GW_PARAM_MAGIC);
    put_u32(&hdr[SLOT_OFF_SEQ], p->next_seq);
    put_u32(&hdr[SLOT_OFF_LEN], len);
    put_u16(&hdr[SLOT_OFF_VERSION], version);
    crc = slot_crc(hdr, data, len);
    put_u32(&hdr[SLOT_OFF_CRC], crc);

#ifdef GW_PARAM_TRACE
    {
        int k;
        printf("[TR] 写 slot %u: len=%lu ver=%u crc=%08lX hdr=",
               slot, (unsigned long)len, (unsigned)version, (unsigned long)crc);
        for (k = 0; k < (int)SLOT_HDR_LEN; k++) { printf("%02X ", hdr[k]); }
        printf("\n");
    }
#endif

    if (p->iface.write(p->iface.ctx, off, hdr, SLOT_HDR_LEN) != 0) {
        return GW_PARAM_ERR_FLASH;
    }
    if (len > 0u) {
        if (p->iface.write(p->iface.ctx, off + SLOT_OFF_DATA, data, len) != 0) {
            return GW_PARAM_ERR_FLASH;
        }
    }

    /* ---- 4) 回读校验 ----
       不做这一步的话，"写失败"会被静默当成成功，
       下次上电才发现参数是坏的 —— 而那时已经无法定位原因。
       校验做两件事：CRC 一致（防位翻转）与逐字节一致（防写偏）。 */
    {
        uint8_t  vhdr[GW_PARAM_SLOT_OVERHEAD];
        uint32_t vlen = 0u;
        uint32_t vseq = 0u;

        if (slot_read_header(p, slot, vhdr) != GW_PARAM_OK) {
            p->stat.verify_fails++;
            return GW_PARAM_ERR_VERIFY;
        }
        if (!slot_is_valid(p, slot, vhdr, &vseq, &vlen, 0) || vlen != len) {
            p->stat.verify_fails++;
            return GW_PARAM_ERR_VERIFY;
        }
        if (len > 0u) {
            if (p->iface.read(p->iface.ctx, off + SLOT_OFF_DATA,
                              verify, len) != 0) {
                p->stat.verify_fails++;
                return GW_PARAM_ERR_VERIFY;
            }
            if (memcmp(verify, data, len) != 0) {
                p->stat.verify_fails++;
                return GW_PARAM_ERR_VERIFY;
            }
        }
    }

    /* ---- 5) 校验通过，才推进状态 ----
       顺序很重要：先写、后校验、最后才更新内存状态。
       若在写入过程中掉电，next_seq 与 last_slot 都不会变，
       下次上电扫描时会把那个"写了一半"的槽判为无效并跳过。 */
    p->last_slot = slot;
    p->next_seq++;
    p->valid = 1u;
    p->stat.writes++;

    if (p->next_seq == 0u) {
        p->next_seq = 1u;               /* 序号回绕：重新从 1 开始 */
        p->stat.seq_wraps++;
    }
    return GW_PARAM_OK;
}

int gw_param_needs_save(const gw_param_t *p, const uint8_t *data, uint32_t len)
{
    uint8_t  cur[GW_PARAM_MAX_DATA];
    uint32_t cur_len = 0u;

    if (p == 0) {
        return 0;
    }
    if (p->valid == 0u) {
        return 1;                       /* 从没存过，必须写 */
    }
    /* 用 load 而不是直接读 Flash：这样复用同一套有效性校验 */
    if (gw_param_load((gw_param_t *)p, cur, sizeof(cur), &cur_len, 0)
        != GW_PARAM_OK) {
        return 1;
    }
    if (cur_len != len) {
        return 1;
    }
    return (len == 0u) ? 0 : ((memcmp(cur, data, len) != 0) ? 1 : 0);
}

const gw_param_stat_t *gw_param_stats(const gw_param_t *p)
{
    return (p == 0) ? 0 : &p->stat;
}

const char *gw_param_strerror(gw_param_err_t e)
{
    switch (e) {
    case GW_PARAM_OK:           return "OK";
    case GW_PARAM_ERR_NO_VALID: return "no valid slot (first boot or all corrupt)";
    case GW_PARAM_ERR_PARAM:    return "invalid parameter";
    case GW_PARAM_ERR_FLASH:    return "flash operation failed";
    case GW_PARAM_ERR_VERIFY:   return "read-back verify failed";
    case GW_PARAM_ERR_TOO_BIG:  return "data too large";
    case GW_PARAM_ERR_NO_SPACE: return "no writable slot";
    default:                    return "unknown error";
    }
}
