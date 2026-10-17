/**
 * @file    gw_flash_stm32.c
 * @brief   STM32F103 内部 Flash 读写擦除实现（供 gw_param 使用）
 *
 * ============================================================================
 *  F1 的 Flash 编程特点（与 F4 不同，最容易踩坑的地方）
 *
 *    1) 最小编程单位是**半字（16 位）**，不是字节
 *       F4 可以按字节写，F1 不行。所以要先把数据拼成半字再写。
 *       奇数字节结尾时，最后一个半字的高字节补 0xFF。
 *
 *    2) 必须解锁才能写/擦：HAL_FLASH_Unlock() / HAL_FLASH_Lock()
 *       HAL 内部处理了 KEY1/KEY2 序列，不必自己写寄存器。
 *
 *    3) 写之前必须是擦除态（0xFF）
 *       Flash 只能把 1 变成 0，不能把 0 变回 1。
 *       本实现显式检查这一点并拒绝，而不是静默接受 ——
 *       静默接受会产生"写成功但内容被按位与打乱"的诡异现象。
 *
 *    4) 页大小 2 KB（大容量产品）
 *       已由 gw_flash_map.h 的编译期断言保证对齐。
 *
 *  ----------------------------------------------------------------------------
 *  关键设计：写操作不加缓存、不做批量优化
 *
 *  参数保存是低频操作（只在参数真正变化时发生），
 *  为了追求写入速度而引入缓存或合并，只会增加"掉电时数据状态不确定"的风险。
 *  gw_param 的掉电安全依赖"每一步都立即落盘"，所以这里必须逐半字直写。
 * ============================================================================
 */
#include "gw_flash_stm32.h"
#include "gw_flash_map.h"
#include "gw_flash_guard.h"      /* ★ Flash 访问互斥（RTOS 下必须有；缺陷 X） */

#include "stm32f1xx_hal.h"

/* ------------------------------------------------------------------
 * 内部：读
 * ------------------------------------------------------------------ */
static int flash_read(void *ctx, uint32_t offset, uint8_t *buf, uint32_t len)
{
    uint32_t addr = GW_PARAM_FLASH_ADDR + offset;
    uint32_t i;

    (void)ctx;

    if (buf == 0 || len == 0u) {
        return -1;
    }
    /* 边界检查：越界读会读到别的分区（例如应用代码区），
       虽然读不会损坏数据，但会让上层的校验逻辑基于错误内容做判断。 */
    if (offset + len > GW_PARAM_FLASH_SIZE) {
        return -1;
    }

    /* 内部 Flash 可以直接按字节寻址读（不需要解锁） */
    for (i = 0u; i < len; i++) {
        buf[i] = *(volatile uint8_t *)(addr + i);
    }
    return 0;
}

/* ------------------------------------------------------------------
 * 内部：写（按半字编程）
 * ------------------------------------------------------------------ */
static int flash_write(void *ctx, uint32_t offset, const uint8_t *buf, uint32_t len)
{
    uint32_t addr = GW_PARAM_FLASH_ADDR + offset;
    uint32_t i;

    (void)ctx;

    if (buf == 0 || len == 0u) {
        return -1;
    }
    if (offset + len > GW_PARAM_FLASH_SIZE) {
        return -1;
    }
    /* 起始地址必须半字对齐：F1 的编程单位是半字 */
    if ((addr & 1u) != 0u) {
        return -1;
    }

    /* ---- 先做"只允许 1->0"检查 ----
       这一步很重要：如果调用方忘了擦除，Flash 硬件会返回 PGERR，
       但错误发生在写入过程中，此时前面的半字已经写进去了，
       数据处于"一半新一半旧"的状态 —— 比直接拒绝更难排查。
       所以先整体检查一遍，不满足就一个字节都不写。 */
    for (i = 0u; i < len; i++) {
        uint8_t cur = *(volatile uint8_t *)(addr + i);

        if ((uint8_t)(cur & buf[i]) != buf[i]) {
            return -2;                  /* 目标不是擦除态，需要先擦除 */
        }
    }

    if (gw_flash_lock() != HAL_OK) {
        return -3;
    }

    /* ---- 逐半字编程 ----
       两个字节一组。最后一个字节落单时，高字节补 0xFF（擦除态），
       这样不会破坏相邻字节。 */
    for (i = 0u; i < len; i += 2u) {
        uint16_t half;

        if ((i + 1u) < len) {
            half = (uint16_t)((uint16_t)buf[i] | ((uint16_t)buf[i + 1u] << 8));
        } else {
            half = (uint16_t)((uint16_t)buf[i] | 0xFF00u);
        }

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                              addr + i, (uint64_t)half) != HAL_OK) {
            gw_flash_unlock();
            return -4;
        }
    }

    gw_flash_unlock();
    return 0;
}

/* ------------------------------------------------------------------
 * 内部：擦除一个槽（= 擦除一页）
 * ------------------------------------------------------------------ */
static int flash_erase_slot(void *ctx, uint8_t slot_index)
{
    FLASH_EraseInitTypeDef er;
    uint32_t               page_error = 0u;

    (void)ctx;

    if (slot_index >= GW_PARAM_FLASH_SLOT_COUNT) {
        return -1;
    }

    if (gw_flash_lock() != HAL_OK) {
        return -2;
    }

    er.TypeErase   = FLASH_TYPEERASE_PAGES;
    er.PageAddress = GW_PARAM_FLASH_ADDR
                     + (uint32_t)slot_index * GW_PARAM_SLOT_FLASH_SIZE;
    er.NbPages     = 1u;

    if (HAL_FLASHEx_Erase(&er, &page_error) != HAL_OK) {
        gw_flash_unlock();
        return -3;
    }

    gw_flash_unlock();

    /* ---- 擦除后回读确认 ----
       只信 HAL 的返回值不够：写保护、电压不足等情况都可能让
       "返回 OK 但实际没擦干净"。回读一次代价极低，
       却能避免"擦不干净 -> 写入失败 -> 参数丢失"的连锁问题。 */
    {
        uint32_t i;
        uint32_t base = er.PageAddress;

        for (i = 0u; i < GW_FLASH_PAGE_SIZE; i++) {
            if (*(volatile uint8_t *)(base + i) != 0xFFu) {
                return -4;              /* 擦除不彻底 */
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------
 * 内部：槽数量
 * ------------------------------------------------------------------ */
static uint8_t flash_slot_count(void *ctx)
{
    (void)ctx;
    return GW_PARAM_FLASH_SLOT_COUNT;
}

/* ------------------------------------------------------------------
 * 内部：槽起始偏移
 * ------------------------------------------------------------------ */
static uint32_t flash_slot_offset(void *ctx, uint8_t slot_index)
{
    (void)ctx;
    return (uint32_t)slot_index * GW_PARAM_SLOT_FLASH_SIZE;
}

/* ------------------------------------------------------------------
 * 对外
 * ------------------------------------------------------------------ */
static const gw_flash_if_t s_iface = {
    flash_read,
    flash_write,
    flash_erase_slot,
    flash_slot_count,
    flash_slot_offset,
    0                               /* ctx 未使用 */
};

const gw_flash_if_t *gw_flash_stm32_iface(void)
{
    return &s_iface;
}

int gw_flash_stm32_selfcheck(void)
{
    /* ---- 本函数**不检查**读保护（RDP）----
     *
     * 原因：开读保护后，调试器读不到 Flash，但**固件自己可以正常读写参数**。
     * 所以读保护不影响参数功能，判它没有意义。
     * 而且从固件里读 FLASH_OBR 位来判断 RDP 状态并不可靠
     * （该位的语义随容量等级变化，且写入 RDP 后需要复位才生效）。
     *
     * 真正需要早发现的是**写保护**：一旦误开了写保护，
     * 后续所有参数保存都会失败，而失败原因从 gw_param_save 的返回值上
     * 看不出来（只会看到 ERR_FLASH）。所以下面用"能否解锁"来探测。
     */

    /* 1) 参数区地址必须页对齐 —— 否则擦除会波及相邻分区 */
    if ((GW_PARAM_FLASH_ADDR % GW_FLASH_PAGE_SIZE) != 0u) {
        return -1;
    }

    /* 2) 槽数与大小必须自洽 */
    if ((uint32_t)GW_PARAM_FLASH_SLOT_COUNT * GW_PARAM_SLOT_FLASH_SIZE
        != GW_PARAM_FLASH_SIZE) {
        return -2;
    }

    /* 3) 解锁探测：解锁失败说明选项字节里的写保护生效了。
          解锁后立即上锁，本函数不会留下"已解锁"的状态。 */
    if (gw_flash_lock() != HAL_OK) {
        return -3;
    }
    gw_flash_unlock();

    return 0;
}

uint32_t gw_flash_param_base(void)
{
    return GW_PARAM_FLASH_ADDR;
}

uint8_t gw_flash_param_slot_count(void)
{
    return GW_PARAM_FLASH_SLOT_COUNT;
}
