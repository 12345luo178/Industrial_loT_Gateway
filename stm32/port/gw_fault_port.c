/**
 * @file    gw_fault_port.c
 * @brief   故障捕获硬件层实现（.noinit + HardFault + 安全模式）
 */
#include "gw_fault_port.h"

/* 注意包含顺序：gw_config.h 使用 HAL 的 GPIO 宏，必须先包含 HAL 头。 */
#include "stm32f1xx_hal.h"

#include <string.h>

/* ==================================================================
 *  .noinit 段：跨复位存活的记录
 *
 *  `__attribute__((section(".noinit")))` 让链接器把它放进 .noinit 段，
 *  启动代码不会初始化它。
 *
 *  ⚠️ 两个实测得出的要点（见头文件里的完整说明）：
 *
 *    1) **通常不需要**手工改链接脚本 —— ld 会把 .noinit 当 orphan
 *       section 自动创建，并放在 .bss 之外。实测两种写法下它都不在
 *       _sbss.._ebss 区间内，所以不会被启动代码清零。
 *
 *    2) `.noinit` 里的变量**不能有非零初始化器** —— ARM GCC 会报
 *       "only zero initializers are allowed in section '.noinit'"。
 *       所以下面 `s_rec` 刻意不写初值，magic 由 gw_fault_boot_init()
 *       在运行期填。
 * ================================================================== */
__attribute__((section(".noinit")))
static gw_fault_rec_t s_rec;

/* 本次运行的故障现场（普通 RAM 即可，不需要跨复位） */
static gw_fault_ctx_t s_ctx;

static gw_boot_cause_t  s_boot_cause = GW_BOOT_UNKNOWN;
static gw_safe_reason_t s_safe       = GW_SAFE_NONE;
static uint32_t         s_uptime_ms  = 0u;
static uint32_t         s_tick_acc   = 0u;

/* ==================================================================
 *  安全模式策略
 * ================================================================== */
static gw_safe_mode_t safe_mode_of(gw_safe_reason_t r)
{
    gw_safe_mode_t m;

    if (r == GW_SAFE_NONE) {
        m.allow_dht    = 1u;
        m.allow_can    = 1u;
        m.allow_master = 1u;
        return m;
    }

    /* 安全模式：只留通信，关掉所有"可能引发崩溃"的活动。
       详见头文件里的说明。 */
    m.allow_dht    = 0u;
    m.allow_can    = 0u;
    m.allow_master = 0u;
    return m;
}

gw_safe_mode_t gw_fault_port_safe_mode(void)
{
    return safe_mode_of(s_safe);
}

/* ==================================================================
 *  初始化
 * ================================================================== */

int gw_fault_port_selfcheck(void)
{
    /* 往记录里写哨兵再读回：验证"这块内存可写、且不是只读段"。
       查不出"是否被启动代码清零"——那需要跨一次真实复位。 */
    uint32_t saved_magic = s_rec.magic;

    s_rec.magic = 0x5A5A5A5Au;
    if (s_rec.magic != 0x5A5A5A5Au) {
        s_rec.magic = saved_magic;
        return -1;
    }
    s_rec.magic = saved_magic;
    return 0;
}

gw_boot_cause_t gw_fault_port_init(void)
{
    uint32_t csr;

    /* ⚠️ 顺序很重要：先自检，再 boot_init。
       自检会临时改 magic，所以它必须在 boot_init 之前完成，
       否则会把一个被人为改过的 magic 交给 boot_init 判定。 */
    (void)gw_fault_port_selfcheck();

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_uptime_ms = 0u;
    s_tick_acc  = 0u;

    s_boot_cause = gw_fault_boot_init(&s_rec);
    s_safe       = gw_fault_need_safe_mode(&s_rec);

    /* ==================================================================
     *  ⚠️ 看门狗复位必须靠**硬件标志**识别，不能靠 .noinit 记录
     *     （2026-10-10 加 IWDG 时补的）
     *
     *  gw_fault_boot_init() 判启动原因靠的是 .noinit 里"上一次留下的故障记录"。
     *  这对 HardFault 有效 —— 故障处理程序有机会写记录再复位。
     *  但**看门狗复位恰恰是"程序已经卡死、没机会写任何东西"**那种情况：
     *      卡死 -> IWDG 数到 0 -> 硬件直接复位 -> .noinit 里什么新信息都没有
     *  于是原因会被误判成"软复位"，OTA 的试用计数虽然仍会前进（那是固件里
     *  的另一条链），但**上位机永远看不到"这次是看门狗复位"**。
     *
     *  RCC_CSR 里的复位标志由硬件置位、且**跨复位保留**，直到写 RMVF 清除。
     *  所以这里先读、再清，用它覆盖/补正启动原因。
     * ================================================================== */
    csr = RCC->CSR;

    if ((csr & RCC_CSR_IWDGRSTF) != 0u) {
        s_boot_cause = GW_BOOT_IWDG;
    } else if ((csr & RCC_CSR_WWDGRSTF) != 0u) {
        s_boot_cause = GW_BOOT_IWDG;        /* 窗口看门狗也归到"看门狗复位" */
    } else if ((csr & RCC_CSR_SFTRSTF) != 0u) {
        s_boot_cause = GW_BOOT_SOFTWARE;    /* 软件复位（含 OTA 重启） */
    } else if ((csr & RCC_CSR_PORRSTF) != 0u
               || (csr & RCC_CSR_PINRSTF) != 0u) {
        s_boot_cause = GW_BOOT_POWER_ON;    /* 上电 / 复位引脚 */
    }
    /* 其余情况保留 .noinit 推出来的结论（例如 HardFault 记录） */

    /* 清标志：不清的话下次启动还会看到同一批标志，误判成"又复位了"。
       ⚠️ 清除动作放在读之后、且只写 RMVF 一位。 */
    RCC->CSR |= RCC_CSR_RMVF;

    return s_boot_cause;
}

const gw_fault_rec_t *gw_fault_port_record(void)
{
    return &s_rec;
}

const gw_fault_ctx_t *gw_fault_port_context(void)
{
    return &s_ctx;
}

gw_safe_reason_t gw_fault_port_safe_reason(void)
{
    return s_safe;
}

void gw_fault_port_tick(uint32_t elapsed_ms)
{
    s_uptime_ms += elapsed_ms;

    /* 秒计数：满 1000 进位。
       用累加器而不是每次除以 1000，避免在 10ms 节拍下频繁做除法。 */
    s_tick_acc += elapsed_ms;
    while (s_tick_acc >= 1000u) {
        s_tick_acc -= 1000u;
    }

    /* ⚠️ 第一阶段**不喂狗也不使能 IWDG**。
     *
     * 为什么现在不开：
     *   裸机版有几处会长时间阻塞的路径 ——
     *     Flash 擦除（F1 一页约 20~40ms）
     *     DHT22 单总线时序（采样期间要关中断约 5ms）
     *     串口发送（115200 下发 133 字节约 12ms）
     *   如果看门狗超时设得比这些短，会**在正常操作时误复位**；
     *   设得比它们长，又失去了"卡死检测"的意义。
     *
     * 正确做法是等 FreeRTOS 化之后：
     *   把喂狗放在优先级最低的"空闲任务钩子"里，
     *   这样任何一个任务卡死都会导致喂不上狗 —— 那才是可靠的卡死检测。
     *
     * 在那之前，本函数只维护运行时长，供上层判断"稳定运行了多久"。 */
}

uint32_t gw_fault_port_uptime_s(void)
{
    return s_uptime_ms / 1000u;
}

int gw_fault_port_iwdg_enabled(void)
{
    /* ==================================================================
     *  ⚠️ 2026-10-10 起这里**真的去读寄存器**，不再硬编码 0
     *
     *  原来固定返回 0（"本层没开狗"），于是所有依赖它的地方都当"没开狗"处理：
     *      gw_ota_port 的擦除/编程**不喂狗**（长擦除会喂不上）
     *      自检也无从确认看门狗到底配起来没有
     *  现在 IWDG 真开了，就必须如实回答。
     * ==================================================================
     *  ⚠️⚠️ 2026-10-11 修正判据（缺陷 O，板上 39Hz 复位循环的根因）
     *
     *  原来的判据写的是"PR/RLR 任一非 0 就算已经跑起来"，注释里还写着
     *  "未初始化时这两个寄存器都是 0" —— **这句是错的**：
     *
     *      IWDG_PR  复位默认值 = 0x000
     *      IWDG_RLR 复位默认值 = 0xFFF   ← 一上电就是非 0
     *
     *  于是冷启动时判据恒为"已使能"，把引导程序送进了
     *      HAL_IWDG_Refresh(&hiwdg)   // 引导程序里 hiwdg.Instance == NULL
     *  = 写地址 0x00000000 -> 不精确总线错误 -> HardFault -> 复位 -> 死循环。
     *
     *  现在判据下沉成纯逻辑的 `gw_fault_iwdg_is_enabled()`（在 gw_fault.c），
     *  按**真实复位默认值**比较 —— 顺带让它变成可主机测试的函数
     *  （本文件在 port/ 下，主机测试不编译，所以原来这条判据一次都没被测过）。
     * ================================================================== */
    return gw_fault_iwdg_is_enabled(IWDG->PR, IWDG->RLR);
}

void gw_fault_port_iwdg_feed(void)
{
    /* ==================================================================
     *  ⚠️⚠️ 2026-10-11：喂狗**不再依赖"开没开"的判断**（缺陷 O）
     *
     *  原来这里先判 `gw_fault_port_iwdg_enabled()`、判到"开"才写 KR。
     *  那条判据一旦答错（冷启动时就会答错，见上面），喂狗本身不会出事
     *  （写 KR 是安全的），**但它把"判据必须正确"变成了喂狗的前提** ——
     *  而判据只用于统计就够了。
     *
     *  所以现在：**无条件写 KR**。
     *      - 狗开着：正常喂 ✓
     *      - 狗没开：写 0xAAAA 是无害空操作（IWDG 只有写 0xCCCC 才启动）✓
     *  这条性质由芯片手册保证，不依赖任何回读，因此不可能判错。
     * ================================================================== */
    IWDG->KR = 0x0000AAAAu;     /* 喂狗：写 0xAAAA */
}

void gw_fault_port_reset(uint32_t delay_ms)
{
    uint32_t start = HAL_GetTick();

    /* 登记"这是有意重启"：不清这个标记的话，
       重启后会被当成故障启动，连续几次就误进安全模式 ——
       而 OTA 重启恰恰是最需要"不被误判"的场景。 */
    gw_fault_note_intentional_reset(&s_rec);

    /* 等一会儿让串口把日志吐完。忙等而不是 HAL_Delay：
       某些调用场景（中断未开）下 HAL_Delay 会死等。 */
    while ((HAL_GetTick() - start) < delay_ms) {
        /* 空转 */
    }

    /* 走 CMSIS 的软复位：比直接写 AIRCR 更可移植 */
    NVIC_SystemReset();
}

/* ==================================================================
 *  HardFault 处理程序
 *
 *  ⚠️ 这个函数要遵守故障上下文的纪律：
 *     - 不调用任何可能阻塞的东西（串口发送、Flash 操作、日志）
 *     - 不依赖栈的完整性（栈可能已经溢出）
 *     - 不返回（返回会再次触发故障）
 *
 *  它只做三件事：抓现场 -> 写 .noinit 记录 -> 复位。
 * ================================================================== */

/**
 * 从异常栈帧里取现场
 *
 * 原理：Cortex-M 进入异常时会自动把 8 个寄存器压栈，
 * 压栈顺序固定为 R0, R1, R2, R3, R12, LR, PC, xPSR。
 * 所以拿到当时的 SP，就能按固定偏移读出故障 PC。
 *
 * **这是唯一可靠拿到故障 PC 的方法** ——
 * 故障发生时 `LR` 里是 EXC_RETURN 而不是调用者地址，
 * 直接读 LR 会得到 0xFFFFFFF9 之类的值，定位不到任何地方。
 */
static void capture_from_stack(uint32_t sp, gw_fault_ctx_t *ctx)
{
    const uint32_t *frame = (const uint32_t *)sp;

    ctx->pc  = frame[6];
    ctx->lr  = frame[5];
    ctx->psr = frame[7];
    ctx->sp  = sp;
}

__attribute__((naked)) void HardFault_Handler(void)
{
    /* naked 函数里只有内联汇编，不生成函数序言/尾声 ——
       这样我们才能拿到**故障发生时**真实的 SP，
       而不是被编译器压栈后的 SP。 */
    __asm volatile
    (
        /* 判断故障前用的是 MSP 还是 PSP：
           EXC_RETURN 的 bit2 = 0 表示用的是 MSP，= 1 表示 PSP。
           FreeRTOS 下任务用的是 PSP，所以这一步必须有。 */
        "tst    lr, #4              \n"
        "ite    eq                  \n"
        "mrseq  r0, msp             \n"
        "mrsne  r0, psp             \n"
        "b      hardfault_report    \n"
    );
}

/* 普通 C 函数：由上面的 naked 处理程序跳转过来，参数是已取好的 SP */
void hardfault_report(uint32_t sp)
{
    /* 抓现场 */
    capture_from_stack(sp, &s_ctx);
    s_ctx.cfsr = SCB->CFSR;
    s_ctx.hfsr = SCB->HFSR;
    s_ctx.mmfar = SCB->MMFAR;
    s_ctx.bfar = SCB->BFAR;
    s_ctx.kind = (uint8_t)((SCB->HFSR & SCB_HFSR_FORCED_Msk) != 0u
                           ? GW_FAULT_HARD : GW_FAULT_HARD);
    s_ctx.valid = 1u;

    /* 写持久化记录（跨复位存活） */
    gw_fault_record(&s_rec, &s_ctx);

    /* 复位。

       ⚠️ 这里刻意**不尝试返回到故障指令**（有些实现会做"跳过故障指令"的
          恢复尝试）。理由：那条指令既然崩了，再执行大概率还是崩，
          而且可能已经破坏了外设状态。**复位是更安全的选择** ——
          至少能回到一个已知的干净状态，而且崩溃现场已经存下来了。

       ⚠️ 也刻意不在这里打印日志：串口发送可能阻塞（没对端时会等超时），
          而故障上下文里等不起。现场已经存在 .noinit 里，
          下次启动时再打印。 */
    NVIC_SystemReset();
}

/* ==================================================================
 *  其它异常处理程序
 *
 *  F1 上 MemManage / BusFault / UsageFault 默认是关闭的，
 *  所有故障都会升级成 HardFault。这里显式打开它们，
 *  好处是**能区分故障类型** —— 日志里写 "BUSFAULT" 比 "HARDFAULT"
 *  对定位根因帮助大得多（前者说明是非法地址访问）。
 * ================================================================== */

/** 打开细分故障，让 HFSR 里能看出真实原因 */
void gw_fault_enable_detailed(void)
{
    SCB->SHCSR |= SCB_SHCSR_USGFAULTENA_Msk
                | SCB_SHCSR_BUSFAULTENA_Msk
                | SCB_SHCSR_MEMFAULTENA_Msk;
}

__attribute__((naked)) void MemManage_Handler(void)
{
    __asm volatile
    (
        "tst    lr, #4              \n"
        "ite    eq                  \n"
        "mrseq  r0, msp             \n"
        "mrsne  r0, psp             \n"
        "b      memmanage_report    \n"
    );
}

void memmanage_report(uint32_t sp)
{
    capture_from_stack(sp, &s_ctx);
    s_ctx.cfsr  = SCB->CFSR;
    s_ctx.hfsr  = SCB->HFSR;
    s_ctx.mmfar = SCB->MMFAR;
    s_ctx.bfar  = SCB->BFAR;
    s_ctx.kind  = (uint8_t)GW_FAULT_MEMMANAGE;
    s_ctx.valid = 1u;

    gw_fault_record(&s_rec, &s_ctx);
    NVIC_SystemReset();
}

__attribute__((naked)) void BusFault_Handler(void)
{
    __asm volatile
    (
        "tst    lr, #4              \n"
        "ite    eq                  \n"
        "mrseq  r0, msp             \n"
        "mrsne  r0, psp             \n"
        "b      busfault_report     \n"
    );
}

void busfault_report(uint32_t sp)
{
    capture_from_stack(sp, &s_ctx);
    s_ctx.cfsr  = SCB->CFSR;
    s_ctx.hfsr  = SCB->HFSR;
    s_ctx.mmfar = SCB->MMFAR;
    s_ctx.bfar  = SCB->BFAR;
    s_ctx.kind  = (uint8_t)GW_FAULT_BUSFAULT;
    s_ctx.valid = 1u;

    gw_fault_record(&s_rec, &s_ctx);
    NVIC_SystemReset();
}

__attribute__((naked)) void UsageFault_Handler(void)
{
    __asm volatile
    (
        "tst    lr, #4              \n"
        "ite    eq                  \n"
        "mrseq  r0, msp             \n"
        "mrsne  r0, psp             \n"
        "b      usagefault_report   \n"
    );
}

void usagefault_report(uint32_t sp)
{
    capture_from_stack(sp, &s_ctx);
    s_ctx.cfsr  = SCB->CFSR;
    s_ctx.hfsr  = SCB->HFSR;
    s_ctx.kind  = (uint8_t)GW_FAULT_USAGEFAULT;
    s_ctx.valid = 1u;

    gw_fault_record(&s_rec, &s_ctx);
    NVIC_SystemReset();
}
