/**
 * @file    gw_can_port.c
 * @brief   bxCAN 硬件层实现（STM32F103 CAN1，500 kbps）
 *
 * ============================================================================
 *  接收路径：中断只搬结构体，映射在主循环
 *
 *    CAN1_RX0 中断
 *        |  只做一件事：HAL_CAN_GetRxMessage 把报文搬进环形缓冲
 *        v
 *    [报文环形缓冲 16 条]
 *        |  can_port_poll() 在主循环里取出来
 *        v
 *    gw_can_core_on_frame()  滤波 + 通道映射（纯逻辑，101 项测试覆盖）
 *
 *  为什么中断里不做映射：
 *    映射本身很快（几十条指令），但在中断里做映射意味着
 *    "中断的执行时间取决于应用配置" —— 一旦现场配了很多映射项，
 *    中断时间就变长，可能影响 Modbus 的 3.5 字符帧界要求。
 *    把映射放到主循环，中断时间就是恒定的"搬一个结构体"。
 *
 *  环形缓冲为什么是 16 条而不是更大：
 *    CAN 报文在中断里被搬走，主循环每轮都会清空。
 *    16 条已经能吸收"一波突发"，再大只是在掩盖"主循环卡住了"这个问题。
 *    丢包会计数并上报，所以"够不够用"能从统计看出来。
 * ============================================================================
 */
#include "gw_can_port.h"

/* 注意包含顺序：gw_config.h 使用 HAL 的 GPIO_PIN_n 宏与 GPIOB 端口宏，
   因此必须先包含 HAL 头、再包含它。gw_config.h 里有守卫检查这一点。 */
#include "stm32f1xx_hal.h"
#include "gw_config.h"

/* ------------------------------------------------------------------
 * 外部依赖：CubeMX 生成的对象
 *
 * 如果链接时提示 hcan 未定义，说明 CubeMX 里 CAN1 没使能，
 * 回去检查 .ioc 配置（并且要确认 PB8/PB9 的重映射已开启）。
 * ------------------------------------------------------------------ */
extern CAN_HandleTypeDef hcan;

/* ------------------------------------------------------------------
 * 位时序参数（500 kbps @ APB1 = 36 MHz）
 *
 * 公式：波特率 = APB1 / (Prescaler × 位时间TQ总数)
 *       位时间TQ总数 = SYNC(1) + BS1 + BS2
 *
 *   36 MHz / (Prescaler × TQ总数) = 500 k
 *   -> Prescaler × TQ总数 = 72
 *
 * 取 Prescaler = 4，TQ 总数 = 18 = SYNC(1) + BS1(15) + BS2(2)
 *   采样点 = (1 + 15) / 18 = 88.9%
 *
 * ------------------------------------------------------------------
 *  ⚠️ bxCAN 的硬性取值范围（写死在硬件里，超了 CubeMX 也不让选）
 *
 *      BS1（TimeSeg1）  1..16     4 位字段
 *      BS2（TimeSeg2）  1..8      3 位字段
 *      SJW              1..4
 *      Prescaler        1..1024
 *      总 TQ 建议 8..25
 *
 *  ⚠️ 本项目文档曾经推荐过 **BS1 = 20**（想凑 24 tq 拿 87.5%），
 *     那是**选不出来的** —— BS1 上限 16。现场表现为
 *     "CubeMX 的 BS1 下拉框里只有 1..16，找不到文档说的值"。
 *     这是一个真实的文档错误，由用户实际配置时发现。
 *
 * ------------------------------------------------------------------
 *  为什么选 88.9% 而不是别的
 *
 *  在 36 MHz / 500 k 的整数解里（Prescaler × 总TQ = 72），
 *  采样点的可选值只有有限几个。按"接近工业惯例 87.5%"排序：
 *
 *      Prescaler=4,  BS1=15, BS2=2  -> 18 tq, 88.9%   ← 本实现
 *      Prescaler=9,  BS1=6,  BS2=1  ->  8 tq, 87.5%   （TQ 太少，容忍度差）
 *      Prescaler=18, BS1=2,  BS2=1  ->  4 tq, 75.0%   （TQ 太少，不推荐）
 *
 *  88.9% 是最接近 87.5% 的**可用**取值，且总 TQ = 18 落在
 *  CiA 推荐的 8..25 区间中部，对晶振偏差与传播延迟的容忍度最好。
 *
 *  采样点为什么不能随便选：
 *      偏早（< 70%）容易受上升沿抖动影响
 *      偏晚（> 90%）留给"相位误差重同步"的余量不足
 * ------------------------------------------------------------------ */
#define CAN_PRESCALER       4u
#define CAN_BS1             15u
#define CAN_BS2             2u
#define CAN_SJW             1u

/* ==================================================================
 *  ⚠️⚠️ 交给 HAL 的必须是**编码宏**，不能是上面这些裸 TQ 数
 *
 *  这是 2026-10-10 用 SWD 读寄存器抓到的真缺陷：
 *
 *      hcan.Init.TimeSeg1 = CAN_BS1;        (15，裸数字)
 *
 *  HAL 的 CAN_InitTypeDef 里这三个字段要的是**带位移的编码值**
 *  （stm32f1xx_hal_can.h 的 CAN_BS1_15TQ = 14<<16 等），
 *  传裸数字的后果是它们全部落进 BTR 的**预分频字段**：
 *
 *      BTR = SJW(1) | BS1(15) | BS2(2) | (PRESC-1)(3) = 0x0000000F
 *      -> 预分频=15、BS1=0、BS2=0 —— 波特率完全不是 500kbps
 *      （正确值应为 0x001E0003）
 *
 *  而**编译期校验查不出来**：它算的是"裸常数之间的自洽"，
 *  与"HAL 真正写进 BTR 的值"是两回事 —— 这正是"判据与被测对象
 *  用同一个错误假设"的典型案例。
 *
 *  现在两头都钉住：
 *    1) 这里用编码宏（与裸常数一一对应）
 *    2) 用 _Static_assert 把宏与裸常数绑死，改了一个不改另一个就编译不过
 *    3) 自检里回读 BTR，用**硬件位布局**算期望值来比对（见 can_port_config_ok）
 * ================================================================== */
_Static_assert((CAN_SJW_1TQ  >> 24) == (CAN_SJW - 1u),  "SJW 宏与常量不一致");
_Static_assert((CAN_BS1_15TQ >> 16) == (CAN_BS1 - 1u),  "BS1 宏与常量不一致");
_Static_assert((CAN_BS2_2TQ  >> 20) == (CAN_BS2 - 1u),  "BS2 宏与常量不一致");

#define CAN_SJW_ENC         CAN_SJW_1TQ
#define CAN_BS1_ENC         CAN_BS1_15TQ
#define CAN_BS2_ENC         CAN_BS2_2TQ

/* 期望的波特率（仅用于编译期校验，不参与运行） */
#define CAN_TARGET_BITRATE  500000u

/* 编译期校验：位时序换算出来的波特率必须等于目标值。
 *
 * 这类"参数之间必须自洽"的约束写成断言最划算 ——
 * 改预分频却忘了改 BS1/BS2 是配 CAN 时最常见的错误，
 * 而症状是"完全通信不了"，很难一眼看出是位时序算错了。 */
#define CAN_APB1_HZ         36000000u
#define CAN_TQ_TOTAL        (1u + CAN_BS1 + CAN_BS2)
_Static_assert((CAN_APB1_HZ / (CAN_PRESCALER * CAN_TQ_TOTAL)) == CAN_TARGET_BITRATE,
               "CAN 位时序算出来的波特率不等于目标值，检查 Prescaler/BS1/BS2");
_Static_assert(CAN_BS1 >= 1u && CAN_BS1 <= 16u, "BS1 超出 bxCAN 允许范围");
_Static_assert(CAN_BS2 >= 1u && CAN_BS2 <= 8u,  "BS2 超出 bxCAN 允许范围");
_Static_assert(CAN_SJW >= 1u && CAN_SJW <= 4u,  "SJW 超出 bxCAN 允许范围");
_Static_assert(CAN_PRESCALER >= 1u && CAN_PRESCALER <= 1024u,
               "Prescaler 超出 bxCAN 允许范围");

/* ------------------------------------------------------------------
 * 报文环形缓冲
 * ------------------------------------------------------------------ */
#define CAN_RX_RING_SIZE    16u
#define CAN_RX_RING_MASK    (CAN_RX_RING_SIZE - 1u)

_Static_assert((CAN_RX_RING_SIZE & CAN_RX_RING_MASK) == 0u,
               "CAN_RX_RING_SIZE 必须是 2 的幂");

static gw_can_frame_t    s_ring[CAN_RX_RING_SIZE];
static volatile uint16_t s_head = 0u;
static volatile uint16_t s_tail = 0u;

/* ------------------------------------------------------------------
 * 内部状态
 * ------------------------------------------------------------------ */
static gw_can_core_t  s_core;
static can_port_stat_t s_stats;
static uint8_t        s_inited = 0u;

/* 记住当前的筛选器配置：Bus-Off 恢复时需要原样重配 */
static uint16_t s_filt_id   = 0u;
static uint16_t s_filt_mask = 0u;

/* 前向声明 */
static int core_reinit_cb(void *ctx);

/* ==================================================================
 *  内部：核心恢复回调
 *
 *  gw_can_core 的回调签名是 `int (*)(void *ctx)`，而本模块的
 *  can_port_reinit() 是 `int (void)`。两者不能直接互相赋值 ——
 *  把函数指针强转成另一个签名再调用是**未定义行为**，
 *  在 ARM 上通常"恰好能跑"，但编译器一旦优化调用约定就会出问题。
 *
 *  所以老老实实写一个签名匹配的包装函数。
 *  多这几行换来的是"不依赖未定义行为"，很划算。
 * ================================================================== */
static int core_reinit_cb(void *ctx)
{
    (void)ctx;
    return can_port_reinit();
}

/* ==================================================================
 *  内部：筛选器配置
 *
 *  bxCAN 的 32 位筛选器把"标准帧 ID"放在高 16 位的高 11 位上。
 *  所以要把 11 位 ID 左移 5 位再放进 FilterIdHigh。
 *
 *  为什么用 32 位掩码模式而不是 16 位列表模式：
 *    32 位掩码模式能表达"只比较 ID 的某些位"（例如只关心高 8 位），
 *    这样一条筛选器就能覆盖一组 ID（如 0x100~0x1FF），
 *    对"多个节点按区间分配 ID"的现场很实用。
 *    16 位列表模式只能精确匹配，一组 ID 要占多个筛选器组。
 *
 *  ⚠️ 若现场用精确匹配（一个 ID 一条筛选器），
 *     把掩码设成 0x7FF 即可 —— 本函数的行为就等价于列表模式。
 * ================================================================== */
static void can_config_filter(uint16_t id, uint16_t mask)
{
    CAN_FilterTypeDef f;

    f.FilterIdHigh         = (uint32_t)(id << 5);
    f.FilterIdLow          = 0u;
    f.FilterMaskIdHigh     = (uint32_t)(mask << 5);
    f.FilterMaskIdLow      = 0u;
    f.FilterFIFOAssignment = CAN_RX_FIFO0;
    f.FilterBank           = 0u;
    /* 掩码模式：FilterMode 用 IDMASK。
       注意 HAL 的宏名是 CAN_FILTERMODE_IDMASK（不是 ID_MASK）。 */
    f.FilterMode           = CAN_FILTERMODE_IDMASK;
    f.FilterScale          = CAN_FILTERSCALE_32BIT;
    f.FilterActivation     = CAN_FILTER_ENABLE;
    f.SlaveStartFilterBank = 14u;   /* 单 CAN 实例：该字段无意义，填默认值 */

    (void)HAL_CAN_ConfigFilter(&hcan, &f);
}

/* ==================================================================
 *  内部：环形缓冲
 * ================================================================== */
static void ring_put(const gw_can_frame_t *f)
{
    uint16_t next = (uint16_t)((s_head + 1u) & CAN_RX_RING_MASK);

    if (next == s_tail) {
        /* 满：丢弃**最新**的一条并计数。
         *
         * 与 ESP32 那边的"丢最旧"不同，这里刻意丢最新：
         *   ESP32 是字节流，丢中间字节会让后续帧失去边界；
         *   而 CAN 是**报文**流，每条报文自带完整边界，
         *   丢哪一条都不会影响其他报文的解析。
         *
         *   在这种情况下，"保留最早的一批"更有价值 ——
         *   溢出的原因通常是主循环被卡住了一段时间，
         *   而卡住期间**最先到达**的几条往往包含状态变化
         *   （例如故障发生的那一刻），比后面的重复周期数据更值得留。
         */
        s_stats.rx_ring_drops++;
        return;
    }
    s_ring[s_head] = *f;
    s_head = next;
}

static int ring_get(gw_can_frame_t *out)
{
    if (s_tail == s_head) {
        return 0;
    }
    *out = s_ring[s_tail];
    s_tail = (uint16_t)((s_tail + 1u) & CAN_RX_RING_MASK);
    return 1;
}

/* ==================================================================
 *  内部：把 HAL 的报文结构转换成本项目的硬件无关表示
 *
 *  ⚠️ 数据段不在 CAN_RxHeaderTypeDef 里。
 *     `HAL_CAN_GetRxMessage(hcan, fifo, &hdr, data)` 把"头部"和"数据"
 *     分成两个参数传出 —— 头部结构里只有 ID/DLC/RTR/时间戳这些元信息。
 *     所以本函数必须额外接收一个 data 指针。
 *
 *     一开始只传 hdr 并访问 hdr->Data，编译器报
 *     "'CAN_RxHeaderTypeDef' has no member named 'Data'"。
 * ------------------------------------------------------------------ */
static void convert_frame(const CAN_RxHeaderTypeDef *hdr, const uint8_t *data,
                          gw_can_frame_t *out)
{
    uint8_t i;

    if (hdr->IDE == CAN_ID_EXT) {
        out->id       = hdr->ExtId;
        out->extended = 1u;
    } else {
        out->id       = hdr->StdId;
        out->extended = 0u;
    }
    out->rtr = (hdr->RTR == CAN_RTR_REMOTE) ? 1u : 0u;

    /* DLC 可能大于 8（经典 CAN 不会，但 HAL 字段是 4 位，防御一下） */
    out->dlc = (hdr->DLC > GW_CAN_MAX_DLC) ? GW_CAN_MAX_DLC : (uint8_t)hdr->DLC;

    /* 先清零再拷贝：即使 dlc < 8，data 的其余字节也是确定值，
       不会把上一次的残留数据带出去（那会让日志与诊断输出难以解读）。 */
    for (i = 0u; i < GW_CAN_MAX_DLC; i++) {
        out->data[i] = 0u;
    }
    for (i = 0u; i < out->dlc; i++) {
        out->data[i] = data[i];
    }
}

/* ==================================================================
 *  对外接口
 * ================================================================== */

can_port_status_t can_port_init(uint16_t filt_id, uint16_t filt_mask)
{
    /* ---- 1) 初始化纯逻辑核心 ----
       把恢复回调指向包装函数 core_reinit_cb（它再调 can_port_reinit），
       这样 Bus-Off 恢复不需要上层介入。 */
    if (gw_can_core_init(&s_core, filt_id, filt_mask,
                         core_reinit_cb, 0) != 0) {
        gw_can_core_mark_failed(&s_core);
        return CAN_PORT_ERR_PARAM;
    }

    s_filt_id   = filt_id;
    s_filt_mask = filt_mask;

    s_head = 0u;
    s_tail = 0u;

    s_stats.rx_isr_frames   = 0u;
    s_stats.rx_ring_drops   = 0u;
    s_stats.tx_frames       = 0u;
    s_stats.hal_errors      = 0u;
    s_stats.bus_off_seen    = 0u;
    s_stats.error_warn_seen = 0u;
    s_stats.reinit_calls    = 0u;

    /* ---- 2) 配置 HAL 层位时序 ----
       注意：MSP（GPIO/时钟）由 CubeMX 生成的 HAL_CAN_MspInit 负责，
       本模块不重复配置，避免"两处都配、改了一处忘另一处"。 */
    hcan.Instance                  = CAN1;
    hcan.Init.Prescaler            = CAN_PRESCALER;
    hcan.Init.Mode                 = CAN_MODE_NORMAL;
    /* ⚠️ 这三个用**编码宏**（CAN_*_ENC），不是裸 TQ 数 —— 理由见文件上方
       那段"交给 HAL 的必须是编码宏"的说明（BTR 曾因此被写成 0x0F）。 */
    hcan.Init.SyncJumpWidth        = CAN_SJW_ENC;
    hcan.Init.TimeSeg1             = CAN_BS1_ENC;
    hcan.Init.TimeSeg2             = CAN_BS2_ENC;
    /* 自动离线管理：**必须关闭**。
       开启后硬件会在 Bus-Off 后自动恢复，但那样软件就看不到
       "发生过 Bus-Off"这件事 —— 而运维恰恰需要这个信息去查物理层。
       我们自己做恢复（有退避、有统计、有上报），比硬件悄悄恢复更有价值。 */
    hcan.Init.AutoBusOff           = DISABLE;
    /* 自动唤醒：睡眠模式自动退出，开着无害 */
    hcan.Init.AutoWakeUp           = DISABLE;
    hcan.Init.AutoRetransmission   = ENABLE;    /* 发送失败自动重发（虽然本设计不发） */
    hcan.Init.ReceiveFifoLocked    = DISABLE;   /* 溢出时覆盖旧报文而不是锁死 */
    hcan.Init.TransmitFifoPriority = DISABLE;

    if (HAL_CAN_Init(&hcan) != HAL_OK) {
        s_stats.hal_errors++;
        /* ⚠️⚠️ 硬件没起来必须让核心知道（2026-10-14 上板实测）。
         *
         * `gw_can_core_init()` 是纯逻辑层，它只能乐观地把状态置成 NORMAL。
         * 而这里提前 return 之后**没有任何人**去改那个 NORMAL ——
         * 于是 `gw_pipeline_status_word()` 置起 IREG_STATUS_BIT_CAN_OK、
         * 界面显示「CAN 总线正常」，而同一时刻 MSR.INAK 还是 1
         * （控制器还在初始化模式，一帧都收不了），自检也报 [FAIL]。
         * 三处矛盾，其中"正常"那个是假的 —— 这类**假报正常**最危险。
         *
         * 置成 FAILED 后：状态字如实报不可用，且状态机会按既有退避节奏
         * 周期性调 core_reinit_cb 重试 ⇒ **后插上总线能自愈**。 */
        gw_can_core_mark_failed(&s_core);
        return CAN_PORT_ERR_HAL;
    }

    /* ---- 3) 筛选器 ---- */
    can_config_filter(filt_id, filt_mask);

    /* ---- 4) 启动 + 打开接收中断 ----
     *
     * ⚠️ HAL_CAN_Start() 会超时失败，条件很特别、一定要知道：
     *    bxCAN 退出初始化模式需要**总线上出现 11 个连续隐性位**（完成同步）。
     *    若 CAN_RX 被外部拉成显性（收发器没供电 / 收发器未工作或未供电 / CAN_H-L 被拉低 /
     *    RXD 没接），控制器就永远出不来 —— 现象是这里返回 CAN_PORT_ERR_HAL，
     *    而板子上的"CAN 坏了"其实是**收发器/总线**的问题，不是 MCU。
     *    2026-10-10 实测：CAN_RX(PB8) = 0（显性），HAL_CAN_Start 超时，
     *    MSR.INAK 恒为 1；连内部回环模式也救不了（同步源仍是 CAN_RX）。
     *
     *    自检里用 can_port_bus_ready() / can_port_config_ok() 把
     *    "控制器配置"与"有没有上总线"分开报，见 gw_can_port.h。 */
    if (HAL_CAN_Start(&hcan) != HAL_OK) {
        s_stats.hal_errors++;
        /* ⚠️⚠️ 硬件没起来必须让核心知道（2026-10-14 上板实测）。
         *
         * `gw_can_core_init()` 是纯逻辑层，它只能乐观地把状态置成 NORMAL。
         * 而这里提前 return 之后**没有任何人**去改那个 NORMAL ——
         * 于是 `gw_pipeline_status_word()` 置起 IREG_STATUS_BIT_CAN_OK、
         * 界面显示「CAN 总线正常」，而同一时刻 MSR.INAK 还是 1
         * （控制器还在初始化模式，一帧都收不了），自检也报 [FAIL]。
         * 三处矛盾，其中"正常"那个是假的 —— 这类**假报正常**最危险。
         *
         * 置成 FAILED 后：状态字如实报不可用，且状态机会按既有退避节奏
         * 周期性调 core_reinit_cb 重试 ⇒ **后插上总线能自愈**。 */
        gw_can_core_mark_failed(&s_core);
        return CAN_PORT_ERR_HAL;
    }
    if (HAL_CAN_ActivateNotification(&hcan,
                                     CAN_IT_RX_FIFO0_MSG_PENDING
                                     | CAN_IT_ERROR_WARNING
                                     | CAN_IT_ERROR_PASSIVE
                                     | CAN_IT_BUSOFF
                                     | CAN_IT_LAST_ERROR_CODE) != HAL_OK) {
        s_stats.hal_errors++;
        /* ⚠️⚠️ 硬件没起来必须让核心知道（2026-10-14 上板实测）。
         *
         * `gw_can_core_init()` 是纯逻辑层，它只能乐观地把状态置成 NORMAL。
         * 而这里提前 return 之后**没有任何人**去改那个 NORMAL ——
         * 于是 `gw_pipeline_status_word()` 置起 IREG_STATUS_BIT_CAN_OK、
         * 界面显示「CAN 总线正常」，而同一时刻 MSR.INAK 还是 1
         * （控制器还在初始化模式，一帧都收不了），自检也报 [FAIL]。
         * 三处矛盾，其中"正常"那个是假的 —— 这类**假报正常**最危险。
         *
         * 置成 FAILED 后：状态字如实报不可用，且状态机会按既有退避节奏
         * 周期性调 core_reinit_cb 重试 ⇒ **后插上总线能自愈**。 */
        gw_can_core_mark_failed(&s_core);
        return CAN_PORT_ERR_HAL;
    }

    s_inited = 1u;
    return CAN_PORT_OK;
}

int can_port_reinit(void)
{
    s_stats.reinit_calls++;

    /* 完整重来一遍：HAL_CAN_Init 会复位控制器并退出 Bus-Off。
       先 Stop 再 Init 是 HAL 的推荐顺序 ——
       直接 Init 一个还在运行的外设行为未定义。 */
    (void)HAL_CAN_Stop(&hcan);

    if (HAL_CAN_Init(&hcan) != HAL_OK) {
        s_stats.hal_errors++;
        return -1;
    }
    can_config_filter(s_filt_id, s_filt_mask);

    if (HAL_CAN_Start(&hcan) != HAL_OK) {
        s_stats.hal_errors++;
        return -1;
    }
    if (HAL_CAN_ActivateNotification(&hcan,
                                     CAN_IT_RX_FIFO0_MSG_PENDING
                                     | CAN_IT_ERROR_WARNING
                                     | CAN_IT_ERROR_PASSIVE
                                     | CAN_IT_BUSOFF
                                     | CAN_IT_LAST_ERROR_CODE) != HAL_OK) {
        s_stats.hal_errors++;
        return -1;
    }

    /* 清空残留报文：Bus-Off 期间缓冲里的东西已经不可信 */
    s_head = 0u;
    s_tail = 0u;

    return 0;
}

/* ==================================================================
 *  自检专用：控制器配置回读 + 是否上了总线
 *
 *  这两问的动机（2026-10-10 实测）：
 *    原来的 CAN 自检只读核心状态（软件记录），而那块板子上
 *    CAN_RX(PB8) 被外部拉成显性 -> HAL_CAN_Start() 超时 ->
 *    控制器一直停在初始化模式；可核心状态照样是 NORMAL，
 *    报告给的是"CAN 通过"。等于什么都没验。
 *
 *    ⚠️ 顺便记一条实测结论：这种情况**内部回环也救不了** ——
 *       回环模式下 INAK 同样清不掉（同步源仍然是 CAN_RX）。
 *       所以"用回环绕过总线验控制器"这条路在 F1 上走不通，
 *       要总线无关地验控制器只能靠**寄存器回读**（本函数第一问）。
 * ================================================================== */

int can_port_config_ok(void)
{
    uint32_t expect_btr;
    uint32_t btr;

    if (hcan.Instance == 0) {
        return 0;                       /* 句柄都没挂实例 */
    }

    /* ---- 1) 位时序回读比对 ----
       ⚠️ 期望值必须按**硬件位布局**算，不能照抄我们自己填进 Init 的那些宏 ——
          否则"填错了"和"查错了"会一起错，检查变成空判据
          （2026-10-10 就是这么漏掉 BTR=0x0F 的）。

       bxCAN 的 BTR 布局（见参考手册 CAN_BTR）：
          bit 0-9   BRP  预分频-1
          bit 16-19 TS1  BS1-1
          bit 20-22 TS2  BS2-1
          bit 24-25 SJW  SJW-1
          bit 30-31 模式位（LBKM/SILM），正常模式应为 0 */
    expect_btr = (((uint32_t)CAN_SJW - 1u) << 24)
                 | (((uint32_t)CAN_BS1 - 1u) << 16)
                 | (((uint32_t)CAN_BS2 - 1u) << 20)
                 | ((uint32_t)CAN_PRESCALER - 1u);
    btr = hcan.Instance->BTR;

    if (btr != expect_btr) {
        return 0;
    }

    /* ---- 2) 滤波器是否真的激活 ----
       FMR.FINIT = 0（不在滤波器初始化模式）
       FA1R 的 bit0 = 1（0 号滤波器组已激活）
       ⚠️ 只回读"激活位"，不比对 ID/掩码 —— 那是用户可配的，
          不是"配置是否正确"的判据。 */
    if ((hcan.Instance->FMR & CAN_FMR_FINIT) != 0u) {
        return 0;
    }
    if ((hcan.Instance->FA1R & 0x1u) == 0u) {
        return 0;
    }

    /* ---- 3) 接收中断在 NVIC 里使能了吗 ----
       ⚠️ 2026-10-10 实测漏过这一项，后果很隐蔽（用回环自检才抓出来）：
          `HAL_CAN_ActivateNotification()` 只置**外设 IER**，
          **不会**使能 NVIC 通道。漏了它则：
              帧被控制器收下 -> 堆在硬件 FIFO（还会溢出 FOVR0）
              -> ISR 永不进入 -> 环形缓冲恒空 -> 收帧计数恒 0
          现象是"CAN 明明收到了却什么都看不到"，而控制器状态一切正常。
          把它纳入"配置回读"，自检就能自查这类漏配。 */
    {
        uint32_t word = (uint32_t)CAN1_RX0_IRQn / 32u;
        uint32_t bit  = 1u << ((uint32_t)CAN1_RX0_IRQn % 32u);

        if (word >= 8u || (NVIC->ISER[word] & bit) == 0u) {
            return 0;
        }
    }

    return 1;
}

int can_port_bus_ready(void)
{
    if (hcan.Instance == 0) {
        return 0;
    }
    /* INAK=1 表示控制器仍在初始化模式 —— 也就是"没上总线"。
       退出初始化模式需要总线上 11 个连续隐性位，所以 INAK=1 基本等价于
       "CAN_RX 被拉成显性 / 收发器没工作"（详见头文件说明）。 */
    return ((hcan.Instance->MSR & CAN_MSR_INAK) == 0u) ? 1 : 0;
}

/* ==================================================================
 *  内部回环自检（不需要第二个节点）
 *
 *  设计要点见头文件；这里只说两处实现细节：
 *
 *  ⚠️ 报文**不自己抢**：中断保持开启，让报文走正常接收路径
 *     （ISR -> 环形缓冲 -> can_port_poll -> 核心），
 *     所以本函数中间会自己调 can_port_poll() 把环形缓冲推进去 ——
 *     不这么做的话，帧会一直躺在环形缓冲里，计数永远不涨
 *     （应用层的 tick 也要过一会儿才来）。
 *
 *  ⚠️ 等待用**固定次数上限**而不是 HAL_GetTick / DWT：
 *     自检可能在时基或 DWT 初始化之前被调用，那两个时间源
 *     在那种情况下不前进，会把这里变成死循环。
 * ================================================================== */
can_port_status_t can_port_loopback_test(uint16_t id, uint8_t dlc)
{
    CAN_TxHeaderTypeDef tx;
    CAN_InitTypeDef     saved_init;
    uint8_t             data[8];
    uint32_t            mailbox = 0u;
    uint32_t            guard;
    uint32_t            rx_before;
    uint8_t             i;
    int                 ok = 0;

    if (hcan.Instance == 0) {
        return CAN_PORT_ERR_PARAM;      /* 从没 init 过，别碰硬件 */
    }
    if (dlc < 1u || dlc > 8u || id > GW_CAN_STD_ID_MAX) {
        return CAN_PORT_ERR_PARAM;
    }

    saved_init = hcan.Init;
    rx_before  = s_core.stat.rx_total;

    for (i = 0u; i < dlc; i++) {
        data[i] = (uint8_t)(0xA5u + i);     /* 有辨识度的固定图案 */
    }

    /* ---- 1) 切回环模式（保留中断，走正常接收路径）---- */
    (void)HAL_CAN_Stop(&hcan);
    hcan.Init.Mode = CAN_MODE_LOOPBACK;

    if (HAL_CAN_Init(&hcan) == HAL_OK) {
        can_config_filter(0u, 0u);      /* 临时全通：不受用户滤波器影响 */

        if (HAL_CAN_Start(&hcan) == HAL_OK
            && HAL_CAN_ActivateNotification(&hcan,
                   CAN_IT_RX_FIFO0_MSG_PENDING) == HAL_OK) {

            tx.StdId              = id;
            tx.ExtId              = 0u;
            tx.IDE                = CAN_ID_STD;
            tx.RTR                = CAN_RTR_DATA;
            tx.DLC                = dlc;
            tx.TransmitGlobalTime = DISABLE;

            if (HAL_CAN_AddTxMessage(&hcan, &tx, data, &mailbox) == HAL_OK) {
                /* ---- 2) 等它经"正常路径"回到核心 ----
                   回环不需要 ACK，帧几乎立刻就到 FIFO；
                   下面的 poll 负责把环形缓冲推进核心。 */
                for (guard = 0u; guard < 20000u; guard++) {
                    if (s_core.stat.rx_total > rx_before) {
                        break;
                    }
                    (void)can_port_poll(1u);    /* ISR 收 -> 缓冲 -> 核心 */
                    if (s_core.stat.rx_total > rx_before) {
                        break;
                    }
                }

                if (s_core.stat.rx_total > rx_before) {
                    /* 再核对一次"收到的确实是这一帧" —— 只涨计数不核对 ID 的话，
                       噪声帧也能让测试变绿。
                       ⚠️ 用**非破坏性**的读取扫描诊断环：
                          `gw_can_core_take_diag()` 会把记录弹走，
                          那是应用层要用的诊断数据，自检不该消费它。 */
                    uint8_t seen = 0u;

                    for (i = 0u; i < GW_CAN_DIAG_DEPTH; i++) {
                        if (s_core.diag[i].id == id
                            && s_core.diag[i].dlc == dlc) {
                            seen = 1u;
                            break;
                        }
                    }
                    ok = (seen != 0u) ? 1 : 0;
                }
            }
        }
    }

    /* ---- 3) 恢复现场（任何失败路径都要走到这里）---- */
    (void)HAL_CAN_Stop(&hcan);
    hcan.Init = saved_init;

    if (can_port_reinit() != 0) {
        return CAN_PORT_ERR_HAL;
    }

    s_stats.loopback_runs++;
    if (ok == 0) {
        s_stats.loopback_fails++;
        return CAN_PORT_ERR_HAL;
    }
    return CAN_PORT_OK;
}

uint16_t can_port_poll(uint32_t elapsed_ms)
{
    gw_can_frame_t f;
    uint16_t       n = 0u;
    uint32_t       err;

    if (s_inited == 0u) {
        return 0u;
    }

    /* ---- 1) 处理中断收下的报文 ----
       限次循环：万一中断里疯狂灌数据，这里不能无限占用主循环。
       剩余的下轮再处理（环形缓冲会兜住）。 */
    while (n < CAN_RX_RING_SIZE && ring_get(&f) != 0) {
        (void)gw_can_core_on_frame(&s_core, &f);
        n++;
    }

    /* ---- 2) 查询错误状态 ----
       为什么用轮询而不是只靠中断回调：
         中断回调在"中断被抢占/丢失"时可能不触发，
         而错误状态是**持续存在**的硬件事实。
         轮询一次 HAL_CAN_GetError 只要几百纳秒，
         却能让"Bus-Off 之后软件不知道"这种最坏情况不会发生。
       这也是与 RS485 的 DE 方向控制不同的思路：
         那里的状态是瞬时的（必须靠中断），
         这里的错误计数是持续的（轮询更可靠）。 */
    err = HAL_CAN_GetError(&hcan);

    if ((err & HAL_CAN_ERROR_BOF) != 0u) {
        /* Bus-Off 已由中断处理过的话，核心那边会忽略重复通知 */
        if (gw_can_core_state(&s_core) != GW_CAN_ST_BUS_OFF
            && gw_can_core_state(&s_core) != GW_CAN_ST_RECOVERING
            && gw_can_core_state(&s_core) != GW_CAN_ST_FAILED) {
            s_stats.bus_off_seen++;
        }
        gw_can_core_notify_bus_off(&s_core);
    } else if ((err & HAL_CAN_ERROR_EWG) != 0u) {
        if (gw_can_core_state(&s_core) == GW_CAN_ST_NORMAL) {
            s_stats.error_warn_seen++;
        }
        gw_can_core_notify_error_warn(&s_core);
    } else if (err == HAL_CAN_ERROR_NONE) {
        /* 错误清零：如果之前在警告态，说明已经恢复。
           注意不把 Bus-Off 态的"错误清零"当成恢复 ——
           Bus-Off 后错误寄存器的表现不可靠，必须走完整重初始化。 */
        if (gw_can_core_state(&s_core) == GW_CAN_ST_ERROR_WARN) {
            gw_can_core_notify_normal(&s_core);
        }
    } else {
        /* 其它协议错误（位填充、格式、ACK 等）：计数但不改状态。
           这些是**瞬态**错误，CAN 控制器自己会重发，
           软件介入反而会打乱硬件的错误管理。 */
        s_stats.hal_errors++;
    }

    /* ---- 3) 推进核心状态机（含 Bus-Off 恢复） ---- */
    gw_can_core_tick(&s_core, elapsed_ms);

    return n;
}

gw_can_core_t *can_port_core(void)
{
    return &s_core;
}

const can_port_stat_t *can_port_stats(void)
{
    return &s_stats;
}

void *can_port_handle(void)
{
    return (void *)&hcan;
}

/* ==================================================================
 *  中断侧
 *
 *  ⚠️ 与串口那边不同：CAN 的中断回调**不需要**全工程唯一分派 ——
 *     `HAL_CAN_RxFifo0MsgPendingCallback` 是 CAN 外设专属的，
 *     本工程只有一个 CAN 实例，所以直接在这里定义即可。
 *
 *     （串口的三个回调是"外设无关"的通用名字，所以才要在
 *       rs485_port.c 里统一分派。两者的区别值得记住。）
 * ================================================================== */

void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan_ptr)
{
    CAN_RxHeaderTypeDef hdr;
    gw_can_frame_t      f;
    uint32_t            guard;

    if (hcan_ptr->Instance != CAN1) {
        return;
    }

    /* 一次中断可能有多条报文待取（FIFO 有 3 级）。
       全部取走，避免留下积压导致 FIFO 满。
       限次防御：万一 FIFO 状态异常，不能死循环在中断里。 */
    for (guard = 0u; guard < 4u; guard++) {
        if (HAL_CAN_GetRxFifoFillLevel(hcan_ptr, CAN_RX_FIFO0) == 0u) {
            break;
        }
        /* 注意第 4 个参数是数据缓冲：数据段不在 hdr 里 */
        if (HAL_CAN_GetRxMessage(hcan_ptr, CAN_RX_FIFO0, &hdr, f.data) != HAL_OK) {
            break;
        }
        /* 这里先把数据段暂存再转换：convert_frame 会先清零 f.data，
           所以必须传入一个独立的源缓冲，不能直接传 f.data（会被自己清掉）。
           用局部数组而不是全局，避免中断重入时的竞争。 */
        {
            uint8_t raw[GW_CAN_MAX_DLC];
            uint8_t i;

            for (i = 0u; i < GW_CAN_MAX_DLC; i++) {
                raw[i] = f.data[i];
            }
            convert_frame(&hdr, raw, &f);
        }
        s_stats.rx_isr_frames++;
        ring_put(&f);
    }
}

/** FIFO 满：说明主循环没跟上，计一次严重过载 */
void HAL_CAN_RxFifo0FullCallback(CAN_HandleTypeDef *hcan_ptr)
{
    if (hcan_ptr->Instance != CAN1) {
        return;
    }
    s_stats.rx_ring_drops++;
}

/** 错误中断：把状态变化尽快通知核心，不等下一次 poll */
void HAL_CAN_ErrorCallback(CAN_HandleTypeDef *hcan_ptr)
{
    uint32_t err;

    if (hcan_ptr->Instance != CAN1) {
        return;
    }
    err = HAL_CAN_GetError(hcan_ptr);

    if ((err & HAL_CAN_ERROR_BOF) != 0u) {
        gw_can_core_notify_bus_off(&s_core);
    } else if ((err & HAL_CAN_ERROR_EWG) != 0u) {
        gw_can_core_notify_error_warn(&s_core);
    } else {
        s_stats.hal_errors++;
    }
}
