/**
 * @file    gw_fault.c
 * @brief   故障捕获与升级策略实现（纯逻辑，可在 PC 上单元测试）
 */
#include "gw_fault.h"

#include <string.h>

/* ==================================================================
 *  内部：判断记录是否有效
 *
 *  ⚠️ 这是整个模块最关键的一个判断。
 *
 *  `.noinit` 段在**冷启动时内容是随机的**（上电后 RAM 里是随机位模式）。
 *  如果不校验就当成"上次故障记录"，会得到：
 *      - 一个随机的 PC（看起来像崩溃过）
 *      - 一个随机的 consecutive 计数（可能直接 >= 3）
 *  结果：**设备一上电就进安全模式**，而且怎么看都"有依据"。
 *
 *  这个 bug 的可怕之处在于它只在冷启动时出现，
 *  而热复位（调试验证时最常用的方式）时 RAM 内容还在，一切正常。
 *  所以测试里必须显式覆盖"随机内容"这条路。
 * ================================================================== */
static int rec_is_valid(const gw_fault_rec_t *rec)
{
    return (rec != 0 && rec->magic == GW_FAULT_MAGIC) ? 1 : 0;
}

/* 把整块记录重置成"刚上电"的状态。
   用逐字段赋值而不是 memset：明确表达"哪些字段被复位了"，
   而且不依赖结构体的填充布局（将来加字段时容易看出要补哪一行）。 */
static void rec_reset(gw_fault_rec_t *rec)
{
    uint8_t i;

    rec->magic     = GW_FAULT_MAGIC;
    rec->boot_count = 1u;
    rec->last_kind = (uint8_t)GW_FAULT_NONE;
    rec->last_boot = (uint8_t)GW_BOOT_POWER_ON;
    rec->consec    = 0u;
    rec->last_pc   = 0u;
    rec->last_lr   = 0u;
    rec->last_psr  = 0u;
    rec->last_cfsr = 0u;
    rec->last_hfsr = 0u;
    rec->hist_count = 0u;

    for (i = 0u; i < GW_FAULT_HISTORY; i++) {
        rec->hist[i].kind = (uint8_t)GW_FAULT_NONE;
        rec->hist[i].pad[0] = 0u;
        rec->hist[i].pad[1] = 0u;
        rec->hist[i].pad[2] = 0u;
        rec->hist[i].pc = 0u;
    }
}

/* 往历史里追加一条（满了就把最旧的挤掉） */
static void hist_push(gw_fault_rec_t *rec, uint8_t kind, uint32_t pc)
{
    uint8_t i;

    if (rec->hist_count < GW_FAULT_HISTORY) {
        rec->hist[rec->hist_count].kind = kind;
        rec->hist[rec->hist_count].pc   = pc;
        rec->hist_count++;
        return;
    }

    /* 满了：整体左移一格，新的放最后。
       为什么用"左移"而不是环形索引：
       历史条数少（4 条），左移的代价可忽略，
       换来的是"hist[0] 永远是最旧的"这个简单语义 ——
       读日志时不需要再算环形偏移，少一处出错机会。 */
    for (i = 1u; i < GW_FAULT_HISTORY; i++) {
        rec->hist[i - 1u] = rec->hist[i];
    }
    rec->hist[GW_FAULT_HISTORY - 1u].kind = kind;
    rec->hist[GW_FAULT_HISTORY - 1u].pc   = pc;
}

/* ==================================================================
 *  对外接口
 * ================================================================== */

gw_boot_cause_t gw_fault_boot_init(gw_fault_rec_t *rec)
{
    gw_boot_cause_t cause;

    if (rec == 0) {
        return GW_BOOT_UNKNOWN;
    }

    /* ---- 1) 有效性校验 ----
       这里是"随机 RAM 被当成崩溃记录"的唯一防线，见 rec_is_valid 的说明。 */
    if (rec_is_valid(rec) == 0) {
        rec_reset(rec);
        return GW_BOOT_POWER_ON;
    }

    /* ---- 2) 累计启动次数 ----
       溢出回绕时归 1：这个计数只用于诊断（"这台设备启过多少次"），
       真回绕了说明已经运行很久，归零比继续涨更可读。 */
    rec->boot_count++;
    if (rec->boot_count == 0u) {
        rec->boot_count = 1u;
    }

    /* ---- 3) 判断启动原因 ---- */
    switch ((gw_fault_kind_t)rec->last_kind) {
    case GW_FAULT_NONE:
        /* 上次是干净启动：本次是软复位或复位引脚 */
        cause = GW_BOOT_SOFTWARE;
        break;
    default:
        /* 上次留下了故障记录：本次启动是故障处理的结果。
           具体是 IWDG 还是 HardFault，由移植层在记录故障时
           写入 last_boot 区分（见 gw_fault_record）。 */
        cause = (gw_boot_cause_t)rec->last_boot;
        if ((uint32_t)cause > (uint32_t)GW_BOOT_UNKNOWN) {
            cause = GW_BOOT_FAULT;      /* 值非法（RAM 损坏）：保守按故障处理 */
        }
        break;
    }

    /* ---- 4) 清掉"本次已处理过"的标记 ----
       为什么在这里就清：不清的话，下一次正常软复位也会被当成故障启动，
       从而不断累加 consecutive，最终无故进安全模式。
       记录已经读出来了（cause），标记的使命完成。 */
    rec->last_kind = (uint8_t)GW_FAULT_NONE;
    rec->last_boot = (uint8_t)GW_BOOT_POWER_ON;

    return cause;
}

void gw_fault_record(gw_fault_rec_t *rec, const gw_fault_ctx_t *ctx)
{
    uint8_t kind;

    if (rec == 0 || ctx == 0) {
        return;
    }

    /* ⚠️ 本函数要能从故障处理程序里调用，所以：
     *    - 不调用 memcpy/memset 之外的库函数（它们通常不需要锁）
     *    - 不调用日志（日志可能涉及锁或缓冲，在故障上下文里不安全）
     *    - 不做任何可能再次触发故障的操作（例如访问 .noinit 之外的 RAM）
     */

    kind = ctx->kind;
    if (kind == (uint8_t)GW_FAULT_NONE) {
        kind = (uint8_t)GW_FAULT_UNKNOWN;
    }

    /* ---- 同签名判定 ----
       签名 = 故障类型 + PC。
       只比 PC 不比 LR：LR 在优化后可能指向调用者或被内联掉，
       稳定性不如 PC。 */
    if (rec_is_valid(rec) != 0
        && rec->last_kind == kind
        && rec->last_pc == ctx->pc) {
        if (rec->consec < 0xFFFFu) {
            rec->consec++;
        }
    } else {
        rec->consec = 1u;               /* 新签名：重新计数 */
    }

    /* 记录有效性：如果 .noinit 里还是随机值，这次记录要把它变成有效记录。
       注意不能调 rec_reset()（那会把 consec 清 0，丢掉刚算出来的计数），
       所以只补 magic 与 boot_count。 */
    if (rec_is_valid(rec) == 0) {
        rec->magic      = GW_FAULT_MAGIC;
        rec->boot_count = 1u;
        rec->hist_count = 0u;
    }

    rec->last_kind = kind;
    rec->last_boot = (uint8_t)GW_BOOT_FAULT;
    rec->last_pc   = ctx->pc;
    rec->last_lr   = ctx->lr;
    rec->last_psr  = ctx->psr;
    rec->last_cfsr = ctx->cfsr;
    rec->last_hfsr = ctx->hfsr;

    hist_push(rec, kind, ctx->pc);
}

gw_safe_reason_t gw_fault_need_safe_mode(const gw_fault_rec_t *rec)
{
    if (rec == 0 || rec_is_valid(rec) == 0) {
        return GW_SAFE_NONE;        /* 无有效记录：不可能判断，不要误进安全模式 */
    }
    if (rec->consec >= GW_FAULT_ESCALATE_AT) {
        return GW_SAFE_FAULT_LOOP;
    }
    return GW_SAFE_NONE;
}

void gw_fault_note_intentional_reset(gw_fault_rec_t *rec)
{
    if (rec == 0) {
        return;
    }
    if (rec_is_valid(rec) == 0) {
        return;                     /* 没记录就不用管 */
    }

    /* 清掉故障标记 + 把连续计数归零。
       只清 last_kind 不够：consec 还留着上次的值，
       启动时虽然不会误判成"故障启动"，
       但下一次真故障会让 consec 从旧值继续涨，提前触发安全模式。 */
    rec->last_kind = (uint8_t)GW_FAULT_NONE;
    rec->consec    = 0u;
}

gw_fault_kind_t gw_fault_last_kind(const gw_fault_rec_t *rec)
{
    if (rec == 0 || rec_is_valid(rec) == 0) {
        return GW_FAULT_NONE;
    }
    return (gw_fault_kind_t)rec->last_kind;
}

uint16_t gw_fault_consecutive(const gw_fault_rec_t *rec)
{
    if (rec == 0 || rec_is_valid(rec) == 0) {
        return 0u;
    }
    return rec->consec;
}

uint8_t gw_fault_history_count(const gw_fault_rec_t *rec)
{
    if (rec == 0 || rec_is_valid(rec) == 0) {
        return 0u;
    }
    if (rec->hist_count > GW_FAULT_HISTORY) {
        return GW_FAULT_HISTORY;    /* 被损坏过：夹到合法范围 */
    }
    return rec->hist_count;
}

int gw_fault_history_at(const gw_fault_rec_t *rec, uint8_t i,
                        uint8_t *kind, uint32_t *pc)
{
    if (rec == 0 || kind == 0 || pc == 0) {
        return -1;
    }
    if (i >= gw_fault_history_count(rec)) {
        return -1;
    }
    *kind = rec->hist[i].kind;
    *pc   = rec->hist[i].pc;
    return 0;
}

int gw_fault_iwdg_is_enabled(uint32_t pr, uint32_t rlr)
{
    /* ⚠️ 判据是"与**复位默认值**不同"，不是"不为 0"。
     *
     *    IWDG_PR  复位默认值 = 0x000
     *    IWDG_RLR 复位默认值 = 0xFFF   ← 关键：**本来就非 0**
     *
     *    写成 `(pr != 0) || (rlr != 0)` 的话，冷启动时 rlr = 0xFFF
     *    就会被判成"狗已使能" —— 而引导程序里 `hiwdg` 句柄从没被
     *    MX_IWDG_Init() 填过（Instance = NULL），于是
     *    HAL_IWDG_Refresh(&hiwdg) 会去写地址 0x00000000：
     *    不精确总线错误 -> HardFault -> 复位 -> 39Hz 复位循环。
     *    （2026-10-11 板上实测，缺陷 O。）
     */
    if (pr != GW_IWDG_PR_RESET) {
        return 1;
    }
    if (rlr != GW_IWDG_RLR_RESET) {
        return 1;
    }
    return 0;
}

const char *gw_fault_kind_str(gw_fault_kind_t k)
{
    switch (k) {
    case GW_FAULT_NONE:       return "NONE";
    case GW_FAULT_HARD:       return "HARDFAULT";
    case GW_FAULT_MEMMANAGE:  return "MEMMANAGE";
    case GW_FAULT_BUSFAULT:   return "BUSFAULT";
    case GW_FAULT_USAGEFAULT: return "USAGEFAULT";
    case GW_FAULT_ASSERT:     return "ASSERT";
    case GW_FAULT_STACK:      return "STACK_OVF";
    case GW_FAULT_WWDG:       return "WWDG";
    case GW_FAULT_UNKNOWN:    return "UNKNOWN";
    default:                  return "?";
    }
}

const char *gw_boot_cause_str(gw_boot_cause_t c)
{
    switch (c) {
    case GW_BOOT_POWER_ON: return "POWER_ON";
    case GW_BOOT_IWDG:     return "IWDG";
    case GW_BOOT_SOFTWARE: return "SOFTWARE";
    case GW_BOOT_FAULT:    return "FAULT";
    default:               return "UNKNOWN";
    }
}

const char *gw_safe_reason_str(gw_safe_reason_t r)
{
    switch (r) {
    case GW_SAFE_NONE:       return "NONE";
    case GW_SAFE_FAULT_LOOP: return "FAULT_LOOP";
    default:                 return "UNKNOWN";
    }
}
