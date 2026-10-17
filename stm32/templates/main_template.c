/**
 * @file    main_template.c
 * @brief   ★ 上板参考 main.c —— 复制到你的 CubeMX 工程里用
 *
 * ============================================================================
 *  ⚠️ 这是模板，不是编译单元
 *
 *  它**不参与本仓库的构建**（文件名是 `_template.c` 而不是 `.c`，
 *  `test/` 之外没有 Makefile 会碰它）。用法：
 *
 *    1) 在你的 CubeMX 工程里新建 `main.c`（或改现有的）
 *    2) 把下面「==== 复制开始 ====」到「==== 复制结束 ====」之间的内容
 *       搬过去，**保留你自己的 CubeMX 初始化代码**
 *    3) 按 `doc/CubeMX配置清单.md` 配好外设
 *
 *  ----------------------------------------------------------------------------
 *  ⚠️⚠️ 初始化顺序是硬要求，不是建议
 *
 *  下面每一步的顺序都有原因，**调换会导致运行时出现莫名其妙的现象**
 *  （不会编译报错）：
 *
 *  | 步骤 | 必须在这之后 | 否则会怎样 |
 *  |---|---|---|
 *  | `gw_dwt_init()` | `SystemClock_Config()` | 微秒计时偏 8 倍（按 HSI 算的），**DHT22 必然读不出** |
 *  | `gw_fault_port_init()` | 任何可能出错的事之前 | 漏掉"上次怎么崩的"，安全模式失效 |
 *  | `rs485_port_init()` | `MX_USART2_UART_Init()` | DMA/IDLE 中断没配好，收不到帧 |
 *  | `gw_param_load` | 寄存器表初始化之后 | 上位机读到默认值而不是保存的值 |
 *
 *  本模板的注释里对每一处都标了原因。
 *
 *  ----------------------------------------------------------------------------
 *  ⚠️ 三个大结构体必须是 static
 *
 *      static gw_app_t      g_app;        // 15.8 KB
 *      static gw_uplink_t   g_uplink;     // 14.5 KB
 *      static mb_slave_t    g_slave;
 *      static gw_param_t    g_param;
 *      static gw_timing_t   g_tm;
 *      static gw_sched_t    g_sched;
 *      static gw_log_t      g_log;
 *      static gw_baud_t     g_baud;
 *      static gw_ota_bridge_t g_ota;
 *
 *  放在栈上会立刻溢出（F103 只有 64 KB RAM）。
 *  **栈溢出的症状很隐蔽**：通常是 `UNDEFINSTR`（返回地址被覆盖），
 *  或者 HFSR.FORCED=1 但 CFSR 全 0。
 *
 *  ----------------------------------------------------------------------------
 *  编译依赖（把哪些文件加进工程）
 *
 *      src/     modbus_crc.c  modbus_rtu.c  modbus_master.c
 *               rs485_arbiter.c  gw_pipeline.c  gw_param.c
 *               gw_devinfo.c  gw_frame.c  gw_uplink.c
 *               gw_can_core.c  gw_log.c  gw_fault.c  gw_timing.c
 *               gw_sched.c  gw_cmd.c  gw_ota.c  gw_crc32.c
 *               gw_boot.c  gw_selfcheck.c
 *      drivers/ dht22.c
 *      app/     gw_app.c
 *      port/    gw_dwt.c  rs485_port.c  esp32_port.c  gw_can_port.c
 *               dht22_port_stm32.c  gw_flash_stm32.c  gw_fault_port.c
 *               gw_selfcheck_port.c
 *
 *  头文件搜索路径：`inc`、`drivers`、`app`、`port`
 * ============================================================================
 */

/* ==================== 复制开始 ==================== */

/* USER CODE BEGIN Includes */
#include "main.h"               /* CubeMX 生成（含各 MX_xxx_Init 声明） */

/* ⚠️⚠️ `.noinit` 段的进度码 `g_prog`（2026-10-13 补进模板）。
 *
 *  为什么模板必须**自带**这一个：`port/lcd_ili9341.c` 里有
 *      `extern volatile uint32_t g_prog;`，并且在**多个位置写它**
 *      （`wr_cmd` / `set_window` / `lcd_init` 各写一个进度码，用来分辨
 *      "屏幕初始化卡在哪一步"）。以前它只存在于工程的
 *      `Core/Src/main.c`（USER CODE BEGIN Includes 区），模板里没有 ——
 *      于是「用 CubeMX 重新生成 main.c，再套用本模板」时这个符号会跟着消失，
 *      **直到链接期**才报 undefined reference。
 *  放进模板之后它就自成闭环，不再依赖工程 main.c 恰好留着它。
 *
 *  `.noinit` 不参与启动清零 ⇒ **跨复位保留**，这正是它能当"进度码"用的原因。
 *  读法：`nm` 找符号地址 → SWD 直读；读到的值就是最后写进去的那个进度码。
 *
 *  ⚠️ 这里**不要**再定义 `s_err_from` / `s_err_tick`：那两个是
 *     `Error_Handler()` 用的，而 `Error_Handler()` 位于 CubeMX 尾巴里
 *     （`SystemClock_Config` 之后），它们已经定义在紧挨 `Error_Handler`
 *     的位置，随尾巴一起进合并结果 —— 在这里再写会**重复定义**。
 *
 *  ⚠️ 若你把本段复制进一个**已经带有**这一行的 main.c，同样会重复定义 ——
 *     两边保留一份即可（内容本就一致）。
 */
__attribute__((section(".noinit"))) volatile uint32_t g_prog;

#include "gw_config.h"          /* 引脚定义（必须先于其它 port 头） */
#include "gw_app.h"
#include "gw_devinfo.h"
#include "gw_param.h"
#include "gw_timing.h"
#include "gw_timing_port.h"     /* cb_refresh_regs 里填耗时统计用 */
#include "gw_sched.h"
#include "gw_log.h"
#include "gw_fault_port.h"
#include "gw_flash_stm32.h"
#include "gw_dwt.h"
#include "rs485_port.h"
#include "esp32_port.h"
#include "gw_can_port.h"
#include "dht22_port_stm32.h"
#include "gw_selfcheck.h"
#include "gw_baud.h"
#include "gw_baud_port.h"
#include "gw_ota_bridge.h"
#include "gw_ota_port.h"
#include "gw_selfcheck_port.h"

/* ---- LVGL 显示路径（只有 -Lvgl 构建开关打开时才编进来）----
   ⚠️ 缺了这两行会有隐式声明警告：下面 gw_rtos_ui_tick() 里要调
      lvgl_port_init() 与 gw_lvgl_ui_build()，声明分别在
      port/lvgl_port.h 与 app/gw_lvgl_ui.h。 */
#if defined(GW_USE_LVGL)
#include "lvgl_port.h"
#include "gw_lvgl_ui.h"
#endif

/* ==================================================================
 *  ★ 裸机 / FreeRTOS 两种构建由 **GW_USE_FREERTOS** 选择
 *
 *  默认（不定义）= 裸机，行为与以前完全一样。
 *  在 CubeMX 工程里启用 RTOS 时，把 `GW_USE_FREERTOS=1` 定义上
 *  （Project -> C/C++ Build -> Settings -> Preprocessor，或写进 gw_config.h）。
 *
 *  ⚠️ 两条路径**都会被 CI 编译**（"模板类型检查"里编两遍），
 *     所以不存在"某条路径长期没人编、悄悄烂掉"—— 缺陷 Q 就是这么来的。
 *  ⚠️ 落地前置条件（中断向量重定向、堆够不够、栈/优先级自洽）由
 *     `tools/check_rtos_hooks.py` 机械核算，见 doc/FreeRTOS迁移指南.md。
 * ================================================================== */
#if defined(GW_USE_FREERTOS) && (GW_USE_FREERTOS == 1)
#include "gw_rtos.h"            /* 建任务 / 使能 IWDG / 启动调度器 */
#include "gw_ui.h"              /* 本地界面（手绘 4 页，最低优先级任务）*/
#endif
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* ==================================================================
 *  GW_MEAS_SEQUENTIAL —— 测量专用开关（2026-10-17 新增）
 *
 *  **默认 0 = 量产行为**。置 1 并构建**非 RTOS** 版本，即得到
 *  "M5/M7/M8 测量专用配置"（纯顺序模式，协作式调度不启用）。
 *
 *  为什么需要它：`gw_app_poll()` 的结构是
 *      if (app->sched != 0) { gw_sched_run(...); return; }   <-- 调度模式
 *      gw_timing_begin(app->timing, GW_PHASE_SLAVE); ...      <-- 顺序模式
 *  相位耗时打点**只在顺序模式**执行，而第 12 步总会启用 gw_sched，
 *  于是三个指标在任何出厂配置下都拿不到值：
 *      - RTOS 构建：gw_app_poll() 根本不被调用
 *      - 非 RTOS 构建：走 gw_sched_run() 分支后 return
 *
 *  ⚠️ 实测结论（2026-10-17）：即使置 1 构建出纯顺序版本，
 *     该配置在硬件上约 2 s 后**应用逻辑冻结**（uwTick 正常但 tick_ms 不传递）
 *     ⇒ 修好 tick 传播之前，此开关不能用于测量。
 *     详见 docs/06-M5-M7-M8测量口径.md
 *
 *  ⚠️ 代价：关掉 gw_sched 后 deadline_miss 统计不可用（仅测量用，可接受）。
 * ================================================================== */
#ifndef GW_MEAS_SEQUENTIAL
#define GW_MEAS_SEQUENTIAL 0   /* 量产默认 0；测量时临时改 1 并构建非 RTOS 版 */
#endif

/* ------------------------------------------------------------------
 *  ⚠️ 全部放静态区 —— 栈上放不下（见文件头说明）
 * ------------------------------------------------------------------ */
static mb_regs_t  g_regs;
static mb_slave_t g_slave;
static gw_param_t g_param;
static gw_app_t   g_app;
static gw_timing_t g_tm;
/* ⚠️ 协作式调度器只在裸机下用；RTOS 下 gw_rtos.c 内部有自己的调度器实例
 *    （gw_rtos_create_tasks() 会初始化它，用于 deadline_miss 统计）。
 *    `GW_MEAS_SEQUENTIAL=1`（测量专用）时也不声明，否则会报 unused 警告。 */
#if !(defined(GW_USE_FREERTOS) && (GW_USE_FREERTOS == 1)) && !GW_MEAS_SEQUENTIAL
static gw_sched_t  g_sched;
#endif
static gw_log_t    g_log;

/* 上云与 CAN 的移植层接口（值拷贝给应用层） */
static gw_uplink_port_t g_up;
static gw_can_port_t    g_can;

/* 波特率运行时切换（9600/19200 可配） */
static gw_baud_t g_baud;

/* OTA 下行帧分发 */
static gw_ota_bridge_t g_ota;

/* 上报节拍累加器（秒 -> 毫秒）
 * ⚠️ 两条路径都用它（RTOS 下由 HOUSE 任务调 app_housekeeping() 驱动）。 */
static uint32_t g_report_acc_ms;
static uint32_t g_report_period_s = 10u;    /* 默认 10 秒，会被参数覆盖 */

/* 运行时日志等级（可被下行指令改） */
static gw_log_level_t g_log_level = GW_LOG_INFO;

/* 系统启动原因（供日志与状态位使用） */
static gw_boot_cause_t g_boot_cause;

/* ==================================================================
 *  日志的串口输出通道
 *
 *  ⚠️ 超时要短（20ms）。
 *     日志不该成为阻塞源 —— 没接串口时 HAL_UART_Transmit 会一直
 *     等到超时，把那段时间白白浪费掉；而在时序敏感的路径里
 *     打日志会把功能搞坏。
 * ================================================================== */
static void log_sink_uart(void *ctx, gw_log_level_t lv, gw_log_module_t mod,
                          uint16_t line, uint32_t a0, uint32_t a1)
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

/* ==================================================================
 *  gw_port_t 的各回调 —— 把应用层与移植层接起来
 *
 *  这些函数都是"一层薄封装"：应用层调它们，它们转调 port 层。
 *  为什么不直接把 rs485_port_* 的函数指针填进 vtable：
 *    因为签名不完全一致（port 层没有 ctx 参数）。
 *    包一层比强转函数指针安全 —— 后者是未定义行为。
 * ================================================================== */

static int cb_take_frame(void *ctx, uint8_t *buf, uint16_t cap, uint16_t *len)
{
    (void)ctx;
    return (rs485_port_take_frame(buf, cap, len) == RS485_OK) ? 0 : -1;
}

static int cb_send(void *ctx, const uint8_t *buf, uint16_t len)
{
    (void)ctx;
    return (rs485_port_send(buf, len) == RS485_OK) ? 0 : -1;
}

static int cb_bus_acquire(void *ctx)
{
    (void)ctx;
    /* 从站角色：它在仲裁里优先级最高，可以抢占主站 */
    return rs485_bus_acquire(RS485_ROLE_SLAVE);
}

static int cb_send_held(void *ctx, const uint8_t *buf, uint16_t len)
{
    (void)ctx;
    /* 主站发请求：以 MASTER 角色发送（vtable 保证已经拿到总线） */
    return (rs485_port_send_as(RS485_ROLE_MASTER, buf, len) == RS485_OK) ? 0 : -1;
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
    /* ⚠️ 只取指针，不刷新内容 —— 这个回调只在 gw_app_init 时被调一次。
       实时字段的刷新在 cb_refresh_regs（见 gw_app.h 的说明）。 */
    return &g_regs;
}

/**
 * DWT 微秒计时的适配函数（给 `gw_timing_init()` 用）
 *
 * ⚠️ **为什么需要这一层包装**：
 *     `gw_dwt_us()` 的签名是 `uint32_t gw_dwt_us(void)`，
 *     而 `gw_timing_init()` 要的是 `uint32_t (*)(void *ctx)` —— 多一个 ctx 参数。
 *     直接把函数指针强转虽然能编过，但那是**未定义行为**：调用方按"有一个参数"
 *     传寄存器，被调用方按"没有参数"实现。ARM 上恰好不出错，
 *     换个编译器或优化级别就可能出问题。
 *
 * ⚠️ 2026-10-17 修复记录：本文件此前把时间源写成 `(uint32_t (*)(void *))0`
 *     （**空指针**），于是 `gw_timing_begin()` 见到 `tm->now_us == 0` 直接 return ——
 *     整个计时插桩**静默失效**，`IREG_TM_SLAVE_MAX / TM_LOOP_MAX / TM_LOAD_PERMIL`
 *     恒为哨兵 `0xFFFE`。而且 `gw_timing_port_bind()` 也从未被调用，
 *     即使测到也读不出寄存器。两处均已补上（见第 11 步）。
 *     这类失效难查的原因：**读出来不是哨兵就是 0，看起来像"性能极好"**。
 */
static uint32_t timing_now_us(void *ctx)
{
    (void)ctx;                          /* DWT 不需要上下文 */
    return gw_dwt_us();
}

/**
 * 刷新寄存器表里的实时字段（诊断 / 耗时统计）
 *
 * ⚠️ 每轮从站任务开头被调用。不能放在 cb_get_regs 里 ——
 *    那个回调只在 gw_app_init() 时调一次，Modbus 读到的会永远是
 *    启动时的陈旧快照（本项上板排查时真踩过这个坑）。
 */
static void cb_refresh_regs(void *ctx)
{
    (void)ctx;

    rs485_port_fill_diag(g_regs.input, (uint16_t)MB_INPUT_REG_COUNT);
    (void)gw_timing_port_fill(g_regs.input, (uint16_t)MB_INPUT_REG_COUNT);
}

static dht22_err_t cb_dht_sample(void *ctx, dht22_filtered_t *f, uint8_t retry)
{
    (void)ctx;
    return dht22_stm32_sample(f, retry);
}

/* ---- 下行指令用到的三个回调 ---- */

static int cb_param_save(void *ctx)
{
    gw_params_t p;
    uint8_t     buf[GW_PARAMS_WIRE_LEN];
    uint32_t    n;

    (void)ctx;

    /* 从寄存器表取参数 -> 打包 -> 落盘 */
    gw_params_from_regs(&p, &g_regs);
    if (gw_params_validate(&p) != GW_PARAMS_OK) {
        return -1;                  /* 寄存器里的值非法，不保存 */
    }
    n = gw_params_pack(&p, buf, sizeof(buf));
    if (n == 0u) {
        return -1;
    }
    return (gw_param_save(&g_param, buf, n, GW_PARAMS_VERSION) == GW_PARAM_OK)
           ? 0 : -1;
}

static int cb_param_restore(void *ctx)
{
    gw_params_t p;
    uint8_t     buf[GW_PARAMS_WIRE_LEN];
    uint32_t    n;

    (void)ctx;

    /* 默认值 -> 落盘 -> 同步进寄存器表 */
    gw_params_default(&p);
    n = gw_params_pack(&p, buf, sizeof(buf));
    if (n == 0u) {
        return -1;
    }
    if (gw_param_save(&g_param, buf, n, GW_PARAMS_VERSION) != GW_PARAM_OK) {
        return -1;
    }
    gw_params_to_regs(&p, &g_regs);
    return 0;
}

static void cb_reboot(void *ctx)
{
    (void)ctx;
    /* ⚠️ 延时 300ms：让下行指令的 ACK 帧有机会发出去。
       不延时的话上位机永远收不到"重启指令已接受"，
       会以为指令丢了，可能重复下发。 */
    gw_fault_port_reset(300u);
    /* 不返回 */
}

/* ---- 上云（ESP32）移植层 ---- */

static int up_esp_send(void *ctx, const uint8_t *buf, uint16_t len)
{
    (void)ctx;
    return (esp32_port_send(buf, len) == ESP32_OK) ? 0 : -1;
}

static void up_esp_tick(void *ctx)
{
    (void)ctx;
    esp32_port_tick();
}

static int up_esp_take_frame(void *ctx, uint8_t *out_type, uint8_t *payload,
                             uint16_t cap, uint16_t *out_len)
{
    (void)ctx;
    return (esp32_port_take_frame(out_type, payload, cap, out_len) == ESP32_OK)
           ? 0 : -1;
}

static uint32_t up_now_ms(void *ctx)
{
    (void)ctx;
    /* 「立即上报」的节拍 */
    return HAL_GetTick();
}

/* ---- CAN 移植层 ---- */

static void can_tick_cb(void *ctx, uint32_t elapsed_ms)
{
    (void)ctx;
    /* can_port_poll 返回本次收到的帧数；它内部已经把帧喂给 CAN 核心，
       所以这里不需要再处理返回值。 */
    (void)can_port_poll(elapsed_ms);
}

static gw_can_core_t *can_get_core_cb(void *ctx)
{
    (void)ctx;
    return can_port_core();
}

/* ==================================================================
 *  OTA 帧回调 —— 把应用层的分派接到 OTA 桥接上
 *
 *  ⚠️ 应用层不直接持有 gw_ota_bridge_t（那会让它依赖 Flash 地址布局），
 *     所以这里包一层：应用层给出"有个 OTA 帧"，
 *     我们知道该转给哪个对象。
 *
 *  返回值语义由应用层原样记录（不解释），
 *  真正的 ACK/NACK 由 gw_ota_bridge_frame 生成。
 * ================================================================== */
static int ota_handler_cb(void *ctx, uint8_t type,
                          const uint8_t *payload, uint16_t len)
{
    gw_ota_bridge_t *ob = (gw_ota_bridge_t *)ctx;
    uint8_t  ack[GW_OTA_ACK_PAYLOAD_LEN];
    uint16_t acklen = 0u;
    gw_ota_br_t r;

    r = gw_ota_bridge_frame(ob, type, payload, len,
                            ack, (uint16_t)sizeof(ack), &acklen);

    /* 回 ACK/NACK 给 ESP32 —— 连 BUSY 与 DUPLICATE 也要回，
       否则上位机在等一个永远不会来的应答。 */
    if (acklen > 0u) {
        uint8_t  frame[GW_FRAME_MAX_TOTAL];
        uint16_t flen = 0u;

        if (gw_frame_build(frame, (uint16_t)sizeof(frame), GW_FT_OTA_ACK,
                           ack, acklen, &flen) == GW_FRAME_OK) {
            if (g_app.up.esp_send != 0) {
                (void)g_app.up.esp_send(g_app.up.ctx, frame, flen);
            }
        }
    }

    /* 返回给应用层：>0 表示"非致命"。BUSY 与 DUPLICATE 都属于这一类 ——
       它们不该被记成错误（DUPLICATE 是链路上正常的重传）。 */
    if (r == GW_OTA_BR_ERR_BUSY || r == GW_OTA_BR_ERR_DUPLICATE) {
        return 1;
    }
    return (int)r;
}

/* ==================================================================
 *  日志输出（把环形缓冲吐到串口）
 * ================================================================== */
static void flush_log(void)
{
    /* ⚠️⚠️ 2026-10-14：**不要 rewind**。
     *
     *  gw_log_rewind() 把 read_idx 归零，而 gw_log_flush() 是从 read_idx 读到尾 ——
     *  于是 flush_log() 每被调用一次，就把**整个环形缓冲重放一遍**。
     *  它被放在主循环末尾（每轮都调）=> 32 条 x 每轮
     *  => 实测调试串口 ~10KB/s、每秒上千行：白占 CPU，还可能挤占 RS485 的时序。
     *
     *  flush 的正确语义是"把**新**日志吐出去"（从当前 read_idx 继续）。
     *  "开机时重放历史"是另一件事，由 dump_log_history() 负责，**它才该 rewind**。 */
    (void)gw_log_flush(&g_log);
}

/* ==================================================================
 *  跑自检并打报告
 *
 *  ⚠️ 自检**会真的动硬件**（详见 port/gw_selfcheck_port.h）：
 *     - FLASH 项会擦写参数区最旧的槽（不破坏有效参数，消耗 1 次寿命）
 *     - RS485 项会申请/释放总线（总线上有别的设备时建议关掉）
 *
 *  不想让它动 Flash 就加：
 *      gw_selfcheck_port_set_flash(0);
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

    /* 报告直接打串口 —— 不经过日志模块，
       因为自检就是为了确认"串口/时钟这些基础项到底通不通"，
       如果它自己依赖日志模块，日志模块坏了就什么都看不到。 */
    (void)HAL_UART_Transmit(&huart1, (uint8_t *)buf,
                            (uint16_t)strlen(buf), 500u);

    return gw_selfcheck_foundation_ok(&sc);
}

/* ==================================================================
 *  main
 * ================================================================== */
/* ==================================================================
 *  ★★ 主循环"内务"：两条路径（裸机 / RTOS）**共用**
 *
 *  ⚠️⚠️ 为什么必须抽出来（缺陷 W，2026-10-11）：
 *     把 main 拆成"裸机主循环 / RTOS 分支"时，最容易漏的不是任务体，
 *     而是**循环里剩下的那些内务**。当时 RTOS 分支只写了
 *     "建任务 + 使能 IWDG + 启动调度器"，于是下面这些**一个都不执行**：
 *
 *        gw_fault_port_tick()          -> 运行时长/故障计数不再推进
 *                                         （而 OTA 确认要读它！）
 *        gw_log_tick()                 -> 日志时标与自动刷新停摆
 *        gw_ota_bridge_tick()          -> **OTA 传输状态机不走 -> 根本收不下固件**
 *        gw_ota_confirm_running()      -> **升级永远不确认 -> 3 次后必然回滚**
 *        上报节拍 / 波特率切换 / 重启挂起 -> 全部失效
 *
 *    也就是说：RTOS 一落地，**OTA 与上报整条链路就死了** ——
 *    而四步实验（⑤ 要做的）第一项就会失败。这些都是**静默**的：
 *    编译、链接、CI 全都不会报。
 *
 *  ⚠️ 调用方：
 *      裸机：主循环每轮调（dt 由 HAL_GetTick 实测）
 *      RTOS：HOUSE 任务（优先级最低、10ms）通过强符号
 *            `gw_rtos_housekeeping()` 调（见下面）
 * ================================================================== */
static void app_housekeeping(uint32_t dt)
{
    static uint32_t confirm_acc = 0u;   /* OTA 确认的 1 秒节拍 */

    gw_fault_port_tick(dt);
    gw_log_tick(&g_log, dt);

    /* ---- 周期刷新诊断寄存器（本地 UI / 任何本地读取都依赖它）----
     *
     * ⚠️ 为什么必须补这一句：
     *   诊断寄存器原本**只在从站任务应答前**刷新（gw_app.h:180 的约定：
     *   "refresh_regs -> 每次从站任务前刷新内容"），所以**没有 Modbus 请求时
     *   g_regs 里是初始化快照**（uptime 恒 0）。主站来请求时会先刷新，
     *   产品行为本身没错；但**本地屏幕**若直接读它就会一直显示开机那一刻的值。
     *   2026-10-11 浸泡巡检正是踩到这个（uptime 列恒 0，最后改用 uw_tick 才绕过）。
     *   这里由 HOUSE（最低优先级、每轮都跑）补一次周期刷新 ->
     *   Modbus 主站与本地屏幕读到的就是**同一份、且都是新的**数据。
     * ⚠️ 数据类寄存器（温湿度/CAN/管道）**不用**在这里刷：HOUSE 的任务入口
     *   已经做了 gw_app_sync_*_to_pipe + gw_pipeline_sync_regs（见 gw_app.c）。*/
    if (g_app.port.refresh_regs != 0) {
        g_app.port.refresh_regs(g_app.port.ctx);
    }

    /* ---- 上报节拍 ----
       周期从保持寄存器读（上位机或下行指令可改）。 */
    {
        uint16_t p = g_regs.holding[HREG_REPORT_PERIOD];

        if (p >= 1u && p <= 3600u) {
            g_report_period_s = p;
        }
        g_report_acc_ms += dt;
        if (g_report_acc_ms >= g_report_period_s * 1000u) {
            g_report_acc_ms = 0u;
            (void)gw_app_submit_telemetry(&g_app);
        }
    }

    /* ---- 下行指令请求的"立即上报" ---- */
    if (gw_app_take_report_now(&g_app) != 0) {
        (void)gw_app_submit_telemetry(&g_app);
    }

    /* ---- 波特率运行时切换 ----
     *
     * 上位机写 HREG_BAUDRATE 之后，这里把请求交给状态机，
     * 由它挑一个"总线空闲且已过静默期"的时机真正切换。
     *
     * ⚠️ 三个条件缺一不可（见 inc/gw_baud.h）：
     *      不在发送中 / 不在主站等待窗口 / 过了静默期
     *    否则应答的后半段会用新波特率发出，对方错位同步，
     *    接下来连续几帧都失败 —— 现场表现是"改了波特率通信就乱了"。
     */
    {
        uint32_t want = gw_baud_from_code(g_regs.holding[HREG_BAUDRATE]);

        /* 只有与当前生效值不同才登记请求。
           上位机会周期性写同一个值，每次都登记会把统计刷爆。 */
        if (want != 0u && want != gw_baud_current(&g_baud)) {
            (void)gw_baud_request(&g_baud, want);
        }

        /* 两个"不安全"信号都来自 RS485 移植层 */
        if (gw_baud_tick(&g_baud, dt,
                         (gw_baud_port_busy() != 0) ? 1 : 0,
                         (rs485_master_holds_bus() != 0) ? 1 : 0) != 0) {
            if (gw_baud_apply(&g_baud) != 0) {
                GW_LOG_ERR(&g_log, GW_LOG_MOD_RS485,
                           gw_baud_pending(&g_baud), 3u);
            } else {
                GW_LOG_INF(&g_log, GW_LOG_MOD_RS485,
                           gw_baud_current(&g_baud), 0u);
            }
        }
    }

    /* ---- OTA：擦除之外的部分（擦除在下面单独做）---- */
    gw_ota_bridge_tick(&g_ota);

    /* ---- OTA：确认这次的升级（必须有，否则升级会被无故回滚）----
     *
     *  ⚠️ 这一段的必要性来自一个真实缺陷：
     *     `gw_ota_should_confirm()` / `gw_ota_meta_confirm()`
     *     原本**只有定义、声明和测试，生产代码里一个调用者都没有**。
     *     于是升级到 B 之后 pending 永远是 1：
     *         复位 -> Bootloader 看到 pending=1 -> boot_tries++
     *              -> 累计 3 次 -> **把完全正常的新固件回滚掉**
     *
     *  ⚠️ 用一步式封装，不要自己拼四步（漏 save 不会报错，只是 pending 永不清）。
     *  ⚠️ 每秒一次（不是每轮）：稳态下开销 = 每秒一次 Flash 读。 */
    confirm_acc += dt;
    if (confirm_acc >= 1000u) {
        confirm_acc = 0u;

        /* len/crc 传 0：它们只是诊断信息（见 gw_ota.h 的说明）。
           ⚠️ 必须传 `gw_ota_running_slot()`：确认的前提是"待确认的升级
              就是我现在跑的这份"（缺陷 P）。 */
        gw_ota_confirm_r_t cr2 = gw_ota_confirm_running(
            gw_ota_port_io(), gw_ota_running_slot(),
            gw_fault_port_uptime_s(), 0u, 0u);

        if (cr2 == GW_OTA_CONFIRM_R_DONE) {
            GW_LOG_WRN(&g_log, GW_LOG_MOD_APP, 6u, 0u);
        } else if (cr2 == GW_OTA_CONFIRM_R_WRONG_SLOT) {
            /* ⚠️ 专门一个日志码（缺陷 P 的症状），每秒一次会变噪音，只报第一次。 */
            static uint8_t ota_ws_warned = 0u;
            if (ota_ws_warned == 0u) {
                ota_ws_warned = 1u;
                GW_LOG_WRN(&g_log, GW_LOG_MOD_APP, 7u, 0u);
            }
        }
    }

    /* ---- 日志输出放在最后 ----
       让本轮产生的日志一起吐出去，而不是在业务逻辑中间阻塞串口。 */
    flush_log();

    /* ---- OTA：执行待处理的擦除 ----
     *
     * ⚠️ 放**末尾**：擦一页 20~40 ms，放中间会让本轮的 RS485 响应晚 40 ms。
     *    `gw_ota_port.c` 的 erase 内部逐页擦 + 逐页"让出 CPU"
     *    （裸机是空函数，RTOS 下是 `vTaskDelay(1)` —— 缺陷 Q 的修法），
     *    所以 RTOS 下这一整段擦除**不会**占着 CPU 不放，
     *    Modbus 从站与看门狗都不受影响。 */
    if (gw_ota_bridge_begin_pending(&g_ota) != 0) {
        if (gw_ota_bridge_do_begin(&g_ota) != 0) {
            GW_LOG_ERR(&g_log, GW_LOG_MOD_APP,
                       (uint32_t)gw_ota_bridge_state(&g_ota), 4u);
        } else {
            GW_LOG_WRN(&g_log, GW_LOG_MOD_APP,
                       (uint32_t)gw_ota_port_stats()->pages, 0u);
        }
    }

    /* ---- OTA：升级完成，延时重启 ----
     *
     * ⚠️ 必须先让 ACK 发出去（已经在帧处理里发出去了），再由这里重启。
     * ⚠️ 用 gw_fault_port_reset(延时) 而不是 HAL_NVIC_SystemReset()：
     *    前者会先把复位原因写进 .noinit，重启后能看出"这是 OTA 主动重启"。 */
    if (gw_ota_bridge_reboot_pending(&g_ota) != 0) {
        GW_LOG_WRN(&g_log, GW_LOG_MOD_APP, 5u, 0u);
        flush_log();
        gw_fault_port_reset(500u);      /* 不返回 */
    }
}

#if defined(GW_USE_FREERTOS) && (GW_USE_FREERTOS == 1)
/* ★ RTOS 下由 HOUSE 任务周期调用（`port/gw_rtos.c` 里是弱符号空实现）。
 *
 *  ⚠️ 这是个**强定义覆盖弱符号**的钩子：如果这段被删掉，链接不会报错，
 *     只会静默地什么都不做（= 缺陷 W 重现）。所以 CI 里有文本检查盯着
 *     本文件必须出现 `void gw_rtos_housekeeping(`（tools/check_rtos_hooks.py）。 */
/* ==================================================================
 *  本地 UI 的接入（手绘 4 页，见 app/gw_ui.h）
 *
 *  三条纪律（都是本次设计时定下的，改这里前先读一遍）：
 *   1) UI **只读**寄存器表、**只经 set_hreg 写**保持寄存器 ——
 *      所以屏幕与 Modbus 主站看到的永远是同一份数据，不存在两份状态。
 *   2) 寄存器表由 HOUSE 周期刷新（见 app_housekeeping 里那段），
 *      否则 UI 会在空闲时读到开机的初始化快照。
 *   3) 界面跑在**最低优先级**任务里（GW_PRIO_UI = 1，与 HOUSE 同级），
 *      业务任务一就绪就抢占它 —— 这才是"刷屏不阻塞采集与上云"的机制。
 * ================================================================== */

static const uint16_t *ui_get_iregs(void)
{
    return g_regs.input;
}

static const uint16_t *ui_get_hregs(void)
{
    return g_regs.holding;
}

static void ui_set_hreg(uint16_t idx, uint16_t val)
{
    /* ⚠️ 不在这里做"应用"：保持寄存器是唯一权威，
       真正的应用由 app_housekeeping 每轮读它完成（波特率/上报周期都是这么做的）。
       这样 UI 写与主站写走的是**完全相同**的路径。 */
    if (idx < MB_HOLDING_REG_COUNT) {
        g_regs.holding[idx] = val;
    }
}

/* 屏幕不在时置 1：将来可以由上报/寄存器把它暴露出去 */
static uint8_t s_ui_absent = 0u;

static const gw_ui_port_t g_ui_port = {
    ui_get_iregs, ui_get_hregs, ui_set_hreg
};

/* 工程侧覆盖弱钩子：UI 每次被调度到就刷一拍（dt 用实测值）*/
void gw_rtos_ui_tick(uint32_t dt_ms)
{
#if defined(GW_USE_LVGL)
    /* LVGL 路径：推进时基 -> 用寄存器表快照刷数据 -> 跑一次 LVGL 定时器/绘制。
       ⚠️ 顺序不能反：先 tick_inc 再 handler，否则 LVGL 的时间基准落后一拍，
          动画与定时器都会慢一个 UI 周期（50ms）。
       ⚠️ 数据仍走 g_ui_port（与自绘界面同一个 vtable）—— 两条显示路径读的是
          同一份寄存器快照，所以"左右对比"才有意义。 */
    lvgl_port_tick(dt_ms);
    gw_lvgl_ui_update(g_ui_port.get_iregs());
    lvgl_port_handler();
#else
    gw_ui_tick(dt_ms);
#endif
}

void gw_rtos_housekeeping(uint32_t dt_ms)
{
    app_housekeeping(dt_ms);
}
#endif

int main(void)
{
    gw_port_t port;

    /* ---------------- 1) CubeMX 生成的外设初始化 ----------------
     *
     * ⚠️ 顺序与 CubeMX 默认一致，不要改动。
     */
    /* ⚠️ 被引导程序跳转过来时，PRIMASK 是引导程序留下的"关中断"状态
       （复位会自动清零，跳转**不会**）—— 不显式打开，TIM4 中断永远不进来、
       HAL_GetTick 恒为 0，所有带超时的 HAL 调用直接挂死。
       本行在"直接复位启动"时是无害的空操作；详细说明见 L0 模板。 */
    __enable_irq();

    HAL_Init();
    SystemClock_Config();           /* ⚠️ 必须在 gw_dwt_init 之前 */
    MX_GPIO_Init();
    MX_DMA_Init();                  /* RS485 与 ESP32 都用了 DMA */
    MX_USART1_UART_Init();          /* 日志口 */
    MX_USART2_UART_Init();          /* RS485 */
    MX_USART3_UART_Init();          /* ESP32 */
    MX_CAN_Init();
    MX_SPI1_Init();          /* ⚠️ 本地屏幕（ILI9341/XPT2046）走 SPI1。
                              *    2026-10-11 实测教训：漏了这句 ->
                              *    hspi1 全零 -> Instance==NULL ->
                              *    启动早期 HardFault -> 安全模式
                              *    （与 hiwdg.Instance==0 同一类）*/

    /* ⚠️ IWDG 句柄**必须**在这里初始化（2026-10-11 板上实测的 HardFault）：
     *    `gw_rtos_iwdg_enable()` 调的是 HAL 的 `HAL_IWDG_Init(&hiwdg)`，
     *    而 `hiwdg.Instance` 只有 `MX_IWDG_Init()` 会赋值。漏掉它 ->
     *    `Instance == 0` -> HAL 往 0x00000008 写寄存器 -> 精确总线错误
     *    -> HardFault（BusFault 未使能）-> 连续 4 次 -> 安全模式空转。
     *    L0 模板一直是在这个位置调它（见 main_L0_bringup.c），生产模板漏了。
     * ⚠️ 这也会**启动**看门狗（HAL 的 IWDG_Init 内含 START）：
     *    这是设计如此 —— 它是唯一能覆盖"启动期静默挂死"的机制。 */
    MX_IWDG_Init();
    /* ⚠️ 调度器起来之前**必须自己喂狗**（2026-10-12 板上实测的复位循环）：
     *   看门狗由 MX_IWDG_Init() 在启动早期就开启（1.6s），而第一次真正的喂狗
     *   要等调度器起来后的空闲钩子 —— 中间这段（启动日志 + 初始化）没人喂。
     *   实测那段约 1.3 秒，加上界面初始化 0.3 秒就越线，现象是"每次停在
     *   同一个进度码 + boot_count 一直涨"。所以这里先把最长的那个间隔封住。 */
    (void)HAL_IWDG_Refresh(&hiwdg);

    /* ---------------- 2) 故障模块（**最先**） ----------------
     *
     * 必须在任何可能出错的事之前：它要读 `.noinit` 判定
     * "上次是怎么启动的"，晚了就覆盖不到了。
     *
     * ⚠️ 它依赖 `.noinit` 段跨复位存活。实测（GNU ld + ARM GCC）：
     *    **通常不需要**手工改链接脚本 —— ld 会自动创建该段并放在
     *    .bss 之外。详细数据见 port/gw_fault_port.h。
     *
     *    **验证方法**：跑一次 -> 按复位 -> 看 boot_count 是否 +1。
     *    不增长就说明没生效。注意 `gw_fault_port_selfcheck()` 查不出
     *    这个问题（它只验证内存可写，查不出"是否被启动代码清零"）。
     */
    g_boot_cause = gw_fault_port_init();

    /* ---------------- 3) DWT 微秒计时 ----------------
     *
     * ⚠️ 必须在 SystemClock_Config 之后：
     *    DWT 的 CYCCNT 按 CPU 周期计数，"1 微秒等于多少周期"
     *    是用 SystemCoreClock 算出来的。时钟没配好就初始化，
     *    微秒计时会偏（例如按 8MHz HSI 算，实际 72MHz，
     *    于是所有延时都短了 9 倍）—— **DHT22 必然读不出**。
     */
    gw_dwt_init();

    /* ---------------- 4) 日志 ---------------- */
    {
        gw_log_io_t io;

        memset(&io, 0, sizeof(io));
        io.sink_text = log_sink_uart;
        io.ctx       = &huart1;     /* CubeMX 生成的句柄名 */
        (void)gw_log_init(&g_log, &io, g_log_level);
    }

    /* ---------------- 5) 从站与寄存器表 ---------------- */

    /* ⚠️ 这里必须把 rs485_port 的从站与寄存器表**交给它自己**：
       rs485_port_init 内部会创建这两个对象并通过
       rs485_get_slave() / rs485_get_regs() 暴露出来。
       我们自己不再另建一份 —— 那样会有两份寄存器表，
       而"两份数据源"迟早不一致。 */
    if (rs485_port_init(GW_RS485_DEFAULT_ADDR) != RS485_OK) {
        GW_LOG_ERR(&g_log, GW_LOG_MOD_RS485, 0u, 0u);
        /* 从站初始化失败不阻止启动：也许只是参数问题，
           主站与上云仍可能工作。 */
    }
    g_slave = *rs485_get_slave();       /* 值拷贝（应用层只读它） */
    memcpy(&g_regs, rs485_get_regs(), sizeof(g_regs));

    /* ---------------- 6) Flash 参数 ----------------
     *
     * ⚠️ 加载失败**必须继续启动**：
     *    参数丢了不该让设备变砖。用默认值起来，
     *    让上位机重新下发即可。
     *
     * 「四道关卡」（读取成功 / 长度合法 / 反序列化+校验 / 版本匹配）
     * 都封装在 `gw_params_pick_startup()` 里，已由 test_devinfo.c 的
     * 116 项测试覆盖。**不要在 main 里重写这段判断** ——
     * 那份逻辑里有几个容易漏的点（例如 slave_addr=0 会让设备变广播地址）。
     */
    {
        static uint8_t pbuf[GW_PARAM_MAX_DATA];
        uint32_t    plen = 0u;
        uint16_t    pver = 0u;
        int32_t     lret;
        gw_param_err_t  pret;
        gw_param_pick_t pick;

        /* ⚠️⚠️ `gw_param_init()` 的返回值**不是"故障"判据**（2026-10-14 实测澄清）：
         *
         *   参数扇区**从未写过**时（上电读 `0x0807C000` 全是 0xFF），
         *   本函数必然返回 `GW_PARAM_ERR_NO_VALID` —— 那是**首次上电的正常状态**，
         *   紧接着下面 `gw_params_pick_startup()` 就会回落到默认值。
         *
         *   所以分级要分开：
         *     - `NO_VALID`      -> INFO（正常，参数用默认值）
         *     - **其它**非 OK   -> ERR （真故障：Flash 读失败 / 参数非法…）
         *
         *   ⚠️ 并且 arg0 必须带**真实错误码**（枚举见 `gw_param_strerror()`），
         *      不能写死常数 —— 写死了就没法从日志区分是哪一种失败。
         *
         *   历史教训：这里原来一律记 ERR 且 arg0 写死 `1u`，
         *   于是每次上电日志都有 `[E][PARAM:890] 1 0`，
         *   被当成"参数初始化失败"排查了一轮，实际只是首次上电。
         */
        pret = gw_param_init(&g_param, gw_flash_stm32_iface());
        if (pret == GW_PARAM_ERR_NO_VALID) {
            GW_LOG_INF(&g_log, GW_LOG_MOD_PARAM, (uint32_t)pret, 0u);
        } else if (pret != GW_PARAM_OK) {
            GW_LOG_ERR(&g_log, GW_LOG_MOD_PARAM, (uint32_t)pret, 0u);
        }

        lret = (int32_t)gw_param_load(&g_param, pbuf, sizeof(pbuf),
                                      &plen, &pver);

        (void)gw_params_pick_startup(lret, pbuf, plen, pver, &pick);

        /* 无论来源是哪，都同步进寄存器表 ——
           这样上位机第一次读就能看到确定的值。 */
        gw_params_to_regs(&pick.params, &g_regs);

        if (pick.source == GW_PARAM_SRC_FLASH) {
            g_report_period_s = pick.params.report_period_s;
            GW_LOG_INF(&g_log, GW_LOG_MOD_PARAM, plen, pver);
        } else {
            g_report_period_s = pick.params.report_period_s;
            /* ⚠️ 用默认值起来时**不写 Flash**。
               每次上电都写一遍会很快耗完擦写寿命（约 1 万次）。
               等上位机显式下发保存命令即可。 */
            GW_LOG_WRN(&g_log, GW_LOG_MOD_PARAM,
                       (uint32_t)pick.reason, 1u);
        }

        if (g_report_period_s < 1u || g_report_period_s > 3600u) {
            g_report_period_s = 10u;
        }
    }

    /* ---------------- 7) 应用层 ---------------- */
    {
        memset(&port, 0, sizeof(port));
        port.rs485_take_frame       = cb_take_frame;
        port.rs485_send             = cb_send;
        port.rs485_bus_acquire      = cb_bus_acquire;
        port.rs485_send_held        = cb_send_held;
        port.rs485_enter_wait       = cb_enter_wait;
        port.rs485_exit_wait        = cb_exit_wait;
        port.rs485_release_master   = cb_release_master;
        port.rs485_in_wait          = cb_in_wait;
        port.rs485_master_holds_bus = cb_holds_bus;
        port.get_regs               = cb_get_regs;
    port.refresh_regs           = cb_refresh_regs;
        port.dht_sample             = cb_dht_sample;
        port.param_save             = cb_param_save;
        port.param_restore          = cb_param_restore;
        port.reboot                 = cb_reboot;

        if (gw_app_init(&g_app, &g_slave, &port) != 0) {
            GW_LOG_ERR(&g_log, GW_LOG_MOD_APP, 1u, 0u);
            for (;;) { }            /* 应用层都起不来，没必要往下走 */
        }
    }

    /* ---------------- 8) DHT22 硬件层 ---------------- */
    (void)dht22_stm32_init();

    /* ---------------- 9) 上云（ESP32） ----------------
     *
     * ⚠️ 三个回调缺一个，gw_app_enable_uplink 会把整个上云**静默禁用**
     *    （见它的实现）。所以这里三个都填。 */
    {
        memset(&g_up, 0, sizeof(g_up));
        g_up.esp_send       = up_esp_send;
        g_up.esp_tick       = up_esp_tick;
        g_up.esp_take_frame = up_esp_take_frame;
        g_up.now_ms         = up_now_ms;

        if (esp32_port_init() != ESP32_OK) {
            GW_LOG_WRN(&g_log, GW_LOG_MOD_UPLINK, 0u, 0u);
        }
        (void)gw_app_enable_uplink(&g_app, &g_up, (uint32_t)100000u);
    }

    /* ---------------- 10) CAN ----------------
     *
     * 滤波 ID / 掩码来自保持寄存器（上位机可配）。 */
    {
        uint16_t fid = (uint16_t)((g_regs.holding[HREG_CAN_FILT_ID_HI] << 8)
                                  | g_regs.holding[HREG_CAN_FILT_ID_LO]);
        uint16_t fmsk = (uint16_t)((g_regs.holding[HREG_CAN_FILT_MSK_HI] << 8)
                                   | g_regs.holding[HREG_CAN_FILT_MSK_LO]);

        memset(&g_can, 0, sizeof(g_can));
        g_can.tick     = can_tick_cb;
        g_can.get_core = can_get_core_cb;

        if (can_port_init(fid, fmsk) != CAN_PORT_OK) {
            GW_LOG_WRN(&g_log, GW_LOG_MOD_CAN, fid, fmsk);
        }
        (void)gw_app_enable_can(&g_app, &g_can);
    }

    /* ---------------- 11) 时间预算统计 ----------------
     *
     * ⚠️ 三件事必须**同时**成立，缺一件就退化成"读出来全是 0 或哨兵"：
     *      ① init 时给**真实时间源** —— 不能用空指针强转（见 timing_now_us 的说明）
     *      ② enable —— 不给应用层打点，各段耗时恒为 0
     *      ③ bind   —— 不绑给 port 层，cb_refresh_regs 里 fill 出来就是哨兵 0xFFFE
     *
     * 早期 templates/main_L0_bringup.c 里作者留过原话：
     *   "不 enable→不打点；不 bind→读不到；三种情况结果都是读出来全是 0，
     *    ⚠️ 而那**不代表性能好，只代表没接线**。"
     * 本文件迁移到生产模板时把 ①③ 丢了，直到 2026-10-17 才通过
     * `IREG_TM_*` 恒为 0xFFFE 反查出来。 */
    if (gw_timing_init(&g_tm, timing_now_us, 0) == 0) {
        gw_app_enable_timing(&g_app, &g_tm);      /* ② 打点 */
        gw_timing_port_bind(&g_tm);               /* ③ ★ 2026-10-17 补：此前缺失 */
    } else {
        GW_LOG_WRN(&g_log, GW_LOG_MOD_SYS, 9u, 0u);
    }

    /* ---------------- 12) 调度器 ----------------
     *
     *  ⚠️ RTOS 构建下**不要**启用协作式调度：每个任务自己 `for(;;)`，
     *     主循环根本不存在；两边都去推进同一个任务会打架。
     *     但 `gw_sched_t` 仍然要初始化 —— 不是为了分派，而是为了统计
     *     （`gw_sched_stat().deadline_miss` 是判断"有没有任务超时"的唯一依据）。
     *     那件事由 `gw_rtos_create_tasks()` 做，见下面的 RTOS 分支。
     *
     *  ⚠️ `GW_MEAS_SEQUENTIAL`（定义在文件顶部的 Includes 之后）：
     *     置 1 则**不启用**协作式调度，使 `gw_app_poll()` 走纯顺序分支，
     *     从而让 `gw_timing` 的相位打点生效。仅用于 M5/M7/M8 测量。
     *     实测：该配置在硬件上约 2 s 后应用逻辑冻结，修好 tick 传播前不可用。 */
#if !(defined(GW_USE_FREERTOS) && (GW_USE_FREERTOS == 1))
#if GW_MEAS_SEQUENTIAL
    /* 测量专用：不启用协作式调度 -> gw_app_poll() 走纯顺序分支 -> 计时生效 */
#else
    if (gw_sched_init(&g_sched, gw_app_task_defs(), 0, 0) == 0) {
        gw_sched_start(&g_sched);
        gw_app_enable_sched(&g_app, &g_sched);
    }
#endif
#endif

    /* ---------------- 13) 自检（可选，但强烈建议首次上板时开） ----------------
     *
     * 它会打印一份带"失败了先查什么"的报告。
     * 首次上板时把这一行打开，能省掉大量猜测。
     */
    if (run_selfcheck() == 0) {
        /* 基础项（时钟/GPIO/DWT/Flash）没过，后面的验证没有意义。
           不要急着往下调 —— 先看报告里第一个 FAIL 的"先查什么"。 */
        for (;;) { }
    }

    /* ---------------- 15) 波特率运行时切换 ----------------
     *
     * ⚠️ 初值用 `gw_baud_port_actual()` **从 BRR 反算出来**，
     *    而不是写死 GW_RS485_UART_BAUD。
     *    理由：CubeMX 生成的初始化可能与我们以为的不一致
     *    （例如 .ioc 里改了波特率但没重新生成）。
     *    以硬件实际值为准，状态机才不会与事实脱节。
     */
    {
        uint32_t actual = gw_baud_port_actual();

        if (actual == 0u) {
            actual = GW_RS485_UART_BAUD;    /* 读不出来才退回编译期常量 */
        }
        if (gw_baud_init(&g_baud, actual) != 0) {
            GW_LOG_ERR(&g_log, GW_LOG_MOD_RS485, actual, 1u);
        }
    }

    /* ---------------- 16) OTA 下行帧 ----------------
     *
     * ⚠️ 目标分区必须是**当前运行分区的另一个**。
     *    如果传成当前分区，就成了"原地升级"——
     *    写到一半断电会把正在运行的固件毁掉，且无法回滚。
     *
     *    `gw_ota_boot_decide()` 里已经有 SAME_SLOT 检查会拒绝，
     *    但这里明确算出来更清楚。实际项目中由 Bootloader 决定，
     *    见 doc/ 里的 OTA 说明。
     */
    {
        gw_ota_slot_t target = GW_OTA_SLOT_B;    /* 从 A 启动时升级到 B */

        if (gw_ota_bridge_init(&g_ota, gw_ota_port_io(), target) != 0) {
            GW_LOG_ERR(&g_log, GW_LOG_MOD_APP, 0u, 2u);
        }
        gw_ota_port_stats_reset();

        /* ⚠️ 注册回调之后，应用层才会把 OTA 帧转过来。
           在此之前那三个类型是"显式忽略"的 ——
           测试全绿不等于功能接了线，这一步才是接线。 */
        gw_app_enable_ota(&g_app, ota_handler_cb, &g_ota);
    }

    /* ---------------- 17) 报告启动原因 ---------------- */
    GW_LOG_WRN(&g_log, GW_LOG_MOD_FAULT,
               (uint32_t)g_boot_cause,
               (uint32_t)gw_fault_port_record()->boot_count);

    /* ⚠️ IWDG 尚未使能。理由见 doc/故障处理与日志.md：
       裸机下有 Flash 擦除（20~40ms）、DHT22 时序（关中断 5ms）
       等长阻塞路径，开狗容易误复位。
       FreeRTOS 化后把喂狗放到空闲任务钩子里才可靠。 */


#if defined(GW_USE_FREERTOS) && (GW_USE_FREERTOS == 1)
    /* ---------------- 14) RTOS：建任务 + 使能 IWDG + 启动调度器 ----------------
     *
     *  ⚠️ 这一段与下面的裸机主循环**互斥**。
     *     **业务代码一行都不用改** —— 6 个任务入口的签名
     *     （`void (*entry)(void *ctx)`）与 FreeRTOS 的 `TaskFunction_t` 完全一致。
     */

    /* 建任务（失败要能报出来，所以与启动分开） */
    /* 本地界面：初始化放在建任务之前 —— 这一步里有 lcd_read_id 自检，
      屏幕没接上时返回负值，我们只记一笔然后继续（界面不是必需品）。
      ⚠️ 它内部用 HAL_Delay，必须在调度器起来之前做。 */
    /* ⚠️ 调度器起来之前**必须先喂一次狗**（2026-10-12 板上实测的复位循环）：
     *   MX_IWDG_Init() 在启动早期就开了看门狗（1.6s），而首次喂狗要等调度器起来后的
     *   空闲钩子。本次启动到这里已经花了约 1.3s（启动日志 + 外设初始化），
     *   紧接着的界面初始化还要约 0.3s -> 越过 1.6s -> 就在 hw_reset() 的
     *   HAL_Delay(20) 里被狗复位，现象是"每次停在同一个进度码 + boot_count 一直涨"。
     *   L0 设计里本来就写着"长阻塞路径自己喂狗"，这里补上。 */
    (void)HAL_IWDG_Refresh(&hiwdg);

    {
#if defined(GW_USE_LVGL)
    /* ---- LVGL 显示路径（构建开关 -Lvgl，见 tools/build_ota_images.ps1）----
     * ⚠️ 与自绘界面**二选一**：两条路都往同一块屏上画，同时开会互相覆盖。
     * ⚠️ LVGL 也要先 lcd_init()（面板上电序列 / 横屏 / 16bpp 都在里面）。
     * ⚠️ LVGL 的 tick/handler 由 RTOS 已有的 UI 钩子驱动（见 port/gw_rtos.c）：
     *    不新增任务、不改任务表、不动栈预算。
     * 起不来时同样是 ui_rc != 0，走下面原有的背光故障信号。 */
    int ui_rc = -1;
    if (lcd_init() == 0) {
        (void)lvgl_port_init();
        gw_lvgl_ui_build();
        ui_rc = 0;
    }
#else
    int ui_rc = gw_ui_init(&g_ui_port);
#endif
    if (ui_rc != 0) {
        /* 屏幕没接上：只记一笔就继续，界面不是必需品。
           ⚠️ 这里**没有**写日志 —— 本仓库没有 GW_LOG_WARN 这类便捷宏，
              日志要走 gw_log_write（签名我没核对过，所以按纪律不猜 API）。
              等真要打时先读 inc/gw_log.h，用正式接口补上。 */
        s_ui_absent = 1;
        /* ⚠️ 自解释的故障信号（2026-10-12 加）：界面起不来时**没有任何反馈**最难查
         * —— 看不出是供电/接线/读 ID 失败，还是界面逻辑问题，而且现场不一定有
         * 串口工具或 SWD。背光是已经确认能工作的一路（用户实测见过它亮），
         * 所以拿它当指示灯：
         *     正常 = 红屏闪一下 -> 出界面，背光常亮
         *     失败 = 背光闪 3 下（150ms 间隔），然后不画界面，其它功能照跑
         * 零额外硬件、零额外工具，一眼就能区分"屏幕/接线"与"界面逻辑"。 */
        {
            /* ⚠️ 闪的次数 = |返回码|：-1 闪 1 下、-2 闪 2 下、-3 闪 3 下。
               这样不用串口、不用 SWD，看背光闪几下就知道卡在哪一步。 */
            int k, n = (ui_rc < 0) ? -ui_rc : ui_rc;
            if (n < 1) { n = 1; }
            if (n > 6) { n = 6; }
            for (k = 0; k < n; k++) {
                lcd_backlight(0);
                HAL_Delay(150u);
                lcd_backlight(1);
                HAL_Delay(150u);
            }
            lcd_backlight(1);
        }
    }
    }

    if (gw_rtos_create_tasks(&g_app) != 0) {
        /* 返回值是"第几个任务失败 + 3"。
           最常见原因：configTOTAL_HEAP_SIZE 不够 ——
           ⚠️ 那个值由 tools/check_rtos_hooks.py 在 CI 里机械核算（缺陷 S）。
           ⚠️ 用工程自己的日志宏 + flush_log()，不要用 printf：
              本工程**没有重定向 printf**（没有 _write/fputc），
              裸机上 printf 通常什么都不输出。 */
        GW_LOG_ERR(&g_log, GW_LOG_MOD_APP, 0u, 0u);
        flush_log();
        for (;;) { }
    }

    /* 使能 IWDG。
       ⚠️ 喂狗在**空闲任务钩子** `vApplicationIdleHook()` 里，判据是
          `gw_app_watchdog_ok()`（"6 个任务都跑过一轮了"）——
          比裸机下"主循环转了一圈"严格，也是唯一能检出**任务级死锁**的判据。 */
    if (gw_rtos_iwdg_enable(1) != 0) {
        GW_LOG_WRN(&g_log, GW_LOG_MOD_APP, 1u, 0u);
        flush_log();
    }

    /* 启动调度器 —— 正常情况下**不返回** */
    gw_rtos_start(&g_app);

    /* 走到这里说明调度器没起来（堆不够时会先在上面的 create_tasks 返回非 0） */
    GW_LOG_ERR(&g_log, GW_LOG_MOD_APP, 2u, 0u);
    flush_log();
    for (;;) { }

#else  /* ---------------- 14) 裸机主循环 ---------------- */
    for (;;) {
        uint32_t now;
        uint32_t dt;

        /* ⚠️ 用 HAL_GetTick 算实际间隔，而不是假设一定是 10ms。
           这样即使某一轮被中断拖长，上报节拍仍然准确。 */
        static uint32_t last_tick = 0u;

        now = HAL_GetTick();
        dt  = now - last_tick;
        last_tick = now;

        if (dt == 0u) {
            continue;               /* 同一毫秒内重复进入：跳过 */
        }

        /* 推进应用层（内部按调度器或顺序模式跑 6 个任务） */
        gw_app_poll(&g_app, dt);

        /* ⚠️ 其余内务与 RTOS 路径**共用同一个函数**（缺陷 W）——
           别把这里的任何一段抄回主循环，否则两条路径迟早不一致。 */
        app_housekeeping(dt);

        HAL_Delay(1);
    }
#endif  /* GW_USE_FREERTOS：裸机主循环与 RTOS 分支互斥 */
}

/* ==================== 复制结束 ==================== */
