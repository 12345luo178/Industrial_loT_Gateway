/* ==================================================================
 *  本地 UI 实现 —— 深色底 + 蓝色标题栏 + 彩色状态，4 页
 *
 *  设计约束（与 port/lcd_ili9341.h 一致）：
 *    · 不用 LVGL、不做全屏缓冲 —— 这是 64KB RAM / 512KB flash 的 F103ZET6，
 *      而 app 槽只有 136KB。驱动只给"矩形填充 + 8x16 文本"这类最小图元，
 *      页面逻辑在这里用它们拼。
 *    · 本任务（HOUSE）栈只有 256 字 = 1KB —— 所有缓冲都是小的定长数组，
 *      绝不 malloc、绝不放大栈占用。
 *    · 脏区备忘（s_key/s_tag/s_cell_fg/s_lbl_memo/s_dot_*）是**防闪烁的关键**：
 *      同位置同内容的文本/色点不重画，否则 200ms 一轮会把整屏反复写一遍。
 *      文字一律"定宽补齐"（ui_pad），所以同一格先长后短不会留残字。
 *    · ⚠️ 标题栏底色与页签高亮**必须是 LCD_BLUE**：test/test_ui_render.c
 *      以这两个像素为锚点断言（"标题栏底色不是蓝的" / "页签没有高亮"）。
 *    · ⚠️ 诊断页必须显示 LCD 自检结果（g_lcd_id_ok / g_lcd_last_id），
 *      同一测试也断言了它（"诊断页没有显示 LCD 自检结果"）。
 *
 *  配色全部是编译期常量，**不占 RAM**；lcd_text 本身带 fg/bg 两个参数，
 *  所以上色不需要新增任何显示基础设施。
 * ================================================================== */
#include "stm32f1xx_hal.h"
#include "gw_config.h"
#include "lcd_ili9341.h"
#include "touch_xpt2046.h"
#include "gw_ui.h"
#include "modbus_regs.h"

#include <stdio.h>
#include <string.h>

/* ================= 配色 ================= */

#define UI_BG        LCD_BLACK                       /* 正文区底色 */
#define UI_BLUE      LCD_BLUE                        /* 标题栏 / 页签高亮（测试锚点）*/
#define UI_ACCENT    LCD_RGB565(90u, 190u, 255u)     /* 浅蓝：强调条 / 按钮边框 / 通道号 */
#define UI_PANEL     LCD_RGB565(18u, 22u, 30u)       /* 页签栏底色 */
#define UI_BTN_BG    LCD_RGB565(28u, 56u, 104u)      /* 按钮填充 */
#define UI_DIM       LCD_RGB565(148u, 158u, 176u)    /* 次要标签：淡蓝灰
                                                      * ⚠️ 2026-10-13：原来是 LCD_GRAY(0x8410)，
                                                      * 配合当时 lcd_text 的字节序 bug（见缺陷 AJ）
                                                      * 实际渲染成近乎全黑 ⇒ 标签"看不清"。
                                                      * 根因修掉后中灰已可读，这里再调亮一档，
                                                      * 让 TEMP/HUMI/... 与页签未选中项更清楚。 */
#define UI_LINE      LCD_DARKGRAY                    /* 分隔线 */
#define UI_FG        LCD_WHITE                       /* 正文 */
#define UI_VAL       LCD_CYAN                        /* 主数值 */
#define UI_OK        LCD_GREEN
#define UI_WARN      LCD_ORANGE
#define UI_ERR       LCD_RED
#define UI_OFF       LCD_DARKGRAY                    /* 未启用 / 无数据 */
#define UI_BAR_BG    LCD_RGB565(28u, 32u, 38u)       /* 量程条未填充部分 */

/* ================= 版式（320x240 横屏） ================= */

#define UI_TITLE_H      20u
#define UI_TAB_H        22u
#define UI_TAB_Y        (LCD_H - UI_TAB_H)
#define UI_ROW_H        18u
#define UI_COL_X        8u
#define UI_VAL_X        96u
#define UI_COL2_X       172u

#define UI_REDRAW_MS    200u
#define UI_TAP_TICKS    3u

typedef enum { PAGE_OVW = 0, PAGE_CAN, PAGE_DIAG, PAGE_CFG, PAGE_COUNT } ui_page_t;

static const char *const s_tab_name[PAGE_COUNT] = { "OVW", "CAN", "DIAG", "CFG" };

static gw_ui_port_t s_port;

__attribute__((weak)) void gw_ui_watchdog_feed(void) { }
static int s_touch_ok = 1;
static ui_page_t    s_page   = PAGE_OVW;
static int          s_ready  = 0;
static uint32_t     s_acc_ms = 0u;
static int          s_force  = 1;

/* ================= 调试开关：触摸 / 重绘诊断（默认关闭） =================
 *
 *  ⚠️ 2026-10-13：触摸标定已完成、缺陷 AH/AI/AJ 均已修，本开关**已置 0**（出厂状态）。
 *     这里选择**保留**这套诊断代码而不是删除 —— 换屏 / 换板要重新标定时，把它置 1
 *     就能立刻拿到原始值与重绘计数，而 `#if 0` 下不占 Flash 也不占 RAM，成本为零。
 *
 *  什么时候需要把它打开：
 *    · 触摸点位与画面不符   -> 看 `g_ui_tx_raw/ty_raw`（SWD 直读，不用抄屏）
 *    · 按下去画面不更新     -> 看 `g_ui_tick_cnt` / `g_ui_redraw_cnt` / `g_ui_mark`
 *    · 点页签没反应         -> 看 `g_ui_tapfire` / `g_ui_tabfire` / `g_ui_mx,my`
 *
 *  为什么需要它：DIAG 页只在**切到那一页**时才采样触摸，而"切页"本身要靠触摸 ——
 *  标定之前用的是占位常数（映射是错的），于是**根本点不进 DIAG 页**，
 *  形成死循环（实测：用户点页签完全没反应）。
 *  所以这里做三件事：
 *    ① 每个 UI tick 都采样（不再依赖当前在哪一页）；
 *    ② 把原始值**暴露成全局符号**，可以用 SWD 直接读（连抄屏都不用）；
 *    ③ 在**默认页（总览页）**显示一行，用户不用点任何东西就能看见。
 *
 *  ⚠️ 额外记录"每次按压松手时的原始值"（第 1 次 / 第 2 次），而不是锁存 min/max：
 *     标定要的是**配对**（哪个角给出哪一对数）。min/max 会丢掉配对 ——
 *     若某个轴是反的，左上角会给出 ymax、右下角给出 ymin，只看四个极值分不清谁是谁。
 *     按"第几次按压"分开记，配对关系就保住了，也便于事后用 SWD 一次性读走。
 *
 *  ⚠️ 为什么不用 `lcd_text` 直接画而是走 `ui_cell`：需要"同值不重画"，
 *     否则 200ms 一轮会把这一行反复写（其它行都靠脏区备忘防闪）。
 */
#define UI_TOUCH_DIAG 0
#if UI_TOUCH_DIAG
volatile uint16_t g_ui_tx_raw = 0u;    /* 最近一次读到的原始 x（失败时也记）*/
volatile uint16_t g_ui_ty_raw = 0u;    /* 原始 y */
volatile uint16_t g_ui_t_irq  = 0u;    /* touch_pressed() 电平：1 = 按下 */
volatile uint16_t g_ui_t_rc   = 2u;    /* 上次 touch_read_raw：0 = 成功，1 = 失败，2 = 还没按过 */
volatile uint16_t g_ui_t_press = 0u;   /* 累计"看到按下"的 tick 数 */
volatile uint16_t g_ui_t_ok   = 0u;    /* 按住且读取成功的累计次数 */

/* 逐次按压的"松手值"（保留配对关系 —— 标定就靠这四个数）*/
volatile uint16_t g_ui_t_npress = 0u;                  /* 已完成的"按下->松开"次数 */
volatile uint16_t g_ui_t_p1x = 0u, g_ui_t_p1y = 0u;    /* 第 1 次松手时的原始值 */
volatile uint16_t g_ui_t_p2x = 0u, g_ui_t_p2y = 0u;    /* 第 2 次 */

/* ---- 「按下去之后整个界面不更新」的定位探针（2026-10-13 加）----
 *
 *  现象：按屏幕左上角后**整个界面不再更新**。但固件是活的 ——
 *  `.noinit` 的故障记录是 `magic=F4170001 / boot_count=6 / last_kind=NONE`，
 *  既没有 HardFault 记录、启动次数也远不是复位循环的量级。
 *  ⇒ "卡死"只可能是**画面不再重画**，不可能是整个固件停了。
 *
 *  而"画面不再重画"只有三个可能的位置，必须一次分清（否则只能靠猜）：
 *    ① `gw_ui_tick()` 压根没被调用（UI 钩子/任务死了）
 *       -> `g_ui_tick_cnt` 不涨
 *    ② 调了、但被 200ms 门控挡住（`dt_ms` 恒为 0 ⇒ `s_acc_ms` 永远到不了 200）
 *       -> `tick_cnt` 涨、`redraw_cnt` 不涨，且 `g_ui_dt_last == 0`
 *    ③ 进了页面绘制但没回来（里面有死循环）
 *       -> `g_ui_mark` 停在 2
 *  另外 `g_ui_tapfire` / `g_ui_tabfire` 用来判断"这一按到底有没有被当成页签点击"，
 *  `g_ui_mx/g_ui_my` 是映射后的屏幕坐标（标定正确性 + 命中区域一起看）。
 *
 *  ⚠️ 与上面那批一样由 `UI_TOUCH_DIAG` 统一开关（当前为 0，不占 Flash/RAM）。 */
volatile uint32_t g_ui_tick_cnt   = 0u;   /* gw_ui_tick 进入次数 */
volatile uint32_t g_ui_redraw_cnt = 0u;   /* 通过门控（真的去画）的次数 */
volatile uint32_t g_ui_force_cnt  = 0u;   /* 其中"整屏重画"的次数 */
volatile uint32_t g_ui_dt_last    = 0u;   /* 最近一次传进来的 dt_ms */
volatile uint32_t g_ui_acc        = 0u;   /* s_acc_ms 快照 */
volatile uint16_t g_ui_mark       = 0u;   /* 0=刚进/未就绪 1=取到寄存器指针 2=过门控 3=画完 */
volatile uint16_t g_ui_page       = 0u;   /* 当前页（s_page）*/
volatile uint16_t g_ui_tapfire    = 0u;   /* handle_tap 被调用次数 */
volatile uint16_t g_ui_tabfire    = 0u;   /* 页签**真的切了页**的次数 */
volatile uint16_t g_ui_tapc       = 0u;   /* s_tap_cnt（0/1/2/3，3 才算点击成立）*/
volatile uint16_t g_ui_armed      = 0u;   /* s_touch_armed */
volatile uint16_t g_ui_mx         = 0xFFFFu;  /* 最近一次映射出的屏幕 x */
volatile uint16_t g_ui_my         = 0xFFFFu;  /* 最近一次映射出的屏幕 y */

static int      s_diag_was_pressed = 0;
static uint16_t s_diag_last_x = 0u, s_diag_last_y = 0u;

/* ---- 一次性 MISO 自检：读 ILI9341 的 RDDID ----
 *
 *  目的：把"**PA6/MISO 这条读回路整体不通**"与"**只有触摸那几根线没接**"区分开。
 *  为什么需要：屏幕能显示只证明 **MOSI/SCK/CS/DC/RST** 都好（写通路），
 *  而 PA6 是**读通路**，只有"读"才会用到它 —— 偏偏全工程只有两处读：
 *  ① `lcd_read_id()`（**当前没有任何调用点**，2026-10-12 从初始化里删掉了）
 *  ② `touch_read_raw()`（返回全 0）。
 *  所以只靠触摸这一条证据，分不清是"PA6 没接"还是"触摸那几根线没接"。
 *
 *  ⚠️ 只在**初始化完成之后**读（跑起来约 5 秒时读一次）：
 *     2026-10-12 那次是在**初始化序列中途**读，疑似把后续命令搞乱导致白屏；
 *     这里不碰初始化，读的又是一条只读命令，且 `HAL_SPI_Receive` 有超时上限（不会挂死）。
 *
 *  ⚠️ 结果写进 `g_lcd_last_id` / `g_lcd_id_ok` —— 已确认这两个量**没有任何地方写**
 *     （`lcd_read_id()` 全仓库无人调用），所以诊断页那行 `LCD ID` 一直显示 FAIL，
 *     那是"没人调用"而不是"读失败"。
 *
 *  判读：读到 `9341` ⇒ PA6 与 LCD 的 SDO 都通 ⇒ 故障在触摸那几根线（T_CS/T_CLK/T_DIN/T_DO）；
 *        读到 `----` ⇒ LCD 的 SDO 没到 PA6（PA6 很可能整体没接）。 */
static uint16_t s_diag_lcd_done = 0u;
static uint32_t s_diag_tick_cnt = 0u;

static void ui_touch_diag_lcd_once(void)
{
    uint32_t id = 0u;

    if (s_diag_lcd_done) { return; }
    s_diag_tick_cnt++;
    if (s_diag_tick_cnt < 100u) { return; }   /* 100 × 50ms ≈ 5s */
    s_diag_lcd_done = 1u;

    if (lcd_read_id(&id) == 0) {
        g_lcd_last_id = id;
        g_lcd_id_ok   = 1;
    } else {
        g_lcd_last_id = 0u;
        g_lcd_id_ok   = 0;
    }
    /* 保险：读完之后强制整屏重画一次（万一这次读扰动了面板状态，至少内容是对的）*/
    s_force = 1;
}

static void ui_touch_diag_sample(void)
{
    uint16_t rx = 0u, ry = 0u;
    int      pressed = touch_pressed();
    int      rc;

    g_ui_t_irq = (uint16_t)(pressed ? 1u : 0u);

    if (!pressed) {
        /* 松手 -> 把"按住期间最后一次成功的原始值"存成一次完整按压 */
        if (s_diag_was_pressed) {
            s_diag_was_pressed = 0;
            if (g_ui_t_npress == 0u) {
                g_ui_t_p1x = s_diag_last_x;
                g_ui_t_p1y = s_diag_last_y;
                g_ui_t_npress = 1u;
            } else if (g_ui_t_npress == 1u) {
                g_ui_t_p2x = s_diag_last_x;
                g_ui_t_p2y = s_diag_last_y;
                g_ui_t_npress = 2u;
            }
        }
        return;
    }

    s_diag_was_pressed = 1;
    g_ui_t_press++;

    rc = touch_read_raw(&rx, &ry);
    g_ui_t_rc   = (uint16_t)((rc == 0) ? 0u : 1u);
    g_ui_tx_raw = rx;      /* 失败也记：能看到"读到的到底是什么" */
    g_ui_ty_raw = ry;
    if (rc == 0) {
        g_ui_t_ok++;
        s_diag_last_x = rx;
        s_diag_last_y = ry;
    }
}
#endif

/* ================= 脏区备忘：定宽文本格 ================= */

#define UI_MAX_CELLS    32u
static const void *s_key[UI_MAX_CELLS];
static uint32_t    s_tag[UI_MAX_CELLS];
static uint16_t    s_cell_fg[UI_MAX_CELLS];

static void ui_cells_reset(void)
{
    uint8_t i;
    for (i = 0u; i < UI_MAX_CELLS; i++) {
        s_key[i] = 0;
        s_tag[i] = 0u;
        s_cell_fg[i] = 0u;
    }
}

/* 同位置 + 同 tag + 同颜色才跳过重画。
   ⚠️ 颜色必须参与比较：状态从"绿"变"红"但数值没变时也要重画。 */
static void ui_cell(uint16_t x, uint16_t y, const char *text, uint32_t tag,
                    uint16_t fg)
{
    const void *key = (const void *)(uintptr_t)(((uint32_t)x << 16) | y);
    uint8_t i, slot = UI_MAX_CELLS;

    for (i = 0u; i < UI_MAX_CELLS; i++) {
        if (s_key[i] == key) { slot = i; break; }
        if (s_key[i] == 0 && slot == UI_MAX_CELLS) { slot = i; }
    }
    if (slot == UI_MAX_CELLS) { return; }
    if (s_key[slot] == key && !s_force && s_tag[slot] == tag
        && s_cell_fg[slot] == fg) {
        return;
    }
    s_key[slot] = key;
    s_tag[slot] = tag;
    s_cell_fg[slot] = fg;
    (void)lcd_text(x, y, text, fg, UI_BG);
}

static void ui_cell_u(uint16_t x, uint16_t y, uint32_t v)
{
    char buf[16];
    (void)snprintf(buf, sizeof(buf), "%5lu", (unsigned long)v);
    ui_cell(x, y, buf, v, UI_FG);
}

/* 定宽补齐：不足 cols 列用空格补满（空格按背景色画 ⇒ 擦掉旧的尾巴） */
static void ui_pad(char *dst, unsigned cap, const char *src, unsigned cols)
{
    unsigned n = 0u;
    while (src[n] != '\0' && n < cols && (n + 1u) < cap) {
        dst[n] = src[n];
        n++;
    }
    while (n < cols && (n + 1u) < cap) { dst[n++] = ' '; }
    dst[n] = '\0';
}

/* ================= 脏区备忘：静态标签 ================= */

#define UI_LABEL_SLOTS  24u
static struct {
    uint16_t x, y, fg;
    char     txt[20];
} s_lbl_memo[UI_LABEL_SLOTS];
static uint8_t s_lbl_next;

static void ui_label_memo_reset(void)
{
    uint8_t i;
    for (i = 0u; i < UI_LABEL_SLOTS; i++) {
        s_lbl_memo[i].x = 0xFFFFu;
        s_lbl_memo[i].y = 0xFFFFu;
        s_lbl_memo[i].fg = 0u;
        s_lbl_memo[i].txt[0] = '\0';
    }
    s_lbl_next = 0u;
}

static void ui_label_c(uint16_t x, uint16_t y, const char *s, uint16_t fg)
{
    uint8_t i, slot = 0xFFu;
    for (i = 0u; i < UI_LABEL_SLOTS; i++) {
        if (s_lbl_memo[i].x == x && s_lbl_memo[i].y == y) { slot = i; break; }
    }
    if (slot != 0xFFu && s_lbl_memo[slot].fg == fg
        && strcmp(s_lbl_memo[slot].txt, s) == 0) {
        return;
    }
    if (slot == 0xFFu) {
        slot = s_lbl_next;
        s_lbl_next = (uint8_t)((s_lbl_next + 1u) % UI_LABEL_SLOTS);
    }
    s_lbl_memo[slot].x = x;
    s_lbl_memo[slot].y = y;
    s_lbl_memo[slot].fg = fg;
    (void)snprintf(s_lbl_memo[slot].txt, sizeof(s_lbl_memo[slot].txt), "%s", s);
    (void)lcd_text(x, y, s, fg, UI_BG);
}

/* 默认次要色（版式标签：TEMP / HUMI / CH1 ...） */
static void ui_label(uint16_t x, uint16_t y, const char *s)
{
    ui_label_c(x, y, s, UI_DIM);
}

/* ================= 脏区备忘：状态色点 ================= */

#define UI_MAX_DOTS 8u
static uint16_t s_dot_x[UI_MAX_DOTS];
static uint16_t s_dot_y[UI_MAX_DOTS];
static uint16_t s_dot_c[UI_MAX_DOTS];
static uint8_t  s_dot_n;

static void ui_dots_reset(void)
{
    uint8_t i;
    for (i = 0u; i < UI_MAX_DOTS; i++) {
        s_dot_x[i] = 0xFFFFu;
        s_dot_y[i] = 0xFFFFu;
        s_dot_c[i] = 0u;
    }
    s_dot_n = 0u;
}

/* 8x8 状态色点，纵向与 16px 文本居中（y 传文本行坐标）。同位置同色不重画。 */
static void ui_dot(uint16_t x, uint16_t y, uint16_t color)
{
    uint8_t i, slot = 0xFFu;
    for (i = 0u; i < UI_MAX_DOTS; i++) {
        if (s_dot_x[i] == x && s_dot_y[i] == y) { slot = i; break; }
    }
    if (slot == 0xFFu) {
        if (s_dot_n >= UI_MAX_DOTS) { return; }
        slot = s_dot_n;
        s_dot_n++;
        s_dot_x[slot] = x;
        s_dot_y[slot] = y;
        s_dot_c[slot] = 0u;
    }
    if (!s_force && s_dot_c[slot] == color) { return; }
    s_dot_c[slot] = color;
    lcd_fill_rect(x, (uint16_t)(y + 4u), 8u, 8u, color);
}

/* ================= 脏区备忘：量程条 ================= */

#define UI_MAX_BARS 4u
static uint16_t s_bar_x[UI_MAX_BARS];
static uint16_t s_bar_y[UI_MAX_BARS];
static uint32_t s_bar_v[UI_MAX_BARS];
static uint16_t s_bar_c[UI_MAX_BARS];
static uint8_t  s_bar_n;

static void ui_bars_reset(void)
{
    uint8_t i;
    for (i = 0u; i < UI_MAX_BARS; i++) {
        s_bar_x[i] = 0xFFFFu;
        s_bar_y[i] = 0xFFFFu;
        s_bar_v[i] = 0xFFFFFFFFu;
        s_bar_c[i] = 0u;
    }
    s_bar_n = 0u;
}

/* 横向量程条：把 [vmin,vmax] 映射到条宽，直观看出"高/低"。
   同位置 + 同值 + 同色不重画（值不变时不会每 200ms 刷一遍）。 */
static void ui_bar(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                   uint32_t v, uint32_t vmin, uint32_t vmax, uint16_t color)
{
    uint8_t  i, slot = 0xFFu;
    uint32_t span = vmax - vmin;
    uint16_t fill;

    if (span == 0u || w < 4u || h < 4u) { return; }
    if (v < vmin) { v = vmin; }
    if (v > vmax) { v = vmax; }
    fill = (uint16_t)(((v - vmin) * (uint32_t)(w - 2u)) / span);

    for (i = 0u; i < UI_MAX_BARS; i++) {
        if (s_bar_x[i] == x && s_bar_y[i] == y) { slot = i; break; }
    }
    if (slot == 0xFFu) {
        if (s_bar_n >= UI_MAX_BARS) { return; }
        slot = s_bar_n;
        s_bar_n++;
        s_bar_x[slot] = x;
        s_bar_y[slot] = y;
        s_bar_v[slot] = 0xFFFFFFFFu;
        s_bar_c[slot] = 0u;
    }
    if (!s_force && s_bar_v[slot] == v && s_bar_c[slot] == color) { return; }
    s_bar_v[slot] = v;
    s_bar_c[slot] = color;

    lcd_frame(x, y, w, h, UI_LINE);
    lcd_fill_rect((uint16_t)(x + 1u), (uint16_t)(y + 1u),
                  (uint16_t)(w - 2u), (uint16_t)(h - 2u), UI_BAR_BG);
    if (fill > 0u) {
        lcd_fill_rect((uint16_t)(x + 1u), (uint16_t)(y + 1u), fill,
                      (uint16_t)(h - 2u), color);
    }
}

/* ================= 框架 ================= */

/* 标题栏：蓝底白字 + 左侧浅蓝强调条 + 底部一条浅蓝线 */
static void ui_title(const char *s)
{
    lcd_fill_rect(0u, 0u, LCD_W, UI_TITLE_H, UI_BLUE);
    lcd_fill_rect(0u, 0u, 5u, UI_TITLE_H, UI_ACCENT);
    (void)lcd_text((uint16_t)(UI_COL_X + 4u),
                   (uint16_t)((UI_TITLE_H - LCD_CHAR_H) / 2u),
                   s, UI_FG, UI_BLUE);
    lcd_fill_rect(0u, (uint16_t)(UI_TITLE_H - 1u), LCD_W, 1u, UI_ACCENT);
}

/* 底部页签：当前页用蓝色实心块反白，其余为暗灰字（比 [] 括号清楚得多） */
static void ui_tabs(void)
{
    uint8_t  i;
    uint16_t w = (uint16_t)(LCD_W / PAGE_COUNT);

    lcd_fill_rect(0u, UI_TAB_Y, LCD_W, UI_TAB_H, UI_PANEL);
    lcd_fill_rect(0u, UI_TAB_Y, LCD_W, 1u, UI_LINE);

    for (i = 0u; i < PAGE_COUNT; i++) {
        uint16_t tx = (uint16_t)(i * w);
        uint16_t tw = (uint16_t)((unsigned)strlen(s_tab_name[i]) * LCD_CHAR_W);
        uint16_t x  = (uint16_t)(tx + (uint16_t)((w - tw) / 2u));
        uint16_t y  = (uint16_t)(UI_TAB_Y
                                 + (uint16_t)((UI_TAB_H - LCD_CHAR_H) / 2u));

        if (i == (uint8_t)s_page) {
            lcd_fill_rect((uint16_t)(tx + 5u), (uint16_t)(UI_TAB_Y + 3u),
                          (uint16_t)(w - 10u), (uint16_t)(UI_TAB_H - 6u), UI_BLUE);
            (void)lcd_text(x, y, s_tab_name[i], UI_FG, UI_BLUE);
        } else {
            (void)lcd_text(x, y, s_tab_name[i], UI_DIM, UI_PANEL);
        }
    }
}

static void ui_page_clear(void)
{
    lcd_fill_rect(0u, UI_TITLE_H, LCD_W,
                  (uint16_t)(UI_TAB_Y - UI_TITLE_H), UI_BG);
    ui_cells_reset();
    ui_dots_reset();
    ui_bars_reset();
}

static uint32_t u32_of(const uint16_t *r, uint16_t hi, uint16_t lo)
{
    return ((uint32_t)r[hi] << 16) | (uint32_t)r[lo];
}

/* ================= 页面 ================= */

static void page_overview(const uint16_t *ir)
{
    uint16_t st     = ir[IREG_DEV_STATUS];
    int      dht_ok = (st & IREG_STATUS_BIT_DHT_OK) ? 1 : 0;
    uint16_t y;

    /* --- 左列：传感器与状态 --- */
    y = (uint16_t)(UI_TITLE_H + 4u);

    ui_label(UI_COL_X, y, "TEMP");
    if (dht_ok) {
        int32_t  t = (int32_t)(int16_t)ir[IREG_TEMP_X10];
        char     raw[16], buf[16];
        char     sign = (t < 0) ? '-' : '+';
        uint32_t a = (uint32_t)((t < 0) ? -t : t);
        (void)snprintf(raw, sizeof(raw), "%c%lu.%lu C", sign,
                       (unsigned long)(a / 10u), (unsigned long)(a % 10u));
        ui_pad(buf, sizeof(buf), raw, 10u);
        ui_cell(UI_VAL_X, y, buf, ((uint32_t)ir[IREG_TEMP_X10] << 1) | 1u, UI_VAL);
    } else {
        ui_cell(UI_VAL_X, y, "-- no DHT ", 0u, UI_ERR);
    }

    y = (uint16_t)(y + UI_ROW_H);
    ui_label(UI_COL_X, y, "HUMI");
    if (dht_ok) {
        char raw[16], buf[16];
        (void)snprintf(raw, sizeof(raw), "%lu.%lu %%",
                       (unsigned long)(ir[IREG_HUMI_X10] / 10u),
                       (unsigned long)(ir[IREG_HUMI_X10] % 10u));
        ui_pad(buf, sizeof(buf), raw, 10u);
        ui_cell(UI_VAL_X, y, buf, ((uint32_t)ir[IREG_HUMI_X10] << 1) | 1u, UI_VAL);
    } else {
        ui_cell(UI_VAL_X, y, "-- no DHT ", 0u, UI_ERR);
    }

    y = (uint16_t)(y + UI_ROW_H);
    ui_label(UI_COL_X, y, "CAN");
    {
        int ok = (st & IREG_STATUS_BIT_CAN_OK) ? 1 : 0;
        ui_dot(84u, y, ok ? UI_OK : UI_ERR);
        ui_cell(UI_VAL_X, y, ok ? "BUS OK " : "NO BUS ",
                (uint32_t)(st & IREG_STATUS_BIT_CAN_OK), ok ? UI_OK : UI_ERR);
    }

    y = (uint16_t)(y + UI_ROW_H);
    ui_label(UI_COL_X, y, "MQTT");
    {
        static const char *const st_name[4] = { "OFF", "CONN", "OK", "AUTH!" };
        static const uint16_t    st_col[4]  = { UI_OFF, UI_WARN, UI_OK, UI_ERR };
        char     buf[16];
        uint16_t s = ir[IREG_MQTT_STATUS];
        if (s > 3u) { s = 0u; }
        ui_dot(84u, y, st_col[s]);
        ui_pad(buf, sizeof(buf), st_name[s], 6u);
        ui_cell(UI_VAL_X, y, buf, ((uint32_t)s << 1) | 1u, st_col[s]);
    }

    y = (uint16_t)(y + UI_ROW_H);
    ui_label(UI_COL_X, y, "UPTIME");
    ui_cell_u(UI_VAL_X, y, u32_of(ir, IREG_UPTIME_HI, IREG_UPTIME_LO));

    /* 分隔线（放在 UPTIME 行**下方**，不要压到字形底部） */
    lcd_fill_rect(UI_COL_X, (uint16_t)(y + UI_ROW_H),
                  (uint16_t)(LCD_W - 2u * UI_COL_X), 1u, UI_LINE);

    /* --- 右列：故障与统计（绿=正常 / 红=异常 / 橙=进行中） --- */
    y = (uint16_t)(UI_TITLE_H + 4u);
    {
        char     b[24];
        uint16_t c;

        c = (st & IREG_STATUS_BIT_IWDG_RESET) ? UI_ERR : UI_OK;
        ui_pad(b, sizeof(b),
               (st & IREG_STATUS_BIT_IWDG_RESET) ? "IWDG RESET" : "iwdg ok", 14u);
        ui_label_c(UI_COL2_X, y, b, c);

        y = (uint16_t)(y + UI_ROW_H);
        c = (st & IREG_STATUS_BIT_HARDFAULT) ? UI_ERR : UI_OK;
        ui_pad(b, sizeof(b),
               (st & IREG_STATUS_BIT_HARDFAULT) ? "HARDFAULT" : "no hardfault", 14u);
        ui_label_c(UI_COL2_X, y, b, c);

        y = (uint16_t)(y + UI_ROW_H);
        c = (st & IREG_STATUS_BIT_OTA_BUSY) ? UI_WARN : UI_DIM;
        ui_pad(b, sizeof(b),
               (st & IREG_STATUS_BIT_OTA_BUSY) ? "OTA BUSY" : "ota idle", 14u);
        ui_label_c(UI_COL2_X, y, b, c);

        y = (uint16_t)(y + UI_ROW_H);
        ui_label(UI_COL2_X, y, "CAN RX");
        ui_cell_u((uint16_t)(UI_COL2_X + 48u), y,
                  u32_of(ir, IREG_CAN_RX_HI, IREG_CAN_RX_LO));
    }

    /* --- 下半屏：环境量程条 ---
       把 "23.5C / 61.2%" 变成能一眼看出高低的条（否则读者不知道 61% 算不算高）。
       温度量程 0..50.0C、湿度 0..100.0%；DHT 失效时条归零并变灰（表示数据不可信）。*/
    ui_label(UI_COL_X, 128u, "TEMP RANGE");
    ui_bar(UI_VAL_X, 129u, (uint16_t)(LCD_W - UI_VAL_X - UI_COL_X), 22u,
           dht_ok ? (uint32_t)ir[IREG_TEMP_X10] : 0u, 0u, 500u,
           dht_ok ? UI_VAL : UI_OFF);

    ui_label(UI_COL_X, 160u, "HUMI RANGE");
    ui_bar(UI_VAL_X, 161u, (uint16_t)(LCD_W - UI_VAL_X - UI_COL_X), 22u,
           dht_ok ? (uint32_t)ir[IREG_HUMI_X10] : 0u, 0u, 1000u,
           dht_ok ? UI_VAL : UI_OFF);

#if UI_TOUCH_DIAG
    /* 诊断区（由 UI_TOUCH_DIAG 开关，当前为 0）：占页签栏上方的空档（y=184..217），共两行。
       放在**默认页**上，这样不用点任何东西就能看见 —— 绕开
       "标定前映射是错的 -> 点不进 DIAG 页 -> 看不到原始值"这个死循环。
         行 1：实时原始值（按住时刷新）
         行 2：中断脚电平 / 原始值读取返回码 / 已完成的按压次数
       ⚠️ 标签与 DIAG 页那行故意不同名（那里是 `TOUCH`），免得混淆。*/
    {
        char tb[40], tb2[24];

        ui_label(UI_COL_X, 186u, "TOUCH RAW");
        (void)snprintf(tb, sizeof(tb), "%u,%u",
                       (unsigned)g_ui_tx_raw, (unsigned)g_ui_ty_raw);
        ui_pad(tb2, sizeof(tb2), tb, 12u);
        ui_cell(UI_VAL_X, 186u, tb2,
                ((uint32_t)g_ui_tx_raw << 16) | (uint32_t)g_ui_ty_raw,
                UI_VAL);

        ui_label(UI_COL_X, 202u, "STATUS");
        {
            char lb[8];
            if (g_lcd_id_ok) {
                (void)snprintf(lb, sizeof(lb), "%04X",
                               (unsigned)(g_lcd_last_id & 0xFFFFu));
            } else {
                (void)snprintf(lb, sizeof(lb), "----");
            }
            (void)snprintf(tb, sizeof(tb), "IRQ%u RC%u N=%u LCD=%s",
                           (unsigned)g_ui_t_irq, (unsigned)g_ui_t_rc,
                           (unsigned)g_ui_t_npress, lb);
        }
        ui_pad(tb2, sizeof(tb2), tb, 22u);
        ui_cell(UI_VAL_X, 202u, tb2,
                ((uint32_t)g_ui_t_irq     << 24)
                    | ((uint32_t)g_ui_t_rc      << 16)
                    | ((uint32_t)g_ui_t_npress  << 8)
                    | ((uint32_t)(g_lcd_id_ok & 1) << 7)
                    | (uint32_t)(g_lcd_last_id & 0x7Fu),
                UI_VAL);
    }
#endif
}

static void page_can(const uint16_t *ir)
{
    uint8_t  i;
    uint16_t st = ir[IREG_DEV_STATUS];
    uint16_t y  = (uint16_t)(UI_TITLE_H + 4u);
    int      ok = (st & IREG_STATUS_BIT_CAN_OK) ? 1 : 0;

    {
        char b[32];
        ui_pad(b, sizeof(b), ok ? "BUS OK" : "NO BUS / ERROR-PASSIVE", 24u);
        ui_dot(UI_COL_X, y, ok ? UI_OK : UI_ERR);
        ui_label_c((uint16_t)(UI_COL_X + 14u), y, b, ok ? UI_OK : UI_ERR);
    }

    lcd_fill_rect(UI_COL_X, (uint16_t)(y + UI_ROW_H),
                  (uint16_t)(LCD_W - 2u * UI_COL_X), 1u, UI_LINE);
    y = (uint16_t)(y + UI_ROW_H + 4u);

    for (i = 0u; i < 4u; i++) {
        char     nm[8];
        uint16_t yy = (uint16_t)(y + i * UI_ROW_H);
        (void)snprintf(nm, sizeof(nm), "CH%u", (unsigned)(i + 1u));
        ui_label_c(UI_COL_X, yy, nm, UI_ACCENT);
        ui_cell_u((uint16_t)(UI_COL_X + 48u), yy, ir[IREG_CAN_CH1 + i]);
        (void)snprintf(nm, sizeof(nm), "CH%u", (unsigned)(i + 5u));
        ui_label_c(UI_COL2_X, yy, nm, UI_ACCENT);
        ui_cell_u((uint16_t)(UI_COL2_X + 48u), yy, ir[IREG_CAN_CH1 + i + 4u]);
    }

    y = (uint16_t)(UI_TAB_Y - UI_ROW_H - 2u);
    lcd_fill_rect(UI_COL_X, (uint16_t)(y - 4u),
                  (uint16_t)(LCD_W - 2u * UI_COL_X), 1u, UI_LINE);
    ui_label(UI_COL_X, y, "RX TOTAL");
    ui_cell_u((uint16_t)(UI_COL_X + 80u), y,
              u32_of(ir, IREG_CAN_RX_HI, IREG_CAN_RX_LO));
}

static void page_diag(const uint16_t *ir)
{
    uint16_t y  = (uint16_t)(UI_TITLE_H + 4u);
    uint16_t st = ir[IREG_DEV_STATUS];

    ui_label(UI_COL_X, y, "MB RX");
    ui_cell_u(UI_VAL_X, y, u32_of(ir, IREG_MB_RX_HI, IREG_MB_RX_LO));
    ui_label(UI_COL2_X, y, "MB ERR");
    ui_cell_u((uint16_t)(UI_COL2_X + 64u), y,
              u32_of(ir, IREG_MB_ERR_HI, IREG_MB_ERR_LO));

    y = (uint16_t)(y + UI_ROW_H);
    ui_label(UI_COL_X, y, "CAN RX");
    ui_cell_u(UI_VAL_X, y, u32_of(ir, IREG_CAN_RX_HI, IREG_CAN_RX_LO));
    ui_label(UI_COL2_X, y, "DIAG ST");
    ui_cell_u((uint16_t)(UI_COL2_X + 64u), y, ir[IREG_DIAG_STATE]);

    y = (uint16_t)(y + UI_ROW_H);
    ui_label(UI_COL_X, y, "OFFL Q");
    ui_cell_u(UI_VAL_X, y, ir[IREG_OFFLINE_DEPTH]);
    ui_label(UI_COL2_X, y, "DROP");
    ui_cell_u((uint16_t)(UI_COL2_X + 64u), y, ir[IREG_OFFLINE_DROP]);

    y = (uint16_t)(y + UI_ROW_H);
    ui_label(UI_COL_X, y, "FW VER");
    {
        char buf[16];
        (void)snprintf(buf, sizeof(buf), "%u.%u", ir[IREG_FW_VER_MAJOR],
                       ir[IREG_FW_VER_MINOR]);
        ui_cell(UI_VAL_X, y, buf,
                ((uint32_t)ir[IREG_FW_VER_MAJOR] << 16) | ir[IREG_FW_VER_MINOR],
                UI_FG);
    }
    ui_label(UI_COL2_X, y, "MST ERR");
    ui_cell_u((uint16_t)(UI_COL2_X + 64u), y, ir[IREG_MASTER_ERR]);

    /* --- 触摸原始值（标定用）+ LCD 自检 --- */
    {
        static uint16_t last_raw_x = 0u, last_raw_y = 0u;
        uint16_t rx = 0u, ry = 0u;
        char     tb[16];

        if (touch_pressed() && touch_read_raw(&rx, &ry) == 0) {
            last_raw_x = rx; last_raw_y = ry;
        }
        ui_label(UI_COL_X, 196u, "TOUCH");
        (void)snprintf(tb, sizeof(tb), "%u,%u",
                       (unsigned)last_raw_x, (unsigned)last_raw_y);
        ui_cell(UI_VAL_X, 196u, tb,
                ((uint32_t)last_raw_x << 16) | last_raw_y, UI_VAL);

        /* ⚠️ LCD 自检结果（RDDID 读到的 ID，正常 0x9341；读不到显示 FAIL）。
           这是"接线通不通"的直接证据，也是 test/test_ui_render.c 的断言项，
           不要删。 */
        ui_label(UI_COL2_X, 196u, "LCD ID");
        {
            char ib[8];
            if (g_lcd_id_ok) {
                (void)snprintf(ib, sizeof(ib), "%04X",
                               (unsigned)(g_lcd_last_id & 0xFFFFu));
            } else {
                (void)snprintf(ib, sizeof(ib), "FAIL");
            }
            ui_cell(226u, 196u, ib,
                    (uint32_t)(g_lcd_id_ok ? g_lcd_last_id : 0u),
                    g_lcd_id_ok ? UI_OK : UI_ERR);
        }
    }

    /* --- 状态位 --- */
    y = (uint16_t)(y + UI_ROW_H + 4u);
    {
        char     b[24];
        uint16_t c;

        c = (st & IREG_STATUS_BIT_PARAM_OK) ? UI_OK : UI_WARN;
        ui_pad(b, sizeof(b),
               (st & IREG_STATUS_BIT_PARAM_OK) ? "PARAM OK" : "PARAM DEF", 12u);
        ui_label_c(UI_COL_X, y, b, c);
        c = (st & IREG_STATUS_BIT_DHT_OK) ? UI_OK : UI_ERR;
        ui_pad(b, sizeof(b),
               (st & IREG_STATUS_BIT_DHT_OK) ? "DHT OK" : "DHT FAIL", 12u);
        ui_label_c(UI_COL2_X, y, b, c);

        y = (uint16_t)(y + UI_ROW_H);
        c = (st & IREG_STATUS_BIT_WIFI_OK) ? UI_OK : UI_OFF;
        ui_pad(b, sizeof(b),
               (st & IREG_STATUS_BIT_WIFI_OK) ? "WIFI OK" : "WIFI --", 12u);
        ui_label_c(UI_COL_X, y, b, c);
        c = (st & IREG_STATUS_BIT_MQTT_OK) ? UI_OK : UI_OFF;
        ui_pad(b, sizeof(b),
               (st & IREG_STATUS_BIT_MQTT_OK) ? "MQTT OK" : "MQTT --", 12u);
        ui_label_c(UI_COL2_X, y, b, c);
    }
}

/* ---- 配置页 ---- */
#define CFG_BTN_W   34u
#define CFG_BTN_H   18u
#define CFG_BTN_PLUS_X   190u
#define CFG_BTN_MINUS_X  (CFG_BTN_PLUS_X - CFG_BTN_W - 4u)
#define CFG_ROW1_Y  (UI_TITLE_H + 20u)
#define CFG_ROW2_Y  (CFG_ROW1_Y + 44u)

static void cfg_button(uint16_t x, uint16_t y, const char *s)
{
    lcd_fill_rect(x, y, CFG_BTN_W, CFG_BTN_H, UI_BTN_BG);
    lcd_frame(x, y, CFG_BTN_W, CFG_BTN_H, UI_ACCENT);
    (void)lcd_text((uint16_t)(x + 13u), (uint16_t)(y + 1u), s, UI_FG, UI_BTN_BG);
}

static void page_cfg(const uint16_t *hr)
{
    ui_label(UI_COL_X, CFG_ROW1_Y, "REPORT PERIOD (s)");
    ui_cell_u(UI_COL_X, (uint16_t)(CFG_ROW1_Y + UI_ROW_H),
              hr[HREG_REPORT_PERIOD]);
    cfg_button(CFG_BTN_MINUS_X, CFG_ROW1_Y, "-");
    cfg_button(CFG_BTN_PLUS_X, CFG_ROW1_Y, "+");

    ui_label(UI_COL_X, CFG_ROW2_Y, "BAUD CODE (0..3)");
    ui_cell_u(UI_COL_X, (uint16_t)(CFG_ROW2_Y + UI_ROW_H),
              hr[HREG_BAUDRATE]);
    cfg_button(CFG_BTN_MINUS_X, CFG_ROW2_Y, "-");
    cfg_button(CFG_BTN_PLUS_X, CFG_ROW2_Y, "+");

    lcd_fill_rect(UI_COL_X, (uint16_t)(CFG_ROW2_Y + 2u * UI_ROW_H + 6u),
                  (uint16_t)(LCD_W - 2u * UI_COL_X), 1u, UI_LINE);
    ui_label(UI_COL_X, (uint16_t)(CFG_ROW2_Y + 2u * UI_ROW_H + 10u),
             "writes holding regs master uses");
}

/* ================= 触摸 ================= */

static int      s_touch_armed = 0;
static uint16_t s_tap_x = 0u, s_tap_y = 0u;
static uint8_t  s_tap_cnt = 0u;

static int in_rect(uint16_t x, uint16_t y, uint16_t rx, uint16_t ry,
                   uint16_t rw, uint16_t rh)
{
    return (x >= rx && x < (uint16_t)(rx + rw)
            && y >= ry && y < (uint16_t)(ry + rh)) ? 1 : 0;
}

static void handle_tap(uint16_t x, uint16_t y, const uint16_t *hr)
{
    uint16_t w = (uint16_t)(LCD_W / PAGE_COUNT);

#if UI_TOUCH_DIAG
    g_ui_tapfire++;
#endif

    if (y >= UI_TAB_Y) {
        uint8_t i = (uint8_t)(x / (w ? w : 1u));
        if (i < (uint8_t)PAGE_COUNT && i != (uint8_t)s_page) {
            s_page  = (ui_page_t)i;
            s_force = 1;
#if UI_TOUCH_DIAG
            g_ui_tabfire++;
#endif
        }
        return;
    }
    if (s_page != PAGE_CFG) return;

    if (in_rect(x, y, CFG_BTN_PLUS_X, CFG_ROW1_Y, CFG_BTN_W, CFG_BTN_H)) {
        uint16_t v = (uint16_t)(hr[HREG_REPORT_PERIOD] + 1u);
        if (v > 3600u) { v = 3600u; }
        s_port.set_hreg(HREG_REPORT_PERIOD, v);
    } else if (in_rect(x, y, CFG_BTN_MINUS_X, CFG_ROW1_Y, CFG_BTN_W, CFG_BTN_H)) {
        uint16_t v = hr[HREG_REPORT_PERIOD];
        if (v > 1u) { v--; }
        s_port.set_hreg(HREG_REPORT_PERIOD, v);
    } else if (in_rect(x, y, CFG_BTN_PLUS_X, CFG_ROW2_Y, CFG_BTN_W, CFG_BTN_H)) {
        s_port.set_hreg(HREG_BAUDRATE,
                        (uint16_t)((hr[HREG_BAUDRATE] + 1u) & 0x0003u));
    } else if (in_rect(x, y, CFG_BTN_MINUS_X, CFG_ROW2_Y, CFG_BTN_W, CFG_BTN_H)) {
        s_port.set_hreg(HREG_BAUDRATE,
                        (uint16_t)((hr[HREG_BAUDRATE] + 3u) & 0x0003u));
    }
}

static void touch_poll(const uint16_t *hr)
{
    if (!s_touch_ok) return;
    uint16_t x, y;
    if (!touch_get(&x, &y)) {
        s_touch_armed = 0; s_tap_cnt = 0u;
#if UI_TOUCH_DIAG
        g_ui_armed = 0u; g_ui_tapc = 0u;
#endif
        return;
    }
#if UI_TOUCH_DIAG
    g_ui_mx = x; g_ui_my = y;
#endif
    if (!s_touch_armed) {
        s_touch_armed = 1; s_tap_x = x; s_tap_y = y; s_tap_cnt = 1u;
#if UI_TOUCH_DIAG
        g_ui_armed = 1u; g_ui_tapc = 1u;
#endif
        return;
    }
    if (x != s_tap_x || y != s_tap_y) {
        s_tap_x = x; s_tap_y = y; s_tap_cnt = 1u;
#if UI_TOUCH_DIAG
        g_ui_tapc = 1u;
#endif
        return;
    }
    if (s_tap_cnt < UI_TAP_TICKS) {
        s_tap_cnt++;
#if UI_TOUCH_DIAG
        g_ui_tapc = s_tap_cnt;
#endif
        if (s_tap_cnt == UI_TAP_TICKS) handle_tap(x, y, hr);
    }
}

/* ================= 对外 ================= */

int gw_ui_init(const gw_ui_port_t *port)
{
    if (port == 0 || port->get_iregs == 0 || port->get_hregs == 0
        || port->set_hreg == 0) {
        return -1;
    }
    s_port = *port;
    lcd_backlight(1);
    if (lcd_init() != 0) return -3;

    gw_ui_watchdog_feed();
    /* 整屏黑底填充（首次清屏） */
    lcd_fill(UI_BG);
    gw_ui_watchdog_feed();

    if (touch_init() != 0) s_touch_ok = 0;

    s_ready = 1;
    s_force = 1;
    return 0;
}

void gw_ui_invalidate(void)
{
    s_force = 1;
    ui_cells_reset();
    ui_dots_reset();
    ui_bars_reset();
}

void gw_ui_tick(uint32_t dt_ms)
{
    const uint16_t *ir;
    const uint16_t *hr;

#if UI_TOUCH_DIAG
    g_ui_tick_cnt++;
    g_ui_dt_last = dt_ms;
    g_ui_mark    = 0u;
#endif
    if (!s_ready) return;
    ir = s_port.get_iregs();
    hr = s_port.get_hregs();
    if (ir == 0 || hr == 0) return;
#if UI_TOUCH_DIAG
    g_ui_mark = 1u;
#endif

    touch_poll(hr);

#if UI_TOUCH_DIAG
    /* 临时诊断：每 tick 都采一次（不受"当前在哪一页"影响，见上面那段注释）*/
    ui_touch_diag_sample();
    ui_touch_diag_lcd_once();
#endif

    s_acc_ms += dt_ms;
#if UI_TOUCH_DIAG
    g_ui_acc  = s_acc_ms;
    g_ui_page = (uint16_t)s_page;
#endif
    if (!s_force && s_acc_ms < UI_REDRAW_MS) return;
    s_acc_ms = 0u;
#if UI_TOUCH_DIAG
    g_ui_redraw_cnt++;
    g_ui_mark = 2u;
#endif

    if (s_force) {
#if UI_TOUCH_DIAG
        g_ui_force_cnt++;
#endif
        ui_page_clear();
        ui_label_memo_reset();
        ui_tabs();
        switch (s_page) {
        case PAGE_OVW:  ui_title("GATEWAY / OVERVIEW"); break;
        case PAGE_CAN:  ui_title("CAN CHANNELS");       break;
        case PAGE_DIAG: ui_title("DIAGNOSTICS");        break;
        case PAGE_CFG:  ui_title("CONFIG (touch)");     break;
        default: break;
        }
        s_force = 0;
    }

    switch (s_page) {
    case PAGE_OVW:  page_overview(ir); break;
    case PAGE_CAN:  page_can(ir);      break;
    case PAGE_DIAG: page_diag(ir);     break;
    case PAGE_CFG:  page_cfg(hr);      break;
    default: break;
    }
#if UI_TOUCH_DIAG
    g_ui_mark = 3u;                 /* 走到这里 = 本轮绘制完整返回 */
#endif
}
