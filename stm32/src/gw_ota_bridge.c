/**
 * @file    gw_ota_bridge.c
 * @brief   OTA 下行帧分发实现
 */
#include "gw_ota_bridge.h"

#include <string.h>

/* ==================================================================
 *  大端读写
 *
 *  ⚠️ 手写而不是用 memcpy 到结构体：
 *     结构体有对齐填充，跨编译器/跨芯片不保证一致。
 *     帧头的 LEN 字段已经确立了大端约定，这里保持一致。
 * ================================================================== */
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* ==================================================================
 *  载荷编解码
 * ================================================================== */

uint16_t gw_ota_pack_begin(uint16_t major, uint16_t minor,
                           uint32_t total_len, uint32_t crc32,
                           uint8_t *out, uint16_t cap)
{
    if (out == 0 || cap < GW_OTA_BEGIN_PAYLOAD_LEN) {
        return 0u;
    }
    put_u16(&out[0], major);
    put_u16(&out[2], minor);
    put_u32(&out[4], total_len);
    put_u32(&out[8], crc32);
    return GW_OTA_BEGIN_PAYLOAD_LEN;
}

int gw_ota_unpack_begin(const uint8_t *in, uint16_t len,
                        uint16_t *major, uint16_t *minor,
                        uint32_t *total_len, uint32_t *crc32)
{
    /* ⚠️ 长度必须**精确相等**，不能只判 `>=`：
       多了字节说明发送方的布局与我不一致（版本不同），
       按我的布局解释会读到错位的值。宁可拒绝。 */
    if (in == 0 || len != GW_OTA_BEGIN_PAYLOAD_LEN) {
        return -1;
    }
    if (major != 0)     { *major     = get_u16(&in[0]); }
    if (minor != 0)     { *minor     = get_u16(&in[2]); }
    if (total_len != 0) { *total_len = get_u32(&in[4]); }
    if (crc32 != 0)     { *crc32     = get_u32(&in[8]); }
    return 0;
}

uint16_t gw_ota_pack_chunk(uint32_t offset, const uint8_t *data,
                           uint16_t data_len, uint8_t *out, uint16_t cap)
{
    if (out == 0) {
        return 0u;
    }
    if (data_len > GW_OTA_CHUNK_DATA_MAX) {
        return 0u;
    }
    if (data_len > 0u && data == 0) {
        return 0u;
    }
    if ((uint32_t)cap < (uint32_t)GW_OTA_CHUNK_HDR_LEN + data_len) {
        return 0u;
    }
    put_u32(&out[0], offset);
    if (data_len > 0u) {
        memcpy(&out[GW_OTA_CHUNK_HDR_LEN], data, data_len);
    }
    return (uint16_t)(GW_OTA_CHUNK_HDR_LEN + data_len);
}

int gw_ota_unpack_chunk(const uint8_t *in, uint16_t len,
                        uint32_t *offset, const uint8_t **data,
                        uint16_t *data_len)
{
    if (in == 0 || len < GW_OTA_CHUNK_HDR_LEN) {
        return -1;
    }
    /* 至少要有 1 字节数据 —— 空分片没有意义，
       而且会让 gw_ota_chunk 的偏移推进逻辑退化成死循环。 */
    if (len == GW_OTA_CHUNK_HDR_LEN) {
        return -1;
    }
    if (offset != 0)   { *offset   = get_u32(&in[0]); }
    if (data != 0)     { *data     = &in[GW_OTA_CHUNK_HDR_LEN]; }
    if (data_len != 0) { *data_len = (uint16_t)(len - GW_OTA_CHUNK_HDR_LEN); }
    return 0;
}

uint16_t gw_ota_pack_ack(uint8_t phase, gw_ota_br_t result,
                         uint32_t progress, uint8_t *out, uint16_t cap)
{
    if (out == 0 || cap < GW_OTA_ACK_PAYLOAD_LEN) {
        return 0u;
    }
    out[0] = phase;
    out[1] = (uint8_t)result;
    put_u32(&out[2], progress);
    return GW_OTA_ACK_PAYLOAD_LEN;
}

/* ==================================================================
 *  初始化
 * ================================================================== */

int gw_ota_bridge_init(gw_ota_bridge_t *b, const gw_ota_io_t *io,
                       gw_ota_slot_t target)
{
    if (b == 0) {
        return -1;
    }

    memset(b, 0, sizeof(*b));

    if (io != 0) {
        b->io = *io;
        /* ⚠️ io 为 NULL 时仍然允许初始化 —— 那样本模块只在
           "解析层"工作（状态机会转但不会碰 Flash）。
           测试需要这种模式：不测 Flash 时不该被迫提供一个假接口。 */
        if (gw_ota_init(&b->ota, &b->io, target) != 0) {
            return -2;
        }
    }

    b->inited = 1u;
    return 0;
}

/* ==================================================================
 *  帧分发
 * ================================================================== */

static gw_ota_br_t handle_begin(gw_ota_bridge_t *b,
                                const uint8_t *payload, uint16_t len)
{
    uint16_t major = 0u;
    uint16_t minor = 0u;
    uint32_t total = 0u;
    uint32_t crc   = 0u;

    if (gw_ota_unpack_begin(payload, len, &major, &minor,
                            &total, &crc) != 0) {
        b->errors++;
        return GW_OTA_BR_ERR_PAYLOAD;
    }

    /* ⚠️ 容量检查**不在这里做**，而在 `gw_ota_begin()` 里 ——
       它在擦除**之前**判 `total_len > GW_APP_SLOT_SIZE` 并拒绝。
       所以"传了放不下的固件"不会白花那 2~3 秒的擦除时间。

       ⚠️ 但这也意味着调用方要清楚：
          **擦除可能失败**（容量不足 / Flash 写保护）。
          `gw_ota_bridge_do_begin()` 会返回负值，
          调用方应据此回 NACK 给上位机，而不是干等。 */

    /* 只登记参数 —— 擦除交给 gw_ota_bridge_do_begin()，
       因为那时才能选一个"不影响 RS485 响应、不饿死看门狗"的时机。 */
    b->begin_major    = major;
    b->begin_minor    = minor;
    b->begin_total    = total;
    b->begin_crc      = crc;
    b->begin_pending  = 1u;

    return GW_OTA_BR_OK;
}

static gw_ota_br_t handle_chunk(gw_ota_bridge_t *b,
                                const uint8_t *payload, uint16_t len)
{
    uint32_t       offset = 0u;
    const uint8_t *data   = 0;
    uint16_t       dlen   = 0u;
    int            r;

    /* ---- 限速 ----
       ⚠️ 返回 BUSY 而不是"静默变慢"：
          上位机能感知到背压并稍后重发，
          而"静默变慢"会让它以为链路卡了、进而超时重传整段。 */
    if (b->chunks_this_tick >= GW_OTA_CHUNKS_PER_TICK) {
        b->chunks_busy++;
        return GW_OTA_BR_ERR_BUSY;
    }

    if (gw_ota_unpack_chunk(payload, len, &offset, &data, &dlen) != 0) {
        b->errors++;
        return GW_OTA_BR_ERR_PAYLOAD;
    }

    /* ---- 显式检查状态机是否真的在"下载中" ----
       ⚠️ 为什么要单独查一次（`gw_ota_chunk` 自己也会判）：
          不加这道检查的话，"没 BEGIN 就发 CHUNK"的错误码会**取决于
          调用方有没有调过 `do_begin()`** ——
          没调过是 STATE，调过（但状态机没进 DOWNLOAD）是 FLASH。
          同一个上位机错误得到两种不同的原因，排查时会误导。

          现在语义确定了：**没进入下载态一律 STATE**。 */
    if (b->io.erase != 0
        && gw_ota_state(&b->ota) != GW_OTA_ST_DOWNLOAD) {
        b->errors++;
        return GW_OTA_BR_ERR_STATE;
    }
    if (b->begin_pending != 0u) {
        b->errors++;
        return GW_OTA_BR_ERR_STATE;
    }
    if (b->io.erase == 0) {
        /* 没提供 Flash 接口（测试模式）：只做解析，不写 */
        b->chunks_this_tick++;
        b->chunks_ok++;
        return GW_OTA_BR_OK;
    }

    b->chunks_this_tick++;

    r = gw_ota_chunk(&b->ota, offset, data, (uint32_t)dlen);

    if (r == 0) {
        b->chunks_ok++;
        return GW_OTA_BR_OK;
    }
    if (r == 1) {
        /* 重复分片：**不是失败**。链路上偶尔重传是正常的，
           报失败会让上位机无意义地重传更多。 */
        b->chunks_dup++;
        return GW_OTA_BR_ERR_DUPLICATE;
    }

    b->errors++;
    return GW_OTA_BR_ERR_FLASH;
}

static gw_ota_br_t handle_end(gw_ota_bridge_t *b,
                              const uint8_t *payload, uint16_t len)
{
    int r;

    (void)payload;

    /* END 的载荷必须为空。
       带载荷说明发送方的帧构造有问题（或者把别的帧类型标错了），
       按"空载荷的 END"处理会掩盖那个问题。 */
    if (len != GW_OTA_END_PAYLOAD_LEN) {
        b->errors++;
        return GW_OTA_BR_ERR_PAYLOAD;
    }
    if (b->begin_pending != 0u) {
        b->errors++;
        return GW_OTA_BR_ERR_STATE;     /* BEGIN 的擦除还没做 */
    }
    if (b->io.erase == 0) {
        return GW_OTA_BR_OK;            /* 测试模式 */
    }

    r = gw_ota_end(&b->ota);

    if (r == 0) {
        /* ⚠️ 不在这里重启。
           要先让 ACK 发出去，否则上位机不知道成功了 ——
           它会以为设备掉线，然后重传整个固件。
           调用方在 ACK 发出之后自行延时重启。 */
        b->reboot_pending = 1u;
        return GW_OTA_BR_OK;
    }

    b->errors++;

    /* ⚠️ 失败原因必须分类，因为**上位机的应对方式不同**：
         - 数据类问题（CRC 不符 / 长度不符 / 偏移不连续）
           -> 重传那一片或整包**有用**
         - Flash / 参数类问题 -> 重传没用，得先修根因
       笼统报"失败"会让上位机盲目重传，浪费一次完整的传输。 */
    switch (gw_ota_err(&b->ota)) {
    case GW_OTA_ERR_CRC_MISMATCH:
    case GW_OTA_ERR_LEN_MISMATCH:
    case GW_OTA_ERR_OFFSET:
        return GW_OTA_BR_ERR_CRC;       /* 数据问题：重传有意义 */

    case GW_OTA_ERR_TOO_BIG:
        return GW_OTA_BR_ERR_NOSPACE;

    case GW_OTA_ERR_BAD_PARAM:
    case GW_OTA_ERR_NO_BEGIN:
    case GW_OTA_ERR_SAME_SLOT:
        return GW_OTA_BR_ERR_STATE;

    default:
        return GW_OTA_BR_ERR_FLASH;
    }
}

gw_ota_br_t gw_ota_bridge_frame(gw_ota_bridge_t *b, uint8_t type,
                                const uint8_t *payload, uint16_t len,
                                uint8_t *out, uint16_t cap,
                                uint16_t *out_len)
{
    gw_ota_br_t r;
    uint8_t     phase = 0u;
    uint32_t    progress = 0u;

    if (out_len != 0) {
        *out_len = 0u;
    }
    if (b == 0 || b->inited == 0u) {
        return GW_OTA_BR_ERR_STATE;
    }

    switch (type) {
    case GW_FT_OTA_BEGIN:
        phase = (uint8_t)GW_OTA_PHASE_BEGIN;
        r = handle_begin(b, payload, len);
        break;

    case GW_FT_OTA_CHUNK:
        phase = (uint8_t)GW_OTA_PHASE_CHUNK;
        r = handle_chunk(b, payload, len);
        progress = b->chunks_ok;
        break;

    case GW_FT_OTA_END:
        phase = (uint8_t)GW_OTA_PHASE_END;
        r = handle_end(b, payload, len);
        progress = b->begin_total;
        break;

    default:
        /* 不是 OTA 帧：本模块不处理，也不应答。
           返回 STATE 让调用方知道"这个类型不该给我"。 */
        return GW_OTA_BR_ERR_STATE;
    }

    /* 生成 ACK/NACK 载荷。
       ⚠️ 连 BUSY 与 DUPLICATE 也要回 ——
          不回的话上位机在等一个永远不会来的应答。 */
    if (out != 0 && out_len != 0) {
        *out_len = gw_ota_pack_ack(phase, r, progress, out, cap);
    }

    return r;
}

void gw_ota_bridge_tick(gw_ota_bridge_t *b)
{
    if (b == 0 || b->inited == 0u) {
        return;
    }
    b->chunks_this_tick = 0u;
}

int gw_ota_bridge_begin_pending(const gw_ota_bridge_t *b)
{
    if (b == 0 || b->inited == 0u) {
        return 0;
    }
    return (b->begin_pending != 0u) ? 1 : 0;
}

int gw_ota_bridge_do_begin(gw_ota_bridge_t *b)
{
    int r;

    if (b == 0 || b->inited == 0u || b->begin_pending == 0u) {
        return -1;
    }
    if (b->io.erase == 0) {
        b->begin_pending = 0u;      /* 测试模式 */
        return 0;
    }

    r = gw_ota_begin(&b->ota, b->begin_major, b->begin_minor,
                     b->begin_total, b->begin_crc);

    if (r != 0) {
        b->errors++;
        b->begin_pending = 0u;      /* 失败就不再重试 —— 重试还是同样的结果 */
        return r;
    }

    b->begin_pending = 0u;
    return 0;
}

int gw_ota_bridge_reboot_pending(const gw_ota_bridge_t *b)
{
    if (b == 0 || b->inited == 0u) {
        return 0;
    }
    return (b->reboot_pending != 0u) ? 1 : 0;
}

gw_ota_state_t gw_ota_bridge_state(const gw_ota_bridge_t *b)
{
    if (b == 0 || b->inited == 0u) {
        return GW_OTA_ST_IDLE;
    }
    if (b->io.erase == 0) {
        /* 测试模式：状态机没跑，用 begin_pending 反映"收到过 BEGIN" */
        return (b->begin_pending != 0u) ? GW_OTA_ST_IDLE : GW_OTA_ST_DOWNLOAD;
    }
    return gw_ota_state(&b->ota);
}

const char *gw_ota_br_str(gw_ota_br_t r)
{
    switch (r) {
    case GW_OTA_BR_OK:            return "OK";
    case GW_OTA_BR_ERR_PAYLOAD:   return "BAD_PAYLOAD";
    case GW_OTA_BR_ERR_STATE:     return "BAD_STATE";
    case GW_OTA_BR_ERR_FLASH:     return "FLASH_ERROR";
    case GW_OTA_BR_ERR_BUSY:      return "BUSY";
    case GW_OTA_BR_ERR_DUPLICATE: return "DUPLICATE";
    case GW_OTA_BR_ERR_CRC:       return "CRC_ERROR";
    case GW_OTA_BR_ERR_NOSPACE:   return "NO_SPACE";
    default:                      return "?";
    }
}
