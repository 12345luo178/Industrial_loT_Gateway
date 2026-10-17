/**
 * @file    gw_pipeline.h
 * @brief   数据管道：把各路采集值汇聚成寄存器映射表的内容
 *
 * ============================================================================
 *  职责边界（高内聚的体现）
 *
 *    DHT22 驱动    负责"把传感器读出来、校验、滤波"        -> 产出 dht22_filtered_t
 *    CAN 驱动      负责"收帧、按 ID 分派"                  -> 产出通道值
 *    计数器        由各层自己维护                          -> 产出原始计数
 *    ↓
 *    **本模块**    负责"把这些值翻译成寄存器表语义"：
 *                    单位换算 -> 寄存器值
 *                    各层状态 -> device_status 位域
 *                    失败次数 -> 状态位与错误码
 *                    32 位计数 -> 高低字拆分
 *    ↓
 *    寄存器表       对外接口契约，主站/从站/上报都读它
 *
 *  为什么单独一层？
 *    如果把"填寄存器"的代码散落在各个任务里，会出现：
 *      - 状态位的置位/清零规则不统一（谁负责清？什么时候清？）
 *      - 32 位量拆分方式不一致（有人高字在前，有人低字在前）
 *      - 单位换算到处重复（温度 ×10 在三个地方各写一遍）
 *    集中一层之后，这些规则只有一个实现，且能单元测试。
 *
 *  纯逻辑：不依赖 HAL、不依赖 FreeRTOS、不依赖真实时钟。
 *          时间由 gw_pipeline_tick(tick_ms) 的调用方推进，可在 PC 上测试。
 * ============================================================================
 */
#ifndef GW_PIPELINE_H
#define GW_PIPELINE_H

#include <stdint.h>

#include "modbus_rtu.h"
#include "modbus_regs.h"
#include "dht22_port.h"

/* ---------------- 采集周期（毫秒） ----------------
 * DHT22 手册要求采样间隔不小于 2 秒，这里取 2.5 秒留余量。
 * 注意：这不是"上报周期"（上报周期是可配置的寄存器 HREG_REPORT_PERIOD）。 */
#define GW_SAMPLE_PERIOD_MS     2500u

/* ---------------- DHT22 连续失败多少次判定"传感器故障" ----------------
 * 判定后清除 IREG_STATUS_BIT_DHT_OK，但**保留上一次的有效读数**
 * （宁可用旧值也不要填 0 —— 0 会被上位机当成"真的 0℃"）。 */
#define GW_DHT_FAIL_LIMIT       3u

/* ---------------- 温湿度寄存器"不可用"哨兵值 ----------------
 *
 * 用途：**开机至今从未取得过有效读数**时，输入寄存器 0/1 写这个值。
 *
 * ⚠️ 为什么不能用 0（这是 2026-10-10 修掉的一个真问题）：
 *    0℃ 与 0%RH **都是合法读数**。原来的实现只在"有有效读数"时才写
 *    那两个寄存器，于是"从来没有读到过"这种情况会让寄存器停在 0 ——
 *    上位机看到的就是 0.0℃ / 0.0%RH，与真实读数无法区分。
 *    本文件上面那段注释早就写明"宁可用旧值也不要填 0"，但实现漏掉了
 *    "一直没有旧值"这条路径。
 *
 * 取值理由：0x8000 在两个解释下都是**不可能的值**，所以能安全地当"不可用"：
 *    当 int16 看 = -32768 -> -3276.8℃（DHT22 量程只有 -40~80℃）
 *    当 uint16 看 = 32768 -> 3276.8%RH（量程只有 0~100%RH）
 *
 * ⚠️ 与"有旧值但传感器故障"是**两种不同状态**，别混：
 *    有旧值 + 故障 -> 保留旧值，靠 IREG_STATUS_BIT_DHT_OK=0 表示"不新鲜"
 *    从无有效值   -> 哨兵值，表示"这里根本没有数据" */
#define GW_DHT_REG_UNAVAILABLE  0x8000u

/* ---------------- DHT22 校验失败时的默认重试次数 ----------------
 * 手册允许的一次采样内补救次数。取 2 意味着最坏情况尝试 3 次，
 * 每次约 18ms 起始信号 + 通信时间，最坏耗时约 60ms ——
 * 对 2.5s 的采样周期完全可接受，也不会让主循环卡顿太久。 */
#define GW_DHT_RETRY_DEFAULT    2u

/* ---------------- 管道上下文 ---------------- */
typedef struct {
    /* 传感器状态 */
    dht22_filtered_t dht;

    /* CAN 通道值（由 CAN 驱动的回调写入） */
    uint16_t can_ch[8];

    /* CAN 控制器是否正常（非 Bus-Off）。置 0 会清 IREG_STATUS_BIT_CAN_OK */
    uint8_t  can_ok;

    /* 运行计数 */
    uint32_t uptime_s;          /* 运行时间（秒） */
    uint32_t mb_rx;             /* Modbus 从站收帧计数 */
    uint32_t mb_err;            /* Modbus 错误帧计数 */
    uint32_t mb_exc;            /* Modbus 异常响应计数 */
    uint32_t can_rx;            /* CAN 收帧计数 */

    /* 上云链路状态（由 ESP32 通信层写入） */
    uint16_t mqtt_status;       /* IREG_MQTT_ST_* */
    uint16_t offline_depth;     /* 断网缓存队列深度 */
    uint16_t offline_drop;      /* 缓存溢出丢包累计 */

    /* 固件版本 */
    uint16_t fw_major;
    uint16_t fw_minor;

    /* 主站状态（由主站模块写入） */
    uint16_t master_online_bmp; /* 在线从机位图 */
    uint16_t master_err;        /* 最近一次错误码 */

    /* sticky 状态位：一旦置位需显式清除（用于"曾经发生过"的告警） */
    uint16_t sticky_flags;      /* IREG_STATUS_BIT_* 的集合 */

    /* ---------------- 内部计时状态 ----------------
     * 全部放在结构体里，不用函数级 static。
     * 原因：static 是隐藏状态 —— 多个实例会互相干扰，
     * 而且单元测试之间会残留，产生"单独跑通过、一起跑失败"的诡异现象。 */
    uint32_t ms_acc;            /* 毫秒累加器，满 1000 进位到 uptime_s */
    uint32_t sample_acc_ms;     /* 采样节拍累加器 */
    uint8_t  sample_due;        /* 1 = 需要采样（由 take_sample_due 读取并清零） */
} gw_pipeline_t;

/**
 * 初始化管道
 * @param p          管道上下文
 * @param fw_major   固件主版本
 * @param fw_minor   固件次版本
 */
void gw_pipeline_init(gw_pipeline_t *p, uint16_t fw_major, uint16_t fw_minor);

/**
 * 心跳推进：累加时间、判断是否需要采样
 *
 * @param p        管道上下文
 * @param tick_ms  距上次调用经过的毫秒数
 *
 * 行为：
 *   - 累加 uptime_s（每满 1000ms 加 1）
 *   - 累加采样节拍；到达 GW_SAMPLE_PERIOD_MS 时置 sample_due = 1
 *   - 应用层看到 sample_due 后去读传感器，读完调用 gw_pipeline_sample_done()
 *
 * @note 本函数不做任何硬件访问 —— 这正是它能在 PC 上测试的原因。
 */
void gw_pipeline_tick(gw_pipeline_t *p, uint32_t tick_ms);

/** 是否需要采样（读取后自动清零 sample_due） */
int  gw_pipeline_take_sample_due(gw_pipeline_t *p);

/**
 * 把一次 DHT22 采样结果并入管道（成功与失败的**唯一**入口）
 *
 * ⚠️ 重要：本函数内部会调用 dht22_read_and_filter()，
 *    它已经把数据写进 p->dht 的滑动窗口了。
 *    所以**不要在外部再调用一次 dht22_filter_push** ——
 *    否则统计与窗口会被记两次（踩过这个坑：失败计数翻倍）。
 *
 * 调用约定：
 *   应用层只调用 gw_pipeline_sample_dht()，然后读 p->dht 得到温湿度。
 *   若硬件层不可用（hw == NULL），会按一次失败处理，并累加失败计数。
 *
 * @param p    管道上下文
 * @param hw   DHT22 硬件接口（来自 dht22_stm32_hw()；为 NULL 表示硬件不可用）
 * @param retry 校验失败时的重试次数
 * @return 本次采样结果（DHT22_OK 或错误码）
 */
dht22_err_t gw_pipeline_sample_dht(gw_pipeline_t *p, const dht22_hw_t *hw, uint8_t retry);

/** 置一个 sticky 状态位（例如 HardFault 发生过） */
void gw_pipeline_set_sticky(gw_pipeline_t *p, uint16_t bit);

/** 清一个 sticky 状态位 */
void gw_pipeline_clear_sticky(gw_pipeline_t *p, uint16_t bit);

/**
 * 把管道里的全部数据写入寄存器表
 *
 * 这是本模块的核心：所有"值 -> 寄存器"的翻译规则都集中在这里。
 *
 * @param p     管道上下文
 * @param regs  寄存器表（由移植层提供，见 rs485_get_regs()）
 *
 * 写入内容：
 *   输入寄存器 0x00~0x1A 全部字段
 *   保持寄存器中属于"设备自身"的只读项（如 OTA 状态）不在此处处理
 */
void gw_pipeline_sync_regs(const gw_pipeline_t *p, mb_regs_t *regs);

/**
 * 从寄存器表同步"可写配置"回管道（反向路径）
 *
 * 目前只同步固件版本之外的少量字段；保留此接口是为了让
 * "配置变更 -> 需要重新读取"的路径也有明确归属，而不是散落在各处。
 */
void gw_pipeline_sync_from_regs(gw_pipeline_t *p, const mb_regs_t *regs);

/** 组装 device_status 位域的值（供测试与上报复用） */
uint16_t gw_pipeline_status_word(const gw_pipeline_t *p);

/** 便捷函数：32 位量拆成高低两个 16 位寄存器值 */
void gw_split_u32(uint32_t v, uint16_t *hi, uint16_t *lo);

/** 便捷函数：由高低字重组成 32 位量（必须与 gw_split_u32 配对使用） */
uint32_t gw_join_u32(uint16_t hi, uint16_t lo);

#endif /* GW_PIPELINE_H */
