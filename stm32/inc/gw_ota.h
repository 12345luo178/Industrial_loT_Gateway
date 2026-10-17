/**
 * @file    gw_ota.h
 * @brief   OTA 双分区升级与回滚（纯逻辑，可在 PC 上单元测试）
 *
 * ============================================================================
 *  这个模块要解决的核心问题：**升级失败后设备不能变砖**
 *
 *  单分区升级的死穴：
 *
 *      正在运行的固件被覆盖 -> 断电/写失败 -> 设备彻底起不来
 *
 *  双分区的做法：永远"往另一个分区写"，写完校验通过再切过去。
 *  于是旧固件在切换之前一直是完整的 —— 任何时候断电都能回到它。
 *
 *  ----------------------------------------------------------------------------
 *  ⚠️ 回滚机制最容易做错的一件事：**"待确认"标记必须在跳转之前写**
 *
 *  这是本模块最重要的设计决定，值得单独讲清楚。
 *
 *  常见但**错误**的写法是"先跳过去，跑起来之后再标记为待确认"：
 *
 *      1) 写入新固件，校验通过
 *      2) 设置 active = 新分区，跳转过去          <-- 危险在这里
 *      3) 新固件启动，跑通了，标记"确认成功"
 *
 *  问题：如果新固件根本跑不到第 3 步（例如启动时 HardFault、
 *  或者时钟配置错了导致根本进不了 main），那么：
 *
 *      - 元数据里 active 已经是新分区
 *      - 而"待确认"标记从来没写成功
 *      - Bootloader 看到 active 是新分区、没有待确认标记
 *      - **于是每次都跳进那个坏固件，无限重启**
 *
 *  正确的写法是**先标记再跳**：
 *
 *      1) 写入新固件，校验通过
 *      2) 设置 active = 新分区，**同时设 pending = 1**（一次写入，原子）
 *      3) 跳转过去
 *      4) 新固件跑通后，把 pending 清 0（这一步叫"确认"）
 *
 *  这样即使第 4 步永远执行不到，Bootloader 也会看到 pending = 1，
 *  在重试若干次之后**自动回滚**到旧分区。
 *
 *  一句话总结：**"待确认"是给"跑不起来的新固件"留的后路，
 *  所以它必须在"可能跑不起来"之前就写好。**
 *
 *  ----------------------------------------------------------------------------
 *  ⚠️ 回滚判据不能只看"启动次数"，还要看**运行时长**
 *
 *  只数启动次数会有个漏洞：新固件能起来、能走到"确认"那一步，
 *  但起来之后 10 秒就崩了（例如某个外设初始化有竞态）。
 *  这种固件"确认"成功了，但实际不可用。
 *
 *  本模块的判据是两者的组合：
 *      "连续 N 次启动都没能稳定运行够 T 秒" -> 才回滚
 *
 *  这样既能救"完全起不来"的固件，也能救"起来就崩"的固件。
 * ============================================================================
 */
#ifndef GW_OTA_H
#define GW_OTA_H

#include <stdint.h>

#include "gw_flash_map.h"

/* ==================================================================
 *  元数据（放在 GW_OTA_META_ADDR）
 * ================================================================== */

#define GW_OTA_META_MAGIC       0x4F544130u     /* "OTA0" */

/* 元数据结构版本：改动布局时必须递增，否则旧记录会被误读 */
#define GW_OTA_META_VERSION     2u      /* 2 = 加了 seq（双页 ping-pong） */

/* ---------------- 分区 ID ----------------
 *
 * ⚠️ 数值是**跨复位契约**：0 / 1 分别对应 App A / App B。
 *    改数值会让已烧录设备的元数据失效。
 */
typedef enum {
    GW_OTA_SLOT_A = 0,
    GW_OTA_SLOT_B = 1,
    /* ---- golden 出厂镜像（2026-10-11 新增）----
     *
     * ⚠️⚠️ 它**不是** OTA 的槽，而是"两个槽都坏时的最后一道兜底"。
     *     两条硬约束（都由代码保证，不是靠注释）：
     *       1) `gw_ota_meta_valid()` 只接受 active = A 或 B ->
     *          **golden 永远不可能出现在元数据里**（写进去会被判无效记录）。
     *       2) OTA 只写"非 active 的那个槽"，而 target 只会是 A/B ->
     *          **golden 在软件上就不可被 OTA 写**。
     *     所以它是一个只读的第三方镜像，引导程序只**读**它。
     *
     * ⚠️ 数值 2 是**运行期**约定，不是跨复位契约（元数据里不会出现它）。 */
    GW_OTA_SLOT_GOLDEN = 2,
    GW_OTA_SLOT_NONE = 0xFF
} gw_ota_slot_t;

/**
 * 这个 slot 值是不是"一个真实的镜像区"（A / B / golden）
 *
 * ⚠️ 为什么需要它：`gw_boot_check_*` 原来写的是"只认 A/B"，
 *    加 golden 时如果漏改某一处，症状是"golden 明明烧好了却永远说没有可用镜像"
 *    —— 所以把这条判断收成一个函数，一处改、处处生效，并且可被测试。
 */
int gw_ota_slot_is_image(gw_ota_slot_t slot);

/* ---------------- 元数据状态 ---------------- */
typedef enum {
    GW_OTA_META_EMPTY   = 0,    /* 未初始化（首次上电） */
    GW_OTA_META_VALID   = 1     /* 记录有效 */
} gw_ota_meta_state_t;

/**
 * 持久化元数据
 *
 * ⚠️ 布局是跨复位契约：
 *    - 字段顺序与大小不能随意改
 *    - 加字段必须递增 GW_OTA_META_VERSION
 *    - 总大小应 <= GW_FLASH_PAGE_SIZE（一页可容纳，便于整页擦写）
 *
 * ⚠️ 2026-10-10 加 `seq` 并把版本 1 -> 2（双页 ping-pong）：
 *    新旧判据必须有"序号"，否则"两份都有效"时无法判谁更新。
 *    版本号**必须**同时递增：否则一条旧固件写的记录会被按新布局读，
 *    `seq` 读到的是垃圾 —— 那正好会伪造出"两份都有效却互相矛盾"的假象。
 *    版本检查因此是**精确相等**（见 gw_ota_meta_valid），不做迁移。
 */
typedef struct {
    uint32_t magic;             /* GW_OTA_META_MAGIC */
    uint16_t version;           /* GW_OTA_META_VERSION */
    /* 落盘代数：只在**真正写盘**时 +1（内容没变不写、也不递增）。
       语义是"第几次真实落盘"，越大越新 —— 见 gw_ota_meta_save 的说明。 */
    uint32_t seq;
    uint8_t  state;             /* gw_ota_meta_state_t */
    uint8_t  active;            /* 当前应启动的分区（gw_ota_slot_t） */
    uint8_t  pending;           /* 1 = 新分区待确认（**跳转前就已置位**） */
    uint8_t  boot_tries;        /* pending 状态下已尝试启动的次数 */
    uint8_t  reserved0[2];

    uint32_t pending_len;       /* 待确认固件的长度（诊断用） */
    uint32_t pending_crc;       /* 待确认固件的 CRC32（诊断用） */

    uint32_t confirmed_len;     /* 已确认固件的长度 */
    uint32_t confirmed_crc;     /* 已确认固件的 CRC32 */

    uint32_t meta_crc;          /* 本结构（除本字段外）的 CRC32 —— 防元数据本身损坏 */
} gw_ota_meta_t;

/* ==================================================================
 *  元数据双页 ping-pong（2026-10-10）
 *
 *  为什么：元数据原来是**单份**，而"写一份元数据" = 擦一整页 + 写几十字节。
 *  擦除那 20~40ms 窗口里掉电，记录就没了 -> 引导程序按"出厂默认"走 -> 跑回 A。
 *  也就是用户会看到：**"升级明明成功了，断电一次就变回旧版本"**。
 *
 *  规则（受一条不变量约束：**任何瞬间至少一份有效记录**）：
 *      - 两页都有效  -> 取 seq 大的为"当前"，另一页是下次的写目标
 *      - 只有一页有效 -> 取它，写另一页
 *      - seq 相同（异常损坏）-> 取 pending==0（已确认）的那份；仍相同 -> 出厂默认
 *      - 两页都无效  -> 出厂默认，写目标 = 页 0，seq 从 1 开始
 *
 *  于是每次写完**两页都有效**（一新一旧），下次写才擦掉旧的那页 ——
 *  既不需要额外的"标记失效"步骤，也不会出现"两份都无效"的瞬间。
 *  副作用：擦除在**两页之间交替**，单页擦写次数减半。
 * ================================================================== */

/** 元数据页数（必须与 port/gw_flash_map.h 的 GW_OTA_META_SIZE 一致） */
#define GW_OTA_META_PAGES           2u

/* ==================================================================
 *  回滚判据
 * ================================================================== */

/** 允许新分区尝试启动的次数；超过就回滚 */
#define GW_OTA_MAX_BOOT_TRIES       3u

/** 新固件必须稳定运行够这么久才算"确认成功"（秒） */
#define GW_OTA_CONFIRM_UPTIME_S     30u

/**
 * 单个分片的最大长度
 *
 * ⚠️ 这里**刻意不引用 `GW_FRAME_MAX_PAYLOAD`**，而是定义一个自己的上限。
 *
 * 理由：OTA 模块不该依赖帧协议模块。当前上限恰好等于帧的载荷上限
 * （128 字节），但那个值将来可能因为"帧要支持更长的日志行"而改变 ——
 * 那时 OTA 的分片上限不该跟着被动变化（它是按 Flash 写入效率定的，
 * 与帧的载荷无关）。
 *
 * 如果上层的分片比这个大，`gw_ota_chunk()` 会明确拒绝（BAD_PARAM），
 * 而不是悄悄截断。
 */
#define GW_OTA_MAX_CHUNK            128u

/* ==================================================================
 *  下载状态机
 * ================================================================== */
typedef enum {
    GW_OTA_ST_IDLE      = 0,    /* 空闲，没有进行中的升级 */
    GW_OTA_ST_DOWNLOAD  = 1,    /* 正在接收分片 */
    GW_OTA_ST_VERIFY    = 2,    /* 分片收完，正在校验 */
    GW_OTA_ST_READY     = 3,    /* 校验通过，等待重启切换 */
    GW_OTA_ST_FAILED    = 4     /* 失败（原因见 err） */
} gw_ota_state_t;

/** 与寄存器 HREG_OTA_ST_* 的取值对齐（便于直接上报） */
#define GW_OTA_ST_TO_REG(s)     ((uint16_t)(s))

/* ---------------- 失败原因 ----------------
 *
 * 分开定义而不是只报"失败"：现场排查时"CRC 不对"与"Flash 写失败"
 * 要采取完全不同的动作（前者重传，后者检查硬件）。
 */
typedef enum {
    GW_OTA_ERR_NONE         = 0,
    GW_OTA_ERR_NO_BEGIN     = 1,    /* 没收到 OTA_BEGIN 就来了分片 */
    GW_OTA_ERR_BAD_PARAM    = 2,    /* 参数非法（长度/CRC/版本） */
    GW_OTA_ERR_TOO_BIG      = 3,    /* 固件超过分区容量 */
    GW_OTA_ERR_OFFSET       = 4,    /* 分片偏移不连续（丢片或重传错位） */
    GW_OTA_ERR_FLASH_WRITE  = 5,    /* Flash 写失败 */
    GW_OTA_ERR_FLASH_ERASE  = 6,    /* Flash 擦除失败 */
    GW_OTA_ERR_LEN_MISMATCH = 7,    /* 收完的长度与声明不符 */
    GW_OTA_ERR_CRC_MISMATCH = 8,    /* CRC32 校验不通过 */
    GW_OTA_ERR_SAME_SLOT    = 9     /* 目标分区与当前运行分区相同（不允许原地升级） */
} gw_ota_err_t;

/* ==================================================================
 *  硬件抽象（由移植层注入）
 *
 *  ⚠️ 为什么把"校验写入结果"做成独立回调而不是复用 write 的返回值：
 *     有些 Flash 驱动在写失败时返回成功（例如没有等待 BSY 位）。
 *     把"读回比对"作为**必须实现**的回调，能强制移植层做这件事 ——
 *     否则"写进去了但内容是错的"这种问题会被 CRC 校验推迟到很晚才发现。
 * ================================================================== */
typedef struct {
    /**
     * 擦除 [addr, addr+len) 区间；len 必须是页大小的整数倍
     * @return 0 成功；<0 失败
     */
    int (*erase)(void *ctx, uint32_t addr, uint32_t len);

    /**
     * 写入 [addr, addr+len)
     * @return 0 成功；<0 失败
     */
    int (*write)(void *ctx, uint32_t addr, const uint8_t *data, uint32_t len);

    /**
     * 读回比对（**必须实现**）
     * @return 0 一致；<0 不一致或读失败
     */
    int (*verify)(void *ctx, uint32_t addr, const uint8_t *data, uint32_t len);

    /**
     * 读元数据**某一页**（page = 0..GW_OTA_META_PAGES-1）
     *
     * @param buf 长度 >= sizeof(gw_ota_meta_t)
     * @return 0 成功；<0 失败
     *
     * ⚠️ 2026-10-10 由"整块 meta_read"改成分页：双页 ping-pong 需要
     *    **分别**读两页，由纯逻辑层决定哪一页是"当前有效的"。
     *    一页都读不到时返回 <0 —— 但注意"读到了但内容无效"**不是**失败，
     *    那由 gw_ota_meta_valid() 判定（全 FF 的空页就是典型的"读到但无效"）。
     */
    int (*meta_read_page)(void *ctx, uint8_t page, uint8_t *buf, uint32_t len);

    /**
     * 擦除元数据**某一页**
     * @return 0 成功；<0 失败
     *
     * ⚠️ 顺序由**纯逻辑层**决定：永远是"擦 seq 较小的那一页（旧的那份）"，
     *    擦完再写新记录。把顺序放在上层是为了让"掉电时会不会丢唯一副本"
     *    这件事能在主机端用假介质确定性重放（见 test_ota 的注入用例）。
     * ⚠️ 一页 2KB 的擦除要 20~40ms，是长阻塞路径 —— 实现方负责喂狗。
     */
    int (*meta_erase_page)(void *ctx, uint8_t page);

    /**
     * 写元数据**某一页**（该页必须已被擦除）+ **回读比对**
     *
     * @return 0 成功（写完且回读一致）；<0 失败
     *
     * ⚠️ 本函数**不负责擦除**（见上）：这样测试能分别注入"擦除失败"与
     *    "写入中途失败"两种掉电情形，而不是只能注入一个笼统的"写失败"。
     * ⚠️ 实现方应做**幂等兜底**：目标页内容与要写的内容完全一致时直接返回成功，
     *    不擦不写 —— 它兜的是"上层判断失误/重复调用"那一类（主防线在
     *    gw_ota_meta_save 的业务字段比对，不是这里）。
     */
    int (*meta_write_page)(void *ctx, uint8_t page, const uint8_t *buf, uint32_t len);

    void *ctx;
} gw_ota_io_t;

/* ==================================================================
 *  OTA 上下文
 * ================================================================== */
typedef struct {
    /* ---- 下载状态 ---- */
    gw_ota_state_t state;
    gw_ota_err_t   err;

    gw_ota_slot_t  target;      /* 本次写入的目标分区 */
    uint32_t       total_len;   /* OTA_BEGIN 声明的固件总长 */
    uint32_t       expect_crc;  /* OTA_BEGIN 声明的 CRC32 */
    uint32_t       written;     /* 已写入字节数（也用作下一个分片的期望偏移） */
    uint32_t       running_crc; /* 分片累积 CRC32（与 OTA_END 时的比对用） */
    uint16_t       fw_major;
    uint16_t       fw_minor;
    uint32_t       chunks;      /* 已接收分片数（诊断用） */

    /* ---- 统计（诊断用） ---- */
    uint32_t       retries;     /* 被拒绝的分片数（上层可据此重传） */

    gw_ota_io_t    io;
    uint8_t        inited;
} gw_ota_t;

/* ==================================================================
 *  接口
 * ================================================================== */

/**
 * 初始化 OTA 模块（**不读元数据**，只清运行时状态）
 *
 * @param target 本次运行允许写入的目标分区
 *               —— 通常是"当前运行分区的另一个"，由 Bootloader 告知。
 *                  传 GW_OTA_SLOT_NONE 表示禁止升级。
 */
int gw_ota_init(gw_ota_t *o, const gw_ota_io_t *io, gw_ota_slot_t target);

/** 取当前状态 / 错误码 */
gw_ota_state_t gw_ota_state(const gw_ota_t *o);
gw_ota_err_t   gw_ota_err(const gw_ota_t *o);

/**
 * 处理 OTA_BEGIN
 *
 * @param major/minor 固件版本
 * @param total_len  固件总长度（字节）
 * @param crc32      整包 CRC32
 *
 * 做的事：参数校验 -> 擦除目标分区 -> 复位进度
 *
 * ⚠️ 擦除放在 BEGIN 而不是 END：
 *    擦除一个分区（136 KB，分区表 v2）需要约 1.4~2.7 秒（F1 一页 20~40ms × 68 页）。
 *    如果放在 END，那 1.4~2.7 秒会发生在"已经收完所有数据、
 *    就等一个 ACK"的时刻 —— 上位机会以为超时了。
 *    放在 BEGIN 则可以在 BEGIN 的 ACK 之前完成，
 *    而且此时还没有数据丢失风险。
 */
int gw_ota_begin(gw_ota_t *o, uint16_t major, uint16_t minor,
                 uint32_t total_len, uint32_t crc32);

/**
 * 处理 OTA_CHUNK
 *
 * @param offset 本片在固件中的偏移
 * @param data   数据
 * @param len    数据长度
 *
 * @return 0 已写入；1 已忽略（重复分片，无需重传）；<0 失败（需重传）
 *
 * ⚠️ 关于"重复分片"的处理：
 *    Modbus/串口链路上偶尔会出现重传。如果同一片收到两次，
 *    第二次会写到同一个地址 —— 对已经写过的 Flash 再写同样的值
 *    在理论上是允许的（1 只能变 0），但实际上第二次写会因为
 *    "目标不是全 1"而被拒绝。
 *    所以这里**显式识别重复分片并返回 1（已忽略）**，
 *    而不是让它去撞 Flash 的写入限制、报一个误导性的错误。
 */
int gw_ota_chunk(gw_ota_t *o, uint32_t offset, const uint8_t *data, uint32_t len);

/**
 * 处理 OTA_END
 *
 * @return 0 校验通过，进入 READY；<0 失败（见 err）
 *
 * 做的事：长度比对 -> CRC32 比对 -> 写元数据（active = 新分区, pending = 1）
 *
 * ⚠️ **pending 在这里就置位了，不是等新固件跑起来再置**。
 *    理由见文件头的说明 —— 这是回滚机制能生效的前提。
 */
int gw_ota_end(gw_ota_t *o);

/** 复位下载状态机（放弃本次升级，不影响已确认的固件） */
void gw_ota_abort(gw_ota_t *o);

/* ==================================================================
 *  元数据操作（Bootloader 与应用共用）
 * ================================================================== */

/** 初始化一块空元数据（首次上电时用） */
void gw_ota_meta_make_default(gw_ota_meta_t *m, gw_ota_slot_t active);

/** 校验元数据是否可信（魔数 + 版本 + 自身 CRC） */
int gw_ota_meta_valid(const gw_ota_meta_t *m);

/** 重算并填入 meta_crc */
void gw_ota_meta_seal(gw_ota_meta_t *m);

/** 取分区基地址 */
uint32_t gw_ota_slot_addr(gw_ota_slot_t slot);

/** 取分区的另一个（A <-> B） */
gw_ota_slot_t gw_ota_slot_other(gw_ota_slot_t slot);

/**
 * ⚠️ 决定本次该启动哪个分区（**Bootloader 的核心决策**）
 *
 * @param m       元数据（会被修改：启动计数、回滚等）
 * @param commit  1 = 调用方要求把修改写回 Flash；0 = 只做判断
 * @return 要启动的分区（GW_OTA_SLOT_NONE 表示没有可启动的分区）
 *
 * 决策表：
 *
 *   pending = 0  ->  启动 active
 *                    （正常情况，也是确认成功之后的常态）
 *
 *   pending = 1 且 boot_tries < GW_OTA_MAX_BOOT_TRIES
 *                ->  启动 active，boot_tries++
 *                    （给新固件机会；计数在跳转前写回，见下方说明）
 *
 *   pending = 1 且 boot_tries >= GW_OTA_MAX_BOOT_TRIES
 *                ->  **回滚**：active 切回另一个分区，pending = 0
 *                    （新固件试了 N 次都没确认，判定不可用）
 *
 * ⚠️ **计数必须在跳转之前写回 Flash**。
 *    如果只在内存里加一、等新固件跑起来再写回，那么一个
 *    "连 Bootloader 都跳不过去"的坏固件（例如向量表损坏）
 *    会让计数永远停在 0，无限重试。
 *
 * ⚠️⚠️ **本函数会修改 `m` 的字段，但不重算 CRC** ——
 *      调用方**必须**紧接着调用 `gw_ota_meta_save()`，
 *      它内部会 `gw_ota_meta_seal()` 重算 CRC 再落盘。
 *
 *      如果忘了这一步，会出两种症状（都很隐蔽）：
 *        1) 内存里的记录 CRC 不对 -> `gw_ota_meta_valid()` 返回 0
 *           -> 后续按"元数据损坏"处理 -> 意外回滚到 A
 *        2) 更糟：如果实现改成"不校验 CRC 直接落盘"，
 *           那么下次启动时 Bootloader 会读到一份 CRC 不符的记录
 *
 *      我在写 test_boot.c 时就踩到了第 1 种：
 *      在 `boot_decide()` 之后直接拿内存里的 `m` 去校验，得到 valid=0，
 *      排查了半天才发现是漏了 save（差一个字节：boot_tries 从 0 变 1）。
 *
 *      正确的调用顺序：
 *          boot = gw_ota_boot_decide(&m);
 *          gw_ota_meta_save(&io, &m);        // 必须：重算 CRC 并落盘
 *          gw_ota_meta_load(&io, &m);        // 可选：读回验证
 *          gw_boot_prepare(...);             // 校验向量表与固件
 *
 *      ⚠️ 但 Bootloader 请直接用 `gw_ota_boot_persist()`（下一个函数），
 *         它把"决策 + 只在必要时落盘"合成一步，避免下一条说的磨损问题。
 */
gw_ota_slot_t gw_ota_boot_decide(gw_ota_meta_t *m);

/**
 * 启动决策 + **只在必要时**落盘（Bootloader 就该用这一个）
 *
 * @param io        元数据读写接口（io->meta_write 为空时退化为只决策）
 * @param m         元数据；**会被就地修改**（与 gw_ota_boot_decide 相同）
 * @param out_saved 出参，可为 NULL：
 *                    1 = 写过 Flash（试用计数 +1 或发生了回滚）
 *                    0 = 内容没变，**一次擦写都没做**（正常启动的常态）
 *                   -1 = 该写但写失败了（此时放过启动，但回滚计数不可靠）
 * @return 要启动的分区（GW_OTA_SLOT_NONE 表示没有可启动的分区）
 *
 * ==================================================================
 *  ⚠️ 为什么必须"只在必要时写"（2026-10-10 修掉的一个真问题）
 *
 *  STM32 内部 Flash 一页的擦写寿命约 **1 万次**，而"写元数据" =
 *  **擦一整页（2 KB） + 写 32 字节** —— 磨损来自擦除。
 *
 *  原来的 Bootloader 模板是这么写的：
 *
 *      target = gw_ota_boot_decide(&m);
 *      gw_ota_meta_save(&io, &m);          // ← 无条件！每次都擦
 *
 *  而 `pending == 0`（老固件已确认）时 decide **一个字段都不改**，
 *  这次擦除纯属浪费。后果分两种量级：
 *
 *      正常设备 1 天重启 1 次   -> 1 万天 ≈ 27 年（看似没事，但是无谓消耗）
 *      **复位循环** 1 次/秒      -> 86400 次/天
 *                               -> **约 3 小时就把元数据页磨穿** ❌
 *
 *  （复位循环很现实：电源抖动、看门狗超时、反复 HardFault、
 *    或者一个"启动就复位"的坏固件。）
 *
 *  改成本函数之后：常态启动走**纯只读**路径（out_saved = 0），
 *  擦写次数只与**升级次数**相关，与"运行多久、重启多少次"彻底解耦：
 *
 *      升级成功      2 次（写 active+pending=1、30 秒后确认）
 *      升级失败回滚  5 次（finalize 1 + 试用计数 3 + 回滚 1）
 *      **普通重启    0 次**
 *
 *  ⚠️ 判据是"调用前后 memcmp 整个结构体"，而不是靠调用方声明改了什么 ——
 *     这样以后在 decide 里新增任何字段修改，都自动被这个判断覆盖，
 *     不会出现"新加了改动但忘了标记 changed"→ 计数丢掉 → 永不回滚。
 */
gw_ota_slot_t gw_ota_boot_persist(const gw_ota_io_t *io, gw_ota_meta_t *m,
                                  int *out_saved);

/**
 * 确认升级成功（新固件稳定运行够久之后调用）
 *
 * 做的事：pending = 0，把 pending_len/crc 记为 confirmed，清零 boot_tries
 *
 * ⚠️ 确认之后**不能再回滚** —— 这是刻意的：
 *    如果允许确认后回滚，那么"新固件运行中偶发崩溃导致重启"
 *    会被误判成升级失败，从而回到一个更旧的固件，
 *    而现场看到的是"升级成功了但版本又变回去了"。
 */
void gw_ota_meta_confirm(gw_ota_meta_t *m, uint32_t len, uint32_t crc);

/**
 * 判断"是否该确认了"
 *
 * @param m          元数据
 * @param uptime_s   本次已稳定运行的秒数
 * @param running    **我此刻跑在哪个槽**（`gw_ota_running_slot()`）
 * @return 1 = 该确认；0 = 还不到时候 / 不是我的 / 参数非法
 *
 * 判据是**运行时长**而不是"启动次数"：
 * 见文件头的说明（只数次数救不了"起来就崩"的固件）。
 *
 * ⚠️⚠️ 2026-10-11 加 `running`（缺陷 P）：**"待确认的升级是不是我现在跑的这份"
 *    是确认的前提，不是可选项**。所以这个参数是**必须**的 ——
 *    接口层面不给你"忘记判槽"的机会，而不是靠注释提醒。
 */
int gw_ota_should_confirm(const gw_ota_meta_t *m, uint32_t uptime_s,
                          gw_ota_slot_t running);

/**
 * "待确认的这次升级，是不是我现在跑的这份？"
 *
 * @param m        元数据（NULL / 无效 -> 0）
 * @param running  我此刻跑在哪个槽（`gw_ota_running_slot()`）
 * @return 1 = 是（我就是在跑 active 那个槽，有权确认）；0 = 不是
 *
 * ⚠️ 单独抽出来是为了让这个判据**能被单独测试**，也为了让日志能如实区分
 *    "还不到时候" 与 "这根本不是我的事"（缺陷 P 的核心）。
 */
int gw_ota_confirm_is_mine(const gw_ota_meta_t *m, gw_ota_slot_t running);

/* ---------------- 高层封装：一步确认，别自己拆 ------------------
 *
 * ⚠️ 用这个，不要自己拼 load + should_confirm + meta_confirm + save。
 *
 * 原因（真实缺陷）：那四个原语曾经**一个调用方都没有**，
 * 因为调用方需要自己记住"改完必须 save"，而漏掉 save 不会有任何报错 ——
 * 只是 pending 永远不清，于是**一个完全正常的新固件会在重启 3 次后被回滚掉**。
 *
 * 这个函数把"读-判-改-落盘"封在一起，调用方只表达意图。
 */
typedef enum {
    GW_OTA_CONFIRM_R_NONE = 0,  /* 没有待确认的升级，或时间还不够（常态） */
    GW_OTA_CONFIRM_R_DONE = 1,  /* 刚刚完成确认并已落盘 */
    /* ★ 2026-10-11 新增（缺陷 P）：有待确认的升级，但它**不是我现在跑的这份**
       -> **拒绝确认，一个字都不写**。
       为什么会走到这里：引导程序发现目标槽的镜像校验不过（空/坏）时会
       **降级启动另一个槽**，于是"我"是旧固件，而元数据里的 active 是新槽。
       此时若替新槽确认，就会把 pending 清掉、boot_tries 归零 ->
       **回滚计数永远到不了 GW_OTA_MAX_BOOT_TRIES，试用期保护被绕过**。 */
    GW_OTA_CONFIRM_R_WRONG_SLOT = 2,
    GW_OTA_CONFIRM_R_ERR  = -1  /* 读/写元数据失败：**没有改动 Flash** 或落盘失败 */
} gw_ota_confirm_r_t;

/**
 * 跑够时间就把这次的升级确认为可用（幂等）
 *
 * @param io        元数据读写接口（`gw_ota_port_io()`）
 * @param running   **我此刻跑在哪个槽**（`gw_ota_running_slot()`）
 * @param uptime_s  本次已稳定运行的秒数（`gw_fault_port_uptime_s()`）
 * @param len       写进 confirmed_len 的值（诊断用，见下方说明）
 * @param crc       写进 confirmed_crc 的值（诊断用）
 * @return 见 gw_ota_confirm_r_t
 *
 * ⚠️ `len` / `crc` 当前**没有任何代码读来做判断**，只是诊断信息。
 *    传本分区声明的长度与 CRC 即可。
 *    要让"确认"具备防篡改含义（确认的必须是此刻在跑的那份二进制），
 *    需要链接期符号来算运行中固件的大小与 CRC —— 本项目尚未提供。
 */
gw_ota_confirm_r_t gw_ota_confirm_running(const gw_ota_io_t *io,
                                          gw_ota_slot_t running,
                                          uint32_t uptime_s,
                                          uint32_t len, uint32_t crc);

/**
 * 我此刻跑在哪个槽？（自定位）
 *
 * 判据：**本函数自己的链接地址**落在哪个槽区间里 —— 这是编译期事实，
 * 不依赖任何寄存器或运行时设置。
 *
 * @return GW_OTA_SLOT_A / GW_OTA_SLOT_B；判断不出来时返回 **GW_OTA_SLOT_NONE**
 *
 * ⚠️ 返回 NONE 是**如实回答"我不知道"**，不是错误：
 *      - 主机单元测试（PC 地址不在槽区间里）
 *      - 旧的单镜像构建（链接在 0x08000000，缺陷 L 那种配置）
 *      - 与槽位化布局不一致的其它链接脚本
 *    调用方**必须**把 NONE 当作"无权确认"处理（`gw_ota_confirm_is_mine()` 会返回 0）
 *    —— 宁可永不确认，也不能替一个不知道是谁的固件签字。
 *
 * ⚠️ 为什么不用 `SCB->VTOR`：VTOR 反映的是"向量表在哪"，它会被改
 *    （引导程序跳转前会设、应用也可能重定位）；而"我这份代码被链接到哪"
 *    才是"我是谁"的可靠答案。
 */
gw_ota_slot_t gw_ota_running_slot(void);

/** 确认结果的字符串（日志用） */
const char *gw_ota_confirm_r_str(gw_ota_confirm_r_t r);

/* ---------------- 读写元数据（经 io 回调） ---------------- */
int gw_ota_meta_load(const gw_ota_io_t *io, gw_ota_meta_t *out);
int gw_ota_meta_save(const gw_ota_io_t *io, const gw_ota_meta_t *m);

/* ---------------- 字符串（日志用） ---------------- */
const char *gw_ota_state_str(gw_ota_state_t st);
const char *gw_ota_err_str(gw_ota_err_t e);
const char *gw_ota_slot_str(gw_ota_slot_t s);

#endif /* GW_OTA_H */
