/* ==================================================================
 *  界面离线渲染测试（host / PC）
 *
 *  为什么值得做：布局、几何、残字这些问题**编译查不出来**，而每次上板
 *  试错要烧录+复位+盯着看。这里把 app/gw_ui.c 原样编到 PC 上，
 *  用桩件把显示与触摸接口画进内存帧缓冲，导出 PPM 后转成 PNG 看一眼，
 *  就能在**烧板之前**发现大部分界面问题。
 *
 *  ⚠️ 本测试**不**编译真实驱动（lcd_ili9341.c / touch_xpt2046.c）：
 *     它们的显示与触摸接口由本文件实现（画到 fb）。
 *     字模用真实的 port/lcd_font_8x16.c（纯数据，host 可编）。
 *
 *  编译（在仓库根目录）：
 *    gcc -std=c99 -Wall -Wextra -Iinc -Iport -Iapp -Itest/ui_sim \
 *        app/gw_ui.c port/lcd_font_8x16.c test/test_ui_render.c -o ui_render
 *    ./ui_render            # 生成 ui_page1..4.ppm
 * ================================================================== */
#include <stdio.h>
#include <string.h>

#include "stm32f1xx_hal.h"      /* 本地桩件（test/ui_sim） */
#include "gw_ui.h"
#include "lcd_ili9341.h"
#include "lcd_font_8x16.h"
#include "modbus_regs.h"

/* ---- 桩件要的对象 ---- */
GPIO_TypeDef *const GPIOA = 0;
GPIO_TypeDef *const GPIOB = 0;
GPIO_TypeDef *const GPIOG = 0;

void HAL_Delay(uint32_t ms) { (void)ms; }

/* ---- 40 个输入寄存器 + 16 个保持寄存器（假的现场数据）---- */
static uint16_t g_in[40];
static uint16_t g_hold[16];

static const uint16_t *get_iregs(void) { return g_in; }
static const uint16_t *get_hregs(void) { return g_hold; }
/* 记录最近一次 set_hreg，供"触摸配置"断言用 */
static int      g_last_idx = -1;
static uint16_t g_last_val = 0u;
static int      g_set_cnt  = 0;

static void set_hreg(uint16_t idx, uint16_t val)
{
    if (idx < 16u) { g_hold[idx] = val; }
    g_last_idx = (int)idx;
    g_last_val = val;
    g_set_cnt++;
}

/* ---- 帧缓冲 + 桩件的"显示"实现 ---- */
#define FBW 320u
#define FBH 240u
static uint16_t fb[FBW * FBH];

static void fb_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         uint16_t color)
{
    uint16_t i, j;
    for (j = 0u; j < h; j++) {
        for (i = 0u; i < w; i++) {
            uint16_t px = (uint16_t)(x + i), py = (uint16_t)(y + j);
            if (px < FBW && py < FBH) { fb[py * FBW + px] = color; }
        }
    }
}

void lcd_fill(uint16_t color) { fb_fill_rect(0u, 0u, FBW, FBH, color); }
void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t c)
{
    fb_fill_rect(x, y, w, h, c);
}
void lcd_frame(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t c)
{
    fb_fill_rect(x, y, w, 1u, c);
    fb_fill_rect(x, (uint16_t)(y + h - 1u), w, 1u, c);
    fb_fill_rect(x, y, 1u, h, c);
    fb_fill_rect((uint16_t)(x + w - 1u), y, 1u, h, c);
}

uint16_t lcd_text(uint16_t x, uint16_t y, const char *s, uint16_t fg, uint16_t bg)
{
    uint16_t drawn = 0u;
    while (*s != '\0' && (uint16_t)(x + LCD_FONT_W) <= FBW) {
        uint8_t ch = (uint8_t)*s;
        const uint8_t *g = 0;
        uint8_t r, c;
        if (ch >= LCD_FONT_FIRST && ch < (uint8_t)(LCD_FONT_FIRST + LCD_FONT_CHARS)) {
            g = g_lcd_font8x16[ch - LCD_FONT_FIRST];
        }
        for (r = 0u; r < LCD_FONT_H; r++) {
            uint8_t bits = g ? g[r] : 0u;
            for (c = 0u; c < LCD_FONT_W; c++) {
                uint16_t px = (uint16_t)(x + c), py = (uint16_t)(y + r);
                uint16_t color = (bits & (0x80u >> c)) ? fg : bg;
                if (px < FBW && py < FBH) { fb[py * FBW + px] = color; }
            }
        }
        drawn++;
        s++;
        x = (uint16_t)(x + LCD_FONT_W);
    }
    return drawn;
}

void lcd_backlight(int on) { (void)on; }

/* 驱动里新增的、不依赖 HAL 时基的毫秒延时；host 测试里给个空实现 */
void lcd_delay_ms(uint32_t ms) { (void)ms; }

/* 驱动里定义的自检结果：本测试不编驱动，所以在这里定义 */
uint32_t g_lcd_last_id = 0x009341u;
int      g_lcd_id_ok   = 1;
int  lcd_init(void) { return 0; }              /* 假装屏幕应答了 */
int  lcd_read_id(uint32_t *id) { if (id) { *id = 0x009341u; } return 0; }

/* ---- 触摸桩件：按脚本模拟点击页签 ---- */
static int      s_touch_on = 0;
static uint16_t s_touch_x = 0u, s_touch_y = 0u;

int  touch_init(void) { return 0; }
int  touch_pressed(void) { return s_touch_on; }
/* 原始值桩件：固定回一组可预期的数，供断言用 */
int  touch_read_raw(uint16_t *raw_x, uint16_t *raw_y)
{
    if (!raw_x || !raw_y) { return -1; }
    *raw_x = 1234u;
    *raw_y = 3210u;
    return 0;
}
int  touch_get(uint16_t *x, uint16_t *y)
{
    if (!s_touch_on || !x || !y) { return 0; }
    *x = s_touch_x;
    *y = s_touch_y;
    return 1;
}
void touch_set_calib(uint16_t a, uint16_t b, uint16_t c, uint16_t d,
                     int e, int f, int g)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
}

/* ---- 导出 PPM（P6）---- */
static int dump_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    unsigned i;
    if (!f) { return -1; }
    fprintf(f, "P6\n%u %u\n255\n", FBW, FBH);
    for (i = 0u; i < FBW * FBH; i++) {
        uint16_t c = fb[i];
        unsigned char rgb[3];
        rgb[0] = (unsigned char)(((c >> 11) & 0x1Fu) * 255u / 31u);
        rgb[1] = (unsigned char)(((c >> 5) & 0x3Fu) * 255u / 63u);
        rgb[2] = (unsigned char)((c & 0x1Fu) * 255u / 31u);
        fwrite(rgb, 1u, 3u, f);
    }
    fclose(f);
    printf("  写出 %s\n", path);
    return 0;
}

/* 版式常量（与 gw_ui.c 内部一致；改了那边这里也要跟着改）*/
#define UI_TITLE_H_TEST  20u

/* ---- 点一下某个页签（x 中心，y 在底部页签带里）---- */
static void tap_tab(int page)
{
    uint16_t w = (uint16_t)(FBW / 4u);
    s_touch_on = 1;
    s_touch_x = (uint16_t)(page * w + w / 2u);
    s_touch_y = (uint16_t)(FBH - 10u);
}

int main(int argc, char **argv)
{
    gw_ui_port_t port;
    int page, i;
    /* ⚠️ 默认**不写文件**：CI 从仓库根运行，写图会在根目录留垃圾。
       要看图就 `./t_ui.exe build/ui`，会导出 <前缀>page1..4.ppm。 */
    const char *prefix = (argc > 1) ? argv[1] : 0;

    /* 假现场数据：让它看起来像真在跑 */
    memset(g_in, 0, sizeof(g_in));
    memset(g_hold, 0, sizeof(g_hold));
    g_in[IREG_TEMP_X10]   = 235u;                       /* 23.5 C */
    g_in[IREG_HUMI_X10]   = 612u;                       /* 61.2 %RH */
    for (i = 0; i < 8; i++) { g_in[IREG_CAN_CH1 + i] = (uint16_t)(100u + i * 37u); }
    g_in[IREG_DEV_STATUS] = IREG_STATUS_BIT_DHT_OK      /* DHT 好、CAN 坏（如实显示）*/
                          | IREG_STATUS_BIT_WIFI_OK
                          | IREG_STATUS_BIT_MQTT_OK
                          | IREG_STATUS_BIT_MASTER_OK
                          | IREG_STATUS_BIT_PARAM_OK;
    g_in[IREG_UPTIME_HI]  = 0u;
    g_in[IREG_UPTIME_LO]  = 372u;
    g_in[IREG_MB_RX_LO]   = 1234u;
    g_in[IREG_MB_ERR_LO]  = 3u;
    g_in[IREG_CAN_RX_LO]  = 88u;
    g_in[IREG_MQTT_STATUS] = IREG_MQTT_ST_CONNECTED;
    g_in[IREG_OFFLINE_DEPTH] = 2u;
    g_in[IREG_OFFLINE_DROP]  = 0u;
    g_in[IREG_FW_VER_MAJOR] = 1u;
    g_in[IREG_FW_VER_MINOR] = 0u;
    g_in[IREG_MASTER_ERR] = 0u;
    g_hold[HREG_REPORT_PERIOD] = 10u;
    g_hold[HREG_BAUDRATE] = 3u;

    port.get_iregs = get_iregs;
    port.get_hregs = get_hregs;
    port.set_hreg  = set_hreg;

    if (gw_ui_init(&port) != 0) {
        printf("  [FAIL] gw_ui_init 失败\n");
        return 1;
    }

    /* 逐页渲染：第一次是 OVW；后三页通过"点页签"切过去 */
    for (page = 0; page < 4; page++) {
        char path[64];
        if (page > 0) {
            tap_tab(page);
            for (i = 0; i < 5; i++) { gw_ui_tick(50u); }   /* 稳定 3 拍 -> 触发点击 */
            s_touch_on = 0;                                 /* 松手 */
        }
        for (i = 0; i < 3; i++) { gw_ui_tick(200u); }
        if (prefix != 0) {
            (void)snprintf(path, sizeof(path), "%spage%d.ppm", prefix, page + 1);
            if (dump_ppm(path) != 0) { return 1; }
        }
    }
    /* ---- 断言：不只是画出来，还要**画出预期内容** ---- */
    {
        int pass = 0, failn = 0;

#define CHECK(cond, what) do { \
            if (cond) { pass++; } \
            else { failn++; printf("  [FAIL] %s\n", what); } \
        } while (0)

        /* 帧缓冲不可能是空的（整屏黑 = 什么都没画）*/
        {
            unsigned nz = 0u, k;
            for (k = 0u; k < FBW * FBH; k++) { if (fb[k] != 0u) { nz++; } }
            CHECK(nz > 2000u, "整屏几乎是黑的（界面没画出来）");
        }
        /* 页签高亮必须跟着页面走：当前页的页签区应该是蓝色 */
        {
            int pg;
            for (pg = 0; pg < 4; pg++) {
                (void)pg;   /* 逐页渲染时已切过，最后一页是 CFG */
            }
            CHECK(fb[(FBH - 10u) * FBW + (3u * 80u + 40u)] == LCD_BLUE,
                  "最后一页的页签没有高亮（CFG）");
        }
        /* 标题栏必须是蓝色底 */
        CHECK(fb[2u * FBW + 160u] == LCD_BLUE, "标题栏底色不是蓝的");
        /* 定宽补齐生效：同一格先长后短不该留残字（用画过 CAN 状态行的那一行查）*/
        CHECK(fb[26u * FBW + 200u] == 0u, "CAN 状态行右侧有残字");

        /* ---- A. 残字回归（具体像素）----
           右列第 2 行：先以 HARDFAULT 清零画出 "no hardfault"（12 列），
           再置位 HARDFAULT 重绘成 "HARDFAULT!"（10 列）。
           若没有定宽补齐，旧的第 11~12 列（x=254..261）会留下 "lt" 的墨迹。*/
        {
            /* ⚠️ 版式：UI_TITLE_H=20，行高 UI_ROW_H=18，右列 x=166。
               第 2 行（HARDFAULT 那行）y = 20+6+18 = 44。
               "no hardfault" 12 列，最后一个 't' 在第 11 列 => x=166+88=254。
               字形顶行常为空白，所以要扫整行 16 行看有没有墨迹。*/
            const uint16_t y0 = 44u;
            const uint16_t x0 = 254u;
            int ink_before = 0, ink_after = 0;
            uint16_t dx, dy;

            tap_tab(0);
            for (i = 0; i < 5; i++) { gw_ui_tick(50u); }
            s_touch_on = 0;
            g_in[IREG_DEV_STATUS] &= (uint16_t)~IREG_STATUS_BIT_HARDFAULT;
            gw_ui_invalidate();
            for (i = 0; i < 3; i++) { gw_ui_tick(200u); }
            for (dy = 0u; dy < 16u; dy++) {
                for (dx = 0u; dx < 8u; dx++) {
                    if (fb[(uint32_t)(y0 + dy) * FBW + (x0 + dx)] != 0u) { ink_before++; }
                }
            }
            CHECK(ink_before > 0, "长文本 no hardfault 没画出来（坐标或版式变了？）");

            /* 置位 -> 文本变短（12 -> 10 列）-> 尾巴必须被覆盖成背景 */
            g_in[IREG_DEV_STATUS] |= IREG_STATUS_BIT_HARDFAULT;
            for (i = 0; i < 3; i++) { gw_ui_tick(200u); }
            for (dy = 0u; dy < 16u; dy++) {
                for (dx = 0u; dx < 8u; dx++) {
                    if (fb[(uint32_t)(y0 + dy) * FBW + (x0 + dx)] != 0u) { ink_after++; }
                }
            }
            CHECK(ink_after == 0, "短文本重绘后残留了长文本的尾巴（定宽补齐失效）");
        }

        /* ---- B. 触摸配置路径（逻辑）---- */
        {
            uint16_t before = g_hold[HREG_REPORT_PERIOD];
            tap_tab(3);                                  /* CFG 页 */
            for (i = 0; i < 5; i++) { gw_ui_tick(50u); }
            s_touch_on = 0;
            /* 点 "+"（CFG_BTN_PLUS_X=180, CFG_ROW1_Y=UI_TITLE_H+24=44）*/
            s_touch_on = 1;
            s_touch_x = (uint16_t)(180u + 10u);
            s_touch_y = (uint16_t)(44u + 8u);
            for (i = 0; i < 5; i++) { gw_ui_tick(50u); }
            s_touch_on = 0;
            for (i = 0; i < 3; i++) { gw_ui_tick(200u); }
            CHECK(g_last_idx == (int)HREG_REPORT_PERIOD,
                  "点 + 没有写到上报周期寄存器");
            CHECK(g_last_val == (uint16_t)(before + 1u), "点 + 的值不对");
            CHECK(g_hold[HREG_REPORT_PERIOD] == (uint16_t)(before + 1u),
                  "点 + 之后保持寄存器没变");
        }

        /* ---- B2. 诊断页必须显示触摸原始值（标定要用）---- */
        {
            tap_tab(2);                                  /* DIAG 页 */
            for (i = 0; i < 5; i++) { gw_ui_tick(50u); }
            s_touch_on = 0;
            /* 模拟一次按压，让页面记住原始值 */
            s_touch_on = 1;
            s_touch_x = 100u; s_touch_y = 100u;
            for (i = 0; i < 3; i++) { gw_ui_tick(50u); }
            s_touch_on = 0;
            for (i = 0; i < 3; i++) { gw_ui_tick(200u); }
            /* "TOUCH RAW" 那一行（y=196）的值区 x>=84 应该有墨迹 */
            {
                int ink = 0;
                uint16_t dx, dy;
                for (dy = 0u; dy < 16u; dy++) {
                    for (dx = 0u; dx < 60u; dx++) {
                        if (fb[(uint32_t)(196u + dy) * FBW + (84u + dx)] != 0u) { ink++; }
                    }
                }
                CHECK(ink > 0, "诊断页没有显示触摸原始值（标定会没法做）");
            }
            /* 同一行右列：LCD 自检结果（读 ID 成不成）*/
            {
                int ink2 = 0;
                uint16_t dx, dy;
                for (dy = 0u; dy < 16u; dy++) {
                    for (dx = 0u; dx < 24u; dx++) {
                        if (fb[(uint32_t)(196u + dy) * FBW + (226u + dx)] != 0u) { ink2++; }
                    }
                }
                CHECK(ink2 > 0, "诊断页没有显示 LCD 自检结果（LCD ID）");
            }
        }

        /* ---- C. 降级态：DHT 失效也要画出内容 ---- */
        {
            unsigned nz = 0u, k;
            g_in[IREG_DEV_STATUS] &= (uint16_t)~IREG_STATUS_BIT_DHT_OK;
            gw_ui_invalidate();
            for (i = 0; i < 3; i++) { gw_ui_tick(200u); }
            for (k = 0u; k < FBW * FBH; k++) { if (fb[k] != 0u) { nz++; } }
            CHECK(nz > 2000u, "DHT 失效时界面几乎是空的");
        }

        printf("通过 %d 项, 失败 %d 项  --> %s\n", pass, failn,
               (failn == 0) ? "ALL PASS" : "存在失败项");
        return (failn == 0) ? 0 : 1;
#undef CHECK
    }
}
