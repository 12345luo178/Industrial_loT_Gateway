/**
 * @file    e2e_slave_server.c
 * @brief   交互式从站服务端 —— 让外部程序（Python）当"活的主站"
 *
 * ============================================================================
 *  与 e2e_scenario.c 的区别
 *
 *  `e2e_scenario.c` 是**固定剧本**：跑一遍，dump 字节流，交给验证器看。
 *  它能验证"规范符合性"，但**不能验证"没被写进剧本的边界"**。
 *
 *  本程序是**交互式**的：外部程序（`tools/e2e_master.py`）逐条发请求，
 *  它逐条应答。于是 Python 可以：
 *
 *    - 主动构造任意畸形帧（剧本里想不到的）
 *    - 验证"不该应答时必须不应答"（需要**等超时**才能确认，剧本做不到）
 *    - 测粘包（一次发两帧）
 *    - 测半帧后补齐
 *    - 快速连发做压力测试
 *    - 边界的数量值（1、125、126、0、65535）
 *
 *  ----------------------------------------------------------------------------
 *  ⚠️ 为什么"不该应答时必须不应答"值得单列
 *
 *  Modbus 从站的三条静默规则（地址不匹配 / 广播 / CRC 错）
 *  在固定剧本里只能验证"响应总数对不对" —— 那是个**间接**证据：
 *  多一个响应、少一个响应可能互相抵消。
 *
 *  交互式测试能直接验证：Python 发一帧该被忽略的请求，
 *  然后**等一个超时**；超时内没收到任何字节，才算通过。
 *  这才叫"验证了静默"。
 *
 *  ----------------------------------------------------------------------------
 *  协议（行文本，UTF-8）
 *
 *      Python -> C     <hex>\n              一帧完整的 Modbus RTU 请求（含 CRC）
 *      Python -> C     PING\n               连通性检查
 *      Python -> C     QUIT\n               结束
 *
 *      C -> Python     OK <hex>\n           有响应，十六进制字节
 *      C -> Python     NONE\n               无响应（被静默忽略）
 *      C -> Python     PONG\n               PING 的应答
 *      C -> Python     ERR <text>\n         输入格式错误
 *
 *  ⚠️ 每次请求处理完**立即 flush**。不 flush 的话 Python 会一直等，
 *     而 C 这边在等下一行 —— 双向死锁。
 * ============================================================================
 */

#include <stdio.h>
#include <string.h>

#include "modbus_rtu.h"
#include "modbus_regs.h"

static mb_regs_t REGS;
static mb_slave_t S;

/* 十六进制解析：返回字节数；-1 表示格式非法 */
static int parse_hex(const char *s, uint8_t *out, int cap)
{
    int n = 0;
    int hi = -1;

    while (*s != '\0' && *s != '\n' && *s != '\r') {
        int v;
        char c = *s++;

        if (c == ' ' || c == '\t') {
            continue;               /* 容忍空白，便于手工调试 */
        }
        if (c >= '0' && c <= '9')      { v = c - '0'; }
        else if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; }
        else if (c >= 'A' && c <= 'F') { v = c - 'A' + 10; }
        else { return -1; }

        if (hi < 0) {
            hi = v;
        } else {
            if (n >= cap) { return -1; }
            out[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    if (hi >= 0) { return -1; }     /* 奇数个十六进制字符 */

    return n;
}

static void print_hex(const uint8_t *b, uint16_t n)
{
    uint16_t i;
    for (i = 0u; i < n; i++) {
        printf("%02X", b[i]);
    }
}

int main(void)
{
    char     line[1024];
    uint8_t  req[MB_MAX_ADU + 16];
    int      n;
    int      r;

    memset(&REGS, 0, sizeof(REGS));
    mb_slave_init(&S, 1u, &REGS);

    /* 给寄存器表填一些可辨识的值，方便 Python 侧核对"读写生效" */
    REGS.holding[HREG_DEVICE_ID_HI] = 0x0001u;
    REGS.holding[HREG_DEVICE_ID_LO] = 0x86A0u;      /* 100000 */
    REGS.holding[HREG_REPORT_PERIOD] = 10u;
    REGS.holding[HREG_SLAVE_ADDR]    = 1u;
    REGS.input[IREG_TEMP_X10]        = 253;         /* 25.3 ℃ */
    REGS.input[IREG_HUMI_X10]        = 612u;        /* 61.2 %RH */
    REGS.input[IREG_DEV_STATUS]      = 0x0001u;     /* DHT_OK */

    while (fgets(line, (int)sizeof(line), stdin) != 0) {

        /* ---- 控制命令 ---- */
        if (strncmp(line, "QUIT", 4) == 0) {
            printf("BYE\n");
            fflush(stdout);
            break;
        }
        if (strncmp(line, "PING", 4) == 0) {
            printf("PONG\n");
            fflush(stdout);
            continue;
        }

        /* ---- 一帧请求 ---- */
        n = parse_hex(line, req, (int)sizeof(req));
        if (n < 0) {
            printf("ERR bad hex\n");
            fflush(stdout);
            continue;
        }
        if (n == 0) {
            continue;               /* 空行：忽略 */
        }

        S.tx_len = 0u;
        r = mb_slave_process(&S, req, (uint16_t)n);

        if (r == 1 && S.tx_len > 0u) {
            printf("OK ");
            print_hex(S.tx_buf, S.tx_len);
            printf("\n");
        } else {
            /* ⚠️ 无响应：可能是"地址不匹配 / 广播 / CRC 错"，
               也可能是"这一帧被消耗但没有响应"。对 Python 侧来说
               都是"没收到字节" —— 静默就是静默。 */
            printf("NONE\n");
        }
        fflush(stdout);
    }

    return 0;
}
