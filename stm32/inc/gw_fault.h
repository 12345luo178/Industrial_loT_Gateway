/**
 * @file    gw_fault.h
 * @brief   故障捕获与"故障升级"策略（纯逻辑部分）
 *
 * ============================================================================
 *  这个模块解决一个具体而危险的问题：**启动循环**
 *
 *  一个带看门狗的设备，如果固件某处必然崩溃：
 *
 *      上电 -> 崩溃 -> 看门狗复位 -> 上电 -> 崩溃 -> 看门狗复位 -> ...
 *
 *  表现是"设备一直在重启，完全没法用，也没法通过 OTA 修复"——
 *  因为 OTA 需要设备能稳定运行几秒钟。
 *  现场只能拆机接调试器，代价极高。
 *
 *  ----------------------------------------------------------------------------
 *  本模块的策略：**同一签名连续故障 -> 升级 -> 进安全模式**
 *
 *    第 1 次故障  ->  记录现场，正常重启，尝试恢复
 *    第 2 次同签名 ->  仍然重启（可能是偶发，给一次机会）
 *    第 3 次同签名 ->  **进入安全模式**：只跑最小功能（通信 + OTA），
 *                     不做任何可能崩溃的初始化
 *
 *  为什么"同签名"而不是"任何故障都计数"：
 *    如果两次崩溃的原因不同（例如第一次是 CAN 干扰，第二次是 Flash 写失败），
 *    那说明设备没有系统性问题，重启是合理的处理。
 *    只有**同一个地方反复崩**才说明"这条路走不通"。
 *
 *  签名怎么算：`PC + 故障类型`。同一个 PC 崩同一个异常 -> 同签名。
 *
 *  ----------------------------------------------------------------------------
 *  为什么故障记录要放"不掉电的 RAM"
 *
 *  复位会清 RAM，所以普通变量存不住"上次怎么崩的"。
 *  解决办法是把记录放在 **`.noinit` 段**：
 *  启动文件不会清零这一段，复位后内容还在。
 *
 *  代价是上电冷启动时内容是随机的 —— 所以必须有魔数校验，
 *  否则会把随机值当成"上次故障记录"，从而误判成崩溃循环。
 *  这个校验是最关键的一步，测试里专门覆盖。
 * ============================================================================
 */
#ifndef GW_FAULT_H
#define GW_FAULT_H

#include <stdint.h>

/* ---------------- 故障类型 ----------------
 *
 * 与 ARM Cortex-M3 的异常编号对应（低 4 位保留 0，便于人工识别）。
 */
typedef enum {
    GW_FAULT_NONE      = 0x00,
    GW_FAULT_HARD      = 0x10,      /* HardFault */
    GW_FAULT_MEMMANAGE = 0x20,      /* MemManage（F1 无 MPU，保留） */
    GW_FAULT_BUSFAULT  = 0x30,      /* BusFault */
    GW_FAULT_USAGEFAULT = 0x40,     /* UsageFault */
    GW_FAULT_ASSERT    = 0x50,      /* 断言失败（自定义） */
    GW_FAULT_STACK     = 0x60,      /* 栈溢出检测（自定义） */
    GW_FAULT_WWDG      = 0x70,      /* 窗口看门狗（保留） */
    GW_FAULT_UNKNOWN   = 0xF0
} gw_fault_kind_t;

/* ---------------- 启动原因 ---------------- */
typedef enum {
    GW_BOOT_POWER_ON   = 0,     /* 冷启动（上电/复位引脚） */
    GW_BOOT_IWDG       = 1,     /* 独立看门狗复位 */
    GW_BOOT_SOFTWARE   = 2,     /* 软件主动复位（例如收到重启指令） */
    GW_BOOT_FAULT      = 3,     /* 由故障处理程序触发的复位 */
    GW_BOOT_UNKNOWN    = 4
} gw_boot_cause_t;

/* ---------------- 安全模式原因 ---------------- */
typedef enum {
    GW_SAFE_NONE        = 0,    /* 正常运行 */
    GW_SAFE_FAULT_LOOP  = 1     /* 同一签名连续故障，进入安全模式 */
} gw_safe_reason_t;

/* ---------------- 连续故障判定阈值 ---------------- */
#define GW_FAULT_ESCALATE_AT    3u  /* 同签名累计到第 3 次就进安全模式 */

/* ---------------- 环形记录深度 ----------------
 * 记 4 次：够看出"崩在同一个地方"还是"到处都崩"，
 * 又不至于占太多 .noinit（每次 16 字节，共 64 字节）。
 */
#define GW_FAULT_HISTORY        4u

/* ---------------- 持久化记录（放在 .noinit 段） ----------------
 *
 * ⚠️ 字段顺序与大小是**跨复位契约**，改动会让旧记录失效。
 *    改结构体时应同时提升 GW_FAULT_MAGIC 的版本位。
 */
#define GW_FAULT_MAGIC          0xF4170001u

typedef struct {
    uint32_t magic;                 /* GW_FAULT_MAGIC（含版本） */
    uint32_t boot_count;            /* 累计启动次数（用于识别"刚上电"） */
    uint8_t  last_kind;             /* 上次故障类型 */
    uint8_t  last_boot;             /* 上次启动原因 */
    uint16_t consec;                /* 同签名连续故障次数 */
    uint32_t last_pc;               /* 上次故障时的 PC */
    uint32_t last_lr;               /* 上次故障时的 LR */
    uint32_t last_psr;              /* 上次故障时的 xPSR */
    uint32_t last_cfsr;             /* 可配置故障状态寄存器 */
    uint32_t last_hfsr;             /* 硬故障状态寄存器 */

    /* 历史：每条 8 字节（kind + pc），按时间从旧到新 */
    struct {
        uint8_t  kind;
        uint8_t  pad[3];
        uint32_t pc;
    } hist[GW_FAULT_HISTORY];
    uint8_t  hist_count;
} gw_fault_rec_t;

/* ---------------- 故障现场（不落盘，只在本次运行内有效） ---------------- */
typedef struct {
    uint32_t pc;
    uint32_t lr;
    uint32_t psr;
    uint32_t sp;
    uint32_t cfsr;
    uint32_t hfsr;
    uint32_t mmfar;
    uint32_t bfar;
    uint8_t  valid;
    uint8_t  kind;
} gw_fault_ctx_t;

/* ==================================================================
 *  接口
 * ================================================================== */

/**
 * 冷启动初始化（**必须在任何故障判定之前调用**）
 *
 * 做的事：
 *   1) 校验 .noinit 里的记录是否有效（魔数 + 版本）
 *      **校验失败就把整块清成"刚上电"的状态** ——
 *      这是防止"随机值被当成崩溃记录"的关键
 *   2) 累计启动次数
 *   3) 判断是否需要进安全模式
 *
 * @param rec 指向 .noinit 段的记录（由移植层提供）
 * @return 本次启动原因
 */
gw_boot_cause_t gw_fault_boot_init(gw_fault_rec_t *rec);

/**
 * 记录一次故障（在故障处理程序里调用）
 *
 * @param rec  持久化记录
 * @param ctx  故障现场
 *
 * @note 本函数**必须能从故障处理程序里安全调用**：
 *       不调用任何库函数、不依赖栈的完整性、不写日志（日志可能用到锁）。
 *       所以它只做"结构体赋值 + 计数器 + 边界判断"。
 */
void gw_fault_record(gw_fault_rec_t *rec, const gw_fault_ctx_t *ctx);

/**
 * 判断是否需要进入安全模式
 *
 * @return GW_SAFE_FAULT_LOOP 需要；GW_SAFE_NONE 不需要
 */
gw_safe_reason_t gw_fault_need_safe_mode(const gw_fault_rec_t *rec);

/**
 * 软件主动请求复位之前调用：登记"这是有意重启，不算故障"
 *
 * 为什么要这个：如果 OTA 完成后软件复位，重启后看到上次是"故障"，
 * 可能误判成崩溃循环而进安全模式 —— 那就永远升不了级。
 */
void gw_fault_note_intentional_reset(gw_fault_rec_t *rec);

/** 取上次故障类型（无有效记录时返回 GW_FAULT_NONE） */
gw_fault_kind_t gw_fault_last_kind(const gw_fault_rec_t *rec);

/** 取同签名连续故障次数 */
uint16_t gw_fault_consecutive(const gw_fault_rec_t *rec);

/** 取历史故障条数 */
uint8_t gw_fault_history_count(const gw_fault_rec_t *rec);

/**
 * 取第 i 条历史（0 = 最旧）
 * @return 0 成功；-1 越界
 */
int gw_fault_history_at(const gw_fault_rec_t *rec, uint8_t i,
                        uint8_t *kind, uint32_t *pc);

/* ---------------- IWDG 复位默认值（判"看门狗开没开"的依据） ----------------
 *
 * ⚠️⚠️ 这两个值**必须**来自芯片手册，不能凭直觉写 0：
 *      STM32F1 的 `IWDG_PR`  复位默认值 = 0x000
 *                `IWDG_RLR` 复位默认值 = **0xFFF**  ← 不是 0！
 *
 *  2026-10-11 的教训（板上 39Hz 复位循环的根因）：
 *      `port/gw_fault_port.c` 原来写的是 `(PR != 0) || (RLR != 0)`，
 *      而 RLR 一上电就是 0xFFF -> **明明没开狗却报"已使能"**
 *      -> 引导程序（独立构建，从不调 MX_IWDG_Init，`hiwdg.Instance == NULL`）
 *         走进 `HAL_IWDG_Refresh(&hiwdg)` = 写地址 0x00000000
 *      -> 不精确总线错误 -> HardFault -> 故障处理程序复位 -> 无限循环。
 *      见缺陷 O（上板调试交接.md）。
 *
 *  这也是把这条判据**下沉到纯逻辑层**的理由：它在 `port/` 里时
 *  主机测试根本不编译那个文件，所以一次都没被测到。
 * ------------------------------------------------------------------ */
#define GW_IWDG_PR_RESET    0x00000000u
#define GW_IWDG_RLR_RESET   0x00000FFFu

/**
 * 按 IWDG 的 PR / RLR 寄存器值判断看门狗是否已经跑起来（纯逻辑，可主机测试）
 *
 * @param pr   IWDG->PR  的实测值
 * @param rlr  IWDG->RLR 的实测值
 * @return 1 = 已使能（必须一直喂）；0 = 未使能
 *
 * ⚠️ 这是**只读回读**判据，F1 上 IWDG 没有"使能位"可查，所以：
 *      - 只要 MX_IWDG_Init() 跑过，PR/RLR 就与复位默认值不同 -> 判为已使能 ✓
 *      - 残余歧义：若有人**故意**把狗配成 PR=0 / RLR=0xFFF（与复位默认值
 *        完全相同的组合），这里会误判为"未使能"。所以本判据**只能用于
 *        统计与报告**；真正喂狗必须走 `gw_fault_port_iwdg_feed()`，
 *        它无条件写 KR（没开狗时是无害空操作），不依赖这个判据。
 */
int gw_fault_iwdg_is_enabled(uint32_t pr, uint32_t rlr);

/** 故障类型转字符串（日志用） */
const char *gw_fault_kind_str(gw_fault_kind_t k);

/** 启动原因转字符串 */
const char *gw_boot_cause_str(gw_boot_cause_t c);

/** 安全模式原因转字符串 */
const char *gw_safe_reason_str(gw_safe_reason_t r);

#endif /* GW_FAULT_H */
