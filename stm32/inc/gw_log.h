/**
 * @file    gw_log.h
 * @brief   分级日志（纯逻辑：环形缓冲 + 等级过滤 + 多路输出分发）
 *
 * ============================================================================
 *  为什么日志要单独做一层，而不是直接调用串口打印
 *
 *  现场设备出了问题，第一件事是"看日志"。但嵌入式日志有几个现实约束：
 *
 *    1) 串口可能没接 —— 而问题恰恰发生在没接串口的现场
 *    2) 串口可能被 Modbus 占用 —— 不能因为打日志打乱 3.5 字符的帧界
 *    3) 打日志本身可能很慢（115200 下一行 60 字节要 5ms）
 *       —— 在中断或时序敏感路径里打日志会把功能搞坏
 *    4) 现场没有调试器 —— 出问题时只能事后从设备里把日志取出来
 *
 *  所以本模块的设计是：**日志先写进 RAM 环形缓冲，再由调用方决定何时输出**。
 *
 *    - 写日志 = 往环形缓冲塞一个 16 字节结构（几十纳秒，可以放进大部分路径）
 *    - 输出日志 = 由主循环/低优先级任务从缓冲取出并送到串口/Flash/ESP32
 *
 *  这样做的好处：**打日志的耗时与输出设备的速度解耦**。
 *  串口再慢也不会拖慢业务逻辑，最坏情况只是环形缓冲被覆盖。
 *
 *  ----------------------------------------------------------------------------
 *  为什么不存字符串，只存"消息 ID + 参数"
 *
 *  一条日志如果存完整字符串，至少要 64 字节；32 条就是 2 KB RAM。
 *  而 STM32F103 只有 64 KB，日志不该占 3%。
 *
 *  本模块的做法：每条日志存 **16 字节**
 *  （时间戳 4 + 等级/模块/行号打包 4 + 参数1 4 + 参数2 4），
 *  字符串文本由**烧在 Flash 里的格式表**决定。
 *  读取时用 ID 去查表，拼出可读文本。
 *
 *  代价是“读出日志需要一个查表函数”，收益是 RAM 占用远低于存字符串。
 *
 *  ⚠️ 这里曾经写的是“只存 8 字节（时间戳 + 等级 + 模块 + 行号 + 参数）”，
 *     那句话**自相矛盾** —— 8 字节装不下这五项。
 *     而当时代码确实只存了 8 字节，即**没有存参数**，
 *     于是 flush 重放时参数只能是 0（见 gw_log_rec_t 的说明）。
 *
 *     ⚠️ 这种不一致比文档写错更危险：
 *        读文档的人会以为“参数在”，从而不去验证它。
 *  对只有 64 KB RAM 的芯片来说这个交换很划算。
 *
 *  ----------------------------------------------------------------------------
 *  覆盖策略：**丢最旧**
 *
 *  与 CAN 报文的缓冲不同（那里丢最新），日志必须丢最旧：
 *  排查问题时最关心的是"**刚刚**发生了什么" ——
 *  崩溃前最后几条日志是定位根因的关键，绝不能因为缓冲满而丢掉。
 *  历史日志价值低得多，丢掉可以接受。
 * ============================================================================
 */
#ifndef GW_LOG_H
#define GW_LOG_H

#include <stdint.h>

/* ---------------- 等级 ----------------
 *
 * 等级数值越小越严重。这样"等级 <= 阈值"就是"要不要记录"，
 * 比较逻辑最简单，也不会出现"反了"的低级错误。
 */
typedef enum {
    GW_LOG_ERROR = 0,   /* 功能已失效，需要立即处理 */
    GW_LOG_WARN  = 1,   /* 还能工作，但已偏离正常（例如重试、降级） */
    GW_LOG_INFO  = 2,   /* 正常的状态变化（连接建立、参数保存） */
    GW_LOG_DEBUG = 3    /* 仅开发期使用，量产默认关闭 */
} gw_log_level_t;

/* 默认运行时阈值：量产固件只记 INFO 及以上，DEBUG 不占空间 */
#define GW_LOG_DEFAULT_LEVEL    GW_LOG_INFO

/* ---------------- 规模 ----------------
 *
 * 32 条 × 8 字节 = 256 字节 RAM。
 * 为什么是 32 而不是更大：崩溃前最后几条最关键，
 * 再往前的历史价值急剧下降。256 字节换"能定位问题"已经很划算。
 */
#define GW_LOG_RING_DEPTH       32u

/* ---------------- 参数个数上限 ---------------- */
#define GW_LOG_MAX_ARGS         2u

/* ---------------- 模块 ID ----------------
 *
 * 用枚举而不是字符串：字符串要占 Flash，而且比较慢。
 * 打印时由 gw_log_module_str() 翻译成可读名字。
 */
typedef enum {
    GW_LOG_MOD_APP    = 0,
    GW_LOG_MOD_RS485  = 1,
    GW_LOG_MOD_MODBUS = 2,
    GW_LOG_MOD_DHT22  = 3,
    GW_LOG_MOD_CAN    = 4,
    GW_LOG_MOD_UPLINK = 5,
    GW_LOG_MOD_PARAM  = 6,
    GW_LOG_MOD_FRAME  = 7,
    GW_LOG_MOD_FAULT  = 8,
    GW_LOG_MOD_SYS    = 9,
    GW_LOG_MOD_COUNT  = 10
} gw_log_module_t;

/* ---------------- 单条日志（16 字节） ----------------
 *
 * 布局：
 *   ts_ms    4 字节  时间戳（毫秒，32 位在 49 天回绕，够用）
 *   packed   4 字节  level(2) | module(6) | line(24)
 *   arg0     4 字节  参数 1
 *   arg1     4 字节  参数 2
 *
 * ----------------------------------------------------------------------------
 *  ⚠️⚠️ arg0/arg1 是**后加的**，原因是一个真实缺陷
 *
 *  原来只有 8 字节（ts_ms + packed），**没有存参数**。
 *  而 sink_text 的签名要求传 arg0/arg1，所以 gw_log_flush() 在读回
 *  记录时只能硬编码：
 *
 *      l->io.sink_text(..., gw_log_rec_line(rec.packed), 0u, 0u);
 *                                                       ^^^^^^
 *
 *  后果：**任何从环形缓冲重放的日志，参数都会变成 0。**
 *
 *  上板时正好暴露了它 —— 启动时那行
 *      GW_LOG_WRN(&g_log, GW_LOG_MOD_FAULT, boot_cause, boot_count);
 *  先被"写入即输出"打了一次（参数正确），
 *  紧接着 dump_log_history() 从缓冲重放又打了一次（参数成了 0 0）：
 *
 *      [W][0][FAULT:443] 2 4      <- 写入时输出，正确
 *      [W][0][FAULT:443] 0 0      <- flush 重放，参数丢了
 *
 *  而"崩溃后 dump 历史"**正是这个功能存在的理由** ——
 *  那些参数恰恰是排查故障最需要的数据。丢了它们，
 *  dump 出来的历史只剩"崩在哪一行"，价值大打折扣。
 *
 *  ⚠️ 代价（如实记录）：记录从 8 字节变 16 字节，
 *     32 条环形缓冲从 256 字节变 512 字节。多花 256 字节 RAM，
 *     换"重放的日志参数不丢"。
 *
 *     为什么不存 16 位参数：GW_LOG_WRN(..., boot_cause, boot_count)
 *     里 boot_count 是 uint32；GW_LOG_ERR(..., id, reason) 这类调用
 *     也传 32 位。截断会再次变成"静默丢数据"。
 * ----------------------------------------------------------------------------
 *
 * 为什么把 level/module/line 三个字段塞进 32 位：
 *   独立字段要 2+1+4（含对齐填充）= 8 字节。
 *   位域手工打包而不是用 C 的 `unsigned x : 6`：
 *   位域的内存布局依赖编译器，换编译器/换优化级别就可能变，
 *   而这份数据结构将来要传给 ESP32 做日志转发。显式移位没有这个问题。
 */
typedef struct {
    uint32_t ts_ms;
    uint32_t packed;
    uint32_t arg0;
    uint32_t arg1;
} gw_log_rec_t;

/* ---------------- 回调 ----------------
 *
 * 提供三个可选的输出通道。设计成"可选"而不是"必须"：
 *   - 量产设备可能只有串口
 *   - 带外部 Flash 的版本可以额外落盘
 *   - 接了 ESP32 的版本可以转发到云端
 * 三个都用函数指针注入，PC 测试时用一个假的收集器。
 */
typedef struct {
    /** 时间戳来源（毫秒）；为 NULL 时用内部累加计数 */
    uint32_t (*now_ms)(void *ctx);

    /** 输出一条（通常是串口）；为 NULL 表示不输出，只留在环形缓冲里 */
    void (*sink)(void *ctx, const gw_log_rec_t *rec);

    /**
     * 格式化并输出（由移植层实现，因为它要用 printf/sprintf）
     * 为 NULL 时应用层会跳过文本输出，只保留结构化记录
     */
    void (*sink_text)(void *ctx, gw_log_level_t lv, gw_log_module_t mod,
                      uint16_t line, uint32_t arg0, uint32_t arg1);

    void *ctx;
} gw_log_io_t;

/* ---------------- 统计量 ----------------
 *
 * `dropped` 必须暴露：如果只看到"日志正常"却不知道丢了多少，
 * 就会误以为"没记录说明没发生"。**丢掉日志比丢数据更危险**，
 * 因为它会让人对系统状态产生错误的信心。
 */
typedef struct {
    uint32_t written;           /* 累计写入条数 */
    uint32_t dropped;           /* 因缓冲满而覆盖掉的条数 */
    uint32_t filtered;          /* 被等级过滤掉的条数 */
    uint32_t errors;            /* ERROR 级条数（用于快速判断"有没有大问题"） */
    uint32_t warns;             /* WARN 级条数 */
} gw_log_stat_t;

/* ---------------- 上下文 ---------------- */
typedef struct {
    gw_log_rec_t  ring[GW_LOG_RING_DEPTH];
    uint16_t      head;         /* 下一个写入位置 */
    uint16_t      count;        /* 当前有效条数 */
    uint16_t      read_idx;     /* 读取游标（在 count 范围内） */

    gw_log_level_t level;       /* 运行时阈值：只记录 <= 该等级的 */
    gw_log_io_t    io;
    uint32_t       ts_acc;      /* 内部时间累加器（无 now_ms 时使用） */

    gw_log_stat_t  stat;
    uint8_t        inited;
} gw_log_t;

/* ==================================================================
 *  接口
 * ================================================================== */

/**
 * 初始化
 * @param level 运行时阈值（GW_LOG_ERROR ~ GW_LOG_DEBUG）
 */
int gw_log_init(gw_log_t *l, const gw_log_io_t *io, gw_log_level_t level);

/**
 * 记录一条日志（核心函数）
 *
 * @param l     上下文
 * @param lv    等级
 * @param mod   模块
 * @param line  行号（用 __LINE__ 传入，便于定位到具体语句）
 * @param arg0  附加参数（可为 0）
 * @param arg1  附加参数（可为 0）
 *
 * @note 本函数**尽量短**：一次等级比较 + 一次环形缓冲写入。
 *       输出（可能很慢）由 gw_log_flush() 在合适的时机做。
 */
void gw_log_write(gw_log_t *l, gw_log_level_t lv, gw_log_module_t mod,
                  uint16_t line, uint32_t arg0, uint32_t arg1);

/* ---------------------------------------------------------------------------
 *  ★ 日志互斥钩子（**弱符号**；缺陷 Y，2026-10-11）
 *
 *  ⚠️ 为什么需要：`gw_log_write()` 直接改环形的 `head`/`count`/`stat`，
 *     而 **RTOS 下任何任务都可能写日志**（这正是它的用途）。并发写会：
 *       · 两个任务写同一个槽 -> **丢记录**
 *       · head/count 交错 -> 记录顺序错乱
 *       · 两边的 sink 同时调 `HAL_UART_Transmit`（**不可重入**）-> 输出交错/超时
 *     ⚠️ 最刺眼的是：丢的往往是**故障前后那几条** —— 恰恰是排查最需要的。
 *
 *  做法：`gw_log_write()` 与 `gw_log_read()` 各自"取锁 -> 做事 -> 放锁"，
 *  **不嵌套**（一次公开调用只取一次），所以用普通互斥量即可。
 *  裸机 / 测试里是空实现；RTOS 由 `port/gw_rtos.c` 给强定义。
 *  ⚠️ 弱定义在本文件对应的 `src/gw_log.c` 里 —— 少一边不会报错，只会静默失去保护。
 * ------------------------------------------------------------------------- */
void gw_log_guard_enter(void);
void gw_log_guard_exit(void);

/** 推进内部时间（无 now_ms 回调时使用） */
void gw_log_tick(gw_log_t *l, uint32_t elapsed_ms);

/** 设置运行时等级阈值（现场调试时可由上位机下发） */
void gw_log_set_level(gw_log_t *l, gw_log_level_t level);

/** 取当前等级阈值 */
gw_log_level_t gw_log_get_level(const gw_log_t *l);

/**
 * 从环形缓冲取一条（非阻塞，按"最旧优先"顺序）
 *
 * @return 1 取到；0 没有更多
 *
 * @note 取完一圈后需要调用 gw_log_rewind() 才能重新读。
 *       这样设计是为了让"读"变成显式操作 ——
 *       避免了"读游标被意外推进导致日志丢失"这类问题。
 */
int gw_log_read(gw_log_t *l, gw_log_rec_t *out);

  /**
   * 把读取游标拨回**最旧**那条（供崩溃后 dump 用）
   *
   * ========================================================================
   *  ⚠⚠ 这两个函数的配合方式很容易用错
   *
   *  它们有**两种**用法，后果完全不同：
   *
   *    (a) 周期性输出新日志  → 只调 gw_log_flush()
   *        每轮主循环调一次，只吐**新增的**那几条。
   *
   *    (b) 唯一一次的崩溃后 dump → gw_log_rewind() 然后 gw_log_flush()
   *        把**全部**历史（最多 GW_LOG_RING_DEPTH 条）输出一遍。
   *
   *  ⚠️ 坑：在**周期路径**里写成 (b) 就是每轮重打整个环形缓冲 ——
   *     串口刷屏，看起来像“日志一直在发”，实际是同一批在循环。
   *     这个缺陷在上板时才暴露（PC 测试里每次都先 rewind）。
   *     已补 test_log.c 的 test_flush_no_duplicate 回归用例。
   * ========================================================================
   */
  void gw_log_rewind(gw_log_t *l);

/**
 * 输出全部未输出的日志（由主循环在低优先级时机调用）
 *
 * 会调用 io.sink 与 io.sink_text。输出后游标停在末尾。
 *
 * @return 本次输出的条数
 */
uint16_t gw_log_flush(gw_log_t *l);

/** 取统计量 */
const gw_log_stat_t *gw_log_stats(const gw_log_t *l);

/** 当前缓冲里的日志条数 */
uint16_t gw_log_count(const gw_log_t *l);

/** 解包：取等级 / 模块 / 行号 */
gw_log_level_t  gw_log_rec_level(uint32_t packed);
gw_log_module_t gw_log_rec_module(uint32_t packed);
uint16_t        gw_log_rec_line(uint32_t packed);

/** 打包（供测试与移植层使用） */
uint32_t gw_log_pack(gw_log_level_t lv, gw_log_module_t mod, uint16_t line);

/** 等级转字符串 */
const char *gw_log_level_str(gw_log_level_t lv);

/** 模块转字符串 */
const char *gw_log_module_str(gw_log_module_t mod);

/* ==================================================================
 *  便捷宏
 *
 *  用宏是为了能把 __LINE__ 与 __FILE__ 带进去 ——
 *  没有行号的日志在现场基本没用（"某某出错了"但不知道在哪一行）。
 * ================================================================== */
#define GW_LOG_ERR(l, mod, a0, a1) \
    gw_log_write((l), GW_LOG_ERROR, (mod), (uint16_t)__LINE__, (uint32_t)(a0), (uint32_t)(a1))
#define GW_LOG_WRN(l, mod, a0, a1) \
    gw_log_write((l), GW_LOG_WARN, (mod), (uint16_t)__LINE__, (uint32_t)(a0), (uint32_t)(a1))
#define GW_LOG_INF(l, mod, a0, a1) \
    gw_log_write((l), GW_LOG_INFO, (mod), (uint16_t)__LINE__, (uint32_t)(a0), (uint32_t)(a1))
#define GW_LOG_DBG(l, mod, a0, a1) \
    gw_log_write((l), GW_LOG_DEBUG, (mod), (uint16_t)__LINE__, (uint32_t)(a0), (uint32_t)(a1))

#endif /* GW_LOG_H */
