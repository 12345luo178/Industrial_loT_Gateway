/**
 * @file    gw_baud.c
 * @brief   RS485 波特率运行时切换实现（纯逻辑）
 */
#include "gw_baud.h"

#include <string.h>

int gw_baud_supported(uint32_t bps)
{
    switch (bps) {
    case 9600u:
    case 19200u:
    case 38400u:
    case 115200u:
        return 1;
    default:
        return 0;
    }
}

uint32_t gw_baud_from_code(uint16_t code)
{
    switch (code) {
    case (uint16_t)GW_BAUD_9600:   return 9600u;
    case (uint16_t)GW_BAUD_19200:  return 19200u;
    case (uint16_t)GW_BAUD_38400:  return 38400u;
    case (uint16_t)GW_BAUD_115200: return 115200u;
    default:                       return 0u;
    }
}

int gw_baud_init(gw_baud_t *b, uint32_t initial)
{
    if (b == 0) {
        return -1;
    }
    if (gw_baud_supported(initial) == 0) {
        return -2;              /* 初始值非法：调用方给错了，不能静默接受 */
    }

    memset(b, 0, sizeof(*b));
    b->current = initial;
    b->pending = 0u;
    b->inited  = 1u;
    return 0;
}

int gw_baud_request(gw_baud_t *b, uint32_t bps)
{
    if (b == 0 || b->inited == 0u) {
        return -1;
    }

    /* 传 0：取消待切换的请求。
       为什么需要它：上位机可能写了非法值、或者操作员改主意了。
       没有取消途径的话，那个请求会一直挂着。 */
    if (bps == 0u) {
        b->pending = 0u;
        b->quiet_acc_ms = 0u;
        return 0;
    }

    if (gw_baud_supported(bps) == 0) {
        b->rejected++;
        return -2;
    }

    /* 已经是目标值：清掉可能存在的旧请求，报告"无需切换" */
    if (bps == b->current) {
        b->pending = 0u;
        b->quiet_acc_ms = 0u;
        return 1;
    }

    /* 同一个目标重复请求：不重复计数（上位机会周期性写同一个值） */
    if (bps != b->pending) {
        b->pending = bps;
        b->quiet_acc_ms = 0u;   /* 目标变了，静默期重新计 */
        b->requests++;
    }
    return 0;
}

int gw_baud_tick(gw_baud_t *b, uint32_t elapsed_ms,
                 int tx_busy, int in_wait)
{
    if (b == 0 || b->inited == 0u) {
        return 0;
    }
    if (b->pending == 0u) {
        return 0;               /* 没有待切换的请求 */
    }

    /* ---- 条件 ①：不能正在发送 ----
       发送中途改 BRR 会让当前帧后半段用新波特率发出，
       对方收到半个帧 + 乱码，而且会因此错位同步、连续几帧失败。 */
    if (tx_busy != 0) {
        b->quiet_acc_ms = 0u;   /* 总线忙：静默期重新计 */
        return 0;
    }

    /* ---- 条件 ②：不能在主站等待窗口内 ----
       此时改波特率会让响应收不到，而主站会把这算成"从机超时"，
       进而把一台正常的从机标记为离线 —— 那是误判。 */
    if (in_wait != 0) {
        b->quiet_acc_ms = 0u;
        return 0;
    }

    /* ---- 条件 ③：静默期 ----
       上位机可能刚发完请求、3.5 字符的帧界判定还没到。
       再等一小段，确保这一帧已经被收完。 */
    b->quiet_acc_ms += elapsed_ms;
    if (b->quiet_acc_ms < GW_BAUD_QUIET_MS) {
        return 0;
    }

    return 1;                   /* 现在安全：调用方去改硬件 */
}

void gw_baud_commit(gw_baud_t *b, uint32_t bps)
{
    if (b == 0 || b->inited == 0u) {
        return;
    }

    /* ⚠️ 以**硬件实际值**为准，而不是 pending。
       如果移植层没能设成目标值（例如 BRR 算不出精确分频），
       状态机会如实记录实际值 —— 否则"报文里说的"与"实际跑的"不一致，
       而那种不一致会让上位机永远也猜不对。 */
    b->current = bps;
    b->pending = 0u;
    b->quiet_acc_ms = 0u;
    b->applied++;
}

uint32_t gw_baud_current(const gw_baud_t *b)
{
    return (b == 0) ? 0u : b->current;
}

uint32_t gw_baud_pending(const gw_baud_t *b)
{
    return (b == 0) ? 0u : b->pending;
}
