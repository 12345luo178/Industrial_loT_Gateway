/* ==================================================================
 *  XPT2046 电阻触摸实现 —— 设计约束与三个坑见 touch_xpt2046.h
 *
 *  上板标定方法（第一次接屏幕时做一次，约两分钟）
 *  ---------------------------------------------------------------
 *  ✅ **本机已于 2026-10-13 标定完成**，结果见下面 "校准常数" 那一块。
 *     下面保留的是**通用方法**，换屏/换线后照做。
 *
 *  1) 让 UI 跑起来，切到 **DIAG 页** —— 最下面一行 `TOUCH RAW` 直接显示
 *     touch_read_raw() 的原始值（x,y，0..4095）。按住屏幕左上角抄一组，
 *     再按右下角抄一组；**松手后读数会保留**，所以可以松手再看屏幕。
 *     （2026-10-12 加：这样标定不需要串口或 SWD，一眼就能抄数。）
 *  2) 这两组最小/最大原始值填进下面的默认常数（或用 touch_set_calib()）；
 *  3) 判据：点左上角返回 (0,0)±4，点右下角返回 (319,239)±4，中间线性。
 *  ⚠️ 若左右/上下**反了**，改 invert_* 而不是去调 min/max —— 后者会把
 *     线性度调坏，症状是"四角准、中间偏"。
 *
 *  ⚠️⚠️ 2026-10-13 补两条实测教训（本机真踩过，详见下面"校准常数"的注释）：
 *     ① **对角两点分不出"轴互换"**：左上+右下在"两轴都反"和"互换且都反"
 *        两种模型下结果完全一样。必须再按一个**非对角点**（右上或左下）。
 *        本机就是互换的，只按对角会得出错误的"只需 invert"结论。
 *     ② **标定必须用"按住不放"的值，不能用松手值**：松手瞬间手指抬起、
 *        接触面积在变，读数不可信（实测同一位置松手值能差 300+ 个码）。
 *        而且四角本身是电阻屏非线性最差的区域，min/max 最好由
 *        "两个靠内一点的点 + 最小二乘外推"得到，而不是直接抄四角。
 * ================================================================== */
#include "stm32f1xx_hal.h"      /* ⚠️ 必须在 gw_config.h 之前 */
#include "gw_config.h"
#include "lcd_ili9341.h"        /* 只为 lcd_bus_acquire/release（SPI1 总线锁）*/
#include "touch_xpt2046.h"

extern SPI_HandleTypeDef hspi1; /* CubeMX 在 spi.c 定义；port 层不 include Core/Inc */

/* ---- XPT2046 命令字（12 位模式，差分关）---- */
#define XP_CMD_X        0xD0u   /* S=1 A2..A0=101 -> X 通道 */
#define XP_CMD_Y        0x90u   /* S=1 A2..A0=001 -> Y 通道 */

/* 9MHz 对 XPT2046 太快，读取期间降到 APB2/64 = 1.125MHz */
#define XP_SLOW_PRESCALER   SPI_BAUDRATEPRESCALER_64

/* 每轴采样次数（取中值）；以及一致性门限：最大-最小超过它就认为"读飞了" */
#define XP_SAMPLES       3u
#define XP_SPREAD_MAX    120u

/* ---- 校准常数：**已上板实测**（2026-10-13）----
 *
 *  ⚠️ 这块屏的触摸轴与画面**既互换、又都反向** —— 任何"典型值"都不可能对，
 *     必须实测。下面是实测数据与判据，改之前先读完。
 *
 *  三组**按住不放**测到的原始值（`g_ui_tx_raw/ty_raw`，SWD 直读）：
 *
 *      画面左上角 (0,0)        -> raw (3804, 3598)
 *      画面右上角 (319,0)      -> raw (3740,  306)   <- 判定"互换"的关键点
 *      画面正中 (159,119)      -> raw (1961, 1923)   <- 最可信的一点（长按 ±4）
 *
 *  ⚠️⚠️ 为什么"按对角两点"不够 —— 这是本次踩的最大的坑：
 *      若只是"两轴都反"（不互换），右上角的 raw 应是 (384, 3598)（x 小 y 大）；
 *      若"互换且都反"，右上角应是 (3804, 672)（x 大 y 小）。
 *      两者在**对角线上表现完全一致**（左上+右下分不出来），
 *      必须用**非对角点**（右上/左下）才能区分。
 *      实测 (3740, 306) = x 大 y 小 ⇒ **确认互换**。
 *      （反证：不互换模型会把"按正中"那一点算成 x 被钳到 319，明显荒谬。）
 *
 *  佐证：LCD 的 `MADCTL = 0xE8` 里带 `MV=1`（行列互换），面板本身是转 90° 用的，
 *        触摸的 X/Y 电极跟着转 ⇒ 互换在物理上是合理的，不是巧合。
 *
 *  min/max 取法：对上表三点做最小二乘拟合后外推到 0/319、0/239。
 *  ⚠️ **没有**采用"画面右下角"那一组（松手值 384/672）—— 两个理由：
 *     ① 松手瞬间手指抬起、接触面积在变，读数本身就不可信；
 *     ② 电阻屏四角是非线性最差的区域。
 *
 *  复核判据：点四角应分别回到 (0,0)/(319,0)/(0,239)/(319,239) ±15；
 *           点页签栏四个页签应分别切到 OVW / CAN / DIAG / CFG。 */
static uint16_t s_x_min = 300u,  s_x_max = 3590u;   /* 约束 raw_y（互换后的 X）*/
static uint16_t s_y_min = 180u,  s_y_max = 3740u;   /* 约束 raw_x（互换后的 Y）*/
static int      s_swap = 1, s_inv_x = 1, s_inv_y = 1;

/* ================= 诊断计数器（2026-10-16 新增） =================
 *
 * 起因：出现"画面在动、就是点不动"，而现有的 g_lvgl_touch_reads 只在
 * **按压 + SPI 读数都成功**时才自增 ⇒ 它为 0 有两种完全不同的病因：
 *     (a) PENIRQ 没通        —— touch_pressed() 恒 0，根本不进 SPI
 *     (b) MISO 悬空/没驱动   —— 按压看到了，但 raw 读到全 0/全 1 被判失败
 * 两者在 RAM 里长得一模一样，只能靠人肉推理（2026-10-13 那次查了一整轮）。
 * 下面这几个量就是为了让"一次 SWD 读"就能定论：
 *
 *     press_seen 在涨 + fail 在涨 + reads 不涨  -> (b) SPI 读数
 *     press_seen 恒 0                            -> (a) PENIRQ
 *     press_seen 在涨 + fail 不涨 + reads 在涨   -> 链路正常，问题在校准/命中
 *
 * ⚠️ 都是 volatile 且只写/只读，不参与任何控制逻辑 —— 加它们不改行为。
 */
volatile uint32_t g_touch_press_seen = 0u;  /* touch_pressed() == 1 的次数 */
volatile uint32_t g_touch_read_fail  = 0u;  /* touch_read_raw() 返回 -1 的次数 */
volatile uint32_t g_touch_fail_reason = 0u; /* 最近一次原因，见下面 REASON_* */
volatile uint32_t g_touch_raw0 = 0u;        /* 最近读到的原始 X（失败也记） */
volatile uint32_t g_touch_raw1 = 0u;        /* 最近读到的原始 Y（失败也记） */

/* ================= 引脚 ================= */

int touch_init(void)
{
    GPIO_InitTypeDef g = { 0 };

    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* T_CS：输出，空闲拉高（不选中）*/
    HAL_GPIO_WritePin(GW_TOUCH_CS_PORT, GW_TOUCH_CS_PIN, GPIO_PIN_SET);
    g.Pin   = GW_TOUCH_CS_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GW_TOUCH_CS_PORT, &g);

    /* T_IRQ：**上拉输入**（PENIRQ 开漏，悬空会读到随机值 —— 见头文件坑 2）*/
    g.Pin  = GW_TOUCH_IRQ_PIN;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GW_TOUCH_IRQ_PORT, &g);

    return 0;
}

int touch_pressed(void)
{
    /* PENIRQ 低有效 */
    int down = (HAL_GPIO_ReadPin(GW_TOUCH_IRQ_PORT, GW_TOUCH_IRQ_PIN)
                == GPIO_PIN_RESET) ? 1 : 0;

    /* ★ 诊断：统计"我看到按压了"的次数。
       ⚠️ 这是区分两种**现象完全相同**的故障的唯一判据（2026-10-16 新增）：
            press_seen 在涨、但 touch_reads 不涨 -> PENIRQ 是好的，
            卡在 SPI 读数（raw 全 0/全 1，典型是 MISO 悬空）
            press_seen 恒为 0                    -> PENIRQ 这一跳没通
          没有它的时候，两种病因在 RAM 里长得一模一样，只能靠人肉推理。 */
    if (down) {
        g_touch_press_seen++;
    }
    return down;
}

/* ================= 原始读取 ================= */

/* 一次转换：发命令 -> 读 2 字节 -> 取 12 位。
   ⚠️ 全程假设已持有总线锁、已把 SPI 降速。 */
static uint16_t xp_convert(uint8_t cmd)
{
    uint8_t tx[3] = { cmd, 0x00u, 0x00u };
    uint8_t rx[3] = { 0, 0, 0 };

    HAL_GPIO_WritePin(GW_TOUCH_CS_PORT, GW_TOUCH_CS_PIN, GPIO_PIN_RESET);
    /* 命令 + 两个空字节的时钟；结果落在 rx[1..2] */
    (void)HAL_SPI_TransmitReceive(&hspi1, tx, rx, 3u, 20u);
    HAL_GPIO_WritePin(GW_TOUCH_CS_PORT, GW_TOUCH_CS_PIN, GPIO_PIN_SET);

    return (uint16_t)((((uint16_t)rx[1] << 8) | rx[2]) >> 3) & 0x0FFFu;
}

static int median3(uint16_t a, uint16_t b, uint16_t c)
{
    uint16_t t;
    if (a > b) { t = a; a = b; b = t; }
    if (b > c) { t = b; b = c; c = t; }
    if (a > b) { t = a; a = b; b = t; }
    return (int)b;
}

int touch_read_raw(uint16_t *raw_x, uint16_t *raw_y)
{
    uint32_t saved;
    uint16_t xs[XP_SAMPLES], ys[XP_SAMPLES];
    uint16_t xmn, xmx, ymn, ymx;
    uint8_t  i;
    int      ret = 0;
    uint32_t reason;               /* 诊断：本次结果的分类，见文件头的说明 */

    if (raw_x == 0 || raw_y == 0) {
        g_touch_fail_reason = 1u;  /* 1 = 传空指针（调用方错误）*/
        g_touch_read_fail++;
        return -1;
    }

    lcd_bus_acquire();
    /* 降速 -> 采样 -> 恢复。restore 一定要做，否则 LCD 会一直慢 8 倍。 */
    saved = hspi1.Init.BaudRatePrescaler;
    hspi1.Init.BaudRatePrescaler = XP_SLOW_PRESCALER;
    (void)HAL_SPI_Init(&hspi1);

    for (i = 0u; i < XP_SAMPLES; i++) {
        xs[i] = xp_convert(XP_CMD_X);
        ys[i] = xp_convert(XP_CMD_Y);
    }

    hspi1.Init.BaudRatePrescaler = saved;
    (void)HAL_SPI_Init(&hspi1);
    lcd_bus_release();

    if (XP_SAMPLES == 3u) {
        *raw_x = (uint16_t)median3(xs[0], xs[1], xs[2]);
        *raw_y = (uint16_t)median3(ys[0], ys[1], ys[2]);
    } else {
        *raw_x = xs[0];
        *raw_y = ys[0];
    }

    /* 一致性检查：三次采样差太远说明手指在动或读飞了，这一拍丢弃 */
    xmn = xmx = xs[0];
    ymn = ymx = ys[0];
    for (i = 1u; i < XP_SAMPLES; i++) {
        if (xs[i] < xmn) { xmn = xs[i]; }
        if (xs[i] > xmx) { xmx = xs[i]; }
        if (ys[i] < ymn) { ymn = ys[i]; }
        if (ys[i] > ymx) { ymx = ys[i]; }
    }

    /* 诊断：原始值**失败也记** —— 能看到"读到的到底是什么"，这是判 MISO 的关键。
       ⚠️ 必须放在两个判据**之前**：失败的那一拍才是最有价值的证据。 */
    g_touch_raw0 = (uint32_t)*raw_x;
    g_touch_raw1 = (uint32_t)*raw_y;

    /* 判据 1：三次采样差太远说明手指在动或读飞了，这一拍丢弃 */
    if ((uint16_t)(xmx - xmn) > XP_SPREAD_MAX
        || (uint16_t)(ymx - ymn) > XP_SPREAD_MAX) {
        ret = -1;
        reason = 2u;                   /* 2 = 三次采样离散过大（手指在动）*/
    }
    /* 判据 2：全 0 / 全 1 通常是屏幕没接或 MISO 悬空 */
    else if (*raw_x == 0u || *raw_x == 0x0FFFu
             || *raw_y == 0u || *raw_y == 0x0FFFu) {
        ret = -1;
        reason = 3u;                   /* 3 = 全 0/全 1（MISO 没在驱动）*/
    } else {
        reason = 0u;                   /* 成功 */
    }

    if (ret != 0) {
        g_touch_read_fail++;
    }
    g_touch_fail_reason = reason;
    return ret;
}

static int scale(uint16_t v, uint16_t lo, uint16_t hi, uint16_t out_max)
{
    int r;
    if (hi <= lo) {
        return 0;
    }
    r = (int)(((int32_t)(v - lo) * (int32_t)out_max) / (int32_t)(hi - lo));
    if (r < 0) { r = 0; }
    if (r > (int)out_max) { r = (int)out_max; }
    return r;
}

int touch_get(uint16_t *scr_x, uint16_t *scr_y)
{
    uint16_t rx, ry;
    int x, y;

    if (scr_x == 0 || scr_y == 0) {
        return 0;
    }
    if (!touch_pressed()) {
        /* 诊断：PENIRQ 这一跳没通 —— 与"SPI 读不到"区分开的关键分支。
           ⚠️ 这个分支**不**动 g_touch_read_fail：那只统计"进了 SPI 但失败"，
              否则两种病因的计数会混在一起，又回到分不清的状态。 */
        g_touch_fail_reason = 5u;      /* 5 = 没有按压（PENIRQ 为高）*/
        return 0;
    }
    if (touch_read_raw(&rx, &ry) != 0) {
        return 0;
    }

    if (s_swap) {
        uint16_t t = rx; rx = ry; ry = t;
    }
    x = scale(rx, s_x_min, s_x_max, LCD_W - 1u);
    y = scale(ry, s_y_min, s_y_max, LCD_H - 1u);
    if (s_inv_x) { x = (int)(LCD_W - 1u) - x; }
    if (s_inv_y) { y = (int)(LCD_H - 1u) - y; }

    *scr_x = (uint16_t)x;
    *scr_y = (uint16_t)y;
    return 1;
}

void touch_set_calib(uint16_t x_min, uint16_t x_max,
                     uint16_t y_min, uint16_t y_max,
                     int swap_xy, int invert_x, int invert_y)
{
    s_x_min = x_min; s_x_max = x_max;
    s_y_min = y_min; s_y_max = y_max;
    s_swap = swap_xy; s_inv_x = invert_x; s_inv_y = invert_y;
}
