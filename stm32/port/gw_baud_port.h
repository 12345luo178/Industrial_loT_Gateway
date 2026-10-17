/**
 * @file    gw_baud_port.h
 * @brief   波特率切换的硬件层：真正去改 USART2 的 BRR 并重启接收
 *
 * ============================================================================
 *  本文件只做一件事：把"什么时候切"（`gw_baud.c`，89 项测试覆盖）
 *  与"怎么切"（这里）分开。
 *
 *  用法（在 main 里）：
 *
 *      gw_baud_t baud;
 *      (void)gw_baud_init(&baud, 9600u);
 *
 *      // 主循环里：
 *      uint32_t want = gw_baud_from_code(g_regs.holding[HREG_BAUDRATE]);
 *      if (want != 0u && want != gw_baud_current(&baud)) {
 *          (void)gw_baud_request(&baud, want);
 *      }
 *      if (gw_baud_tick(&baud, dt, rx_tx_busy(), master_in_wait()) != 0) {
 *          gw_baud_apply(&baud);       // 改硬件 + commit
 *      }
 *
 *  ----------------------------------------------------------------------------
 *  ⚠️ 改波特率时必须"停接收 -> 改 BRR -> 重启接收"，顺序不能反
 *
 *  如果只改 `huart2.Init.BaudRate` 而不重新 `HAL_UART_Init`，
 *  BRR 寄存器**不会变** —— 波特率实际没改，但软件以为改了。
 *  那种 bug 的现象是"改了参数但通信还是老样子"，
 *  而且读回寄存器看到的值是"新"的，特别容易误判。
 *
 *  重启接收是必须的：切换瞬间 DMA 里可能有半个帧的残留数据，
 *  不清掉的话它会被当成一帧的有效开头，导致第一次通信就错位。
 * ============================================================================
 */
#ifndef GW_BAUD_PORT_H
#define GW_BAUD_PORT_H

#include <stdint.h>

#include "gw_baud.h"

/**
 * 执行波特率切换（改硬件 + 通知状态机）
 *
 * @param b 上下文（其中必须有一个待切换的波特率）
 * @return 0 成功（状态机已 commit）；
 *         <0 失败（**不 commit** —— 状态机仍认为当前波特率是旧值）
 *
 * ⚠️ 失败时**不能** commit：如果硬件没改成，而状态机以为改了，
 *    那么"软件认为的波特率"与"实际跑的"就不一致 ——
 *    上位机永远也猜不对，现场表现为"改一次波特率就再也通不上"。
 *
 * @note 调用前应先由 `gw_baud_tick()` 确认"现在安全"。
 *       本函数不检查总线状态 —— 那是纯逻辑层的职责。
 */
int gw_baud_apply(gw_baud_t *b);

/**
 * 读回硬件里**实际生效**的波特率
 *
 * 用于初始化时确定初值，以及切换后核实。
 * 实现是读 BRR 反算（`PCLK1 / BRR`），而不是读 `huart->Init.BaudRate` ——
 * 后者只是软件写进去的期望值，不代表分频器真的按它工作了。
 *
 * @return 实际波特率（bps）；无法确定时返回 0
 */
uint32_t gw_baud_port_actual(void);

/**
 * 总线是否正在收发（供 tick 判断条件 ①）
 *
 * @return 1 = 忙（发送中或接收中）
 */
int gw_baud_port_busy(void);

#endif /* GW_BAUD_PORT_H */
