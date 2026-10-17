/**
 * @file    frameboundary_probe.c
 * @brief   验证"一帧被切成两段"和"噪声毛刺"在协议层的实际后果
 *
 * ============================================================================
 *  这个探针要回答什么
 *
 *  帧边界判定**全在 port/ 层**：src/modbus_rtu.c 只有
 *      int mb_slave_process(mb_slave_t *s, const uint8_t *req, uint16_t req_len);
 *  它一次拿**一整帧**，没有流式喂字节的接口，也没有任何 3.5T 逻辑。
 *
 *  所以"IDLE 过早触发把一帧切成两帧"在协议层就表现为
 *  **slave 收到的是两个不完整的字节序列**。本探针把这两个序列
 *  直接喂给 mb_slave_process，看它到底怎么处理。
 *
 *  ⚠️ 本探针**不测时序**（PC 上测不了 IDLE）。它测的是
 *     "如果 port 层递上来的是半帧，协议层会怎样" —— 这是可以确定的。
 *
 *  ⚠️ 本文件里所有输出文本**不使用引号**，只用中文标点。
 *     原因：在中文文本里打 ASCII 双引号会提前结束 C 字符串字面量，
 *     报错位置离现场很远（本项目反复踩过）。
 * ============================================================================
 */

#include <stdio.h>
#include <string.h>

#include "modbus_rtu.h"
#include "modbus_regs.h"
#include "modbus_crc.h"

static mb_regs_t  REGS;
static mb_slave_t S;

static void dump(const char *tag, const uint8_t *b, uint16_t n)
{
    uint16_t i;
    printf("  %-26s ", tag);
    for (i = 0u; i < n; i++) { printf("%02X ", b[i]); }
    printf("\n");
}

static void report(const char *what, int rc, const mb_slave_t *s)
{
    printf("  %-26s -> ", what);
    if (rc == 0) {
        printf("无响应\n");
        return;
    }
    printf("有响应  fc=0x%02X", s->tx_buf[1]);
    if ((s->tx_buf[1] & 0x80u) != 0u && s->tx_len >= 3u) {
        printf("  异常码 0x%02X", s->tx_buf[2]);
    }
    printf("\n");
}

int main(void)
{
    static const uint8_t good_pdu[] = {
        0x01, 0x03, 0x00, 0x00, 0x00, 0x04
    };
    uint8_t  full[16];
    uint16_t full_len;
    uint16_t crc;
    int      rc;

    memset(&REGS, 0, sizeof(REGS));
    REGS.holding[0] = 0x0001u;
    REGS.holding[1] = 0x86A0u;
    REGS.holding[2] = 0x0000u;
    REGS.holding[3] = 0x000Au;

    mb_slave_init(&S, 1u, &REGS);

    memcpy(full, good_pdu, sizeof(good_pdu));
    crc = mb_crc16(good_pdu, sizeof(good_pdu));
    full[sizeof(good_pdu)]      = (uint8_t)(crc & 0xFFu);
    full[sizeof(good_pdu) + 1u] = (uint8_t)(crc >> 8);
    full_len = (uint16_t)(sizeof(good_pdu) + 2u);

    printf("========== 帧边界探针 ==========\n\n");

    printf("[0] 基准：完整的一帧\n");
    dump("请求", full, full_len);
    mb_slave_init(&S, 1u, &REGS);
    rc = mb_slave_process(&S, full, full_len);
    report("完整帧", rc, &S);
    printf("       响应: ");
    {
        uint16_t i;
        for (i = 0u; i < S.tx_len; i++) { printf("%02X ", S.tx_buf[i]); }
    }
    printf("\n\n");

    printf("[1] 一帧被切成两段（模拟 IDLE 在帧中间误触发）\n");
    {
        uint16_t cut = 4u;

        dump("前半段", full, cut);
        mb_slave_init(&S, 1u, &REGS);
        rc = mb_slave_process(&S, full, cut);
        report("前半段", rc, &S);

        dump("后半段", full + cut, (uint16_t)(full_len - cut));
        mb_slave_init(&S, 1u, &REGS);
        rc = mb_slave_process(&S, full + cut, (uint16_t)(full_len - cut));
        report("后半段", rc, &S);

        printf("\n       结论：两段都拿不到完整应答，\n");
        printf("             主站收不到合法响应 -> 超时 -> 重试。\n");
    }

    printf("\n[2] 噪声毛刺：总线上多了一个假字节\n");
    {
        uint8_t  noisy[16];
        uint16_t n;

        noisy[0] = 0xA5u;
        memcpy(noisy + 1, full, full_len);
        n = (uint16_t)(full_len + 1u);
        dump("前导垃圾加完整帧", noisy, n);
        mb_slave_init(&S, 1u, &REGS);
        rc = mb_slave_process(&S, noisy, n);
        report("前导垃圾", rc, &S);

        memcpy(noisy, full, full_len);
        noisy[full_len] = 0x5Au;
        n = (uint16_t)(full_len + 1u);
        dump("完整帧加尾部垃圾", noisy, n);
        mb_slave_init(&S, 1u, &REGS);
        rc = mb_slave_process(&S, noisy, n);
        report("尾部垃圾", rc, &S);

        printf("\n       结论：多一个字节就整帧作废（长度必须精确匹配）。\n");
        printf("             更严重的在 port 层：毛刺会占掉那一帧的槽位，\n");
        printf("             并让接收进入 abort 状态，\n");
        printf("             紧接着到来的合法帧会落进死窗口被丢掉。\n");
    }

    printf("\n[3] 两帧背靠背粘在一起（中间没有静默）\n");
    {
        uint8_t  glued[32];
        uint16_t n;

        memcpy(glued, full, full_len);
        memcpy(glued + full_len, full, full_len);
        n = (uint16_t)(full_len * 2u);
        dump("两帧拼接", glued, n);
        mb_slave_init(&S, 1u, &REGS);
        rc = mb_slave_process(&S, glued, n);
        report("拼接帧", rc, &S);

        printf("\n       结论：长度不匹配 -> 被拒，两帧都得不到应答。\n");
    }

    printf("\n======================================================\n");
    printf("  以上全是协议层的实际行为（可复现）。\n");
    printf("  至于 IDLE 究竟在什么时刻触发、会不会真的切帧，\n");
    printf("  取决于 port 层与硬件时序，PC 上测不了 —— 需要上板。\n");
    printf("======================================================\n");

    return 0;
}
