/* ==================================================================
 *  ILI9341 驱动实现（SPI1 轮询）—— 见 lcd_ili9341.h 的设计约束
 * ================================================================== */
#include "stm32f1xx_hal.h"      /* ⚠️ 必须在 gw_config.h 之前（见它的包含顺序检查）*/
#include "gw_config.h"          /* 引脚宏以本仓库为准（GW_LCD_*）*/
#include "lcd_ili9341.h"
#include "lcd_font_8x16.h"

/* ⚠️ 为什么用 gw_config.h 而不是 CubeMX 的 main.h：
 *   CI 的交叉编译只带 `-Iinc -Iport -Idrivers -Iapp`（**不含工程的 Core/Inc**），
 *   所以 port 层不能依赖 CubeMX 生成的标签名，否则 CI 直接编译不过。
 *   这里把仓库登记的 GW_LCD_* 映射成本文件内部用的名字 ——
 *   好处是将来 CubeMX 若改了标签名，本驱动完全不受影响。
 *   （gw_config.h:154 就是"引脚已被屏幕占用"的登记处。）*/
#define LCD_CS_GPIO_Port    GW_LCD_CS_PORT
#define LCD_CS_Pin          GW_LCD_CS_PIN
#define LCD_DC_GPIO_Port    GW_LCD_DC_PORT
#define LCD_DC_Pin          GW_LCD_DC_PIN
#define LCD_RST_GPIO_Port   GW_LCD_RST_PORT
#define LCD_RST_Pin         GW_LCD_RST_PIN
#define LCD_BL_GPIO_Port    GW_LCD_BL_PORT
#define LCD_BL_Pin          GW_LCD_BL_PIN

/* CubeMX 在 spi.c 里定义、spi.h 里声明。这里只做声明，
   理由同上：port 层不 include 工程的 Core/Inc。 */
extern SPI_HandleTypeDef hspi1;
extern volatile uint32_t g_prog;  /* 临时诊断 */

/* ---- 分片大小：单次 SPI 传输最多多少个像素 ----
 * 64 像素 = 128 字节栈缓冲。整屏 320x240 由外层循环分片完成，
 * 所以**单次调用占用的栈是常数**，与画面大小无关。 */
#define LCD_CHUNK_PX     64u

/* ---- 文本按行缓冲：一次最多处理多少个字符 ----
 * 受栈约束（HOUSE 的栈只有 256 字 = 1KB），所以这里刻意取小：
 * 20 字符 x 8 像素 x 2 字节 = 320 字节。更长的串由外层分片重画。 */
#define LCD_TEXT_CHUNK   20u

/* ================= 低层：命令/数据 ================= */

static inline void cs_low(void)
{
    HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_RESET);
}

static inline void cs_high(void)
{
    HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
}

static inline void dc_cmd(void)
{
    HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_RESET);
}

static inline void dc_data(void)
{
    HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_SET);
}

/* 超时统一用一个保守值：9MHz 下 128 字节约 114us，给 100ms 余量足够，
   而且**有界**（不会因为屏幕没接就把 UI 任务挂死）。 */
#define LCD_SPI_TIMEOUT  100u

/* ⚠️ 2026-10-12：这里**不能**用 HAL_Delay —— 实测在本系统里时基(uwTick)会在
   某个环节停止前进，导致 HAL_Delay 永久卡死（进度码证明卡在 hw_reset 的第一个
   lcd_delay_ms(50)，而更早的相同代码是好的）。面板只需要 50~120ms 的等待，
   用 DWT 周期计数忙等即可，与 HAL 时基彻底解耦。 */
/* 喂狗的弱钩子：由工程侧给强定义（HAL_IWDG_Refresh）。
   放在 lcd_delay_ms 里调用 = 任何 LCD 等待都顺带喂狗
   （L0 设计原则"长阻塞路径自己喂狗"；驱动本身仍不依赖看门狗）。 */
__attribute__((weak)) void lcd_watchdog_feed(void) { }

void lcd_delay_ms(uint32_t ms)
{
    uint32_t start;
    uint32_t ticks;

    lcd_watchdog_feed();

    /* 使能 DWT 周期计数器（幂等；gw_dwt_init 也会做，这里自保）*/
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    ticks = ms * (SystemCoreClock / 1000u);   /* 72MHz 时 1ms = 72000 周期 */
    start = DWT->CYCCNT;
    while ((uint32_t)(DWT->CYCCNT - start) < ticks) {
        /* busy wait */
    }
}

/* ⚠️ CS 处理按 martnak/STM32-ILI9341（MIT）对齐（2026-10-12）：
   它的 Write_Command/Write_Data 是**每传一个字节就拉低 CS、发完拉高**；
   我们原来是整段保持 CS 低。逐字节翻转更宽容，照它改；
   像素突发按块翻转（与它的 Draw_Colour_Burst 一致）。 */
static void wr_cmd(uint8_t c)
{
    dc_cmd();
    cs_low();
    (void)HAL_SPI_Transmit(&hspi1, &c, 1u, LCD_SPI_TIMEOUT);
    cs_high();
    g_prog = 0x66u;
}

static void wr_data8(uint8_t d)
{
    dc_data();
    cs_low();
    (void)HAL_SPI_Transmit(&hspi1, &d, 1u, LCD_SPI_TIMEOUT);
    cs_high();
}

static void wr_buf(const uint8_t *p, uint16_t n)
{
    dc_data();
    cs_low();
    (void)HAL_SPI_Transmit(&hspi1, (uint8_t *)p, n, LCD_SPI_TIMEOUT);
    cs_high();
}

/* 设置绘制窗口（列/页地址 + 写显存命令） */
/* 面板的**真实**像素尺寸。⚠️ 不要用 LCD_W/LCD_H（那两个是界面版式，
   我们按 320x240 横屏设计；面板实际是 240x320 竖屏）—— 窗口越界会让面板不显示。 */
#define LCD_PANEL_W  320u
#define LCD_PANEL_H  240u

static void set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint8_t b[4];
    uint16_t x1, y1;

    /* ⚠️ 裁剪到面板真实尺寸（2026-10-12）：界面按 320 宽绘制，若直接把 319
       写进 CASET，面板列范围只有 0..239 => 地址越界 => 整屏不显示（实测）。*/
    if (x >= LCD_PANEL_W || y >= LCD_PANEL_H) { return; }
    if ((uint32_t)x + w > LCD_PANEL_W) { w = (uint16_t)(LCD_PANEL_W - x); }
    if ((uint32_t)y + h > LCD_PANEL_H) { h = (uint16_t)(LCD_PANEL_H - y); }

    x1 = (uint16_t)(x + w - 1u);
    y1 = (uint16_t)(y + h - 1u);

    wr_cmd(0x2Au);                       /* CASET */
    b[0] = (uint8_t)(x >> 8);  b[1] = (uint8_t)x;
    b[2] = (uint8_t)(x1 >> 8); b[3] = (uint8_t)x1;
    wr_buf(b, 4u);

    wr_cmd(0x2Bu);                       /* RASET */
    b[0] = (uint8_t)(y >> 8);  b[1] = (uint8_t)y;
    b[2] = (uint8_t)(y1 >> 8); b[3] = (uint8_t)y1;
    wr_buf(b, 4u);

    wr_cmd(0x2Cu);                       /* RAMWR */
}

/* ================= 初始化 ================= */

int lcd_read_id(uint32_t *id)
{
    uint8_t  cmd = 0x04u;
    uint8_t  rx[4] = { 0, 0, 0, 0 };
    uint32_t saved;
    int      ok = -1;

    /* ⚠️ 2026-10-13 修（缺陷 AG）：原来是
     *       wr_cmd(0x04u); dc_data(); cs_low();
     *   而 wr_cmd() 内部是"发完一个字节就抬 CS"（见上面 wr_cmd 的说明，
     *   那是照 martnak 的逐字节翻转写法改的）——于是 CS 在"命令字节"与
     *   "读数据"之间**被抬起过一次**。ILI9341 的接口状态机在 CS 上升沿复位，
     *   RDDID 的读周期随之作废，后面收到的 4 个字节必然是 00 00 00 00。
     *   ⇒ 这个自检**无论接线对不对都读不到 ID**，它给出的"读不到"是假证据。
     *   修法：**读类命令不能沿用逐字节翻转**，命令与读共用一次 CS 低。
     *
     * 同时把 SPI 降速：ILI9341 的读周期 tRC 最小值约 160ns（≈6.25MHz），
     * 而总线跑在 9MHz（APB2/8）。直接读违反读时序，读回的值不可信。
     * 读完立刻恢复，别让 LCD 一直慢 8 倍。 */
    lcd_bus_acquire();
    g_prog = 0x65u;

    saved = hspi1.Init.BaudRatePrescaler;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_64;   /* 72/64 = 1.125MHz */
    (void)HAL_SPI_Init(&hspi1);

    dc_cmd();
    cs_low();
    (void)HAL_SPI_Transmit(&hspi1, &cmd, 1u, LCD_SPI_TIMEOUT);

    dc_data();
    /* RDDID(0x04) 之后要先来一个空字节的时钟，ID 落在 rx[1..3]（00h 93h 41h）*/
    if (HAL_SPI_Receive(&hspi1, rx, 4u, LCD_SPI_TIMEOUT) == HAL_OK) {
        if (id != 0) {
            *id = ((uint32_t)rx[1] << 16) | ((uint32_t)rx[2] << 8) | rx[3];
        }
        ok = 0;
    }
    cs_high();

    hspi1.Init.BaudRatePrescaler = saved;
    (void)HAL_SPI_Init(&hspi1);

    lcd_bus_release();
    g_prog = 0x6Bu;
    return ok;
}

/* ⚠️ 时序按 martnak/STM32-ILI9341（MIT, (c) 2017 Matej Artnak）的
   ILI9341_Reset() 对齐（2026-10-12）：
   https://github.com/martnak/STM32-ILI9341
   RST 低 -> 200ms -> CS 低 -> 200ms -> RST 高。原来只有 20ms + 120ms，
   对某些批次的面板偏短（上电时序没走完 -> 命令收下但不显示 = 背光亮却全黑）。 */
/* ⚠️ 2026-10-12：初始化序列**原样照搬用户自己跑通过的那份驱动**
   （D:\stm32CubeMX\test_lcd\Core\Src\ili9341.c 的 ILI9341_Init()）。原因：
   本模块（红色 2.4" TFT SPI 240x320）在"通用库的长序列"
   （0xEF/0xCF/0xED/0xE8/0xCB/0xF7/0xC0/0xC1/0xC5/0xC7/0xB1/0xB6/0xF2/0x26/0xE0/0xE1）
   下**完全不显示**（背光亮、屏全白）；而用户那份**只发 5 条命令**的最小序列
   在同一块屏、同一套接线上**能正常显示** —— 所以以它为准：

       RST 低 50ms -> RST 高 50ms
       0x01 SWRESET + 120ms
       0x11 SLPOUT  + 120ms
       0x36 MADCTL  = 0x48      （与用户驱动一致；想横屏 320x240 改成 0x28）
       0x3A COLMOD  = 0x55      （16bpp RGB565）
       0x29 DISPON  + 50ms

   另外两处对齐用户驱动：
     ① 硬复位**不加 CS 低电平阶段**（用户驱动没有）；
     ② 读 ID 从"初始化之前"挪到"初始化之后"（用户驱动不读 ID；在未初始化的
        面板上先发 0x04 可能把它带进错误状态）。读 ID 仍只作诊断，读不到不阻断。 */
static void hw_reset(void)
{
    HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin, GPIO_PIN_RESET);
    g_prog = 0x61u;
    lcd_delay_ms(50u);
    g_prog = 0x62u;
    HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin, GPIO_PIN_SET);
    g_prog = 0x63u;
    lcd_delay_ms(50u);
}

/* 最后一次 RDDID 读到的原始值（非 static：便于 SWD 读，也显示在诊断页）*/
uint32_t g_lcd_last_id = 0u;
int      g_lcd_id_ok   = 0;

int lcd_init(void)
{
    lcd_bus_acquire();
    cs_high();                            /* 空闲时片选拉高 */

    hw_reset();
    g_prog = 0x67u;

    wr_cmd(0x01u);                        /* SWRESET */
    g_prog = 0x68u;
    lcd_delay_ms(120u);

    wr_cmd(0x11u);                        /* Sleep out */
    g_prog = 0x69u;
    lcd_delay_ms(120u);

    /* ⚠️ MADCTL：0x48 与用户驱动一致（竖屏 240x320）。
       若上板后要横屏，改这一个字节为 0x28；镜像/红蓝反可试 0xE8/0x88。 */
    /* ⚠️ 2026-10-12：0x28 = MV|BGR 横屏 320x240，与我们的界面/窗口一致。
       原先跟用户驱动用 0x48（竖屏 240x320），但我们的填充会写到 x=319，
       超出面板 0..239 的列范围 => 地址窗口越界 => 不显示。
       若上板后画面旋转/镜像，依次试 0xE8 / 0x88。 */
    /* ⚠️ 2026-10-12：回到用户驱动验证过的 0x48（竖屏 240x320）。
       本面板真实是 240x320；界面按 320 宽写窗口会越界 => 不显示。
       配合下面的 set_window 裁剪，界面的左边 240 列应能正常显示。 */
    /* ⚠️ 2026-10-12：横屏 0x28（MV|BGR, 320x240）—— 与界面版式一致，也是上板
       实测通过（屏幕完整显示、方向正确）的配置。0x48 是竖屏 240x320，
       界面按 320 宽画会被裁掉右边约 80 列。
       若以后要竖屏，改回 0x48 并同步把下面的 LCD_PANEL_W/H 换成 240/320。 */
    wr_cmd(0x36u); wr_data8(0xE8u);

    wr_cmd(0x3Au); wr_data8(0x55u);       /* 16bpp RGB565 */

    wr_cmd(0x29u);                        /* Display ON */
    g_prog = 0x6Au;
    lcd_delay_ms(50u);

    /* ⚠️ 2026-10-12：这里原先在 DISPON 之后发 0x04 读 RDDID 作诊断。
       用户跑通过的 test_lcd/ili9341.c **完全不读 ID**，而这是本初始化里唯一多做、
       且会改变面板状态的动作 —— 实测就是它让屏不显示。故删除。
       g_lcd_last_id / g_lcd_id_ok 保留（诊断页仍显示，只是值为 0 / '----'）。 */
    g_lcd_last_id = 0u;
    g_lcd_id_ok   = 0;


    cs_high();
    lcd_bus_release();
    return 0;
}

/* ================= 图元 ================= */

/* ---- 填充通路诊断计数器（2026-10-13 加）----
 *
 *  ⚠️ 为什么需要：`lcd_text()` 画得出来，而 `lcd_fill_rect()` 的整块填充
 *     **完全没写进屏幕** —— 症状是"切页之后旧页面还在、新页面直接叠上去"
 *     （照片实证：标题栏里新旧两个标题重叠；总览页的 TEMP RANGE / HUMI RANGE
 *     在切到 CAN 页之后仍然可见）。要一次分清是下面哪一种：
 *       ① HAL 压根没把数据发出去（返回码不是 HAL_OK）-> g_lcd_tx_bad 涨
 *       ② 数据发出去了但面板不认（CS/时序）-> g_lcd_tx_ok 涨而屏幕依旧不变
 *
 *  ⚠️ 2026-10-13 决定**保留**（不删）：代价是 4 个 uint32（共 16 B RAM），
 *     收益是以后再遇到"色块 / 背景画不出来"能一眼分清是"没发出去"还是"发出去但面板不认"。
 *     这四个量没有任何功能依赖，真要删也不影响任何行为。 */
volatile uint32_t g_lcd_fill_calls = 0u;   /* lcd_fill_rect 被调用次数 */
volatile uint32_t g_lcd_tx_ok      = 0u;   /* HAL_SPI_Transmit 返回 HAL_OK 次数 */
volatile uint32_t g_lcd_tx_bad     = 0u;   /* 返回非 HAL_OK 次数 */
volatile uint32_t g_lcd_px_sent    = 0u;   /* 累计已"成功发出"的像素数 */

/**
 * 把一块 RGB565 像素写进矩形区域（LVGL 等图形库刷新的落点）
 *
 * ⚠️⚠️ 顺序陷阱（2026-10-13 现场踩出来的，见下面 lcd_fill_rect 的注释）：
 *    `set_window()` 内部靠 wr_cmd()/wr_buf() 发 CASET/RASET/RAMWR，
 *    而这两个函数**每发一次就抬一次 CS** ⇒ set_window() 返回时 CS 是高电平。
 *    所以必须**在 set_window() 之后重新 cs_low()**，让整段像素流都在 CS 低电平下完成。
 *    顺序写反的后果是"调用返回成功、屏幕毫无变化"（最难查的一类）。
 */
void lcd_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
              const uint16_t *px)
{
    uint32_t       left;
    const uint8_t *src;

    if (px == 0 || w == 0u || h == 0u
        || x >= LCD_W || y >= LCD_H) {
        return;
    }
    if ((uint32_t)x + w > LCD_W) { w = (uint16_t)(LCD_W - x); }
    if ((uint32_t)y + h > LCD_H) { h = (uint16_t)(LCD_H - y); }

    lcd_bus_acquire();
    set_window(x, y, w, h);
    cs_low();                         /* ⚠️ 必须在 set_window 之后（见上面的说明）*/

    dc_data();                        /* 整段像素流：DC 高、CS 一直 hold 住 */
    src  = (const uint8_t *)px;
    left = (uint32_t)w * h;
    while (left > 0u) {
        uint16_t n = (left > 64u) ? 64u : (uint16_t)left;   /* 64 像素 = 128 字节 */
        if (HAL_SPI_Transmit(&hspi1, (uint8_t *)src,
                             (uint16_t)(n * 2u), LCD_SPI_TIMEOUT) == HAL_OK) {
            g_lcd_tx_ok++;
            g_lcd_px_sent += n;
        } else {
            g_lcd_tx_bad++;
        }
        src  += (uint32_t)n * 2u;
        left -= n;
    }
    g_lcd_fill_calls++;

    cs_high();
    lcd_bus_release();
}

void lcd_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                   uint16_t color)
{
    uint8_t chunk[LCD_CHUNK_PX * 2u];
    uint32_t left;
    uint16_t i;

    g_lcd_fill_calls++;

    if (w == 0u || h == 0u || x >= LCD_W || y >= LCD_H) {
        return;
    }
    if ((uint32_t)x + w > LCD_W) { w = (uint16_t)(LCD_W - x); }
    if ((uint32_t)y + h > LCD_H) { h = (uint16_t)(LCD_H - y); }

    for (i = 0u; i < LCD_CHUNK_PX; i++) {            /* 一次填好分片内容 */
        chunk[i * 2u]      = (uint8_t)(color >> 8);
        chunk[i * 2u + 1u] = (uint8_t)color;
    }

    lcd_bus_acquire();
    cs_low();
    set_window(x, y, w, h);

    /* ⚠️⚠️ 2026-10-13 修复（这是"整块填充不生效"的根因）：
     *
     *   `set_window()` 内部是靠 `wr_cmd()` / `wr_buf()` 发 CASET/RASET/RAMWR 的，
     *   而这两个函数**每发一次就抬一次 CS**（`cs_low(); transmit; cs_high();`）。
     *   所以 `set_window()` 返回时 CS 已经是**高**的 ——
     *   上面那句 `cs_low()` 被它自己吃掉了，整段像素流是在 **CS 高** 的状态下发出去的，
     *   面板根本不收（数据全丢，屏幕上看不到任何变化）。
     *
     *   而 `lcd_text()` 之所以正常，正是因为它**每行都走 `wr_buf()`**，
     *   而 `wr_buf()` 自己会 `cs_low()` —— 像素期间 CS 是低的。
     *
     *   证据（照片）：`ui_title()` 画标题前先用本函数把标题栏填成蓝色，
     *   如果填充生效旧标题必被盖掉；实测新旧两个标题**重叠**在一起
     *   ⇒ 本函数全程没写进屏幕。修法：`set_window()` 之后**重新拉低 CS**，
     *   让整段像素流（含中间的分片循环）都在 CS 低电平下完成 ——
     *   这正是下面那句旧注释本来想表达的意图。 */
    cs_low();

    left = (uint32_t)w * h;
    dc_data();                        /* 整段像素流：DC 高、CS 由上面那次 cs_low() 一直 hold 住 */
    while (left > 0u) {
        uint16_t n = (left > LCD_CHUNK_PX) ? LCD_CHUNK_PX : (uint16_t)left;
        /* ⚠️ 2026-10-12：这里以前调 wr_buf()，而它**每个分片都会抬一次 CS**
           （1200 次 CS 脉冲）。实测：手工"CS 一直拉低"传像素能显示，走这条路径完全
           不显示 —— 兼容屏在像素流中途抬 CS 会中断/重新同步，数据全丢。
           所以改成直接 HAL_SPI_Transmit，CS 由外层一直 hold 住。 */
        if (HAL_SPI_Transmit(&hspi1, chunk, (uint16_t)(n * 2u),
                             LCD_SPI_TIMEOUT) == HAL_OK) {
            g_lcd_tx_ok++;
            g_lcd_px_sent += n;
        } else {
            g_lcd_tx_bad++;
        }
        left -= n;
    }

    cs_high();
    lcd_bus_release();
}

void lcd_fill(uint16_t color)
{
    lcd_fill_rect(0u, 0u, LCD_W, LCD_H, color);
}

void lcd_frame(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (w < 2u || h < 2u) {
        return;
    }
    lcd_fill_rect(x, y, w, 1u, color);
    lcd_fill_rect(x, (uint16_t)(y + h - 1u), w, 1u, color);
    lcd_fill_rect(x, y, 1u, h, color);
    lcd_fill_rect((uint16_t)(x + w - 1u), y, 1u, h, color);
}

uint16_t lcd_text(uint16_t x, uint16_t y, const char *s,
                  uint16_t fg, uint16_t bg)
{
    uint16_t drawn = 0u;
    /* ⚠️ 2026-10-13 修复（缺陷 AJ）：这里以前是 `uint16_t line[...]`，
       最后靠 `wr_buf((const uint8_t *)line, ...)` 把数组**按本机小端序**当字节流发出去。
       而 ILI9341 的 16bpp SPI 像素流是**高字节在前（大端）**——
       于是所有文字颜色都被**字节颠倒**了：`lcd_fill_rect()` 用大端（见本文件里
       它逐字节填 `color >> 8` / `color`），所以填充色是对的，文字色全错。

       实机症状（照片实证）：
         UI_BLUE  0x001F -> 0x1F00 = 亮绿   ⇒ 标题栏/选中页签的文字底变成**绿块**
         UI_DIM   0x8410 -> 0x1084 ≈ 全黑   ⇒ 次要标签**几乎看不见**（用户报的"看不清"）
         UI_OK    0x07E0 -> 0xE007 = 品红   ⇒ "iwdg ok" / "BUS OK" 变成粉色
         UI_VAL   0x07FF -> 0xFF07 = 黄     ⇒ 数值变成黄色
         UI_ERR   0xF800 -> 0x00F8 = 蓝     ⇒ 红色告警变成蓝色
         UI_FG    0xFFFF -> 0xFFFF = 白     ⇒ 只有白色"看起来正常"（对称，掩盖了 bug）
       互证：同一个 UI_OK 常量，`ui_dot()` 走 lcd_fill_rect 显示为**绿**，
       `ui_cell()` 走本函数显示为**粉** —— 两条通路两种结果，锁定在本函数。

       修法：行缓冲改成 uint8_t 字节流，**手工按大端**写入（高字节在前），
       与 lcd_fill_rect 的口径完全一致。缓冲总大小不变（20*8*2 = 320 字节）。 */
    uint8_t  line[LCD_TEXT_CHUNK * LCD_FONT_W * 2u];
    uint16_t n, row, col;

    if (s == 0 || x >= LCD_W || y >= LCD_H) {
        return 0u;
    }

    while (*s != '\0' && x + LCD_FONT_W <= LCD_W) {
        /* 先把这一片（最多 LCD_TEXT_CHUNK 个字符）拼进行缓冲 */
        n = 0u;
        while (n < LCD_TEXT_CHUNK && s[n] != '\0'
               && (uint32_t)x + (n + 1u) * LCD_FONT_W <= LCD_W) {
            n++;
        }
        if (n == 0u) {
            break;
        }

        lcd_bus_acquire();
        cs_low();
        set_window(x, y, (uint16_t)(n * LCD_FONT_W), LCD_FONT_H);

        for (row = 0u; row < LCD_FONT_H; row++) {
            uint16_t px = 0u;            /* 注意：这里是**字节**下标，不是像素下标 */
            for (col = 0u; col < n; col++) {
                uint8_t ch = (uint8_t)s[col];
                const uint8_t *g;
                uint8_t bits = 0u;

                if (ch >= LCD_FONT_FIRST
                    && ch < (uint8_t)(LCD_FONT_FIRST + LCD_FONT_CHARS)) {
                    g = g_lcd_font8x16[ch - LCD_FONT_FIRST];
                    bits = g[row];
                }
                for (uint8_t b = 0u; b < LCD_FONT_W; b++) {
                    uint16_t c = (bits & (0x80u >> b)) ? fg : bg;
                    line[px++] = (uint8_t)(c >> 8);      /* 高字节在前（大端） */
                    line[px++] = (uint8_t)(c & 0xFFu);
                }
            }
            wr_buf(line, px);
        }

        cs_high();
        lcd_bus_release();

        drawn = (uint16_t)(drawn + n);
        s += n;
        x = (uint16_t)(x + n * LCD_FONT_W);
    }
    return drawn;
}

void lcd_backlight(int on)
{
    HAL_GPIO_WritePin(LCD_BL_GPIO_Port, LCD_BL_Pin,
                      on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* ---- 弱符号总线锁：裸机下是空实现；RTOS 侧用互斥量覆盖 ---- */
__attribute__((weak)) void lcd_bus_acquire(void) { }
__attribute__((weak)) void lcd_bus_release(void) { }
