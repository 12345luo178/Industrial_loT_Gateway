/**
 * @file    gw_flash_map.h
 * @brief   Flash 分区规划（整个项目的持久化布局，单点定义）
 *
 * ============================================================================
 *  STM32F103ZET6：512 KB Flash，页大小 **2 KB**（0x800），共 256 页。
 *
 *  ⚠️ 页大小是最容易记错的参数：
 *     小容量/中容量产品（如 F103C8）是 1 KB，大容量是 2 KB。
 *     记错会导致擦除范围不对 —— 要么擦掉别的数据，要么擦不干净。
 *     HAL 的 FLASH_PAGE_SIZE 在本芯片上就是 0x800，已核实。
 *
 *  ----------------------------------------------------------------------------
 *  分区表（**v2**，2026-10-11 为 golden 出厂镜像重排）
 *
 *    地址范围                     大小    用途
 *    --------------------------  ------  --------------------------------
 *    0x08000000 – 0x08007FFF      32 KB   Bootloader
 *    0x08008000 – 0x08008FFF       4 KB   OTA 元数据（A/B 冗余，各 2 KB）
 *    0x08009000 – 0x0802AFFF     136 KB   应用 A 槽
 *    0x0802B000 – 0x0804CFFF     136 KB   应用 B 槽
 *    0x0804D000 – 0x0806EFFF     136 KB   golden 出厂镜像（只读，不参与 OTA）
 *    0x0806F000 – 0x08070FFF       8 KB   预留（对齐余量）
 *    0x08071000 – 0x0807BFFF      44 KB   日志区（预留）
 *    0x0807C000 – 0x0807FFFF      16 KB   参数区  ← 本文件主要关心
 *
 *  ⚠️ **v1 → v2 改了什么、为什么**（2026-10-11）
 *
 *    v1 是"B 槽 208 KB，日志区紧跟其后"，**512 KB 正好排满、一个字节余量都没有**。
 *    于是要加 golden 就只能：① 缩小 A/B 槽；② 吃掉日志区（那是个**未实现但已规划**
 *    的功能区）。选了 ① —— 因为"借用一个已规划的功能区"正是同一天刚在输入寄存器
 *    28~31 上踩过的坑（见交接文档 §五）。
 *
 *    **只动了两处**：B 槽搬家（0x0803D000 -> 0x0802B000）+ 新增 golden 区。
 *    A 槽、元数据区、日志区、参数区**地址全都没变** —— 所以已经烧在设备上的
 *    参数区数据不受影响，元数据里的槽 ID（0/1）也不受影响（它存的不是地址）。
 *
 *    ⚠️ 代价说清楚：单槽上限从 208 KB 降到 **136 KB**（当前应用约 39 KB，
 *       单镜像时代的全功能应用约 68 KB —— 仍有 2 倍余量）。
 *
 *  ----------------------------------------------------------------------------
 *  参数区布局
 *
 *    槽大小 = 1 页 = 2 KB，共 8 个槽，合计 16 KB
 *
 *    槽 0..7 依次位于 0x0807C000 + i*0x800
 *
 *  为什么槽大小取 1 页：
 *    gw_param 的擦除接口是"擦除第 i 个槽"，而 Flash 只能按页擦除。
 *    把槽大小设成页大小的整数倍，就能让"擦一个槽"直接对应"擦一页"，
 *    不需要跨页拼接逻辑，也不会有"擦多了"的风险。
 *
 *  为什么留 8 个槽而不是 2 个：
 *    磨损均衡的效果与槽数成正比。8 槽把单页擦写次数摊薄到 1/8。
 *    代价只是 16 KB Flash（总容量的 3%），非常划算。
 * ============================================================================
 */
#ifndef GW_FLASH_MAP_H
#define GW_FLASH_MAP_H

#include <stdint.h>

/* ---------------- 芯片参数 ---------------- */
#define GW_FLASH_BASE           0x08000000u
#define GW_FLASH_SIZE_KB        512u
#define GW_FLASH_PAGE_SIZE      0x800u      /* 2 KB（大容量产品） */
#define GW_FLASH_PAGE_COUNT     256u        /* 512 KB / 2 KB */

/* ---------------- 各分区地址 ---------------- */
#define GW_BOOT_ADDR            0x08000000u
#define GW_BOOT_SIZE            (32u * 1024u)

#define GW_OTA_META_ADDR        0x08008000u
#define GW_OTA_META_SIZE        (4u * 1024u)

#define GW_APP_A_ADDR           0x08009000u
#define GW_APP_SLOT_SIZE        (136u * 1024u)

#define GW_APP_B_ADDR           (GW_APP_A_ADDR + GW_APP_SLOT_SIZE)  /* 0x0802B000 */
/* ---- golden 出厂镜像（2026-10-11 新增）----
 *
 * ⚠️ 它**不是** OTA 的槽：元数据里的 active/pending 只可能是 0/1/0xFF，
 *    所以 golden **在软件上就不可被 OTA 写**（OTA 只会写"非 active 的那个槽"）。
 *    引导程序只**读**它（gw_boot_io_t 本来就只有 read）。
 *
 * 用途：A、B 两个槽**都**校验不过时（例如某个固件 bug 写错了地址、
 *      或某页 Flash 坏了），仍然能启动一个**已知良好**的固件 ——
 *      设备不会变成"只能用 ST-Link 救"的砖。
 *
 * 顺序：先试 active，再试另一个槽，**最后**才试 golden。 */
#define GW_APP_GOLDEN_ADDR      (GW_APP_B_ADDR + GW_APP_SLOT_SIZE)  /* 0x0804D000 */

/* 三个镜像区等大（同一条规则，构建脚本与自检都不用为 golden 写特例） */
#define GW_LOG_ADDR             0x08071000u
#define GW_LOG_SIZE             (44u * 1024u)

/* ---------------- 参数区 ---------------- */
#define GW_PARAM_FLASH_ADDR     0x0807C000u
#define GW_PARAM_FLASH_SIZE     (16u * 1024u)

/* 槽大小 = 1 页，槽数 = 16KB / 2KB = 8 */
#define GW_PARAM_SLOT_FLASH_SIZE    GW_FLASH_PAGE_SIZE
#define GW_PARAM_FLASH_SLOT_COUNT   8u

/* 编译期检查：分区不得越界，且参数区必须按页对齐（否则擦除会波及相邻区域） */
_Static_assert((GW_PARAM_FLASH_SLOT_COUNT * GW_PARAM_SLOT_FLASH_SIZE)
               == GW_PARAM_FLASH_SIZE,
               "参数区大小与槽数量不匹配");
_Static_assert((GW_PARAM_FLASH_ADDR % GW_FLASH_PAGE_SIZE) == 0u,
               "参数区起始地址必须按页对齐，否则擦除会波及相邻区域");
_Static_assert((GW_PARAM_FLASH_SIZE % GW_FLASH_PAGE_SIZE) == 0u,
               "参数区大小必须是页大小的整数倍");
_Static_assert((GW_PARAM_FLASH_ADDR + GW_PARAM_FLASH_SIZE)
               <= (GW_FLASH_BASE + GW_FLASH_SIZE_KB * 1024u),
               "参数区超出 Flash 容量");
_Static_assert((GW_APP_A_ADDR + GW_APP_SLOT_SIZE) <= GW_APP_B_ADDR,
               "应用 A 槽与 B 槽重叠");
_Static_assert((GW_APP_B_ADDR + GW_APP_SLOT_SIZE) <= GW_APP_GOLDEN_ADDR,
               "应用 B 槽与 golden 区重叠");
_Static_assert((GW_APP_GOLDEN_ADDR + GW_APP_SLOT_SIZE) <= GW_LOG_ADDR,
               "golden 区与日志区重叠");
/* ⚠️ 三个镜像区必须**等大**：A/B 的 OTA 要求"能装进 A 的镜像也一定能装进 B"，
   golden 用同一份构建产物，所以也必须一样大。 */
_Static_assert((GW_APP_GOLDEN_ADDR - GW_APP_B_ADDR) == GW_APP_SLOT_SIZE,
               "golden 区大小必须与槽大小一致");
_Static_assert((GW_APP_B_ADDR - GW_APP_A_ADDR) == GW_APP_SLOT_SIZE,
               "B 槽大小必须与槽大小一致");
_Static_assert((GW_LOG_ADDR + GW_LOG_SIZE) <= GW_PARAM_FLASH_ADDR,
               "日志区与参数区重叠");

/* 参数区至少要有 2 个槽，否则磨损均衡与"不覆盖当前有效槽"都无从谈起 */
_Static_assert(GW_PARAM_FLASH_SLOT_COUNT >= 2u,
               "参数槽至少需要 2 个，否则掉电安全无法保证");

#endif /* GW_FLASH_MAP_H */
