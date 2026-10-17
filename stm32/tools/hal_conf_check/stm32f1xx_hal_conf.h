/**
 * @file    stm32f1xx_hal_conf.h
 * @brief   **仅用于交叉编译验证**的最小 HAL 模块配置
 *
 * ============================================================================
 *  ⚠️ 这个文件不是交付物的一部分，也不要复制到你的 CubeMX 工程里。
 *
 *  用途：让本项目在**没有完整 CubeMX 工程**的情况下，
 *  也能验证 `port` 目录下的移植层能否通过 `arm-none-eabi-gcc` 编译。
 *
 *  ----------------------------------------------------------------------------
 *  ⚠️⚠️ 重要：真实的 CubeMX 工程里**必须手动确认 CAN 模块被使能**
 *
 *  HAL 的模块开关就在你自己的 `Core/Inc/stm32f1xx_hal_conf.h` 里。
 *  CubeMX 只会在你勾选了对应外设时才打开开关 ——
 *  **如果 .ioc 里没勾 CAN1，`HAL_CAN_MODULE_ENABLED` 就不会被定义，
 *  于是 `CAN_HandleTypeDef` 根本不存在，`port/gw_can_port.c` 编译失败。**
 *
 *  报错是 "unknown type name 'CAN_HandleTypeDef'"，看起来像代码问题，
 *  实际是工程配置问题。排查顺序：
 *    1) CubeMX 里 Connectivity -> CAN1 是否勾选
 *    2) 生成后 `stm32f1xx_hal_conf.h` 里是否有 `HAL_CAN_MODULE_ENABLED`
 *    3) `stm32f1xx_hal_conf.h` 里是否有
 *         #ifdef HAL_CAN_MODULE_ENABLED
 *           #include "stm32f1xx_hal_can.h"
 *         #endif
 *       这一块（CubeMX 会自动加，但如果手工改过配置就可能丢）
 *
 *  ----------------------------------------------------------------------------
 *  ⚠️ 另一个容易踩的坑：HAL 的模块头**不是在 `stm32f1xx_hal.h` 里包含的**
 *
 *  `stm32f1xx_hal.h` 全文只有一条 include（第 29 行）：
 *      #include "stm32f1xx_hal_conf.h"
 *  所有模块头都由 **本文件**（hal_conf.h）负责按开关包含。
 *
 *  所以本文件必须自己：
 *    1) 引入 CMSIS 设备头（提供 `__IO`，`hal.h` 第 59 行就要用）
 *    2) 按模块开关包含各模块头（提供 `HAL_StatusTypeDef`、各外设句柄）
 *
 *  这两件事漏掉任何一件，报错都会指向 HAL 库文件本身
 *  （"unknown type name '__IO'" 或 "unknown type name 'HAL_StatusTypeDef'"），
 *  完全看不出是自己的配置问题。
 * ============================================================================
 */
#ifndef STM32F1xx_HAL_CONF_H
#define STM32F1xx_HAL_CONF_H

/* ----------------------------------------------------------------------------
 * 1) CMSIS 设备头必须先引入
 *
 * `stm32f1xx_hal.h` 第 59 行就用了 `__IO`：
 *      extern __IO uint32_t uwTick;
 * 而 `__IO` 由 `core_cm3.h` 提供，经 `stm32f1xx.h -> stm32f103xe.h` 引入。
 *
 * 本文件正是在 hal.h 的第 29 行被包含的 —— 是"补类型"的最后机会。
 *
 * 注：CubeMX 生成的 main.h 里也会 `#include "stm32f1xx.h"`，
 * 但有 include guard，重复包含无副作用。
 * ---------------------------------------------------------------------------- */
#include "stm32f1xx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------------------
 * 2) 模块使能开关（本项目用到的全部打开）
 *
 * 真实工程里这些由 CubeMX 按 .ioc 勾选情况生成，
 * 你只需要确认 CAN1 被勾上（见文件头部的说明）。
 * ---------------------------------------------------------------------------- */
#define HAL_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED

/* 本地屏幕：port/lcd_ili9341.c 用 SPI1（轮询）。
 * ⚠️ 加了新 HAL 模块要同步这里，否则 CI 的交叉编译会报
 *    unknown type name（2026-10-11 踩过）。 */
#define HAL_SPI_MODULE_ENABLED
#define HAL_CAN_MODULE_ENABLED      /* ★ RS485/ESP32/DHT22 之外最容易被漏的一个 */
#define HAL_PWR_MODULE_ENABLED
#define HAL_IWDG_MODULE_ENABLED     /* ★ 独立看门狗（报告要求 1.6s） */
#define HAL_TIM_MODULE_ENABLED

/* ---------------- 晶振与时钟 ---------------- */
#if !defined(HSE_VALUE)
#define HSE_VALUE               8000000U    /* 最小系统板晶振，务必与实际一致 */
#endif
#if !defined(HSE_STARTUP_TIMEOUT)
#define HSE_STARTUP_TIMEOUT     100U
#endif
#if !defined(HSI_VALUE)
#define HSI_VALUE               8000000U
#endif
#if !defined(LSI_VALUE)
#define LSI_VALUE               40000U
#endif
#if !defined(LSE_VALUE)
#define LSE_VALUE               32768U
#endif
#if !defined(LSE_STARTUP_TIMEOUT)
#define LSE_STARTUP_TIMEOUT     5000U
#endif

/* ---------------- HAL 运行参数 ---------------- */
#if !defined(VDD_VALUE)
#define VDD_VALUE               3300U
#endif
#if !defined(TICK_INT_PRIORITY)
#define TICK_INT_PRIORITY       0x0FU
#endif
#if !defined(USE_RTOS)
#define USE_RTOS                0U
#endif
#if !defined(PREFETCH_ENABLE)
#define PREFETCH_ENABLE         1U
#endif

/* ============================================================================
 * 3) 按模块开关包含各模块头（**这一块不能省**）
 *
 * 为什么必须在这里包含、而不是在 stm32f1xx_hal.h 里：
 *   `stm32f1xx_hal.h` 只包含本文件这一条 include，
 *   模块头全部由本文件负责。`HAL_StatusTypeDef`、各外设句柄类型
 *   都是通过这一块引入的。
 *
 * 顺序参考 CubeMX 生成的结果：rcc -> gpio -> dma -> cortex -> flash -> ...
 * 顺序本身不敏感（各模块头互相之间有 guard），但保持一致便于对比。
 * ============================================================================ */
#ifdef HAL_RCC_MODULE_ENABLED
  #include "stm32f1xx_hal_rcc.h"
#endif

#ifdef HAL_GPIO_MODULE_ENABLED
  #include "stm32f1xx_hal_gpio.h"
#endif

#ifdef HAL_DMA_MODULE_ENABLED
  #include "stm32f1xx_hal_dma.h"
#endif

#ifdef HAL_CORTEX_MODULE_ENABLED
  #include "stm32f1xx_hal_cortex.h"
#endif

#ifdef HAL_FLASH_MODULE_ENABLED
  #include "stm32f1xx_hal_flash.h"
#endif

#ifdef HAL_TIM_MODULE_ENABLED
  #include "stm32f1xx_hal_tim.h"
#endif

#ifdef HAL_UART_MODULE_ENABLED
  #include "stm32f1xx_hal_uart.h"
#endif

#ifdef HAL_SPI_MODULE_ENABLED
  #include "stm32f1xx_hal_spi.h"
#endif

#ifdef HAL_CAN_MODULE_ENABLED
  #include "stm32f1xx_hal_can.h"
#endif

#ifdef HAL_IWDG_MODULE_ENABLED
  #include "stm32f1xx_hal_iwdg.h"
#endif
#ifdef HAL_PWR_MODULE_ENABLED
  #include "stm32f1xx_hal_pwr.h"
#endif

/* ---------------- 断言 ---------------- */
/* 编译验证时关掉（打开会引入 assert_failed 的 printf 依赖） */
/* #define USE_FULL_ASSERT    1U */

#ifdef USE_FULL_ASSERT
  #define assert_param(expr) ((expr) ? (void)0U : assert_failed((uint8_t *)__FILE__, __LINE__))
  void assert_failed(uint8_t *file, uint32_t line);
#else
  #define assert_param(expr) ((void)0U)
#endif

#ifdef __cplusplus
}
#endif

#endif /* STM32F1xx_HAL_CONF_H */
