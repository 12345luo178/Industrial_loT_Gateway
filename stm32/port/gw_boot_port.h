/**
 * @file    gw_boot_port.h
 * @brief   Bootloader 的硬件层：Flash 读取 + 跳转到应用
 *
 * ============================================================================
 *  本文件只做两件事
 *
 *    1) 提供"从 Flash 读数据"的回调（给 `gw_boot.c` 校验向量表与 CRC 用）
 *    2) **执行跳转**（那段汇编）
 *
 *  所有"该不该跳"的判断都在 `gw_boot.c`（纯逻辑，89 项测试覆盖）。
 *  这里只负责"跳"这个动作本身 —— 因为它是唯一必须在真机上做的事。
 *
 *  ----------------------------------------------------------------------------
 *  ⚠️ 跳转的三个必备步骤，少一个都会出问题
 *
 *  **① 关掉所有中断与外设。**
 *     App 会重新初始化一切。如果跳过去时 SysTick 还在跑、
 *     USART 还在收数据，App 的初始化就会与这些残留状态冲突。
 *     典型症状：跳转后立刻 HardFault，或者串口发出的第一个字节是乱的。
 *
 *  **② 设置向量表偏移（VTOR）。**
 *     Cortex-M3 的向量表默认在 0x00000000（映射到 Flash 起始）。
 *     如果不设 VTOR，App 里任何一个中断都会跳到 **Bootloader 的**
 *     中断向量上 —— 那是灾难性的：App 的 SysTick 会跑进
 *     Bootloader 的代码里。
 *
 *  **③ 设置栈顶指针（MSP）与跳转。**
 *     MSP 必须用**应用向量表的第 0 项**，不能用 Bootloader 自己的 ——
 *     否则 App 一用栈就会覆盖 Bootloader 的 RAM 区域。
 *
 *  ----------------------------------------------------------------------------
 *  ⚠️ 调试提示：跳转之后 ST-Link 会"跟丢"
 *
 *  这是正常的：跳转后 PC 跑到了另一个分区的代码里，
 *  而 ST-Link 的调试会话还停留在 Bootloader 的符号表上。
 *
 *  想调试 App，有两种做法：
 *    a) 直接把 App 烧到 0x08000000（覆盖 Bootloader）——
 *       开发阶段最常用，但你得记得最后再烧回 Bootloader
 *    b) 在 App 工程里把链接地址改成 0x08009000（或 B 分区），
 *       并在 main 开头自己设一下 VTOR，然后用"attach"方式调试
 * ============================================================================
 */
#ifndef GW_BOOT_PORT_H
#define GW_BOOT_PORT_H

#include <stdint.h>

#include "gw_boot.h"

/**
 * 取 Flash 读取接口（给 gw_boot_prepare / gw_boot_check_crc 用）
 *
 * 实现就是 `memcpy` 从 Flash 地址读 —— STM32 的 Flash 可以直接按地址访问，
 * 不需要特殊的读函数。
 */
const gw_boot_io_t *gw_boot_port_io(void);

/**
 * ⚠️ 跳转到应用程序 —— **本函数不返回**
 *
 * @param plan 由 `gw_boot_prepare()` 生成的启动计划
 *
 * 前置条件：`plan` 已通过校验（SP 在 RAM 内、复位向量在分区内且有 Thumb 位）。
 * 本函数**不重复校验** —— 校验是 `gw_boot.c` 的职责，
 * 这里再查一遍只会让"唯一真相"变成两处。
 *
 * 如果传入一个未校验的 plan，后果是"跳进垃圾地址"，
 * 设备会 HardFault 重启 —— 所以调用顺序不能反。
 */
void gw_boot_port_jump(const gw_boot_plan_t *plan);

/**
 * 回读校验辅助：跳转前可选地打印一下计划内容
 *
 * 单独导出是为了让 Bootloader 能在跳转前把"要跳到哪"打到串口 ——
 * 现场排查"为什么没起来"时，这一行日志往往就是关键。
 */
void gw_boot_port_dump_plan(const gw_boot_plan_t *plan);

#endif /* GW_BOOT_PORT_H */
