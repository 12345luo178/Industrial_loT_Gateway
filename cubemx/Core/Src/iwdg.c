/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    iwdg.c
  * @brief   This file provides code for the configuration
  *          of the IWDG instances.
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
#include "iwdg.h"

/* USER CODE BEGIN 0 */

/* ---- 时序参数与编译期自洽校验 ----
 *
 * 把"超时"写成从参数算出来的常量，再用 _Static_assert 钉住它 ——
 * 这样以后谁改了预分频或重装值却忘了改文档/注释，编译期就会报。
 * （CAN 位时序那边踩过"常量与宏不一致"的坑，同一个套路。）
 *
 * ⚠️⚠️ 缺陷 AK（2026-10-13 定位）：上面那句"改了参数就编译报错"的自检
 *      **曾经被绕过，而且完全静默**。
 *
 *   原因是这里有**两个独立的字面量**：宏 `IWDG_PRESCALER_DIV = 64u`
 *   和下面赋值用的 HAL 常量 `IWDG_PRESCALER_128`。
 *   2026-10-12 把窗口从 1.6s 改成 3.2s 时，只改了**赋值那一处**：
 *
 *       真正的预分频 = 128   -> 真实超时 3.2s
 *       IWDG_TIMEOUT_MS     仍算出 1600
 *       _Static_assert(== 1600u) **照样通过**
 *
 *   也就是说，自检检查的是那个没人改的宏，而不是真正写进 IWDG_PR 的值 ——
 *   它盯着一个和事实无关的变量，所以永远发现不了。
 *
 *   修法：让赋值与自检**共用同一个常量**（唯一真源），两者不可能再分叉。
 *   下面 `IWDG_PRESCALER_REG` 是唯一真源：赋值用它，分频比和超时都从它推。
 *   以后要改窗口，**只改这一行**，超时和自检会自动跟着变；
 *   如果新窗口与文档记录不符，_Static_assert 就会在编译期报出来。
 */
#define IWDG_LSI_HZ          40000u     /* LSI 标称值（实际 30~60kHz） */
#define IWDG_RELOAD          999u       /* 重装值（12 位） */

/* ★ 唯一真源：真正写进 IWDG_PR 寄存器的那个值。
   要改看门狗窗口，**只改这一行**。 */
#define IWDG_PRESCALER_REG   IWDG_PRESCALER_128

/* 寄存器值 -> 分频比：HAL 的 IWDG_PRESCALER_x 恰好就是 0..6（4 << x） */
#define IWDG_DIV_OF(reg)     (4u << (reg))
#define IWDG_PRESCALER_DIV   IWDG_DIV_OF(IWDG_PRESCALER_REG)   /* 128 */

#define IWDG_TIMEOUT_MS      ((IWDG_PRESCALER_DIV * (IWDG_RELOAD + 1u) \
                               * 1000u) / IWDG_LSI_HZ)         /* 3200 */

/* ⚠️ 这个断言是"文档与代码同步"的守卫。改 IWDG_PRESCALER_REG 之后
   它算出来的值若和文档记录的不一样，就在这里报错 —— 请**两边一起改**。 */
_Static_assert(IWDG_TIMEOUT_MS == 3200u,
               "IWDG 超时与文档记录的 3.2s 不一致，请同步更新文档");
_Static_assert(IWDG_RELOAD <= 0xFFFu, "IWDG 重装值是 12 位");
_Static_assert(IWDG_PRESCALER_REG == IWDG_PRESCALER_4
               || IWDG_PRESCALER_REG == IWDG_PRESCALER_8
               || IWDG_PRESCALER_REG == IWDG_PRESCALER_16
               || IWDG_PRESCALER_REG == IWDG_PRESCALER_32
               || IWDG_PRESCALER_REG == IWDG_PRESCALER_64
               || IWDG_PRESCALER_REG == IWDG_PRESCALER_128
               || IWDG_PRESCALER_REG == IWDG_PRESCALER_256,
               "IWDG 预分频只能是 4/8/16/32/64/128/256");

/* USER CODE END 0 */

IWDG_HandleTypeDef hiwdg;

/* IWDG init function */
void MX_IWDG_Init(void)
{

  /* USER CODE BEGIN IWDG_Init 0 */

  /* USER CODE END IWDG_Init 0 */

  /* USER CODE BEGIN IWDG_Init 1 */

  /* USER CODE END IWDG_Init 1 */
  hiwdg.Instance = IWDG;
  /* ⚠️ 2026-10-12：由 /64 改为 /128，看门狗窗口 1.6s -> 3.2s。
     原因：启动期（启动日志 + 界面初始化里的多处 50~120ms 等待）总耗时
     超过 1.6s，导致每次都在 gw_ui_init 途中被复位（现象：屏上只有红/绿带、
     背光反复闪）。3.2s 能覆盖启动期；RTOS 起来后由任务心跳 + 空闲钩子喂狗。
     ⚠️ 2026-10-13（缺陷 AK）：改成 IWDG_PRESCALER_REG —— 见文件上方说明，
        这里必须用那个唯一真源，否则又会和超时自检分叉。 */
  hiwdg.Init.Prescaler = IWDG_PRESCALER_REG;
  hiwdg.Init.Reload = 999;
  if (HAL_IWDG_Init(&hiwdg) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN IWDG_Init 2 */

  /* ⚠️ 从这里开始，狗已经在跑：本函数之后到主循环之间的所有代码
     合计耗时必须 < 3.2s（按 LSI 的 60kHz 上限算还有 2.13s）。
     实测这段（外设初始化 + 应用初始化 + 上板自检）在 1s 以内，
     其中自检的 DHT22 稳定等待是最大头，所以那里也单独喂了狗。 */

  /* USER CODE END IWDG_Init 2 */

}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
