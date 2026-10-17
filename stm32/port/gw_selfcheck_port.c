/**
 * @file    gw_selfcheck_port.c
 * @brief   上板自检的硬件项实现
 */
#include "gw_selfcheck_port.h"

/* 注意包含顺序：其它 port 头会间接包含 HAL。 */
#include "stm32f1xx_hal.h"

#include "gw_config.h"
#include "gw_dwt.h"
#include "gw_flash_stm32.h"
#include "gw_fault_port.h"
#include "dht22_port_stm32.h"
#include "rs485_port.h"
#include "esp32_port.h"
#include "gw_can_port.h"

#include <string.h>

/* ==================================================================
 *  可配置开关
 * ================================================================== */
static int      s_rs485_enable  = 1;
static int      s_flash_enable  = 1;
static uint32_t s_expect_clock  = GW_SC_EXPECT_SYSCLK;

void gw_selfcheck_port_set_rs485(int enable) { s_rs485_enable = enable; }
void gw_selfcheck_port_set_flash(int enable) { s_flash_enable = enable; }
void gw_selfcheck_port_set_expect_clock(uint32_t hz)
{
    if (hz > 0u) {
        s_expect_clock = hz;
    }
}

/* ==================================================================
 *  1) CLOCK —— 报告 SystemCoreClock
 *
 *  ⚠️ 这一项**无法自证**。理由见头文件里的说明：
 *     SystemCoreClock 是软件算出来的，它只反映"代码认为的配置"，
 *     不反映"晶振实际振在多少 Hz"。
 *
 *     所以它在报告里的作用是**让人拿这个数去对照**。
 *     我在 hint 里写清楚了验证方法。
 * ================================================================== */
static gw_sc_status_t chk_clock(int32_t *out_value, const char **out_unit)
{
    uint32_t clk = SystemCoreClock;
    int32_t  diff;
    int32_t  tol = (int32_t)(s_expect_clock / 100u);     /* ±1% */

    if (out_unit != 0) {
        *out_unit = "Hz";
    }
    if (out_value != 0) {
        *out_value = (int32_t)clk;
    }

    diff = (int32_t)clk - (int32_t)s_expect_clock;
    if (diff < 0) {
        diff = -diff;
    }

    if (diff <= tol) {
        return GW_SC_PASS;
    }
    /* 偏差在 5% 以内算"可疑"（可能是 HSE 起了但倍频算错一档） */
    if (diff <= (int32_t)(s_expect_clock / 20u)) {
        return GW_SC_WARN;
    }
    return GW_SC_FAIL;
}

/* ==================================================================
 *  2) GPIO —— 用 DWT 侧的一个引脚做翻转自检
 *
 *  怎么测：把一个 GPIO 设成输出，翻转两次，读回输入寄存器。
 *  ⚠️ 但这只验证了 ODR 寄存器写得进 —— 不验证引脚真的动了。
 *     真正的验证要示波器（或者把输出短接到另一个输入引脚）。
 *
 *  所以本项实际做的是：**配置 RS485 的 DE 引脚并翻转它**，
 *  然后读 IDR 确认电平跟随（DE 引脚如果外部有下拉，读回会是 0）。
 *  这比起"什么都不查"强，但也确实弱 —— hint 里说清楚。
 * ================================================================== */
static gw_sc_status_t chk_gpio(int32_t *out_value, const char **out_unit)
{
    GPIO_PinState s1;
    GPIO_PinState s2;

    if (out_unit != 0) {
        *out_unit = "";
    }

    /* 翻转 RS485 的 DE 引脚（PA1）。
       它正常工作时本来就会被翻转，所以这里的动作是安全的。 */
    HAL_GPIO_WritePin(GW_RS485_DE_PORT, GW_RS485_DE_PIN, GPIO_PIN_SET);
    s1 = HAL_GPIO_ReadPin(GW_RS485_DE_PORT, GW_RS485_DE_PIN);

    HAL_GPIO_WritePin(GW_RS485_DE_PORT, GW_RS485_DE_PIN, GPIO_PIN_RESET);
    s2 = HAL_GPIO_ReadPin(GW_RS485_DE_PORT, GW_RS485_DE_PIN);

    /* 恢复成接收态（DE=0），避免自检结束后总线被占着 */
    HAL_GPIO_WritePin(GW_RS485_DE_PORT, GW_RS485_DE_PIN, GPIO_PIN_RESET);

    if (out_value != 0) {
        *out_value = (int32_t)((s1 == GPIO_PIN_SET && s2 == GPIO_PIN_RESET) ? 1 : 0);
    }

    return (s1 == GPIO_PIN_SET && s2 == GPIO_PIN_RESET) ? GW_SC_PASS
                                                        : GW_SC_FAIL;
}

/* ==================================================================
 *  3) DWT —— 自洽性检查
 *
 *  怎么测：用 dwd_delay_us 延时，再用 gw_dwt_us 量。
 *
 *  ⚠️ 这**查不出时钟频率错**（两者同源），但能查出：
 *     - DWT 没使能（CYCCNT 恒为 0）
 *     - CYCCNT 没在走（读到同一个值）
 *     - 延时函数实现有问题（偏差过大）
 * ================================================================== */
static gw_sc_status_t chk_dwt(int32_t *out_value, const char **out_unit)
{
    uint32_t t0;
    uint32_t t1;
    uint32_t dt;
    int32_t  err;

    if (out_unit != 0) {
        *out_unit = "us";
    }

    if (gw_dwt_is_ready() == 0) {
        if (out_value != 0) {
            *out_value = 0;
        }
        return GW_SC_FAIL;
    }

    /* 测 1000µs 的延时。用 1000 而不是 10：
       分辨率和中断抖动的相对影响更小。 */
    t0 = gw_dwt_us();
    gw_dwt_delay_us(1000u);
    t1 = gw_dwt_us();
    dt = t1 - t0;

    if (out_value != 0) {
        *out_value = (int32_t)dt;
    }

    /* 期望 1000µs，容差 ±10%（延时循环有开销，不可能精确） */
    err = (int32_t)dt - 1000;
    if (err < 0) {
        err = -err;
    }

    if (dt == 0u) {
        /* CYCCNT 没在走 —— DWT 没使能，或者计数器被别处清了 */
        return GW_SC_FAIL;
    }
    if (err <= 100) {
        return GW_SC_PASS;
    }
    if (err <= 500) {
        return GW_SC_WARN;
    }
    return GW_SC_FAIL;
}

/* ==================================================================
 *  4) FLASH —— 用已有的 selfcheck（它检查页对齐、槽一致性、解锁）
 * ================================================================== */
static gw_sc_status_t chk_flash(int32_t *out_value, const char **out_unit)
{
    int r;

    if (out_unit != 0) {
        *out_unit = "";
    }

    if (s_flash_enable == 0) {
        if (out_value != 0) {
            *out_value = 0;
        }
        /* 关掉时返回 WARN 而不是 PASS：
           报告里要能看出"这一项没测" —— 返回 PASS 会让人以为测过了。 */
        return GW_SC_WARN;
    }

    r = gw_flash_stm32_selfcheck();

    if (out_value != 0) {
        *out_value = (int32_t)((r == 0) ? 1 : 0);
    }
    return (r == 0) ? GW_SC_PASS : GW_SC_FAIL;
}

/* ==================================================================
 *  5) PARAM —— 参数区的槽数与可读性
 * ================================================================== */
static gw_sc_status_t chk_param(int32_t *out_value, const char **out_unit)
{
    uint8_t  slots;
    uint32_t base;

    if (out_unit != 0) {
        *out_unit = "slots";
    }

    base  = gw_flash_param_base();
    slots = gw_flash_param_slot_count();

    if (out_value != 0) {
        *out_value = (int32_t)slots;
    }

    if (base == 0u || slots < 2u) {
        return GW_SC_FAIL;
    }
    /* 至少要有 2 个槽，否则"掉电安全"无从谈起
       （写新槽时旧槽必须是完好的） */
    return GW_SC_PASS;
}

/* ==================================================================
 *  6) DHT22 —— 真读一次，看校验和
 * ================================================================== */
static gw_sc_status_t chk_dht22(int32_t *out_value, const char **out_unit)
{
    dht22_filtered_t f;
    dht22_err_t      e;

    if (out_unit != 0) {
        *out_unit = "x10";
    }
    memset(&f, 0, sizeof(f));

    if (out_value != 0) {
        *out_value = 0;
    }

    /* ==================================================================
     *  ⚠️ 本层没启用 DHT22 -> **跳过**，不是失败（2026-10-10 改）
     *
     *  L0 那一层（templates/main_L0_bringup.c）**故意不接** DHT22 / CAN /
     *  上云 / OTA —— 而自检项表是各层**共用**的。于是"这一层压根没建立
     *  调用链"会被报成 [FAIL]，现场看起来就是"传感器坏了"：
     *
     *      未 init 时 s_pin.ready == 0
     *      -> dht22_stm32_sample() 直接返回 DHT22_ERR_PARAM（入参错）
     *      -> 报告里 [FAIL] DHT22 ... 0 x10
     *
     *  实测就是这样：板上传感器是好的，报告却说失败，白查一轮。
     *  `dht22_stm32_hw()` 恰好是"有没有 init 过"的判据（未 init 返回 NULL）。
     * ================================================================== */
    if (dht22_stm32_hw() == 0) {
        return GW_SC_SKIP;
    }

    /* ⚠️ 这一项会占用 PG11 约 5ms，期间关中断。
     *
     * ⚠️ 为什么要有限次重试（原来写的是"一次定生死"）：
     *    DHT22 手册要求**上电后 1~2 秒**才开始通信，而自检在启动后
     *    几十毫秒就跑 —— 不重试的话，一个完全正常的传感器也会被报 FAIL。
     *    本模块自己的 hint 里就写着这条（"自检要在那之后跑"），
     *    但代码当时没做到：这是"提示与实现不一致"。
     *
     *    重试次数与间隔是有意的：最多 3 次 × 400ms ≈ 1.2 秒，
     *    正好覆盖手册要求的稳定期；再长就说明真的读不到，不该继续等。
     *    ⚠️ 用 DWT 延时而不是 HAL_Delay：本层可能还没起时基（TIM4），
     *       HAL_Delay 会直接挂死在这里；而 DWT 在 DHT22 init 时已确保可用
     *       （init 里 gw_dwt_is_ready() 不过就直接返回失败了）。 */
    {
        uint8_t attempt;

        for (attempt = 0u; attempt < 4u; attempt++) {
            e = dht22_stm32_sample(&f, 0u);
            if (e == DHT22_OK) {
                break;
            }
            if (attempt < 3u) {
                gw_dwt_delay_us(400000u);       /* 等过传感器的上电稳定期 */
            }
            /* ⚠️ 必须在这里喂狗（2026-10-10 加 IWDG 时补）：
               本项最坏情况要忙等 3 × 400ms = 1.2 秒；IWDG 标称 3.2s、
               按 LSI 60kHz 上限算最短 2.13s —— 现在 1.2s < 2.13s 已经挤得进
               窗口（2026-10-12 由 /64 放宽到 /128 之前窗口只有 1.07s，那时
               这里**确实**会被误复位）。这里仍然喂：一是留余量，二是
               "开了看门狗就必须盘点所有长阻塞路径"这条纪律 —— 窗口会变，
               别让判据跟着窗口漂。 */
            gw_fault_port_iwdg_feed();
        }
    }

    if (out_value != 0) {
        *out_value = (int32_t)f.temp_x10;
    }

    if (e != DHT22_OK) {
        return GW_SC_FAIL;
    }

    /* 合理性检查：DHT22 量程 -40~80℃、0~100%RH。
       超出量程说明时序有问题（读到的是错位的数据）。 */
    if (f.temp_x10 < -400 || f.temp_x10 > 800
        || f.humi_x10 > 1000u) {
        return GW_SC_WARN;      /* 能读出但值不合理 */
    }
    return GW_SC_PASS;
}

/* ==================================================================
 *  7) RS485 —— 自发自收（需要把 A/B 短接，或者接了回环）
 *
 *  ⚠️ 这一项的语义要说清楚：**它需要外部回环**。
 *     默认不测（返回 WARN）—— 因为没人会把 A/B 短接起来跑自检。
 *
 *     如果总线上接了一个真实的 Modbus 从站（地址 1），
 *     那它就会应答 —— 那时这一项才有意义。
 * ================================================================== */
static gw_sc_status_t chk_rs485(int32_t *out_value, const char **out_unit)
{
    if (out_unit != 0) {
        *out_unit = "";
    }
    if (out_value != 0) {
        *out_value = 0;
    }

    if (s_rs485_enable == 0) {
        return GW_SC_WARN;      /* 明确表示"没测" */
    }

    /* 只验证外设基本可用性：能否申请总线并进入/退出等待窗口。
       真正的收发验证需要上位机配合（见 doc/最小上板验证包.md 的 L4）。 */
    if (rs485_bus_acquire(RS485_ROLE_MASTER) != 0) {
        return GW_SC_FAIL;
    }
    if (rs485_bus_release_master() != 0) {
        return GW_SC_FAIL;
    }
    if (out_value != 0) {
        *out_value = 1;
    }
    return GW_SC_PASS;
}

/* ==================================================================
 *  8) CAN —— 只验证控制器能初始化到 NORMAL
 *
 *  ⚠️ 总线上没有第二个节点时，CAN 控制器会一直处于
 *     "错误主动"甚至进入 Bus-Off（因为发不出 ACK）。
 *     但我们**只收不发**，所以不会主动发 —— 复位后应该是 NORMAL。
 * ================================================================== */
static gw_sc_status_t chk_can(int32_t *out_value, const char **out_unit)
{
    gw_can_core_t *core;

    if (out_unit != 0) {
        *out_unit = "";
    }

    core = can_port_core();
    if (core == 0) {
        if (out_value != 0) {
            *out_value = 0;
        }
        return GW_SC_FAIL;
    }

    {
        int st = (int)gw_can_core_state(core);

        if (out_value != 0) {
            *out_value = st;
        }

        /* ⚠️ UNINIT = **本层从未调用 can_port_init()**（L0 就是这种情形）
         *    -> 跳过，而不是失败。理由与 chk_dht22 那段相同：
         *       自检项表各层共用，"没启用"被报成 [FAIL] 会让人去查
         *       并不存在的硬件问题。 */
        if (st == (int)GW_CAN_ST_UNINIT) {
            return GW_SC_SKIP;
        }

        /* ==================================================================
         *  本项分两问，**必须分开报**（2026-10-10 实测踩过）
         *
         *  ① 控制器配置对不对？ -> 回读 BTR（位时序）与滤波器激活位（总线无关）
         *  ② 有没有真的上总线？ -> MSR.INAK 是否已清
         *
         *  为什么要分开：bxCAN 退出初始化模式要求总线上有 **11 个连续隐性位**。
         *  实测板子上 CAN_RX(PB8) 被一直拉成显性（读数 = 0），于是
         *      HAL_CAN_Start() 超时、INAK 永远是 1 —— CAN 不可用，
         *  但**控制器与配置都是好的**（① 能过）。
         *  若只报一个总状态，就会把"收发器/总线的问题"误当成"MCU 的 CAN 坏了"，
         *  排查方向直接跑偏。
         *
         *  ⚠️ 实测补充：这种情况下**内部回环也救不了** ——
         *     回环模式下 INAK 同样清不掉（同步源仍然是 CAN_RX）。
         *     所以 ① 只能用寄存器回读来做，不能靠"发一帧收回来"。
         * ================================================================== */

        /* ---- ① 配置回读（总线无关的硬证据）---- */
        if (can_port_config_ok() == 0) {
            if (out_value != 0) {
                *out_value = 0;         /* 0 = 控制器/配置这一层就没过 */
            }
            return GW_SC_FAIL;
        }
        if (out_value != 0) {
            *out_value = 1;             /* 1 = 控制器 + 位时序 + 滤波器都写进去了 */
        }

        /* ---- ② 有没有上总线（需要总线上有 11 个连续隐性位）---- */
        if (can_port_bus_ready() == 0) {
            return GW_SC_FAIL;          /* 值=1 但仍 FAIL：问题在收发器/总线 */
        }

        /* NORMAL(1) 或 ERROR_WARN(2) 都算通过 ——
           总线上没有别的节点时 ERROR_WARN 是正常的。 */
        if (st == (int)GW_CAN_ST_NORMAL || st == (int)GW_CAN_ST_ERROR_WARN) {
            return GW_SC_PASS;
        }
        return GW_SC_FAIL;
    }
}

/* ==================================================================
 *  9) ESP32 —— 只验证串口外设初始化了，不要求对方在线
 * ================================================================== */
static gw_sc_status_t chk_esp32(int32_t *out_value, const char **out_unit)
{
    if (out_unit != 0) {
        *out_unit = "";
    }
    if (out_value != 0) {
        *out_value = 0;
    }

    /* esp32_port_tick 是空操作级别的调用（内部只是取 DMA 数据），
       所以调它不会阻塞。它没有返回值，我们用"能否调用"来判断
       外设是否初始化 —— 真正的验证需要 ESP32 发一帧过来。 */
    esp32_port_tick();

    /* 恒返回 WARN：**串口自检无法自证**。
       "初始化成功"不等于"对端在线且波特率一致"。
       真正的验证在 doc/最小上板验证包.md 的对应项。 */
    return GW_SC_WARN;
}

/* ==================================================================
 *  10) FAULT —— 用已有的 selfcheck（检查 .noinit 可写）
 * ================================================================== */
static gw_sc_status_t chk_fault(int32_t *out_value, const char **out_unit)
{
    int r;

    if (out_unit != 0) {
        *out_unit = "";
    }
    r = gw_fault_port_selfcheck();

    /* ==================================================================
     *  额外一问：**IWDG 到底配起来了没有**（2026-10-10 加）
     *
     *  为什么放在 FAULT 项里：看门狗属于故障处理机制 ——
     *  HardFault 有异常处理兜着，但"静默挂死"只有 IWDG 能兜。
     *  它是 OTA 试用回滚能生效的前提（卡死 -> 复位 -> 计数 +1 -> 回滚）。
     *
     *  ⚠️ 判据是**回读寄存器**（PR/RLR），不是读某个软件标志：
     *     配置没写进去、或者以为开了其实没开，这类"静默失效"
     *     在这块板子上已经踩过好几次（CAN 中断、滤波器、状态位）。
     *
     *  ⚠️ 值 = 1 表示真的配起来了；0 = 没开（裸机调试早期允许，
     *     但**生产配置必须为 1**，否则静默挂死不会复位）。
     * ================================================================== */
    {
        int iwdg_on = gw_fault_port_iwdg_enabled();

        if (r != 0) {
            if (out_value != 0) {
                *out_value = 0;
            }
            return GW_SC_FAIL;
        }
        if (out_value != 0) {
            *out_value = iwdg_on;
        }
        return (iwdg_on != 0) ? GW_SC_PASS : GW_SC_WARN;
    }
}

/* ==================================================================
 *  项表
 *
 *  ⚠️ 顺序必须与 `gw_sc_id_t` 一致 —— 否则报告里的名字与状态会错位。
 *     这是本文件最容易出错的地方，所以每一项都带 ID 注释。
 * ================================================================== */
static const gw_sc_item_t s_items[GW_SC_COUNT] = {
    /* GW_SC_CLOCK */
    { "CLOCK", chk_clock, GW_SC_COUNT, (int32_t)GW_SC_EXPECT_SYSCLK,
      (int32_t)GW_SC_CLOCK_TOL,
      "⚠️ 这一项软件无法自证：SystemCoreClock 只反映代码认为的配置\n"
      "验证方法：用 MCO 把时钟输出到 PA8，接示波器/频率计量\n"
      "常见原因：CubeMX 里 HSE 频率填错（8MHz 板子填了 12MHz）\n"
      "          或者 HSE 起振失败自动切到了 HSI（8MHz）" },

    /* GW_SC_GPIO */
    { "GPIO",  chk_gpio,  GW_SC_COUNT, 1, 0,
      "检查 CubeMX 里的引脚分配（DE=PA1）\n"
      "用万用表量 PA1 在自检期间是否有电平变化\n"
      "注意：本项只验证 ODR 写得进，不验证引脚真的动了" },

    /* GW_SC_DWT */
    { "DWT",   chk_dwt,   GW_SC_CLOCK, 1000, 100,
      "DWT 未使能？确认 gw_dwt_init() 在 SystemClock_Config 之后调用\n"
      "确认 CoreDebug->DEMCR 的 TRCENA 位被置起\n"
      "读数为 0 说明 CYCCNT 没在走" },

    /* GW_SC_FLASH */
    { "FLASH", chk_flash, GW_SC_CLOCK, 1, 0,
      "确认 .noinit 段的落点（见 port/gw_fault_port.h 里的实测数据）\n"
      "芯片是否被设了读保护（RDP）？那会导致写入失败\n"
      "F1 的页大小是 2KB，确认分区表用的是 2KB 对齐" },

    /* GW_SC_PARAM */
    { "PARAM", chk_param, GW_SC_FLASH, 8, 0,
      "参数区应有 8 个槽（每槽 2KB = 一页）\n"
      "确认 GW_PARAM_FLASH_ADDR 与链接脚本不冲突" },

    /* GW_SC_DHT22 */
    { "DHT22", chk_dht22, GW_SC_DWT, 1, 0,
      "PG11 接线是否正确？数据线需要 4.7k 上拉电阻\n"
      "DHT22 上电后需要约 1~2 秒稳定时间，自检要在那之后跑\n"
      "读到的值超出量程说明时序有问题（微秒延时不准）" },

    /* GW_SC_RS485 */
    { "RS485", chk_rs485, GW_SC_GPIO, 1, 0,
      "PA1(DE) / PA2(TX) / PA3(RX) 接线\n"
      "A/B 线是否接反（接反了完全收不到数据）\n"
      "本项只验证总线仲裁与外设，真正收发要上位机配合" },

    /* GW_SC_CAN */
    { "CAN",   chk_can,   GW_SC_CLOCK, 1, 0,
      "本项分两问：① 控制器配置回读（位时序 BTR + 滤波器激活位）② 有没有真的上总线（MSR.INAK）。\n"
      "若值=1 但仍 FAIL：控制器与配置是好的，问题在**收发器/总线** ——\n"
      "   bxCAN 退出初始化模式需要总线上 11 个连续隐性位，CAN_RX 被拉成显性就出不来。\n"
      "   查：模块 VCC 是否符合芯片要求（**TJA1051 系列要 5V**）、共地、"
      "RXD 静态是否为高（恒 0 = 模块没工作）、TXD 是否接到 PB9、"
      "CAN_H/L 是否接反或被拉低、120Ω 终端。\n"
      "若值=0（配置回读就不过）：查 CAN 时钟(RCC_APB1ENR.CAN1EN)、APB1 是否 36MHz、\n"
      "   滤波器是否被软件关掉、**NVIC 里 CAN1_RX0 中断有没有使能**\n"
      "   （漏了这一项的现象：帧堆在硬件 FIFO 且溢出、ISR 永不进入、收帧计数恒 0）。\n"
      "⚠️ 本项**不经过引脚**，所以即使通过也不代表收发器与接线没问题；\n"
      "   要验那两项必须有第二个节点（或外部回环夹具）。" },

    /* GW_SC_ESP32 */
    { "ESP32", chk_esp32, GW_SC_CLOCK, 1, 0,
      "USART3 的 PB10(TX) / PB11(RX) —— 注意要与 ESP32 交叉接\n"
      "两边波特率都是 115200\n"
      "⚠️ 本项恒为 WARN：串口无法自证对端在线" },

    /* GW_SC_FAULT */
    { "FAULT", chk_fault, GW_SC_FLASH, 1, 0,
      "本项查两件事：① .noinit 故障记录可写可读（跨复位）② **IWDG 是否真的配起来**。\n"
      "值=1 表示看门狗在跑（PR/RLR 已写入）；值=0（WARN）说明没开 ——\n"
      "  后果：静默挂死（死循环/死锁）不会复位 -> OTA 的试用计数不前进 -> **永不回滚**。\n"
      "验证方法（故障注入）：把 GW_L0_IWDG_FEED 改成 0 烧进去 ->\n"
      "  设备应每 ~1.6 秒复位一次，且 DEV_STATUS 的 IWDG_RESET 位（input[10] bit6）被置起来；\n"
      "  验证完必须改回 1。\n"
      "验证方法（故障记录）：跑一次 -> 按复位 -> 看 boot_count 是否累加。" }
};

int gw_selfcheck_port_init(gw_selfcheck_t *sc)
{
    return gw_selfcheck_init(sc, s_items);
}
