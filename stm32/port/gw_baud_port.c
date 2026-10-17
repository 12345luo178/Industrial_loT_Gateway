/**
 * @file    gw_baud_port.c
 * @brief   波特率切换硬件层实现
 */
#include "gw_baud_port.h"

#include "stm32f1xx_hal.h"
#include "gw_config.h"
#include "rs485_port.h"
#include "modbus_regs.h"

extern UART_HandleTypeDef huart2;

/* ==================================================================
 *  读回实际波特率
 *
 *  ⚠️ 为什么读 BRR 反算而不是读 huart2.Init.BaudRate：
 *    后者是"软件希望的值"，前者才是"硬件实际在用的值"。
 *    两者不一致时（例如 HAL_UART_Init 失败、或者时钟变了），
 *    只有 BRR 是真的。
 *
 *  USART 的波特率公式（F1 参考手册）：
 *      当 OVER8 = 0（16 倍过采样）：
 *          Tx/Rx baud = fCK / (16 × USARTDIV)
 *          BRR = (DIV_Mantissa << 4) | DIV_Fraction
 *      所以 USARTDIV = BRR / 16，fCK = PCLK1（USART2 挂在 APB1）
 * ================================================================== */
uint32_t gw_baud_port_actual(void)
{
    uint32_t pclk1;
    uint32_t brr;
    uint32_t div16;             /* BRR 的定点表示（低 4 位是小数） */

    /* USART2 挂在 APB1。HAL_RCC_GetPCLK1Freq() 返回的是**定时器之外**
       外设的时钟，而 USART 用的是这个值（不需要 ×2）。 */
    pclk1 = HAL_RCC_GetPCLK1Freq();
    if (pclk1 == 0u) {
        return 0u;
    }

    brr = huart2.Instance->BRR;
    if (brr == 0u) {
        return 0u;
    }

    div16 = brr & 0xFFFFu;      /* 只用低 16 位（F1 的 BRR 有效位） */

    /* baud = pclk1 / (16 * USARTDIV)
            = pclk1 / (16 * div16 / 16)
            = pclk1 / div16
       这里用整数除法，误差在 ±1% 以内（BRR 本身就是近似分频）。 */
    if (div16 == 0u) {
        return 0u;
    }
    return pclk1 / div16;
}

/* ==================================================================
 *  总线忙判定
 *
 *  "忙"包含两种：正在发送、正在接收。
 *  两种都不能改波特率：
 *    - 发送中改 -> 当前帧后半段用新波特率发出
 *    - 接收中改 -> 打断这一帧
 * ================================================================== */
int gw_baud_port_busy(void)
{
    /* ---- 发送中 ----
       ⚠️ 判据是 TC（Transmission Complete）而**不是** TXE。
       TXE 只说明"发送数据寄存器空了、可以再塞一个字节"，
       此时移位寄存器里还有一个字节正在往外发 —— 改波特率会让
       那个字节用新波特率发出去。TC 才代表"最后一个位也发完了"。 */
    if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_TC) == RESET) {
        return 1;
    }

    /* ---- 接收中 ----
       用模块自己的状态判断，而不是 RXNE 标志：
       RXNE 只反映"此刻有字节"，而我们要知道的是"正在等一帧"。

         RS485_ST_RX       -> 正在接收：改了会打断这一帧
         RS485_ST_RX_READY -> 有整帧待取：也要等（静默期条件覆盖）
         RS485_ST_TX       -> 正在发送：同上
         RS485_ST_IDLE     -> 空闲：安全 */
    {
        rs485_state_t st = rs485_port_state();

        if (st == RS485_ST_RX || st == RS485_ST_RX_READY
            || st == RS485_ST_TX) {
            return 1;
        }
    }
    return 0;
}

/* ==================================================================
 *  执行切换
 * ================================================================== */
int gw_baud_apply(gw_baud_t *b)
{
    uint32_t target;
    uint32_t actual;
    uint32_t pclk1;

    if (b == 0) {
        return -1;
    }

    target = gw_baud_pending(b);
    if (target == 0u || gw_baud_supported(target) == 0u) {
        return -1;              /* 没有待切换，或值非法 */
    }

    /* ---- ① 停掉接收 ----
       切换瞬间 DMA 里可能有半个帧的残留，不清掉会被当成有效开头。 */
    (void)HAL_UART_AbortReceive(&huart2);

    /* ---- ② 改波特率并重新初始化外设 ----
       ⚠️ 只改 `huart2.Init.BaudRate` 是不够的！那只改了软件结构体，
          BRR 寄存器不会变 —— "软件以为改了、实际没改"，
          现象是"改了参数但通信还是老样子"，极难查。

          HAL_UART_Init() 内部会按 Init.BaudRate 重算 BRR 并写进去。 */
    huart2.Init.BaudRate = target;
    if (HAL_UART_Init(&huart2) != HAL_OK) {
        /* 初始化失败：**不能 commit**。
           状态机仍认为当前是旧波特率，与实际一致（BRR 没被改坏
           的话）。下次 tick 会再试一次。 */
        return -2;
    }

    /* ---- ③ 重启接收 ---- */
    if (rs485_port_restart_rx() != 0) {
        return -3;              /* 接收没起来：不 commit，等下次重试 */
    }

    /* ---- ④ 核实硬件实际值，并以它为准 commit ----
       不直接用 target：万一分频算不出精确值（例如 PCLK1 不是标准频率），
       实际波特率会与目标有偏差。状态机必须记录**实际值**。 */
    actual = gw_baud_port_actual();
    if (actual == 0u) {
        actual = target;        /* 读不出来就按目标值记（退而求其次） */
    }

    pclk1 = HAL_RCC_GetPCLK1Freq();
    (void)pclk1;

    gw_baud_commit(b, actual);
    return 0;
}
