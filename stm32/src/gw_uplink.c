/**
 * @file    gw_uplink.c
 * @brief   上云链路管理实现（纯逻辑，可在 PC 上单元测试）
 *
 * ============================================================================
 *  一条遥测的完整生命周期（这是理解本模块的关键）
 *
 *     submit()
 *        |  分配 seq，压入环形缓存
 *        v
 *     [环形缓存]  <- 断网期间数据停在这里，满了丢最旧并计数
 *        |
 *        |  tick() 发现链路 CONNECTED 且没有待确认帧
 *        v
 *     ring_pop() 取出 -> 存入 pending -> 组帧发送 -> awaiting_ack = 1
 *        |
 *        +--- 收到 ACK ------> 统计 sent_ok，pending 丢弃（数据已确认送达）
 *        |
 *        +--- 超时 --------> retry_left-- 后重发同一帧
 *        |
 *        +--- 重试用尽 -----> 统计 send_fail，pending 丢弃
 *
 *  为什么要有 pending 这一层，而不是"发失败就放回队列"：
 *    放回队列会让"重发"与"新数据入队"的顺序纠缠在一起 ——
 *    重发的帧可能被新数据挤到后面，序号就不再单调递增，
 *    云端按 seq 去重时会误判。
 *    把"正在等确认的那一帧"单独存起来，环形缓存只放"还没开始发"的数据，
 *    顺序语义就清晰了。
 * ============================================================================
 */
#include "gw_uplink.h"

#include <string.h>

/* ------------------------------------------------------------------
 * 退避序列：1, 2, 4, 8, 16, 32, 60（上限 60 秒）
 *
 * 为什么用指数退避：固定间隔在长时间断网时会持续冲击 AP 与 Broker，
 *   既费电又可能被对端限流；指数退避让重试压力随时间下降，
 *   同时保证恢复后能快速接回。
 *
 * 为什么要设上限：若一直翻倍，断网 10 分钟后重试间隔会到十几分钟，
 *   实际网络早已恢复而设备迟迟不回来，运维会误判成设备故障。
 * ------------------------------------------------------------------ */
static uint32_t backoff_step(uint32_t idx)
{
    uint32_t v = 1u;
    uint32_t i;

    for (i = 0u; i < idx; i++) {
        v <<= 1;
        if (v >= GW_UPLINK_BACKOFF_MAX_S) {
            return GW_UPLINK_BACKOFF_MAX_S;
        }
    }
    return (v > GW_UPLINK_BACKOFF_MAX_S) ? GW_UPLINK_BACKOFF_MAX_S : v;
}

/* ------------------------------------------------------------------
 * 环形缓存
 * ------------------------------------------------------------------ */

/* 入队。满了则丢最旧并计数 —— 见头文件里的溢出策略说明。
   "丢包计数必须可见"是本模块的一条硬要求：
   否则上层会以为一条都没丢，而实际上历史已经出现空洞。 */
static void ring_push(gw_uplink_t *u, const gw_telemetry_t *t)
{
    if (u->count >= GW_UPLINK_RING_DEPTH) {
        u->tail = (uint16_t)((u->tail + 1u) % GW_UPLINK_RING_DEPTH);
        u->stat.dropped_overflow++;
        u->count--;
    }
    u->ring[u->head] = *t;
    u->head = (uint16_t)((u->head + 1u) % GW_UPLINK_RING_DEPTH);
    u->count++;
    u->stat.queued++;
}

static int ring_pop(gw_uplink_t *u, gw_telemetry_t *out)
{
    if (u->count == 0u) {
        return 0;
    }
    *out = u->ring[u->tail];
    u->tail = (uint16_t)((u->tail + 1u) % GW_UPLINK_RING_DEPTH);
    u->count--;
    return 1;
}

/* ------------------------------------------------------------------
 * 组帧发送
 * ------------------------------------------------------------------ */

static int send_telemetry(gw_uplink_t *u, const gw_telemetry_t *t)
{
    uint8_t  payload[GW_TELEMETRY_LEN];
    uint8_t  frame[GW_FRAME_MAX_TOTAL];
    uint16_t plen;
    uint16_t flen = 0u;

    plen = gw_telemetry_pack(t, payload, sizeof(payload));
    if (plen == 0u) {
        return -1;
    }
    if (gw_frame_build(frame, sizeof(frame), GW_FT_TELEMETRY,
                       payload, plen, &flen) != GW_FRAME_OK) {
        return -1;
    }
    return u->io.send(u->io.ctx, frame, flen);
}

static int send_control(gw_uplink_t *u, uint8_t type)
{
    uint8_t  frame[GW_FRAME_MAX_TOTAL];
    uint16_t flen = 0u;

    if (gw_frame_build(frame, sizeof(frame), type, 0, 0u, &flen) != GW_FRAME_OK) {
        return -1;
    }
    return u->io.send(u->io.ctx, frame, flen);
}

/* 把 pending 里的那一帧重新发一遍（重试用） */
static int resend_pending(gw_uplink_t *u)
{
    return send_telemetry(u, &u->pending);
}

/* ------------------------------------------------------------------
 * 状态迁移
 * ------------------------------------------------------------------ */

static void enter_state(gw_uplink_t *u, gw_link_state_t st)
{
    if (u->state == st) {
        return;
    }

    /* 记录"本次断网持续了多久" —— 现场排查最关心的数字之一 */
    if (st == GW_LINK_DISCONNECTED || st == GW_LINK_AUTH_FAILED) {
        if (u->offline_since_s == 0u) {
            u->offline_since_s = u->uptime_s;
        }
    } else if (st == GW_LINK_CONNECTED) {
        if (u->offline_since_s != 0u) {
            u->stat.last_offline_s = u->uptime_s - u->offline_since_s;
            u->offline_since_s = 0u;
        }
    }

    u->state    = st;
    u->state_ms = u->last_ms;

    if (st == GW_LINK_CONNECTED) {
        u->backoff_s      = 1u;     /* 恢复后重置退避，下次断网从头开始 */
        u->flush_in_batch = 0u;
        u->hb_ms          = u->last_ms;
    }
}

/* ==================================================================
 *  对外接口
 * ================================================================== */

gw_uplink_err_t gw_uplink_init(gw_uplink_t *u, const gw_uplink_io_t *io,
                               uint32_t dev_id)
{
    if (u == 0 || io == 0 || io->send == 0 || io->now_ms == 0) {
        return GW_UPLINK_ERR_PARAM;
    }
    if (dev_id == 0u) {
        /* 设备 ID 为 0 会导致云端无法区分设备，早失败好过上线后才发现 */
        return GW_UPLINK_ERR_PARAM;
    }

    memset(u, 0, sizeof(*u));
    u->io     = *io;
    u->dev_id = dev_id;

    /* 序号从 1 开始：0 留给"未初始化/无效"的语义 */
    u->next_seq = 1u;

    u->state      = GW_LINK_DISCONNECTED;
    u->backoff_s  = 1u;
    u->last_ms    = io->now_ms(io->ctx);
    u->retry_left = GW_UPLINK_MAX_RETRY;
    u->inited     = 1u;

    return GW_UPLINK_OK;
}

gw_uplink_err_t gw_uplink_submit(gw_uplink_t *u, gw_telemetry_t *t)
{
    if (u == 0 || t == 0 || u->inited == 0u) {
        return GW_UPLINK_ERR_PARAM;
    }

    /* 序号与设备 ID 由本模块统一填充，调用方不必关心 ——
       这样"云端去重"依赖的 seq 一定单调递增，不会因为调用方忘记递增而出错。 */
    t->seq    = u->next_seq;
    t->dev_id = u->dev_id;
    u->next_seq++;

    ring_push(u, t);

    /* 链路正常且没有待确认帧时立刻尝试发出去，降低时延。
       发送失败也不丢数据：帧仍在环形缓存里，tick 会继续尝试。 */
    if (u->state == GW_LINK_CONNECTED && u->awaiting_ack == 0u) {
        gw_telemetry_t head;

        if (ring_pop(u, &head)) {
            if (send_telemetry(u, &head) == 0) {
                u->pending      = head;
                u->awaiting_ack = 1u;
                u->retry_left   = GW_UPLINK_MAX_RETRY;
                u->ack_ms       = u->last_ms;
                /* 计数与补传路径共用，这里也要推进，否则"本批已发条数"
                   会与实际发出的帧数脱节（见 tick 里重置处的说明）。 */
                u->flush_in_batch++;
            } else {
                /* 发送失败：数据必须先回到队列，绝不能丢 */
                u->count++;
                u->tail = (uint16_t)((u->tail + GW_UPLINK_RING_DEPTH - 1u)
                                     % GW_UPLINK_RING_DEPTH);
                u->ring[u->tail] = head;
                u->stat.send_fail++;
            }
        }
    }

    return GW_UPLINK_OK;
}

void gw_uplink_tick(gw_uplink_t *u, uint32_t elapsed_ms)
{
    if (u == 0 || u->inited == 0u) {
        return;
    }

    /* ---- 时间维护 ----
       毫秒累加器放在结构体里，不用函数级 static：
       static 会让多个实例互相干扰，也破坏单元测试的独立性。 */
    u->last_ms += elapsed_ms;
    u->ms_acc  += elapsed_ms;
    while (u->ms_acc >= 1000u) {
        u->ms_acc -= 1000u;
        u->uptime_s++;
    }

    /* ---- 等待 ACK 的超时与重试 ---- */
    if (u->awaiting_ack != 0u) {
        if ((u->last_ms - u->ack_ms) >= GW_UPLINK_ACK_TIMEOUT_MS) {
            if (u->retry_left > 0u) {
                u->retry_left--;
                u->ack_ms = u->last_ms;         /* 重新计时 */
                if (resend_pending(u) != 0) {
                    /* 重发都发不出去（串口故障），视为本条失败 */
                    u->awaiting_ack = 0u;
                    u->stat.send_fail++;
                }
            } else {
                /* 重试用尽：本条上报失败。
                   注意：**不把链路判为断开** —— 单条数据发送失败
                   与链路好坏是两件事，链路状态由 ESP32 的 LINK_STATUS 决定。
                   混在一起会导致"偶尔丢一条就疯狂重连"。 */
                u->awaiting_ack = 0u;
                u->stat.send_fail++;
            }
        }
    }

    /* ---- 按链路状态推进 ---- */
    switch (u->state) {
    case GW_LINK_CONNECTED:
        /* 心跳：即使没有遥测也要定期发，双向确认链路存活 */
        if ((u->last_ms - u->hb_ms) >= (GW_UPLINK_HEARTBEAT_S * 1000u)) {
            u->hb_ms = u->last_ms;
            if (send_control(u, GW_FT_LINK_STATUS) == 0) {
                u->stat.heartbeats++;
            }
        }
        break;

    case GW_LINK_DISCONNECTED:
        /* 按退避窗口重试 */
        if ((u->last_ms - u->state_ms) >= (u->backoff_s * 1000u)) {
            u->stat.reconnect_tries++;
            u->stat.backoff_events++;
            if (send_control(u, GW_FT_MQTT_CONFIG) == 0) {
                enter_state(u, GW_LINK_CONNECTING);
            } else {
                /* 连请求都发不出去（STM32<->ESP32 串口有问题）：
                   抬高退避，下轮再试 */
                u->backoff_s = backoff_step(u->stat.backoff_events);
                u->state_ms  = u->last_ms;
            }
        }
        break;

    case GW_LINK_CONNECTING:
        /* 等 ESP32 上报结果。超过退避窗口还没结果 -> 视为失败，进入下一次退避 */
        if ((u->last_ms - u->state_ms) >= (u->backoff_s * 1000u)) {
            u->backoff_s = backoff_step(u->stat.backoff_events);
            enter_state(u, GW_LINK_DISCONNECTED);
        }
        break;

    case GW_LINK_AUTH_FAILED:
        /* 鉴权失败不自动重试：重试不会成功，只会刷日志与耗电。
           等上层更新密钥后调用 gw_uplink_set_link_state() 恢复。 */
        break;

    default:
        enter_state(u, GW_LINK_DISCONNECTED);
        break;
    }

    /* ---- 补传：链路正常且有积压时按批发送 ----
       分批是刻意的：一次性把几百条打出去会长时间占用串口，
       导致下行指令（例如急停）被延迟处理。 */
    if (u->state == GW_LINK_CONNECTED && u->awaiting_ack == 0u
        && u->count > 0u) {
        if (u->flush_in_batch >= GW_UPLINK_FLUSH_BATCH) {
            /* 本批已满：让出一次。
               ⚠️ 这里必须清零，否则会因为"已经发满一批"而永久不再发送。
               踩过的坑：早期版本在**发送成功后**就把计数清了，
               结果批内限制完全失效；改成只在"本批已满且确实要停下"时清零，
               并且让所有发送路径（submit / 补传）都推进同一个计数。 */
            u->flush_in_batch = 0u;
            return;
        }

        {
            gw_telemetry_t head;

            if (ring_pop(u, &head)) {
                if (send_telemetry(u, &head) == 0) {
                    u->pending      = head;
                    u->awaiting_ack = 1u;
                    u->retry_left   = GW_UPLINK_MAX_RETRY;
                    u->ack_ms       = u->last_ms;
                    u->flush_in_batch++;
                } else {
                    /* 发送失败：放回队列（用与 submit 相同的回滚方式） */
                    u->count++;
                    u->tail = (uint16_t)((u->tail + GW_UPLINK_RING_DEPTH - 1u)
                                         % GW_UPLINK_RING_DEPTH);
                    u->ring[u->tail] = head;
                    u->stat.send_fail++;
                }
            }
        }
    }
}

/* 应用一次链路状态变化（把"能不能切状态"的判断抽出来，
   便于被 on_frame 与延迟处理两条路径共用） */
static void apply_link_state(gw_uplink_t *u, gw_link_state_t st)
{
    if (st == u->state) {
        return;
    }
    if (st == GW_LINK_CONNECTED) {
        u->stat.reconnects++;
    }
    enter_state(u, st);
}

gw_uplink_err_t gw_uplink_on_frame(gw_uplink_t *u, uint8_t type,
                                   const uint8_t *payload, uint16_t len)
{
    if (u == 0 || u->inited == 0u) {
        return GW_UPLINK_ERR_PARAM;
    }

    switch (type) {
    case GW_FT_LINK_STATUS:
        /* 载荷约定：[0]=wifi_state [1]=mqtt_state [2..3]=rssi
           第 2 字节与 gw_link_state_t 同值域。 */
        if (len >= 2u) {
            gw_link_state_t st = (gw_link_state_t)payload[1];

            /* ---- 关键：等 ACK 期间不能当场切换链路状态 ----
             *
             * 踩过的坑：ESP32 上报"已连接"时若本模块正处于"等待 ACK"状态，
             * 直接 enter_state() 会把状态从 CONNECTED 改成别的，
             * 或者把刚发出的请求的确认窗口作废 ——
             * 结果是"链路明明通了，数据却发不出去，状态卡在 CONNECTING"。
             *
             * 正确做法：把这条状态通知**延迟**到 ACK 处理完之后再应用。
             * 只延迟一条（够用且不会累积）。 */
            if (u->awaiting_ack != 0u) {
                uint16_t k;
                u->defer_type  = type;
                u->defer_len   = (len > sizeof(u->defer_payload))
                                 ? (uint16_t)sizeof(u->defer_payload) : len;
                for (k = 0u; k < u->defer_len; k++) {
                    u->defer_payload[k] = payload[k];
                }
                u->defer_valid = 1u;
                return GW_UPLINK_OK;
            }

            apply_link_state(u, st);
        } else if (len >= 1u) {
            /* 只给了 Wi-Fi 状态：连上 Wi-Fi 不等于 MQTT 可用，
               所以只在"断开"时更新状态，不把 Wi-Fi 上线的当成 MQTT 就绪。 */
            if ((gw_link_state_t)payload[0] == GW_LINK_DISCONNECTED) {
                enter_state(u, GW_LINK_DISCONNECTED);
            }
        }
        return GW_UPLINK_OK;

    case GW_FT_ACK:
        /* 确认收到：本条数据真正送达，可以丢弃 pending */
        u->awaiting_ack = 0u;
        u->stat.sent_ok++;
        {
            uint32_t rtt = u->last_ms - u->ack_ms;
            u->stat.last_rtt_ms = (uint16_t)((rtt > 0xFFFFu) ? 0xFFFFu : rtt);
            if (u->stat.last_rtt_ms > u->stat.max_rtt_ms) {
                u->stat.max_rtt_ms = u->stat.last_rtt_ms;
            }
        }
        /* ACK 处理完，应用之前延迟的链路状态变化 */
        if (u->defer_valid != 0u
            && u->defer_type == GW_FT_LINK_STATUS
            && u->defer_len >= 2u) {
            gw_link_state_t st = (gw_link_state_t)u->defer_payload[1];
            u->defer_valid = 0u;
            apply_link_state(u, st);
        }
        return GW_UPLINK_OK;

    case GW_FT_NACK:
        /* 对端明确拒绝：不重试，直接记为失败并停等 ACK，
           避免把链路时间浪费在一个对端不接受的数据上。 */
        u->awaiting_ack = 0u;
        u->stat.send_fail++;
        if (u->defer_valid != 0u
            && u->defer_type == GW_FT_LINK_STATUS
            && u->defer_len >= 2u) {
            gw_link_state_t st = (gw_link_state_t)u->defer_payload[1];
            u->defer_valid = 0u;
            apply_link_state(u, st);
        }
        return GW_UPLINK_OK;

    default:
        /* 其它类型属于业务（下行指令、日志转发），交给上层。
           本模块刻意不解释它们 —— 保持"只负责链路"的边界。 */
        return GW_UPLINK_ERR_PARAM;
    }
}

void gw_uplink_set_link_state(gw_uplink_t *u, gw_link_state_t st)
{
    if (u == 0 || u->inited == 0u) {
        return;
    }
    apply_link_state(u, st);
}

gw_link_state_t gw_uplink_state(const gw_uplink_t *u)
{
    return (u == 0) ? GW_LINK_DISCONNECTED : u->state;
}

uint16_t gw_uplink_pending(const gw_uplink_t *u)
{
    return (u == 0) ? 0u : u->count;
}

const gw_uplink_stat_t *gw_uplink_stats(const gw_uplink_t *u)
{
    return (u == 0) ? 0 : &u->stat;
}

uint32_t gw_uplink_backoff_s(const gw_uplink_t *u)
{
    return (u == 0) ? 0u : u->backoff_s;
}

const char *gw_uplink_strerror(gw_uplink_err_t e)
{
    switch (e) {
    case GW_UPLINK_OK:          return "OK";
    case GW_UPLINK_ERR_PARAM:   return "invalid parameter";
    case GW_UPLINK_ERR_OFFLINE: return "link offline";
    case GW_UPLINK_ERR_TIMEOUT: return "ack timeout";
    case GW_UPLINK_ERR_SEND:    return "send failed";
    default:                    return "unknown error";
    }
}

const char *gw_link_state_str(gw_link_state_t st)
{
    switch (st) {
    case GW_LINK_DISCONNECTED: return "DISCONNECTED";
    case GW_LINK_CONNECTING:   return "CONNECTING";
    case GW_LINK_CONNECTED:    return "CONNECTED";
    case GW_LINK_AUTH_FAILED:  return "AUTH_FAILED";
    default:                   return "UNKNOWN";
    }
}
