/**
 * @file    gw_timing_port.c
 * @brief   把片内耗时统计映射到输入寄存器（上板量化测量用）
 *
 * ============================================================================
 *  为什么需要"片内测量"而不是看 PC 端的数字
 *
 *  我实测过 PC 端的往返延迟：
 *      measure_latency.py -> 中位 4.46 ms
 *  但 115200 下 9 字节响应在线上只需 0.78 ms
 *  —— 也就是说那 4.46 ms 里**绝大部分是 USB 串口驱动与 Python 的开销**，
 *     **不是从站的响应延迟**。
 *
 *  ⚠️ 那个数字不能写进报告当"从站响应延迟"，面试一追问就露馅。
 *
 *  所以用片内 DWT 微秒计数器测：**不含任何 PC/USB/驱动开销**，
 *  得到的才是可以写进报告的真值。
 *
 *  ----------------------------------------------------------------------------
 *  为什么这个文件里几乎没逻辑
 *
 *  `gw_timing.c` 是纯逻辑层（已测试），`gw_app_poll()` 里也早已在
 *  6 个相位的边界调好了 `gw_timing_begin()`（见 app/gw_app.c 第 774~789 行）。
 *  这里只做**一件事**：把统计值搬进寄存器。
 *
 *  也就是说：本项目原设计里就留了这一步的接线位置，
 *  现在只是把它接上 —— 不需要改任何业务代码。
 * ============================================================================
 */
#include "gw_timing_port.h"

#include "modbus_regs.h"
#include "gw_dwt.h"     /* ⚠️ 临时：诊断用 gw_dwt_is_ready()，定位后随诊断代码一起删 */

/* ⚠️ 用"文件级 static + 公开函数"的持有方式，
 *    因为 cb_get_regs(ctx) 的 ctx 是串口句柄，拿不到 timing 结构。
 *    同一个模式在 port/gw_boot_port.c 里也用了（s_io）。 */
static gw_timing_t *s_tm = 0;

void gw_timing_port_bind(gw_timing_t *tm)
{
    s_tm = tm;
}

int gw_timing_port_fill(uint16_t *iregs, uint16_t count)
{
    if (iregs == 0) {
        return -1;
    }
    if (count < (uint16_t)(IREG_TM_LOAD_PERMIL + 1u)) {
        return -2;
    }

    if (s_tm == 0) {
        iregs[IREG_TM_SLAVE_MAX]   = 0xFFFEu;
        iregs[IREG_TM_SLAVE_AVG]   = 0xFFFEu;
        iregs[IREG_TM_LOOP_MAX]    = 0xFFFEu;
        iregs[IREG_TM_LOAD_PERMIL] = 0xFFFEu;
        return -1;
    }

    /* ---- 从站相位 ---- */
    {
        const gw_phase_stat_t *ph = gw_timing_phase(s_tm, GW_PHASE_SLAVE);

        if (ph != 0 && ph->calls > 0u) {
            iregs[IREG_TM_SLAVE_MAX] =
                (ph->max_us > 65534u) ? 0xFFFFu : (uint16_t)ph->max_us;
            {
                uint32_t av = gw_timing_avg_us(s_tm, GW_PHASE_SLAVE);
                iregs[IREG_TM_SLAVE_AVG] =
                    (av > 65534u) ? 0xFFFFu : (uint16_t)av;
            }
        } else {
            iregs[IREG_TM_SLAVE_MAX] = 0xFFFDu;
            iregs[IREG_TM_SLAVE_AVG] = 0xFFFDu;
        }
    }

    {
        const gw_loop_stat_t *lp = gw_timing_loop(s_tm);

        iregs[IREG_TM_LOOP_MAX] = (lp != 0)
            ? ((lp->max_us > 65534u) ? 0xFFFFu : (uint16_t)lp->max_us)
            : 0xFFFDu;
    }

    {
        uint32_t ld = gw_timing_load_permil(s_tm);

        iregs[IREG_TM_LOAD_PERMIL] = (ld > 65534u) ? 0xFFFFu : (uint16_t)ld;
    }

    return 0;
}
