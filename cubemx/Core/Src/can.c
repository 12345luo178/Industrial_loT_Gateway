/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    can.c
  * @brief   This file provides code for the configuration
  *          of the CAN instances.
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
#include "can.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

CAN_HandleTypeDef hcan;

/* CAN init function */
void MX_CAN_Init(void)
{

  /* USER CODE BEGIN CAN_Init 0 */

  /* USER CODE END CAN_Init 0 */

  /* USER CODE BEGIN CAN_Init 1 */

  /* USER CODE END CAN_Init 1 */
  hcan.Instance = CAN1;
  hcan.Init.Prescaler = 4;
  hcan.Init.Mode = CAN_MODE_NORMAL;
  hcan.Init.SyncJumpWidth = CAN_SJW_1TQ;
  hcan.Init.TimeSeg1 = CAN_BS1_15TQ;
  hcan.Init.TimeSeg2 = CAN_BS2_2TQ;
  hcan.Init.TimeTriggeredMode = DISABLE;
  hcan.Init.AutoBusOff = DISABLE;
  hcan.Init.AutoWakeUp = DISABLE;
  hcan.Init.AutoRetransmission = DISABLE;
  hcan.Init.ReceiveFifoLocked = DISABLE;
  hcan.Init.TransmitFifoPriority = DISABLE;
  if (HAL_CAN_Init(&hcan) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN CAN_Init 2 */

  /* USER CODE END CAN_Init 2 */

}

void HAL_CAN_MspInit(CAN_HandleTypeDef* canHandle)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if(canHandle->Instance==CAN1)
  {
  /* USER CODE BEGIN CAN1_MspInit 0 */

  /* ⚠️ CAN_RX(PB8) 必须是**输入 + 上拉**，不是 CubeMX 默认的浮空输入。
   *
   * 2026-10-10 实测：浮空输入时，若收发器没供电、或它的 RXD 处于高阻，
   * PB8 就悬空 —— 板上实测读到 **PB8 = 0（显性）**，后果是整个 CAN 不可用：
   *     bxCAN 退出初始化模式需要总线上 11 个连续隐性位
   *     -> CAN_RX 一直显性 -> HAL_CAN_Start() 超时、MSR.INAK 恒为 1
   *     -> 控制器永远停在初始化模式（连内部回环也出不来）
   * 而软件侧的核心状态照样是 NORMAL —— **静默失能**。
   *
   * 上拉之后，"收发器没工作"时总线至少是隐性，控制器能正常初始化，
   * 故障会以"发不出去/收不到"的形式暴露，而不是让 CAN 直接起不来。
   *
   * ⚠️ 本行的值来自 CubeMX（`gateway2.ioc` 里 PB8.GPIO_PuPd=GPIO_PULLUP），
   *    重新生成代码时不要手改这一行，改 .ioc 即可。
   *    详见 doc/CubeMX配置清单.md §4.4。
   *
   * ⚠️ 2026-10-15 复查：本行曾被还原成 GPIO_NOPULL —— 当时 `.ioc` 里**根本没有**
   *    PB8.GPIO_PuPd（上面那句"值来自 CubeMX"是假的），CubeMX 生成的自然就是 NOPULL。
   *    后果：收发器没供电时 PB8 悬空 -> 读到显性 -> HAL_CAN_Start() 超时、MSR.INAK=1。
   *    现已把 PB8.GPIO_PuPd=GPIO_PULLUP 补进 .ioc，本行同步改回 PULLUP。
   *    这与缺陷 J 同类：**.ioc 没配 -> 重新生成还原你删掉的**。 */
  /* USER CODE END CAN1_MspInit 0 */
    /* CAN1 clock enable */
    __HAL_RCC_CAN1_CLK_ENABLE();

    __HAL_RCC_GPIOB_CLK_ENABLE();
    /**CAN GPIO Configuration
    PB8     ------> CAN_RX
    PB9     ------> CAN_TX
    */
    GPIO_InitStruct.Pin = GPIO_PIN_8;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_9;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    __HAL_AFIO_REMAP_CAN1_2();

    /* CAN1 interrupt Init */
    /* ⚠️ 2026-10-15：CAN1_RX0 的使能曾整段丢失（缺陷 J 回归，见
       doc/上板调试交接.md §一 缺陷 AL）。根因：.ioc 里只有 NVIC.CAN1_SCE_IRQn
       —— 只要在 CubeMX 里点一次"重新生成"，按 .ioc 还原的 can.c 就会抹掉
       这两行，而 stm32f1xx_it.c 的 CAN1_RX0_IRQHandler 还在，
       于是只剩一个**永远不会被调用**的中断服务函数（帧堆在硬件 FIFO 里直到溢出）。
       现已同步把 NVIC.CAN1_RX0_IRQn 补进 .ioc。 */
    HAL_NVIC_SetPriority(CAN1_RX0_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(CAN1_RX0_IRQn);
    HAL_NVIC_SetPriority(CAN1_SCE_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(CAN1_SCE_IRQn);
  /* USER CODE BEGIN CAN1_MspInit 1 */

  /* USER CODE END CAN1_MspInit 1 */
  }
}

void HAL_CAN_MspDeInit(CAN_HandleTypeDef* canHandle)
{

  if(canHandle->Instance==CAN1)
  {
  /* USER CODE BEGIN CAN1_MspDeInit 0 */

  /* USER CODE END CAN1_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_CAN1_CLK_DISABLE();

    /**CAN GPIO Configuration
    PB8     ------> CAN_RX
    PB9     ------> CAN_TX
    */
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_8|GPIO_PIN_9);

    /* CAN1 interrupt Deinit */
    HAL_NVIC_DisableIRQ(CAN1_RX0_IRQn);
    HAL_NVIC_DisableIRQ(CAN1_SCE_IRQn);
  /* USER CODE BEGIN CAN1_MspDeInit 1 */

  /* USER CODE END CAN1_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
