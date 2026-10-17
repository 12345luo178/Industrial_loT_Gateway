/* ==================================================================
 *  第一个 LVGL 页面的接口（实现见 gw_lvgl_ui.c）
 *
 *  ⚠️ 这是与 app/gw_ui.c（自绘界面）并行的**第二条显示路径**：
 *     目的是先在 LVGL 上把"能不能跑、内存够不够、刷新通不通"验证清楚，
 *     验证通过再决定是否把自绘界面整体换掉。两条路径不要同时初始化。
 * ================================================================== */
#ifndef GW_LVGL_UI_H
#define GW_LVGL_UI_H

#include <stdint.h>

#include "lcd_ili9341.h"        /* 用到 LCD_W / LCD_H */

/** 建一次页面（在 lvgl_port_init() 之后调用）*/
void gw_lvgl_ui_build(void);

/** 当前 LVGL 页签编号（0=OVW, 1=CAN, 2=DIAG, 3=CFG），板上诊断用。 */
extern volatile uint32_t g_lvgl_ui_page;

/** 从**输入寄存器快照**刷新一遍界面（在 UI 任务钩子里周期调用）。
 *
 *  ⚠️ 参数就是 `g_regs.input`（40 个输入寄存器），与自绘界面 gw_ui 读的是
 *     **同一份快照** —— 屏幕上看到的就是主站看到的那一份，不存在第二份状态。
 *     传 NULL 直接返回（屏幕没接上时调用方不必自己分支）。
 *
 *  ⚠️ 内部对"文本没变就不 set"做了判断：LVGL 的 label 是脏矩形刷新，
 *     但 set 同值仍会把对象标脏。UI 任务周期 50ms，若每拍都无脑 set，
 *     温度/时间没变时也会白白重绘。 */
void gw_lvgl_ui_update(const uint16_t *iregs);

/* ---- 数值更新（与自绘版 gw_ui 的语义对齐）---- */
void gw_lvgl_ui_set_temp(int16_t temp_x10);
void gw_lvgl_ui_set_humi(uint16_t humi_x10);
void gw_lvgl_ui_set_uptime(uint32_t sec);
void gw_lvgl_ui_set_can(int bus_ok);
void gw_lvgl_ui_set_mqtt(int mqtt_ok);
void gw_lvgl_ui_set_bar(uint8_t idx, uint8_t pct);

#endif /* GW_LVGL_UI_H */
