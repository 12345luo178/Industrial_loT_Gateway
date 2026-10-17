/* 由 tools/gen_font.py 生成 —— 不要手改。 */
#ifndef LCD_FONT_8X16_H
#define LCD_FONT_8X16_H

#include <stdint.h>

#define LCD_FONT_W        8u
#define LCD_FONT_H        16u
#define LCD_FONT_BYTES    16u
#define LCD_FONT_FIRST    0x20u
#define LCD_FONT_CHARS    95u

/* [字符序号][行]，字符序号 = ch - LCD_FONT_FIRST */
extern const uint8_t g_lcd_font8x16[LCD_FONT_CHARS][LCD_FONT_BYTES];

#endif /* LCD_FONT_8X16_H */
