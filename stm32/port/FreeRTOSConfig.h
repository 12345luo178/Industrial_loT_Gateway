/**
 * @file    FreeRTOSConfig.h
 * @brief   ★ 本项目的 FreeRTOS 配置（复制到你的 CubeMX 工程 Include 路径下）
 *
 * ============================================================================
 *  适配的版本：FreeRTOS Kernel **V10.3.1**（STM32Cube_FW_F1_V1.8.7 自带）
 *  位置：`Middlewares/Third_Party/FreeRTOS/Source`
 *  移植层：`Source/portable/GCC/ARM_CM3`（Cortex-M3）
 *  堆管理：`Source/portable/MemMang/heap_4.c`
 *
 *  ⚠️ 如果你用 CubeMX 生成的 FreeRTOS，它会**覆盖**本文件。
 *     两种情况二选一：
 *
 *     (a) 让 CubeMX 生成 FreeRTOS，然后把它生成的 FreeRTOSConfig.h
 *         与本文件**逐项对比**，把下面标了 ★ 的项改过去。
 *         ★ 标记的是"错了会出问题"的项，其余用 CubeMX 的默认值即可。
 *
 *     (b) 不用 CubeMX 的 FreeRTOS 中间件，手动把 Source 下的
 *         tasks.c / queue.c / list.c / timers.c + portable 加进工程，
 *         直接用本文件。
 *
 *  ----------------------------------------------------------------------------
 *  本配置针对本项目的三个特点做了取舍
 *
 *  **① 时间基准用 1 kHz（1 ms tick）**
 *     本项目最紧的硬超时是 Modbus 从站响应 5000µs（来自
 *     `gw_app_task_defs()` 的 deadline 字段）。1 ms 的 tick 分辨率
 *     对这种"毫秒级硬超时"够用 —— 因为硬超时不是靠 tick 判的，
 *     而是靠 DWT 微秒计时（`gw_timing` / `gw_sched`）。
 *     tick 只负责"该跑哪个任务了"。
 *
 *  **② 抢占式调度 + 时间片轮转**
 *     Modbus 从站必须在帧收完后立刻响应（3.5 字符的帧界之后
 *     上位机就开始等应答了）。协作式调度做不到这个确定性。
 *
 *  **③ 空闲任务里喂狗 —— 这是 IWDG 能可靠工作的关键**
 *     裸机下不能开 IWDG 的原因：Flash 擦除（20~40 ms）、
 *     DHT22 时序（关中断 5 ms）都是长阻塞路径，喂狗时机不可控。
 *
 *     上 RTOS 之后不一样了：
 *       - 喂狗放在**空闲任务钩子**里
 *       - 而空闲任务只有在"所有就绪任务都阻塞了"时才跑
 *       - 所以"狗被喂了"等价于"所有任务都按时让出了 CPU"
 *       - 任一任务卡死 -> 空闲任务永远不跑 -> 狗叫 -> 复位
 *
 *     这是**用狗来检测"任务级死锁"**，而不是像裸机那样
 *     只能检测"主循环还在不在转"。见 `gw_rtos.c` 的 vApplicationIdleHook。
 * ============================================================================
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* ==================================================================
 *  调度器基本配置
 * ================================================================== */

/** ★ 抢占式调度：Modbus 从站需要确定性响应 */
#define configUSE_PREEMPTION                    1

/** ★ 时间片轮转：同优先级任务轮流跑。
 *  本项目的 6 个任务优先级都不相同（见 gw_rtos.c），
 *  所以轮转实际不生效 —— 但留着它，将来加同优先级任务时不会踩坑。 */
#define configUSE_TIME_SLICING                  1

/** ★ 空闲任务必须让出 CPU（配合 configUSE_IDLE_HOOK 喂狗）。
 *  设为 0 会让空闲任务死循环不让出，低优先级任务永远跑不到。 */
#define configUSE_IDLE_HOOK                     1

/** ★ 用 tick 钩子做运行统计（uxTaskGetSystemState 需要） */
#define configUSE_TICK_HOOK                     0

/** ★ 1 kHz：1 ms 一个 tick */
#define configCPU_CLOCK_HZ                      (72000000UL)
#define configTICK_RATE_HZ                      ((TickType_t)1000)

#define configMAX_PRIORITIES                    (7)

/** ★ 最小栈深度。
 *  Cortex-M3 上 StackType_t 是 4 字节，所以 128 字 = 512 字节。
 *  ⚠️ 这个宏是"下限校验"用：xTaskCreate 传入的深度小于它会断言失败。 */
#define configMINIMAL_STACK_SIZE                ((unsigned short)128)

#define configMAX_TASK_NAME_LEN                 (12)

#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1

/* ==================================================================
 *  同步原语
 * ================================================================== */

/** ★ 互斥量：Modbus 寄存器表被多个任务共享（从站/CAN/管道同步），
 *  必须用互斥量保护。而且互斥量的**优先级继承**能缓解优先级反转 ——
 *  这在"高优先级从站任务"与"低优先级上云任务"争寄存器表时很关键。 */
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           1

/** ★ 队列集：本项目不用（各任务独立轮询自己的输入），关掉省 RAM */
#define configUSE_QUEUE_SETS                    0
#define configQUEUE_REGISTRY_SIZE               0

/** ★ 任务通知：比信号量更省 RAM 且更快。
 *  本项目暂时没用（任务都是周期轮询模型），留着以便将来优化。 */
#define configUSE_TASK_NOTIFICATIONS            1

/* ==================================================================
 *  内存管理
 * ================================================================== */

/** ★ 动态分配（heap_4：带合并的首次适配，抗碎片最好） */
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configSUPPORT_STATIC_ALLOCATION         0

/** ★ 堆大小。
 *
 *  ⚠️⚠️ 2026-10-11 修：原来是 **8 KB**，而下面那段"估算依据"把 6 个任务栈
 *      写成"≈ 5.5 KB"（`gw_rtos.h` 里算出来是 8.2 KB）—— 两处**自相矛盾**，
 *      而堆是按那个偏小的数配的。真实算式（1 字 = 4 字节）：
 *
 *        6 个任务栈（gw_rtos.h）：384+384+256+512+256+256 = 2048 字 = 8192 B
 *        + 空闲任务栈 configMINIMAL_STACK_SIZE（128 字）          =  512 B
 *        + 7 个 TCB（Cortex-M3 约 92~100 B/个，按 128 B 保守算）  =  896 B
 *        + heap_4 每块 8 B 头 + 8 B 对齐（7 块）                  =  112 B
 *        + 余量                                                  = 1024 B
 *        --------------------------------------------------------------
 *        合计需要 ≈ 10736 B  ->  取 **12 KB（12288 B）**，余约 1.5 KB
 *
 *  ⚠️ 为什么 8 KB 不行：8 KB 连**任务栈本身**都放不下（8192 B 正好用满），
 *     `xTaskCreate` 会失败，启动直接走到 `[FATAL] task create failed`。
 *     而这条**只在真上板启动 RTOS 时**才暴露 —— 编译、链接、CI 全都不报。
 *
 *  ⚠️ **别再手算**：`tools/check_rtos_hooks.py`（已挂进 CI）会解析
 *     `gw_rtos.h` 里的任务栈尺寸与这里的堆大小，机械地算一遍并断言够用。
 *     以后改栈尺寸或改堆，CI 立刻给出"够/不够、差多少"。
 *
 *  ⚠️ 这个值仍**必须上板用 uxTaskGetStackHighWaterMark() 校准**：
 *     栈给多了浪费 RAM，给少了是**偶发崩溃**（症状随机、极难定位）。 */
/* ⚠️ 2026-10-11 12KB -> 14KB：加了 UI 任务（栈 2KB）之后，
   check_rtos_hooks.py 的堆预算检查算出需要 13120B（7 个任务栈 10240
   + 空闲 512 + TCB 1024 + 块开销/互斥量 320 + 余量 1024），12KB 差 832B。
   ⚠️ 这个坑只在**真上板启动 RTOS** 时才暴露（链接与编译都不报），
   所以这个检查值得留着 —— 它比人算得准。 */
#define configTOTAL_HEAP_SIZE                   ((size_t)(16 * 1024))

#define configAPPLICATION_ALLOCATED_HEAP        0

/* ==================================================================
 *  钩子函数
 * ================================================================== */

/** ★ 栈溢出钩子：**必须开**。
 *  栈溢出在 Cortex-M3 上的表现是"随机 HardFault"（返回地址被覆盖），
 *  没有这个钩子就只能看到一个与栈无关的 PC，排查成本极高。
 *  有了它可以直接打印出是哪个任务溢出了。 */
#define configCHECK_FOR_STACK_OVERFLOW          2

/** ★ 内存分配失败钩子：堆不够时能立刻发现，
 *  而不是在某个任务的 xTaskCreate 里静默失败（那样任务根本没建起来）。 */
#define configUSE_MALLOC_FAILED_HOOK            1

/** ★ 运行时间统计：用于实测各任务真实 CPU 占用。
 *  本项目额外有自己的 DWT 时间预算统计（gw_timing），
 *  两者互补：FreeRTOS 给"任务级"占用，gw_timing 给"微秒级"分布。 */
#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_TRACE_FACILITY                1
#define configUSE_STATS_FORMATTING_FUNCTIONS    1

/* ==================================================================
 *  软件定时器
 * ================================================================== */

/** ★ 本项目不用软件定时器（所有周期任务自己 vTaskDelay），
 *  关掉可以省一个任务与它的栈。 */
#define configUSE_TIMERS                        0

/* ==================================================================
 *  Cortex-M3 中断优先级
 *
 *  ⚠️⚠️ 这两个宏配错是**最常见的 FreeRTOS 移植问题**，而且症状隐蔽：
 *      配错的后果是"configASSERT 在中断里失败"或者
 *      "中断里调 API 导致随机崩溃"。
 * ================================================================== */

/** ★ 内核不进临界区的中断优先级阈值。
 *  数值 = (8 - 抢占优先级位数)。STM32 的 NVIC 用 4 位优先级，
 *  所以可取 5。**优先级数值 >= 5 的中断可以安全调用 FromISR API**；
 *  数值更小（更紧急）的中断绝不能调。 */
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY    5

/** ★ 内核认为"最低优先级"的值（用于初始化 BASEPRI 寄存器） */
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY         15

/** ★ NVIC 用几位优先级。STM32F1 的 Cortex-M3 是 **4 位**。
 *
 *  ⚠️ 必须定义在下面那两个用到它的宏**之前** —— 宏展开是文本替换，
 *     顺序反了会得到 `<< (8 - configPRIO_BITS)` 里一个未定义的名字，
 *     而报错会指向 FreeRTOS 的内核头文件，看起来像"内核配置错了"。 */
#define configPRIO_BITS                         4

/** ★ 下面两个是给内核用的移位后的值（不要改） */
#define configKERNEL_INTERRUPT_PRIORITY \
    (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* ==================================================================
 *  断言
 *
 *  ⚠️ 开发阶段**必须开**。
 *  它会在参数非法时立刻停在出错的那一行（而不是几小时后以别的形式爆出来）。
 *  常见触发：xTaskCreate 栈深度小于 configMINIMAL_STACK_SIZE、
 *           在中断里调了非 FromISR 的 API、优先级越界。
 *
 *  量产前可以关掉（省几百字节），但**上板调试阶段一定开着**。
 * ================================================================== */
#ifndef NDEBUG
#define configASSERT(x)  do { if ((x) == 0) { taskDISABLE_INTERRUPTS(); for (;;) { } } } while (0)
#endif

/* ---- API 裁剪：只保留本项目用到的 ---- */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetCurrentTaskHandle       1

/* ==================================================================
 *  ★★ 三个中断向量名必须**在这里**重定向（最容易漏、且漏了很沉默）
 *
 *  FreeRTOS 的 Cortex-M3 移植在 `port.c` 里定义的是这三个名字：
 *
 *      vPortSVCHandler / xPortPendSVHandler / xPortSysTickHandler
 *
 *  而启动文件（`startup_stm32f103zetx.s`）的向量表里是 CMSIS 标准名：
 *
 *      SVC_Handler / PendSV_Handler / SysTick_Handler   （且是**弱符号**）
 *
 *  所以必须把内核那几个名字**重定向到标准名**，否则：
 *      向量表指向的还是启动文件里那个弱定义（死循环/默认处理）
 *      -> 内核的 SVC 从没被执行 -> **第一次任务切换就 HardFault**
 *      而现场看起来与任务毫无关系（PC 指向某个奇怪的地方），极难联想。
 *
 *  ⚠️⚠️ **为什么必须写在本文件里，而不是 main.c**：
 *      port.c 是通过 `#include "FreeRTOS.h"` -> `FreeRTOSConfig.h`
 *      才看到这些宏的。写在 main.c 里对 **port.c 的编译毫无影响** ——
 *      链接不会报错（vPortSVCHandler 只是没人调用），
 *      但向量表没接上，运行时才炸。迁移指南原来就是那么写的（已更正）。
 *      官方模板 `FreeRTOSConfig_template.h` 也把它们放在这个文件里。
 * ================================================================== */
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

/* ==================================================================
 *  ⚠️ 本文件**不要**声明钩子函数的原型
 *
 *  FreeRTOS 的内核源文件（tasks.c）自己会声明它们：
 *
 *      extern void vApplicationStackOverflowHook(TaskHandle_t xTask,
 *                                                char *pcTaskName);
 *
 *  如果你在 FreeRTOSConfig.h 里也声明一遍，签名必须**逐字相同** ——
 *  写成 `void *xTask` 会报：
 *
 *      error: conflicting types for 'vApplicationStackOverflowHook'
 *      note: previous declaration ... with type 'void(void *, char *)'
 *
 *  这个报错指向 tasks.c 的第 407 行（内核文件），看起来像
 *  "FreeRTOS 内核有问题"，实际原因在你自己的配置头里。
 *  **让内核去声明，自己只管定义。**
 * ================================================================== */

#endif /* FREERTOS_CONFIG_H */
