/**
 * @file    gw_timing_port.h
 * @brief   把片内耗时统计映射到输入寄存器（上板量化测量用）
 *
 * ============================================================================
 *  用法（三步，都在 main.c 里）
 *
 *      static gw_timing_t g_timing;
 *
 *      // 1) 初始化：时间源指向 DWT 微秒计数器
 *      gw_timing_init(&g_timing, gw_dwt_us, 0);
 *
 *      // 2) 接到应用层（它会在 6 个相位边界自动打点）
 *      gw_app_enable_timing(&g_app, &g_timing);
 *
 *      // 3) 接到寄存器填充（这样上位机就能读到）
 *      gw_timing_port_bind(&g_timing);
 *
 *  ⚠️ 第 2 步之后，**业务代码一行都不用改** ——
 *     `gw_app_poll()` 里早就埋好了 `gw_timing_begin()`。
 *
 *  ⚠️ 若不做第 2 步，统计恒为 0（是"没接线"，不是"性能好"）。
 *     这个区别很重要：读出来全 0 时不要以为是最优表现。
 *
 *  寄存器映射见 inc/modbus_regs.h 的 IREG_TM_*（36~39）。
 * ============================================================================
 */
#ifndef GW_TIMING_PORT_H
#define GW_TIMING_PORT_H

#include <stdint.h>

#include "gw_timing.h"

/**
 * 绑定被填充的统计结构
 *
 * @param tm  gw_timing_t 实例（传 0 表示解绑，填充函数随后返回错误）
 *
 * 为什么用"绑定"而不是把 tm 当参数传：
 *   填寄存器的调用点在 `cb_get_regs(ctx)` 里，而那个 ctx 是串口句柄，
 *   拿不到 timing 结构。用文件级指针是最小改动。
 */
void gw_timing_port_bind(gw_timing_t *tm);

/**
 * 把统计值写进输入寄存器
 *
 * @param iregs  输入寄存器数组
 * @param count  数组长度（必须 >= IREG_TM_LOAD_PERMIL + 1）
 * @return 0 成功；-1 未绑定 tm；-2 寄存器表太短
 *
 * ⚠️ 统计值是**从开机累起**的。要测某个区间，先调 `gw_timing_reset()`。
 */
int gw_timing_port_fill(uint16_t *iregs, uint16_t count);

#endif /* GW_TIMING_PORT_H */
