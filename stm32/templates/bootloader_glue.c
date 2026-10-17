/**
 * @file    bootloader_glue.c
 * @brief   Bootloader 镜像缺的那几个"CubeMX 生成在 main.c 里"的函数
 *
 * ⚠️ 为什么需要单独一个文件（2026-10-10 做 OTA 槽位化时加的）
 *
 *     CubeMX 把 `SystemClock_Config()` / `Error_Handler()` /
 *     `HAL_TIM_PeriodElapsedCallback()` 生成在**工程的 main.c** 里。
 *     而引导程序自己的 `main()` 在 `templates/bootloader_main.c` ——
 *     两份 main.c 不能一起编译（`main` 重复定义）。
 *
 *     所以这里把这三个函数单独提供一份，**内容与工程 main.c 里逐字一致**
 *     （时钟树：HSE 8MHz × PLL9 = 72MHz，APB1=/2、APB2=/1，Flash 2 等待）。
 *
 *     ⚠️ 改时钟配置时必须**两边一起改**：工程 main.c（应用镜像用的那份）
 *        和这里（引导程序镜像用的那份）。这也是为什么把这段说明放在文件头。
 *
 *     （另一种做法是让构建脚本从 main.c 里抽函数文本 —— 太聪明、容易碎，
 *       宁可显式重复一小段代码并把"要一起改"写在这里。）
 */

#include "main.h"

/* ==================================================================
 *  时钟：与应用镜像保持完全一致
 *
 *  ⚠️ 引导程序其实可以用 HSI 8MHz 凑合跑（跳转前应用会重配时钟），
 *     但保持一致更省心：串口波特率、Flash 等待周期、跳转前的时序
 *     全都按同一套配置推，出问题时少一个变量。
 * ================================================================== */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_ON;
    RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
        Error_Handler();
    }

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                  | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK) {
        Error_Handler();
    }
}

/**
 * 时基中断回调（TIM4）—— 必须转发给 HAL_IncTick()，否则 HAL_GetTick 不动，
 * 而引导程序里的 `HAL_UART_Transmit(..., 100u)` 这类带超时的调用会立刻超时。
 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM4) {
        HAL_IncTick();
    }
}

/**
 * 出错处理：关中断后死等（等 ST-Link 接上来）。
 *
 * ⚠️ 引导程序里**不能**在这里复位或者跳转：
 *    走到这里说明时钟或初始化真的失败了，此时任何"再试一次"的猜测
 *    都可能把设备带进复位循环。停下来让人看清楚是更安全的选择。
 */
void Error_Handler(void)
{
    __disable_irq();
    for (;;) {
    }
}
