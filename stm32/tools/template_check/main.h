/**
 * @file    main.h
 * @brief   ★ 仅用于 CI 类型检查的桩头文件（**不是交付物**）
 *
 * ============================================================================
 *  这个文件解决什么问题
 *
 *  `templates/main_template.c` 与 `templates/bootloader_main.c` 是**模板** ——
 *  它们是给用户复制进 CubeMX 工程用的，所以依赖 CubeMX 生成的
 *  `main.h` 与 `MX_xxx_Init()` 函数声明。
 *
 *  这意味着在 CI 里直接编译它们会失败（找不到 `main.h`），
 *  于是**两个模板长期没有被编译器检查过** ——
 *  而它们恰恰是最容易写错的部分（初始化顺序、API 签名、句柄名）。
 *
 *  本文件提供最小的桩声明，让 CI 能把两个模板**当普通源文件编译**，
 *  从而抓住：
 *
 *    - API 签名用错（例如 `gw_boot_prepare()` 少传参数）
 *    - 句柄名写错（`huart1` / `hiwdg` / `hcan`）
 *    - 类型不匹配（例如把 `gw_boot_io_t*` 传给要 `gw_ota_io_t*` 的函数）
 *    - 嵌套注释、拼写错误、用了不存在的宏
 *
 *  ⚠️ 实际项目中这个文件由 CubeMX 生成，内容与本桩不同
 *     （会有更多外设句柄与 `MX_xxx_Init` 声明）。
 *     本桩只声明两个模板**实际用到**的那些 ——
 *     这正是"类型检查"能做到的部分：
 *     名字与签名对了，剩下的差异（多出来的外设）不影响模板的正确性。
 *
 *  ⚠️ 它**不参与**产品构建，也不应该被复制到用户工程里。
 * ============================================================================
 */
#ifndef GW_CI_MAIN_STUB_H
#define GW_CI_MAIN_STUB_H

#include <stdint.h>

/* 用真实的 HAL 头 —— 类型检查的价值就在于"签名对得上"，
   如果用假类型就查不出 HAL 相关的错。 */
#include "stm32f1xx_hal.h"

/* ---- CubeMX 会生成的句柄（两个模板用到的全部）---- */
extern UART_HandleTypeDef huart1;   /* 调试/日志口 */
extern UART_HandleTypeDef huart2;   /* RS485 */
extern UART_HandleTypeDef huart3;   /* ESP32 */
extern IWDG_HandleTypeDef hiwdg;    /* 独立看门狗 */
extern CAN_HandleTypeDef  hcan;     /* bxCAN */

/* ---- CubeMX 会生成的初始化函数 ---- */
HAL_StatusTypeDef HAL_Init(void);
void SystemClock_Config(void);
void MX_GPIO_Init(void);
void MX_DMA_Init(void);
void MX_USART1_UART_Init(void);
void MX_USART2_UART_Init(void);
void MX_USART3_UART_Init(void);
void MX_CAN_Init(void);
void MX_IWDG_Init(void);
void MX_SPI1_Init(void);

/* 本地屏幕：main 在界面初始化失败时会用背光闪 3 下作故障信号 */
void lcd_backlight(int on);        /* ⚠️ 本地屏幕用 SPI1；漏了它会 启动即 HardFault */        /* ⚠️ 2026-10-11 补：模板必须调它初始化 hiwdg，
                                   否则 gw_rtos_iwdg_enable() 里的 HAL_IWDG_Init
                                   会拿 Instance==0 往 0x00000008 写 -> HardFault */

#endif /* GW_CI_MAIN_STUB_H */
