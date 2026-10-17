/* ==================================================================
 *  ILI9341 320x240 TFT 驱动（SPI1，轮询）
 *
 *  设计约束（2026-10-11 与用户确认的设计，见交接文档）：
 *    · **不用 LVGL、不做全屏缓冲** —— 这是一颗 64KB RAM/512KB flash 的
 *      F103ZET6，而 app 槽只有 136KB。所以驱动只提供"矩形填充 + 8x16 文本"
 *      这类最小图元，页面逻辑放在 app/gw_ui.c 里用它们拼。
 *    · **单次调用有界**：所有整块传输都用固定大小的栈缓冲分片发送
 *      （见 LCD_CHUNK_PX），绝不 malloc、绝不要求调用方提供大缓冲。
 *    · **总线分时**：SPI1 与 XPT2046 触摸共用，所以对外暴露一对
 *      **弱符号** lcd_bus_acquire()/lcd_bus_release()，由 RTOS 侧用互斥量
 *      覆盖（仓库里 gw_rtos_housekeeping 就是同样的弱覆盖模式）。
 *    · **RTOS 无关**：本文件不 include 任何 FreeRTOS 头，也不做延时之外的
 *      阻塞等待；可以被裸机路径直接调用。
 *
 *  引脚（全部由 CubeMX 生成并已在 gpio.c 初始化，本文件只使用不重配）：
 *    SCK PA5 / MISO PA6 / MOSI PA7 / CS PA4 / DC PB0 / RST PB1 / BL PB5
 *  ⚠️ 背光 v1 只做开/关（PB5 是普通输出）。PWM 调光需要新增 TIM3_CH2
 *     （PB5 = TIM3_CH2），那要动 CubeMX —— 明确留到 v2，避免重生成覆盖手工修复。
 * ================================================================== */
#ifndef LCD_ILI9341_H
#define LCD_ILI9341_H

#include <stdint.h>

/* ---- 几何（横屏）---- */
#define LCD_W            320u
#define LCD_H            240u

/* ---- 颜色（RGB565）---- */
#define LCD_RGB565(r, g, b)  ((uint16_t)((((r) & 0xF8u) << 8) | \
                                         (((g) & 0xFCu) << 3) | \
                                         (((b) & 0xF8u) >> 3)))
#define LCD_BLACK        0x0000u
#define LCD_WHITE        0xFFFFu
#define LCD_RED          0xF800u
#define LCD_GREEN        0x07E0u
#define LCD_BLUE         0x001Fu
#define LCD_YELLOW       0xFFE0u
#define LCD_CYAN         0x07FFu
#define LCD_MAGENTA      0xF81Fu
#define LCD_GRAY         0x8410u
#define LCD_DARKGRAY     0x4208u
#define LCD_ORANGE       0xFD20u

/* 8x16 文本的字符尺寸（与 lcd_font_8x16.h 一致）*/
#define LCD_CHAR_W       8u
#define LCD_CHAR_H       16u

/**
 * 初始化（含硬复位、ILI9341 上电序列、横屏、16bpp）
 * @return 0 成功；-1 复位后读 ID 失败（多半是接线/供电问题）
 *
 * ⚠️ 会用 lcd_read_id() 自检：这一步是上板第一件要做的事 ——
 *    它把"接线对不对"与"后面初始化序列对不对"分开，省掉大量盲调。
 */
int  lcd_init(void);

/* 不依赖 HAL 时基的毫秒延时（DWT 周期计数忙等）。
   ⚠️ 面板与界面初始化都用它 —— 实测本系统里 uwTick 会在某处停止前进，
   HAL_Delay 会永久卡死；这个函数与 HAL 时基彻底解耦。 */
void lcd_delay_ms(uint32_t ms);

/* 上一次 RDDID 自检的结果：g_lcd_id_ok=1 表示面板应答了（ID 存在 g_lcd_last_id）。
   ⚠️ 读 ID 走 MISO —— 它是"接线通不通"的直接证据，所以诊断页会显示它。
   读不到**不代表**屏幕不能用（很多模块没引出 SDO），代码不会因此不显示。 */
extern uint32_t g_lcd_last_id;
extern int      g_lcd_id_ok;

/** 读控制器 ID（RDDID 0x04）。成功返回 0 并写 *id（正常为 0x009341）。 */
int  lcd_read_id(uint32_t *id);

/** 填充整屏 */
void lcd_fill(uint16_t color);

/** 填充矩形（自动裁剪到屏幕内；w/h 为 0 则直接返回） */
void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                   uint16_t color);

/**
 * 把一块 RGB565 像素直接写进矩形区域（LVGL 这类图形库刷新的落点）
 *
 * ⚠️ 这是给"外部图形库"用的唯一原始接口：调用方给像素，驱动只管搬运。
 * ⚠️ 实现上**必须整段拉低 CS**（与 lcd_fill_rect 同样的理由）：兼容屏在像素流
 *    中途抬 CS 会中断/重新同步，表现为"数据全丢、屏上什么都没有"（实测过）。
 * ⚠️ 不做缓冲：直接分片发调用方的内存，单次调用有界（不 malloc）。
 *
 * @param px  指向左上角开始的连续 RGB565 像素（每行 w 个，**无行间 padding**）
 */
void lcd_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
              const uint16_t *px);

/** 画一个 1px 边框（页面分区用） */
void lcd_frame(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/**
 * 在 (x,y) 画一串 ASCII 文本（8x16，超出右边界自动截断）
 * @param s  以 '\0' 结尾
 * @return 实际画出的字符数
 */
uint16_t lcd_text(uint16_t x, uint16_t y, const char *s,
                  uint16_t fg, uint16_t bg);

/** 背光开/关（v1 仅两态） */
void lcd_backlight(int on);

/* ---- 总线分时钩子（弱符号，RTOS 侧可用互斥量覆盖）----
 * 为什么需要：SPI1 同时挂 LCD 与 XPT2046。UI 任务与触摸读取如果并发，
 * 会把命令/数据相位交错，表现为"偶发花屏/触摸乱跳"这种最难查的故障。 */
void lcd_bus_acquire(void);
void lcd_bus_release(void);

#endif /* LCD_ILI9341_H */
