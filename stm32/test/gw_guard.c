/**
 * @file    gw_guard.c
 * @brief   内存边界哨兵（金丝雀）检查实现 —— 测试用工具
 */
#include "gw_guard.h"

#include <stdio.h>
#include <string.h>

uint8_t *gw_guard_data(gw_guard_t *g)
{
    return (g == 0) ? 0 : g->data;
}

uint16_t gw_guard_capacity(void)
{
    return GW_GUARD_MAX_DATA;
}

void gw_guard_init(gw_guard_t *g)
{
    if (g == 0) {
        return;
    }
    memset(g->pre,  GW_GUARD_PRE_FILL,  sizeof(g->pre));
    memset(g->data, 0x00u,              sizeof(g->data));
    memset(g->post, GW_GUARD_POST_FILL, sizeof(g->post));
}

int gw_guard_check(const gw_guard_t *g)
{
    uint16_t i;

    if (g == 0) {
        return -1000;
    }

    /* 前哨：从**紧邻数据区的那一端**开始查（偏移大的那边更可能被改） */
    for (i = 0u; i < GW_GUARD_PAD; i++) {
        uint16_t idx = (uint16_t)(GW_GUARD_PAD - 1u - i);

        if (g->pre[idx] != GW_GUARD_PRE_FILL) {
            /* 返回负值表示前哨；绝对值表示距离数据区多少字节 */
            return -(int)(i + 1u);
        }
    }

    /* 后哨：从紧邻数据区的那一端开始查 */
    for (i = 0u; i < GW_GUARD_PAD; i++) {
        if (g->post[i] != GW_GUARD_POST_FILL) {
            return (int)(i + 1u);
        }
    }
    return 0;
}

void gw_guard_dump(const gw_guard_t *g, const char *label)
{
    int r;
    uint16_t i;

    if (g == 0) {
        return;
    }

    r = gw_guard_check(g);
    if (r == 0) {
        printf("      [guard] %s: 完好\n", (label != 0) ? label : "?");
        return;
    }

    if (r < 0) {
        int dist = -r;

        printf("      [guard] %s: **前哨被破坏**，距数据区起点 %d 字节\n",
               (label != 0) ? label : "?", dist);
        printf("              诊断：负向越界（下标算成了负数？）\n");
        printf("              前哨内容: ");
        for (i = 0u; i < GW_GUARD_PAD; i++) {
            printf("%02X ", g->pre[i]);
        }
        printf("\n");
    } else {
        printf("      [guard] %s: **后哨被破坏**，距数据区终点 %d 字节\n",
               (label != 0) ? label : "?", r);
        if (r <= 4) {
            printf("              诊断：正向越界写（最常见）\n");
        } else {
            printf("              诊断：写飞了（偏移 %d 字节，通常是野指针）\n", r);
        }
        printf("              后哨内容: ");
        for (i = 0u; i < GW_GUARD_PAD; i++) {
            printf("%02X ", g->post[i]);
        }
        printf("\n");
    }
}
