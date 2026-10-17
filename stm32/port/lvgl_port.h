/* ==================================================================
 *  LVGL 移植层（F103ZET6 + ILI9341 SPI + 极限瘦身配置）
 *
 *  由来（2026-10-14 实测）：
 *    · 之前的设计前提是"**不用 LVGL**"（64KB RAM / app 槽 136KB 太紧）。
 *    · 本次按"极限瘦身"实测：LVGL v8.2 + label/btn/bar/line 四个控件
 *      + 一个 ASCII 字库 + 关默认主题/日志/断言/flex/grid
 *      ⇒ **text ≈ 53.5 KB**（app 槽还剩 ~76 KB）⇒ **放得下**。
 *    · RAM 是紧的一侧：LVGL 静态池 8 KB + 绘制缓冲 6.4 KB ≈ 14.4 KB
 *      （现有固件余量 ~20 KB）⇒ 所以两者都比官方推荐值小，这是**刻意**的。
 *
 *  设计约束：
 *    · **不引入 LVGL 的后台任务**：tick、handler 与 pointer 输入采样都由调用方
 *      （现有 UI 任务）驱动，不新增任务、不改任务表、不动栈预算。
 *    · **绘制缓冲 320x10**（6.4KB）：一屏 240 行 ⇒ 24 次 flush。
 *      按 SPI 9MHz 算，整屏 ≈ 150KB/1.125MBps ≈ 130ms；局部刷新远小于此。
 *    · **触摸输入**：XPT2046 与 ILI9341 共用 SPI1，复用 touch_xpt2046.c 的总线锁、
 *      降速与已标定坐标转换；LVGL 注册一个 pointer indev。
 * ================================================================== */
#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#include <stdint.h>

/* 绘制缓冲的像素数：320 x 10 = 3200 像素 = 6400 字节 */
#define LVGL_PORT_BUF_PX   3200u

/**
 * 初始化 LVGL 与显示驱动（lv_init + 注册 disp_drv）
 *
 * ⚠️ 必须在 lcd_init() 成功之后调用（flush 回调直接调 lcd_blit）
 * @return 0 成功
 */
int lvgl_port_init(void);

/** 推进 LVGL 内部时基（由调用方按自己的周期调用，例如每 50ms 调 lvgl_port_tick(50)）*/
void lvgl_port_tick(uint32_t elapsed_ms);

/** 跑一次 LVGL 的定时器/刷新处理（由调用方周期调用，例如每 50ms 一次）*/
void lvgl_port_handler(void);

/** 诊断：累计 flush 次数与发送像素数（上板验证刷新链路用）*/
extern volatile uint32_t g_lvgl_flush_calls;
extern volatile uint32_t g_lvgl_flush_px;

/** 诊断：整帧刷新耗时（微秒）—— "LVGL 能不能做动效"的判据。
 *
 *  · `g_lvgl_frame_us_max`：历次"本帧真有像素出去"的 handler 调用中最长的一次
 *  · `g_lvgl_frame_flushes`：最近一帧 flush 了几块
 *    （绘制缓冲 320x10 ⇒ 整屏 240 行 = 24 块；只刷 1~2 块就是局部刷新）
 *
 *  ⚠️ 计时用 DWT（`gw_dwt_us()`），要求 `gw_dwt_init()` 已调用；
 *     没初始化时该函数返回 0 ⇒ 这里恒为 0，不会给出假数据。 */
extern volatile uint32_t g_lvgl_frame_us_max;
extern volatile uint32_t g_lvgl_frame_flushes;

/** 触摸诊断：初始化成功标志、成功采样次数与最近可信坐标（RAM 取证用）。
 *
 *  ⚠️ `g_lvgl_touch_init_ok` **不能**当"触摸可用"的判据：
 *     touch_init() 只配置 T_CS/T_IRQ 就无条件 return 0，不与 XPT2046 通信
 *     ⇒ 它恒为 1（2026-10-16 排查时差点被这个假绿灯带偏）。
 *     要判"触摸真的通"请用 **`g_lvgl_touch_probe_ok`**（成功读到过坐标才置 1）。
 */
extern volatile uint32_t g_lvgl_touch_init_ok;
extern volatile uint32_t g_lvgl_touch_probe_ok;
extern volatile uint32_t g_lvgl_touch_reads;
extern volatile uint32_t g_lvgl_touch_x;
extern volatile uint32_t g_lvgl_touch_y;

#endif /* LVGL_PORT_H */
