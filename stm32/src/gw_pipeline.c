/**
 * @file    gw_pipeline.c
 * @brief   数据管道实现（纯逻辑，可在 PC 上单元测试）
 */
#include "gw_pipeline.h"

#include <string.h>

/* 显式声明 dht22_filter_push 的依赖：它定义在 dht22_port.h 里，
   通过 gw_pipeline.h 间接引入。这里不重复包含，只做注释说明，
   避免"以为是别的模块提供的"这类误解。 */

void gw_pipeline_init(gw_pipeline_t *p, uint16_t fw_major, uint16_t fw_minor)
{
    if (p == 0) {
        return;
    }
    memset(p, 0, sizeof(*p));

    dht22_filter_init(&p->dht);

    p->fw_major      = fw_major;
    p->fw_minor      = fw_minor;
    p->mqtt_status   = IREG_MQTT_ST_DISCONNECTED;
    p->master_online_bmp = 0u;
    p->master_err    = 0u;
}

void gw_pipeline_tick(gw_pipeline_t *p, uint32_t tick_ms)
{
    if (p == 0) {
        return;
    }

    /* 运行时间：按毫秒累计，满 1000ms 进位到秒。
       用累加而不是读 RTC，是为了让本模块不依赖任何时钟源。
       累加器放在结构体里（p->ms_acc），不用函数级 static ——
       static 会让多个实例互相干扰，也破坏单元测试的独立性。 */
    p->ms_acc += tick_ms;
    while (p->ms_acc >= 1000u) {
        p->ms_acc -= 1000u;
        p->uptime_s++;
    }

    /* 采样节拍 */
    p->sample_acc_ms += tick_ms;
    if (p->sample_acc_ms >= GW_SAMPLE_PERIOD_MS) {
        /* 减去一个周期而不是清零：如果 tick_ms 偶尔较大（任务被抢占），
           这样能保证长期平均采样周期正确，而不是每次都被拉长。 */
        p->sample_acc_ms -= GW_SAMPLE_PERIOD_MS;
        if (p->sample_acc_ms >= GW_SAMPLE_PERIOD_MS) {
            p->sample_acc_ms = 0u;      /* 落后太多则丢弃积压，避免"追赶风暴" */
        }
        p->sample_due = 1u;
    }
}

int gw_pipeline_take_sample_due(gw_pipeline_t *p)
{
    if (p == 0 || p->sample_due == 0u) {
        return 0;
    }
    p->sample_due = 0u;
    return 1;
}

dht22_err_t gw_pipeline_sample_dht(gw_pipeline_t *p, const dht22_hw_t *hw, uint8_t retry)
{
    dht22_err_t err;

    if (p == 0) {
        return DHT22_ERR_PARAM;
    }

    if (hw == 0) {
        /* 硬件层不可用（例如 DWT 初始化失败、或外接传感器未接）：
           按一次失败处理，让状态位如实反映"这个传感器现在没有数据"。
           注意不清空已有读数 —— 宁可用旧值，也不要让上位机读到 0。
           理由：0℃ 是一个合法读数，会被上位机当成真实值。 */
        p->dht.stat_reads++;
        if (p->dht.fail_streak < 0xFFu) {
            p->dht.fail_streak++;
        }
        return DHT22_ERR_PARAM;
    }

    /* 传感器驱动内部完成：读取 -> 校验（失败重试）-> 写入滑动窗口 -> 更新统计。
       这是数据进入管道的唯一路径，不存在"重复 push"的可能。 */
    err = dht22_read_and_filter(hw, &p->dht, retry);
    return err;
}

void gw_pipeline_set_sticky(gw_pipeline_t *p, uint16_t bit)
{
    if (p == 0) {
        return;
    }
    p->sticky_flags |= bit;
}

void gw_pipeline_clear_sticky(gw_pipeline_t *p, uint16_t bit)
{
    if (p == 0) {
        return;
    }
    p->sticky_flags &= (uint16_t)(~bit);
}

uint16_t gw_pipeline_status_word(const gw_pipeline_t *p)
{
    uint16_t w = 0u;

    if (p == 0) {
        return 0u;
    }

    /* ---- 数据有效性类 ---- */
    if (p->dht.valid != 0u && p->dht.fail_streak < GW_DHT_FAIL_LIMIT) {
        w |= IREG_STATUS_BIT_DHT_OK;
    }

    /* ---- CAN 控制器状态 ---- */
    if (p->can_ok != 0u) {
        w |= IREG_STATUS_BIT_CAN_OK;
    }

    /* ---- 上云链路 ---- */
    if (p->mqtt_status == IREG_MQTT_ST_CONNECTED) {
        w |= IREG_STATUS_BIT_MQTT_OK;
        w |= IREG_STATUS_BIT_WIFI_OK;   /* MQTT 通了说明链路整体是通的 */
    }
    /* 注意：MQTT 状态由 ESP32 侧上报，Wi-Fi 与 MQTT 状态合并在一个状态字里
       上报是刻意的简化 —— 分开需要 ESP32 上报两个独立字段，
       而对使用者来说"能不能上云"才是关心的。若要分开，扩展 IREG
       预留区即可，接口不用改。 */

    /* ---- 参数校验 ---- */
    if ((p->sticky_flags & IREG_STATUS_BIT_PARAM_OK) != 0u) {
        w |= IREG_STATUS_BIT_PARAM_OK;
    }

    /* ---- sticky 位：曾经发生过的事件 ---- */
    w |= (uint16_t)(p->sticky_flags
                    & (IREG_STATUS_BIT_IWDG_RESET | IREG_STATUS_BIT_HARDFAULT));

    /* ---- 主站 ---- */
    if (p->master_online_bmp != 0u) {
        w |= IREG_STATUS_BIT_MASTER_OK;
    }

    return w;
}

void gw_pipeline_sync_regs(const gw_pipeline_t *p, mb_regs_t *regs)
{
    if (p == 0 || regs == 0) {
        return;
    }

    /* ---------------- 环境采集 ----------------
     * 温度是 int16（可能为负），湿度是 uint16。
     *
     * ⚠️ 三种状态必须分清（2026-10-10 修正）：
     *   ① 有过有效读数（含"现在故障"） -> 写**上一次有效值**
     *      不新鲜这件事由 IREG_STATUS_BIT_DHT_OK=0 表达；
     *   ② **从未**有过有效读数           -> 写哨兵值 GW_DHT_REG_UNAVAILABLE
     *      原来这条路径什么都不写，寄存器停在 0 —— 而上位机无法把
     *      "0.0℃" 与 "根本没有数据" 区分开。本文件上面那段注释
     *      （"宁可用旧值也不要填 0"）要防的正是这件事，但实现漏了这条路径。 */
    if (p->dht.valid != 0u) {
        regs->input[IREG_TEMP_X10] = (uint16_t)p->dht.temp_x10;
        regs->input[IREG_HUMI_X10] = p->dht.humi_x10;
    } else {
        regs->input[IREG_TEMP_X10] = (uint16_t)GW_DHT_REG_UNAVAILABLE;
        regs->input[IREG_HUMI_X10] = (uint16_t)GW_DHT_REG_UNAVAILABLE;
    }

    /* ---------------- CAN 通道 ---------------- */
    {
        uint8_t i;
        for (i = 0u; i < 8u; i++) {
            regs->input[IREG_CAN_CH1 + i] = p->can_ch[i];
        }
    }

    /* ---------------- 状态位 ---------------- */
    regs->input[IREG_DEV_STATUS] = gw_pipeline_status_word(p);

    /* ---------------- 运行时间（32 位，高字在前） ---------------- */
    {
        uint16_t hi, lo;
        gw_split_u32(p->uptime_s, &hi, &lo);
        regs->input[IREG_UPTIME_HI] = hi;
        regs->input[IREG_UPTIME_LO] = lo;
    }

    /* ---------------- 通信统计（32 位各占两字） ---------------- */
    {
        uint16_t hi, lo;

        gw_split_u32(p->mb_rx, &hi, &lo);
        regs->input[IREG_MB_RX_LO] = lo;
        regs->input[IREG_MB_RX_HI] = hi;

        gw_split_u32(p->mb_err, &hi, &lo);
        regs->input[IREG_MB_ERR_LO] = lo;
        regs->input[IREG_MB_ERR_HI] = hi;

        gw_split_u32(p->can_rx, &hi, &lo);
        regs->input[IREG_CAN_RX_LO] = lo;
        regs->input[IREG_CAN_RX_HI] = hi;

        gw_split_u32(p->mb_exc, &hi, &lo);
        regs->input[IREG_MB_EXC_LO] = lo;
        regs->input[IREG_MB_EXC_HI] = hi;
    }

    /* ---------------- 上云链路 ---------------- */
    regs->input[IREG_MQTT_STATUS]   = p->mqtt_status;
    regs->input[IREG_OFFLINE_DEPTH] = p->offline_depth;
    regs->input[IREG_OFFLINE_DROP]  = p->offline_drop;

    /* ---------------- 版本与主站 ---------------- */
    regs->input[IREG_FW_VER_MAJOR]  = p->fw_major;
    regs->input[IREG_FW_VER_MINOR]  = p->fw_minor;
    regs->input[IREG_MASTER_ONLINE] = p->master_online_bmp;
    regs->input[IREG_MASTER_ERR]    = p->master_err;
}

void gw_pipeline_sync_from_regs(gw_pipeline_t *p, const mb_regs_t *regs)
{
    if (p == 0 || regs == 0) {
        return;
    }

    /* 目前没有需要从寄存器表反向同步到管道的字段。
       保留这个接口是为了让"配置变更"路径有明确归属：
       将来若加入"上位机写入采集周期"之类的功能，
       在这里实现，而不是让应用层到处直接读 regs->holding[]。 */
}

void gw_split_u32(uint32_t v, uint16_t *hi, uint16_t *lo)
{
    if (hi != 0) {
        *hi = (uint16_t)(v >> 16);
    }
    if (lo != 0) {
        *lo = (uint16_t)(v & 0xFFFFu);
    }
}

uint32_t gw_join_u32(uint16_t hi, uint16_t lo)
{
    return ((uint32_t)hi << 16) | (uint32_t)lo;
}
