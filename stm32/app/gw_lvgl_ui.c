#if defined(GW_USE_LVGL)
/* 本文件只在打开 -Lvgl 构建开关时参与编译（见 tools/build_ota_images.ps1）。
   关掉开关时它是空文件 —— 保证现有构建与浸泡基线一个字节都不变。 */

/* ==================================================================
 *  第一个 LVGL 页面（内容对齐现有自绘 OVERVIEW 页，便于左右对比）
 *
 *  目的：验证"LVGL 在 F103 上能不能跑、RAM/Flash 够不够、刷新路径通不通"，
 *        所以刻意做成与 gw_ui.c 的 OVERVIEW 页同样的元素：
 *          标题栏 · TEMP/HUMI/CAN/MQTT/UPTIME 五行 · 两个 RANGE 条 · 4 个页签
 *        并且**故意用中文标签**（走 lv_font_cn_16 子集字库）验证中文显示。
 *
 *  ⚠️ 本文件与 app/gw_ui.c 是**两条独立的显示路径**，互不干扰：
 *     先在 LVGL 这条路上把显示/内存/帧率验证清楚，再决定是否替换自绘界面。
 *
 *  ⚠️ 中文文案一旦增删，必须重跑 `python tools/gen_lvgl_font.py` 重新生成
 *     子集字库 —— 漏字的表现是"该字位置画空白"，**不报错、不崩溃**，极易漏看。
 *     脚本的 `--check` 模式可放进 CI 当护栏。
 * ================================================================== */
#include "lvgl.h"

#include "gw_lvgl_ui.h"
#include "lvgl_port.h"          /* g_lvgl_frame_us_max / _flushes：刷新耗时诊断 */
#include "modbus_regs.h"        /* IREG_* 与 IREG_STATUS_BIT_* */

LV_FONT_DECLARE(lv_font_cn_16);

static lv_obj_t *s_title;
static lv_obj_t *s_perf;        /* 标题栏右侧：上一帧刷新耗时（诊断用）*/
static lv_obj_t *s_temp;
static lv_obj_t *s_humi;
static lv_obj_t *s_can;
static lv_obj_t *s_mqtt;
static lv_obj_t *s_uptime;
static lv_obj_t *s_tabs[4];
/* 进度条（对应自绘版的 RANGE 条）：**轨道 + 填充两个纯矩形**，刻意不用 lv_bar。
 *
 * ⚠️⚠️ 为什么不用 lv_bar（2026-10-14 实机踩到，症状是"两条进度条都占满了"）：
 *   `src/widgets/lv_bar.c:481-488` 里，指示器填充画的是**整条 `mask_indic_max_area`**：
 *       lv_draw_mask_radius_init(&mask_indic_param, &bar->indic_area, ...);
 *       lv_draw_rect(draw_ctx, &draw_rect_dsc, &mask_indic_max_area);   // 整条！
 *   "只填一部分"完全靠那个 **mask** 实现 —— 而它整段在 `#if LV_DRAW_COMPLEX` 里。
 *   本工程为省 Flash 把 `LV_DRAW_COMPLEX` 关成 0（见 port/lv_conf.h），
 *   于是 mask 被编译掉、填充直接铺满整条 ⇒ 实机就是"进度条占满了"。
 *   （`cur_value` 一直是对的：RAM 取证读回 56 / 61，纯属**绘制**问题。）
 *   换成两个矩形后：视觉完全一致（本来就是 radius=0 的纯色块），
 *   不再依赖 mask，`lv_bar.c` 整块还能被 --gc-sections 回收。
 *   ⇒ 教训：`LV_DRAW_COMPLEX 0` 会**悄悄改变控件的绘制语义**，不只是"少个圆角"。 */
#define GW_BAR_W   300          /* 轨道宽（px），与自绘版一致 */
#define GW_BAR_H   14           /* 轨道高（px） */
static lv_obj_t *s_bar[2];      /* 轨道（深灰底） */
static lv_obj_t *s_bar_fill[2]; /* 填充（青色），宽度 = 百分比 */
static lv_obj_t *s_overview;
static lv_obj_t *s_page_info;
static uint8_t  s_page;
static uint8_t  s_page_info_dirty = 1u;
static uint32_t s_page_info_uptime = 0xFFFFFFFFu;
volatile uint32_t g_lvgl_ui_page = 0u;

/* ---- "文本没变就不 set"的缓存 ----
   ⚠️ 初值一律取**不可能出现的哨兵值**，保证第一次 update 一定会写一次。
   为什么需要：LVGL 的 label 是脏矩形刷新，但 `set_text` 同值仍会把对象标脏；
   UI 任务 50ms 一拍，若每拍都无脑 set，温度/时间没变时也在白白重绘。 */
static int16_t  s_shown_temp   = INT16_MIN;
static uint16_t s_shown_humi   = 0xFFFFu;
static uint32_t s_shown_uptime = 0xFFFFFFFFu;
static int      s_shown_dht    = -1;      /* DHT 是否可信（决定显示数值还是 --.-）*/
static int      s_shown_can    = -1;
static int      s_shown_mqtt   = -1;
static uint8_t  s_shown_bar[2] = { 0xFFu, 0xFFu };
static uint32_t s_perf_uptime  = 0xFFFFFFFFu;   /* 耗时标签的 1 秒节流 */

/* ---- 背景诊断（2026-10-14 加，纯诊断、可删）----
   实机出现过"整屏蓝底"，需要区分是"样式没设上"还是"设上了但渲染没走这条路"。
   在 build 末尾读回 screen 的 bg_opa 存这里，用 RAM 取证（-u 0x20000000 0x10000）读：
     255 = 样式已生效；0 = 样式没设上。 */
volatile uint32_t g_dbg_scr_opa = 0xFFFFFFFFu;

static void set_page(uint8_t page)
{
    static const char * const title[4] = {
        "网关 / 总览", "CAN / BUS", "DIAGNOSTICS", "CONFIGURATION"
    };
    int i;

    if (page >= 4u || page == s_page) {
        return;
    }

    s_page = page;
    g_lvgl_ui_page = page;             /* 板上 RAM 取证：0=OVW, 1=CAN, 2=DIAG, 3=CFG */
    s_page_info_dirty = 1u;
    (void)lv_label_set_text(s_title, title[page]);

    if (page == 0u) {
        lv_obj_clear_flag(s_overview, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_page_info, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_perf, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_overview, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_page_info, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_perf, LV_OBJ_FLAG_HIDDEN);
    }

    for (i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(s_tabs[i],
            (i == (int)page) ? lv_color_make(0x00, 0x40, 0xC0)
                             : lv_color_make(0x30, 0x30, 0x30), 0);
    }
}

static void tab_event_cb(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target(e);
    int i;

    for (i = 0; i < 4; i++) {
        if (target == s_tabs[i]) {
            set_page((uint8_t)i);
            break;
        }
    }
}

void gw_lvgl_ui_build(void)
{
    lv_obj_t *scr = lv_scr_act();
    static const char *tab_txt[4] = { "OVW", "CAN", "DIAG", "CFG" };
    int i;

    /* ⚠️ 先摘掉主题给 screen 铺的那层背景样式，再自己设黑底。
       （LV_USE_THEME_BASIC 仍是 1：btn/bar 的默认观感还靠它，只是不让它管 screen。）*/
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* ⚠️⚠️ 2026-10-14 补：**display 级背景**才是"屏幕最底层"，必须一起设。
       看 LVGL v8 的 lv_refr.c，每块条带渲染前先判断"有没有不透明顶层对象覆盖"：
         · screen 的 bg_opa == COVER  ⇒ screen 自己就是那个顶层对象 ⇒ 画 screen 背景；
         · screen 的 bg_opa == TRANSP  ⇒ 判定为"没有" ⇒ 回落到画 disp 背景
           （dsc.bg_color = disp->bg_color，dsc.bg_opa = disp->bg_opa）。
       两条路都要通向黑底，所以 disp 背景也显式设成黑 + COVER。

       实机证据（2026-10-14 照片）：只设 screen 背景时整屏是**蓝色**，
       连 bar(320x20 深蓝) 和两条 300x14 进度条都看不见 —— 那是面板 GRAM 的
       上电残留：LVGL 判定"这块不用画"就根本不 flush，屏上留着旧内容。 */
    {
        lv_disp_t *d = lv_disp_get_default();
        if (d != 0) {
            lv_disp_set_bg_color(d, lv_color_black());
            lv_disp_set_bg_opa(d, LV_OPA_COVER);
        }
    }

    /* ---- 标题栏：蓝底白字（与自绘版一致）---- */
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, LCD_W, 20);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_make(0x00, 0x00, 0xC0), 0);
    /* ⚠️ 只设 bg_color 不设 bg_opa ⇒ 默认 LV_OPA_TRANSP ⇒ 背景透明、看不见。
       无 theme 时 LVGL 不会替我们补默认不透明度，**每个要底色的对象都要显式设**。
       （2026-10-14 照片实证：标题栏和两条进度条全都"隐形"了。）*/
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(bar);
    lv_obj_set_style_text_font(s_title, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(s_title, lv_color_white(), 0);
    lv_label_set_text(s_title, "网关 / 总览");
    lv_obj_align(s_title, LV_ALIGN_LEFT_MID, 4, 0);

    /* 标题栏右侧：刷新耗时（"能不能做动效"的判据）。纯 ASCII，不占字库。*/
    s_perf = lv_label_create(bar);
    lv_obj_set_style_text_font(s_perf, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(s_perf, lv_color_make(0xC0, 0xE0, 0xFF), 0);
    lv_label_set_text(s_perf, "--");
    lv_obj_align(s_perf, LV_ALIGN_RIGHT_MID, -4, 0);

    /* ---- 内容区：总览控件作为一组隐藏/显示，触摸切页时不销毁对象 ---- */
    s_overview = lv_obj_create(scr);
    lv_obj_remove_style_all(s_overview);
    lv_obj_set_pos(s_overview, 0, 20);
    lv_obj_set_size(s_overview, LCD_W, 194);
    lv_obj_set_style_bg_opa(s_overview, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_overview, 0, 0);
    lv_obj_set_style_pad_all(s_overview, 0, 0);
    lv_obj_clear_flag(s_overview, LV_OBJ_FLAG_SCROLLABLE);

    s_page_info = lv_label_create(scr);
    lv_obj_set_style_text_font(s_page_info, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(s_page_info, lv_color_white(), 0);
    lv_obj_set_pos(s_page_info, 10, 34);
    lv_obj_set_size(s_page_info, LCD_W - 20, 170);
    lv_label_set_text(s_page_info, "Touch a tab to switch page");
    lv_obj_add_flag(s_page_info, LV_OBJ_FLAG_HIDDEN);

    /* ---- 五行数据（左列标签用中文，右列数值用 ASCII 大字库）---- */
    s_temp = lv_label_create(s_overview);
    lv_obj_set_style_text_font(s_temp, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(s_temp, lv_color_white(), 0);
    lv_obj_set_pos(s_temp, 4, 4);
    lv_label_set_text(s_temp, "温度  --.- C");

    s_humi = lv_label_create(s_overview);
    lv_obj_set_style_text_font(s_humi, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(s_humi, lv_color_white(), 0);
    lv_obj_set_pos(s_humi, 4, 24);
    lv_label_set_text(s_humi, "湿度  --.- %");

    s_can = lv_label_create(s_overview);
    lv_obj_set_style_text_font(s_can, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(s_can, lv_color_white(), 0);
    lv_obj_set_pos(s_can, 4, 44);
    lv_label_set_text(s_can, "CAN   BUS OK");

    s_mqtt = lv_label_create(s_overview);
    lv_obj_set_style_text_font(s_mqtt, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(s_mqtt, lv_color_white(), 0);
    lv_obj_set_pos(s_mqtt, 4, 64);
    lv_label_set_text(s_mqtt, "上云  离线");

    s_uptime = lv_label_create(s_overview);
    lv_obj_set_style_text_font(s_uptime, &lv_font_cn_16, 0);
    lv_obj_set_style_text_color(s_uptime, lv_color_white(), 0);
    lv_obj_set_pos(s_uptime, 4, 84);
    lv_label_set_text(s_uptime, "时间  0 s");

    /* ---- 两个进度条（对应自绘版的 RANGE 条）----
       ⚠️ 用「轨道 + 填充」两个纯矩形，**不用 lv_bar** —— 理由见 s_bar 的声明处。 */
    for (i = 0; i < 2; i++) {
        /* 轨道 */
        s_bar[i] = lv_obj_create(s_overview);
        lv_obj_remove_style_all(s_bar[i]);      /* 去掉默认 pad/border/圆角 -> 纯矩形 */
        lv_obj_set_size(s_bar[i], GW_BAR_W, GW_BAR_H);
        lv_obj_set_pos(s_bar[i], 10, 116 + i * 24);
        lv_obj_set_style_radius(s_bar[i], 0, 0);
        lv_obj_set_style_border_width(s_bar[i], 0, 0);
        lv_obj_set_style_pad_all(s_bar[i], 0, 0);
        lv_obj_set_style_bg_color(s_bar[i], lv_color_make(0x20, 0x20, 0x20), 0);
        /* ⚠️ 必须显式设 bg_opa：`lv_style_prop_get_default()` 里 BG_OPA 默认
           LV_OPA_TRANSP（见 port/lv_conf.h），只设 bg_color 会整个看不见。 */
        lv_obj_set_style_bg_opa(s_bar[i], LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_bar[i], LV_OBJ_FLAG_SCROLLABLE);

        /* 填充：挂在轨道下，x 从 0 起，宽度由 gw_lvgl_ui_set_bar() 按百分比设 */
        s_bar_fill[i] = lv_obj_create(s_bar[i]);
        lv_obj_remove_style_all(s_bar_fill[i]);
        lv_obj_set_size(s_bar_fill[i], 0, GW_BAR_H);
        lv_obj_set_pos(s_bar_fill[i], 0, 0);
        lv_obj_set_style_radius(s_bar_fill[i], 0, 0);
        lv_obj_set_style_border_width(s_bar_fill[i], 0, 0);
        lv_obj_set_style_pad_all(s_bar_fill[i], 0, 0);
        lv_obj_set_style_bg_color(s_bar_fill[i], lv_color_make(0x00, 0xC0, 0xE0), 0);
        lv_obj_set_style_bg_opa(s_bar_fill[i], LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_bar_fill[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    /* ---- 4 个页签按钮 ---- */
    for (i = 0; i < 4; i++) {
        s_tabs[i] = lv_btn_create(scr);
        lv_obj_set_size(s_tabs[i], 76, 22);
        lv_obj_set_pos(s_tabs[i], 4 + i * 79, LCD_H - 24);
        lv_obj_set_style_radius(s_tabs[i], 0, 0);
        lv_obj_set_style_bg_color(s_tabs[i], (i == 0) ? lv_color_make(0x00, 0x40, 0xC0)
                                                      : lv_color_make(0x30, 0x30, 0x30), 0);
        lv_obj_set_style_bg_opa(s_tabs[i], LV_OPA_COVER, 0);   /* ⚠️ 补（同 bar）*/
        lv_obj_add_event_cb(s_tabs[i], tab_event_cb, LV_EVENT_CLICKED, 0);

        lv_obj_t *l = lv_label_create(s_tabs[i]);
        lv_label_set_text(l, tab_txt[i]);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_obj_center(l);
    }

    /* 诊断：读回 screen 实际生效的 bg_opa（255=样式生效；0=没设上）。
       实机出现"整屏蓝底"时，用它区分"样式没设上"和"设上了但渲染没走这条路"。
       RAM 取证：-u 0x20000000 0x10000 ram.bin 后在 .bss 里按地址找 g_dbg_scr_opa。 */
    g_dbg_scr_opa = (uint32_t)lv_obj_get_style_bg_opa(scr, LV_PART_MAIN);
}

/* ==================================================================
 *  从寄存器表刷新界面
 *
 *  ⚠️ 只**读** `g_regs.input`，与自绘界面 gw_ui 读的是同一份快照
 *     （那份快照由 HOUSE 任务周期刷新）—— 所以两条显示路径看到的数据
 *     天然一致，这正是"左右对比"能成立的前提。
 * ================================================================== */
void gw_lvgl_ui_update(const uint16_t *ir)
{
    uint16_t st;
    int      dht_ok;
    uint32_t up;

    if (ir == 0) {
        return;
    }

    st     = ir[IREG_DEV_STATUS];
    dht_ok = (st & IREG_STATUS_BIT_DHT_OK) != 0u;

    if (!dht_ok) {
        /* ---- DHT 校验没过：显示 "--.-"（与自绘版一致：不可信就不显示数）----
           同时把数值缓存复位，等 DHT 恢复时强制重画一次。 */
        if (s_shown_dht != 0) {
            s_shown_dht    = 0;
            s_shown_temp   = INT16_MIN;
            s_shown_humi   = 0xFFFFu;
            s_shown_bar[0] = 0xFFu;
            s_shown_bar[1] = 0xFFu;
            (void)lv_label_set_text(s_temp, "温度  --.- C");
            (void)lv_label_set_text(s_humi, "湿度  --.- %");
            gw_lvgl_ui_set_bar(0, 0u);
            gw_lvgl_ui_set_bar(1, 0u);
        }
    } else {
        int16_t  t = (int16_t)ir[IREG_TEMP_X10];
        uint16_t h = ir[IREG_HUMI_X10];
        uint8_t  pt, ph;

        s_shown_dht = 1;

        if (t != s_shown_temp) {
            s_shown_temp = t;
            gw_lvgl_ui_set_temp(t);
        }
        if (h != s_shown_humi) {
            s_shown_humi = h;
            gw_lvgl_ui_set_humi(h);
        }

        /* 量程与自绘版 gw_ui 的 RANGE 条一致：温度 0..50.0C、湿度 0..100.0% */
        pt = (t <= 0) ? 0u : ((t >= 500) ? 100u : (uint8_t)(t / 5));
        ph = (h >= 1000u) ? 100u : (uint8_t)(h / 10u);
        if (pt != s_shown_bar[0]) {
            s_shown_bar[0] = pt;
            gw_lvgl_ui_set_bar(0, pt);
        }
        if (ph != s_shown_bar[1]) {
            s_shown_bar[1] = ph;
            gw_lvgl_ui_set_bar(1, ph);
        }
    }

    /* ---- 运行时间（32 位，高字在前，见 modbus_regs.h 的约定）---- */
    up = ((uint32_t)ir[IREG_UPTIME_HI] << 16) | (uint32_t)ir[IREG_UPTIME_LO];
    if (up != s_shown_uptime) {
        s_shown_uptime = up;
        gw_lvgl_ui_set_uptime(up);
    }

    /* ---- CAN / 上云状态位 ---- */
    {
        int can  = (st & IREG_STATUS_BIT_CAN_OK)  != 0u;
        int mqtt = (st & IREG_STATUS_BIT_MQTT_OK) != 0u;
        if (can != s_shown_can) {
            s_shown_can = can;
            gw_lvgl_ui_set_can(can);
        }
        if (mqtt != s_shown_mqtt) {
            s_shown_mqtt = mqtt;
            gw_lvgl_ui_set_mqtt(mqtt);
        }
    }

    /* ---- 诊断：把"最坏一帧的刷新耗时"显示到标题栏右侧 ----
       ⚠️ 这是"LVGL 能不能做动效"的判据：整屏刷新若 > 100ms，动效会明显卡顿，
          而且会挤占 UI 任务的 50ms 周期。用 uptime 秒做节流（1 秒更新一次），
          避免每 50ms 都重绘这行字。 */
    if (up != s_perf_uptime) {
        s_perf_uptime = up;
        (void)lv_label_set_text_fmt(s_perf, "%lums x%lu",
                                    (unsigned long)(g_lvgl_frame_us_max / 1000u),
                                    (unsigned long)g_lvgl_frame_flushes);
    }

    /* 非总览页共用一个轻量信息面板，最多每秒刷新一次；点击切页时立即刷新。
       只用 ASCII，避免扩大精简中文子集字库。 */
    if (s_page != 0u
        && (s_page_info_dirty != 0u || up != s_page_info_uptime)) {
        uint32_t can_rx = ((uint32_t)ir[IREG_CAN_RX_HI] << 16)
                        | (uint32_t)ir[IREG_CAN_RX_LO];
        s_page_info_uptime = up;
        s_page_info_dirty = 0u;

        if (s_page == 1u) {
            (void)lv_label_set_text_fmt(s_page_info,
                "CAN BUS\nState: %s\nRX frames: %lu\nCH1: %u  CH2: %u\nCH3: %u  CH4: %u",
                (st & IREG_STATUS_BIT_CAN_OK) ? "ONLINE" : "NO BUS",
                (unsigned long)can_rx,
                (unsigned int)ir[IREG_CAN_CH1], (unsigned int)ir[IREG_CAN_CH2],
                (unsigned int)ir[IREG_CAN_CH3], (unsigned int)ir[IREG_CAN_CH4]);
        } else if (s_page == 2u) {
            (void)lv_label_set_text_fmt(s_page_info,
                "DIAGNOSTICS\nDHT: %s\nCAN: %s\nWIFI: %s\nMQTT: %s\nUPTIME: %lu s",
                (st & IREG_STATUS_BIT_DHT_OK) ? "OK" : "NO DATA",
                (st & IREG_STATUS_BIT_CAN_OK) ? "OK" : "NO BUS",
                (st & IREG_STATUS_BIT_WIFI_OK) ? "ONLINE" : "OFFLINE",
                (st & IREG_STATUS_BIT_MQTT_OK) ? "ONLINE" : "OFFLINE",
                (unsigned long)up);
        } else {
            (void)lv_label_set_text_fmt(s_page_info,
                "CONFIGURATION\nFW VERSION: %u.%u\nTOUCH: %s\nSCREEN: 320 x 240\nSETTINGS: MODBUS",
                (unsigned int)ir[IREG_FW_VER_MAJOR],
                (unsigned int)ir[IREG_FW_VER_MINOR],
                g_lvgl_touch_init_ok ? "READY" : "INIT FAIL");
        }
    }
}

void gw_lvgl_ui_set_temp(int16_t x10)
{
    (void)lv_label_set_text_fmt(s_temp, "温度  %d.%d C",
                                x10 / 10, (x10 < 0 ? -x10 : x10) % 10);
}

void gw_lvgl_ui_set_humi(uint16_t x10)
{
    (void)lv_label_set_text_fmt(s_humi, "湿度  %u.%u %%", x10 / 10u, x10 % 10u);
}

void gw_lvgl_ui_set_uptime(uint32_t s)
{
    (void)lv_label_set_text_fmt(s_uptime, "时间  %lu s", (unsigned long)s);
}

void gw_lvgl_ui_set_can(int ok)
{
    (void)lv_label_set_text(s_can, ok ? "CAN   总线正常" : "CAN   无总线");
}

void gw_lvgl_ui_set_mqtt(int ok)
{
    (void)lv_label_set_text(s_mqtt, ok ? "上云  在线" : "上云  离线");
}

void gw_lvgl_ui_set_bar(uint8_t idx, uint8_t pct)
{
    if (idx < 2u) {
        uint32_t w;

        if (pct > 100u) {
            pct = 100u;
        }
        /* 按像素定宽：300 * 56% = 168 px。
           ⚠️ 不能用 lv_bar_set_value —— 那正是被 LV_DRAW_COMPLEX 0 打坏的通路，
              见 s_bar 的声明处。 */
        w = ((uint32_t)GW_BAR_W * (uint32_t)pct) / 100u;
        lv_obj_set_width(s_bar_fill[idx], (lv_coord_t)w);
    }
}

#endif /* GW_USE_LVGL */
