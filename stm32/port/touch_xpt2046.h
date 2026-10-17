/* ==================================================================
 *  XPT2046 电阻触摸驱动（与 ILI9341 共用 SPI1）
 *
 *  2026-10-11 与用户确认：模块是 XPT2046（SPI），占用 T_CS=PB6 / T_IRQ=PB7。
 *
 *  三个必须知道的坑（都写在这里，免得下次重踩）
 *  1) **SPI1 要临时降速**：ILI9341 跑 9MHz，而 XPT2046 上限约 2MHz。
 *     所以本驱动在读取期间把 SPI1 的预分频临时调大（约 1.1MHz），读完**恢复**。
 *     全程持有 LCD 侧那条总线锁（lcd_bus_acquire/release）。
 *  2) **T_IRQ 必须上拉输入**：PENIRQ 是开漏，悬空读到的值是随机的 —— 
 *     与 CAN_RX(PB8) 当初不加上拉就被读成显性，是同一类坑。
 *  3) **校准常数必须上板标定**：本文件给的是典型值，第一次上板务必用
 *     touch_set_calib() 跑一遍（详见 touch_xpt2046.c 顶部注释）。
 *
 *  ⚠️ 引脚由本驱动**自己配置**（不经 CubeMX），理由见 gw_config.h 里的登记注释：
 *     这样 CubeMX 重新生成工程不会把 T_CS/T_IRQ 的配置冲掉。
 * ================================================================== */
#ifndef TOUCH_XPT2046_H
#define TOUCH_XPT2046_H

#include <stdint.h>

/** 初始化引脚（GPIOB 时钟、T_CS 输出、T_IRQ 上拉输入）。返回 0 成功。 */
int  touch_init(void);

/** 当前是否有触摸（读 T_IRQ，低有效）。1 = 有 */
int  touch_pressed(void);

/** 读原始 12 位坐标（每轴多次采样取中值；返回 0 成功，-1 采样不一致/SPI 失败）*/
int  touch_read_raw(uint16_t *raw_x, uint16_t *raw_y);

/** 读屏幕坐标（已滤波 + 按校准常数转换 + 裁剪到 0..319 / 0..239）；
 *  @return 1 = 有有效触摸并写回坐标；0 = 无触摸/值不可信 */
int  touch_get(uint16_t *scr_x, uint16_t *scr_y);

/** 设置校准（上板标定用；不调用则用文件里的典型值）*/
void touch_set_calib(uint16_t x_min, uint16_t x_max,
                     uint16_t y_min, uint16_t y_max,
                     int swap_xy, int invert_x, int invert_y);

/* ================= 诊断计数器（只读取证用，不参与控制逻辑） =================
 *
 * 用途：把两种**现象完全相同**的"点不动"分开（2026-10-16 新增，详见 .c 里说明）：
 *   g_touch_press_seen 在涨、g_touch_read_fail 在涨 -> SPI 读数问题（MISO 悬空）
 *   g_touch_press_seen 恒 0                          -> PENIRQ 没通
 *
 * g_touch_fail_reason 的取值：
 *   0 = 上一次读取成功
 *   1 = 传了空指针（调用方错误）
 *   2 = 三次采样离散过大（手指在动）
 *   3 = 读回全 0 / 全 1（典型是 MISO 没在驱动）
 *   4 = 其它
 *   5 = 没有按压（PENIRQ 为高）—— 由 touch_get() 记，**不计入** _read_fail
 */
extern volatile uint32_t g_touch_press_seen;
extern volatile uint32_t g_touch_read_fail;
extern volatile uint32_t g_touch_fail_reason;
extern volatile uint32_t g_touch_raw0;
extern volatile uint32_t g_touch_raw1;

#endif /* TOUCH_XPT2046_H */
