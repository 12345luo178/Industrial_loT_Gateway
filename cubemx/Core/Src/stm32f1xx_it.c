/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32f1xx_it.c
  * @brief   Interrupt Service Routines.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32f1xx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
/* ⚠️ 需要 can.h 才能看到 `hcan` —— main.h 只包含 stm32f1xx_hal.h。
   本文件里 CAN1_RX0_IRQHandler / CAN1_SCE_IRQHandler 要用它。 */
#include "can.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern DMA_HandleTypeDef hdma_usart2_rx;
extern DMA_HandleTypeDef hdma_usart3_rx;
extern UART_HandleTypeDef huart2;
extern UART_HandleTypeDef huart3;
extern TIM_HandleTypeDef htim4;

/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex-M3 Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/* ============================================================================
 *  ⚠️ 四个故障处理函数已改为「可被工程覆盖」
 *
 *  `port/gw_fault_port.c` 里定义了这四者的**真实实现**（带黑匣子记录：
 *  抓 PC / LR / 故障状态寄存器存进 .noinit，下次启动可读）。
 *
 *  这里原来的四个空壳（只有 while(1)）会造成链接错误：
 *      multiple definition of `HardFault_Handler'
 *
 *  ⚠️ 为什么用宏判断而不是直接删掉：
 *      直接删掉的话，下次在 CubeMX 里 **Generate Code** 会把它生成回来，
 *      又变成重复定义 —— 而那时你可能已经忘了这件事。
 *      用宏判断的好处是：**重新生成也不会冲突**。
 *
 *  ---------------------------------------------------------------------------
 *  ⚠️ 用户必须知道的一点（CubeMX 的生成行为）
 *
 *      整段 `#if` 块都是"用户代码"，CubeMX 不会碰它。
 *      但**这四个函数的签名行**在重新生成时会不会被 CubeMX 认作"库代码"
 *      而重新插入，取决于你配的 CubeMX 版本 —— 本项目未逐一验证过所有版本。
 *
 *      所以：**每次 Generate Code 之后，请检查这个文件开头的
 *      `GW_FAULT_HANDLERS_EXTERNAL` 是否还在。** 如果被删了、
 *      又出现 multiple definition，就是这个问题，把本段重新贴回去即可。
 * ============================================================================ */
#ifndef GW_FAULT_HANDLERS_EXTERNAL
#define GW_FAULT_HANDLERS_EXTERNAL 1
#endif

#if !GW_FAULT_HANDLERS_EXTERNAL

/**
  * @brief This function handles Hard fault interrupt.
  */

/**
  * @brief This function handles Memory management fault.
  */

/**
  * @brief This function handles Prefetch fault, memory access fault.
  */

/**
  * @brief This function handles Undefined instruction or illegal state.
  */

#endif /* !GW_FAULT_HANDLERS_EXTERNAL —— 实现在 port/gw_fault_port.c */

/**
  * @brief This function handles System service call via SWI instruction.
  */

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/**
  * @brief This function handles Pendable request for system service.
  */

/**
  * @brief This function handles System tick timer.
  */

/******************************************************************************/
/* STM32F1xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32f1xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles DMA1 channel3 global interrupt.
  */
void DMA1_Channel3_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Channel3_IRQn 0 */

  /* USER CODE END DMA1_Channel3_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_usart3_rx);
  /* USER CODE BEGIN DMA1_Channel3_IRQn 1 */

  /* USER CODE END DMA1_Channel3_IRQn 1 */
}

/**
  * @brief This function handles DMA1 channel6 global interrupt.
  */
void DMA1_Channel6_IRQHandler(void)
{
  /* USER CODE BEGIN DMA1_Channel6_IRQn 0 */

  /* USER CODE END DMA1_Channel6_IRQn 0 */
  HAL_DMA_IRQHandler(&hdma_usart2_rx);
  /* USER CODE BEGIN DMA1_Channel6_IRQn 1 */

  /* USER CODE END DMA1_Channel6_IRQn 1 */
}

/**
  * @brief This function handles TIM4 global interrupt.
  */
void TIM4_IRQHandler(void)
{
  /* USER CODE BEGIN TIM4_IRQn 0 */

  /* USER CODE END TIM4_IRQn 0 */
  HAL_TIM_IRQHandler(&htim4);
  /* USER CODE BEGIN TIM4_IRQn 1 */

  /* USER CODE END TIM4_IRQn 1 */
}

/**
  * @brief This function handles USART2 global interrupt.
  */
void USART2_IRQHandler(void)
{
  /* USER CODE BEGIN USART2_IRQn 0 */

  /* USER CODE END USART2_IRQn 0 */
  HAL_UART_IRQHandler(&huart2);
  /* USER CODE BEGIN USART2_IRQn 1 */

  /* USER CODE END USART2_IRQn 1 */
}

/**
  * @brief This function handles CAN1 RX0 interrupts.
  *
  * ⚠️ 这两个处理函数原来**缺失**（2026-10-10 用 CAN 回环自检抓出来的）：
  *    NVIC 里没使能 + 没有中断服务函数 = 帧收到后躺在硬件 FIFO 里没人取，
  *    端口层的 HAL_CAN_RxFifo0MsgPendingCallback() 永远不会被调用。
  *    详见 Core/Src/can.c 的 HAL_CAN_MspInit 注释。
  */
void CAN1_RX0_IRQHandler(void)
{
  /* USER CODE BEGIN CAN1_RX0_IRQn 0 */

  /* USER CODE END CAN1_RX0_IRQn 0 */
  HAL_CAN_IRQHandler(&hcan);
  /* USER CODE BEGIN CAN1_RX0_IRQn 1 */

  /* USER CODE END CAN1_RX0_IRQn 1 */
}

/**
  * @brief This function handles CAN1 SCE interrupts (状态变化/错误中断).
  */
void CAN1_SCE_IRQHandler(void)
{
  /* USER CODE BEGIN CAN1_SCE_IRQn 0 */

  /* USER CODE END CAN1_SCE_IRQn 0 */
  HAL_CAN_IRQHandler(&hcan);
  /* USER CODE BEGIN CAN1_SCE_IRQn 1 */

  /* USER CODE END CAN1_SCE_IRQn 1 */
}

/**
  * @brief This function handles USART3 global interrupt.
  */
void USART3_IRQHandler(void)
{
  /* USER CODE BEGIN USART3_IRQn 0 */

  /* USER CODE END USART3_IRQn 0 */
  HAL_UART_IRQHandler(&huart3);
  /* USER CODE BEGIN USART3_IRQn 1 */

  /* USER CODE END USART3_IRQn 1 */
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
