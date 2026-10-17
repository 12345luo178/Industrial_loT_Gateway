/**
 * @file    gw_selfcheck.h
 * @brief   上板自检框架（纯逻辑，可在 PC 上单元测试）
 *
 * ============================================================================
 *  这个模块解决什么问题
 *
 *  "首次点亮"这件事在嵌入式里有个特点：**一次面对多个未知**。
 *  上电后没输出，可能是时钟配错、DWT 没使能、串口引脚错、
 *  波特率算错、或者程序压根没跑起来 —— 现象都是"什么都没有"。
 *
 *  没有系统方法的做法是"挨个猜"，而猜的顺序往往是最费时的那种。
 *  本模块的做法：**把"该确认什么"和"失败了先查什么"固化成代码**，
 *  于是每次上板都跑同一套、输出同一份报告。
 *
 *  ----------------------------------------------------------------------------
 *  三个设计决定
 *
 *  **① 失败要"阻断下游"，而不是全部报错。**
 *
 *     如果时钟没配对，DHT22 一定读不出来、Modbus 一定波特率错、
 *     CAN 一定不通。全部报"失败"会让人以为有 5 个独立问题，
 *     而实际上只有 1 个根因。
 *
 *     所以每一项都可以声明"我依赖谁"：依赖项失败时，
 *     本项状态是 **SKIP（跳过）** 并注明原因，而不是 FAIL。
 *     报告里一眼就能看出"真正要修的是哪一个"。
 *
 *  **② 结果里必须带"实际值"，不能只有"通过/失败"。**
 *
 *     "DWT 计时失败"没有用；"DWT 计时失败：1000µs 实测 3400µs（偏差 240%）"
 *     直接指出了方向。所以每项都要有一个数值型实测值 + 单位。
 *
 *  **③ 每项都要有"失败了先查什么"。**
 *
 *     这是本模块最有价值的部分：把排查经验**放进代码**而不是文档里。
 *     上板的人（包括几个月后的我自己）不需要翻手册。
 *
 *  ----------------------------------------------------------------------------
 *  在 PC 上能测什么
 *
 *  能测：状态流转、依赖阻断、汇总统计、报告生成、边界（NULL/重复注册）。
 *  不能测：具体的硬件检查本身（那些在 `port/gw_selfcheck_port.c`）。
 *
 *  这个划分让"自检逻辑本身对不对"可以被验证 ——
 *  否则一个写错的自检会给出错误的安心感，比没有自检更糟。
 * ============================================================================
 */
#ifndef GW_SELFCHECK_H
#define GW_SELFCHECK_H

#include <stdint.h>

/* ==================================================================
 *  自检项 ID
 *
 *  顺序即**执行顺序**，也基本就是"排查顺序"：
 *  前面的不过，后面的没有意义。
 *
 *  与 `doc/最小上板验证包.md` 的 L0~L7 对应（见注释）。
 * ================================================================== */
typedef enum {
    GW_SC_CLOCK      = 0,   /* L0  时钟与外设（最基础，一切的前提） */
    GW_SC_GPIO       = 1,   /* L0  GPIO 读写（能真正翻转引脚） */
    GW_SC_DWT        = 2,   /* L2  微秒计时（依赖时钟） */
    GW_SC_FLASH      = 3,   /* L5  Flash 读写擦（依赖时钟） */
    GW_SC_PARAM      = 4,   /* L5  参数存取（依赖 Flash） */
    GW_SC_DHT22      = 5,   /* L3  DHT22 单总线（依赖 DWT） */
    GW_SC_RS485      = 6,   /* L4  RS485 收发（依赖 GPIO + DWT） */
    GW_SC_CAN        = 7,   /* -   CAN 控制器（依赖时钟） */
    GW_SC_ESP32      = 8,   /* -   ESP32 串口（依赖时钟） */
    GW_SC_FAULT      = 9,   /* L7  故障记录（依赖 .noinit） */
    GW_SC_COUNT      = 10
} gw_sc_id_t;

/* ==================================================================
 *  单项结果
 * ================================================================== */
typedef enum {
    GW_SC_PASS  = 0,        /* 通过 */
    GW_SC_FAIL  = 1,        /* 失败 */
    GW_SC_SKIP  = 2,        /* 跳过（依赖项失败） */
    GW_SC_WARN  = 3         /* 通过但可疑（例如偏差在容差边缘） */
} gw_sc_status_t;

/** 单项的检查函数 */
typedef struct {
    const char *name;               /* 项名，报告里显示 */
    /**
     * 执行检查
     *
     * @param out_value 输出实测值（可为 NULL）
     * @param out_unit  输出单位字符串（可为 NULL）
     * @return GW_SC_PASS / FAIL / WARN
     *
     * ⚠️ 本函数**不应阻塞超过 100ms**（除了明确标注的耗时段）：
     *    自检本身跑在主循环里，卡住太久会让人以为"卡死了"。
     */
    gw_sc_status_t (*run)(int32_t *out_value, const char **out_unit);

    /** 依赖的前置项（GW_SC_COUNT 表示无依赖） */
    gw_sc_id_t depends_on;

    /** 期望值（用于报告里显示"期望 vs 实际"；无期望填 0） */
    int32_t    expect;

    /** 容差（绝对值；0 = 不做容差判断） */
    int32_t    tol;

    /**
     * 失败时"先查什么" —— **本模块最有价值的部分**
     *
     * 多行文本（用 '\n' 分隔），报告里直接打印。
     */
    const char *hint;
} gw_sc_item_t;

/* ==================================================================
 *  上下文
 * ================================================================== */
typedef struct {
    const gw_sc_item_t *items;      /* 项表（长度 = GW_SC_COUNT） */
    gw_sc_status_t      status[GW_SC_COUNT];
    int32_t             value[GW_SC_COUNT];
    const char         *unit[GW_SC_COUNT];
    uint32_t            ran;        /* 实际执行过的项数 */
    uint8_t             inited;
} gw_selfcheck_t;

/* ==================================================================
 *  接口
 * ================================================================== */

/**
 * 初始化
 *
 * @param s     上下文
 * @param items 项表（**长度必须为 GW_SC_COUNT**，顺序必须与 gw_sc_id_t 一致）
 * @return 0 成功；<0 失败（项表为空或某项的 name/run 为空）
 */
int gw_selfcheck_init(gw_selfcheck_t *s, const gw_sc_item_t *items);

/**
 * 执行全部自检
 *
 * @return 通过的项数
 *
 * 行为：
 *   1) 按 ID 顺序遍历
 *   2) 依赖项不是 PASS/WARN 时 -> 本项标 SKIP，**不调用 run**
 *   3) 否则调用 run，记录状态与实测值
 *
 * @note 依赖项为 SKIP 时，本项也 SKIP（递归阻断）。
 */
uint32_t gw_selfcheck_run(gw_selfcheck_t *s);

/** 只跑某一项（用于"我想单独重测一下 CAN"） */
gw_sc_status_t gw_selfcheck_run_one(gw_selfcheck_t *s, gw_sc_id_t id);

/** 取某项状态 */
gw_sc_status_t gw_selfcheck_status(const gw_selfcheck_t *s, gw_sc_id_t id);

/** 取某项实测值 */
int32_t gw_selfcheck_value(const gw_selfcheck_t *s, gw_sc_id_t id);

/**
 * 取**第一个真正的失败项**（跳过 SKIP）
 *
 * @return 失败项 ID；全部通过时返回 GW_SC_COUNT
 *
 * ⚠️ 为什么需要它：报告里可能有 5 个 FAIL + 3 个 SKIP，
 *    但**真正要修的第一个**只有一个。这个函数直接指出它 ——
 *    省掉"哪一个是根因"的判断。
 */
gw_sc_id_t gw_selfcheck_first_failure(const gw_selfcheck_t *s);

/** 统计 */
uint32_t gw_selfcheck_count(const gw_selfcheck_t *s, gw_sc_status_t st);

/**
 * 判断整体是否"可以继续往下验证"
 *
 * @return 1 = 基础项都通过（CLOCK/GPIO/DWT/FLASH）；
 *         0 = 有基础项失败，后面的检查没有意义
 *
 * ⚠️ 这个判断的目的是**避免浪费时间**：
 *    时钟没对的时候去调 DHT22 时序是纯浪费 ——
 *    因为 DHT22 的微秒延时是按时钟算的，时钟错了它必然读不出来。
 */
int gw_selfcheck_foundation_ok(const gw_selfcheck_t *s);

/**
 * 生成报告
 *
 * @param s    上下文
 * @param out  输出缓冲
 * @param cap  容量
 * @return 写入的字符数（不含 '\0'）
 *
 * 输出格式（对齐、便于直接贴进测试报告）：
 *
 *     [SELFCHECK] 通过 7 / 失败 2 / 跳过 1
 *     [ OK ] CLOCK      168000000 Hz  (期望 72000000 ±720000)
 *     [FAIL] DWT             3400 us  (期望 1000 ±10)
 *            先查: DWT 未使能？确认 gw_dwt_init() 在 SystemClock_Config 之后调用
 *                  ...
 *     [SKIP] DHT22               -   (依赖 DWT 未通过)
 */
uint16_t gw_selfcheck_report(const gw_selfcheck_t *s, char *out, uint16_t cap);

/** 状态字符串（报告与日志用） */
const char *gw_selfcheck_status_str(gw_sc_status_t st);

/** 项名字符串（越界时返回 "?"） */
const char *gw_selfcheck_id_str(gw_sc_id_t id);

#endif /* GW_SELFCHECK_H */
