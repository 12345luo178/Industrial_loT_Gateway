/**
 * @file    modbus_master.c
 * @brief   Modbus RTU 主站协议引擎实现
 *
 * 本文件不依赖任何 HAL / 寄存器 / 全局时钟 —— 所有外部交互通过回调完成，
 * 时间由调用方通过 mb_master_poll(m, elapsed_ms) 推进。
 * 因此可以在 PC 上把"3 秒超时 + 3 次重试 + 离线退避"在微秒内完整跑完，
 * 而且结果完全确定（不依赖真实时间）。
 */
#include "modbus_master.h"
#include "modbus_crc.h"
#include "modbus_rtu.h"

#include <string.h>

/* ------------------------------------------------------------------
 * 内部工具
 * ------------------------------------------------------------------ */

/* 追加 CRC（低字节在前）—— Modbus 规定，容易记反 */
static void append_crc(uint8_t *buf, uint16_t len)
{
    uint16_t crc = mb_crc16(buf, len);
    buf[len]      = (uint8_t)(crc & 0xFFu);
    buf[len + 1u] = (uint8_t)(crc >> 8);
}

/* 校验帧尾 CRC */
static int crc_ok(const uint8_t *buf, uint16_t len)
{
    uint16_t crc_recv;
    uint16_t crc_calc;

    if (len < 4u) {
        return 0;
    }
    /* 收到的 CRC：低字节在前 */
    crc_recv = (uint16_t)(((uint16_t)buf[len - 1u] << 8) | buf[len - 2u]);
    crc_calc = mb_crc16(buf, (uint16_t)(len - 2u));
    return (crc_recv == crc_calc) ? 1 : 0;
}

/* ==================================================================
 *  从机槽位映射
 *
 *  从机状态按"已登记"紧凑存放（最多 MB_M_MAX_SLAVES 个），
 *  而不是按地址 1..247 开大数组 —— 后者一个数组就要 6.2 KB。
 *
 *  不变量：**轮询表里出现的从机地址，一定能查到槽位**。
 *  这一点由 mb_master_add_item() 在登记轮询项时顺带注册来保证，
 *  所以运行期（advance / note_ok / note_fail）的查表不会失败。
 * ================================================================== */

/** 查地址对应的槽位下标；未登记返回 -1 */
static int addr_to_slot(const mb_master_t *m, uint8_t addr)
{
    uint8_t i;

    if (addr == 0u) {
        return -1;                      /* 0 是广播地址，不对应任何从机状态 */
    }
    for (i = 0u; i < m->slave_count; i++) {
        if (m->addr[i] == addr) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * 登记一个从机地址（已存在则直接返回其槽位）
 * @return 槽位下标；表满或地址非法返回 -1
 */
static int register_slave(mb_master_t *m, uint8_t addr)
{
    int slot = addr_to_slot(m, addr);

    if (slot >= 0) {
        return slot;
    }
    if (addr == 0u || addr > 247u) {
        return -1;
    }
    if (m->slave_count >= (uint8_t)MB_M_MAX_SLAVES) {
        return -1;                      /* 表满：由调用方转成明确的错误码 */
    }

    slot = (int)m->slave_count;
    m->addr[slot] = addr;
    memset(&m->stat[slot], 0, sizeof(m->stat[slot]));

    /* 初始视为"未探测但在线" = 第一次轮询就去试。
       若置为离线，首轮会被退避逻辑跳过，表现为"上电后要等好几秒才开始采集"。
       探测失败后自然转入离线退避逻辑。 */
    m->stat[slot].online = 1u;

    m->slave_count++;
    return slot;
}

static void slave_stat_note_fail(mb_master_t *m, uint8_t slave, mb_master_err_t err)
{
    int slot = addr_to_slot(m, slave);
    mb_m_slave_stat_t *s;

    if (slot < 0) {
        return;
    }
    s = &m->stat[slot];

    s->cnt_fail++;
    s->last_err = (uint8_t)(-err);      /* 存正值便于上报 */

    if (err == MB_M_ERR_TIMEOUT) {
        s->cnt_timeout++;
    } else if (err == MB_M_ERR_EXCEPTION) {
        s->cnt_exception++;
    }

    if (s->fail_streak < 0xFFu) {
        s->fail_streak++;
    }

    /* 达到阈值：判定离线，进入退避 —— 这是"故障隔离"的关键 */
    if (s->online != 0u && s->fail_streak >= MB_M_FAIL_TO_OFFLINE) {
        s->online     = 0u;
        s->skip_count = MB_M_OFFLINE_SKIP;
        m->stats.cnt_offline++;
    }
}

static void slave_stat_note_ok(mb_master_t *m, uint8_t slave, uint16_t rtt_ms)
{
    int slot = addr_to_slot(m, slave);
    mb_m_slave_stat_t *s;

    if (slot < 0) {
        return;
    }
    s = &m->stat[slot];

    s->cnt_ok++;
    if (s->online == 0u) {
        m->stats.cnt_recover++;         /* 记录一次恢复，便于统计恢复耗时 */
        s->online = 1u;
        s->skip_count = 0u;
    }
    s->fail_streak = 0u;
    s->last_err    = 0u;
    s->last_rtt_ms = rtt_ms;
    if (rtt_ms > s->max_rtt_ms) {
        s->max_rtt_ms = rtt_ms;
    }
}

/* 构造并发送当前请求。返回 MB_M_OK 表示已发出（进入等待响应状态）。 */
static mb_master_err_t send_current_request(mb_master_t *m)
{
    const mb_m_poll_item_t *it = &m->items[m->cur_index];
    uint8_t *b = m->req_buf;
    int      r;

    b[0] = it->slave;
    b[1] = it->func;
    b[2] = (uint8_t)(it->start >> 8);
    b[3] = (uint8_t)(it->start & 0xFFu);
    b[4] = (uint8_t)(it->qty >> 8);
    b[5] = (uint8_t)(it->qty & 0xFFu);
    append_crc(b, 6u);

    m->stats.cnt_requests++;

    r = m->send(m->send_ctx, b, 8u);
    if (r != 0) {
        /* 发送失败按一次失败处理（通常是总线被占用或 UART 故障） */
        return MB_M_ERR_TIMEOUT;
    }

    m->awaiting   = 1u;
    m->elapsed_ms = 0u;
    return MB_M_OK;
}

/* 推进到下一个轮询项。
 *
 * 设计要点（这一版是修过的，前一版有逻辑缺陷）：
 *
 *   1) 用"已检查项数"控制扫描次数，最多扫描 item_count 次（正好一圈）。
 *      不能用 `scanned < n` 配上"先自增再比较"的写法 ——
 *      当 n=1 时会先自增、再比较，逻辑绕且容易出错。
 *
 *   2) 离线从机退避到期的那一次要**优先发送**：
 *      如果只清 skip_count 就继续找"下一个可用项"，那么退避到期的从机
 *      会被后面在线的从机插队，等到下一轮才轮到它 ——
 *      结果是"恢复检测"被无限推迟。这里置 force_retry 标记，
 *      让本次请求直接发给它。
 */
static void advance(mb_master_t *m)
{
    uint16_t checked = 0u;
    uint16_t n = m->item_count;

    m->force_retry = 0u;

    if (n == 0u) {
        m->awaiting = 0u;
        return;
    }

    while (checked < n) {
        int slot;

        m->cur_index = (uint8_t)((m->cur_index + 1u) % n);
        if (m->cur_index == 0u) {
            m->stats.cnt_poll_rounds++;
        }

        slot = addr_to_slot(m, m->items[m->cur_index].slave);
        if (slot < 0) {
            /* 理论上不会发生（add_item 时已登记）。
               真发生了就当作"不可用"跳过，绝不解引用非法下标。 */
            checked++;
            continue;
        }

        /* 离线且仍在退避期：跳过 */
        if (m->stat[slot].online == 0u && m->stat[slot].skip_count > 0u) {
            m->stat[slot].skip_count--;
            checked++;
            continue;
        }

        /* 离线但退避已到期：本轮优先重试它 */
        if (m->stat[slot].online == 0u) {
            m->force_retry = 1u;
        }
        break;
    }

    m->retry_left = m->max_retry;
    m->awaiting   = 0u;
    m->elapsed_ms = 0u;
}

/* 一次请求彻底失败后的收尾 */
static void finish_fail(mb_master_t *m, mb_master_err_t err)
{
    uint8_t slave = m->items[m->cur_index].slave;

    slave_stat_note_fail(m, slave, err);
    advance(m);
}

/* ==================================================================
 *  对外接口
 * ================================================================== */

mb_master_err_t mb_master_init(mb_master_t *m,
                               int (*send)(void *ctx, const uint8_t *buf, uint16_t len),
                               void *send_ctx,
                               int (*store)(void *ctx, uint8_t area, uint16_t base,
                                            const uint16_t *values, uint16_t qty),
                               void *store_ctx)
{
    if (m == 0 || send == 0 || store == 0) {
        return MB_M_ERR_PARAM;
    }

    memset(m, 0, sizeof(*m));

    m->send      = send;
    m->send_ctx  = send_ctx;
    m->store     = store;
    m->store_ctx = store_ctx;

    m->timeout_ms = 300u;       /* 默认超时：现场仪表通常 <100ms 响应 */
    m->max_retry  = 1u;         /* 默认重试 1 次 */

    /* 轮询节拍默认关闭（0 = 不限制），保持与旧行为一致。
       ⚠️ idle_ms 初始化为**饱和值**，表示"总线已经空闲很久了" ——
          于是首次请求立即发出，不需要先干等一个节拍。
          （若初始化为 0，设了节拍之后上电要等满一个间隔才开始轮询。） */
    m->poll_interval_ms = 0u;
    m->idle_ms          = 0xFFFFu;

    /* 关键：retry_left 必须在这里就初始化。
     *
     * 踩过的坑：retry_left 原先只在 advance() 里赋值，而**首次发送不经过
     * advance()** —— 于是第一次请求的 retry_left 是 0（memset 的结果），
     * 超时后直接判定"重试次数用尽"而失败。
     * 现象是"从机上电后第一次轮询永远不重试，但从第二次起正常"，
     * 很容易被误判成"从机启动慢"，实际上主站逻辑的问题。
     * 现在初始化、清空轮询表、修改时序三处都会同步维护它。 */
    m->retry_left = m->max_retry;

    /* 从机表初始为空；轮询项由 mb_master_add_item() 添加时顺带登记从机。
       不给"所有 247 个地址"预置状态，见 mb_master_t 里的说明。 */
    m->slave_count = 0u;
    memset(m->addr, 0, sizeof(m->addr));
    memset(m->stat, 0, sizeof(m->stat));

    return MB_M_OK;
}

mb_master_err_t mb_master_add_item(mb_master_t *m, const mb_m_poll_item_t *item)
{
    if (m == 0 || item == 0) {
        return MB_M_ERR_PARAM;
    }
    if (m->item_count >= MB_M_MAX_POLL_ITEMS) {
        return MB_M_ERR_PARAM;
    }
    if (item->slave < MB_ADDR_MIN || item->slave > MB_ADDR_MAX) {
        return MB_M_ERR_PARAM;
    }
    if (item->func != MB_FC_READ_HOLDING && item->func != MB_FC_READ_INPUT) {
        return MB_M_ERR_PARAM;      /* 主站本实现只支持读；写留给后续扩展 */
    }
    if (item->qty < 1u || item->qty > MB_MAX_READ_QTY) {
        return MB_M_ERR_PARAM;
    }
    if (item->map_area > 1u) {
        return MB_M_ERR_PARAM;
    }

    m->items[m->item_count] = *item;

    /* 登记从机。放在这里而不是 add_item 开头，是为了"先校验全部通过、
       再产生副作用" —— 校验失败时不该留下半个登记记录。
       表满时返回明确的错误码，而不是静默截断（静默截断会让用户以为
       第 17 台从机在轮询，实际上它的状态永远不会被更新）。 */
    if (register_slave(m, item->slave) < 0) {
        return MB_M_ERR_PARAM;          /* 从机数超过 MB_M_MAX_SLAVES */
    }

    m->item_count++;
    return MB_M_OK;
}

void mb_master_clear_items(mb_master_t *m)
{
    if (m == 0) {
        return;
    }
    m->item_count  = 0u;
    m->cur_index   = 0u;
    m->awaiting    = 0u;
    m->force_retry = 0u;
    m->elapsed_ms  = 0u;
    m->retry_left  = m->max_retry;      /* 与 init 保持一致，见 init 中的说明 */
    m->idle_ms     = 0xFFFFu;           /* 清表后视为"空闲很久"，下一个请求立即发 */

    /* 从机表一并清空：轮询表都没了，从机状态也就没有意义了。
       不清会让"重新配置轮询表"后残留上一条配置的在线位图。 */
    m->slave_count = 0u;
    memset(m->addr, 0, sizeof(m->addr));
    memset(m->stat, 0, sizeof(m->stat));
}

mb_master_err_t mb_master_set_timing(mb_master_t *m, uint16_t timeout_ms, uint8_t max_retry)
{
    if (m == 0) {
        return MB_M_ERR_PARAM;
    }
    if (timeout_ms < 10u || timeout_ms > 5000u) {
        return MB_M_ERR_PARAM;
    }
    if (max_retry > 5u) {
        return MB_M_ERR_PARAM;
    }
    m->timeout_ms = timeout_ms;
    m->max_retry  = max_retry;
    /* 同步剩余重试次数，否则改了配置却要等下一轮才生效 */
    m->retry_left = max_retry;
    return MB_M_OK;
}

mb_master_err_t mb_master_set_interval(mb_master_t *m, uint16_t interval_ms)
{
    if (m == 0) {
        return MB_M_ERR_PARAM;
    }
    if (interval_ms > MB_M_MAX_POLL_INTERVAL_MS) {
        return MB_M_ERR_PARAM;
    }
    m->poll_interval_ms = interval_ms;

    /* ⚠️ 刻意**不重置** idle_ms：
       把节拍调小（或关掉）时，如果当前空闲时间已经超过新间隔，
       下一个 poll 应当立刻发出去，而不是从头再等一遍。
       idle_ms 只在"成功发出请求"时清零。 */
    return MB_M_OK;
}

void mb_master_poll(mb_master_t *m, uint16_t elapsed_ms)
{
    if (m == 0 || m->item_count == 0u) {
        return;
    }

    if (m->awaiting == 0u) {
        mb_master_err_t e;

        /* ==================================================================
         *  轮询节拍：两次请求之间的最小空闲
         *
         *  ⚠️ 为什么要放在**发送之前**：一次都没有发出去的情况下才谈得上
         *     "间隔"。等待响应期间 elapsed 走的是超时账（m->elapsed_ms），
         *     与这里的空闲账（idle_ms）是两本账，别混。
         *
         *  ⚠️ 为什么 idle_ms 只在这里累加：
         *     它表示"事务结束之后过了多久"。所以 awaiting 期间不累加 ——
         *     否则"从机响应慢"会被算成"已经隔够了"，节拍形同虚设。
         * ================================================================== */
        /* ⚠️ 顺序：**先累加、再判断**。
           反过来写（判断通过才累加、然后直接 return）会让"刚好到点"的那一轮
           白等一次 —— 单元测试当场抓到了这个：90ms 时累加到 100ms 却又 return，
           请求要等到第 11 轮才发出去，实际节拍变成 110ms。 */
        {
            uint32_t acc = (uint32_t)m->idle_ms + (uint32_t)elapsed_ms;

            m->idle_ms = (acc > 0xFFFFu) ? 0xFFFFu : (uint16_t)acc;
        }
        if (m->idle_ms < m->poll_interval_ms) {
            return;                             /* 还没到点，这一轮什么都不做 */
        }

        e = send_current_request(m);
        if (e != MB_M_OK) {
            /* 发送就失败了：按失败处理，必要时重试 */
            if (m->retry_left > 0u) {
                m->retry_left--;
                m->stats.cnt_retries++;
                return;
            }
            finish_fail(m, e);
            return;
        }

        /* 请求已发出：空闲账清零，下一次要重新等满节拍 */
        m->idle_ms = 0u;
        return;
    }

    /* 等待响应中：累加时间并判超时 */
    if (elapsed_ms != 0u) {
        uint32_t acc = (uint32_t)m->elapsed_ms + (uint32_t)elapsed_ms;
        m->elapsed_ms = (acc > 0xFFFFu) ? 0xFFFFu : (uint16_t)acc;
    }

    if (m->elapsed_ms >= m->timeout_ms) {
        if (m->retry_left > 0u) {
            m->retry_left--;
            m->stats.cnt_retries++;
            (void)send_current_request(m);      /* 重发（失败则下轮再处理） */
            if (m->awaiting == 0u) {
                /* 重发也失败：直接按超时收尾，避免卡死 */
                finish_fail(m, MB_M_ERR_TIMEOUT);
            }
            return;
        }
        finish_fail(m, MB_M_ERR_TIMEOUT);
    }
}

mb_master_err_t mb_master_on_response(mb_master_t *m, const uint8_t *buf, uint16_t len)
{
    mb_master_err_t        err;
    const mb_m_poll_item_t *it;
    uint8_t                slave;
    uint8_t                func;

    if (m == 0 || buf == 0) {
        return MB_M_ERR_PARAM;
    }
    /* 长度检查必须放在访问 buf[0] 之前，否则过短帧会先被当成"站号不符"，
       错误码误导排查方向（实测踩到：3 字节帧返回了 ERR_ADDR）。 */
    if (len < 4u) {
        return MB_M_ERR_LEN;
    }
    if (m->item_count == 0u || m->awaiting == 0u) {
        return MB_M_ERR_BUSY;           /* 没在等响应，或表为空 */
    }

    it    = &m->items[m->cur_index];
    slave = buf[0];
    func  = buf[1];

    /* ---- 站号必须与请求一致 ----
     *
     * ⚠️⚠️ 站号不符时**直接返回，绝不能记从机失败、也不能推进轮询项**。
     *
     * 这一条是 2026-10-10 上板实测抓出来的（主从共存时的关键缺陷）：
     *
     *   主站在等 2 号从机的响应。此时上位机发来一帧**给本机从站(1 号)的读请求**，
     *   应用层的帧归属派发会把它先交给本函数（判断"是不是主站等的那个响应"）。
     *   原来这里 `goto fail`，于是：
     *       slave_stat_note_fail(2 号)   <- 把"总线上的无关报文"记成"2 号失败"
     *       advance()                    <- 还跳过一个轮询项
     *   连续 3 次（MB_M_FAIL_TO_OFFLINE）就把一台**完全正常的**从机判成离线。
     *
     *   表现（实测）：主从共存时"在线位图永远是 0、错误码固定 4"，而
     *   保持寄存器 8~11 的数据明明是对的 —— 因为**上位机每读一次板子，
     *   就顺手给下游从机记一次失败**。多主站总线上的其它报文同理。
     *
     *   正确语义：站号不符的帧**不是我们的响应**，主站应当继续等自己的响应
     *   （直到超时），这帧由调用方继续交给从站引擎处理。 */
    if (slave != it->slave) {
        return MB_M_ERR_ADDR;
    }
    /* ---- CRC ---- */
    if (crc_ok(buf, len) == 0) {
        err = MB_M_ERR_CRC;
        goto fail;
    }

    /* ---- 异常响应：功能码最高位置 1 ---- */
    if (func == (uint8_t)(it->func | 0x80u)) {
        if (len != 5u) {
            err = MB_M_ERR_LEN;
            goto fail;
        }
        err = MB_M_ERR_EXCEPTION;
        goto fail;
    }

    /* ---- 功能码必须回显 ---- */
    if (func != it->func) {
        err = MB_M_ERR_FUNC;
        goto fail;
    }

    /* ---- 读响应：站号+功能+字节数+数据+CRC2 ---- */
    {
        uint8_t  byte_count;
        uint16_t expect_len;
        uint16_t i;
        uint16_t values[MB_MAX_READ_QTY];

        if (len < 5u) {
            err = MB_M_ERR_LEN;
            goto fail;
        }
        byte_count = buf[2];
        if (byte_count != (uint8_t)(it->qty * 2u) || (byte_count & 1u) != 0u) {
            err = MB_M_ERR_LEN;
            goto fail;
        }
        expect_len = (uint16_t)(3u + byte_count + 2u);
        if (len != expect_len) {
            err = MB_M_ERR_LEN;
            goto fail;
        }

        /* 大端：高字节在前 */
        for (i = 0u; i < it->qty; i++) {
            values[i] = (uint16_t)(((uint16_t)buf[3u + i * 2u] << 8)
                                   | buf[4u + i * 2u]);
        }

        /* 交给应用层写入寄存器表；回调返回非 0 视为本次轮询失败 */
        if (m->store(m->store_ctx, it->map_area, it->map_base,
                     values, it->qty) != 0) {
            err = MB_M_ERR_PARAM;
            goto fail;
        }
    }

    /* ---- 成功 ---- */
    m->stats.cnt_responses++;
    slave_stat_note_ok(m, it->slave, m->elapsed_ms);
    advance(m);
    return MB_M_OK;

fail:
    if (err == MB_M_ERR_CRC) {
        m->stats.cnt_responses++;       /* 收到帧但非法：仍计为一次接收，便于诊断 */
    }
    {
        mb_master_err_t e = err;
        uint8_t s = m->items[m->cur_index].slave;
        slave_stat_note_fail(m, s, e);
        advance(m);
        return e;
    }
}

/* ==================================================================
 *  查询接口
 * ================================================================== */

int mb_master_abort_pending(mb_master_t *m)
{
    if (m == 0) {
        return -1;
    }
    if (m->awaiting == 0u) {
        return -1;                  /* 本来就没在等，无需处理 */
    }

    /* 只清"正在等"这个状态，**不动** cur_index 与 retry_left：
       下一轮 tick 会重新发同一个请求，重试预算不受影响。
       也不动 fail_streak —— 抢占是我们自己让出总线，
       与从机是否健康无关。 */
    m->awaiting   = 0u;
    m->elapsed_ms = 0u;
    return 0;
}

uint8_t mb_master_slave_count(const mb_master_t *m)
{
    return (m == 0) ? 0u : m->slave_count;
}

int mb_master_slave_at(const mb_master_t *m, uint8_t index, uint8_t *addr)
{
    if (m == 0 || addr == 0) {
        return -1;
    }
    if (index >= m->slave_count) {
        return -1;
    }
    *addr = m->addr[index];
    return 0;
}

const mb_m_slave_stat_t *mb_master_slave_stat(const mb_master_t *m, uint8_t slave)
{
    int slot;

    if (m == 0) {
        return 0;
    }
    slot = addr_to_slot(m, slave);
    if (slot < 0) {
        return 0;               /* 该地址未登记：如实返回"无数据"而不是全零结构 */
    }
    return &m->stat[slot];
}

const mb_m_stats_t *mb_master_stats(const mb_master_t *m)
{
    return (m == 0) ? 0 : &m->stats;
}

uint16_t mb_master_online_bitmap(const mb_master_t *m)
{
    uint16_t bmp = 0u;
    uint16_t i;

    if (m == 0) {
        return 0u;
    }

    /* 只统计"在轮询表里 且 在线"的从机。
     *
     * 为什么不直接遍历从机状态表？
     *   表里只有"已登记"的从机，但登记不代表当前在轮询表里
     *   （clear_items 会一并清空，所以当前实现下两者一致；
     *    但以轮询表为准能让语义不依赖这个巧合）。
     *   位图的语义是"这台网关正在成功采集哪些从机"。 */
    for (i = 0u; i < m->item_count; i++) {
        uint8_t sl = m->items[i].slave;
        int     slot;

        if (sl < 1u || sl > MB_M_MAX_SLAVES) {
            continue;                   /* 位图容量为 MB_M_MAX_SLAVES 台 */
        }
        slot = addr_to_slot(m, sl);
        if (slot < 0 || m->stat[slot].online == 0u) {
            continue;
        }
        /* 去重不是必须的：同一从机多个轮询项，重复置位无副作用 */
        bmp |= (uint16_t)(1u << (sl - 1u));
    }
    return bmp;
}

int mb_master_is_idle(const mb_master_t *m)
{
    if (m == 0) {
        return 0;
    }
    return (m->awaiting == 0u) ? 1 : 0;
}

uint16_t mb_master_current_index(const mb_master_t *m)
{
    return (m == 0) ? 0u : (uint16_t)m->cur_index;
}

const char *mb_master_strerror(mb_master_err_t err)
{
    switch (err) {
    case MB_M_OK:            return "OK";
    case MB_M_ERR_PARAM:     return "invalid parameter";
    case MB_M_ERR_TIMEOUT:   return "response timeout";
    case MB_M_ERR_CRC:       return "response CRC error";
    case MB_M_ERR_ADDR:      return "slave address mismatch";
    case MB_M_ERR_FUNC:      return "function code mismatch";
    case MB_M_ERR_LEN:       return "illegal response length";
    case MB_M_ERR_EXCEPTION: return "slave returned exception";
    case MB_M_ERR_BUSY:      return "no pending request";
    default:                 return "unknown error";
    }
}
