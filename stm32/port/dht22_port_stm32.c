/**
 * @file    dht22_port_stm32.c
 * @brief   DHT22 在 STM32F103 上的硬件实现（PG11 单总线 + DWT 微秒计时）
 *
 * ============================================================================
 *  本文件是"纯逻辑（drivers/dht22.c）"与"真实硬件"之间的那层胶水。
 *
 *  纯逻辑部分已经用假引脚在 PC 上完整测试过（57 项），
 *  所以这里只剩两件事需要保证：
 *    1) 引脚方向切换与电平读写正确
 *    2) 微秒延时足够准（DWT，误差约 1 个时钟周期）
 *
 *  为什么直接操作寄存器而不调 HAL_GPIO_WritePin？
 *    DHT22 的位宽只有 26µs / 70µs，而 HAL_GPIO_WritePin 要做参数校验、
 *    位带/BSRR 计算等一堆工作，在 72MHz 下开销可达数百纳秒；
 *    位内还有 if/while 判断，累积误差会挤占时序余量。
 *    直接用 BSRR/BRR 是一条 STR 指令（约 14ns），把余量全留给时序。
 *    这是"微秒级时序必须绕开 HAL"的典型场景，面试可以讲这个取舍。
 *
 *  ⚠️ 单总线是开漏结构：
 *      - 主机只能"拉低"或"释放（高阻）"，不能主动输出高电平
 *      - "释放"后由外部上拉电阻把线拉高（DHT22 模块通常已自带 4.7k~10k 上拉）
 *      - 所以输出模式必须配成**开漏输出**（GPIO_MODE_OUTPUT_OD），
 *        配成推挽的话，主机输出高电平时会和传感器的拉低直接对撞，
 *        轻则通信失败，重则损坏 IO。
 * ============================================================================
 */
#include "dht22_port_stm32.h"

/* 包含顺序要求：gw_config.h 使用 HAL 的 GPIO_PIN_n 宏与 GPIOA/PG 等端口宏
   （引脚分配 + 编译期断言），因此必须先包含 HAL 头再包含它。
   gw_config.h 里有守卫会明确提示这一点。 */
#include "stm32f1xx_hal.h"
#include "gw_config.h"
#include "gw_dwt.h"

/* ------------------------------------------------------------------
 * 引脚封装
 *
 * 为什么把"引脚号"和"CRH 配置位"分开存？
 *   GPIO 的配置寄存器分 CRL（pin0~7）与 CRH（pin8~15），
 *   每个引脚占 4 位。PG11 属于 CRH，位偏移 = (11-8)*4 = 12。
 *   把偏移在初始化时算好，运行时切换方向只需读改写 4 位，开销最小。
 * ------------------------------------------------------------------ */
typedef struct {
    GPIO_TypeDef *port;
    uint16_t      pin;          /* GPIO_PIN_n（位掩码） */
    uint8_t       cr_shift;     /* 在 CRL/CRH 中的位偏移 */
    volatile uint32_t *cr;      /* 指向 CRL 或 CRH */
    uint8_t       ready;
} dht_pin_t;

static dht_pin_t    s_pin;
static dht22_hw_t   s_hw;

/* ------------------------------------------------------------------
 * 内部：引脚操作
 * ------------------------------------------------------------------ */

/* 切为开漏输出（主机可拉低 / 释放） */
static void dht_set_out(void *ctx)
{
    dht_pin_t *p = (dht_pin_t *)ctx;
    uint32_t  cr = *p->cr;

    cr &= ~(0xFu << p->cr_shift);
    cr |=  ((uint32_t)0x7u << p->cr_shift);   /* 0b0111 = 通用开漏输出, 50MHz */
    *p->cr = cr;
}

/* 切为输入（读取传感器驱动的电平）。
   配上拉输入：即使模块没自带外部上拉，也能有确定的高电平；
   若模块已有外部上拉，内部上拉只是并联，不会有害。 */
static void dht_set_in(void *ctx)
{
    dht_pin_t *p = (dht_pin_t *)ctx;
    uint32_t  cr = *p->cr;

    cr &= ~(0xFu << p->cr_shift);
    cr |=  ((uint32_t)0x8u << p->cr_shift);   /* 0b1000 = 输入, 上拉/下拉 */
    *p->cr = cr;

    /* 选择上拉（ODR 对应位置 1 表示上拉，置 0 表示下拉） */
    p->port->ODR |= (uint32_t)p->pin;
}

/* 输出电平：1 = 释放（高阻，由上拉拉高）；0 = 主动拉低。
   开漏模式下"输出 1"等价于释放总线，这正是单总线需要的语义。 */
static void dht_write(void *ctx, int level)
{
    dht_pin_t *p = (dht_pin_t *)ctx;

    if (level) {
        p->port->BSRR = (uint32_t)p->pin;       /* 置位 ODR -> 释放 */
    } else {
        p->port->BRR  = (uint32_t)p->pin;       /* 清位 ODR -> 拉低 */
    }
}

/* 读引脚电平。用 IDR（输入数据寄存器）而不是 ODR ——
   开漏输出时 ODR 反映的是"我们想输出什么"，不是"线上实际是什么"。 */
static int dht_read(void *ctx)
{
    dht_pin_t *p = (dht_pin_t *)ctx;
    return ((p->port->IDR & (uint32_t)p->pin) != 0u) ? 1 : 0;
}

static void dht_delay_us(void *ctx, uint32_t us)
{
    (void)ctx;
    gw_dwt_delay_us(us);
}

static uint32_t dht_now_us(void *ctx)
{
    (void)ctx;
    return gw_dwt_us();
}

/* ------------------------------------------------------------------
 * 临界区（缺陷 AF，2026-10-13）
 *
 * ⚠️ 为什么用 CMSIS 的 PRIMASK 原语，而不是 FreeRTOS 的
 *    `taskENTER_CRITICAL()`：
 *      port/gw_rtos.h 明确写着"本文件是**唯一**依赖 FreeRTOS 的文件"，
 *      业务层与移植层都不认识 FreeRTOS。为了让这条架构约束不被打破，
 *      这里只用一个"关中断"的语义 —— 而 PRIMASK 正是它的最小实现，
 *      并且对裸机 / RTOS 两种构建都成立（裸机下同样正确）。
 *
 * ⚠️ 为什么不是裸的 `__disable_irq()` + `__enable_irq()`：
 *      如果调用点外层已经关着中断（例如某个外层临界区），
 *      无条件 `__enable_irq()` 会**提前打开**中断，把外层临界区破坏掉。
 *      所以这里保存/恢复 PRIMASK —— 只恢复到自己进来之前的状态。
 *
 * ⚠️ 保存值用一个文件级 static：DHT22 采样只由 SENSOR 任务单线程调用，
 *    且 enter/exit 严格嵌套（驱动保证成对、中间无早退），所以不会交错。
 *    若将来出现"多处并发采样"，这里要改成每实例/栈上保存。
 * ------------------------------------------------------------------ */
static uint32_t s_primask_saved;

static void dht_enter_critical(void *ctx)
{
    (void)ctx;
    s_primask_saved = __get_PRIMASK();
    __disable_irq();
}

static void dht_exit_critical(void *ctx)
{
    (void)ctx;
    __set_PRIMASK(s_primask_saved);
}

/* ------------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------------ */

int dht22_stm32_init(void)
{
    GPIO_InitTypeDef gpio;

    gw_dwt_init();
    if (gw_dwt_is_ready() == 0) {
        /* 没有可靠的微秒计时，DHT22 的位宽判定必然失准。
           这里明确返回失败，让上层能感知并禁用该传感器，
           而不是"看起来在跑但数据全是错的"。 */
        s_pin.ready = 0u;
        return -1;
    }

    /* 使能 GPIOG 时钟（PG11 在 GPIOG 上） */
    __HAL_RCC_GPIOG_CLK_ENABLE();

    /* 引脚先配成开漏输出、输出高（释放总线）。
       注意：如果模块没接，内部上拉会把线拉高，dht22_read 会返回
       STUCK_HIGH —— 这正是"未接线"的准确诊断。 */
    gpio.Pin   = GW_DHT22_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_OD;
    gpio.Pull  = GPIO_PULLUP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GW_DHT22_PORT, &gpio);
    HAL_GPIO_WritePin(GW_DHT22_PORT, GW_DHT22_PIN, GPIO_PIN_SET);

    /* 缓存寄存器位置，运行时切换方向不再查表 */
    s_pin.port     = GW_DHT22_PORT;
    s_pin.pin      = GW_DHT22_PIN;
    s_pin.cr_shift = (uint8_t)(((GW_DHT22_PIN == GPIO_PIN_8)  ? 0u :
                                (GW_DHT22_PIN == GPIO_PIN_9)  ? 4u :
                                (GW_DHT22_PIN == GPIO_PIN_10) ? 8u :
                                (GW_DHT22_PIN == GPIO_PIN_11) ? 12u :
                                (GW_DHT22_PIN == GPIO_PIN_12) ? 16u :
                                (GW_DHT22_PIN == GPIO_PIN_13) ? 20u :
                                (GW_DHT22_PIN == GPIO_PIN_14) ? 24u : 28u));
    s_pin.cr       = &GW_DHT22_PORT->CRH;       /* PG11 属 CRH */
    s_pin.ready    = 1u;

    s_hw.set_out  = dht_set_out;
    s_hw.set_in   = dht_set_in;
    s_hw.write    = dht_write;
    s_hw.read     = dht_read;
    s_hw.delay_us = dht_delay_us;
    s_hw.now_us   = dht_now_us;
    s_hw.ctx      = &s_pin;

    /* 缺陷 AF：响应窗口关中断（详见 drivers/dht22.c 的说明）。
       不注册这两个回调也能编译运行 —— 但 RTOS 下必然读不出数据。 */
    s_hw.enter_critical = dht_enter_critical;
    s_hw.exit_critical  = dht_exit_critical;

    /* 上电后等 2ms 让传感器完成内部复位（手册要求上电稳定时间 ≥1s，
       但通信前等待 1~2ms 已足够让总线空闲；真正的 1s 由应用层在
       系统启动流程里保证 —— 见 app 层说明）。 */
    gw_dwt_delay_us(2000u);

    return 0;
}

const dht22_hw_t *dht22_stm32_hw(void)
{
    return (s_pin.ready != 0u) ? &s_hw : 0;
}

dht22_err_t dht22_stm32_sample(dht22_filtered_t *f, uint8_t retry)
{
    if (s_pin.ready == 0u) {
        return DHT22_ERR_PARAM;
    }
    if (f == 0) {
        return DHT22_ERR_PARAM;
    }
    return dht22_read_and_filter(&s_hw, f, retry);
}
