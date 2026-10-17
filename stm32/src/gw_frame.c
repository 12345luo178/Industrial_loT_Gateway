/**
 * @file    gw_frame.c
 * @brief   串口帧协议实现（纯逻辑，可在 PC 上单元测试）
 */
#include "gw_frame.h"

#include <string.h>

/* ------------------------------------------------------------------
 * CRC8（多项式 0x07，初值 0x00，无反射）
 *
 * 为什么帧校验用 CRC8 而不是 CRC16：
 *   帧较短（最长约 133 字节），CRC8 已能可靠检出单/双比特错误；
 *   而且 ESP32 侧用同样的算法（常见于 1-Wire、部分传感器协议），
 *   实现简单、无查表、开销小。Modbus 那边用 CRC16 是因为帧更长、
 *   且那是协议规定。
 * ------------------------------------------------------------------ */
static uint8_t crc8_update(uint8_t crc, const uint8_t *data, uint16_t len)
{
    uint16_t i;
    uint8_t  b;

    for (i = 0u; i < len; i++) {
        crc ^= data[i];
        for (b = 0u; b < 8u; b++) {
            if (crc & 0x80u) {
                crc = (uint8_t)((crc << 1) ^ 0x07u);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

/* ------------------------------------------------------------------
 * 大端读写（跨芯片传输必须显式约定字节序）
 * ------------------------------------------------------------------ */
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

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

/* ==================================================================
 *  解析器
 *
 *  状态机逐字节推进。这样做而不是"先找帧头再整块解析"的原因：
 *    调用方可以按任意块大小喂数据（1 字节、串口 DMA 一次 20 字节、
 *    或粘了三帧的 200 字节），状态机天然支持，不需要调用方配合。
 * ================================================================== */

void gw_frame_parser_init(gw_frame_parser_t *p)
{
    if (p == 0) {
        return;
    }
    memset(p, 0, sizeof(*p));
    p->state = GW_RX_WAIT_HEAD;
}

/**
 * 复位到"搜索帧头"状态，但**保留统计量**。
 *
 * resync 参数用于区分两种复位：
 *   1 = 坏帧导致的重同步（计一次 resync，用于诊断链路质量）
 *   0 = 正常取走一帧后的复位（不计）
 */
static void parser_reset(gw_frame_parser_t *p, int resync)
{
    p->state = GW_RX_WAIT_HEAD;
    p->idx   = 0u;
    p->need  = 0u;
    if (resync) {
        p->stat.rx_resync++;
    }
}

/**
 * 重同步：从 buf 的第 start 个字节起重新搜索帧头。
 *
 * 这是本模块最关键的一步。收到坏帧时**不能整块丢弃** ——
 * 因为缓冲区里剩余的字节很可能就是下一帧的开头。
 * 只丢一个字节再重扫，保证不会连带丢掉后面的好帧。
 *
 * @param p      解析器
 * @param start  从 buf 的哪个下标开始搜索（通常是 1，即跳过已被否定的那个帧头）
 * @return 1 = 找到新的帧头并已把状态机置于"等长度"；0 = 没找到，回到等帧头
 */
static int parser_resync(gw_frame_parser_t *p, uint16_t start)
{
    uint16_t i;
    uint16_t have = p->idx;         /* 已收字节数 */

    for (i = start; i < have; i++) {
        if (p->buf[i] == GW_FRAME_HEAD) {
            /* 把从 i 开始的剩余字节搬到缓冲区开头 */
            uint16_t remain = (uint16_t)(have - i);

            if (i > 0u) {
                memmove(&p->buf[0], &p->buf[i], remain);
            }

            /* ---- 关键：idx 必须反映"缓冲区里实际已有的字节数" ----
             *
             * 踩过的坑：早期版本把 idx 写成 1（只算帧头），
             * 但 memmove 已经把 remain 个字节搬过来了（可能包含完整的
             * 长度、类型、载荷甚至 CRC）。
             * 于是状态机以为"只有 1 字节"，后续字节从 buf[1] 开始覆盖，
             * 把已经搬过来的好数据毁掉 —— 表现为
             * "坏帧之后的好帧被丢掉，且丢失的字节数正好等于帧长"。
             *
             * 正确做法：idx 置为 remain，再按 remain 的多少把状态机
             * 推进到正确的位置，并把长度字段解出来。 */
            p->idx = remain;

            if (remain == 1u) {
                p->state = GW_RX_WAIT_LEN;
            } else if (remain == 2u) {
                p->state = GW_RX_WAIT_LEN2;
            } else {
                /* 至少已有 HEAD + LEN 两个字节，可以解出长度 */
                uint16_t flen = get_u16(&p->buf[1]);

                if (flen < 1u || flen > GW_FRAME_MAX_LEN) {
                    /* 这个帧头也是假的：跳过它继续找 */
                    p->stat.rx_garbage++;
                    continue;
                }
                p->need = (uint16_t)(4u + flen);
                if (remain >= p->need) {
                    p->state = GW_RX_WAIT_CRC;      /* 已收满 */
                } else if (remain == 3u) {
                    p->state = GW_RX_WAIT_TYPE;
                } else {
                    p->state = GW_RX_WAIT_BODY;
                }
            }

            p->stat.rx_garbage += i;    /* i 个字节被确认为噪声 */
            return 1;
        }
    }

    /* 没找到新帧头：这些全是噪声 */
    p->stat.rx_garbage += have;
    parser_reset(p, 1);
    return 0;
}

/**
 * 内部：处理一段字节流（不含 pending 逻辑）
 *
 * @return 1 = 帧已收满并已停止处理（剩余字节需保存到 pending）
 *         0 = 本段全部处理完
 */
static int feed_inner(gw_frame_parser_t *p, const uint8_t *data, uint16_t len,
                      uint16_t *consumed)
{
    uint16_t i;

    *consumed = 0u;

    for (i = 0u; i < len; i++) {
        uint8_t b = data[i];

        /* ---- 一帧收满就停止，剩余字节由调用方转存 ----
         *
         * 踩过的坑：早期版本没有这个判断，一帧收满后
         * 后面的字节仍被继续塞进同一个缓冲区（idx 涨到 26 而 need 只有 9），
         * 缓冲区被后续帧覆盖破坏。
         * 现象是"粘包时只能解析出第一帧，其余全丢"。 */
        if (p->need != 0u && p->idx >= p->need) {
            *consumed = i;
            return 1;
        }

        switch (p->state) {
        case GW_RX_WAIT_HEAD:
            if (b == GW_FRAME_HEAD) {
                p->buf[0] = b;
                p->idx    = 1u;
                p->state  = GW_RX_WAIT_LEN;
            } else {
                p->stat.rx_garbage++;
            }
            break;

        case GW_RX_WAIT_LEN:
            p->buf[p->idx++] = b;
            p->state = GW_RX_WAIT_LEN2;
            break;

        case GW_RX_WAIT_LEN2:
            p->buf[p->idx++] = b;
            /* LEN = TYPE + PAYLOAD 的总长（大端） */
            {
                uint16_t flen = get_u16(&p->buf[1]);

                if (flen < 1u || flen > GW_FRAME_MAX_LEN) {
                    /* 长度非法：几乎可以肯定是假同步（噪声里恰好出现 0xA5）。
                       重同步而不是丢弃整块。 */
                    p->stat.rx_len_err++;
                    (void)parser_resync(p, 1);
                    break;
                }
                /* 总帧长 = HEAD(1) + LEN(2) + flen + CRC(1) */
                p->need  = (uint16_t)(4u + flen);
                p->state = GW_RX_WAIT_TYPE;
            }
            break;

        case GW_RX_WAIT_TYPE:
        case GW_RX_WAIT_BODY:
            if (p->idx >= GW_FRAME_MAX_TOTAL) {
                /* 防御：不该发生（need 已限长），但绝不允许越界写 */
                p->stat.rx_dropped++;
                parser_reset(p, 1);
                break;
            }
            p->buf[p->idx++] = b;
            /* need = 4 + flen：HEAD(1) + LEN(2) + flen + CRC(1)。
               收满 need 时最后一个字节就是 CRC，本帧已完整。
               置成 WAIT_CRC 表示"收满待取"；take() 按 idx >= need 判断，
               不依赖具体状态值。 */
            if (p->idx >= p->need) {
                p->state = GW_RX_WAIT_CRC;
            } else if (p->idx == 3u) {
                p->state = GW_RX_WAIT_TYPE;     /* 刚收完 TYPE，仍在等载荷 */
            } else {
                p->state = GW_RX_WAIT_BODY;
            }
            break;

        case GW_RX_WAIT_CRC:
            /* 正常情况下不会进入：收满后 feed_inner 会在循环开头提前返回。
               能走到这里只可能是重同步后 remain > need（缓冲区里已有多帧），
               此时当前帧的字节已在 buf 里，**绝不能**再往里追加 ——
               那会覆盖下一帧的开头。
               所以这里直接把状态机复位到"等帧头"，让 take() 先取走当前帧。 */
            *consumed = i;
            return 1;

        default:
            parser_reset(p, 1);
            break;
        }
    }

    *consumed = len;
    return 0;
}

/**
 * 对外接口：喂入字节流
 *
 * 处理顺序：
 *   1) 先把上次残留的 pending 字节吃掉（它们属于上一次粘包的后续帧）
 *   2) 再处理本次传入的字节
 *   3) 若中途某一帧收满，把剩余字节转存到 pending，留待下次
 *
 * 这样调用方的用法很简单：
 *     gw_frame_feed(&p, buf, len);
 *     while (gw_frame_take(&p, ...) == GW_FRAME_OK) { 处理; }
 * 不必自己管理"这次 feed 里有几帧、剩下多少字节"。
 */
void gw_frame_feed(gw_frame_parser_t *p, const uint8_t *data, uint16_t len)
{
    uint16_t consumed = 0u;

    if (p == 0) {
        return;
    }

    /* ---- 1) 先消化上次残留 ---- */
    if (p->pending_len > 0u) {
        uint8_t  tmp[GW_FRAME_MAX_TOTAL];
        uint16_t n = p->pending_len;
        uint16_t rest;

        memcpy(tmp, p->pending, n);
        p->pending_len = 0u;

        if (feed_inner(p, tmp, n, &consumed) != 0) {
            /* 又在其中收满了一帧：把剩余部分重新存起来 */
            rest = (uint16_t)(n - consumed);
            if (rest > 0u && rest <= (uint16_t)sizeof(p->pending)) {
                memcpy(p->pending, &tmp[consumed], rest);
                p->pending_len = rest;
            }
            return;                 /* 等调用方 take() 之后再来 */
        }
    }

    if (data == 0 || len == 0u) {
        return;
    }
    p->stat.rx_bytes += len;

    /* ---- 2) 处理本次数据 ----
       若中途收满一帧，把剩余字节存起来，等调用方 take() 之后再继续。
       不做"自动接着解析"是刻意的 —— 调用方需要在取帧后立刻处理它
       （例如回 ACK），自动往下解析会让处理顺序难以推理。 */
    if (feed_inner(p, data, len, &consumed) != 0) {
        uint16_t rest = (uint16_t)(len - consumed);

        if (rest > 0u && rest <= (uint16_t)sizeof(p->pending)) {
            memcpy(p->pending, &data[consumed], rest);
            p->pending_len = rest;
        } else if (rest > (uint16_t)sizeof(p->pending)) {
            p->stat.rx_dropped += rest;
        }
    }
}

/**
 * 内部：若 pending 里还有数据，继续往里喂
 *
 * 由 take() 在正常取走一帧后调用，让"一次 feed 里的多帧"
 * 能通过"反复 take"逐个取出来。
 */
static void feed_pending(gw_frame_parser_t *p)
{
    if (p->pending_len == 0u) {
        return;
    }
    gw_frame_feed(p, 0, 0u);        /* feed 内部会先消化 pending */
}

gw_frame_err_t gw_frame_take(gw_frame_parser_t *p, uint8_t *out_type,
                             uint8_t *payload, uint16_t cap, uint16_t *out_len)
{
    uint16_t flen;
    uint8_t  crc_calc;
    uint8_t  crc_recv;

    if (p == 0 || out_type == 0 || payload == 0 || out_len == 0) {
        return GW_FRAME_ERR_PARAM;
    }

    /* 还没收满一帧 */
    if (p->need == 0u || p->idx < p->need) {
        return GW_FRAME_ERR_NO_SPACE;
    }

    flen     = get_u16(&p->buf[1]);
    crc_recv = p->buf[p->need - 1u];
    /* CRC 覆盖 LEN_L + LEN_H + TYPE + PAYLOAD（即 buf[1..need-2]） */
    crc_calc = crc8_update(0x00u, &p->buf[1], (uint16_t)(p->need - 2u));

    if (crc_calc != crc_recv) {
        p->stat.rx_crc_err++;
        /* 关键：CRC 错不丢弃整块，从帧头的下一个字节重扫。
           否则连锁丢帧，表现为"偶尔中断、需重启恢复"。 */
        (void)parser_resync(p, 1);
        /* 复位后 pending 里的数据可以继续解析了 */
        feed_pending(p);
        return GW_FRAME_ERR_CRC;
    }

    /* 载荷长度 = flen - 1（flen 含 TYPE） */
    {
        uint16_t plen = (uint16_t)(flen - 1u);

        if (plen > cap) {
            p->stat.rx_dropped++;
            parser_reset(p, 1);
            feed_pending(p);
            return GW_FRAME_ERR_TOO_BIG;
        }
        if (plen > 0u) {
            memcpy(payload, &p->buf[4], plen);
        }
        *out_type = p->buf[3];
        *out_len  = plen;
    }

    p->stat.rx_frames++;
    parser_reset(p, 0);     /* 正常复位，不计 resync */

    /* 状态机已复位：把上次粘包留下的后续帧字节继续解析，
       这样调用方反复 take() 就能逐个取出所有帧。 */
    feed_pending(p);

    return GW_FRAME_OK;
}

/* ==================================================================
 *  组帧
 * ================================================================== */
gw_frame_err_t gw_frame_build(uint8_t *out, uint16_t cap, uint8_t type,
                              const uint8_t *payload, uint16_t payload_len,
                              uint16_t *out_len)
{
    uint16_t flen;
    uint16_t total;
    uint8_t  crc;

    if (out == 0 || out_len == 0) {
        return GW_FRAME_ERR_PARAM;
    }
    if (payload_len > GW_FRAME_MAX_PAYLOAD) {
        return GW_FRAME_ERR_TOO_BIG;
    }
    if (payload_len > 0u && payload == 0) {
        return GW_FRAME_ERR_PARAM;
    }

    flen  = (uint16_t)(payload_len + 1u);       /* TYPE + PAYLOAD */
    total = (uint16_t)(flen + 4u);              /* +HEAD +LEN +CRC */
    if (total > cap) {
        return GW_FRAME_ERR_NO_SPACE;
    }

    out[0] = GW_FRAME_HEAD;
    put_u16(&out[1], flen);
    out[3] = type;
    if (payload_len > 0u) {
        memcpy(&out[4], payload, payload_len);
    }
    /* CRC 覆盖 buf[1 .. 3+payload_len] */
    crc = crc8_update(0x00u, &out[1], (uint16_t)(3u + payload_len));
    out[total - 1u] = crc;

    *out_len = total;
    return GW_FRAME_OK;
}

const gw_frame_stat_t *gw_frame_stats(const gw_frame_parser_t *p)
{
    return (p == 0) ? 0 : &p->stat;
}

const char *gw_frame_strerror(gw_frame_err_t e)
{
    switch (e) {
    case GW_FRAME_OK:           return "OK";
    case GW_FRAME_ERR_PARAM:    return "invalid parameter";
    case GW_FRAME_ERR_TOO_BIG:  return "payload too large";
    case GW_FRAME_ERR_BAD_LEN:  return "illegal length field";
    case GW_FRAME_ERR_CRC:      return "CRC8 mismatch";
    case GW_FRAME_ERR_NO_SPACE: return "no complete frame available";
    default:                    return "unknown error";
    }
}

const char *gw_frame_type_str(uint8_t type)
{
    switch (type) {
    case GW_FT_NONE:        return "NONE";
    case GW_FT_WIFI_CONFIG: return "WIFI_CONFIG";
    case GW_FT_MQTT_CONFIG: return "MQTT_CONFIG";
    case GW_FT_TELEMETRY:   return "TELEMETRY";
    case GW_FT_STATUS_SET:  return "STATUS_SET";
    case GW_FT_OTA_BEGIN:   return "OTA_BEGIN";
    case GW_FT_OTA_CHUNK:   return "OTA_CHUNK";
    case GW_FT_OTA_END:     return "OTA_END";
    case GW_FT_LINK_STATUS: return "LINK_STATUS";
    case GW_FT_COMMAND:     return "COMMAND";
    case GW_FT_OTA_ACK:     return "OTA_ACK";
    case GW_FT_LOG:         return "LOG";
    case GW_FT_ACK:         return "ACK";
    case GW_FT_NACK:        return "NACK";
    default:                return "UNKNOWN";
    }
}

/* ==================================================================
 *  遥测数据序列化
 *
 *  显式逐字段序列化，**不 memcpy 结构体**：
 *    STM32（ARM，32 位）与 ESP32（Xtensa，32 位）的对齐/填充规则不同，
 *    直接把结构体写进串口会导致两端字段错位。
 *    这类 bug 在两端代码各自"看起来都对"时极难发现。
 * ================================================================== */

uint16_t gw_telemetry_pack(const gw_telemetry_t *t, uint8_t *out, uint16_t cap)
{
    uint8_t i;

    if (t == 0 || out == 0 || cap < GW_TELEMETRY_LEN) {
        return 0u;
    }

    put_u32(&out[0],  t->dev_id);
    put_u32(&out[4],  t->ts);
    put_u32(&out[8],  t->seq);
    put_u16(&out[12], (uint16_t)t->temp_x10);
    put_u16(&out[14], t->humi_x10);
    for (i = 0u; i < 4u; i++) {
        put_u16(&out[16u + i * 2u], t->can[i]);
    }
    put_u16(&out[24], t->status);
    put_u16(&out[26], t->uptime_s);

    return GW_TELEMETRY_LEN;
}

int gw_telemetry_unpack(const uint8_t *in, uint16_t len, gw_telemetry_t *out)
{
    uint8_t i;

    if (in == 0 || out == 0 || len < GW_TELEMETRY_LEN) {
        return -1;
    }

    out->dev_id   = get_u32(&in[0]);
    out->ts       = get_u32(&in[4]);
    out->seq      = get_u32(&in[8]);
    out->temp_x10 = (int16_t)get_u16(&in[12]);
    out->humi_x10 = get_u16(&in[14]);
    for (i = 0u; i < 4u; i++) {
        out->can[i] = get_u16(&in[16u + i * 2u]);
    }
    out->status   = get_u16(&in[24]);
    out->uptime_s = get_u16(&in[26]);
    return 0;
}
