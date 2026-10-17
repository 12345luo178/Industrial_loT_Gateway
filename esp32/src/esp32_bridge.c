/**
 * @file    esp32_bridge.c
 * @brief   ESP32 桥接状态机实现（纯逻辑，零依赖）
 */
#include "esp32_bridge.h"

#include <string.h>

/* ==================================================================
 *  重连退避
 * ================================================================== */

int esp32_backoff_init(esp32_backoff_t *b)
{
    if (b == 0) {
        return -1;
    }
    memset(b, 0, sizeof(*b));
    b->cur_delay_s = ESP32_BACKOFF_MIN_S;
    b->inited      = 1u;
    return 0;
}

void esp32_backoff_fail(esp32_backoff_t *b, uint32_t now_ms)
{
    if (b == 0 || b->inited == 0u) {
        return;
    }

    b->attempts++;
    b->waiting = 1u;

    /* ⚠️ 退避是"下一次要等多久"，
       所以第一次失败后应该等 MIN（1s），而不是等 MIN×2。 */
    b->next_try_ms = now_ms + b->cur_delay_s * 1000u;

    /* 翻倍，但不超上限 */
    if (b->cur_delay_s < ESP32_BACKOFF_MAX_S) {
        b->cur_delay_s *= 2u;
        if (b->cur_delay_s > ESP32_BACKOFF_MAX_S) {
            b->cur_delay_s = ESP32_BACKOFF_MAX_S;
        }
    }
}

void esp32_backoff_ok(esp32_backoff_t *b)
{
    if (b == 0 || b->inited == 0u) {
        return;
    }

    /* ⚠️ 成功必须**完全重置**，包括 attempts 与 cur_delay_s。
       只重置 waiting 是不行的：那样下次断线会接着上次的
       60s 开始，现场表现是"重启后第一次断线要等一分钟"。 */
    b->waiting     = 0u;
    b->attempts    = 0u;
    b->cur_delay_s = ESP32_BACKOFF_MIN_S;
    b->next_try_ms = 0u;
}

int esp32_backoff_ready(const esp32_backoff_t *b, uint32_t now_ms)
{
    if (b == 0 || b->inited == 0u) {
        return 0;
    }
    if (b->waiting == 0u) {
        return 1;               /* 没在退避：随时可以试 */
    }
    /* ⚠️ 用有符号差值比较，不要写 `now_ms >= next_try_ms`：
       HAL_GetTick 会回绕（49.7 天），无符号直接比大小会在
       回绕点附近得出错误结论 —— "还要等 49 天"。
       有符号差值在回绕前后都正确（只要间隔不超过 2^31 毫秒）。 */
    return ((int32_t)(now_ms - b->next_try_ms) >= 0) ? 1 : 0;
}

uint32_t esp32_backoff_delay(const esp32_backoff_t *b)
{
    if (b == 0 || b->inited == 0u) {
        return ESP32_BACKOFF_MIN_S;
    }
    return b->cur_delay_s;
}

/* ==================================================================
 *  待确认指令
 * ================================================================== */

int esp32_cmd_track_init(esp32_cmd_track_t *t)
{
    if (t == 0) {
        return -1;
    }
    memset(t, 0, sizeof(*t));
    t->inited = 1u;
    return 0;
}

int esp32_cmd_track_start(esp32_cmd_track_t *t, const char *text,
                          uint32_t req_id, int has_req_id, uint32_t now_ms)
{
    uint16_t n;

    if (t == 0 || t->inited == 0u || text == 0) {
        return -1;
    }

    /* ⚠️ 已有一条在等：**不覆盖**，返回 1 让调用方决定排队还是拒绝。
       覆盖会让前一条永远收不到回执，云端一直等到超时，
       而设备其实什么都没做。 */
    if (t->active != 0u) {
        return 1;
    }

    n = (uint16_t)strlen(text);
    if (n == 0u || n >= (uint16_t)ESP32_CMD_TEXT_MAX) {
        return -2;              /* 空指令或超长：都不该发出去 */
    }

    memcpy(t->text, text, n);
    t->text[n]     = '\0';
    t->req_id      = req_id;
    t->has_req_id  = (has_req_id != 0) ? 1u : 0u;
    t->sent_at_ms  = now_ms;
    t->retry_left  = ESP32_CMD_MAX_RETRY;
    t->active      = 1u;

    return 0;
}

int esp32_cmd_track_ack(esp32_cmd_track_t *t, const char *ack_text,
                        int ok, char *out_text, uint16_t cap,
                        uint32_t *out_req_id, int *out_has_req_id)
{
    uint16_t n;

    if (t == 0 || t->inited == 0u) {
        return 0;
    }
    /* 没有待确认的指令：这是一条**迟到的 ACK**。
       忽略它是正确的 —— 那条指令早就超时并回失败给云端了。
       如果在这里"顺便"把它当成有效，云端会收到两条回执。 */
    if (t->active == 0u) {
        return 0;
    }

    (void)ok;

    if (out_text != 0 && cap > 0u) {
        if (ack_text == 0) {
            out_text[0] = '\0';
        } else {
            n = (uint16_t)strlen(ack_text);
            if (n >= cap) {
                n = (uint16_t)(cap - 1u);
            }
            memcpy(out_text, ack_text, n);
            out_text[n] = '\0';
        }
    }
    if (out_req_id != 0) {
        *out_req_id = t->req_id;
    }
    if (out_has_req_id != 0) {
        *out_has_req_id = (int)t->has_req_id;
    }

    /* 清空待确认状态，可以接下一條 */
    t->active = 0u;
    t->text[0] = '\0';
    return 1;
}

int esp32_cmd_track_tick(esp32_cmd_track_t *t, uint32_t now_ms)
{
    if (t == 0 || t->inited == 0u || t->active == 0u) {
        return 0;
    }

    /* 用有符号差值，理由同 backoff_ready（tick 回绕） */
    if ((int32_t)(now_ms - t->sent_at_ms)
        < (int32_t)ESP32_CMD_ACK_TIMEOUT_MS) {
        return 0;               /* 还没超时 */
    }

    if (t->retry_left > 0u) {
        t->retry_left--;
        t->sent_at_ms = now_ms;     /* 重发后重新计时 */
        return 1;                   /* 应该重发 */
    }

    /* 重发次数用完：放弃。
       ⚠️ 这里**不清 active** —— 调用方要先
          esp32_cmd_track_take_failed() 取走内容，
          再调用它来清空（见下面的实现）。
          直接清掉的话，那条失败的回执就永远发不出去了。 */
    return 2;
}

int esp32_cmd_track_text(const esp32_cmd_track_t *t,
                         char *out, uint16_t cap, uint16_t *out_len)
{
    uint16_t n;

    if (t == 0 || t->inited == 0u || out == 0 || cap == 0u) {
        return 0;
    }
    if (t->active == 0u) {
        return 0;
    }

    n = (uint16_t)strlen(t->text);
    if (n >= cap) {
        return 0;               /* 缓冲太小：不返回截断的指令 */
    }
    memcpy(out, t->text, n);
    out[n] = '\0';
    if (out_len != 0) {
        *out_len = n;
    }
    return 1;
}

int esp32_cmd_track_take_failed(esp32_cmd_track_t *t,
                                char *out_text, uint16_t cap,
                                uint32_t *out_req_id, int *out_has_req_id)
{
    if (t == 0 || t->inited == 0u || t->active == 0u) {
        return 0;
    }

    if (out_text != 0 && cap > 0u) {
        uint16_t n = (uint16_t)strlen(t->text);

        if (n >= cap) {
            n = (uint16_t)(cap - 1u);
        }
        memcpy(out_text, t->text, n);
        out_text[n] = '\0';
    }
    if (out_req_id != 0) {
        *out_req_id = t->req_id;
    }
    if (out_has_req_id != 0) {
        *out_has_req_id = (int)t->has_req_id;
    }

    /* 取走之后才清空 */
    t->active  = 0u;
    t->text[0] = '\0';
    return 1;
}

int esp32_cmd_track_busy(const esp32_cmd_track_t *t)
{
    if (t == 0 || t->inited == 0u) {
        return 0;
    }
    return (t->active != 0u) ? 1 : 0;
}

/* ==================================================================
 *  遥测积压环形缓冲
 * ================================================================== */

int esp32_telem_ring_init(esp32_telem_ring_t *r)
{
    if (r == 0) {
        return -1;
    }
    memset(r, 0, sizeof(*r));
    r->inited = 1u;
    return 0;
}

int esp32_telem_ring_push(esp32_telem_ring_t *r, const char *json,
                          uint16_t len)
{
    int rc = 0;

    if (r == 0 || r->inited == 0u || json == 0) {
        return -1;
    }
    if (len == 0u || len >= (uint16_t)ESP32_TELEM_JSON_MAX) {
        return -2;              /* 空或超长：不入队，避免存半截 JSON */
    }

    /* 满了：丢**最旧**的一条，给新数据腾位置。
       丢最新会让云端永远看到过期数据，那更糟。 */
    if (r->count >= (uint16_t)ESP32_TELEM_RING_DEPTH) {
        r->tail = (uint16_t)((r->tail + 1u) % (uint16_t)ESP32_TELEM_RING_DEPTH);
        r->count--;
        r->dropped++;
        rc = 1;
    }

    memcpy(r->buf[r->head], json, len);
    r->buf[r->head][len] = '\0';
    r->head = (uint16_t)((r->head + 1u) % (uint16_t)ESP32_TELEM_RING_DEPTH);
    r->count++;

    return rc;
}

int esp32_telem_ring_peek(esp32_telem_ring_t *r, char *out, uint16_t cap,
                          uint16_t *out_len)
{
    uint16_t n;

    if (r == 0 || r->inited == 0u || out == 0 || cap == 0u) {
        return 0;
    }
    if (r->count == 0u) {
        return 0;
    }

    n = (uint16_t)strlen(r->buf[r->tail]);
    if (n >= cap) {
        /* 调用方缓冲太小：不返回截断的 JSON */
        return 0;
    }
    memcpy(out, r->buf[r->tail], n);
    out[n] = '\0';
    if (out_len != 0) {
        *out_len = n;
    }
    return 1;
}

void esp32_telem_ring_pop(esp32_telem_ring_t *r)
{
    if (r == 0 || r->inited == 0u || r->count == 0u) {
        return;
    }
    r->tail = (uint16_t)((r->tail + 1u) % (uint16_t)ESP32_TELEM_RING_DEPTH);
    r->count--;
}

uint16_t esp32_telem_ring_count(const esp32_telem_ring_t *r)
{
    return (r == 0) ? 0u : r->count;
}

uint32_t esp32_telem_ring_dropped(const esp32_telem_ring_t *r)
{
    return (r == 0) ? 0u : r->dropped;
}
