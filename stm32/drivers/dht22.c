/**
 * @file    dht22.c
 * @brief   DHT22 单总线温湿度传感器驱动（纯逻辑部分，可在 PC 上单元测试）
 *
 * 本文件不依赖任何 HAL / 寄存器 —— 所有硬件操作通过 dht22_hw_t 的函数指针进来。
 * 因此：
 *   - PC 上可以用假数据完整测试解析、位解码、滤波、错误分类
 *   - 真机上只需替换 hw 的实现，驱动逻辑一行不改
 */
#include "dht22_port.h"

/* ------------------------------------------------------------------
 * 时序常数（依据 DHT22 手册 + 现场实测余量）
 *
 * 位编码原理：DHT22 用"高电平持续时间"区分 0 和 1
 *   位 0 → 高电平 22~30 µs
 *   位 1 → 高电平 68~75 µs
 * 阈值取 50 µs，落在两者正中间，给噪声留最大余量。
 * ------------------------------------------------------------------ */
#define DHT22_BIT_THRESHOLD_US   50u

/* 起始信号：主机拉低至少 18ms（手册要求 1~10ms 也够，但 18ms 更稳） */
#define DHT22_START_LOW_MS       18u

/* 起始信号释放后，等待传感器拉低响应（手册典型 20~40µs，最坏 200µs） */
#define DHT22_RESP_WAIT_US       300u

/* 单次电平等待超时。正常半周期最长约 75µs，取 150µs 留足余量。
   真出现 150µs 不跳变，说明传感器异常或时序被中断打断。 */
#define DHT22_EDGE_TIMEOUT_US    150u

/* 判定"数据线卡死"的阈值：连续读不到任何跳变超过这个时间 */
#define DHT22_STUCK_TIMEOUT_US   5000u

/* 等待电平变为 level，并测量等待耗时。
 *
 * 为什么用"轮询 read + now_us"而不是给硬件层加一个 wait() 回调？
 *   这是单总线驱动最经典、最容易在真机上验证的写法：
 *   计时完全由 now_us 决定，没有额外的状态机耦合。
 *   早先版本加过 wait() 回调，结果驱动的"等待"语义与假硬件的
 *   "分段推进"语义必须严格同步 —— 一旦不同步就出现时序错位，
 *   而且错位后表现为难以理解的乱值。去掉这一层，问题面立刻收敛。
 *
 * ⚠️ 结论用"显式标志 + 独立输出参数"表达，不要让返回值兼任两义。
 *   踩过的坑：若约定"返回耗时、0 表示超时"，那么跳变点恰好落在当前
 *   时刻（例如传感器在 t=0 立即拉低）时会算出 0µs，被误判成超时。
 *
 * @param out_us 成功时写入等待耗时（可能为 0）
 * @return 1 = 等到目标电平；0 = 超时
 */
static int dht22_wait_level(const dht22_hw_t *hw, int level,
                            uint32_t timeout_us, uint32_t *out_us)
{
    uint32_t t0 = hw->now_us(hw->ctx);

    for (;;) {
        uint32_t dt;

        if (hw->read(hw->ctx) == level) {
            if (out_us != 0) { *out_us = hw->now_us(hw->ctx) - t0; }
            return 1;
        }
        dt = hw->now_us(hw->ctx) - t0;
        if (dt >= timeout_us) {
            if (out_us != 0) { *out_us = 0u; }
            return 0;
        }
    }
}

/**
 * 等待失败后判断到底是"偶发超时"还是"数据线卡死"
 *
 * 判据是电平保持的时长，而不是单次采样：
 *   卡死在某个电平 -> 长时间（毫秒级）没有任何跳变
 *   偶发超时       -> 电平其实在动，只是慢/被中断打断
 *
 * 区分这两者很有价值：卡死说明接线或器件问题（重试无意义），
 * 偶发超时说明时序被干扰（重试有效）。错误信息说清了，
 * 现场排障就不用靠猜。
 */
static dht22_err_t dht22_classify_timeout(const dht22_hw_t *hw)
{
    int      lvl = hw->read(hw->ctx);
    uint32_t t0  = hw->now_us(hw->ctx);

    /* 忙等一小段，看这条线上到底有没有跳变。
       阈值取 5ms：远大于任何合法半周期（最长约 75µs），
       又足够小，不会让一次失败的读取卡太久。 */
    while (hw->now_us(hw->ctx) - t0 < DHT22_STUCK_TIMEOUT_US) {
        if (hw->read(hw->ctx) != lvl) {
            return DHT22_ERR_TIMEOUT;       /* 电平在动 -> 只是超时 */
        }
    }
    return (lvl != 0) ? DHT22_ERR_STUCK_HIGH : DHT22_ERR_STUCK_LOW;
}

/* ==================================================================
 * 纯逻辑 1：40 个高电平时长 -> 5 字节
 * ================================================================== */
dht22_err_t dht22_bits_to_bytes(const uint16_t highs_us[40], uint8_t out[5])
{
    uint8_t i;

    if (highs_us == 0 || out == 0) {
        return DHT22_ERR_PARAM;
    }

    for (i = 0u; i < 5u; i++) {
        out[i] = 0u;
    }

    for (i = 0u; i < 40u; i++) {
        if (highs_us[i] > DHT22_BIT_THRESHOLD_US) {
            /* 高电平更长 -> 位 1。MSB 先到：第 i 位落在字节 i/8 的第 (7 - i%8) 位 */
            out[i / 8u] |= (uint8_t)(0x80u >> (i % 8u));
        }
        /* 否则为位 0，该位保持 0，不需要动作 */
    }
    return DHT22_OK;
}

/* ==================================================================
 * 纯逻辑 2：5 字节 -> 温湿度物理量
 * ================================================================== */
dht22_err_t dht22_decode(const uint8_t raw[5], dht22_raw_t *out)
{
    uint8_t  sum;
    uint16_t humi_raw;
    uint16_t temp_raw;

    if (raw == 0 || out == 0) {
        return DHT22_ERR_PARAM;
    }

    /* 校验和 = 前 4 字节之和的低 8 位 */
    sum = (uint8_t)(raw[0] + raw[1] + raw[2] + raw[3]);

    out->raw[0] = raw[0];
    out->raw[1] = raw[1];
    out->raw[2] = raw[2];
    out->raw[3] = raw[3];
    out->raw[4] = raw[4];

    if (sum != raw[4]) {
        out->checksum_ok = 0u;
        out->temp_x10    = 0;
        out->humi_x10    = 0u;
        return DHT22_ERR_CHECKSUM;
    }
    out->checksum_ok = 1u;

    /* 湿度：16 位无符号，×10 定标 */
    humi_raw = (uint16_t)(((uint16_t)raw[0] << 8) | raw[1]);
    out->humi_x10 = humi_raw;

    /* 温度：16 位，bit15 为符号位（1 = 负温度） */
    temp_raw = (uint16_t)(((uint16_t)raw[2] << 8) | raw[3]);
    if (temp_raw & 0x8000u) {
        /* 负温度：清掉符号位后取负。手册规定负温度用补码表示，
           但实际器件输出的是"符号位 + 绝对值"，这里按后者处理并注释清楚，
           便于现场发现不一致时快速定位。 */
        out->temp_x10 = (int16_t)(-((int16_t)(temp_raw & 0x7FFFu)));
    } else {
        out->temp_x10 = (int16_t)temp_raw;
    }

    return DHT22_OK;
}

/* ==================================================================
 * 纯逻辑 3：滑动均值滤波
 * ================================================================== */
void dht22_filter_init(dht22_filtered_t *f)
{
    uint8_t i;

    if (f == 0) {
        return;
    }
    f->temp_x10    = 0;
    f->humi_x10    = 0u;
    f->valid       = 0u;
    f->fail_streak = 0u;
    f->stat_reads  = 0u;
    f->stat_ok     = 0u;
    f->stat_err_crc = 0u;
    f->stat_err_to  = 0u;

    for (i = 0u; i < DHT22_FILTER_WINDOW; i++) {
        f->win_temp[i] = 0;
        f->win_humi[i] = 0u;
    }
    f->win_count = 0u;
    f->win_idx   = 0u;
}

void dht22_filter_push(dht22_filtered_t *f, const dht22_raw_t *sample)
{
    uint8_t i;
    int32_t  temp_sum = 0;
    uint32_t humi_sum = 0u;
    uint8_t  n;

    if (f == 0) {
        return;
    }

    f->stat_reads++;

    if (sample == 0 || sample->checksum_ok == 0u) {
        /* 失败采样：不写入窗口，避免污染均值；只累计失败次数 */
        f->fail_streak++;
        return;
    }

    f->stat_ok++;
    f->fail_streak = 0u;

    /* 环形写入（窗口在结构体内，无隐藏状态） */
    f->win_temp[f->win_idx] = (int32_t)sample->temp_x10;
    f->win_humi[f->win_idx] = (uint32_t)sample->humi_x10;
    f->win_idx = (uint8_t)((f->win_idx + 1u) % DHT22_FILTER_WINDOW);
    if (f->win_count < DHT22_FILTER_WINDOW) {
        f->win_count++;
    }

    n = f->win_count;

    /* 遍历整个窗口求和。
       为什么遍历 DHT22_FILTER_WINDOW 而不是前 n 个？
       因为窗口是环形缓冲，未填满时有效数据分布在 0..n-1，
       填满后有效数据是全部 5 个槽 —— 统一遍历全部槽位，
       未用到的槽恒为 0，不影响求和结果，但边界意图更清楚、更不易改错。
       （用 "遍历前 n 个" 的写法时，若将来改成"不重置已用槽"的实现就会算错。） */
    for (i = 0u; i < DHT22_FILTER_WINDOW; i++) {
        temp_sum += f->win_temp[i];
        humi_sum += f->win_humi[i];
    }

    /* 整数均值 + 四舍五入（加 n/2 再整除），避免累计截断误差。
       温度可能为负，负数需要减 n/2 —— 否则 -1.5 会朝 0 取整而不是向下。 */
    f->temp_x10 = (int16_t)((temp_sum >= 0)
                            ? ((temp_sum + (int32_t)(n / 2u)) / (int32_t)n)
                            : ((temp_sum - (int32_t)(n / 2u)) / (int32_t)n));
    f->humi_x10 = (uint16_t)((humi_sum + (uint32_t)(n / 2u)) / (uint32_t)n);
    f->valid    = 1u;
}

/* ==================================================================
 * 硬件时序：读取一次（含校验重读）
 *
 * ⚠️⚠️ 缺陷 AF（2026-10-13 板上定位）：**响应窗口必须关中断**
 *
 *   症状：FreeRTOS 构建下每次采样都返回 `DHT22_ERR_STUCK_HIGH(-6)`，
 *         且 `stat_err_crc` / `stat_err_to` 都是 0 —— 因为 STUCK_* 这类
 *         "数据线卡死"错误按设计**不计入**通信类计数，于是界面上表现为
 *         "采样在跑、所有错误计数都是 0"，很容易被误判成"传感器没接"。
 *
 *   根因：本文件原来把"起始信号 + 响应窗口 + 40 bit 采样"整段写成一条
 *         顺序流，全程没有任何临界区。裸机下没问题（主循环是唯一的
 *         执行流），但在 FreeRTOS 下任务随时可能被更高优先级任务抢占：
 *
 *           释放总线 -> 传感器 20~40µs 后开始应答
 *                     -> 整帧（80+80µs 握手 + 40×~100µs 数据）约 4.2ms
 *
 *         只要在"释放总线后 300µs 内"发生一次超过 4.2ms 的抢占，
 *         整个响应就过去了；等本任务回来，总线早已回到空闲高电平，
 *         于是 `dht22_classify_timeout()` 看到"5ms 内电平从未变化"
 *         -> 如实报 STUCK_HIGH。**它报的不是"没接"，而是"这一刻线上没有跳变"。**
 *
 *   判据（实验，而不是推断）：把整段采样放进 `taskENTER_CRITICAL()`
 *         后，6/6 次采样全部 OK，寄存器出 28.9℃ / 63.5%RH、DHT_OK=1。
 *         关中断即恢复 => 抢占是真凶，硬件（接线/上拉/器件）无问题。
 *
 *   为什么只保护响应窗口、不保护起始信号：
 *         起始信号是**主机主动把线拉低 18ms**。这段被打断只会让低电平
 *         变得更长（DHT22 对主机拉低时长只有下限 1ms，没有严格上限），
 *         语义不变。而保护它要多关 18ms 中断 —— 代价大、收益为零。
 *
 *   ⚠️ 关中断时长核算（RTOS 下每 2.5s 采样一次）：
 *         单次响应窗口 ≈ 4.2ms（+ 30µs 释放延时）。
 *         正常情况第一次就成功 -> 每次采样关中断约 4.3ms
 *         => 占空比约 0.17%，对 SysTick 计数的影响可忽略。
 *         （对比"整段保护"方案：18ms 起始信号 ×3 次重试 ≈ 55ms，
 *           会丢掉约 55 个 tick，让系统时基漂移 2%+，不可接受。）
 *
 *   ⚠️ 保护是**可选**的（`enter_critical == NULL` 即退化为原行为）：
 *         PC 上的单元测试注入假引脚，不提供该回调 -> 保持可测。
 * ================================================================== */

/**
 * 响应窗口（**必须在 enter_critical 保护下运行**）
 *
 * 从"释放总线"开始，到 40 bit 采样结束 —— 这段时间里传感器的时序
 * 由它自己驱动，主机只做被动测量，任何抢占都会让测量失真。
 */
static dht22_err_t dht22_read_response(const dht22_hw_t *hw, dht22_raw_t *out)
{
    uint16_t highs[40];
    uint8_t  bytes[5];
    uint8_t  i;

    hw->write(hw->ctx, 1);                          /* 释放总线，由上拉拉高 */
    hw->delay_us(hw->ctx, 30u);                     /* 手册要求 20~40µs */

    hw->set_in(hw->ctx);                            /* 切换为输入，读传感器 */

    /* ---------- 等传感器响应：拉低 ---------- */
    if (dht22_wait_level(hw, 0, DHT22_RESP_WAIT_US, 0) == 0) {
        /* 没等到拉低。可能是"根本没接"（恒高）或"被短路"（恒低）或偶发超时，
           交给统一分类函数判断，避免在这里写一堆特例分支。 */
        dht22_err_t e = dht22_classify_timeout(hw);
        return (e == DHT22_ERR_TIMEOUT) ? DHT22_ERR_NO_RESPONSE : e;
    }

    /* 响应的低电平约 80µs，随后拉高约 80µs，之后开始 40 个数据位 */
    if (dht22_wait_level(hw, 1, DHT22_EDGE_TIMEOUT_US, 0) == 0) {
        return dht22_classify_timeout(hw);
    }
    if (dht22_wait_level(hw, 0, DHT22_EDGE_TIMEOUT_US, 0) == 0) {
        return dht22_classify_timeout(hw);
    }

    /* ---------- 抓取 40 个位 ----------
     *
     * DHT22 的位编码（手册）：
     *   每一位 = 约 50µs 低电平（分隔）+ 一段高电平
     *   高电平 26~28µs -> 位 0
     *   高电平 70µs    -> 位 1
     * 所以判据是"高电平持续时长"，驱动必须测高电平，不能测低电平
     * （低电平是固定的分隔时间，不含信息）。
     *
     * 循环结构：先等上升沿（进入高电平），再测高电平持续到下降沿的时长。
     */
    for (i = 0u; i < 40u; i++) {
        uint32_t dur = 0u;

        if (dht22_wait_level(hw, 1, DHT22_EDGE_TIMEOUT_US, 0) == 0) {
            return dht22_classify_timeout(hw);
        }
        /* 测量高电平持续时长：等到下降沿，耗时即为高电平时长 */
        if (dht22_wait_level(hw, 0, DHT22_EDGE_TIMEOUT_US, &dur) == 0) {
            return dht22_classify_timeout(hw);
        }
        highs[i] = (uint16_t)((dur > 0xFFFFu) ? 0xFFFFu : dur);
    }

    /* ---------- 位解码 + 字节解析 ---------- */
    (void)dht22_bits_to_bytes(highs, bytes);
    return dht22_decode(bytes, out);
}

static dht22_err_t dht22_read_once(const dht22_hw_t *hw, dht22_raw_t *out)
{
    dht22_err_t e;

    /* ---------- 1. 起始信号（不保护，见文件头"为什么只保护响应窗口"） ---------- */
    hw->set_out(hw->ctx);
    hw->write(hw->ctx, 0);                          /* 拉低 */

    /* 拉低持续 18ms。用 delay_us 是因为接口只有微秒延时；
       18ms = 18000µs，调用方实现需保证准确。 */
    hw->delay_us(hw->ctx, (uint32_t)DHT22_START_LOW_MS * 1000u);

    /* ---------- 2. 响应窗口（进入临界区） ----------
     *
     * ⚠️ enter/exit 必须**成对且无早退**：所以把带多个 return 的响应
     *    解析拆到 dht22_read_response() 里，由本函数保证配对 ——
     *    如果直接把 enter/exit 塞进原来的顺序流，任何一个 timeout
     *    分支的 return 都会漏掉 exit_critical，后果是**中断永久关闭**。
     *    （这正是"临界区要包住整个函数、而不是散在分支里"的经典理由。） */
    if (hw->enter_critical != 0) {
        hw->enter_critical(hw->ctx);
    }
    e = dht22_read_response(hw, out);
    if (hw->exit_critical != 0) {
        hw->exit_critical(hw->ctx);
    }

    return e;
}

void dht22_filter_note_error(dht22_filtered_t *f, dht22_err_t err)
{
    if (f == 0) {
        return;
    }
    switch (err) {
    case DHT22_ERR_CHECKSUM:
        f->stat_err_crc++;
        break;
    case DHT22_ERR_TIMEOUT:
    case DHT22_ERR_NO_RESPONSE:
        f->stat_err_to++;
        break;
    default:
        /* STUCK_LOW / STUCK_HIGH / PARAM 属于硬件或配置故障，
           不计入"通信类错误"，由上层通过返回值单独处理 */
        break;
    }
}

dht22_err_t dht22_read(const dht22_hw_t *hw, dht22_raw_t *out, uint8_t retry,
                       dht22_filtered_t *f)
{
    dht22_err_t err = DHT22_ERR_PARAM;
    uint8_t     attempt;

    if (hw == 0 || out == 0) {
        return DHT22_ERR_PARAM;
    }
    /* 所有回调都必须提供，缺一个就没法工作 —— 早失败好过运行到一半崩 */
    if (hw->set_out == 0 || hw->set_in == 0 || hw->write == 0 ||
        hw->read == 0 || hw->delay_us == 0 || hw->now_us == 0) {
        return DHT22_ERR_PARAM;
    }

    /* retry 表示"失败后再试几次"，所以总尝试次数 = retry + 1 */
    for (attempt = 0u; attempt <= retry; attempt++) {
        err = dht22_read_once(hw, out);

        if (err == DHT22_OK) {
            return DHT22_OK;
        }

        if (f != 0) {
            dht22_filter_note_error(f, err);
        }

        /* 硬件故障（短路/断路）重试没有意义，直接返回，省掉 3 次 18ms 的等待 */
        if (err == DHT22_ERR_STUCK_LOW || err == DHT22_ERR_STUCK_HIGH) {
            return err;
        }

        /* 两次读取之间要留间隔。手册要求采样间隔不小于 2 秒，
           但重试属于"同一次采样内的补救"，间隔 20ms 即可，
           既不违反器件限制，也不会让上层任务等太久。 */
        if (attempt < retry) {
            hw->delay_us(hw->ctx, 20000u);
        }
    }
    return err;
}

dht22_err_t dht22_read_and_filter(const dht22_hw_t *hw,
                                  dht22_filtered_t *f,
                                  uint8_t retry)
{
    dht22_raw_t raw;
    dht22_err_t err;

    if (f == 0) {
        return DHT22_ERR_PARAM;
    }

    err = dht22_read(hw, &raw, retry, f);

    /* 无论成功失败都并入滤波器：
       成功 -> 写入窗口参与均值
       失败 -> push(NULL) 只累加 fail_streak，不污染均值 */
    dht22_filter_push(f, (err == DHT22_OK) ? &raw : 0);

    return err;
}

const char *dht22_strerror(dht22_err_t err)
{
    switch (err) {
    case DHT22_OK:              return "OK";
    case DHT22_ERR_NO_RESPONSE: return "no response from sensor";
    case DHT22_ERR_TIMEOUT:     return "edge wait timeout";
    case DHT22_ERR_CHECKSUM:    return "checksum mismatch";
    case DHT22_ERR_PARAM:       return "invalid parameter";
    case DHT22_ERR_STUCK_LOW:   return "data line stuck low (short or dead sensor)";
    case DHT22_ERR_STUCK_HIGH:  return "data line stuck high (not connected)";
    default:                    return "unknown error";
    }
}
