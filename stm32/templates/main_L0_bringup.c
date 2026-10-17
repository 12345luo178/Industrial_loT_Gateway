/**
 * @file    main_L0_bringup.c
 * @brief   ★ L0 上板 bring-up 用的 main.c —— 最小、可诊断、先跑通再扩展
 *
 * ============================================================================
 *  ⚠️ 这是模板，不参与本仓库构建（`_template`/非 `.c` 命名约定）
 *
 *  用法：把本文件的**全部内容**覆盖到
 *      D:\stm32cubeIDE\gateway2\Core\Src\main.c
 *  但**保留你自己的 `SystemClock_Config()` 和 `Error_Handler()`**
 *  （那两个函数由 CubeMX 生成，内容与本模板不同，本文件里已用
 *    `extern` 声明，不要复制我这里的占位实现）。
 *
 *  ----------------------------------------------------------------------------
 *  ⚠️ 为什么单独做一个"L0 版"而不是直接用 main_template.c
 *
 *  `main_template.c` 一次接入**全部**功能（RS485 + CAN + DHT22 + 上云 +
 *  OTA + 调度器 + 参数 Flash）。第一次上板就用它，出问题时**变量太多**：
 *  串口没输出、时钟对不对、DHT22 时序、CAN 位时序、Flash 能不能写 ——
 *  全都搅在一起，只能靠猜。
 *
 *  本文件的策略是**分层验证**：
 *
 *      第 1 层（本文件）：时钟 + DWT + 日志 + 自检报告
 *                        —— 只要这几项过了，说明"最底层是好的"
 *      第 2 层（加 RS485）：Modbus 从站能应答
 *      第 3 层：加 DHT22 / CAN / 上云 / OTA
 *
 *  所以本文件**故意不接** CAN、DHT22、上云、OTA、调度器。
 *  它们各自的移植层代码仍然被编译（所以能确认"编译得过"），
 *  只是**主循环里不调用** —— 这样第一层的问题边界非常清楚。
 *
 *  ----------------------------------------------------------------------------
 *  预期串口输出（USART1，115200 8N1）
 *
 *      [BOOT] main_L0_bringup
 *      [INFO][0][FAULT:...] ...          启动原因
 *      ==== 上板自检报告 ====
 *      [PASS] 时钟  ...                  ← 最关键：SYSCLK 到底是不是 72MHz
 *      [PASS] GPIO
 *      [PASS] DWT   ...                  ← 微秒计数是否可信
 *      ...
 *      [INFO][..][RS485:..] ...
 *
 *  ⚠️ **先把这份报告贴出来**，再往下做第 2 层。
 *     报告里第一个 FAIL 的"先查什么"就是下一步。
 * ============================================================================
 */

/* ============================================================================
 *  ==== 复制开始 ====
 * ============================================================================ */

/* ---- 按依赖顺序包含 ------------------------------------------------------
 *
 * ⚠️⚠️ **第一行必须是 `main.h`**
 *
 *     `gw_config.h` 内部有一个包含顺序检查（`#error`），因为它的宏用到了
 *     HAL 的 `GPIO_PIN_x` 这类定义。没先包含 HAL 头就直接包含它，
 *     会得到一堆 "GPIO_PIN_1 undeclared" —— 那些报错**指错了方向**，
 *     看起来像引脚配错，实际只是包含顺序问题。
 *
 *     `main.h` 是 CubeMX 生成的，它内部会包含 `stm32f1xx_hal.h`。
 *     所以顺序是：main.h -> gw_config.h -> 其余。
 *
 *     这个顺序问题在实测中踩到过，所以这里写死了 `#error` 而不是
 *     静默失败。不要调整下面几行的相对顺序。
 * -------------------------------------------------------------------------- */
#include "main.h"

#include "gw_config.h"

#include "gw_app.h"
#include "gw_log.h"
#include "gw_fault_port.h"
#include "gw_dwt.h"
#include "rs485_port.h"
#include "gw_selfcheck.h"
#include "gw_selfcheck_port.h"
#include "gw_flash_stm32.h"
#include "gw_frame.h"
/* ⚠️ OTA 的确认要用到。注意它与 gw_boot.h 是**两套**接口：
      gw_ota_port_io() 有元数据读写（确认要用它）
      gw_boot_port_io() 只有 read（Bootloader 专用） */
#include "gw_ota.h"
#include "gw_ota_port.h"
/* 片内耗时测量：PC 端量到的延迟被 USB 驱动污染，
   只有片内 DWT 测出来的才能写进报告 */
#include "modbus_master.h"   /* 主站：轮询项登记与时序 */
#include "gw_timing.h"
#include "gw_timing_port.h"
#include "dht22_port_stm32.h"   /* DHT22 硬件层（本层只做 init，供自检真读一次） */
#include "gw_can_port.h"        /* CAN 硬件层（本层只做 init，不接应用层） */

#include <stdio.h>
#include <string.h>

/* ---- CubeMX 生成的全局句柄（extern 声明，不重复定义）------------------- */
extern UART_HandleTypeDef huart1;       /* 调试/日志口 */
extern UART_HandleTypeDef huart2;       /* RS485 */
extern UART_HandleTypeDef huart3;       /* ESP32 */
extern CAN_HandleTypeDef  hcan;
extern DMA_HandleTypeDef  hdma_usart2_rx;
extern DMA_HandleTypeDef  hdma_usart3_rx;

/* ---- CubeMX 生成的初始化函数（extern，不重复定义）---------------------- */
extern void SystemClock_Config(void);
extern void MX_GPIO_Init(void);
extern void MX_DMA_Init(void);
extern void MX_USART1_UART_Init(void);
extern void MX_USART2_UART_Init(void);
extern void MX_USART3_UART_Init(void);
extern void MX_CAN_Init(void);
extern void MX_IWDG_Init(void);

/* ---- 全部放静态区 --------------------------------------------------------
 *
 * ⚠️ 不能放栈上：`gw_app_t` 约 15.8 KB，stm32f1xx 默认栈只有 1 KB。
 *    `gw_selfcheck_t` 的报告缓冲也有 2 KB。
 * -------------------------------------------------------------------------- */
/* 前向声明：run_selfcheck 定义在下方，但 main 里要用 */
static int run_selfcheck(void);

static mb_regs_t  g_regs;
static mb_slave_t g_slave;
static gw_app_t   g_app;
static gw_log_t   g_log;
static gw_port_t  g_port;

/* 片内耗时统计（纯数据结构，实例放在这里，指针交给 gw_app） */
static gw_timing_t g_timing;

static gw_boot_cause_t g_boot_cause;

/* 主站轮询的从机地址。
 *
 * ⚠️ 它**必须不等于本机从站地址**（GW_RS485_DEFAULT_ADDR = 1）。
 *
 * 踩过的坑（上板验证时真栽了）：
 *    原来这里写的是 1，而 PC 侧的模拟从机也用 1，于是同一个地址
 *    在总线上出现两次 —— 上位机发给**本机从站**的读请求，会先被
 *    应用层的帧归属派发喂给 mb_master_on_response()；因为站号对得上、
 *    CRC 也对，它继续校验到"字节数"才发现不对，于是判为一次
 *    **下游从机响应错误**（slave_stat_note_fail + advance）。
 *    现象是：上位机每读一次，主站就误记一次从机失败、跳过一个轮询项，
 *    在线位图永远是 0 —— 看起来像"主站根本没工作"。
 *
 *    所以：本机当从站用 1，被轮询的模拟从机用 2。
 *    PC 侧对应命令：pc_slave_sim.py COM8 --addr 2
 *
 * ⚠️ 登记入口 gw_app_master_add_item() 会显式拦下这种同地址配置，
 *    返回 GW_APP_ITEM_ERR_LOCAL（见下面的启动代码）。 */
#define MB_L0_MASTER_SLAVE_ADDR   2u

/* ==================================================================
 *  CAN 内部回环自检的周期与测试帧 ID（**诊断用**）
 *
 *  为什么要有它：CAN 与 RS485 不同 —— 发送方必须收到 ACK，所以
 *  "单节点 + 没有第二个节点"时无法验证任何东西。而回环模式让控制器
 *  自己发、自己收，于是**不需要第二个节点**就能把整条软件链验一遍：
 *      控制器 + 位时序 + 滤波器 + 发送通路
 *        -> 接收中断 -> 环形缓冲 -> can_port_poll -> 核心统计 -> 应用层
 *
 *  ⚠️ 验证方式：读 input[17]/[18]（CAN 收帧计数）应当随周期稳定增长；
 *     ID 0x123 还配了"->通道 1"的映射，所以 input[2] 会出现 0xA6A5
 *     （数据 = A5 A6 A7 A8 ...，小端：低字节在前）。
 *  ⚠️ 回环不经过引脚 —— 它证明不了收发器、CAN_H/L 接线、120Ω 终端。
 *     那三项必须用第二个节点（PCAN）验，见 tools/verify_can_dual.py。
 *  ⚠️ **量产时把周期设成 0**（它每轮会把控制器切到回环再切回来，
 *     切换窗口内到达的外部报文会丢）。
 * ================================================================== */
#define GW_L0_CAN_LOOPBACK_PERIOD_S   5u      /* 0 = 关闭 */
#define GW_L0_CAN_LOOPBACK_ID         0x123u

/* ==================================================================
 *  IWDG（独立看门狗）开关与喂狗开关 —— **两个都要能单独关**，理由不同
 *
 *  GW_L0_IWDG_ENABLE
 *      1 = 初始化 IWDG（生产配置）
 *      0 = 完全不启用（用于对比"没狗时是什么行为"）
 *
 *  GW_L0_IWDG_FEED
 *      1 = 正常喂狗（生产配置）
 *      0 = **故意不喂**：这是**故障注入**，用来验证两件事
 *           ① 狗真的会咬人：设备应当每 ~1.6 秒复位一次
 *           ② 复位原因能被识别：DEV_STATUS 的 IWDG_RESET 位（input[10] bit6）
 *              应当被置起来（靠 RCC_CSR 的硬件标志，不是 .noinit 记录 ——
 *              卡死时根本没机会写记录）
 *      注入验证完必须改回 1。
 * ================================================================== */
#define GW_L0_IWDG_ENABLE             1
#define GW_L0_IWDG_FEED               1

/* ==================================================================
 *  DWT 时间源的适配器
 *
 * ⚠️ 为什么需要这一层包装：
 *     `gw_dwt_us()` 的签名是 `uint32_t gw_dwt_us(void)`
 *     而 `gw_timing_init()` 要的是 `uint32_t (*)(void *ctx)`
 *     —— 多一个 ctx 参数，**不能直接传**。
 *
 *     直接把函数指针强转是能编过，但那是未定义行为：
 *     调用方会按"有一个参数"的约定压栈/传寄存器，
 *     被调用方却按"没有参数"实现。ARM 上恰好不出错，
 *     换个编译器或优化级别就可能出问题。
 *
 *     所以这里用一个真正的适配函数，不做类型双关。
 * ================================================================== */
static uint32_t timing_now_us(void *ctx)
{
    (void)ctx;                          /* DWT 不需要上下文 */
    return gw_dwt_us();
}

/* ==================================================================
 *  日志的串口输出通道
 *
 *  ⚠️ `gw_log_io_t` 有**两个**输出回调，别填错：
 *      sink       (void *ctx, const gw_log_rec_t *rec)      结构化记录
 *      sink_text  (..., 等级, 模块, 行号, 参数1, 参数2)       格式化文本
 *
 *  串口上要看的是**文本**，所以填 `sink_text`。
 *  填成 `sink` 不会报错（都是函数指针），但签名不兼容 ——
 *  编译器会给一个 incompatible-pointer-types 警告，
 *  运行期则是未定义行为（参数对不上，打出来是乱码）。
 *  实测中踩到过，所以这里写清楚。
 *
 *  ⚠️ 超时要短（20ms）。日志不该成为阻塞源 ——
 *     没接串口时 HAL_UART_Transmit 会一直等到超时。
 * ================================================================== */
static void log_sink_text_uart(void *ctx, gw_log_level_t lv,
                               gw_log_module_t mod, uint16_t line,
                               uint32_t a0, uint32_t a1)
{
    UART_HandleTypeDef *hu = (UART_HandleTypeDef *)ctx;
    char buf[80];
    int  n;

    /* 格式：[等级][运行秒数][模块:行号] 参数1 参数2
       **行号是关键** —— 没有行号的日志在现场基本没用。 */
    n = snprintf(buf, sizeof(buf), "[%s][%lu][%s:%u] %lu %lu\r\n",
                 gw_log_level_str(lv),
                 (unsigned long)gw_fault_port_uptime_s(),
                 gw_log_module_str(mod),
                 (unsigned)line,
                 (unsigned long)a0, (unsigned long)a1);

    if (n > 0) {
        (void)HAL_UART_Transmit(hu, (uint8_t *)buf, (uint16_t)n, 20u);
    }
}

/** 时间戳来源：直接用 HAL 的毫秒滴答 */
static uint32_t log_now_ms(void *ctx)
{
    (void)ctx;
    return HAL_GetTick();
}

/* ==================================================================
 *  ⚠⚠ 日志输出：两种用法必须分开
 *
 *      (a) 周期输出新日志  → **只调 flush()，不要 rewind**
 *      (b) 崩溃后 dump 全部历史  → rewind() 然后 flush()
 *
 *  ----------------------------------------------------------------------------
 *  ⚠️ 这里曾经写错，而且是**上板时才暴露的**
 *
 *  原来写的是：
 *
 *      static void flush_log(void) {
 *          gw_log_rewind(&g_log);       // 把游标拨回最旧
 *          (void)gw_log_flush(&g_log);  // 从游标读到末尾
 *      }
 *
 *  而主循环**每轮都调**它，于是：
 *      整个环形缓冲（32 条）被完整重打一遍 → 串口刷屏。
 *      现象看起来像“日志一直在发”，
 *      实际是同一批记录在循环。
 *
 *  ⚠⚠ 为什么 PC 上 2228 项测试没抓到：
 *      test_log.c 里 flush 只出现 2 次，
 *      而**每次 flush 前都有 rewind** ——
 *      “不 rewind 直接 flush 两次”这个场景从没被测过，
 *      而缺陷恰好只在那个场景下出现。
 *      已补 test_flush_no_duplicate（8 项）回归。
 * ================================================================== */
static void flush_log(void)
{
    /* ⚠️ 不要在这里加 gw_log_rewind()！
     *    它会让每一轮都重打整个环形缓冲。
     *    需要崩溃后 dump 时，另写一个函数显式调 rewind。 */
    (void)gw_log_flush(&g_log);
}

/**
 * 崩溃后 dump 全部日志历史（最多 GW_LOG_RING_DEPTH 条）
 *
 * ⚠️ 只应在“发现上次是故障启动”时调**一次**，
 *    不能放进主循环 —— 那就是上面说的刷屏。
 */
static void dump_log_history(void)
{
    gw_log_rewind(&g_log);
    (void)gw_log_flush(&g_log);
}

/* ==================================================================
 *  gw_port_t 的回调 —— 应用层与移植层的粘合层
 *
 *  ⚠️ 为什么包一层而不直接把 rs485_port_* 填进 vtable：
 *     签名不一致（port 层没有 ctx 参数）。强转函数指针是未定义行为。
 * ================================================================== */
static int cb_take_frame(void *ctx, uint8_t *buf, uint16_t cap, uint16_t *len)
{
    (void)ctx;
    return (rs485_port_take_frame(buf, cap, len) == RS485_OK) ? 0 : -1;
}

/**
 * DHT22 采样回调
 *
 * ⚠️ 这里必须走 dht22_stm32_hw() 判一次"硬件层是否可用"：
 *    应用层只在回调非空时才会周期采样（见 gw_app.c 的采样任务），
 *    DWT 不可用 / init 失败时回调留空，状态位就会如实显示"没数据"，
 *    而不是"每次采样都失败但没人知道为什么"。 */
static dht22_err_t cb_dht_sample(void *ctx, dht22_filtered_t *f, uint8_t retry)
{
    (void)ctx;
    if (dht22_stm32_hw() == 0) {
        return DHT22_ERR_PARAM;         /* 硬件层没起来 */
    }
    return dht22_stm32_sample(f, retry);
}

/* ==================================================================
 *  CAN 通路（2026-10-10 接上）
 *
 * ⚠️ 之前 L0 只调了 can_port_init()，**没有填这两个回调** ——
 *    应用层因此拿不到 CAN 核心（app->can_core == NULL），
 *    结果整条 CAN 通路是惰性的：
 *      端口中断把帧收进环形缓冲 -> 没人搬 -> 核心统计永远是 0
 *      -> 寄存器 input[17]/[18]（CAN 收帧计数）永远 0
 *      -> DEV_STATUS 的 CAN_OK 位也永远是 0
 *    "初始化成功" 与 "真的在收帧" 是两件事，这里就是那段落差。
 *
 * ⚠️ 本项目 CAN **只收不发**，所以把一个节点接上总线也不会进 Bus-Off
 *    （它从不主动发数据帧）。
 * ================================================================== */
static void cb_can_tick(void *ctx, uint32_t elapsed_ms)
{
    (void)ctx;
    /* 取报文交给核心 + 查询错误状态 + 跑 Bus-Off 恢复状态机 */
    (void)can_port_poll(elapsed_ms);
}

static gw_can_core_t *cb_can_get_core(void *ctx)
{
    (void)ctx;
    return can_port_core();
}

static int cb_send(void *ctx, const uint8_t *buf, uint16_t len)
{
    (void)ctx;
    return (rs485_port_send(buf, len) == RS485_OK) ? 0 : -1;
}

static int cb_bus_acquire(void *ctx)
{
    (void)ctx;
    /* ⚠️⚠️ 这里**曾经写的是 RS485_ROLE_SLAVE** —— 上板"主站一帧都发不出去"
       的真正原因，记在这里避免重犯：

       这个回调是给**主站**路径用的（见 app/gw_app.h 里 vtable 的注释：
       "以主站身份申请总线发送权（只决策，不发送）"）。
       配成从站之后，主站路径就变成了"申请两次"：

           第 1 次 acquire(SLAVE) -> 成功，总线 owner = SLAVE
           第 2 次 发送时内部再 acquire(MASTER)
                   -> 仲裁规定主站只有 owner == NONE 才拿得到 -> BUSY
           收尾   release_master() -> owner 不是 MASTER -> 不释放

       结果就是 owner **永远停在 SLAVE**：主站 46873 次重试全部失败
       （s_tx_master 一直是 0），而且从站自己发响应也要 acquire(SLAVE)
       —— owner 已经是 SLAVE，同样 BUSY，于是**启用主站后从站也不再应答**。
       主循环每轮都在重试，CPU 被拉到 1000‰。

       ⚠️ 现在改成调用不带角色参数的 rs485_bus_acquire_master()：
          角色由移植层的接口名固定，**调用点没有选错角色的机会**。 */
    return rs485_bus_acquire_master();
}

static int cb_send_held(void *ctx, const uint8_t *buf, uint16_t len)
{
    (void)ctx;
    /* ⚠️ 必须是"只发送、不再申请"的版本。
       原先用的是 rs485_port_send_as(RS485_ROLE_MASTER, ...)，
       那个函数内部会**再申请一次**总线 —— 与上面的 cb_bus_acquire
       叠加起来就是"申请两次"，第二次必然 BUSY。 */
    return (rs485_port_send_master_held(buf, len) == RS485_OK) ? 0 : -1;
}

static void cb_enter_wait(void *ctx) { (void)ctx; rs485_bus_enter_wait(); }
static void cb_exit_wait(void *ctx)  { (void)ctx; rs485_bus_exit_wait(); }

static int cb_release_master(void *ctx)
{
    (void)ctx;
    return rs485_bus_release_master();
}

static int cb_in_wait(void *ctx)
{
    (void)ctx;
    return rs485_bus_in_wait();
}

static int cb_holds_bus(void *ctx)
{
    (void)ctx;
    return rs485_master_holds_bus();
}

static mb_regs_t *cb_get_regs(void *ctx)
{
    (void)ctx;

    /* ⚠️ 这里**只取指针**，不做任何刷新。
     *
     * 踩过的坑：我原来把"刷新诊断/耗时统计"写在这里，以为它每次
     * Modbus 读寄存器都会被调到。实际上
     *     app/gw_app.c:185   app->regs = port->get_regs(port->ctx);
     * **只在 gw_app_init() 时调一次**，之后再也不调。
     * 于是那些字段只被写过一次（启动自检那一刻），
     * Modbus 读到的永远是**启动时的陈旧快照**。
     *
     * 上板表现：诊断显示"模块未绑定"，而用 ST-Link 直接读内存
     * 却发现 s_tm / 魔数都好好的 —— 就是这个原因。
     *
     * 正确的刷新点是 refresh_regs（每轮从站任务开头调用）。
     */
    return &g_regs;
}

/**
 * 刷新寄存器表里的"实时字段"
 *
 * ⚠️ 在每轮从站任务开头被调用（见 app/gw_app.c 的 slave 任务）。
 *    这才是"每次读都该是最新值"的字段该待的地方。
 */
static void cb_refresh_regs(void *ctx)
{
    (void)ctx;

    /* RS485 接收路径诊断（输入寄存器 28~31） */
    rs485_port_fill_diag(g_regs.input, (uint16_t)MB_INPUT_REG_COUNT);

    /* 片内耗时测量（输入寄存器 36~39） */
    (void)gw_timing_port_fill(g_regs.input, (uint16_t)MB_INPUT_REG_COUNT);

    /* ⚠️ 2026-10-11：输入寄存器 32~35 上的"主站内部状态"临时诊断**已清掉**
     *
     *    它当初是为了定位"主站对外表现为错误码 0、在线位图 0、且从不发请求"
     *    这个推不出来的组合（[32] 轮询项数 / [33] awaiting / [34] elapsed_ms
     *    / [35] cnt_requests）。主站链路现在已验穿（缺陷 C 已修，
     *    `s_tx_master` 持续增长、在线位图正常），所以按原计划收回。
     *
     *    **32~35 恢复为留空区**。⚠️ 谁将来要再借用这块，请在这里写清
     *    借用范围 + 判读方法 + **归还条件** —— 这块的"留空"本身是对外契约。
     *    （对照：28~31 的 RS485 诊断是**在 0~31 契约之内**、且 RS485 还有
     *      未闭环的问题，所以它留下了并已写进 README 寄存器表。） */
}

/* ==================================================================
 *  自检
 *
 *  ⚠️ 自检**会真的动硬件**：
 *     - FLASH 项会擦写参数区最旧的槽（不破坏有效参数，消耗 1 次寿命）
 *     - RS485 项会申请/释放总线
 *
 *     如果这些你不希望第一次就跑，可在 gw_selfcheck_port_init(&sc)
 *     之后调用 gw_selfcheck_port_set_flash(0) —— 那会跳过 Flash 项。
 * ================================================================== */
static int run_selfcheck(void)
{
    static gw_selfcheck_t sc;
    static char           buf[2048];

    if (gw_selfcheck_port_init(&sc) != 0) {
        return 0;
    }

    (void)gw_selfcheck_run(&sc);
    (void)gw_selfcheck_report(&sc, buf, sizeof(buf));

    /* 报告直接打串口 —— 不经过日志模块。
       因为自检就是为了确认"串口/时钟这些基础项到底通不通"，
       如果它自己依赖日志模块，日志模块坏了就什么都看不到。
       ⚠️ 超时从 2000ms 收紧到 500ms：2KB @115200 只要约 174ms，
          而 IWDG 最短 1.07s —— 一个坏串口不该让看门狗误判"程序卡死"。 */
    (void)HAL_UART_Transmit(&huart1, (uint8_t *)buf,
                            (uint16_t)strlen(buf), 500u);

    /* 自检整体（含上面的串口输出）之后喂一次狗，
       让"自检 -> 主循环"这段衔接也留出余量 */
    gw_fault_port_iwdg_feed();

    return gw_selfcheck_foundation_ok(&sc);
}

/* ==================================================================
 *  main
 * ================================================================== */
int main(void)
{
    /* ---------------- 1) CubeMX 生成的外设初始化 ----------------
     *
     * ⚠️ 顺序与 CubeMX 默认一致，不要改动。
     * ⚠️ `MX_SPI1_Init()` 本层用不到（本次不写 LCD UI），
     *    但你的工程里有它 —— 保留，不影响。
     */
    /* ==================================================================
     *  ⚠️⚠️ 第一件事：**显式开全局中断**（被引导程序跳转过来时必须）
     *
     *  2026-10-10 做 OTA 槽位化时踩到，现象极具迷惑性：
     *      引导程序确实把控制权交过来了（SCB->VTOR 已是 0x08009000、无故障标志），
     *      但应用"活着却不干活"：
     *          TIM4 在跑（CR1.CEN=1）、中断使能了（DIER.UIE=1）、
     *          NVIC 也使能了（ISER0 的 bit30）、SR.UIF 挂着、ISPR0 也 pending，
     *          可 **ISR 从来没进过** —— 于是 uwTick 恒为 0，
     *          HAL_Delay 及所有带超时的 HAL 调用直接挂死。
     *
     *  原因：**复位会把 PRIMASK 清零（中断默认开），而"跳转"不会** ——
     *      引导程序为防止自己的 ISR 在跳转瞬间乱跑，跳之前 __disable_irq()，
     *      这个状态被应用完整继承了。CubeMX 生成的应用从不重新打开它，
     *      因为它默认自己总是从复位进来的。
     *
     *  ⚠️ 这一行在"直接复位启动"（单镜像烧在 0x08000000）时是**无害的空操作**
     *      —— 那时 PRIMASK 本来就是 0。所以两种启动方式都可以留着它。
     * ================================================================== */
    __enable_irq();

    HAL_Init();
    SystemClock_Config();           /* ⚠️ 必须在 gw_dwt_init 之前 */

    MX_GPIO_Init();
    MX_DMA_Init();
    MX_USART1_UART_Init();
    MX_USART2_UART_Init();
    MX_USART3_UART_Init();
    MX_CAN_Init();

    /* ==================================================================
     *  IWDG（独立看门狗）—— 2026-10-10 启用
     *
     * ⚠️ 为什么它和 OTA 回滚是同一件事：
     *    A/B 升级的"试用 + 回滚"依赖**坏固件真的把设备复位**。
     *    HardFault 有故障处理里 NVIC_SystemReset() 兜着 ✓，
     *    但**静默挂死**（死循环/死锁/阻塞）没有任何异常 -> 永远不复位
     *    -> boot_tries 不前进 -> 永远不回滚 -> 现场设备变砖。
     *    IWDG 由独立 LSI 时钟驱动，与主时钟、中断都无关，
     *    是唯一能覆盖这一类的机制。
     *
     * ⚠️ 一旦启动无法用软件关闭，所以从这里起：
     *      主循环每轮喂狗 + 长阻塞路径自己喂（Flash 擦除、DHT 自检等待）。
     *    GW_L0_IWDG_FEED=0 是**故意不喂**的注入开关：用来验证
     *    "狗真的会咬人、且复位原因能被识别"（见 selfcheck 的 IWDG 项）。 */
#if (GW_L0_IWDG_ENABLE)
    MX_IWDG_Init();
#endif

    /* ---- 先打一行"我起来了"，确认串口通路 ----
       这一步在接任何模块之前，所以它失败一定是串口/时钟问题。 */
    {
        static const char hello[] = "\r\n[BOOT] main_L0_bringup\r\n";
        (void)HAL_UART_Transmit(&huart1, (uint8_t *)hello,
                                (uint16_t)(sizeof(hello) - 1u), 500u);
    }

    /* ---------------- 2) 故障模块（**最先**） ----------------
     *
     * 必须在任何可能出错的事之前：它要读 `.noinit` 判定
     * "上次是怎么启动的"，晚了就被覆盖了。
     */
    memset(&g_regs, 0, sizeof(g_regs));
    g_boot_cause = gw_fault_port_init();

    /* ---------------- 3) DWT 微秒计时 ----------------
     *
     * ⚠️ 必须在 SystemClock_Config() 之后。
     *    DWT 的微秒换算依赖 SystemCoreClock，
     *    如果它还是 HSI 的 8MHz，微秒会偏 8 倍 —— DHT22 必然读不出。
     */
    gw_dwt_init();

    /* ---------------- 4) 日志 ---------------- */
    {
        gw_log_io_t io;
        memset(&io, 0, sizeof(io));
        io.now_ms    = log_now_ms;
        io.sink_text = log_sink_text_uart;   /* 注意不是 sink，见上方说明 */
        io.ctx       = &huart1;

        (void)gw_log_init(&g_log, &io, GW_LOG_INFO);
    }

    /* ---------------- 5) 从站 + 寄存器表 ---------------- */
    mb_slave_init(&g_slave, GW_RS485_DEFAULT_ADDR, &g_regs);

    /* ---------------- 6) RS485 移植层 ----------------
     *
     * ⚠️ 必须在 MX_USART2_UART_Init() 之后 ——
     *    它要挂 DMA 与 IDLE 中断。
     */
    if (rs485_port_init(GW_RS485_DEFAULT_ADDR) != RS485_OK) {
        GW_LOG_ERR(&g_log, GW_LOG_MOD_RS485, 1u, 0u);
    }

    /* ---------------- 7) 应用层 ----------------
     *
     * ⚠️ 本层只填 RS485 相关的回调。DHT22 / 上云 / CAN / OTA
     *    留空 —— 应用层会**优雅降级**（相关寄存器报"不可用"而不是崩）。
     *    这正是"不做假数据"的设计：没接的功能如实报告。
     */
    memset(&g_port, 0, sizeof(g_port));
    g_port.rs485_take_frame       = cb_take_frame;
    g_port.rs485_send             = cb_send;
    g_port.rs485_bus_acquire      = cb_bus_acquire;
    g_port.rs485_send_held        = cb_send_held;
    g_port.rs485_enter_wait       = cb_enter_wait;
    g_port.rs485_exit_wait        = cb_exit_wait;
    g_port.rs485_release_master   = cb_release_master;
    g_port.rs485_in_wait          = cb_in_wait;
    g_port.rs485_master_holds_bus = cb_holds_bus;
    g_port.get_regs               = cb_get_regs;
    /* ⚠️ 刷新实时字段（诊断/耗时统计）。必须设这个 ——
       只在 get_regs 里刷新是错的：那个回调只在 gw_app_init 时调一次。 */
    g_port.refresh_regs           = cb_refresh_regs;

    /* ✅ DHT22 采样：接上之后温湿度寄存器（输入 0/1）才是真数据。
     *
     * ⚠️ 这里**无条件赋值**，而不是先判 dht22_stm32_hw()：
     *     DHT22 硬件层是在下面 7.8) 才 init 的（顺序：vtable -> gw_app_init
     *     -> ... -> 7.8 DHT22/CAN init -> 8) 自检），此刻还不可用。
     *     判空放在回调内部 —— 应用层只看"回调是否为空"来决定要不要周期采样，
     *     硬件层没起来时回调返回 PARAM，状态位就会如实显示"没数据"。 */
    g_port.dht_sample             = cb_dht_sample;

    /* param_save / param_restore / reboot 本层故意留空
       （L0 不碰 Flash 参数与复位；要这些能力见 main_template.c） */

    if (gw_app_init(&g_app, &g_slave, &g_port) != 0) {
        GW_LOG_ERR(&g_log, GW_LOG_MOD_APP, 1u, 0u);
        for (;;) { }
    }

    /* ---- 把"本次启动原因"记进状态位（sticky）----
     * ⚠️ 这两个位在寄存器表里定义了、注释写着"sticky，仅复位清除"，
     *    但**以前从来没人设置过** —— 上位机永远看不到
     *    "这台设备刚被看门狗/异常复位过"这个最关键的现场信息。
     *    接口承诺了却没有实现，属于静默失效。 */
    gw_app_note_boot_cause(&g_app, g_boot_cause);

    /* ==================================================================
     *  ⚠️ 启用 Modbus **主站**功能（主从共存）
     *
     *  这是本项目的核心差异化：同一块板子既当从站（应答上位机），
     *  又当主站（主动轮询下游仪表）。
     *
     *  ------------------------------------------------------------------
     *  ⚠️ 关键：主站模块**已经在 gw_app_init() 里初始化好了**，
     *     而且 gw_app_task_entry_master() **每轮都在跑 mb_master_poll()**。
     *     但**一个轮询项都没有登记** —— 所以它一直在空转。
     *
     *     这也是为什么"主站从没上板验证过却也不报错"：
     *     它只是安静地什么都不做。**没有错误不等于在工作。**
     *
     *  ------------------------------------------------------------------
     *  轮询项的含义（mb_m_poll_item_t）：
     *      slave     从机地址（1~247）
     *      func      0x03 读保持 / 0x04 读输入
     *      start     从机侧起始寄存器地址
     *      qty       读取数量（1~125）
     *      map_base  读回的数据写到**本地寄存器表**的哪个偏移
     *      map_area  0 = 输入寄存器区，1 = 保持寄存器区
     *
     *  ⚠️ map_base 的选择有讲究：
     *     本地输入寄存器 0~31 是"本机对外发布的实时数据"，
     *     如果让主站往那里写，会**覆盖自己的采集结果**。
     *     所以这里映射到**保持寄存器 8~11** —— 那几个位置是
     *     "主站设置区"（MASTER_TIMEOUT/RETRY 等）之外的空位，
     *     且保持寄存器本来就是可读写的，语义上合适。
     *
     *  ⚠️ 验证方法：tools/verify_master_dual.py COM8 --base 1000
     *     （一个程序同时扮两个角色：应答本机主站的轮询 + 读本机从站）
     *
     *     ⚠️ 不要"开一个 pc_slave_sim.py 再开一个读取脚本"：
     *        Windows 串口是独占的，两个程序打不开同一个 COM 口；
     *        verify_master_dual.py 已经把从机角色内建进去了。
     *
     *     ⚠️ 被轮询的模拟从机地址必须是 **2**（--sim-addr 2 / --addr 2），
     *        不能是 1 —— 理由见 MB_L0_MASTER_SLAVE_ADDR 那里的说明。
     * ================================================================== */
    {
        mb_master_t *mst = gw_app_master(&g_app);
        mb_m_poll_item_t it;
        int              e;

        if (mst != 0) {
            /* 超时 300ms、重试 1 次 —— 现场仪表的典型响应 <100ms。
               gw_app_init 里已设过默认值，这里显式再设一次，
               让"本层用了什么时序"在代码里可见。 */
            (void)mb_master_set_timing(mst, 300u, 1u);

            /* 轮询节拍：两次请求之间至少空出 1 秒。
             *
             * ⚠️ 这一行不是"调优"，是**必需**的 —— 2026-10-10 上板实测：
             *    不设节拍时，从机应答够快（PC 模拟从机 ~2ms）会让主站
             *    **全速轮询**：实测 177 次/秒、CPU 负载 1000‰、单轮耗时打满。
             *    那不是性能好，而是把 RS485 总线长期占着（同总线上别的
             *    节点没机会说话），现场仪表也被上百次/秒地无意义访问。
             *
             *    一个真实网关的轮询周期是**秒级**。要更快/更慢改这个数即可
             *    （0 = 不限制，恢复一问一答连发；最大 60000ms）。 */
            (void)mb_master_set_interval(mst, 1000u);

            memset(&it, 0, sizeof(it));
            it.slave    = MB_L0_MASTER_SLAVE_ADDR;
            it.func     = MB_FC_READ_INPUT;
            it.start    = 0u;
            it.qty      = 4u;
            it.map_base = 8u;          /* 本地保持寄存器 8~11 */
            it.map_area = 1u;          /* 1 = 保持寄存器区 */

            /* ⚠️ 走 gw_app_master_add_item() 而不是 mb_master_add_item()：
               前者会拦下"轮询地址 = 本机从站地址"这种**静默失效**的配置
               （详见 app/gw_app.h 里那段说明）。 */
            e = gw_app_master_add_item(&g_app, &it);

            if (e == GW_APP_ITEM_ERR_LOCAL) {
                /* 这个分支必须**单独报**，而且给一个专门的日志码：
                   它的现象（主站安静地不工作）与"主站没启用"一模一样，
                   混在通用错误码里事后根本查不出来。 */
                GW_LOG_ERR(&g_log, GW_LOG_MOD_APP, 4u,
                           (uint32_t)MB_L0_MASTER_SLAVE_ADDR);
            } else if (e != GW_APP_ITEM_OK) {
                /* ⚠️ 登记失败要**明确报出来**，不能静默 ——
                   否则现象是"主站一直不工作"，而原因在启动时就发生了。 */
                GW_LOG_ERR(&g_log, GW_LOG_MOD_APP, 2u, (uint32_t)e);
            } else {
                GW_LOG_INF(&g_log, GW_LOG_MOD_APP, 3u,
                           (uint32_t)MB_L0_MASTER_SLAVE_ADDR);
            }
        }
    }

    /* ---------------- 7.5) 接上片内耗时测量 ----------------
     *
     * ⚠️ 为什么必须做这一步（而不只是"顺便加个功能"）：
     *
     *    PC 端量到的往返延迟中位 4.46 ms，但 115200 下 9 字节响应
     *    在线上只需 0.78 ms —— 那 4.46ms 里绝大部分是
     *    **USB 串口驱动与 Python 的开销**，不是从站的响应延迟。
     *
     *    那个数字**不能**写进报告当"从站响应延迟"。
     *    要拿真值只能用片内 DWT 测，也就是下面这三步。
     *
     * ⚠️ 三步缺一不可：
     *     - 不 init  -> 时间源是 NULL，统计恒为 0
     *     - 不 enable-> 应用层不打点，统计恒为 0
     *     - 不 bind  -> 统计有值但读不到寄存器
     *    三种情况的结果都是"读出来全是 0"，
     *    ⚠️ 而那**不代表性能好，只代表没接线**。 */
    if (gw_timing_init(&g_timing, timing_now_us, 0) == 0) {
        gw_app_enable_timing(&g_app, &g_timing);
        gw_timing_port_bind(&g_timing);
    } else {
        GW_LOG_WRN(&g_log, GW_LOG_MOD_SYS, 9u, 0u);
    }

    /* ---------------- 7.8) DHT22 / CAN 硬件层初始化 ----------------
     *
     * ⚠️ 为什么这里要 init 它们（本层原来**故意不接** DHT22 / CAN）：
     *
     *   自检项表是各层**共用**的，而"没 init"会让自检报 [FAIL]：
     *       DHT22：未 init -> dht22_stm32_sample() 返回 ERR_PARAM -> [FAIL]
     *       CAN  ：未 init -> 控制器状态是 UNINIT          -> [FAIL]
     *   现象就是"日志说 DHT22/CAN 失败"，而硬件其实是好的 ——
     *   2026-10-10 实测踩过这个坑（板子上传感器是好的，报告却写失败）。
     *
     *   现在改成：**硬件层 init 起来，但不接进应用层**，于是自检能真的
     *   去读一次传感器 / 读一次控制器状态，报出来的结论才可信。
     *
     *   ⚠️ 两者的边界（为什么"只 init 不接应用层"是安全的）：
     *      DHT22：init 只配置 PG11（开漏 + 内部上拉）。
     *             1~2 秒的上电稳定期由自检项内部的重试兜住（见 gw_selfcheck_port.c）。
     *      CAN  ：init + Start 之后控制器处于 NORMAL，并已通过
     *             `g_port.can` 接进应用层（tick + get_core）——
     *             帧会被搬进核心、累加收帧计数、刷新寄存器。
     *             ⚠️ 本项目 CAN **只收不发**，所以即使总线上暂时没有
     *                第二个节点也不会进 Bus-Off（它从不主动发数据帧）。
     *
     *   想让它们真正参与业务（周期采样 / 收发），再按
     *   doc/上板集成指南.md 把回调和 vtable 接到应用层。 */
    {
        int               dht_rc = dht22_stm32_init();
        can_port_status_t can_rc = can_port_init(0u, 0u);

        if (dht_rc != 0) {
            /* init 失败只有一种可能：DWT 不可用（微秒计时不可靠，
               此时 DHT22 的位宽判定必然失准，宁可明确报出来）。 */
            GW_LOG_WRN(&g_log, GW_LOG_MOD_SYS, 10u, (uint32_t)(-dht_rc));
        }
        if (can_rc != CAN_PORT_OK) {
            GW_LOG_WRN(&g_log, GW_LOG_MOD_CAN, 0u, (uint32_t)can_rc);
        }

        /* ---- 演示/验证用的 ID -> 通道映射 ----
         * 把测试帧 ID 0x123 映射到 1 号通道（输入寄存器 2）。
         * 这样"PCAN 发一帧 0x123"既能看收帧计数（input[17]/[18]），
         * 也能在寄存器里看到**数值本身** —— 否则通道值会一直是 0，
         * 看起来像"没收到"，其实只是没配映射。
         *
         * 验证命令：python tools/verify_can_dual.py --via swd
         * （第 1 个字节是低字节，所以发 11 22 -> 寄存器值是 0x2211） */
        {
            gw_can_core_t *cc = can_port_core();

            if (cc != 0) {
                (void)gw_can_core_set_map(cc, 0x123u, 0u);
            }
        }

        /* ---- 把 CAN 接进应用层 ----
         * ⚠️ 用 gw_app_enable_can()，不是往 g_port 里塞 ——
         *    `gw_can_port_t can` 是 gw_app_t 的成员（不在 gw_port_t 里），
         *    应用层通过它拿核心指针并每个 tick 推进硬件层。
         *
         * ⚠️ 必须在 gw_app_init() **之后**调用（它检查 app->inited）。 */
        {
            gw_can_port_t can_port;

            memset(&can_port, 0, sizeof(can_port));
            can_port.tick     = cb_can_tick;
            can_port.get_core = cb_can_get_core;
            can_port.ctx      = 0;

            if (gw_app_enable_can(&g_app, &can_port) != 0) {
                GW_LOG_WRN(&g_log, GW_LOG_MOD_CAN, 1u, 0u);
            }
        }
    }

    /* ---------------- 8) 自检 + 报告 ----------------
     *
     * ⚠️ 放在这里（而不是更早）：自检要检查 RS485 总线状态，
     *    而 rs485_port_init() 刚做完。
     */
    if (run_selfcheck() == 0) {
        /* 基础项（时钟 / GPIO / DWT / Flash）没过，
           后面的验证没有意义。不要急着往下调 ——
           先看报告里第一个 FAIL 的"先查什么"。 */
        (void)HAL_UART_Transmit(&huart1,
                                (uint8_t *)"\r\n[STOP] foundation check failed\r\n",
                                34u, 500u);
        for (;;) { }
    }

    /* ---------------- 9) 报告启动原因 ----------------
     *
     * ⚠️ 这里只调 flush()（输出新日志），
     *    **绝不能改成 dump_log_history()** ——
     *    那会把整个环形缓冲重打，而且在主循环里每轮都重打。 */
    GW_LOG_WRN(&g_log, GW_LOG_MOD_FAULT,
               (uint32_t)g_boot_cause,
               (uint32_t)gw_fault_port_record()->boot_count);
    flush_log();

    

    /* ---- 如果上次是故障启动，把上次崩之前的日志历史 dump 一遍 ----
     *
     * ⚠️ 这里**才是** dump_log_history() 的正确位置：
     *    “需要看全部历史”且“只做一次”。
     *    现场调试时有它，就不用接调试器去看崩在哪一行了。 */
    if (g_boot_cause == GW_BOOT_FAULT) {
        dump_log_history();
    }

    (void)HAL_UART_Transmit(&huart1,
                            (uint8_t *)"[READY] L0 up, RS485 slave listening\r\n",
                            39u, 500u);

    /* ---------------- 10) 主循环 ----------------
     *
     * ⚠️ 用 HAL_GetTick 算**实际**间隔，而不是假设一定是 10ms。
     *    这样即使某一轮被中断拖长，上报节拍仍然准确。
     */
    {
        uint32_t last_tick    = HAL_GetTick();
        uint32_t confirm_acc  = 0u;     /* OTA 确认的 1 秒节拍 */
        uint32_t can_lb_acc   = 0u;     /* CAN 回环自检的秒计数 */

        /* ==============================================================
         *  ⚠️ 清零耗时统计 —— 这决定了读到的数字能不能写进报告
         *
         *  `gw_timing` 的统计是**从开机累起**的，所以 Max 里混着
         *  **启动阶段的离群点**（时钟配置、外设初始化、自检、Flash 擦写）。
         *
         *  实测就撞上了这个：
         *      从站处理 Max = 1275 us，而 Avg 只有 81 us
         *  那个 1275 是启动离群点，**不是"从站响应最大耗时"**。
         *  若直接写进报告，被问"那 1.2ms 是什么造成的"就答不上来。
         *
         *  所以在**进入主循环之前**清零一次。之后累起来的
         *  Max 就只包含稳态运行，那才是可以写进报告的数字。
         *
         *  ⚠️ 为什么不用"Modbus 写命令寄存器"来触发清零：
         *     查证发现 —— `HREG_PARAM_SAVE_CMD` / `HREG_PARAM_DFLT_CMD` /
         *     `HREG_REBOOT_CMD` 那几个魔数宏**定义了却没有任何处理器读它**，
         *     写它们只是把值存进寄存器，**不会触发动作**；
         *     动作走的是文本指令协议（带确认令牌）。
         *     所以加一个"统计清零"命令寄存器会是**假接口**。
         *     启动时清零更简单，也不会误导后面的人。
         *
         *  ⚠️ 含义：从 [READY] 之后累起。掉电重启会重新开始，
         *     这对"跑一段已知负载测稳态"的用法正好合适。
         * ============================================================== */
        gw_timing_reset(&g_timing);

        /* 第一轮 poll 之后再清一次。
         *
         * ⚠️ 为什么需要第二次清零 —— 实测数据：
         *    第一次清零后，Max 仍然是 1275us，而 Avg 只有 37us；
         *    连读 8 次，Max **一动不动**（1275 恒定），Avg 缓慢下降。
         *    也就是说那一次 1275us 是**一轮性的瞬态**，不是持续现象。
         *
         *    它的成因我没有查到底（可能是第一次进入从站任务时的
         *    分支预测/缓存预热，也可能是首帧的 DMA 建立开销）。
         *    ⚠️ 与其猜它是什么，不如**把它排除掉**：
         *       报告里要的是"稳态最坏耗时"，
         *       一次开机的瞬态本来就不该算进去。
         *
         * ⚠️ 这样做的代价（如实说明）：
         *    读到的 Max 是"跑起来之后"的最坏值，
         *    **不包含开机第一轮**。这是刻意的取舍，写报告时要注明。 */
        uint8_t warmed = 0u;

        for (;;) {
            uint32_t now = HAL_GetTick();
            uint32_t dt  = now - last_tick;

            /* ---- 喂狗（主循环每轮）----
             * ⚠️ 放在**最外层循环的开头**：只要主循环还在转，狗就不会咬；
             *    任何让它转不动的路径（死循环、死锁、被拖死的阻塞）
             *    都会导致喂不上 -> 1.6s 后复位 -> 引导程序计数 +1 -> 最终回滚。
             * ⚠️ GW_L0_IWDG_FEED=0 是故障注入开关（验证狗会不会咬人）。 */
#if ((GW_L0_IWDG_ENABLE) && (GW_L0_IWDG_FEED))
            gw_fault_port_iwdg_feed();
#endif

            if (dt == 0u) {
                continue;               /* 同一毫秒内重复进入：跳过 */
            }
            last_tick = now;

            gw_fault_port_tick(dt);
            gw_log_tick(&g_log, dt);
            gw_app_poll(&g_app, dt);

            /* ---- 第一轮 poll 之后清掉统计里的开机瞬态 ----
             *
             * ⚠️ 实测依据：清零后 Max 仍是 1275us 而 Avg 只有 37us，
             *    连读 8 次 Max **一动不动**、Avg 缓慢下降
             *    -> 那是一次性瞬态，不是持续现象。
             *    它不该被算进"稳态最坏耗时"。 */
            if (warmed == 0u) {
                warmed = 1u;
                gw_timing_reset(&g_timing);
            }

            /* ==========================================================
             *  ⚠️⚠️ OTA 确认：**必须有这一段，否则升级会被无故回滚**
             *
             *  它的必要性来自一个真实缺陷：
             *    `gw_ota_should_confirm()` / `gw_ota_meta_confirm()`
             *    原本**只有定义、声明和测试，生产代码里一个调用者都没有**。
             *    于是升级到 B 之后 pending 永远是 1：
             *
             *        复位 -> Bootloader 看到 pending=1 -> boot_tries++
             *             -> 累计 3 次 -> **把完全正常的新固件回滚掉**
             *
             *    也就是说：升级"成功"了，但重启几次之后版本又变回去了。
             *
             *  ----------------------------------------------------------
             *  ⚠️ 用一步式封装，不要自己拼四步：
             *     自己拼要写 load + should_confirm + meta_confirm + save，
             *     而 `save` 漏掉**不会有任何报错** —— 只是 pending 永不清。
             *     那种"漏一步就静默失效"的接口就是当初被漏掉的原因。
             *
             *  ⚠️ 每秒一次，不是每轮主循环。
             *     稳态下开销是"每秒一次 Flash 读"：
             *     读出来发现没 pending 就直接返回，不写。
             * ========================================================== */
            confirm_acc += dt;
            if (confirm_acc >= 1000u) {
                gw_ota_confirm_r_t cr;

                confirm_acc = 0u;

                /* len/crc 传 0：它们只是诊断信息（见 gw_ota.h 的说明），
                   当前没有任何代码读它们做判断。
                   传真实值需要链接期符号算运行中固件的大小与 CRC，
                   本项目尚未提供 —— 这一点写在 README 的已知限制里。 */
                cr = gw_ota_confirm_running(gw_ota_port_io(),
                                            gw_ota_running_slot(),
                                            gw_fault_port_uptime_s(),
                                            0u, 0u);
                if (cr == GW_OTA_CONFIRM_R_DONE) {
                    /* ⚠️ 只在**真的确认成功**时打一行 ——
                       每轮都打会变成噪音。 */
                    (void)HAL_UART_Transmit(
                        &huart1,
                        (uint8_t *)"[OTA] upgrade CONFIRMED\r\n", 25u, 500u);
                } else if (cr == GW_OTA_CONFIRM_R_WRONG_SLOT) {
                    /* ⚠️⚠️ 缺陷 P：有待确认的升级，但**我现在跑的不是 `active` 那个槽**
                       —— 说明引导程序是"降级启动"我的（目标槽镜像校验不过）。
                       这里**必须拒绝**：让引导程序的 boot_tries 继续数到 3、
                       按设计回滚；替它确认会把回滚计数清零、试用期保护直接废掉。
                       ⚠️ 每次启动只打一行（每秒一次会变成噪音）。 */
                    static uint8_t ws_warned = 0u;
                    if (ws_warned == 0u) {
                        ws_warned = 1u;
                        (void)HAL_UART_Transmit(
                            &huart1,
                            (uint8_t *)"[OTA] confirm refused: wrong slot\r\n",
                            35u, 500u);
                    }
                }

                /* ---------------- CAN 内部回环自检（诊断）----------------
                 * 不需要第二个节点，就能验证**整条 CAN 软件链**：
                 *   控制器 + 位时序 + 滤波器 + 发送通路
                 *     -> 接收中断 -> 环形缓冲 -> can_port_poll -> 核心统计
                 * 报文的第一个字节还会经"ID -> 通道映射"进 input[2]，
                 * 所以连应用层的搬运也一起验了。
                 *
                 * ⚠️ 回环**不经过引脚**，所以它证明不了收发器、CAN_H/L 接线
                 *    与 120Ω 终端 —— 那三项必须靠第二个节点（PCAN）。
                 * ⚠️ 这是**诊断**：量产时把周期设成 0 关掉。 */
#if (GW_L0_CAN_LOOPBACK_PERIOD_S > 0u)
                can_lb_acc++;
                if (can_lb_acc >= (uint32_t)GW_L0_CAN_LOOPBACK_PERIOD_S) {
                    can_lb_acc = 0u;
                    if (can_port_loopback_test((uint16_t)GW_L0_CAN_LOOPBACK_ID,
                                               8u) != CAN_PORT_OK) {
                        GW_LOG_WRN(&g_log, GW_LOG_MOD_CAN, 2u, 0u);
                    }
                }
#endif
            }

            flush_log();
        }
    }
}

/* ============================================================================
 *  ==== 复制结束 ====
 *
 *  ⚠️ 下面这两个函数**不要**从本文件复制 —— 用你工程里 CubeMX 生成的那份。
 *     这里只列出签名，方便你确认它们存在：
 *
 *          void SystemClock_Config(void);     由 CubeMX 生成，保留你的版本
 *          void Error_Handler(void);          由 CubeMX 生成，保留你的版本
 *
 *     本文件开头已经用 `extern` 声明了 SystemClock_Config 与外设初始化，
 *     所以覆盖 main.c 之后它们仍会解析到你工程里的那份定义。
 * ============================================================================ */
