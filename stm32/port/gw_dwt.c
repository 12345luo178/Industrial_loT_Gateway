/**
 * @file    gw_dwt.c
 * @brief   DWT CYCCNT 微秒延时与计时实现
 *
 * 依赖：STM32F1 的 CMSIS 头（core_cm3.h 里的 DWT/CoreDebug 定义）。
 * 本文件只在 STM32 工程里编译，PC 端测试不包含它。
 */
#include "gw_dwt.h"
#include "stm32f1xx_hal.h"

#include "core_cm3.h"

/* 每微秒的时钟周期数，初始化时按 SystemCoreClock 算一次。
   用 SystemCoreClock（HAL 维护的全局变量）而不是写死 72，
   这样改主频时不必回来改这里 —— 少一个"改了时钟忘了改延时"的坑。 */
static uint32_t s_cycles_per_us = 72u;
static int      s_ready = 0;

void gw_dwt_init(void)
{
    uint32_t tries;

    /* 使能跟踪与调试模块（TRCENA）。
       不开这一位，CYCCNT 根本不会计数。 */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;

    /* 清 CYCCNT 并使其在调试器停机时也继续计数 */
    DWT->CYCCNT = 0u;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    /* 按当前系统时钟算每微秒周期数。
       先按 1MHz 假设，再用实测校准（见下）—— 这样即使 SystemCoreClock
       因为某些原因不准，延时仍然正确。 */
    if (SystemCoreClock >= 1000000u) {
        s_cycles_per_us = SystemCoreClock / 1000000u;
    } else {
        s_cycles_per_us = 1u;
    }

    /* 自检：等待一小段时间，确认 CYCCNT 真的在走。
       如果 DWT 不可用（例如被某些调试配置禁用），这里能发现。 */
    {
        uint32_t t0 = DWT->CYCCNT;
        for (tries = 0u; tries < 1000u; tries++) {
            if (DWT->CYCCNT != t0) {
                s_ready = 1;
                break;
            }
        }
    }

    if (s_ready == 0) {
        /* DWT 不可用时的降级：把 s_cycles_per_us 保底为 1，
           调用方的延时会变短（不准），但不会死等。
           这里不返回错误码，是为了保持接口简单 ——
           上层可以用 gw_dwt_is_ready() 查询并决定是否禁用 DHT22。 */
        s_cycles_per_us = 1u;
    }
}

int gw_dwt_is_ready(void)
{
    return s_ready;
}

uint32_t gw_dwt_us(void)
{
    if (s_ready == 0) {
        return 0u;
    }
    return DWT->CYCCNT / s_cycles_per_us;
}

uint32_t gw_dwt_cycles(void)
{
    if (s_ready == 0) {
        return 0u;
    }
    return DWT->CYCCNT;
}

uint32_t gw_dwt_elapsed_us(uint32_t t0_cycles)
{
    uint32_t now;

    if (s_ready == 0) {
        return 0u;
    }
    now = DWT->CYCCNT;

    /* ⚠️⚠️ 顺序不能反：**先**对 32 位周期数做无符号减法（回绕周期正好 2^32，
       减法天然正确），**再**除以每微秒周期数换算成微秒。
       若写成 `gw_dwt_us() - t0`，因为中间先除了 72，回绕周期变成
       59,652,323 µs（不是 2 的幂），跨回绕点会算出约 42.9 亿的假值
       —— 2026-10-14 实机就是标题栏显示 `4235335ms`。 */
    return (now - t0_cycles) / s_cycles_per_us;
}

void gw_dwt_delay_us(uint32_t us)
{
    uint32_t start;
    uint32_t ticks;
    uint32_t elapsed;

    if (us == 0u) {
        return;
    }
    if (s_ready == 0) {
        /* DWT 不可用：退化为简单的循环等待，至少不会立刻返回 */
        volatile uint32_t n = us * 8u;
        while (n-- > 0u) { }
        return;
    }

    ticks = us * s_cycles_per_us;

    start = DWT->CYCCNT;
    /* 用无符号减法判断已过周期数，天然处理 CYCCNT 回绕。
       不要写成 while (DWT->CYCCNT - start < ticks) 之外的形式，
       例如先比较大小再相减 —— 回绕时会出错。 */
    do {
        elapsed = DWT->CYCCNT - start;
    } while (elapsed < ticks);
}
