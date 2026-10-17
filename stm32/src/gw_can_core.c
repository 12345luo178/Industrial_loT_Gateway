/**
 * @file    gw_can_core.c
 * @brief   CAN 报文处理与 Bus-Off 恢复实现（纯逻辑）
 */
#include "gw_can_core.h"

#include <string.h>

/* ==================================================================
 *  报文辅助
 * ================================================================== */

int gw_can_frame_valid(const gw_can_frame_t *f)
{
    if (f == 0) {
        return -1;
    }
    if (f->dlc > GW_CAN_MAX_DLC) {
        return -2;
    }
    /* ⚠️ 远程帧（RTR）必须拒绝。
     *
     * 踩过的坑：最初这里只检查了 dlc 与 id 范围，**没检查 rtr**。
     * 后果很隐蔽：远程帧是"请求别人发数据"，它自己**没有数据段**。
     * 如果放它进通道映射，`gw_can_frame_get_u16()` 会从
     * `frame.data[]` 里取值 —— 而那个结构体的 data 是上一次
     * 收到的报文留下的（调用方复用同一个结构体时）。
     *
     * 于是通道里会出现**一个看起来合理但完全过期的值**，
     * 上报到云端后没人能看出它是假的。这比"通道没数据"危险得多 ——
     * 后者至少能被发现。
     *
     * 这个 bug 是造随机帧的 fuzz 测试抓出来的（见 test/test_fuzz.c）：
     * 随机帧里 1/2 的概率带 rtr=1，于是大量远程帧被当成数据帧处理。 */
    if (f->rtr != 0u) {
        return -4;
    }
    if (f->extended != 0u) {
        if (f->id > GW_CAN_EXT_ID_MAX) {
            return -3;
        }
    } else {
        if (f->id > GW_CAN_STD_ID_MAX) {
            return -3;
        }
    }
    return 0;
}

int gw_can_frame_get_u16(const gw_can_frame_t *f, uint8_t offset, uint16_t *out)
{
    if (f == 0 || out == 0) {
        return -1;
    }
    if ((uint16_t)offset + 2u > (uint16_t)f->dlc) {
        return -1;                  /* 数据段不够长，取不到 */
    }
    *out = (uint16_t)(((uint16_t)f->data[offset] << 8)
                      | (uint16_t)f->data[offset + 1u]);
    return 0;
}

int gw_can_frame_get_u16_le(const gw_can_frame_t *f, uint8_t offset, uint16_t *out)
{
    if (f == 0 || out == 0) {
        return -1;
    }
    if ((uint16_t)offset + 2u > (uint16_t)f->dlc) {
        return -1;
    }
    *out = (uint16_t)(((uint16_t)f->data[offset + 1u] << 8)
                      | (uint16_t)f->data[offset]);
    return 0;
}

/* ==================================================================
 *  内部：软件滤波
 *
 *  ⚠️ 与硬件滤波的关系：**两者是叠加的，不是替代的**。
 *
 *  硬件滤波（bxCAN 的筛选器组）能大幅降低 CPU 占用 ——
 *  不匹配的报文根本不会产生中断。
 *  但硬件滤波的配置项有限（F1 只有 14 个筛选器组，每个组能覆盖的
 *  模式也有限），而且现场调试时经常需要临时放开通路排查问题。
 *
 *  所以本模块**也做一次软件判断**：既能作为硬件滤波的补充（更复杂的匹配），
 *  也能在硬件滤波被设成"全通过"时独立工作。
 *  代价是一次位运算，可忽略。
 * ================================================================== */
static int sw_filter_pass(const gw_can_core_t *c, uint16_t id)
{
    if (c->filt_mask == 0u) {
        return 1;                   /* 掩码 0 = 全通过 */
    }
    /* 标准 CAN 滤波：(id & mask) == (filt_id & mask) */
    return (((id & c->filt_mask) == (c->filt_id & c->filt_mask)) ? 1 : 0);
}

/* ==================================================================
 *  内部：状态迁移
 * ================================================================== */
static void enter_state(gw_can_core_t *c, gw_can_state_t st)
{
    if (c->state == st) {
        return;
    }
    c->state    = st;
    c->state_ms = 0u;               /* 时间基准清零，由 tick 累加 */
}

/* ==================================================================
 *  对外接口
 * ================================================================== */

int gw_can_core_init(gw_can_core_t *c, uint16_t filt_id, uint16_t filt_mask,
                     int (*reinit)(void *ctx), void *reinit_ctx)
{
    if (c == 0) {
        return -1;
    }
    if (filt_id > GW_CAN_STD_ID_MAX || filt_mask > GW_CAN_STD_ID_MAX) {
        return -2;                  /* 超出标准帧 11 位 */
    }

    memset(c, 0, sizeof(*c));
    c->filt_id    = filt_id;
    c->filt_mask  = filt_mask;
    c->reinit     = reinit;
    c->reinit_ctx = reinit_ctx;

    c->state = GW_CAN_ST_NORMAL;
    c->inited = 1u;
    return 0;
}

void gw_can_core_mark_failed(gw_can_core_t *c)
{
    if (c == 0 || c->inited == 0u) {
        return;                 /* 没初始化过就没资格谈"失败" */
    }

    /* ⚠️ 这里**不能**用 next_try_ms = 0。
       0 的语义是"退避窗口已过、随时可试"，而 FAILED 状态在 tick 里看到
       next_try_ms == 0 会立刻转 RECOVERING 并调用 reinit() ——
       等于"初始化失败后下一个 tick 就马上再试一遍"，退避形同虚设
       （这正是 GW_CAN_RECOVER_DELAY_MS 那段注释里记的坑）。
       所以先退避一个短窗口，让重试按"1s → 1s → 1s → 10s"的既有节奏走。 */
    c->next_try_ms   = GW_CAN_RECOVER_DELAY_MS;
    c->recover_tries = 0u;

    /* 用 enter_state() 而不是直接赋值 —— 它会同时把 state_ms 清零。 */
    enter_state(c, GW_CAN_ST_FAILED);
}

int gw_can_core_set_map(gw_can_core_t *c, uint16_t can_id, uint8_t channel)
{
    uint8_t i;
    int     free_slot = -1;

    if (c == 0 || c->inited == 0u) {
        return -1;
    }
    if (can_id > GW_CAN_STD_ID_MAX) {
        return -2;
    }
    if (channel >= GW_CAN_CH_COUNT) {
        return -3;
    }

    /* 先找同 ID 的已有项（覆盖），同时记住第一个空位。
       两件事一次遍历完成，避免两次循环。 */
    for (i = 0u; i < GW_CAN_CH_COUNT; i++) {
        if (c->map[i].enabled != 0u && c->map[i].can_id == can_id) {
            c->map[i].channel = channel;
            return 0;               /* 覆盖已有映射 */
        }
        if (free_slot < 0 && c->map[i].enabled == 0u) {
            free_slot = (int)i;
        }
    }

    if (free_slot < 0) {
        return -4;                  /* 映射表满 */
    }

    c->map[free_slot].can_id  = can_id;
    c->map[free_slot].channel = channel;
    c->map[free_slot].enabled = 1u;
    return 0;
}

void gw_can_core_clear_map(gw_can_core_t *c)
{
    uint8_t i;

    if (c == 0) {
        return;
    }
    for (i = 0u; i < GW_CAN_CH_COUNT; i++) {
        c->map[i].enabled = 0u;
    }
}

int gw_can_core_on_frame(gw_can_core_t *c, const gw_can_frame_t *f)
{
    uint8_t  i;
    uint16_t id;

    if (c == 0 || f == 0 || c->inited == 0u) {
        return -2;
    }
    if (gw_can_frame_valid(f) != 0) {
        c->stat.rx_malformed++;
        return -2;
    }

    c->stat.rx_total++;

    /* 扩展帧不走本模块的通道映射：寄存器表定义的是标准帧 11 位 ID。
       显式区分而不是"取低 11 位凑合" —— 后者会把两个不同的扩展帧
       映射到同一个通道，而且完全看不出错。 */
    if (f->extended != 0u) {
        c->stat.rx_filtered++;
        return -1;
    }
    id = (uint16_t)f->id;

    /* ---- 诊断记录：先记下来再判滤波 ----
       为什么要记录被过滤掉的报文：现场最常见的排查场景是
       "通道没数据"，原因可能是 ID 映射写错，也可能是滤波把它挡了。
       两种情况的处理方式完全不同，而只有"看到实际收到的 ID"
       才能区分。所以诊断记录必须包含被过滤的报文。 */
    {
        uint8_t slot = c->diag_head;

        c->diag[slot].id  = id;
        c->diag[slot].dlc = (uint8_t)f->dlc;

        c->diag_head = (uint8_t)((c->diag_head + 1u) % GW_CAN_DIAG_DEPTH);
        if (c->diag_count < GW_CAN_DIAG_DEPTH) {
            c->diag_count++;
        }
    }

    if (sw_filter_pass(c, id) == 0) {
        c->stat.rx_filtered++;
        return -1;
    }

    /* ---- 查通道映射 ---- */
    for (i = 0u; i < GW_CAN_CH_COUNT; i++) {
        if (c->map[i].enabled == 0u || c->map[i].can_id != id) {
            continue;
        }

        /* 取数据段前两个字节按大端解释为 16 位值。
           取不到（DLC < 2）时**不算错误**：
           现场确实有"心跳帧"这类只有 1 字节数据的报文，
           它们的作用是"证明节点还活着"，本身没有值。
           这种帧只要收到了就该把通道标为有效。 */
        {
            uint16_t v = 0u;

            if (gw_can_frame_get_u16(f, 0u, &v) != 0) {
                v = 0u;             /* DLC 不足：值为 0，但通道仍标有效 */
            }
            c->ch_value[c->map[i].channel] = v;
            c->ch_valid |= (uint8_t)(1u << c->map[i].channel);
        }

        c->stat.rx_accepted++;
        return 1;
    }

    /* 通过滤波但没有映射：这不是错误，只是"这个 ID 我们暂时不关心"。
       单独计数是为了让运维能发现"现场有个新节点在发报文"，
       从而决定要不要给它配一个通道。 */
    c->stat.rx_unmapped++;
    return 0;
}

void gw_can_core_tick(gw_can_core_t *c, uint32_t elapsed_ms)
{
    if (c == 0 || c->inited == 0u) {
        return;
    }

    c->state_ms += elapsed_ms;
    c->ms_acc   += elapsed_ms;

    /* 退避倒计时：递减到 0 才允许尝试恢复。
       用"剩余时间"而不是"绝对时刻"，避免毫秒计数回绕时的比较问题
       （uint32 在 49 天回绕，而这里每次递减到 0 就重装，永不受回绕影响）。 */
    if (c->next_try_ms > elapsed_ms) {
        c->next_try_ms -= elapsed_ms;
    } else {
        c->next_try_ms = 0u;
    }

    switch (c->state) {
    case GW_CAN_ST_BUS_OFF:
        /* 等退避窗口结束，然后进入"尝试恢复"。
           为什么不在 BUS_OFF 里直接试：状态语义要清楚 ——
           BUS_OFF 是"已知离线、正在等待"，RECOVERING 是"正在动手恢复"。
           两者分开之后，日志里能看出"等了多久才开始动手"。 */
        if (c->next_try_ms == 0u) {
            enter_state(c, GW_CAN_ST_RECOVERING);
        }
        break;

    case GW_CAN_ST_RECOVERING:
        /* ⚠️ 这里**必须**再判一次退避倒计时。
         *
         * 踩过的坑：最初这里直接调用 reinit()，没有倒计时判断。
         * 于是进入 RECOVERING 后每个 tick 都试一次 ——
         * 10ms 节拍下每秒尝试 100 次，3 次失败瞬间进 FAILED，
         * 而"1 秒退避"的意图完全没生效。
         * 表现是"设备 Bus-Off 后疯狂重试然后放弃"，
         * 比不做恢复还糟：既打扰总线，又很快放弃。 */
        if (c->next_try_ms > 0u) {
            break;
        }

        if (c->reinit == 0) {
            /* 没有恢复回调：无法自愈，如实停在 BUS_OFF。
               不假装恢复成功 —— 那会让上层以为 CAN 还好着。 */
            c->next_try_ms = GW_CAN_RECOVER_LONG_MS;
            enter_state(c, GW_CAN_ST_BUS_OFF);
            break;
        }

        c->stat.recover_tries++;
        if (c->reinit(c->reinit_ctx) == 0) {
            c->stat.recover_ok++;
            c->recover_tries = 0u;
            c->next_try_ms   = 0u;
            enter_state(c, GW_CAN_ST_NORMAL);
            break;
        }

        /* ---- 恢复失败 ---- */
        c->stat.recover_fail++;
        if (c->recover_tries < 0xFFu) {
            c->recover_tries++;
        }

        if (c->recover_tries >= GW_CAN_RECOVER_MAX_TRIES) {
            /* 连续失败多次：拉长间隔再试。
               不进入"永久失败" —— 偶发干扰是现场常态，
               永久放弃会让一次干扰变成"必须去现场重启"。 */
            c->next_try_ms = GW_CAN_RECOVER_LONG_MS;
            enter_state(c, GW_CAN_ST_FAILED);
        } else {
            c->next_try_ms = GW_CAN_RECOVER_DELAY_MS;
            enter_state(c, GW_CAN_ST_BUS_OFF);
        }
        break;

    case GW_CAN_ST_FAILED:
        /* 长间隔重试：一直不放弃，但也不频繁打扰总线 */
        if (c->next_try_ms == 0u) {
            c->recover_tries = 0u;          /* 重置，让它再走一轮短重试 */
            enter_state(c, GW_CAN_ST_RECOVERING);
        }
        break;

    case GW_CAN_ST_ERROR_WARN:
        /* 错误警告态：还能通信，由硬件的错误计数回调决定何时转正常/离线。
           这里不做超时迁移 —— 错误计数是硬件的事实，
           软件"猜"它什么时候好转没有意义。 */
        break;

    case GW_CAN_ST_NORMAL:
    case GW_CAN_ST_UNINIT:
    default:
        break;
    }
}

void gw_can_core_notify_bus_off(gw_can_core_t *c)
{
    if (c == 0 || c->inited == 0u) {
        return;
    }
    /* 已经在 Bus-Off / 恢复流程里就不要重复计数：
       HAL 的错误查询是轮询式的，同一次 Bus-Off 会被报告很多次，
       不判断的话统计会瞬间涨到几百，完全失真。 */
    if (c->state == GW_CAN_ST_BUS_OFF || c->state == GW_CAN_ST_RECOVERING
        || c->state == GW_CAN_ST_FAILED) {
        return;
    }

    c->stat.bus_off_count++;
    /* 装填退避窗口：让对端先稳定，再动手恢复 */
    c->next_try_ms   = GW_CAN_RECOVER_DELAY_MS;
    c->recover_tries = 0u;
    enter_state(c, GW_CAN_ST_BUS_OFF);
}

void gw_can_core_notify_normal(gw_can_core_t *c)
{
    if (c == 0 || c->inited == 0u) {
        return;
    }
    c->recover_tries = 0u;
    enter_state(c, GW_CAN_ST_NORMAL);
}

void gw_can_core_notify_error_warn(gw_can_core_t *c)
{
    if (c == 0 || c->inited == 0u) {
        return;
    }
    /* 只在正常态时转警告态：已经在恢复流程里的话，
       "错误警告"是恢复过程中的正常现象，不该覆盖状态机。 */
    if (c->state == GW_CAN_ST_NORMAL) {
        enter_state(c, GW_CAN_ST_ERROR_WARN);
    }
}

void gw_can_core_sync_regs(const gw_can_core_t *c, uint16_t *iregs,
                           uint16_t base, uint16_t ireg_count)
{
    uint8_t i;

    if (c == 0 || iregs == 0 || c->inited == 0u) {
        return;
    }

    for (i = 0u; i < GW_CAN_CH_COUNT; i++) {
        uint16_t off = (uint16_t)(base + i);

        /* 边界检查放在循环里：base + GW_CAN_CH_COUNT 可能超出寄存器表，
           整体拒绝会让"前面几个通道能写"的情况也不写，
           分批处理时反而更难用。逐个判断更合理。 */
        if (off >= ireg_count) {
            break;
        }
        if ((c->ch_valid & (uint8_t)(1u << i)) == 0u) {
            /* 无效通道：**保持原值不动**。
               清零会让上位机无法区分"通道没数据"与"数据真的是 0"。 */
            continue;
        }
        iregs[off] = c->ch_value[i];
    }
}

int gw_can_core_take_diag(gw_can_core_t *c, gw_can_diag_t *out)
{
    uint8_t idx;

    if (c == 0 || out == 0) {
        return 0;
    }
    if (c->diag_count == 0u) {
        return 0;
    }

    /* 从最旧的一条开始取。
       head 指向"下一个写入位置"，所以最旧的在 head - count 处。 */
    idx = (uint8_t)((c->diag_head + GW_CAN_DIAG_DEPTH - c->diag_count)
                    % GW_CAN_DIAG_DEPTH);
    *out = c->diag[idx];
    c->diag_count--;
    return 1;
}

gw_can_state_t gw_can_core_state(const gw_can_core_t *c)
{
    return (c == 0) ? GW_CAN_ST_UNINIT : c->state;
}

uint8_t gw_can_core_valid_mask(const gw_can_core_t *c)
{
    return (c == 0) ? 0u : c->ch_valid;
}

const gw_can_stat_t *gw_can_core_stats(const gw_can_core_t *c)
{
    return (c == 0) ? 0 : &c->stat;
}

const char *gw_can_state_str(gw_can_state_t st)
{
    switch (st) {
    case GW_CAN_ST_UNINIT:     return "UNINIT";
    case GW_CAN_ST_NORMAL:     return "NORMAL";
    case GW_CAN_ST_ERROR_WARN: return "ERROR_WARN";
    case GW_CAN_ST_BUS_OFF:    return "BUS_OFF";
    case GW_CAN_ST_RECOVERING: return "RECOVERING";
    case GW_CAN_ST_FAILED:     return "FAILED";
    default:                   return "UNKNOWN";
    }
}
