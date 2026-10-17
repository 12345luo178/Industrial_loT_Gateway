/**
 * @file    gw_param.h
 * @brief   Flash 参数掉电保存（多槽位磨损均衡 + 回读校验，纯逻辑可测）
 *
 * ============================================================================
 *  为什么参数存储值得单独一个模块，还要用磨损均衡？
 *
 *  1) Flash 的擦写寿命有限。STM32F103 大容量产品约 **1 万次**。
 *     如果参数固定放在一页里，"改一次参数擦一次页"，
 *     现场调试时改上百次参数就把那一页写坏了 —— 而且是**不可逆**的坏。
 *
 *  2) 掉电可能发生在写入过程中的任何时刻。
 *     如果写入流程是"擦除 -> 写入"，那么擦除之后到写入完成之前掉电，
 *     参数就永久丢了（旧值已被擦掉，新值没写进去）。
 *
 *  ----------------------------------------------------------------------------
 *  两条设计对策
 *
 *  **对策一：多槽轮换写入（磨损均衡）**
 *    把参数区分成 N 个槽，每次写入用**新槽**，而不是擦掉旧槽重写。
 *    擦除次数被摊薄到 1/N。并且只在参数真正变化时才写 ——
 *    这一条本身就把写入次数降了两个数量级。
 *
 *  **对策二：永不破坏旧数据（掉电安全）**
 *    写入顺序刻意设计成"新数据先落到独立槽位，校验通过后才更新序号"。
 *    旧槽从头到尾没被碰过，因此：
 *
 *      任意时刻掉电  ->  至少有一个槽是完好的  ->  一定能启动
 *
 *    槽用满之后才擦除"最旧的那个"，而那个槽的数据早已被更新的槽取代。
 *
 *  ----------------------------------------------------------------------------
 *  槽格式与"有效性"判定
 *
 *    偏移  长度  字段        说明
 *    ----  ----  ----------  ------------------------------------------------
 *    0     4     magic       0x50415241 ("PARA")，用来识别"这里写过东西"
 *    4     4     seq         单调递增序号，**最大的那个是最新的**
 *    8     2     version     参数结构版本（便于将来升级兼容）
 *    10    4     len         数据字节数
 *    14    4     crc32       覆盖 magic..len 全部槽头字段 + data
 *    18    N     data[]      参数数据（N <= GW_PARAM_MAX_DATA）
 *
 *  ⚠️ 字段区间必须互不重叠。曾经把 len 放在偏移 8（占 8..11）、
 *     version 放在偏移 10（占 10..11），两者重叠：
 *     写 len=16 后再写 version=7，会把 len 覆盖成 7，
 *     表现为"写入成功但读取校验必然失败"，而槽头 dump 看起来几乎正确。
 *     .c 文件里有 _Static_assert 把这个约束钉死。
 *
 *  三关校验，任一不过即视为无效槽：magic 正确、len 合理、CRC32 正确。
 *  擦除后的 Flash 全 0xFF，magic 必然不匹配，所以"空槽"天然被识别。
 *
 *  ----------------------------------------------------------------------------
 *  本模块是纯逻辑：所有 Flash 访问通过 gw_flash_if_t 回调注入，
 *  因此磨损均衡、掉电恢复、损坏槽跳过等逻辑都能在 PC 上单元测试。
 *  真机上只需实现 3 个薄函数（读/写/擦除）。
 * ============================================================================
 */
#ifndef GW_PARAM_H
#define GW_PARAM_H

#include <stdint.h>

/* ---------------- 规模上限 ---------------- */
#define GW_PARAM_MAX_SLOTS      8u      /* 最多槽位数（受页数限制） */
#define GW_PARAM_MAX_DATA       256u    /* 单份参数最大字节数 */
#define GW_PARAM_SLOT_OVERHEAD  20u     /* 槽头长度（见下方布局表） */
#define GW_PARAM_SLOT_SIZE      (GW_PARAM_SLOT_OVERHEAD + GW_PARAM_MAX_DATA)

/* 槽头魔数："PARA"，用来识别"这个槽被写过" */
#define GW_PARAM_MAGIC          0x50415241u

/* ---------------- 结果码 ---------------- */
typedef enum {
    GW_PARAM_OK             = 0,
    GW_PARAM_ERR_NO_VALID   = -1,   /* 没有任何有效槽（首次上电或全部损坏） */
    GW_PARAM_ERR_PARAM      = -2,   /* 入参非法 */
    GW_PARAM_ERR_FLASH      = -3,   /* Flash 读/写/擦除失败 */
    GW_PARAM_ERR_VERIFY     = -4,   /* 写入后回读校验失败 */
    GW_PARAM_ERR_TOO_BIG    = -5,   /* 数据超过 GW_PARAM_MAX_DATA */
    GW_PARAM_ERR_NO_SPACE   = -6    /* 找不到可写槽（理论上不会发生） */
} gw_param_err_t;

/* ---------------- Flash 抽象接口 ----------------
 *
 * 三个函数就够了。语义要求：
 *   - read：按字节读，地址任意（不必对齐）
 *   - write：写之前目标区域必须是已擦除状态（0xFF）。
 *            必须**拒绝**把 1 位写成 1（即只能 1->0），
 *            否则说明调用方忘了擦除 —— 静默接受会掩盖 bug。
 *   - erase_slot：擦除第 slot_index 个槽所在区域
 */
typedef struct {
    int      (*read)(void *ctx, uint32_t offset, uint8_t *buf, uint32_t len);
    int      (*write)(void *ctx, uint32_t offset, const uint8_t *buf, uint32_t len);
    int      (*erase_slot)(void *ctx, uint8_t slot_index);
    uint8_t  (*slot_count)(void *ctx);      /* 实际可用槽位数 */
    uint32_t (*slot_offset)(void *ctx, uint8_t slot_index); /* 第 i 槽的起始偏移 */
    void     *ctx;
} gw_flash_if_t;

/* ---------------- 统计量（诊断 + 实测数据来源） ---------------- */
typedef struct {
    uint32_t writes;            /* 成功写入次数 */
    uint32_t erases;            /* 擦除次数（磨损均衡效果的度量） */
    uint32_t seq_wraps;         /* 序号回绕次数（理论上极少） */
    uint32_t corrupt_slots;     /* 累计发现无效槽的次数 */
    uint32_t verify_fails;      /* 回读校验失败次数 */
    uint32_t last_write_ms;     /* 最近一次写入耗时（由调用方填入） */
} gw_param_stat_t;

/* ---------------- 存储上下文 ---------------- */
typedef struct {
    gw_flash_if_t iface;
    uint32_t      next_seq;     /* 下一个要使用的序号（= 已知最大序号 + 1） */
    uint8_t       last_slot;    /* 最近一次成功写入的槽号 */
    uint8_t       valid;        /* 1 = 已加载或写入过有效参数 */
    gw_param_stat_t stat;
} gw_param_t;

/* ==================================================================
 *  接口
 * ================================================================== */

/**
 * 初始化并扫描所有槽，找出最新的有效槽
 *
 * @param p      存储上下文
 * @param iface  Flash 抽象接口（会被复制进上下文）
 *
 * @return GW_PARAM_OK           找到有效槽，p->next_seq / last_slot 已就绪
 *         GW_PARAM_ERR_NO_VALID 没有任何有效槽（首次上电的**正常**情况）
 *         GW_PARAM_ERR_PARAM    入参非法
 *
 * @note 首次上电返回 ERR_NO_VALID 不是错误，调用方应加载默认参数。
 *       这样区分"没有参数"和"参数损坏"，便于现场诊断。
 */
gw_param_err_t gw_param_init(gw_param_t *p, const gw_flash_if_t *iface);

/**
 * 读取当前有效参数
 *
 * @param p       存储上下文（需先 init 成功）
 * @param buf     输出缓冲
 * @param buf_size 缓冲大小
 * @param out_len 实际长度
 * @param out_ver 参数结构版本
 */
gw_param_err_t gw_param_load(gw_param_t *p, uint8_t *buf, uint32_t buf_size,
                             uint32_t *out_len, uint16_t *out_ver);

/**
 * 保存参数（掉电安全 + 磨损均衡）
 *
 * 流程（每一步都可被掉电打断，且任意时刻至少一个有效槽存在）：
 *   1) 选目标槽：优先用"空槽"；无空槽则擦除**最旧**的槽
 *   2) 擦除目标槽（只有这一步会破坏数据，而它破坏的是最旧的那份）
 *   3) 写入槽头 + 数据
 *   4) 回读校验（magic + CRC32 + 逐字节比对）
 *   5) 只有校验通过，才把 next_seq 推进
 *
 * @return GW_PARAM_OK 成功
 *         GW_PARAM_ERR_TOO_BIG / ERR_FLASH / ERR_VERIFY / ERR_PARAM
 */
gw_param_err_t gw_param_save(gw_param_t *p, const uint8_t *data, uint32_t len,
                             uint16_t version);

/**
 * 检查是否有需要保存的参数（用于"仅变化时写入"的省寿命优化）
 *
 * @return 1 = 与当前存储内容不同（需要保存）；0 = 相同（无需写入）
 *
 * 现场调试时反复下发同样的参数是很常见的，靠这个函数把写入次数降到最低。
 */
int gw_param_needs_save(const gw_param_t *p, const uint8_t *data, uint32_t len);

/** 取统计量 */
const gw_param_stat_t *gw_param_stats(const gw_param_t *p);

/** 错误码转字符串（日志用） */
const char *gw_param_strerror(gw_param_err_t e);

#endif /* GW_PARAM_H */
