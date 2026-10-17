/**
 * @file    gw_boot.c
 * @brief   Bootloader 启动决策与固件校验实现（纯逻辑）
 */
#include "gw_boot.h"

#include "gw_crc32.h"

/* 读块大小：4 KB 一个栈上缓冲。
   为什么不一次读完整包：136 KB 固件在只有 64 KB RAM 的芯片上不可能缓存。
   为什么是 4 KB：足够摊薄调用开销，又不会占太多栈。 */
#define GW_BOOT_CHUNK   4096u

/* ==================================================================
 *  单项校验
 * ================================================================== */

gw_boot_err_t gw_boot_check_sp(uint32_t sp)
{
    /* ⚠️ 这一条必须最先查。
       跳转的第一步就是设置 MSP，之后任何指令（包括函数调用、
       中断）都会往栈上写。如果 SP 是垃圾值，检查它的代码本身
       就可能已经把 RAM 写坏了。 */
    if (sp < GW_BOOT_RAM_BASE || sp > GW_BOOT_RAM_END) {
        return GW_BOOT_ERR_SP_RANGE;
    }
    /* ARM AAPCS 要求栈指针 8 字节对齐（因为 double 需要 8 字节对齐）。
       Cortex-M 的硬件只保证 4 字节，所以必须由软件保证。
       不对齐不会立刻崩，但会在用到 64 位数据时出问题 —— 那种 bug
       很难查，所以在入口就拦住。 */
    if ((sp & 0x7u) != 0u) {
        return GW_BOOT_ERR_SP_ALIGN;
    }
    return GW_BOOT_OK;
}

gw_boot_err_t gw_boot_check_reset(uint32_t reset, gw_ota_slot_t slot)
{
    uint32_t base;
    uint32_t end;

    if (gw_ota_slot_is_image(slot) == 0) {
        return GW_BOOT_ERR_NO_SLOT;
    }

    /* ① Thumb 位必须是 1。
       Cortex-M **只有 Thumb 状态**，向量表里的地址约定最低位为 1。
       如果这一位是 0，`BX` 会试图切到 ARM 状态 —— M3 不支持，
       立刻 UsageFault（而 UsageFault 默认又升级成 HardFault）。 */
    if ((reset & 0x1u) == 0u) {
        return GW_BOOT_ERR_THUMB_BIT;
    }

    /* ② 去掉 Thumb 位之后，地址必须落在目标分区内。
       本模块只返回**原因**，不改地址 —— 跳转时汇编里那一句 `BX`
       用的就是带 Thumb 位的值，这是硬件约定。 */
    base = gw_ota_slot_addr(slot);
    end  = base + GW_APP_SLOT_SIZE;

    if (reset < base || reset >= end) {
        return GW_BOOT_ERR_PC_RANGE;
    }
    return GW_BOOT_OK;
}

/* ==================================================================
 *  向量表校验
 * ================================================================== */

gw_boot_err_t gw_boot_check_vectors(const gw_boot_io_t *io,
                                    gw_ota_slot_t slot,
                                    gw_boot_plan_t *out)
{
    uint8_t  vec[8];
    uint32_t base;
    uint32_t sp;
    uint32_t reset;
    gw_boot_err_t e;

    if (io == 0 || io->read == 0 || out == 0) {
        return GW_BOOT_ERR_READ;
    }
    if (gw_ota_slot_is_image(slot) == 0) {
        return GW_BOOT_ERR_NO_SLOT;
    }

    base = gw_ota_slot_addr(slot);

    /* 读向量表的前两项（栈顶指针 + 复位向量） */
    if (io->read(io->ctx, base, vec, 8u) != 0) {
        return GW_BOOT_ERR_READ;
    }

    /* 小端拼装：ARM 是小端架构，向量的低字节在前。
       这里显式拼而不是强转指针 —— 强转在 PC 上会有对齐问题，
       而且违反严格别名规则（编译器可能优化出意外结果）。 */
    sp = (uint32_t)vec[0]
       | ((uint32_t)vec[1] << 8)
       | ((uint32_t)vec[2] << 16)
       | ((uint32_t)vec[3] << 24);

    reset = (uint32_t)vec[4]
          | ((uint32_t)vec[5] << 8)
          | ((uint32_t)vec[6] << 16)
          | ((uint32_t)vec[7] << 24);

    /* ⚠️ 顺序：先 SP，再 PC。
       理由见 gw_boot_check_sp 的说明 —— SP 是"检查之前就已经在用"的东西。 */
    e = gw_boot_check_sp(sp);
    if (e != GW_BOOT_OK) {
        return e;
    }
    e = gw_boot_check_reset(reset, slot);
    if (e != GW_BOOT_OK) {
        return e;
    }

    out->slot      = slot;
    out->base      = base;
    out->sp        = sp;
    out->reset     = reset;
    out->image_len = 0u;
    out->image_crc = 0u;
    out->is_pending = 0u;
    out->verified   = 0u;

    return GW_BOOT_OK;
}

/* ==================================================================
 *  整包 CRC 校验
 * ================================================================== */

gw_boot_err_t gw_boot_check_crc(const gw_boot_io_t *io, uint32_t base,
                                uint32_t len, uint32_t crc)
{
    uint8_t  buf[GW_BOOT_CHUNK];
    uint32_t done = 0u;
    uint32_t acc  = 0xFFFFFFFFu;

    if (io == 0 || io->read == 0) {
        return GW_BOOT_ERR_READ;
    }
    if (len == 0u || len > GW_APP_SLOT_SIZE) {
        return GW_BOOT_ERR_CRC;         /* 长度非法：直接判失败 */
    }

    /* 分块读 + 增量 CRC。不缓存整包 —— 见 GW_BOOT_CHUNK 的说明。 */
    while (done < len) {
        uint32_t n = len - done;

        if (n > GW_BOOT_CHUNK) {
            n = GW_BOOT_CHUNK;
        }
        if (io->read(io->ctx, base + done, buf, n) != 0) {
            return GW_BOOT_ERR_READ;
        }
        acc = gw_crc32_update(acc, buf, n);
        done += n;
    }

    if (gw_crc32_final(acc) != crc) {
        return GW_BOOT_ERR_CRC;
    }
    return GW_BOOT_OK;
}

/* ==================================================================
 *  完整准备
 * ================================================================== */

gw_boot_err_t gw_boot_prepare(const gw_boot_io_t *io, gw_ota_slot_t slot,
                              const gw_ota_meta_t *m, int check_crc,
                              gw_boot_plan_t *out)
{
    gw_boot_err_t e;

    if (out == 0) {
        return GW_BOOT_ERR_READ;
    }

    /* ---- 1) 向量表（必须先做：它决定"能不能跳"） ---- */
    e = gw_boot_check_vectors(io, slot, out);
    if (e != GW_BOOT_OK) {
        return e;
    }

    /* ---- 2) 元数据（决定"要不要做 CRC"） ----
     *
     * ⚠️ 这里踩过一个坑，值得记下来：
     *    `gw_ota_meta_valid()` 的返回值约定是 **成功返回 1**（不是 0），
     *    与本项目其它 `*_check()` / `*_validate()` 函数（成功返回 0）**相反**。
     *
     *    我最初写成了 `if (gw_ota_meta_valid(m) != 0)`，语义就反了：
     *    元数据**有效**时反而走了"没有元数据"的分支，
     *    于是 image_len / image_crc / is_pending 全是 0 ——
     *    **整包 CRC 校验被静默跳过**（因为长度为 0），
     *    一个损坏的固件会被正常启动。
     *
     *    这个 bug 是 test_boot.c 的联动测试抓出来的
     *    （断言 is_pending / verified 时发现都是 0）。
     *    单看代码很难发现 —— 两个函数名读起来都对。
     *
     *    教训：**混用两种返回值约定时，一定要在调用点写清楚**。 */
    if (m != 0 && gw_ota_meta_valid(m) == 1) {
        out->image_len  = (m->pending != 0u) ? m->pending_len
                                             : m->confirmed_len;
        out->image_crc  = (m->pending != 0u) ? m->pending_crc
                                             : m->confirmed_crc;
        out->is_pending = m->pending;
    } else {
        /* 没有可用元数据：向量表合法就跳，但**不标记 verified**。
           这不是"信任它"，而是"没有依据怀疑它" ——
           首次烧录后元数据往往是空的，此时必须能启动。 */
        out->is_pending = 0u;
        out->verified   = 0u;
        return GW_BOOT_OK;
    }

    /* ---- 3) 整包 CRC（可选） ----
       ⚠️ 待确认的固件**必须**校验（调用方在 check_crc 里体现）：
          它是新写进去的，最可能有问题。
          而正常启动可以跳过 —— 信任 OTA 阶段已经校验过，
          省下每次上电的 35 ms。 */
    if (check_crc != 0) {
        if (out->image_len == 0u) {
            /* 没有长度信息就无法校验。这**不算失败**：
               可能是首次烧录、元数据还没建立。
               但也不能标记 verified —— 如实反映"没校验过"。 */
            return GW_BOOT_OK;
        }
        e = gw_boot_check_crc(io, out->base, out->image_len,
                              out->image_crc);
        if (e != GW_BOOT_OK) {
            return e;
        }
        out->verified = 1u;
    }

    return GW_BOOT_OK;
}

/* ==================================================================
 *  启动尝试顺序（纯逻辑，可主机测试）
 *
 *  主槽 -> 另一槽 -> golden 出厂镜像 -> 放弃
 *
 *  ⚠️ 为什么把"顺序"拿出来而不是写在引导程序模板里：
 *     它是**策略**，而策略最容易写错又最难测 ——
 *     模板里的代码在 PC 上跑不了，于是"golden 到底有没有被试到"
 *     只能靠上板拔线试。抽到这里以后，用一个假 probe 就能穷举几种组合。
 *
 *  ⚠️ 顺序的意义：golden 是**最后**一道兜底，绝不能排在正常槽前面 ——
 *     否则"有新固件但校验不过"会静默跑回出厂镜像，而用户以为升级成功了。
 * ================================================================== */
gw_ota_slot_t gw_boot_pick_slot(gw_ota_slot_t primary,
                                int (*probe)(void *ctx, gw_ota_slot_t slot),
                                void *ctx)
{
    gw_ota_slot_t order[3];
    int i;
    int j;

    if (probe == 0) {
        return GW_OTA_SLOT_NONE;
    }

    order[0] = primary;                            /* 元数据决定的主槽 */
    order[1] = gw_ota_slot_other(primary);         /* 另一个槽（降级） */
    order[2] = GW_OTA_SLOT_GOLDEN;                 /* 出厂兜底 */

    for (i = 0; i < 3; i++) {
        int dup = 0;

        if (gw_ota_slot_is_image(order[i]) == 0) {
            continue;                   /* NONE 之类：跳过 */
        }
        for (j = 0; j < i; j++) {
            if (order[j] == order[i]) {
                dup = 1;                /* 已经试过同一个槽 */
                break;
            }
        }
        if (dup != 0) {
            continue;
        }
        if (probe(ctx, order[i]) == 0) {
            return order[i];            /* 0 = 可启动 */
        }
    }
    return GW_OTA_SLOT_NONE;
}

/* ==================================================================
 *  字符串
 * ================================================================== */

const char *gw_boot_err_str(gw_boot_err_t e)
{
    switch (e) {
    case GW_BOOT_OK:            return "OK";
    case GW_BOOT_ERR_NO_SLOT:   return "NO_SLOT";
    case GW_BOOT_ERR_READ:      return "READ_FAIL";
    case GW_BOOT_ERR_SP_RANGE:  return "SP_OUT_OF_RAM";
    case GW_BOOT_ERR_SP_ALIGN:  return "SP_NOT_ALIGNED";
    case GW_BOOT_ERR_PC_RANGE:  return "PC_OUT_OF_SLOT";
    case GW_BOOT_ERR_THUMB_BIT: return "NO_THUMB_BIT";
    case GW_BOOT_ERR_CRC:       return "CRC_MISMATCH";
    case GW_BOOT_ERR_META:      return "BAD_META";
    default:                    return "?";
    }
}
