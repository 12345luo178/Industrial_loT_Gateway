#if defined(GW_USE_LVGL)
/* 本文件只在打开 -Lvgl 构建开关时参与编译（见 tools/build_ota_images.ps1）。
   关掉开关时它是空文件 —— 保证现有构建与浸泡基线一个字节都不变。 */

/* ==================================================================
 *  LVGL 移植层实现（见 lvgl_port.h 的说明）
 * ================================================================== */
#include "lvgl.h"

#include "lcd_ili9341.h"
#include "lvgl_port.h"
#include "gw_dwt.h"             /* 整帧刷新耗时统计（诊断）*/
#include "touch_xpt2046.h"      /* XPT2046 坐标已由驱动按上板标定常数转换 */

volatile uint32_t g_lvgl_flush_calls = 0u;
volatile uint32_t g_lvgl_flush_px    = 0u;

volatile uint32_t g_lvgl_frame_us_max  = 0u;
volatile uint32_t g_lvgl_frame_flushes = 0u;

/* 触摸诊断：让板上可通过 RAM 取证区分"没读到触摸"与"点击事件没命中"。 */
volatile uint32_t g_lvgl_touch_init_ok = 0u;
volatile uint32_t g_lvgl_touch_reads   = 0u;
volatile uint32_t g_lvgl_touch_x       = 0u;
volatile uint32_t g_lvgl_touch_y       = 0u;

/* ===== 2026-10-16 新增：把两种"点不动"分开（见 touch_xpt2046.h 的说明） =====
 *
 * ⚠️ 为什么 g_lvgl_touch_init_ok **不能**当判据：
 *    touch_init() 只配置 T_CS/T_IRQ 两个引脚就无条件 return 0，
 *    **全程不与 XPT2046 通信** ⇒ 它恒为 1，与"触摸能不能用"毫无关系。
 *    2026-10-16 的排查就差点被这个假绿灯带偏，所以这里另立一个：
 *      g_lvgl_touch_probe_ok = 1 表示**确实成功读到过一次坐标**（真凭据）。
 */
volatile uint32_t g_lvgl_touch_probe_ok = 0u;

static lv_disp_draw_buf_t s_draw;
static lv_disp_drv_t      s_disp;
static lv_indev_drv_t     s_indev_drv;
static lv_indev_t        *s_indev;
static lv_point_t         s_last_touch;
static uint8_t            s_have_last_touch;

/* 绘制缓冲：320 x 10 像素 = 6400 字节。
   ⚠️ 这是"极限瘦身"的关键取舍之一 —— LVGL 官方推荐 1/10 屏(15KB)，
      我们给 1/24 屏(6.4KB)，代价是整屏刷新要分 24 次，收益是 RAM 省下 8.6KB。 */
static lv_color_t s_buf[LVGL_PORT_BUF_PX];

/**
 * 刷新回调：LVGL 把一个矩形区域的像素交给我们，我们直接搬进 ILI9341
 *
 * ⚠️ 这里**只调用 lcd_blit()**，不自己做任何 SPI/CS 操作 ——
 *    因为"像素流中途抬 CS 会丢数据"那个坑（见 lcd_ili9341.c 的注释）
 *    已经在 lcd_blit() 里处理好了，这里保持单一路径。
 */
static void disp_flush(lv_disp_drv_t *drv, const lv_area_t *area,
                       lv_color_t *px_map)
{
    uint16_t w = (uint16_t)(area->x2 - area->x1 + 1);
    uint16_t h = (uint16_t)(area->y2 - area->y1 + 1);

    lcd_blit((uint16_t)area->x1, (uint16_t)area->y1, w, h,
             (const uint16_t *)px_map);

    g_lvgl_flush_calls++;
    g_lvgl_flush_px += (uint32_t)w * h;

    lv_disp_flush_ready(drv);          /* ⚠️ 必须调，否则 LVGL 不会发下一块 */
}

static void indev_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    uint16_t x, y;
    (void)drv;
    data->continue_reading = false;

    if (touch_pressed()) {
        if (touch_get(&x, &y)) {
            s_last_touch.x = (lv_coord_t)x;
            s_last_touch.y = (lv_coord_t)y;
            s_have_last_touch = 1u;
            g_lvgl_touch_x = x;
            g_lvgl_touch_y = y;
            g_lvgl_touch_reads++;
            g_lvgl_touch_probe_ok = 1u;   /* ★ 真凭据：确实读出过一次坐标 */
        }

        /* 电阻屏偶发的单次采样噪声不应打断一次按压：若本次无效，
           沿用本次按压期间最近一次可信坐标；首次采样失败则暂不报按下。 */
        if (s_have_last_touch != 0u) {
            data->point = s_last_touch;
            data->state = LV_INDEV_STATE_PRESSED;
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        s_have_last_touch = 0u;
    }
}

int lvgl_port_init(void)
{
    lv_init();

    /* LVGL 这条显示路径原先没有调用 touch_init()，因此 PB6/PB7 没有配置；
       自绘路径在 gw_ui_init() 里会做，但生产模板在 -Lvgl 下不会走那里。 */
    g_lvgl_touch_init_ok = (touch_init() == 0) ? 1u : 0u;

    /* 单缓冲（第二个参数传 NULL）—— 省一半 RAM；
       代价是"画的时候不能同时刷"，对静态页没有影响。 */
    lv_disp_draw_buf_init(&s_draw, s_buf, 0, LVGL_PORT_BUF_PX);

    lv_disp_drv_init(&s_disp);
    s_disp.hor_res  = LCD_W;
    s_disp.ver_res  = LCD_H;
    s_disp.flush_cb = disp_flush;
    s_disp.draw_buf = &s_draw;
    (void)lv_disp_drv_register(&s_disp);

    /* 注册触摸屏为 LVGL pointer 输入设备。读回调由现有 UI 任务里的
       lv_timer_handler() 调度，不新增任务，也不会与 LCD 刷屏并发。 */
    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type    = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = indev_read;
    s_indev = lv_indev_drv_register(&s_indev_drv);
    if (s_indev == 0) {
        return -1;
    }

    /* ⚠️⚠️ 首次清屏：对齐自绘版 `gw_ui_init()` 里的 `lcd_fill(UI_BG)`。
       自绘版有这一步，LVGL 版此前漏了 —— 于是"LVGL 首帧还没画"的那段时间
       屏上是面板 GRAM 的上电残留，实机表现就是**整屏蓝**（2026-10-14 照片）。
       注意：这不替代 screen 的 bg_opa 设置，而是补上"最坏情况下的兜底" ——
       即便 LVGL 因某种原因不把 screen 背景算进刷新区域，屏底也是黑的。 */
    lcd_fill(0x0000u);

    return 0;
}

void lvgl_port_tick(uint32_t elapsed_ms)
{
    if (elapsed_ms > 0u) {
        lv_tick_inc(elapsed_ms);
    }
}

void lvgl_port_handler(void)
{
    uint32_t before = g_lvgl_flush_calls;
    /* ⚠️⚠️ 基线必须取**原始周期数**，不能取 gw_dwt_us()。
       原因：gw_dwt_us() 已经除了 72，回绕周期是 59,652,323 µs（不是 2 的幂），
       对它做无符号减法**没有**回绕保护 —— 跨回绕点会得到约 42.9 亿的假值。
       2026-10-14 实机症状：标题栏耗时显示 `4235335ms`（板子只跑了 1583 s）。 */
    uint32_t t0     = gw_dwt_cycles();
    uint32_t dt;

    (void)lv_timer_handler();

    dt = gw_dwt_elapsed_us(t0);

    /* ⚠️ 只有"本帧真的往外发了像素"才更新统计 —— 否则空转的 handler
       （没东西要画）会把"最坏一帧"冲成 0，读不出真实刷新开销。
       整屏刷新 = 24 块（240 行 / 缓冲 10 行），局部刷新远小于此。 */
    if (g_lvgl_flush_calls != before) {
        g_lvgl_frame_flushes = g_lvgl_flush_calls - before;
        /* ⚠️ 兜底上限 1 s：正常一帧渲染是几十毫秒量级（整屏 ~285ms），
           不可能到 1 秒。真出现异常值就丢弃，避免把"最坏帧"记成天文数字
           —— 这是对回绕保护之外的第二道防线（例如调试器把 CPU 停了很久）。 */
        if (dt <= 1000000u && dt > g_lvgl_frame_us_max) {
            g_lvgl_frame_us_max = dt;
        }
    }
}

#endif /* GW_USE_LVGL */
