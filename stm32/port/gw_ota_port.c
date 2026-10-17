/**
 * @file    gw_ota_port.c
 * @brief   OTA 的 Flash 移植层实现
 */
#include "gw_ota_port.h"

#include "stm32f1xx_hal.h"
#include "gw_flash_map.h"
#include "gw_flash_stm32.h"
#include "gw_flash_guard.h"      /* ★ Flash 访问互斥（RTOS 下必须有；缺陷 X） */
#include "gw_fault_port.h"   /* gw_fault_port_iwdg_enabled() */

#include <string.h>

/* ==================================================================
 *  Flash 地址空间的上界
 *
 *  ⚠️ `gw_flash_map.h` 里没有直接给"结束地址"，用已有的宏算出来：
 *       GW_FLASH_BASE + GW_FLASH_SIZE_KB * 1024
 *     即 0x08000000 + 512 KB = 0x08080000
 *
 *  边界检查用它是为了拦住"擦到 Flash 外面"这种错误 ——
 *  那会触发总线错误（HardFault），现场很难看出原因。
 * ================================================================== */
#define GW_FLASH_END    (GW_FLASH_BASE + (GW_FLASH_SIZE_KB * 1024u))

/* ==================================================================
 *  RTOS 让出钩子（**弱符号**，不是宏开关）
 *
 *  ⚠️ 本文件要能在**裸机与 RTOS 两种构建下**编译，而这里需要一个
 *     "RTOS 下 vTaskDelay(1) / 裸机下什么都不做"的分支。
 *
 *  ⚠️⚠️ 2026-10-11 从"宏开关"改成"弱符号"，原因是一个**真实的静默失效**：
 *
 *     原来是 `#if defined(GW_OTA_PORT_USE_RTOS) && (... == 1)`，
 *     而**全工程没有任何地方定义这个宏**，也没有任何人提供强定义 ——
 *     于是 RTOS 构建里这个钩子**静默退化成空函数**：
 *
 *         OTA 擦除 136 KB 要 1.4~2.7 秒，期间不让出 CPU
 *         -> 空闲任务跑不到 -> **喂狗被跳过**（IWDG 只有 2.13~4.27 秒）
 *         -> 升级擦到一半被看门狗复位
 *
 *     这条链**只在真做一次 OTA 时才会暴露**，平时完全看不出来
 *     （而"升级时设备复位"正是迁移指南 §7 结尾提醒要查的那件事）。
 *
 *     弱符号把"要记得定义宏"从人的记忆里拿掉：
 *        · 裸机构建：用下面这个弱定义（空实现）
 *        · RTOS 构建：`port/gw_rtos.c` 提供**强定义**，链接器自动覆盖
 *     不需要任何人记得开开关，也就不存在"忘了开"。
 *
 *  ⚠️ 为什么不让本文件直接 `#include "FreeRTOS.h"`：
 *     那样全部 30+ 个模块的交叉编译验证都要带上 FreeRTOS 头，
 *     而 OTA 的 Flash 操作与 RTOS 无关。
 * ================================================================== */
__attribute__((weak)) void gw_ota_port_yield(void)
{
    /* 裸机：没有别的任务要跑，什么都不用做。 */
}

/* ------------------------------------------------------------------
 *  ⚠️⚠️ 2026-10-11：本文件**不再引用任何 HAL 句柄**（缺陷 O）
 *
 *  以前这里有一个 `extern IWDG_HandleTypeDef hiwdg;` + `HAL_IWDG_Refresh(&hiwdg)`，
 *  用 `HAL_IWDG_MODULE_ENABLED` 开关包着。**这个设计有个致命盲点**：
 *
 *      本文件同时被**应用镜像**和**独立引导程序镜像**编译
 *      （tools/build_ota_images.ps1 会编 port/ 下的全部 .c）。
 *      宏只能保证"类型存在"，**保证不了"句柄被初始化过"**：
 *          应用   ：MX_IWDG_Init() 填好 hiwdg -> Instance 有效  ✓
 *          引导程序：从不调 MX_IWDG_Init -> **hiwdg.Instance == NULL**
 *      于是引导程序一走到喂狗就 `NULL->KR = ...` = 写地址 0x00000000
 *      -> 不精确总线错误 -> HardFault -> 复位 -> 39Hz 复位循环，
 *      引导程序**永远走不到跳转应用那一步**（板上实测，缺陷 O）。
 *
 *  现在的规矩：**喂狗只能走 `gw_fault_port_iwdg_feed()`** ——
 *  它直接写 `IWDG->KR`，不经过任何句柄，也不依赖"狗开没开"的回读。
 *  `port/gw_fault_port.c` 在两个镜像里都被编译，所以一定能用。
 *
 *  ⚠️ 推而广之（这条要记住）：**被引导程序共用的代码不许引用
 *     "只有应用才初始化"的 HAL 句柄**（hiwdg / huart* / hcan / hdma_* …）。
 *     tools/check_boot_shared_handles.py 就是守这条的（已挂进 CI）。
 * ------------------------------------------------------------------ */

/* ==================================================================
 *  统计
 * ================================================================== */
static gw_ota_port_stat_t s_stat;

const gw_ota_port_stat_t *gw_ota_port_stats(void) { return &s_stat; }

void gw_ota_port_stats_reset(void) { memset(&s_stat, 0, sizeof(s_stat)); }

/* ==================================================================
 *  ⚠️ 喂狗（无句柄版本）
 *
 *  IWDG 一旦启动就无法用软件关闭（只能复位）。
 *  `gw_fault_port_iwdg_feed()` 无条件写 `IWDG->KR`：
 *      狗开着 -> 正常喂；狗没开 -> 写 0xAAAA 是无害空操作
 *  所以喂狗**不需要**、也**不应该**依赖"开没开"的回读判断 ——
 *  那个判断以前把引导程序坑死了（见上面缺陷 O）。
 *
 *  `s_stat.feeds` 的口径保持不变：只在**确实开着**时才计数，
 *  这样板上统计仍然能如实回答"这轮启动喂了几次狗"。
 * ================================================================== */
static void ota_feed(void)
{
    /* 先问一句"狗开着吗"——**只用于统计口径**，不作为喂狗的前提。
       （以前它是前提，而它在冷启动时会答错 -> 缺陷 O。） */
    int on = gw_fault_port_iwdg_enabled();

    gw_fault_port_iwdg_feed();      /* 无句柄、无条件写 KR：不可能判错 */

    if (on != 0) {
        s_stat.feeds++;
    }
}

/* ==================================================================
 *  页探测
 * ================================================================== */
int gw_ota_port_page_is_erased(uint32_t addr)
{
    uint32_t i;

    if ((addr % GW_FLASH_PAGE_SIZE) != 0u) {
        return -1;              /* 必须页对齐 */
    }
    if (addr < GW_FLASH_BASE || addr >= GW_FLASH_END) {
        return -1;
    }

    for (i = 0u; i < GW_FLASH_PAGE_SIZE; i++) {
        if (*(volatile uint8_t *)(addr + i) != 0xFFu) {
            return 0;
        }
    }
    return 1;
}

/* ==================================================================
 *  ⚠️ 擦除 —— 逐页擦 + 逐页喂狗
 *
 *  这是本文件存在的主要理由。见头文件的说明。
 * ================================================================== */
static int ota_erase(void *ctx, uint32_t addr, uint32_t len)
{
    FLASH_EraseInitTypeDef ei;
    uint32_t page_err = 0u;
    uint32_t end;
    uint32_t page;

    (void)ctx;

    /* ---- 参数检查：对齐与范围 ----
       ⚠️ 不对齐的擦除会波及相邻分区（例如把参数区擦掉）。
          HAL 自己也会拒绝，但让它拒绝之前我们先给出**明确的返回值**，
          便于上层区分"参数错"与"Flash 坏"。 */
    if ((addr % GW_FLASH_PAGE_SIZE) != 0u) {
        s_stat.erase_fails++;
        return -1;
    }
    if ((len % GW_FLASH_PAGE_SIZE) != 0u) {
        s_stat.erase_fails++;
        return -1;
    }
    if (addr < GW_FLASH_BASE
        || (addr + len) > GW_FLASH_END) {
        s_stat.erase_fails++;
        return -1;
    }

    end = addr + len;

    /* ---- 逐页擦除：**每页单独取放互斥** + 喂狗 + 让出 ----
     *
     *  ⚠️ 用 HAL_FLASHEx_Erase 逐页调用（而不是一次传 PageNb 让它内部循环）：
     *     因为那样**中间没有机会喂狗**，2~3 秒的连续擦除必然触发 IWDG。
     *
     *  ⚠️⚠️ 互斥也要**按页**取放（2026-10-11，缺陷 X 的第二版）：
     *     第一版把互斥包在**整个擦除**外面（1.4~2.7 秒），结果是
     *     "从站任务一旦要保存参数就会被阻塞 2.7 秒" —— 而**逐页让出 CPU**
     *     这条设计的初衷正是"擦除期间 Modbus 从站照常响应"，等于自己把它废掉了。
     *     按页取放后：别的任务最多等**一页**（20~40 ms），而且只影响
     *     **要写 Flash 的操作**（参数保存）—— 寄存器读取不碰 Flash，照常响应。
     *
     *  ⚠️ 配对仍然是"按构造保证"：每轮循环**先取锁再擦、擦完立刻放**，
     *     错误路径也先放锁再返回 —— 见下面两处 gw_flash_unlock()。 */
    for (page = addr; page < end; page += GW_FLASH_PAGE_SIZE) {

        if (gw_flash_lock() != HAL_OK) {
            s_stat.erase_fails++;
            return -1;              /* 写保护生效（选项字节） */
        }

        ei.TypeErase    = FLASH_TYPEERASE_PAGES;
        ei.PageAddress  = page;
        ei.NbPages      = 1u;
        page_err        = 0u;

        if (HAL_FLASHEx_Erase(&ei, &page_err) != HAL_OK
            || page_err != 0xFFFFFFFFu) {
            gw_flash_unlock();
            s_stat.erase_fails++;
            return -1;
        }

        gw_flash_unlock();

        s_stat.pages++;

        /* ---- 每页都喂狗 ----
           一页擦除 20~40 ms，远小于 3.2 s 的 IWDG 超时。
           所以"每页喂一次"有充足余量。 */
        ota_feed();

        /* ---- RTOS 下顺便让出 CPU ----
           让 Modbus 从站**在升级期间依然能响应** ——
           上位机在传固件的同时可能还在读寄存器。
           裸机构建里这个是空函数。
           ⚠️ 让出必须在**放锁之后**：否则别的任务醒了也拿不到互斥，
              等于没让。 */
        gw_ota_port_yield();
    }

    return 0;
}

/* ==================================================================
 *  写 —— 按半字编程（F1 的最小写入单位）
 * ================================================================== */
static int ota_write(void *ctx, uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint32_t i;

    (void)ctx;

    if (data == 0 || len == 0u) {
        return -1;
    }
    if (addr < GW_FLASH_BASE || (addr + len) > GW_FLASH_END) {
        return -1;
    }

    if (gw_flash_lock() != HAL_OK) {
        return -1;
    }

    /* ⚠️ F1 的编程单位是**半字（16 位）**。
       逐半字写而不是逐字节：
         - 逐字节写会让 HAL 拒绝（地址没半字对齐时）
         - 最后可能剩一个奇数字节，用 0xFF 补齐 ——
           0xFF 是"不改变"的值，所以补出来是对的。 */
    for (i = 0u; i + 1u < len; i += 2u) {
        uint16_t hw = (uint16_t)((uint16_t)data[i]
                                 | ((uint16_t)data[i + 1u] << 8));

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                              addr + i, (uint64_t)hw) != HAL_OK) {
            gw_flash_unlock();
            return -1;
        }
        s_stat.write_ops++;
    }

    /* 结尾奇数字节：补 0xFF */
    if (i < len) {
        uint16_t hw = (uint16_t)((uint16_t)data[i] | 0xFF00u);

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                              addr + i, (uint64_t)hw) != HAL_OK) {
            gw_flash_unlock();
            return -1;
        }
        s_stat.write_ops++;
    }

    gw_flash_unlock();

    /* ---- 每片之后喂一次狗 ----
       一片最多 124 字节 = 62 个半字，每个约 50 µs，合计约 3 ms。
       远小于超时，但"写"是有可能在别处被大量调用的（例如整包校验后的
       重试），所以这里也喂一次，代价可以忽略。 */
    ota_feed();

    return 0;
}

/* ==================================================================
 *  回读校验
 *
 *  ⚠️ 为什么必须要回读，而不是信 HAL 的返回值：
 *     HAL_FLASH_Program 返回 HAL_OK 只说明"写操作被接受了"。
 *     在 Flash 寿命末期、或者供电不稳时，写入可能实际没生效 ——
 *     而那种错误只有回读才发现得了。
 *
 *     代价是每次写都多一次读（内部 Flash 读很快，约几十 ns/字节），
 *     相对于 2~3 ms 的写来说可以忽略。
 * ================================================================== */
static int ota_verify(void *ctx, uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint32_t i;

    (void)ctx;

    if (data == 0 || len == 0u) {
        return -1;
    }
    if (addr < GW_FLASH_BASE || (addr + len) > GW_FLASH_END) {
        return -1;
    }

    for (i = 0u; i < len; i++) {
        if (*(volatile uint8_t *)(addr + i) != data[i]) {
            s_stat.verify_fails++;
            return -1;
        }
    }
    return 0;
}

/* ==================================================================
 *  元数据读写（**双页 ping-pong**）
 *
 *  ⚠️ 2026-10-10 改成分页：
 *     元数据区是 4 KB = **2 页**，页 0 = GW_OTA_META_ADDR，
 *     页 1 = GW_OTA_META_ADDR + 2 KB。
 *     "哪一页是当前有效的、这一笔该写哪一页"由**纯逻辑层**决定
 *     （`src/gw_ota.c` 的 meta_pick / gw_ota_meta_save）；本层只提供三件事：
 *        读一页 / 擦一页 / 写一页（写含回读比对）
 *     这样"擦除中途掉电会不会丢唯一副本"能在主机端用假介质**确定性重放**
 *     —— 若把选页逻辑藏在本层，这件事就只能靠板子上反复拔电试。
 *
 *  ⚠️ 为什么需要它：原来单份元数据 + "擦一页再写" -> 擦除那 20~40ms 窗口里
 *     掉电，记录就没了 -> 引导程序按出厂默认走 -> 跑回 A。
 *     用户看到的是"升级成功却断电后变回旧版本"。
 * ================================================================== */

_Static_assert((GW_OTA_META_PAGES * GW_FLASH_PAGE_SIZE) == GW_OTA_META_SIZE,
               "元数据页数与大小跟 flash map 不一致（改一处必须改另一处）");
_Static_assert(sizeof(gw_ota_meta_t) <= GW_FLASH_PAGE_SIZE,
               "一条元数据记录必须能放进一页");
_Static_assert((sizeof(gw_ota_meta_t) % 2u) == 0u,
               "记录长度是奇数：半字写循环需要补 0xFF 处理");

static uint32_t meta_page_addr(uint8_t page)
{
    return GW_OTA_META_ADDR + ((uint32_t)page * GW_FLASH_PAGE_SIZE);
}

static int ota_meta_read_page(void *ctx, uint8_t page, uint8_t *buf, uint32_t len)
{
    uint32_t base;
    uint32_t i;

    (void)ctx;

    if (buf == 0 || len == 0u || len > GW_FLASH_PAGE_SIZE
        || page >= GW_OTA_META_PAGES) {
        return -1;
    }

    base = meta_page_addr(page);
    for (i = 0u; i < len; i++) {
        buf[i] = *(volatile uint8_t *)(base + i);
    }
    return 0;
}

static int ota_meta_erase_page(void *ctx, uint8_t page)
{
    FLASH_EraseInitTypeDef ei;
    uint32_t page_err = 0u;

    (void)ctx;

    if (page >= GW_OTA_META_PAGES) {
        return -1;
    }

    if (gw_flash_lock() != HAL_OK) {
        return -1;
    }

    ei.TypeErase   = FLASH_TYPEERASE_PAGES;
    ei.PageAddress = meta_page_addr(page);
    ei.NbPages     = 1u;

    if (HAL_FLASHEx_Erase(&ei, &page_err) != HAL_OK
        || page_err != 0xFFFFFFFFu) {
        gw_flash_unlock();
        s_stat.erase_fails++;
        return -1;
    }

    gw_flash_unlock();
    s_stat.pages++;

    /* 擦一页 20~40ms：属于长阻塞路径，逐页喂狗 */
    ota_feed();
    return 0;
}

static int ota_meta_write_page(void *ctx, uint8_t page, const uint8_t *buf,
                               uint32_t len)
{
    uint32_t base;
    uint32_t i;

    (void)ctx;

    if (buf == 0 || len == 0u || len > GW_FLASH_PAGE_SIZE
        || page >= GW_OTA_META_PAGES) {
        return -1;
    }

    base = meta_page_addr(page);

    /* ==================================================================
     *  ⚠️ 幂等兜底：目标页**已经就是这条记录**时不擦不写
     *
     *  意义与以前不同了，说清楚：
     *    - 主防线现在是**纯逻辑层**的业务字段比对（gw_ota_meta_save 里
     *      "除 seq 外全等就不写、也不递增 seq"），它才是"常态启动零擦写"的保证；
     *    - 这里兜的是"上层判断失误 / 同一笔重复调用"那一类 ——
     *      让**重复写同一页**变成零代价，于是即便上层出 bug 也不会磨 Flash。
     *
     *  ⚠️ 注意它比较的是**目标页**的内容，而目标页在 ping-pong 下装的通常是
     *     "上一代记录"（seq 不同），所以这个兜底**不会**误命中正常写入 ——
     *     命中只可能来自"这一页确实已经是这份内容"。
     * ================================================================== */
    {
        uint32_t same = 1u;

        for (i = 0u; i < len; i++) {
            if (*(volatile uint8_t *)(base + i) != buf[i]) {
                same = 0u;
                break;
            }
        }
        if (same != 0u) {
            s_stat.meta_write_skipped++;
            return 0;
        }
    }

    if (gw_flash_lock() != HAL_OK) {
        return -1;
    }

    /* ⚠️ 本函数**不擦除**：擦由上层显式调用（顺序可测，见头文件说明）。
       所以要写的那一页必须是已擦状态 —— 如果不是，写入会失败（Flash 只能
       把 1 写成 0），回读比对会把它抓出来。 */
    for (i = 0u; i + 1u < len; i += 2u) {
        uint16_t hw = (uint16_t)((uint16_t)buf[i]
                                 | ((uint16_t)buf[i + 1u] << 8));

        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                              base + i, (uint64_t)hw) != HAL_OK) {
            gw_flash_unlock();
            return -1;
        }
        s_stat.write_ops++;
    }

    gw_flash_unlock();

    /* 元数据是"升级的命门"：写完之后立刻回读确认。
       这里不能省 —— 元数据坏了会导致设备**下次启动进不去**。 */
    for (i = 0u; i < len; i++) {
        if (*(volatile uint8_t *)(base + i) != buf[i]) {
            s_stat.verify_fails++;
            return -1;
        }
    }

    ota_feed();
    return 0;
}

/* ==================================================================
 *  接口表
 * ================================================================== */
static const gw_ota_io_t s_io = {
    ota_erase,
    ota_write,
    ota_verify,
    ota_meta_read_page,
    ota_meta_erase_page,
    ota_meta_write_page,
    0                           /* ctx：本移植层不需要上下文 */
};

const gw_ota_io_t *gw_ota_port_io(void)
{
    return &s_io;
}
