#include "modbus_rtu.h"
#include "modbus_crc.h"

/* 在 buf[0..len-1] 之后追加 CRC（低字节在前） */
static void append_crc(uint8_t *buf, uint16_t len)
{
    uint16_t crc = mb_crc16(buf, len);
    buf[len]        = (uint8_t)(crc & 0xFFu);
    buf[len + 1u]   = (uint8_t)(crc >> 8);
}

/* 组织异常响应：功能码最高位置 1 */
static void build_exception(mb_slave_t *s, uint8_t func, uint8_t exc)
{
    s->tx_buf[0] = s->slave_addr;
    s->tx_buf[1] = (uint8_t)(func | 0x80u);
    s->tx_buf[2] = exc;
    append_crc(s->tx_buf, 3u);
    s->tx_len = 5u;
    s->stat_exceptions++;
}

/* 功能码 03 / 04：读保持寄存器 / 读输入寄存器 */
static void handle_read(mb_slave_t *s, uint8_t func,
                        const uint8_t *req, uint16_t req_len)
{
    const uint16_t *src;
    uint16_t start, qty, i, limit;

    if (req_len != 8u) {              /* 站号+功能+起址2+数量2+CRC2 */
        build_exception(s, func, MB_EXC_ILLEGAL_VALUE);
        return;
    }

    start = (uint16_t)(((uint16_t)req[2] << 8) | req[3]);
    qty   = (uint16_t)(((uint16_t)req[4] << 8) | req[5]);

    if (qty < 1u || qty > MB_MAX_READ_QTY) {
        build_exception(s, func, MB_EXC_ILLEGAL_VALUE);
        return;
    }

    if (func == MB_FC_READ_INPUT) {
        src   = s->regs->input;
        limit = MB_INPUT_REG_COUNT;
    } else {
        src   = s->regs->holding;
        limit = MB_HOLDING_REG_COUNT;
    }

    /* 必须先判上界再取数，否则会越界读 */
    if ((uint32_t)start + (uint32_t)qty > (uint32_t)limit) {
        build_exception(s, func, MB_EXC_ILLEGAL_ADDRESS);
        return;
    }

    s->tx_buf[0] = s->slave_addr;
    s->tx_buf[1] = func;
    s->tx_buf[2] = (uint8_t)(qty * 2u);
    for (i = 0u; i < qty; i++) {
        s->tx_buf[3u + i * 2u] = (uint8_t)(src[start + i] >> 8);
        s->tx_buf[4u + i * 2u] = (uint8_t)(src[start + i] & 0xFFu);
    }
    append_crc(s->tx_buf, (uint16_t)(3u + qty * 2u));
    s->tx_len = (uint16_t)(5u + qty * 2u);
}

/* 功能码 06：写单个保持寄存器 */
static void handle_write_single(mb_slave_t *s, uint8_t func,
                                const uint8_t *req, uint16_t req_len)
{
    uint16_t reg, val;

    if (req_len != 8u) {
        build_exception(s, func, MB_EXC_ILLEGAL_VALUE);
        return;
    }

    reg = (uint16_t)(((uint16_t)req[2] << 8) | req[3]);
    val = (uint16_t)(((uint16_t)req[4] << 8) | req[5]);

    if (reg >= MB_HOLDING_REG_COUNT) {
        build_exception(s, func, MB_EXC_ILLEGAL_ADDRESS);
        return;
    }

    s->regs->holding[reg] = val;

    /* 正常响应为原样回显请求的前 6 字节 */
    s->tx_buf[0] = s->slave_addr;
    s->tx_buf[1] = func;
    s->tx_buf[2] = req[2];
    s->tx_buf[3] = req[3];
    s->tx_buf[4] = req[4];
    s->tx_buf[5] = req[5];
    append_crc(s->tx_buf, 6u);
    s->tx_len = 8u;
}

/* 功能码 16(0x10)：写多个保持寄存器 */
static void handle_write_multiple(mb_slave_t *s, uint8_t func,
                                  const uint8_t *req, uint16_t req_len)
{
    uint16_t start, qty, i;
    uint8_t  byte_count;

    if (req_len < 9u) {
        build_exception(s, func, MB_EXC_ILLEGAL_VALUE);
        return;
    }

    start      = (uint16_t)(((uint16_t)req[2] << 8) | req[3]);
    qty        = (uint16_t)(((uint16_t)req[4] << 8) | req[5]);
    byte_count = req[6];

    if (qty < 1u || qty > MB_MAX_WRITE_QTY || byte_count != (uint8_t)(qty * 2u)) {
        build_exception(s, func, MB_EXC_ILLEGAL_VALUE);
        return;
    }
    if (req_len != (uint16_t)(9u + qty * 2u)) {   /* 帧长必须精确匹配 */
        build_exception(s, func, MB_EXC_ILLEGAL_VALUE);
        return;
    }
    if ((uint32_t)start + (uint32_t)qty > (uint32_t)MB_HOLDING_REG_COUNT) {
        build_exception(s, func, MB_EXC_ILLEGAL_ADDRESS);
        return;
    }

    for (i = 0u; i < qty; i++) {
        s->regs->holding[start + i] =
            (uint16_t)(((uint16_t)req[7u + i * 2u] << 8) | req[8u + i * 2u]);
    }

    s->tx_buf[0] = s->slave_addr;
    s->tx_buf[1] = func;
    s->tx_buf[2] = req[2];   /* 起始地址回显 */
    s->tx_buf[3] = req[3];
    s->tx_buf[4] = req[4];   /* 数量回显 */
    s->tx_buf[5] = req[5];
    append_crc(s->tx_buf, 6u);
    s->tx_len = 8u;
}

void mb_slave_init(mb_slave_t *s, uint8_t addr, mb_regs_t *regs)
{
    uint16_t i;

    if (s == 0) {
        return;
    }

    s->slave_addr      = addr;
    s->regs            = regs;
    s->tx_len          = 0u;
    s->stat_rx_frames  = 0u;
    s->stat_crc_errors = 0u;
    s->stat_exceptions = 0u;
    s->stat_addr_miss  = 0u;

    for (i = 0u; i < MB_MAX_ADU; i++) {
        s->tx_buf[i] = 0u;
    }
    if (regs != 0) {
        for (i = 0u; i < MB_INPUT_REG_COUNT; i++) {
            regs->input[i] = 0u;
        }
        for (i = 0u; i < MB_HOLDING_REG_COUNT; i++) {
            regs->holding[i] = 0u;
        }
    }
}

int mb_slave_process(mb_slave_t *s, const uint8_t *req, uint16_t req_len)
{
    uint8_t  addr, func;
    uint16_t crc_calc, crc_recv;
    int      is_broadcast;

    if (s == 0 || req == 0) {
        return 0;
    }

    s->tx_len = 0u;

    /* 最短合法帧: 站号 + 功能 + CRC16 */
    if (req_len < 4u || req_len > MB_MAX_ADU) {
        return 0;
    }

    addr = req[0];
    func = req[1];

    /* 站号过滤：0 为广播（只执行不响应），其余必须匹配本机地址 */
    is_broadcast = (addr == 0u) ? 1 : 0;
    if (!is_broadcast && addr != s->slave_addr) {
        s->stat_addr_miss++;          /* 多从站总线上的正常现象，统计用于诊断 */
        return 0;
    }

    /* CRC 校验：覆盖除末尾 2 字节外的全部内容 */
    crc_recv = (uint16_t)(((uint16_t)req[req_len - 1u] << 8) | req[req_len - 2u]);
    crc_calc = mb_crc16(req, (uint16_t)(req_len - 2u));
    if (crc_calc != crc_recv) {
        s->stat_crc_errors++;
        return 0;                     /* 校验失败静默丢弃，不回复 */
    }

    s->stat_rx_frames++;

    switch (func) {
    case MB_FC_READ_HOLDING:
    case MB_FC_READ_INPUT:
        handle_read(s, func, req, req_len);
        break;

    case MB_FC_WRITE_SINGLE:
        handle_write_single(s, func, req, req_len);
        break;

    case MB_FC_WRITE_MULTIPLE:
        handle_write_multiple(s, func, req, req_len);
        break;

    default:
        build_exception(s, func, MB_EXC_ILLEGAL_FUNCTION);
        break;
    }

    if (is_broadcast) {
        s->tx_len = 0u;               /* 广播不产生响应 */
        return 0;
    }
    return (s->tx_len > 0u) ? 1 : 0;
}
