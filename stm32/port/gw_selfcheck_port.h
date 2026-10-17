/**
 * @file    gw_selfcheck_port.h
 * @brief   上板自检的硬件项（依赖 HAL，只能上板跑）
 *
 * ============================================================================
 *  怎么用
 *
 *  在 `main()` 里初始化完外设之后、进主循环之前，插一段：
 *
 *      #include "gw_selfcheck.h"
 *      #include "gw_selfcheck_port.h"
 *
 *      gw_selfcheck_t sc;
 *      char           buf[2048];
 *
 *      if (gw_selfcheck_port_init(&sc) == 0) {
 *          (void)gw_selfcheck_run(&sc);
 *          (void)gw_selfcheck_report(&sc, buf, sizeof(buf));
 *          printf("%s\r\n", buf);              // 从 USART1 打出来
 *
 *          if (gw_selfcheck_foundation_ok(&sc) == 0) {
 *              // 基础项没过，后面的验证没有意义。
 *              // 别急着往下调 —— 先看报告里第一个 FAIL 的"先查什么"。
 *              for (;;) { }
 *          }
 *      }
 *
 *  ----------------------------------------------------------------------------
 *  ⚠️ 自检会**真的动硬件**，注意这三点
 *
 *  **① RS485 自检会往总线上发数据。**
 *     如果同一根总线上还挂着别的 Modbus 从站，它们会收到一个
 *     发往地址 0xFA 的请求 —— 正常情况下它们会忽略（地址不匹配），
 *     但如果现场有设备用 0xFA 这个地址，就会产生干扰。
 *     所以 RS485 项在自检里是**可选**的（见 `gw_selfcheck_port_set_rs485`）。
 *
 *  **② Flash 自检会擦写参数区的一个槽。**
 *     它用的是"最旧的槽"（序号最小的那个），所以不会破坏当前有效参数。
 *     但**会消耗一次擦写寿命**（约 1 万次里的一次）。
 *     如果不想让它动 Flash，用 `gw_selfcheck_port_set_flash(0)` 关掉。
 *
 *  **③ DHT22 自检会占用 PG11 约 5ms（期间关中断）。**
 *     如果 PG11 还接着别的东西，可能读到垃圾值 —— 但那也只是"自检失败"，
 *     不会损坏硬件。
 *
 *  ----------------------------------------------------------------------------
 *  ⚠️ 一个必须诚实说明的局限
 *
 *  **CLOCK 项无法真正"验证"时钟频率** ——
 *  `SystemCoreClock` 是软件算出来的值，它与你写的 PLL 配置一致，
 *  但如果 HSE 晶振实际是 8MHz 而你按 12MHz 配，软件照样会认为
 *  "配置正确"。
 *
 *  真正的验证方法只有两个：
 *    a) **用 MCO 把时钟输出到引脚，用示波器/频率计量**（最可靠）
 *    b) **用 DWT 测一个已知时长**：例如用 `HAL_Delay(1000)` 延时 1 秒，
 *       同时用 DWT 数微秒 —— 如果 DWT 读到 1500000，说明
 *       实际时钟比预期慢 1.5 倍（因为 HAL_Delay 也是按时钟算的，
 *       两者的比值**只反映"软件假设"与"硬件实际"是否一致**）
 *
 *  等等 —— (b) 其实也不成立：`HAL_Delay` 和 DWT 都基于同一个时钟，
 *  所以它们的比值永远是 1。**唯一可靠的办法是外部测量**。
 *
 *  所以本模块的 CLOCK 项做的是：**报告 SystemCoreClock 的值，
 *  让上板的人拿它与"应该的值"对照**，并明确提示
 *  "这一项必须用示波器/频率计确认，软件无法自证"。
 *  DWT 项则做**自洽性检查**（例如 `gw_dwt_delay_us(1000)` 是否
 *  真的等了约 1000µs —— 这能查出"DWT 没使能"或"计数器没走"，
 *  但查不出"时钟频率错了"）。
 * ============================================================================
 */
#ifndef GW_SELFCHECK_PORT_H
#define GW_SELFCHECK_PORT_H

#include <stdint.h>

#include "gw_selfcheck.h"

/**
 * 初始化自检上下文（含硬件项表）
 *
 * @return 0 成功；<0 失败
 *
 * 默认全部项都启用。要关掉某几项用下面的 setter。
 */
int gw_selfcheck_port_init(gw_selfcheck_t *sc);

/**
 * 启用/禁用 RS485 自检（默认启用）
 *
 * @param enable 0 = 关（总线上有别的设备时建议关掉）
 */
void gw_selfcheck_port_set_rs485(int enable);

/**
 * 启用/禁用 Flash 自检（默认启用）
 *
 * @param enable 0 = 关（不想消耗擦写寿命时关掉）
 */
void gw_selfcheck_port_set_flash(int enable);

/**
 * 设置期望的时钟频率（Hz）
 *
 * @param hz 期望值，默认按 `GW_SC_EXPECT_SYSCLK`（72 MHz）
 *
 * 如果你的工程用了别的频率（例如超频到 128MHz），必须改这里 ——
 * 否则 CLOCK 项会误报。
 */
void gw_selfcheck_port_set_expect_clock(uint32_t hz);

/** 期望的系统时钟（Hz）。STM32F103 标准配置是 72 MHz。 */
#define GW_SC_EXPECT_SYSCLK     72000000u

/** 时钟容差（±1%）。HSE 晶振精度通常在 ±20~50ppm，1% 足够宽松。 */
#define GW_SC_CLOCK_TOL         720000u

#endif /* GW_SELFCHECK_PORT_H */
