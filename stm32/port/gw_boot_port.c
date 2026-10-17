/**
 * @file    gw_boot_port.c
 * @brief   Bootloader 硬件层实现：Flash 读取 + 跳转汇编
 */
#include "gw_boot_port.h"

/* 注意包含顺序：gw_config.h 使用 HAL 的 GPIO 宏，必须先包含 HAL 头。 */
#include "stm32f1xx_hal.h"

#include <stdio.h>
#include <string.h>

/* ==================================================================
 *  Flash 读取接口
 *
 *  STM32 的 Flash 是**可以直接按地址读**的（不像某些 MCU 需要专门的
 *  读命令）。所以实现就是 memcpy。
 *
 *  ⚠️ 为什么用 memcpy 而不是逐字节：
 *    逐字节读会失去编译器的批量加载优化，208 KB 校验会慢好几倍。
 *    memcpy 在 -O2 下会变成 LDM/STM 批量搬运。
 * ================================================================== */
static int port_read(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len)
{
    (void)ctx;

    if (buf == 0 || len == 0u) {
        return -1;
    }
    /* 地址范围检查：只允许读 Flash 区。
       越界读会返回总线错误（BusFault），而那是可以提前拦住的。 */
    if (addr < GW_BOOT_FLASH_BASE
        || (addr + len) > GW_BOOT_FLASH_END) {
        return -1;
    }

    memcpy(buf, (const void *)addr, len);
    return 0;
}

static const gw_boot_io_t s_io = {
    port_read,
    0
};

const gw_boot_io_t *gw_boot_port_io(void)
{
    return &s_io;
}

/* ==================================================================
 *  跳转
 * ================================================================== */

void gw_boot_port_dump_plan(const gw_boot_plan_t *plan)
{
    if (plan == 0) {
        return;
    }
    /* 用 printf 而不是日志模块：Bootloader 的日志系统还没初始化
       （日志模块本身要占 RAM，而 Bootloader 应该尽量小）。 */
    printf("[BOOT] slot=%s base=0x%08lX sp=0x%08lX reset=0x%08lX\r\n",
           gw_ota_slot_str(plan->slot),
           (unsigned long)plan->base,
           (unsigned long)plan->sp,
           (unsigned long)plan->reset);
    printf("[BOOT] len=%lu crc=0x%08lX pending=%u verified=%u\r\n",
           (unsigned long)plan->image_len,
           (unsigned long)plan->image_crc,
           (unsigned)plan->is_pending,
           (unsigned)plan->verified);
}

/**
 * ⚠️ 关闭所有中断与外设
 *
 * 为什么必须做（而不是"应该做"）：
 *   App 会从零开始初始化所有外设。如果跳过去时 Bootloader 的
 *   SysTick 还在跑、某个 USART 还在收数据、某个定时器还在计数，
 *   那么 App 初始化这些外设时就会与残留状态冲突。
 *
 *   典型症状：
 *     - 跳转后立刻 HardFault（中断向量还没来得及切）
 *     - 串口发出的第一帧是乱码（发送寄存器里有上一个字节）
 *     - 第一个 HAL_Delay 就不返回（SysTick 计数被重置成奇怪的值）
 *
 *  这些症状的共同点是"偶发"、"换个时序就好了"，极难定位。
 *  所以**宁可多关一点**。
 */
static void shutdown_everything(void)
{
    /* 1) 关全局中断。这是最关键的一步 ——
          在向量表切换完成之前，任何中断都可能跳到错误的地址。 */
    __disable_irq();

    /* 2) 关掉 SysTick 与它的中断。
          Bootloader 里 HAL_Init() 会配 SysTick；不关的话
          跳过去后它会继续产生中断，而那时 VTOR 已经指向 App ——
          如果 App 还没来得及设自己的 SysTick 处理，就会跳到一个
          未初始化的向量（通常是 0x00000000 -> 立刻 HardFault）。 */
    SysTick->CTRL = 0u;
    SysTick->LOAD = 0u;
    SysTick->VAL  = 0u;

    /* 3) 关掉所有 NVIC 中断使能与挂起位。
          不清挂起位的话，一个"已挂起但还没被服务"的中断会在
          重新开中断时立刻触发 —— 而那时已经跳到 App 了。 */
    {
        uint8_t i;

        for (i = 0u; i < 8u; i++) {
            NVIC->ICER[i] = 0xFFFFFFFFu;    /* 清使能 */
            NVIC->ICPR[i] = 0xFFFFFFFFu;    /* 清挂起 */
        }
    }

    /* 4) 复位所有外设（RCC 的 APB1/APB2 复位寄存器）。
          比逐个外设 DeInit 更彻底，而且是硬件保证的。
          ⚠️ 注意不要复位自己正在用的东西 —— 所以先关中断再复位。 */
    RCC->APB1RSTR = 0xFFFFFFFFu;
    RCC->APB2RSTR = 0xFFFFFFFFu;
    RCC->APB1RSTR = 0u;
    RCC->APB2RSTR = 0u;
}

void gw_boot_port_jump(const gw_boot_plan_t *plan)
{
    uint32_t sp;
    uint32_t reset;
    void (*app_entry)(void);

    if (plan == 0) {
        return;                     /* 不跳：留在 Bootloader 里 */
    }

    sp    = plan->sp;
    reset = plan->reset;

    shutdown_everything();

    /* ---- ① 设置向量表偏移（VTOR）----
     *
     * ⚠️ 这一步最容易被漏掉，后果也最严重。
     *
     * Cortex-M3 的向量表默认在 0x00000000（该地址被映射到 Flash 起始，
     * 也就是 Bootloader 的向量表）。如果不改 VTOR，App 里任何一个中断
     * 都会跳到 **Bootloader 的中断处理程序** 上。
     *
     * 具体后果：App 的 SysTick 中断会跑进 Bootloader 的 SysTick_Handler，
     * 而那个函数可能引用 Bootloader 的全局变量 —— 于是 App 运行着
     * 却在改 Bootloader 的数据。表现是"莫名其妙的偶发异常"。
     *
     * 注意 VTOR 要求向量表地址按 32 字（128 字节）对齐。
     * 我们的分区地址是 2 KB 对齐的（0x08009000 / 0x0802B000 / 0x0804D000），
     * 天然满足。 */
    SCB->VTOR = plan->base;

    /* ---- ② 设置栈顶指针 ----
     * 必须用 **App 向量表的第 0 项**，不能用 Bootloader 自己的 ——
     * 否则 App 一用栈就会往下长，覆盖 Bootloader 还在用的 RAM 区域。
     *
     * `MSR MSP, r0` 之后，原来的栈内容就不重要了（我们已经不需要返回）。
     */
    __set_MSP(sp);

    /* ---- ③ 取复位向量并跳转 ----
     *
     * `plan->reset` 里带着 Thumb 位（最低位 = 1），这是硬件约定。
     * 直接用 BX 跳过去即可 —— 不需要手动清那一位。
     *
     * ⚠️ 这里不能用函数指针直接调用：
     *      ((void (*)(void))reset)();
     *    那样编译器会生成 BLX，并且**在调用前就把返回地址压栈** ——
     *    而那时我们刚换了 MSP，压栈位置是 App 的栈。
     *    虽然功能上也能跑，但如果 App 的栈顶恰好没准备好，
     *    这一次压栈就会写到非法地址。
     *
     *    用内联汇编 `BX` 更干净：它不压栈、不返回。
     */
    app_entry = (void (*)(void))reset;

    /* 用内联汇编做实际的跳转。
       `bx` 会读寄存器里的地址并跳到那里执行 —— 因为最低位是 1，
       硬件会自动进入 Thumb 状态。 */
    __asm volatile ("bx %0" : : "r" (reset) : );

    /* 走不到这里。加一句防止编译器警告"函数有返回路径"。
       如果真走到了，说明 App 的复位向量指向了一个能返回的地方 ——
       那本身也是 bug，让它在原地打转比跳回未知地址安全。 */
    (void)app_entry;
    for (;;) {
        /* 停在 Bootloader 里：等看门狗复位，或者被重新烧录 */
    }
}
