/**
 * @file    gw_log.c
 * @brief   分级日志实现（纯逻辑）
 */
#include "gw_log.h"

#include <string.h>

/* ==================================================================
 *  打包 / 解包
 *
 *  布局：level(2) | module(6) | line(24)，从高位到低位。
 *  为什么把 line 放低位：它是唯一可能超过 16 位的字段（虽然实际不会），
 *  放低位可以整段取出而不影响其它字段。
 * ================================================================== */
#define LOG_PACK_LEVEL_SHIFT    30u
#define LOG_PACK_MODULE_SHIFT   24u
#define LOG_PACK_LINE_MASK      0x00FFFFFFu
#define LOG_PACK_LEVEL_MASK     0x03u
#define LOG_PACK_MODULE_MASK    0x3Fu

uint32_t gw_log_pack(gw_log_level_t lv, gw_log_module_t mod, uint16_t line)
{
    uint32_t p = 0u;

    p |= ((uint32_t)lv & LOG_PACK_LEVEL_MASK) << LOG_PACK_LEVEL_SHIFT;
    p |= ((uint32_t)mod & LOG_PACK_MODULE_MASK) << LOG_PACK_MODULE_SHIFT;
    p |= ((uint32_t)line & LOG_PACK_LINE_MASK);
    return p;
}

gw_log_level_t gw_log_rec_level(uint32_t packed)
{
    return (gw_log_level_t)((packed >> LOG_PACK_LEVEL_SHIFT) & LOG_PACK_LEVEL_MASK);
}

gw_log_module_t gw_log_rec_module(uint32_t packed)
{
    return (gw_log_module_t)((packed >> LOG_PACK_MODULE_SHIFT) & LOG_PACK_MODULE_MASK);
}

uint16_t gw_log_rec_line(uint32_t packed)
{
    return (uint16_t)(packed & LOG_PACK_LINE_MASK);
}

/* 编译期检查：模块数必须放得进 6 位 */
_Static_assert((uint32_t)GW_LOG_MOD_COUNT <= (LOG_PACK_MODULE_MASK + 1u),
               "模块数量超出 6 位打包空间");
_Static_assert((uint32_t)GW_LOG_DEBUG <= LOG_PACK_LEVEL_MASK,
               "日志等级数量超出 2 位打包空间");
/* 行号必须放得进 24 位：单个源文件不可能超过 1600 万行 */
_Static_assert(0xFFFFu <= LOG_PACK_LINE_MASK, "行号字段太窄");

/* ==================================================================
 *  字符串
 * ================================================================== */
const char *gw_log_level_str(gw_log_level_t lv)
{
    switch (lv) {
    case GW_LOG_ERROR: return "E";
    case GW_LOG_WARN:  return "W";
    case GW_LOG_INFO:  return "I";
    case GW_LOG_DEBUG: return "D";
    default:           return "?";
    }
}

const char *gw_log_module_str(gw_log_module_t mod)
{
    switch (mod) {
    case GW_LOG_MOD_APP:    return "APP";
    case GW_LOG_MOD_RS485:  return "RS485";
    case GW_LOG_MOD_MODBUS: return "MODBUS";
    case GW_LOG_MOD_DHT22:  return "DHT22";
    case GW_LOG_MOD_CAN:    return "CAN";
    case GW_LOG_MOD_UPLINK: return "UPLINK";
    case GW_LOG_MOD_PARAM:  return "PARAM";
    case GW_LOG_MOD_FRAME:  return "FRAME";
    case GW_LOG_MOD_FAULT:  return "FAULT";
    case GW_LOG_MOD_SYS:    return "SYS";
    default:                return "?";
    }
}

/* ==================================================================
 *  内部：取时间戳
 * ================================================================== */
static uint32_t log_now(const gw_log_t *l)
{
    if (l->io.now_ms != 0) {
        return l->io.now_ms(l->io.ctx);
    }
    return l->ts_acc;
}

/* ==================================================================
 *  对外接口
 * ================================================================== */

int gw_log_init(gw_log_t *l, const gw_log_io_t *io, gw_log_level_t level)
{
    if (l == 0) {
        return -1;
    }
    if ((uint32_t)level > (uint32_t)GW_LOG_DEBUG) {
        return -2;                  /* 等级非法：不允许"记录一切"之外的意外值 */
    }

    memset(l, 0, sizeof(*l));
    if (io != 0) {
        l->io = *io;
    }
    l->level  = level;
    l->inited = 1u;
    return 0;
}

/* ==================================================================
 *  日志互斥钩子：默认空实现（裸机 / 主机测试都不需要）
 *
 *  ⚠️ RTOS 构建里 `port/gw_rtos.c` 会给同名的**强定义**（FreeRTOS 互斥量），
 *     链接器自动覆盖这里。少一边不会报错 —— 只会静默失去保护
 *     （RTOS 下并发写日志会丢记录 / 串输出），所以 CI 有文本检查盯着成对存在。
 * ================================================================== */
__attribute__((weak)) void gw_log_guard_enter(void)
{
}

__attribute__((weak)) void gw_log_guard_exit(void)
{
}

void gw_log_write(gw_log_t *l, gw_log_level_t lv, gw_log_module_t mod,
                  uint16_t line, uint32_t arg0, uint32_t arg1)
{
    gw_log_rec_t *r;

    if (l == 0 || l->inited == 0u) {
        return;
    }

    /* ---- 等级过滤 ----
       等级数值越小越严重，所以"lv <= level"就是要记录。
       这是本模块里唯一的热路径判断，尽量放最前面。 */
    if ((uint32_t)lv > (uint32_t)l->level) {
        l->stat.filtered++;
        return;
    }

    /* ★ 从这里开始动环形缓冲与 sink：RTOS 下必须先拿锁（缺陷 Y）
     *
     *  ⚠️ 锁的粒度覆盖"环形写入 + sink 输出"整段，理由：
     *     ① 环形本身的 head/count 是共享状态；
     *     ② sink 里的 `HAL_UART_Transmit` **不可重入**，两个任务同时进来
     *        会输出交错甚至超时。
     *  ⚠️ 但**不嵌套**：一次调用只取一次锁，`gw_log_read()` 同理，
     *     所以普通互斥量就够（不需要递归互斥量）。 */
    gw_log_guard_enter();

    /* ---- 分等级计数 ----
       为什么要单独计 ERROR/WARN：
       上层可以让它进寄存器表（"最近有没有出错"），
       运维不必去翻日志就能看出设备状态。 */
    if (lv == GW_LOG_ERROR) {
        l->stat.errors++;
    } else if (lv == GW_LOG_WARN) {
        l->stat.warns++;
    }

    /* ---- 写环形缓冲 ----
       ⚠️ 缓冲满时**丢最旧**。
       日志与 CAN 报文的策略刻意不同：排查问题时最关心"刚刚发生了什么"，
       崩溃前最后几条是定位根因的关键，绝不能因为缓冲满而丢掉。 */
    r = &l->ring[l->head];
    r->ts_ms  = log_now(l);
    r->packed = gw_log_pack(lv, mod, line);
    /* ⚠️ 参数必须一起存 —— 否则 flush 重放时只能传 0。
       见 inc/gw_log.h 里 gw_log_rec_t 的说明（这是一个真实缺陷的修复）。 */
    r->arg0   = arg0;
    r->arg1   = arg1;

    l->head = (uint16_t)((l->head + 1u) % GW_LOG_RING_DEPTH);
    l->stat.written++;

    if (l->count < GW_LOG_RING_DEPTH) {
        l->count++;
    } else {
        /* 已经满了：这次写入覆盖掉了最旧的一条，如实计数。
           不计数的话，读日志的人会以为"只有这些日志"，
           从而错过更早的关键信息。 */
        l->stat.dropped++;
    }

    /* ---- 立即输出（如果配置了 sink） ----
       为什么在写的时候也输出一次：
       有些调用方只想要"实时打印"的行为（开发期），
       不想每次还要记得调 flush()。
       但要注意：如果 sink 很慢（串口），这条路径就会慢 ——
       量产固件应当**不配 sink**，改由主循环调 flush() 批量输出。
       两种用法都在测试里覆盖了。 */
    if (l->io.sink != 0) {
        l->io.sink(l->io.ctx, r);
    }
    if (l->io.sink_text != 0) {
        l->io.sink_text(l->io.ctx, lv, mod, line, arg0, arg1);
    }

    gw_log_guard_exit();
}

void gw_log_tick(gw_log_t *l, uint32_t elapsed_ms)
{
    if (l == 0 || l->inited == 0u) {
        return;
    }
    l->ts_acc += elapsed_ms;
}

void gw_log_set_level(gw_log_t *l, gw_log_level_t level)
{
    if (l == 0 || l->inited == 0u) {
        return;
    }
    if ((uint32_t)level > (uint32_t)GW_LOG_DEBUG) {
        return;                     /* 非法值：保持原样，不乱改 */
    }
    l->level = level;
}

gw_log_level_t gw_log_get_level(const gw_log_t *l)
{
    return (l == 0) ? GW_LOG_INFO : l->level;
}

/* 前置声明：下面两个公开函数要用它（定义在 gw_log_read 之后，见那里说明） */
static int gw_log_read_locked(gw_log_t *l, gw_log_rec_t *out);

int gw_log_read(gw_log_t *l, gw_log_rec_t *out)
{
    int r;

    gw_log_guard_enter();
    r = gw_log_read_locked(l, out);
    gw_log_guard_exit();

    return r;
}

/* ------------------------------------------------------------------
 * 内部：读一条（**不加锁**）
 *
 * ⚠️ 单独抽出来的理由（缺陷 Y）：`gw_log_flush()` 要"整个循环持一次锁"
 *    —— 它既读记录又调 sink，如果逐条走会加锁的 `gw_log_read()`，
 *    那 sink 就在锁外（两个任务的串口输出又会交错）。
 *    用这个不加锁的版本，flush 就能一次加锁包住整圈，
 *    而**不嵌套**（普通互斥量就够，不需要递归互斥量）。
 * ------------------------------------------------------------------ */
static int gw_log_read_locked(gw_log_t *l, gw_log_rec_t *out)
{
    uint16_t oldest;

    if (l == 0 || out == 0 || l->inited == 0u) {
        return 0;
    }
    if (l->read_idx >= l->count) {
        return 0;                   /* 已读完这一圈 */
    }

    /* 最旧一条的位置：head 往前数 count 条 */
    oldest = (uint16_t)((l->head + GW_LOG_RING_DEPTH - l->count)
                        % GW_LOG_RING_DEPTH);

    *out = l->ring[(uint16_t)((oldest + l->read_idx) % GW_LOG_RING_DEPTH)];
    l->read_idx++;
    return 1;
}

void gw_log_rewind(gw_log_t *l)
{
    if (l == 0) {
        return;
    }
    l->read_idx = 0u;
}

uint16_t gw_log_flush(gw_log_t *l)
{
    gw_log_rec_t rec;
    uint16_t     n = 0u;

    if (l == 0 || l->inited == 0u) {
        return 0u;
    }
    if (l->io.sink == 0 && l->io.sink_text == 0) {
        return 0u;                  /* 没有输出通道：无事可做 */
    }

    /* ⚠️⚠️ 这里**故意不整圈持锁**（缺陷 Y 的第二版，2026-10-11）
     *
     *  第一版是"整圈持一次锁"——正确性没问题，但代价是：
     *      一次 flush 最多吐 一整个环形（GW_LOG_RING_DEPTH 条）× 每条一次
     *      串口发送（超时 20 ms）-> **持锁可达数百毫秒**。
     *  而 `gw_log_write()` 也要取同一把锁 -> **别的任务想打条日志会被卡住
     *  几百毫秒**；从站任务若正好要打日志，Modbus 响应就会被拖过 5 ms 硬超时。
     *
     *  ⚠️ 这正是"缺陷 X 第二版"那类错误：修一个缺陷，把另一条设计（响应时间）
     *     废掉了。所以这里改成**逐条走公开的 `gw_log_read()`**（每条内部取放一次锁，
     *     持锁只有几微秒），代价只是"flush 的串口输出可能与另一条日志交错" ——
     *     那是**观感问题**，而前面那个是**功能问题**，取观感。
     *
     *  ⚠️ 也没有嵌套取锁：锁在 `gw_log_read()` 里取，本函数自己不取。 */
    while (gw_log_read(l, &rec) != 0) {
        if (l->io.sink != 0) {
            l->io.sink(l->io.ctx, &rec);
        }
        if (l->io.sink_text != 0) {
            /* ⚠️ 用**记录里存的**参数，不要硬编码 0。
               原来这里是 `0u, 0u`，导致重放的日志参数全丢 ——
               而"崩溃后 dump 历史"正是要那些参数。见 gw_log.h 的说明。 */
            l->io.sink_text(l->io.ctx,
                            gw_log_rec_level(rec.packed),
                            gw_log_rec_module(rec.packed),
                            gw_log_rec_line(rec.packed),
                            rec.arg0, rec.arg1);
        }
        n++;

        /* 防御：万一 read 的实现有问题，不能死循环在主循环里 */
        if (n >= GW_LOG_RING_DEPTH) {
            break;
        }
    }

    return n;
}

const gw_log_stat_t *gw_log_stats(const gw_log_t *l)
{
    return (l == 0) ? 0 : &l->stat;
}

uint16_t gw_log_count(const gw_log_t *l)
{
    return (l == 0) ? 0u : l->count;
}
