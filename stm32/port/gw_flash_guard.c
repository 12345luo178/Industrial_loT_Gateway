/**
 * @file    gw_flash_guard.c
 * @brief   Flash 访问互斥的"公共层"实现（钩子默认空实现；RTOS 里由 gw_rtos.c 覆盖）
 *
 *  设计说明见 inc/gw_flash_guard.h —— 这里只做两件事：
 *
 *    1) 给 `gw_flash_guard_enter/exit` 提供**弱符号空实现**
 *       （裸机与引导程序构建用它：单线程，Flash 操作天然串行）
 *    2) 把 `gw_flash_lock/unlock` 实现成
 *          "钩子 + 真正的 HAL_FLASH_Unlock/Lock"
 *
 *  ⚠️ 本文件**同时被应用镜像与引导程序镜像编译**（build_ota_images.ps1 会编
 *     port/ 下全部 .c）。所以它只能依赖 HAL 与这两个弱钩子，
 *     绝不能引用 FreeRTOS 或应用层的任何东西。
 */
#include "gw_flash_guard.h"

/* ---------------------------------------------------------------------------
 *  弱符号钩子：默认什么都不做
 *
 *  ⚠️ RTOS 构建里 `port/gw_rtos.c` 会给同名的**强定义**（互斥量），
 *     链接器自动覆盖这里。少一边不会报错 —— 只会静默失去保护，
 *     所以 CI 有文本检查盯着成对存在（tools/check_rtos_hooks.py）。
 * ------------------------------------------------------------------------- */
__attribute__((weak)) void gw_flash_guard_enter(void)
{
}

__attribute__((weak)) void gw_flash_guard_exit(void)
{
}

HAL_StatusTypeDef gw_flash_lock(void)
{
    HAL_StatusTypeDef st;

    /* ⚠️ 顺序：**先拿互斥，再解锁 Flash**。
       反过来会出现"已经解锁了但还没拿到互斥"的窗口 —— 另一个任务可以在这个
       窗口里开始它的 Flash 操作，两边照样撞车。 */
    gw_flash_guard_enter();

    st = HAL_FLASH_Unlock();
    if (st != HAL_OK) {
        /* ⚠️ 解锁失败必须**把互斥放掉**再返回：否则调用方 `return -x` 走了，
           互斥永远被这个任务持有 —— 下一次任何 Flash 操作都会死等。 */
        gw_flash_guard_exit();
    }
    return st;
}

void gw_flash_unlock(void)
{
    /* ⚠️ 顺序与上面相反：**先锁 Flash，再放互斥**。
       反过来的话，另一个任务可能在你还没上锁时就拿到互斥开始操作 ——
       而你的 HAL_FLASH_Lock() 紧接着会把它正在做的事打断。 */
    (void)HAL_FLASH_Lock();

    gw_flash_guard_exit();
}
