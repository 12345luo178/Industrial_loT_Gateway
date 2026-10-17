/**
 * @file    bootloader_main.c
 * @brief   ★ Bootloader 的 main.c —— 复制进独立的 Bootloader 工程
 *
 * ============================================================================
 *  这是一个**独立工程**，不是应用工程的一部分
 *
 *  Flash 分区（见 `port/gw_flash_map.h`，**v2**）：
 *
 *      0x08000000  32 KB   ← **本 Bootloader**（就是这份代码）
 *      0x08008000   4 KB   OTA 元数据（active / pending / boot_tries）
 *      0x08009000 136 KB   应用 A
 *      0x0802B000 136 KB   应用 B
 *      0x0804D000 136 KB   golden 出厂镜像（只读，不参与 OTA）
 *      0x08071000  44 KB   日志区
 *      0x0807C000  16 KB   参数区
 *
 *  两个工程分开编译、分开烧录。Bootloader 只需要烧一次，
 *  之后升级走 OTA，不再动它。
 *
 *  ----------------------------------------------------------------------------
 *  它做的事（就这四件）
 *
 *      1) 读元数据 -> 决定启动哪个分区（`gw_ota_boot_decide()`）
 *      2) 校验那个分区的固件（向量表 + 整包 CRC）
 *      3) 把"这次启动"记进元数据（`boot_tries` +1 / 确认）
 *      4) 跳过去（`gw_boot_port_jump()`）
 *
 *  **启动顺序**：主槽 -> 另一个槽 -> **golden 出厂镜像** -> 都没有才停在 Bootloader
 *  （停在 Bootloader 时只能靠 ST-Link 重烧 —— 这是刻意的：坏固件不该被启动）。
 *
 *  ⚠️ golden 是 2026-10-11 加的**最后一道兜底**：A/B 两个槽都校验不过时
 *     （例如某个固件 bug 写错了 Flash 地址、或某页 Flash 坏了），仍然能跑一个
 *     **已知良好**的固件，设备不至于变成"只能用 ST-Link 救"的砖。
 *     它**不是 OTA 的槽**：元数据里不可能出现它（`gw_ota_meta_valid` 只收 A/B），
 *     所以它在软件上就不可被 OTA 写。
 *     顺序（策略）在 `src/gw_boot.c` 的 `gw_boot_pick_slot()` 里，有主机测试。
 *
 *  ----------------------------------------------------------------------------
 *  ⚠️ 三个容易写错的地方，下面代码里都有注释
 *
 *  **① boot_tries 必须在跳转**之前**落盘**
 *     否则"新固件连 main() 都进不去"时会无限重试 ——
 *     每次复位都从头数，永远到不了 3 次，永远不回滚。
 *
 *  **② 确认（confirm）靠的是运行时长，不是启动次数**
 *     新固件跑够 30 秒才算"确认可用"。这在**应用**里做
 *     （它才知道自己跑了多久），Bootloader 只负责读结果。
 *
 *  **③ 跳转必须用汇编 `bx`，不能用函数指针调用**
 *     函数指针调用会先 `push {lr}` —— 而此时 MSP 已经被换成
 *     应用栈了，那个 push 会写进应用的栈空间，
 *     把应用的数据踩坏（症状是"跳过去之后随机崩溃"）。
 *     这个细节已经封装在 `gw_boot_port_jump()` 里。
 * ============================================================================
 */

#include "main.h"               /* Bootloader 工程自己的 CubeMX 生成物 */
#include "gw_boot.h"
#include "gw_boot_port.h"      /* gw_boot_port_io / jump / dump_plan */
#include "gw_ota_port.h"       /* gw_ota_port_io()：元数据读写 */
#include "gw_ota.h"
#include "gw_flash_map.h"
#include "gw_fault_port.h"     /* gw_fault_port_iwdg_feed：IWDG 跨复位继续跑，必须一起来就喂 */

/* ⚠️ CubeMX 把下面这几个函数的原型放在**它自己生成的** main.c / 外设头里。
 *    独立构建（`tools/build_ota_images.ps1`，只编本文件 + bootloader_glue.c）
 *    时没人声明它们 -> `implicit declaration` 告警，且返回值按 int 处理。
 *    照 L0 模板的做法显式声明一份 —— 这样两种构建方式都干净。 */
extern void SystemClock_Config(void);
extern void MX_GPIO_Init(void);
extern void MX_USART1_UART_Init(void);
#include <stdio.h>
#include <string.h>

/* ==================================================================
 *  调试串口（Bootloader 阶段也要能看东西）
 *
 *  ⚠️ Bootloader 里的 printf 必须用**轮询**发送且超时很短。
 *     中断还没配好，DMA 更没起来。见下面的 boot_puts()。
 * ================================================================== */
extern UART_HandleTypeDef huart1;

static void boot_puts(const char *s)
{
    if (s == 0) {
        return;
    }
    /* 超时 100ms：没接串口时不能卡在这里。
       Bootloader 的职责是"尽快启动应用"，
       不能因为调试口没接就让设备晚启动几百毫秒。 */
    (void)HAL_UART_Transmit(&huart1, (uint8_t *)s,
                            (uint16_t)strlen(s), 100u);
}

/* ==================================================================
 *  探测一个分区能不能启动（**不跳转**）
 *
 *  ⚠️ 为什么改成"探测 -> 挑一个 -> 再跳"三步（2026-10-11 加 golden 时）
 *
 *     启动顺序（主槽 -> 另一槽 -> golden）是一条**策略**，
 *     而策略最容易写错、又最难验：模板里的代码在 PC 上跑不了，
 *     所以"golden 到底有没有被试到"只能上板拔线试。
 *
 *     把它抽到 `src/gw_boot.c` 的 `gw_boot_pick_slot()` 之后，
 *     用假 probe 就能在主机上穷举组合（见 test_boot.c）。
 *     本函数只负责"把一个槽的探测结果如实告诉它"。
 * ================================================================== */
typedef struct {
    const gw_ota_meta_t *meta;      /* 取 image_len / image_crc 用 */
    gw_boot_plan_t       plan;      /* **探测成功那个槽**的启动计划 */
    gw_ota_slot_t        slot;      /* 探测成功的槽 */
    int                  last_err;  /* 最近一次失败的错误码（打印用） */
} boot_probe_ctx_t;

/**
 * @param ctx   boot_probe_ctx_t
 * @param slot  要探测的分区
 * @return 0 = 可启动（plan 已填好）；非 0 = 不可启动
 *
 * ⚠️ `check_crc` 是**传给 `gw_boot_prepare()` 的参数**，
 *    不是在它之后再单独调一次 `gw_boot_check_crc()`。
 *    prepare 内部会按这个标志决定做不做 CRC，并把结果写进
 *    `plan.verified` —— 一次调用搞定，不用自己编排顺序。
 */
static int probe_slot(void *ctx, gw_ota_slot_t slot)
{
    boot_probe_ctx_t   *c  = (boot_probe_ctx_t *)ctx;
    const gw_boot_io_t *io = gw_boot_port_io();
    gw_boot_err_t       e;
    char                buf[96];
    int                 n;
    int                 do_crc;

    /* 只有"待确认的新固件"才做整包 CRC：其余情况没有可信的长度/CRC 依据
       （元数据里那对值描述的是 pending 那个镜像，不是别的槽）。 */
    do_crc = (c->meta != 0
              && c->meta->pending != 0u
              && (gw_ota_slot_t)c->meta->active == slot) ? 1 : 0;

    /* ---- 准备启动计划：读向量表、判 SP / PC 合法性、（可选）整包 CRC ---- */
    e = gw_boot_prepare(io, slot, c->meta, do_crc, &c->plan);
    if (e != GW_BOOT_OK) {
        n = snprintf(buf, sizeof(buf), "[BOOT] %s prepare failed: %s\r\n",
                     gw_ota_slot_str(slot), gw_boot_err_str(e));
        if (n > 0) { boot_puts(buf); }
        c->last_err = -(int)e;
        return -1;
    }

    /* ⚠️ 校验顺序说明（prepare 内部已保证，这里只是复核）：
           SP 范围 -> SP 对齐 -> PC 范围 -> Thumb 位 -> 整包 CRC
           **栈顶指针必须最先查** —— 它是唯一一个"错了会让跳转本身
           就崩掉"的字段（MSP 被设成非法值，第一条指令取指就硬件错误）。 */

    /* 复核 CRC 真的做了（防止 check_crc 参数被误传成 0） */
    if (do_crc != 0 && c->plan.verified == 0u) {
        /* image_len 为 0 时 prepare 会跳过 CRC 但返回 OK。
           那不是错误（首次烧录、元数据未建立），
           但**要如实说出来** —— 否则会误以为"校验过了"。 */
        boot_puts("[BOOT] note: CRC skipped (no length in metadata)\r\n");
    }

    c->slot = slot;
    return 0;
}

/* ==================================================================
 *  按顺序挑一个能启动的槽并跳过去；都起不来就停在 Bootloader 等 ST-Link
 *
 *  ⚠️ 抽成函数是为了让"元数据不可信"那条路径也能用**同一套顺序**：
 *     以前那条路径只试应用 A（A 坏了就停住），现在 A -> B -> golden
 *     全都试一遍 —— 而"元数据都坏了"恰恰是最需要 golden 兜底的场景
 *     （说明 Flash 已经被什么东西写乱过）。
 *
 *  @param target 元数据决定的主槽（A / B；元数据不可信时传 A）
 *  @param m      元数据；**不可信时传 NULL**（probe 据此不做整包 CRC）
 * ================================================================== */
static void boot_or_die(gw_ota_slot_t target, const gw_ota_meta_t *m)
{
    boot_probe_ctx_t pc;
    gw_ota_slot_t    chosen;
    char             buf[96];
    int              n;

    pc.meta     = m;
    pc.slot     = GW_OTA_SLOT_NONE;
    pc.last_err = 0;

    chosen = gw_boot_pick_slot(target, probe_slot, &pc);

    if (chosen == GW_OTA_SLOT_NONE) {
        boot_puts("[BOOT] FATAL: no working image (A/B/golden). "
                  "Waiting for ST-Link.\r\n");
        for (;;) {
            /* ⚠️ 这里不加看门狗复位。
               原因：复位之后还是同样的结果，会变成"无限重启" ——
               那样连 ST-Link 都很难连上（要在复位窗口里抢时机）。
               停住不动反而更好救。 */
        }
    }

    if (pc.last_err != 0) {
        /* 主槽或另一槽失败过 -> 明确说清这次是从哪个槽兜起来的 */
        n = snprintf(buf, sizeof(buf), "[BOOT] fallback -> booting %s\r\n",
                     gw_ota_slot_str(chosen));
        if (n > 0) { boot_puts(buf); }
    }

    gw_boot_port_dump_plan(&pc.plan);

    /* ---- 跳转（不返回） ---- */
    gw_boot_port_jump(&pc.plan);

    /* 走到这里说明跳转失败（理论上是死循环，不会返回） */
    boot_puts("[BOOT] FATAL: jump returned\r\n");
    for (;;) {
    }
}

/* ==================================================================
 *  main
 * ================================================================== */
int main(void)
{
    /* ⚠️ 两个接口**不通用**，别混：
     *      gw_boot_io_t  只有 read      —— Bootloader 只读，不写 Flash
     *      gw_ota_io_t   有 erase/write/verify + 元数据读写
     *
     *    元数据的读写要用 `gw_ota_port_io()`：
     *    它带着 meta_read/meta_write。用 boot 的接口会编译失败
     *    （我在类型检查时才发现的）。 */
    /* probe_slot() 内部自己取 gw_boot_port_io()，这里只需要元数据接口 */
    const gw_ota_io_t  *meta_io = gw_ota_port_io();
    gw_ota_meta_t       meta;
    gw_ota_slot_t       target;
    const char         *reason;
    char                buf[128];
    int                 n;
    int                 meta_saved = 0;     /* 1=写过Flash 0=没写 -1=写失败 */

    /* ---- 最小初始化 ----
       ⚠️ Bootloader **不要**配一堆外设：
          时钟够用就行（HSI 8MHz 也能跑，跳转前应用会重配），
          外设越少，出错的机会越少，启动也越快。 */
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USART1_UART_Init();

    boot_puts("\r\n[BOOT] start\r\n");

    /* ==================================================================
     *  ⚠️⚠️ 立刻喂一次狗（2026-10-10 做槽位化时踩到的竞态）
     *
     *  IWDG **一旦启动就跨复位继续计数**（只有上电复位才清零）——
     *  这是 F1 参考手册里的行为，也正是它能兜住"静默挂死"的原因。
     *  但它带来一个陷阱：
     *
     *      应用启动了 IWDG（1.6s）-> 被软复位 -> 引导程序接手
     *      如果复位那一刻 IWDG 已经数到接近 0，引导程序就只剩几十毫秒，
     *      而它**第一次喂狗**是 ota_feed()，发生在"擦除/写入元数据之后"
     *      -> 于是被看门狗在**擦页中途**复位，留下一个"擦干净但没写"的
     *         元数据页（实测症状：0x08008800 全 FF、记录丢了）。
     *
     *  所以引导程序必须**一起来就喂**：不看自己有没有初始化过看门狗，
     *  只如实回读寄存器判断"它在不在跑"（gw_fault_port_iwdg_enabled 就是干这个的）。
     *  ⚠️ 冷启动时 IWDG 还没被使能，这一次喂狗是空操作 —— 两种情形都安全。
     * ================================================================== */
    gw_fault_port_iwdg_feed();

    /* ---- 读元数据 ---- */
    if (gw_ota_meta_load(meta_io, &meta) != 0
        || gw_ota_meta_valid(&meta) != 1) {
        /* ⚠️ 元数据不可信（首次烧录、或被破坏）：
           退化为"从应用 A 起试"。
           这是唯一一处"不看元数据就启动"的地方 ——
           没有它，元数据一坏设备就彻底不启动了（只能重烧），
           而那对现场设备是不可接受的。

           ⚠️ 2026-10-11（加 golden 时）：这条路径也走**同一套启动顺序**
              （A -> B -> golden），不再只有"启动 A"一条签 ——
              而"元数据都坏了"恰恰是最需要出厂镜像兜底的场景。 */
        boot_puts("[BOOT] meta invalid -> try A/B/golden\r\n");
        boot_or_die(GW_OTA_SLOT_A, 0);      /* 不传元数据 -> 不做整包 CRC */
    }

    /* ---- 决策：启动哪个分区 + **只在必要时**落盘 ----
     *
     * ⚠️ 必须用 gw_ota_boot_persist()，不要自己拼 decide + save：
     *    `gw_ota_meta_save()` 的代价是**擦一整页**（一次写 = 擦 2 KB
     *    + 写 32 字节），而 Flash 一页寿命约 1 万次。
     *    常态启动（pending = 0，老固件已确认）时 decide 一个字段都不改，
     *    这时再擦一次纯属浪费 —— 正常设备看不出来，但**复位循环**
     *    （电源抖动 / 看门狗超时 / 反复 HardFault）1 次/秒是 86400 次/天，
     *    **约 3 小时就能磨穿这一页**。
     *
     *    gw_ota_boot_persist() 内部判断"决策前后有没有变"：
     *      没变 -> 不碰 Flash（普通重启 0 次擦写）
     *      变了 -> 落盘（试用计数 +1 或回滚，每次升级 ≤ 4 次）
     *
     * ⚠️ 计数仍然必须在跳转**之前**落盘（这是它生效的前提）：否则
     *    "新固件连 main() 都进不去"时，复位后计数又从 0 开始算，
     *    永远到不了上限，永远不回滚。persist 在返回前就写完了。 */
    target = gw_ota_boot_persist(meta_io, &meta, &meta_saved);

    /* ⚠️⚠️ 必须在跳转**之前**落盘。
       否则"新固件连 main() 都进不去"的场景下：
         复位 -> boot_tries 从 0 数到 1 -> 跳转 -> 崩溃 -> 复位
         复位后元数据里还是 0（从没保存过）-> 又是 1 -> 又是崩溃
       **永远到不了 3 次，永远不回滚。** */
    if (meta_saved > 0) {
        boot_puts("[BOOT] meta updated (trial counter / rollback)\r\n");
    } else if (meta_saved == 0) {
        /* 常态；这一行也顺便是"本次启动没有额外磨损"的证据 */
        boot_puts("[BOOT] meta unchanged -> no flash write\r\n");
    } else {
        /* 保存失败不阻止启动 —— 但"回滚计数"这次就靠不住了，
           所以接下来**不做 CRC 校验**（宁可多启动一次，
           也不要因为一个写不进去的元数据把设备卡死在 Bootloader）。 */
        boot_puts("[BOOT] WARN: meta save failed, rollback disabled\r\n");
    }

    /* ---- 挑一个能启动的槽并跳过去：主槽 -> 另一槽 -> golden 出厂镜像 ----
     *
     * ⚠️ 顺序（策略）在 src/gw_boot.c 的 gw_boot_pick_slot() 里，
     *    plan 的填充由 probe_slot() 完成 ——
     *    这样"golden 到底有没有被试到"可以在主机上穷举，而不是只能上板拔线。 */
    if (target == GW_OTA_SLOT_NONE) {
        /* 元数据判定"没有可启动的槽"（两槽都坏，或回滚目标也坏）：
           仍然要试 golden —— 这正是它存在的意义。 */
        boot_puts("[BOOT] meta says no bootable slot -> try golden\r\n");
    } else {
        /* 判断这次是不是"待确认的新固件"（是的话要做整包 CRC） */
        reason = (meta.pending != 0u && target == meta.active)
                 ? "pending (verifying)"
                 : "confirmed";

        n = snprintf(buf, sizeof(buf), "[BOOT] target=%s tries=%u/%u %s\r\n",
                     gw_ota_slot_str(target),
                     (unsigned)meta.boot_tries,
                     (unsigned)GW_OTA_MAX_BOOT_TRIES,
                     reason);
        if (n > 0) { boot_puts(buf); }
    }

    boot_or_die(target, &meta);         /* 不返回 */
}
