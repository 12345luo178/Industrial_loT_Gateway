/**
 * @file    gw_rtos.c
 * @brief   FreeRTOS 层实现：建任务、喂狗、栈水位
 */
#include "gw_rtos.h"
#if defined(GW_USE_LVGL)
#include "lvgl_port.h"
#endif

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "stm32f1xx_hal.h"
#include "gw_config.h"
#include "gw_fault_port.h"
#include "gw_log.h"
#include "gw_timing.h"

#include <stdio.h>
#include <string.h>

/* ==================================================================
 *  CubeMX 生成的句柄
 *
 *  ⚠️ 它们是 CubeMX 在 main.c 里定义的**全局变量**，
 *     没有对应的头文件声明（CubeMX 生成的代码风格如此）。
 *     所以这里必须手动 extern —— 少一个就是链接错误
 *     "undefined reference to huart1"。
 *
 *  ⚠️ 名字必须与 CubeMX 生成的一致。如果你在 CubeMX 里
 *     改了 USART1 的实例名（例如 USART1_Init 叫 MX_USART1_UART_Init
 *     但句柄叫 huart1），以实际生成的为准。
 * ================================================================== */
extern UART_HandleTypeDef huart1;   /* 调试/日志串口 */
extern IWDG_HandleTypeDef hiwdg;    /* 独立看门狗 */

/* ==================================================================
 *  内部状态
 * ================================================================== */

/** 6 个任务的句柄（与 gw_task_id_t 一一对应） */
static TaskHandle_t s_handle[GW_TASK_COUNT];

/** 任务与栈尺寸的对应表（顺序必须与 gw_app_task_defs() 一致） */
typedef struct {
    const char *name;
    uint16_t    stack;
    uint8_t     prio;
} rtos_task_cfg_t;

static const rtos_task_cfg_t s_cfg[GW_TASK_COUNT] = {
    /* 顺序与 gw_app_task_defs() 严格一致：
       SLAVE / MASTER / CAN / UPLINK / SENSOR / HOUSE */
    { "SLAVE",  GW_STACK_SLAVE,  GW_PRIO_SLAVE  },
    { "MASTER", GW_STACK_MASTER, GW_PRIO_MASTER },
    { "CAN",    GW_STACK_CAN,    GW_PRIO_CAN    },
    { "UPLINK", GW_STACK_UPLINK, GW_PRIO_UPLINK },
    { "SENSOR", GW_STACK_SENSOR, GW_PRIO_SENSOR },
    { "HOUSE",  GW_STACK_HOUSE,  GW_PRIO_HOUSE  },
    { "UI",     GW_STACK_UI,     GW_PRIO_UI     }
};

static gw_app_t  *s_app       = 0;
static gw_sched_t s_sched;

static uint32_t s_feed_ok     = 0u;
static uint32_t s_feed_reject = 0u;
static uint8_t  s_iwdg_on     = 0u;

/* ==================================================================
 *  喂狗
 *
 *  ⚠️ 这是整个 RTOS 化里最关键的一段，理由见 gw_rtos.h 的文件头。
 *
 *  核心：**喂狗前先问"该做的事都做了吗"**。
 *  无条件喂狗等于没养狗 —— 一个卡死在死循环里的任务，
 *  主循环（或空闲任务）照样会被调度到，照样喂狗，永远不复位。
 *
 *  ⚠️⚠️ 2026-10-11 修正（缺陷 U）：这里原来写着
 *      "gw_app_watchdog_ok() 会问调度器'6 个任务是不是都跑过一轮了'" ——
 *      **那是假的**，因为 `gw_rtos_create_tasks()` 故意不调
 *      `gw_app_enable_sched()`（怕 `gw_app_poll()` 又去分派任务），
 *      于是 `app->sched == NULL` -> 判据走了"无调度器 -> **无条件允许**"
 *      那条路 -> **RTOS 下等于无条件喂狗**，而三处文档都说它在检查。
 *
 *      现在拆开了：**分派用 `app->sched`，判据用 `app->wd_sched`**，
 *      由 `gw_rtos_create_tasks()` 填 `wd_sched`；
 *      判据本身用**按任务的心跳 + 截止时间**（见 gw_sched.c 判据一），
 *      心跳由每个任务体每轮调 `gw_sched_note_beat()` 登记。
 * ================================================================== */
/* 毫秒时间源：直接读内核 tick（configTICK_RATE_HZ = 1000 -> 1 tick = 1ms）。
 * ⚠️ 传 NULL 也不会崩：调度器没起来时 xTaskGetTickCount() 返回 0，
 *    而心跳判据在"还没人登记过心跳"时以启动基准计时，
 *    所以启动瞬间不会被误判成"全部迟到"。 */
static uint32_t rtos_now_ms(void *ctx)
{
    (void)ctx;
    return (uint32_t)xTaskGetTickCount();
}

/* ==================================================================
 *  ★★ 主循环"内务"的 RTOS 侧入口（**弱符号**，模板给强定义覆盖）
 *
 *  ⚠️⚠️ 为什么必须有它（缺陷 W，2026-10-11）：
 *     main 被拆成"裸机主循环 / RTOS 分支"之后，**循环里剩下的内务**
 *     没人调了 —— 而它们一个都不是可选的：
 *
 *        gw_fault_port_tick()      -> 运行时长不推进（OTA 确认要读它）
 *        gw_log_tick()             -> 日志时标停摆
 *        gw_ota_bridge_tick()      -> **OTA 传输状态机不走 -> 收不下固件**
 *        gw_ota_confirm_running()  -> **升级永不确认 -> 3 次后必然回滚**
 *        上报节拍 / 波特率切换 / 重启挂起 -> 全失效
 *
 *     一次性把 RTOS 下的 OTA 与上报整条链路打死，而且**完全静默**：
 *     编译、链接、CI 都不报（这正是本文件里反复出现的那类失效）。
 *
 *  ⚠️ 弱定义在前、模板里的强定义在后 —— 这条链**必须**成对存在：
 *     只有弱定义 = 什么都不做（缺陷 W 悄悄回来）。
 *     所以 CI 有文本检查盯着两边（tools/check_rtos_hooks.py）。
 * ================================================================== */
__attribute__((weak)) void gw_rtos_ui_tick(uint32_t dt_ms)
{
    (void)dt_ms;         /* 弱实现：工程侧不覆盖就是不显示界面 */
}

__attribute__((weak)) void gw_rtos_housekeeping(uint32_t dt_ms)
{
    /* 没接内务实现时什么都不做（裸机模板不引用本文件，所以正常不会走到）。 */
    (void)dt_ms;
}

/* ==================================================================
 *  ★★ Flash 访问互斥（缺陷 X，2026-10-11）
 *
 *  ⚠️ 裸机下 Flash 操作天然串行（只有一个主循环），RTOS 下不是：
 *
 *      HOUSE 任务正在擦 OTA 槽（1.4~2.7 秒，逐页擦 + 逐页让出 CPU）
 *      从站任务同时收到"写保持寄存器" -> 参数保存 -> 也在写 Flash
 *
 *  STM32F1 的 Flash 控制器只有一个（擦/编是通过 FLASH->CR / FLASH->AR
 *  下命令的），两边交错会：
 *      · 一方 Lock 落在另一方 Program 中间 -> 对方失败
 *      · **另一方的 Erase 等 BSY 时页地址被覆盖 -> 擦错页**
 *        （擦到运行中的应用页 = 当场变砖；擦到参数页 = 参数丢失）
 *
 *  ⚠️ 用**互斥量**而不是临界区：OTA 擦除会跨 `vTaskDelay(1)` 持有它
 *     （`gw_ota_port.c` 逐页让出 CPU），临界区跨阻塞调用会把调度器废掉。
 *  ⚠️ 弱定义（空实现）在 `port/gw_flash_guard.c`，这里给强定义覆盖。
 *     少一边不会报错 —— 只会静默失去保护，所以 CI 有文本检查盯着。
 * ================================================================== */
static SemaphoreHandle_t s_flash_mtx = 0;

/* ------------------------------------------------------------------
 *  ★★ 日志互斥（缺陷 Y，2026-10-11）
 *
 *  ⚠️ 缺陷 Y：`gw_log_write()` 直接改环形的 head/count/stat，而 RTOS 下
 *     **任何任务都可能写日志**。并发写会丢记录、乱序，两边的 sink 还会同时
 *     调 `HAL_UART_Transmit`（不可重入）—— 丢的往往正是故障前后那几条。
 *
 *  ⚠️ 用普通互斥量就够：`gw_log_write()` 与 `gw_log_read()` 各自"取一次放一次"，
 *     `gw_log_flush()` 整圈也只取一次（它用**不加锁**的内部读函数），
 *     **没有嵌套**（所以不需要 `configUSE_RECURSIVE_MUTEXES`，那个本项目是 0）。
 *
 *  ⚠️ 弱定义在 `src/gw_log.c`（空实现），这里给强定义覆盖。
 *     少一边不会报错 —— 只会静默失去保护，CI 有文本检查盯着。
 * ------------------------------------------------------------------ */
static SemaphoreHandle_t s_log_mtx = 0;

void gw_log_guard_enter(void)
{
    if (s_log_mtx != 0) {
        (void)xSemaphoreTake(s_log_mtx, portMAX_DELAY);
    }
}

void gw_log_guard_exit(void)
{
    if (s_log_mtx != 0) {
        (void)xSemaphoreGive(s_log_mtx);
    }
}

/* ------------------------------------------------------------------
 *  ★★ RS485 总线仲裁的原子性（缺陷 Z，2026-10-11）
 *
 *  ⚠️ 缺陷 Z：`rs485_arb_acquire()` 是"检查 -> 置 owner"两步。裸机下
 *     主站与从站的两段代码在同一个主循环里**顺序执行**，中间不会被换走；
 *     RTOS 抢占下会：
 *
 *        [主站] 判定可以拿        <-- 被抢占
 *        [从站] 判定也可以拿 -> owner=SLAVE，开始发送
 *        [主站] 继续 owner=MASTER，也开始发送  ==> **总线两个发送者**
 *
 *  ⚠️ 用**临界区**（不是互斥量）：这一段只有几条指令（约 1 µs），
 *     而且**必须**是"不许被换走"的语义 —— 用互斥量会让整个决策过程可能阻塞，
 *     反而放大了窗口。
 *  ⚠️ 只包"决策 + 改状态"，**不包**收发过程：从站抢占主站的等待窗口
 *     是仲裁器的设计语义，粗粒度锁会把它一起锁死。
 *  ⚠️ 临界区可嵌套，所以和调用方自己的临界区叠加也没问题。
 *  ⚠️ 弱定义（空实现）在 `src/rs485_arbiter.c`，这里给强定义覆盖。
 * ------------------------------------------------------------------ */
void rs485_arb_guard_enter(void)
{
    /* ⚠️ 调度器没起来时 taskENTER_CRITICAL() 也是安全的（就是关中断）。 */
    taskENTER_CRITICAL();
}

void rs485_arb_guard_exit(void)
{
    taskEXIT_CRITICAL();
}

void gw_flash_guard_enter(void)
{
    if (s_flash_mtx != 0) {
        /* ⚠️ portMAX_DELAY：Flash 操作迟早会结束，这里等下去是对的
           （超时返回反而会让两个任务同时进 Flash —— 那正是要防的事）。 */
        (void)xSemaphoreTake(s_flash_mtx, portMAX_DELAY);
    }
}

void gw_flash_guard_exit(void)
{
    if (s_flash_mtx != 0) {
        (void)xSemaphoreGive(s_flash_mtx);
    }
}

static void rtos_feed_watchdog(void)
{
    if (s_iwdg_on == 0u) {
        return;
    }

    /* gw_app_watchdog_ok() -> gw_sched_watchdog_ok()（判据一）：
       每个已启动且未挂起的任务，都必须在 period + GW_SCHED_WD_SLACK_MS
       之内登记过心跳。**任一任务卡死 -> 超过它的周期 + 余量后拒喂 -> 狗叫。**
       ⚠️ 检测延迟 = 该任务的 period + 余量（慢周期任务更久），
          不是固定的 1.6 秒。 */
    if (gw_app_watchdog_ok(s_app) != 0) {
        (void)HAL_IWDG_Refresh(&hiwdg);
        s_feed_ok++;
    } else {
        /* ⚠️ 不喂，但要计数。
           现场能从 gw_rtos_feed_rejected() 看出来"有过任务超时"，
           而不是只看到一个复位、不知道为什么。 */
        s_feed_reject++;
    }
}

/* ==================================================================
 *  FreeRTOS 钩子函数
 * ================================================================== */

/**
 * ★ 空闲任务钩子 —— 喂狗就放这里
 *
 * ⚠️ 空闲任务的优先级是 0（比所有业务任务都低），
 *    所以它只在**所有业务任务都阻塞时**才被调度到。
 *
 *    等一下 —— 那如果某个任务一直在跑（不让出 CPU），
 *    空闲任务就跑不到，狗就会叫。**这正是我们想要的**：
 *    "有个任务不让出 CPU"本身就是故障。
 *
 * ⚠️ 本钩子必须**极短**：它每次空闲都会跑，里面做的事会
 *    100% 占用所有剩余 CPU 时间。所以这里只做喂狗与计数，
 *    绝不做日志输出（串口发送会阻塞）。
 */
void vApplicationIdleHook(void)
{
    rtos_feed_watchdog();
}

/**
 * ★ OTA 擦除期间的让出钩子（`gw_ota_port.c` 里的**弱符号**占位）
 *
 *  ⚠️ 为什么必须有这个**强定义**（没有它也不会报错 —— 那才是危险的地方）：
 *     `gw_ota_port.c` 每擦一页都会调 `gw_ota_port_yield()`。若本文件不提供
 *     强定义，链接器就用那边那个**空的弱定义**，于是：
 *
 *         OTA 擦除 136 KB（1.4~2.7 秒）期间不让出 CPU
 *         -> 空闲任务跑不到 -> `vApplicationIdleHook()` 里的喂狗被跳过
 *         -> IWDG（2.13~4.27 秒）咬 -> **升级擦到一半被复位**
 *
 *     而这条链**只在真做一次 OTA 时**才暴露 —— 平时怎么看都是好的。
 *     （迁移指南 §7 结尾"如果上板发现升级时设备复位，先检查这一点"说的就是它；
 *       2026-10-11 之前这里确实没人定义，等于那条设计从未生效。）
 *
 *  ⚠️ 每页只让出 1 个 tick（`configTICK_RATE_HZ = 1000` -> 1 ms）：
 *     一页要 20~40 ms，让出 1 ms 几乎不影响总时长，但足够
 *     ① 让空闲任务喂上一次狗、② 让 Modbus 从站应答 ——
 *     上位机在传固件的同时可能还在读寄存器。
 *
 *  ⚠️ 加调度器状态判断：`vTaskDelay()` 在调度器启动**之前**调用是未定义行为
 *     （`pxCurrentTCB` 还是 NULL）。正常路径下 OTA 都发生在任务里，
 *     但这个保护很便宜，而"未定义行为"在真机上表现为随机的 HardFault。
 */
void gw_ota_port_yield(void)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        vTaskDelay(1);
    }
}

/**
 * ★ 栈溢出钩子
 *
 * 为什么必须有：栈溢出在 Cortex-M3 上表现为"随机 HardFault"
 * （返回地址被覆盖），没有这个钩子就只能看到一个与栈无关的 PC。
 *
 * configCHECK_FOR_STACK_OVERFLOW = 2 时，内核会在任务切换时
 * 检查栈末尾的标记字节是否被改写 —— 所以它能在**溢出发生的那一刻**
 * 抓到，而不是等到崩溃。
 *
 * ⚠️ 本函数在**中断上下文**被调用，且此时系统状态已不可靠：
 *    不能用任何可能阻塞的东西（vTaskDelay、信号量、带超时的串口发送）。
 *    这里只用"带超时的 HAL_UART_Transmit"直发，然后死循环等狗复位。
 */
void vApplicationStackOverflowHook(void *xTask, char *pcTaskName)
{
    char buf[64];
    int  n;

    (void)xTask;

    /* 直接把任务名打到调试串口 —— 这是本钩子唯一有价值的信息：
       "哪个任务的栈不够"。 */
    n = snprintf(buf, sizeof(buf), "\r\n[FATAL] stack overflow: %s\r\n",
                 (pcTaskName != 0) ? pcTaskName : "?");

    if (n > 0) {
        (void)HAL_UART_Transmit(&huart1, (uint8_t *)buf, (uint16_t)n, 100u);
    }

    /* 不再往下跑：此时内存已经被踩坏，
       继续执行只会以更难看的方式崩掉，还会破坏现场。
       等 IWDG 复位（如果没开狗，就停在这里等调试器）。 */
    taskDISABLE_INTERRUPTS();
    for (;;) { }
}

/**
 * ★ 内存分配失败钩子
 *
 * 不装这个钩子的后果：`xTaskCreate` 静默失败、
 * 任务根本没建起来，现场表现是"某个功能一直没有数据"，
 * 而查配置怎么都对。
 */
void vApplicationMallocFailedHook(void)
{
    static const char msg[] = "\r\n[FATAL] FreeRTOS heap exhausted\r\n";

    (void)HAL_UART_Transmit(&huart1, (uint8_t *)msg,
                            (uint16_t)(sizeof(msg) - 1u), 100u);

    taskDISABLE_INTERRUPTS();
    for (;;) { }
}

/* ==================================================================
 *  任务包装
 *
 *  ⚠️ 为什么不直接把 gw_app_task_entry_* 交给 xTaskCreate：
 *
 *    因为需要"周期节拍"。业务任务是**非阻塞的一小步**（设计约定），
 *    所以必须有人反复调它们。裸机下这是主循环的职责，
 *    RTOS 下就是每个任务自己的 for(;;) + vTaskDelayUntil。
 *
 *    ⚠️ 用 vTaskDelayUntil 而不是 vTaskDelay：
 *       vTaskDelay(100) 是"从**现在**起延 100ms"，
 *       所以任务体耗时会被累加到周期里 —— 实际周期是 100 + 耗时。
 *       vTaskDelayUntil 是"到**绝对时刻**再唤醒"，
 *       周期稳定在 100ms，不会漂移。
 * ================================================================== */
static void rtos_task_body(void *ctx, gw_task_id_t id, uint16_t period_ms,
                           void (*entry)(void *), uint32_t dump_s)
{
    TickType_t last     = xTaskGetTickCount();
    TickType_t last_hk  = last;     /* 内务的 dt 基准（只有 HOUSE 用） */
    TickType_t last_ent = last;     /* ★ 任务入口的 dt 基准（见下面那段） */
    uint32_t   elapsed  = 0u;       /* 本任务已运行的秒数（近似；用于定时打印栈水位） */
    uint32_t   next_at  = GW_RTOS_DUMP_FIRST_S;

    for (;;) {
        /* ==============================================================
         *  ★★ 把"本轮经过了多少毫秒"写进应用层上下文（缺陷 AE，2026-10-13）
         *
         *  ⚠️ 不写这一段的后果是**完全静默**的（编译、链接、CI 一个都不报）：
         *
         *     `app->tick_ms` 只在 `gw_app_poll()` 里被赋值，而 RTOS 构建下
         *     `gw_app_poll()` **根本不会被调用**（每个任务自己 for(;;)，
         *     主循环不存在）。于是 `app->tick_ms` 恒为 0，而
         *     `gw_app_task_entry_sensor()` 的第一句就是
         *
         *         gw_pipeline_tick(&app->pipe, app->tick_ms);
         *
         *     -> 采样累加器 `sample_acc_ms` 永远加 0
         *     -> `sample_due` 永不置位 -> **DHT22 一次都不采**
         *     -> `input[0]/[1]` 停在初始快照，屏幕上 TEMP/HUMI 恒显 `-- no DHT`。
         *
         *     症状极具误导性：**驱动是好的**（主机测试 57 项全过）、接线没动过，
         *     就是没有数据 —— 因为断的是**节拍源**，不是驱动。
         *     （同一个坑在裸机侧不存在：`gw_app_poll(elapsed_ms)` 每轮都写。）
         *
         *  ⚠️ 用**实测 tick 差**而不是 period_ms：
         *     period_ms 为 0 表示"每轮都跑"（SENSOR 正是这一类），
         *     直接写 0 等于没修；写死 1 又会在任务被抢占延迟唤醒时少算时间。
         *     实测差在两种情况下都对。
         *
         *  ⚠️ 兜底 `dt == 0` 必须保留：同一毫秒内连跑两轮（1ms tick 下会发生）
         *     会让累加器原地踏步 —— 那正是我们要修的这个病本身。
         *
         *  ⚠️ 多个任务写同一个字段是**安全的**，理由：
         *     目前唯一的读者是 SENSOR（优先级 3）。能抢占它的只有
         *     SLAVE(5)/MASTER(4)/CAN(4)，而这三个的 period 都是 0（每轮约 1ms），
         *     即使恰好在"写"与"读"之间插进来，覆盖上去的也还是 1ms 量级的值，
         *     不会把 2.5s 的采样周期带偏。优先级更低的任务
         *     （UPLINK/HOUSE/UI）根本抢占不了 SENSOR。
         * ============================================================== */
        if (s_app != 0) {
            TickType_t now_e = xTaskGetTickCount();
            uint32_t   dt_e  = (uint32_t)(now_e - last_ent);
            last_ent = now_e;
            if (dt_e == 0u) {
                dt_e = (period_ms != 0u) ? (uint32_t)period_ms : 1u;
            }
            s_app->tick_ms = dt_e;
        }

        /* 推进业务状态机的一小步 */
        entry(ctx);

        /* ★ 登记心跳（喂狗判据一的唯一输入）
         *
         * ⚠️ 为什么放在 entry() **之后**：心跳的含义是"这一轮真的跑完了"。
         *    放在前面的话，一个卡在 entry() 里的任务会先登记再卡死 ——
         *    而"卡死"正是我们要检出的东西，先登记就等于替它撒谎。
         *    （RTOS 下这一类主要由空闲钩子兜住：任务不让出 CPU，
         *      空闲任务根本调度不到，钩子里的喂狗自然也不会执行。） */
        gw_sched_note_beat(&s_sched, id);

        /* ★ 定期打印各任务的**栈水位**（uxTaskGetStackHighWaterMark）
         *
         * ⚠️ 为什么由固件自己打、而不是让用户临时加一行代码：
         *    栈水位是"能不能收紧栈尺寸"的**唯一依据**（gw_rtos.h 里那 2048 字
         *    只是估算）。上板后通常要跑够时间才看得出最坏情况 ——
         *    而"临时加一行 + 重新编译 + 重新烧录"很容易被漏掉或做错。
         *    这里由 HOUSE 任务（周期最长、开销最小）定期打一次：
         *    首次在启动后 GW_RTOS_DUMP_FIRST_S 秒，之后每 dump_s 秒一次。
         * ⚠️ 只在任务上下文里打（不要在空闲钩子/中断里做串口输出）。
         * ⚠️ 间隔为 0 = 不打（其它任务传 0）。 */
        if (dump_s != 0u) {
            elapsed += (period_ms != 0u) ? period_ms : 1u;
            if (elapsed >= next_at) {
                gw_rtos_dump_stack(s_app);
                next_at = elapsed + dump_s;
            }
        }

        /* ★★ 主循环的内务（缺陷 W）
         *
         * ⚠️ 由 **HOUSE 任务**承担（优先级最低 = 1、周期 10ms）：它天然就是
         *    "当别的事都忙完才轮到"的角色，OTA 擦除也会在这里跑 ——
         *    而 OTA 擦除正好需要低优先级 + 逐页让出（迁移指南 §7 的设计）。
         * ⚠️ dt 用**实测 tick 差**而不是 period_ms：任务被抢占而延迟唤醒时，
         *    上报节拍与波特率静默期才不会少算时间。 */
        if (id == GW_TASK_HOUSE) {
            TickType_t now_t = xTaskGetTickCount();
            uint32_t   dt_ms = (uint32_t)(now_t - last_hk);
            last_hk = now_t;
            gw_rtos_housekeeping(dt_ms);
        }

        /* ★ UI 也走这条 dt 驱动的钩子（与 HOUSE 同样的写法）。
         * ⚠️ 它的入口是空的：真正干活的是这个钩子，这样
         *    "多久刷新一次"由周期决定，而 dt 用**实测值**。 */
        if (id == GW_TASK_UI) {
            /* ⚠️ 自己取实测 dt：上面的 dt_ms 是 HOUSE 那个块的局部变量
               （初版写成直接用 dt_ms，编译器直接报 'dt_ms' undeclared —— 好事）。
               last_hk 是 rtos_task_body 的局部变量，每个任务各一份，互不干扰。 */
            TickType_t now_u = xTaskGetTickCount();
            uint32_t   dt_u  = (uint32_t)(now_u - last_hk);
            last_hk = now_u;

#if defined(GW_USE_LVGL)
            /* LVGL 的时基与定时器处理挂在这个**已有的** UI 钩子里：
               不新增任务、不改任务表、不动栈预算。dt_u 是实测周期。
               ⚠️ 必须放在 dt_u 声明之后（第一版放前面，编译直接报 undeclared）。 */
            lvgl_port_tick(dt_u);
            lvgl_port_handler();
#endif

            gw_rtos_ui_tick(dt_u);
        }

        TickType_t before_wait = xTaskGetTickCount();

        if (period_ms == 0u) {
            /* period 为 0 表示"每轮都跑"。
               ⚠️ 但不能不延时 —— 那样会把所有低优先级任务饿死
                  （本任务永远就绪）。让出 1 个 tick 即可。 */
            vTaskDelay(1u);
        } else {
            vTaskDelayUntil(&last, pdMS_TO_TICKS(period_ms));

            /* ⚠️⚠️ 迟到时必须主动让一拍（2026-10-11 板上实测的饥饿 bug）：
             *   vTaskDelayUntil 等的是"**绝对时刻**"。若本轮耗时已经 >= 周期
             *   （内务里的日志是**同步直接写 UART**，一行就是 1ms 级），
             *   它的期限早就过了，于是**立刻返回**、本任务保持就绪 ->
             *   比它优先级低的任务（**包括空闲任务 —— 那正是喂狗那条链**）
             *   永远得不到 CPU -> 看门狗复位循环。
             *   这正是上面 period==0 那段注释警告过的情形，只是没覆盖周期路径。
             *   让出 1 个 tick 即可，而且**不破坏看门狗语义**：
             *   真正卡死的任务根本走不到这一行。 */
            if ((xTaskGetTickCount() - before_wait) < pdMS_TO_TICKS(1u)) {
                vTaskDelay(1u);
            }
        }
    }
}

/* 6 个薄包装。手写而不是用宏，是为了让调试器里的调用栈可读 ——
   宏生成的名字在栈回溯里看不出是哪个任务。
   ⚠️ 最后一个参数是"多久打一次栈水位"，只有 HOUSE 打（0 = 不打）。 */
static void task_slave (void *ctx) { rtos_task_body(ctx, GW_TASK_SLAVE,  0u,  gw_app_task_entry_slave,  0u); }
static void task_master(void *ctx) { rtos_task_body(ctx, GW_TASK_MASTER, 0u,  gw_app_task_entry_master, 0u); }
static void task_can   (void *ctx) { rtos_task_body(ctx, GW_TASK_CAN,    0u,  gw_app_task_entry_can,    0u); }
static void task_uplink(void *ctx) { rtos_task_body(ctx, GW_TASK_UPLINK, 0u,  gw_app_task_entry_uplink, 0u); }
static void task_sensor(void *ctx) { rtos_task_body(ctx, GW_TASK_SENSOR, 0u,  gw_app_task_entry_sensor, 0u); }
static void ui_entry_nop(void *ctx) { (void)ctx; }   /* UI 干活在 gw_rtos_ui_tick 钩子里 */
static void task_ui    (void *ctx) { rtos_task_body(ctx, GW_TASK_UI,     50u, ui_entry_nop,            0u); }
static void task_house (void *ctx) { rtos_task_body(ctx, GW_TASK_HOUSE,  10u, gw_app_task_entry_house,
                                                    GW_RTOS_DUMP_EVERY_S); }

static void (*const s_bodies[GW_TASK_COUNT])(void *) = {
    task_slave, task_master, task_can, task_uplink, task_sensor, task_house,
    task_ui                     /* ⚠️ 必须与 gw_task_id_t 的顺序一致 */
};

/* ==================================================================
 *  对外接口
 * ================================================================== */

int gw_rtos_create_tasks(gw_app_t *app)
{
    uint8_t i;

    if (app == 0) {
        return -1;
    }
    s_app = app;

    /* 调度器上下文：让应用层能查"哪个任务超时了"。
       ⚠️ 但这里**不调用 gw_app_enable_sched()** —— 那个是
          "协作式调度"模式用的（gw_app_poll 会按调度器分派）。
          RTOS 下每个任务自己循环，主循环不存在，
          如果还挂着调度器，两边都会去推进同一个任务。

       那为什么还要 s_sched？因为统计功能需要它：
       gw_sched_stat() 记录的 deadline_miss 是判断
       "有没有任务超时"的唯一依据。 */
    if (gw_sched_init(&s_sched, gw_app_task_defs(), 0, 0) != 0) {
        return -2;
    }

    /* ★ 接上毫秒时间源（喂狗判据一要用），必须在 start() 之前 ——
       start() 会记下"心跳计时基准"，不然启动瞬间会被误判成"全部迟到"。 */
    gw_sched_set_clock_ms(&s_sched, rtos_now_ms, 0);
    gw_sched_start(&s_sched);

    /* ★★ 把判据挂到应用层（缺陷 U 的修法核心）     *
     * ⚠️ 必须用 `gw_app_enable_watchdog_sched()` 而**不是**
     *    `gw_app_enable_sched()`：后者会同时填 `app->sched`，
     *    那会让 `gw_app_poll()` 又去按调度器分派任务 —— 而 RTOS 下
     *    任务由内核调度，两边都推进同一个任务是打架。
     * ⚠️ 这一步被删掉的后果是**静默的**：喂狗判据回落到"无调度器 ->
     *    无条件允许"，也就是**没养狗**，而表面一切正常。
     *    CI 里有检查盯着这一句（tools/check_rtos_hooks.py）。 */
    gw_app_enable_watchdog_sched(app, &s_sched);

    /* ★★ Flash 访问互斥量（缺陷 X）—— 必须在建任务之前建好：
     *    任务一跑起来就可能同时碰 Flash（HOUSE 擦 OTA 槽 / 从站存参数）。
     *  ⚠️ 它的开销（约 80 B + 块头）已经算进 tools/check_rtos_hooks.py 的堆预算里。
     *  ⚠️ 建失败（堆不够）时 `s_flash_mtx` 保持 0，守卫退化成"不互斥但不崩" ——
     *     这是**有意的降级**：宁可少一层保护，也不要启动就 FATAL。
     *     （真缺堆的话，上面 xTaskCreate 会更早失败，不会走到这里。） */
    s_flash_mtx = xSemaphoreCreateMutex();
    s_log_mtx   = xSemaphoreCreateMutex();   /* 日志互斥（缺陷 Y） */

    for (i = 0u; i < (uint8_t)GW_TASK_COUNT; i++) {
        BaseType_t r;

        r = xTaskCreate(s_bodies[i], s_cfg[i].name,
                        (configSTACK_DEPTH_TYPE)s_cfg[i].stack,
                        app,
                        (UBaseType_t)s_cfg[i].prio,
                        &s_handle[i]);
        if (r != pdPASS) {
            /* ⚠️ 返回"第几个失败"而不是笼统的 -1：
               现场要能知道是哪个任务没建起来。
               常见原因：configTOTAL_HEAP_SIZE 不够。 */
            return -(int)(i + 3);
        }
    }

    return 0;
}

void gw_rtos_start(gw_app_t *app)
{
    (void)app;

    /* ⚠️ vTaskStartScheduler() 正常情况下**不返回**。
       一旦返回，唯一的解释是"堆不够，空闲任务创建失败"。 */
    vTaskStartScheduler();

    /* 走到这里说明调度器起不来。
       ⚠️ 不能 return —— 那样调用方会以为一切正常，继续往下跑
          一个没有任何任务在运行的系统（表现是"什么都不工作"）。 */
    {
        static const char msg[] =
            "\r\n[FATAL] vTaskStartScheduler failed (heap?)\r\n";

        (void)HAL_UART_Transmit(&huart1, (uint8_t *)msg,
                                (uint16_t)(sizeof(msg) - 1u), 200u);
    }
    taskDISABLE_INTERRUPTS();
    for (;;) { }
}

/* ==================================================================
 *  IWDG
 * ================================================================== */
int gw_rtos_iwdg_enable(int enable)
{
    if (enable == 0) {
        /* ⚠️ IWDG 一旦启动就**无法用软件停止**（除非复位）。
           所以这里的"关闭"只是"不再使能"：
           如果已经使能过，这个调用没有实际作用，复位后才会失效。 */
        s_iwdg_on = 0u;
        return 0;
    }

    /* ⚠️ 先标记，再使能：HAL_IWDG_Init 之后计数器就开始跑了，
       中间不能有"还没标记就已经可能被喂/不喂"的窗口。 */
    s_iwdg_on = 1u;

    if (HAL_IWDG_Init(&hiwdg) != HAL_OK) {
        s_iwdg_on = 0u;
        return -1;
    }
    return 0;
}

/* ==================================================================
 *  栈水位诊断
 * ================================================================== */
uint16_t gw_rtos_stack_free(gw_task_id_t id)
{
    if ((uint8_t)id >= (uint8_t)GW_TASK_COUNT) {
        return 0u;
    }
    if (s_handle[id] == 0) {
        return 0u;              /* 任务还没建起来 */
    }
    /* ⚠️ uxTaskGetStackHighWaterMark 返回的是**剩余**字数的
       **历史最小值**。所以"返回值很小"意味着"曾经用到很深"——
       这正是我们要看的。 */
    return (uint16_t)uxTaskGetStackHighWaterMark(s_handle[id]);
}

uint16_t gw_rtos_stack_size(gw_task_id_t id)
{
    if ((uint8_t)id >= (uint8_t)GW_TASK_COUNT) {
        return 0u;
    }
    return s_cfg[id].stack;
}

void gw_rtos_dump_stack(gw_app_t *app)
{
    uint8_t i;

    (void)app;

    for (i = 0u; i < (uint8_t)GW_TASK_COUNT; i++) {
        uint16_t total = s_cfg[i].stack;
        uint16_t freew = gw_rtos_stack_free((gw_task_id_t)i);
        uint32_t pct;
        const char *mark = "";

        if (s_handle[i] == 0) {
            continue;           /* 没建起来的任务不报（否则误导） */
        }

        pct = (total == 0u) ? 0u : ((uint32_t)freew * 100u / (uint32_t)total);

        /* ⚠️ 阈值判断的意义：
              < 20% 剩余 -> 危险，随时可能溢出（症状是随机 HardFault）
              > 60% 剩余 -> 可以减小，省 RAM 给将来加功能
           中间区间是"合适"。 */
        if (pct < 20u) {
            mark = "  <-- 危险，栈要加大";
        } else if (pct > 60u) {
            mark = "  (可减小)";
        }

        {
            char buf[72];
            int  n = snprintf(buf, sizeof(buf),
                              "[STACK] %-7s 剩余 %3u/%3u 字 (%2u%%)%s\r\n",
                              s_cfg[i].name, (unsigned)freew,
                              (unsigned)total, (unsigned)pct, mark);

            if (n > 0) {
                (void)HAL_UART_Transmit(&huart1, (uint8_t *)buf,
                                        (uint16_t)n, 50u);
            }
        }
    }

    /* 顺带报喂狗统计 —— 它们与栈水位一起看才有意义：
       "栈还够但喂狗被拒"说明是**逻辑卡死**，不是栈的问题。 */
    {
        char buf[96];
        int  n = snprintf(buf, sizeof(buf),
                          "[STACK] 喂狗 %lu 次 / 被拒 %lu 次%s\r\n",
                          (unsigned long)s_feed_ok,
                          (unsigned long)s_feed_reject,
                          (s_feed_reject != 0u) ? "  <-- 有任务超时！" : "");

        if (n > 0) {
            (void)HAL_UART_Transmit(&huart1, (uint8_t *)buf,
                                    (uint16_t)n, 50u);
        }
    }
}

uint32_t gw_rtos_feed_count(void)    { return s_feed_ok; }
uint32_t gw_rtos_feed_rejected(void) { return s_feed_reject; }
gw_sched_t *gw_rtos_sched(void)      { return &s_sched; }
