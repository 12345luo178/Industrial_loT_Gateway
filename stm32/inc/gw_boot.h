/**
 * @file    gw_boot.h
 * @brief   Bootloader 的启动决策与固件校验（纯逻辑，可在 PC 上单元测试）
 *
 * ============================================================================
 *  这个模块在整条 OTA 链路里的位置
 *
 *      App 侧：接收固件 -> 写另一个分区 -> 置 active + pending（见 gw_ota.c）
 *                      |
 *                      v  重启
 *      Bootloader：读元数据 -> 决定启动哪个分区 -> **校验固件** -> 跳转
 *                      ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ 本模块
 *
 *  `gw_ota.c` 决定"该不该切"，本模块决定"切过去之后能不能真的跑起来"。
 *  少了本模块，一个写坏的固件会让设备**连 Bootloader 都过不去**
 *  （跳进垃圾代码，HardFault，重启，再跳……）——
 *  这正是 OTA 回滚机制要防的最终场景。
 *
 *  ----------------------------------------------------------------------------
 *  ⚠️ 跳转前必须校验三件事，顺序不能反
 *
 *  **① 栈顶指针（向量表第 0 项）必须落在 RAM 范围内。**
 *     跳转的第一步就是 `MSR MSP, ...` —— 如果这个值是垃圾，
 *     后面任何一条指令、包括中断都会立刻把 RAM 写坏。
 *     这一条必须**最先**检查，因为它是唯一"检查之前就已经在用"的东西。
 *
 *  **② 复位向量（向量表第 1 项）必须落在目标分区内。**
 *     否则会跳到别的分区、或者 Flash 空白区（0xFFFFFFFF）。
 *
 *  **③ Thumb 位必须为 1。**
 *     Cortex-M **只有 Thumb 状态**，向量表里的地址约定最低位为 1。
 *     如果那个位是 0，`BX` 会试图切到 ARM 状态 —— M3 不支持，
 *     立刻 UsageFault/HardFault。
 *
 *     实际影响：编译器生成的向量表里这一位**本来是对的**，
 *     所以正常情况查不出问题；但如果 Flash 被部分擦除（位翻转 1->0
 *     不可能，但整字节损坏可能），这一位就可能变成 0。
 *     检查它的成本是 1 条指令，收益是"不会跳进一个必然 HardFault 的地址"。
 *
 *  **④ 可选：整包 CRC32 校验。**
 *     136 KB 在 72 MHz 上查表法约 23 ms（估算，见 doc/量化实测）。
 *     Bootloader 每次上电都花 35 ms 是可接受的，但会让"上电到出数据"
 *     慢一点。所以这一步做成**可选**（`check_crc` 参数）：
 *       正常启动：可以跳过（信任 OTA 阶段已经校验过）
 *       待确认的固件：**必须校验**（因为它是新写进去的，最可能有问题）
 *
 *  ----------------------------------------------------------------------------
 *  本模块不做的事
 *
 *  不做真正的跳转（那是汇编，在 `port/gw_boot_port.c`）。
 *  本模块只负责"算出该跳到哪、并确认那个地址是合法的"。
 *  这样"什么情况下不该跳"可以在 PC 上完整测试，
 *  而移植层只剩下一段 10 行的汇编。
 * ============================================================================
 */
#ifndef GW_BOOT_H
#define GW_BOOT_H

#include <stdint.h>

#include "gw_ota.h"

/* ==================================================================
 *  内存边界（用于校验向量表）
 *
 *  STM32F103ZET6：Flash 512 KB @ 0x08000000，RAM 64 KB @ 0x20000000。
 * ================================================================== */
#define GW_BOOT_FLASH_BASE      0x08000000u
#define GW_BOOT_FLASH_END       0x08080000u     /* 512 KB */
#define GW_BOOT_RAM_BASE        0x20000000u
#define GW_BOOT_RAM_END         0x20010000u     /* 64 KB */

/** Cortex-M 的向量表项数（至少要能放下前两项） */
#define GW_BOOT_VEC_MIN_WORDS   2u

/* ==================================================================
 *  失败原因
 * ================================================================== */
typedef enum {
    GW_BOOT_OK              = 0,
    GW_BOOT_ERR_NO_SLOT     = 1,    /* 元数据里没有可启动的分区 */
    GW_BOOT_ERR_READ        = 2,    /* 读向量表失败 */
    GW_BOOT_ERR_SP_RANGE    = 3,    /* 栈顶指针不在 RAM 范围内 */
    GW_BOOT_ERR_SP_ALIGN    = 4,    /* 栈顶指针未 8 字节对齐 */
    GW_BOOT_ERR_PC_RANGE    = 5,    /* 复位向量不在目标分区内 */
    GW_BOOT_ERR_THUMB_BIT   = 6,    /* 复位向量缺 Thumb 位 */
    GW_BOOT_ERR_CRC         = 7,    /* 整包 CRC 不匹配 */
    GW_BOOT_ERR_META        = 8     /* 元数据不可信 */
} gw_boot_err_t;

/* ==================================================================
 *  启动计划（校验通过后的结果）
 * ================================================================== */
typedef struct {
    gw_ota_slot_t slot;             /* 要启动的分区 */
    uint32_t      base;             /* 分区基地址 */
    uint32_t      sp;               /* 栈顶指针（向量表第 0 项） */
    uint32_t      reset;            /* 复位向量（向量表第 1 项，已含 Thumb 位） */
    uint32_t      image_len;        /* 固件长度（来自元数据） */
    uint32_t      image_crc;        /* 固件 CRC32（来自元数据） */
    uint8_t       is_pending;       /* 1 = 这是待确认的新固件 */
    uint8_t       verified;         /* 1 = 已做过整包 CRC 校验 */
} gw_boot_plan_t;

/* ==================================================================
 *  内存读取抽象
 *
 *  ⚠️ 为什么要抽象：本模块要能在 PC 上测。
 *    在 PC 上"Flash 内容"就是一块普通数组，
 *    在 STM32 上就是 `*(volatile uint32_t *)addr`。
 * ================================================================== */
typedef struct {
    /**
     * 从 Flash 读 len 字节到 buf
     * @return 0 成功；<0 失败（地址非法/未映射）
     */
    int (*read)(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len);
    void *ctx;
} gw_boot_io_t;

/* ==================================================================
 *  接口
 * ================================================================== */

/**
 * ⚠️ 校验一个分区的向量表并生成启动计划（**本模块的核心**）
 *
 * @param io         内存读取接口
 * @param slot       要校验的分区
 * @param m          元数据（用于取 image_len / image_crc；可为 NULL 跳过 CRC）
 * @param check_crc  1 = 做整包 CRC32 校验；0 = 跳过
 * @param out        输出启动计划
 * @return GW_BOOT_OK 或失败原因
 *
 * @note 校验顺序见文件头说明 —— **栈顶指针必须最先查**。
 */
gw_boot_err_t gw_boot_prepare(const gw_boot_io_t *io, gw_ota_slot_t slot,
                              const gw_ota_meta_t *m, int check_crc,
                              gw_boot_plan_t *out);

/**
 * 按"主槽 -> 另一槽 -> golden 出厂镜像"的顺序挑一个能启动的槽
 *
 * @param primary 元数据决定的主槽（A / B；其它值视为未知）
 * @param probe   `probe(ctx, slot)` 返回 **0 = 该槽可启动**；NULL 则直接放弃
 * @param ctx     传给 probe 的上下文
 * @return 第一个可启动的槽；都不行返回 GW_OTA_SLOT_NONE
 *
 * ⚠️ 顺序是**策略**，所以放在纯逻辑层：模板里的代码在 PC 上跑不了，
 *    "golden 到底有没有被试到"就只能靠上板拔线；放在这里可以用假 probe 穷举。
 *
 * ⚠️ golden **必须最后**：否则"新固件校验不过"会静默跑回出厂镜像，
 *    而用户以为升级成功了。
 */
gw_ota_slot_t gw_boot_pick_slot(gw_ota_slot_t primary,
                                int (*probe)(void *ctx, gw_ota_slot_t slot),
                                void *ctx);

/**
 * 只校验向量表（不做 CRC）
 *
 * 拆出来是为了让"上电时的快速检查"与"升级后的完整校验"能分别调用。
 */
gw_boot_err_t gw_boot_check_vectors(const gw_boot_io_t *io,
                                    gw_ota_slot_t slot,
                                    gw_boot_plan_t *out);

/**
 * 校验整包 CRC32
 *
 * @param io   内存读取
 * @param base 分区基地址
 * @param len  固件长度
 * @param crc  期望的 CRC32
 * @return GW_BOOT_OK 或 GW_BOOT_ERR_CRC
 *
 * ⚠️ 读取按块进行，不缓存整包 —— 136 KB 在只有 64 KB RAM 的芯片上
 *    不可能整包缓存。所以必须用 `gw_crc32_update()` 增量算。
 */
gw_boot_err_t gw_boot_check_crc(const gw_boot_io_t *io, uint32_t base,
                                uint32_t len, uint32_t crc);

/**
 * 校验栈顶指针是否合法
 *
 * 独立导出是为了让测试能逐项覆盖边界值（正好等于 RAM 起点/终点等）。
 */
gw_boot_err_t gw_boot_check_sp(uint32_t sp);

/** 校验复位向量是否合法（范围 + Thumb 位） */
gw_boot_err_t gw_boot_check_reset(uint32_t reset, gw_ota_slot_t slot);

/** 失败原因字符串（日志与打屏用） */
const char *gw_boot_err_str(gw_boot_err_t e);

#endif /* GW_BOOT_H */
