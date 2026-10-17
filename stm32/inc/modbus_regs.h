/**
 * @file    modbus_regs.h
 * @brief   寄存器映射表定义 —— 本项目的对外数据接口契约（对应设计文档表 4-2）
 *
 * ============================================================================
 *  为什么寄存器表是"接口契约"而不是随便几个变量？
 *
 *  1) 它是本设备与外部世界（上位机 / 组态软件 / 云端）之间唯一被文档化的约定。
 *     改一个地址，所有下游都要改，所以这张表必须集中定义、单点维护。
 *
 *  2) 输入寄存器（04，只读）与保持寄存器（03/06/16，读写）的划分不是随意的：
 *     实时采集量放只读区，防止上位机误写覆盖真实数据；
 *     配置参数放读写区，且必须带范围校验（非法值返回异常码 03）。
 *
 *  3) 32 位量（uptime、device_id）用两个 16 位寄存器表示，**统一高字在前**。
 *     Modbus 规范没有规定 32 位量的字节序，各厂家实现不一，
 *     所以必须由本文档明确约定，否则对接时必然出问题。
 *
 *  4) 命令类寄存器（param_save / param_default / reboot）使用**魔数**而非 0/1，
 *     目的是防止上位机误写一个 0 或 1 就触发落盘或复位。
 * ============================================================================
 */
#ifndef MODBUS_REGS_H
#define MODBUS_REGS_H

#include <stdint.h>

/* ==========================================================================
 * 输入寄存器区（功能码 04，只读；标准地址 30001 起）
 * 偏移 0x0000 ~ 0x001F，共 32 个
 * ========================================================================== */
enum {
    /* ---- 环境采集 ---- */
    IREG_TEMP_X10        = 0,   /* int16  温度 x10，例 235 = 23.5℃；负值用补码 */
    IREG_HUMI_X10        = 1,   /* uint16 湿度 x10，例 612 = 61.2%RH */

    /* ---- CAN 通道数据（CAN 报文经网关映射而来） ---- */
    IREG_CAN_CH1         = 2,
    IREG_CAN_CH2         = 3,
    IREG_CAN_CH3         = 4,
    IREG_CAN_CH4         = 5,
    IREG_CAN_CH5         = 6,
    IREG_CAN_CH6         = 7,
    IREG_CAN_CH7         = 8,
    IREG_CAN_CH8         = 9,

    /* ---- 设备状态与运行信息 ---- */
    IREG_DEV_STATUS      = 10,  /* uint16 位域，见下方 IREG_STATUS_BIT_* 定义 */
    IREG_UPTIME_HI       = 11,  /* uint16 运行时间（秒）高 16 位 */
    IREG_UPTIME_LO       = 12,  /* uint16 运行时间（秒）低 16 位 */

    /* ---- 通信统计（既是自检手段，也是简历上实测数据的来源） ---- */
    IREG_MB_RX_LO        = 13,  /* uint16 Modbus 从站收帧计数 低 16 位 */
    IREG_MB_RX_HI        = 14,  /* uint16 Modbus 从站收帧计数 高 16 位 */
    IREG_MB_ERR_LO       = 15,  /* uint16 Modbus 错误帧计数 低 16 位 */
    IREG_MB_ERR_HI       = 16,  /* uint16 Modbus 错误帧计数 高 16 位 */
    IREG_CAN_RX_LO       = 17,  /* uint16 CAN 收帧计数 低 16 位 */
    IREG_CAN_RX_HI       = 18,  /* uint16 CAN 收帧计数 高 16 位 */
    IREG_MB_EXC_LO       = 19,  /* uint16 Modbus 异常响应计数 低 16 位 */
    IREG_MB_EXC_HI       = 20,  /* uint16 Modbus 异常响应计数 高 16 位 */

    /* ---- 上云链路状态 ---- */
    IREG_MQTT_STATUS     = 21,  /* uint16 0=未连接 1=连接中 2=已连接 3=鉴权失败 */
    IREG_OFFLINE_DEPTH   = 22,  /* uint16 断网缓存队列深度（条） */
    IREG_OFFLINE_DROP    = 23,  /* uint16 缓存溢出丢包累计数（必须可见，否则"不丢数据"是假话） */

    /* ---- 版本与主站状态 ---- */
    IREG_FW_VER_MAJOR    = 24,  /* uint16 固件主版本 */
    IREG_FW_VER_MINOR    = 25,  /* uint16 固件次版本 */
    IREG_MASTER_ONLINE   = 26,  /* uint16 主站轮询在线从机位图（bit0=从机1 … bit15=从机16） */
    IREG_MASTER_ERR      = 27,  /* uint16 主站最近一次错误：0=无 1=超时 2=CRC错 3=异常响应 */

    /* ---- RS485 接收路径诊断区（28~31） ----
     *
     * ⚠️ 2026-10-11 状态变更：这一块**从"临时借用"转为"有据可依的诊断区"**，
     *    并且已写进 README 的寄存器映射表（原来文档里写的是"保留"）。
     *
     * 当初为什么借（真实故障）：上板压测几百次高速请求后，从站会**永久停止响应**，
     * 而且**按复位键无效、必须断电**（说明外设被卡在了软复位清不掉的硬件状态）。
     * 那个"卡死"的根因已经找到并修掉（交接文档缺陷 C：主站申请总线用错角色 +
     * 双重申请 -> 总线 owner 永久停在 SLAVE）。仅靠 Modbus 看不到内部状态，
     * 当时把关键变量映射到了这里 —— 这套寄存器就是**抓住那个缺陷的工具**。
     *
     * ⚠️ 那为什么**不**跟着一起清掉？两个理由：
     *   1) 它们**落在 0~31 这段对外契约之内**（28 < 32），占用不改变任何契约 ——
     *      清掉它们对"收回契约"没有任何贡献（真正让数量变成 40 的是 36~39 的
     *      耗时测量区，那是正式功能）。
     *   2) RS485 接收路径**还有两件没闭环的事**：物理链路质量（目标"路 1 错误码 0"）
     *      与 IDLE 判帧尾的"死窗口"（IDLE 触发时机 ≈1T，而规范允许 1.5T）。
     *      这两件事恰好都要看这里的状态位，工具是现成的（tools/diag_monitor.py）。
     *
     * **归还条件（写清楚，免得又变成"无主的临时代码"）**：
     *   上面两件 RS485 的事都闭环之后，把 28~31 还原成真正的预留，
     *   同时把本段注释与 README 的对应行一起改掉。
     *
     * 规范上输入寄存器 04 读预留区是允许的，不违反 Modbus 契约；
     * 但**内容不保证跨版本稳定**（README 里已如实标注）。 */
    IREG_DIAG_STATE      = 28,  /* 诊断：位段打包，见下方 IREG_DIAG_ 位定义 */
    IREG_DIAG_SR         = 29,  /* 诊断：读寄存器时 USART2->SR 的原值 */
    IREG_DIAG_RX_LEN     = 30,  /* 诊断：s_rx_len（当前帧长度） */
    IREG_DIAG_CNDTR      = 31,  /* 诊断：DMA1_Channel6 的 CNDTR（剩余传输数） */

    /* ---- 片内耗时测量区（36~39，见 inc/gw_timing.h） ----
     *
     * ⚠️ 为什么要单独放一块：PC 端量到的"响应延迟"里，
     *    **绝大部分是 USB 串口驱动的开销**（实测中位 4.46ms，
     *    而 9 字节响应在 115200 下线上只需 0.78ms）。
     *    那个数字**不能**当作从站的响应延迟写进报告。
     *
     *    这里的数字是用片内 DWT 微秒计数器测的，
     *    **不含任何 PC/USB/驱动开销**，才是可以写进报告的真值。
     *
     * ⚠️ 原来只有 32 个输入寄存器；为了放这块测量区扩到 40 个。
     *    **32~35 是留空区**（对外契约的一部分，不许随手占用）。
     *    ⚠️ 2026-10-11：这里原来还借给"主站内部状态"做临时诊断（[32] 轮询项数
     *       /[33] awaiting/[34] elapsed_ms/[35] cnt_requests），主站链路验穿后
     *       **已清掉**。要再借用请写清范围 + 判读 + 归还条件
     *       （见 templates/main_L0_bringup.c 里 cb_refresh_regs() 的注释）。
     *
     * ----------------------------------------------------------------------------
     *  ⚠️ IREG_TM_SLAVE_* 的**确切含义**（实测踩过一次，必须写清楚）
     *
     *  它测的是"从取帧到组好响应"的耗时，**不包含响应帧的传输时间**。
     *
     *  为什么这点必须强调：`rs485_send()` 是**阻塞发送**，而它本来就在
     *  GW_PHASE_SLAVE 的测量区间内，于是"处理耗时"被传输时间主导：
     *
     *      13 字节响应: 理论传输 1129us，实测 Max 1260us
     *      69 字节响应: 理论传输 5990us，实测 Max 6340us
     *
     *  两个数字差 5 倍 —— 混在一个指标里根本没法用。
     *  所以代码里在发送前 `begin(MASTER)`、发送后 `begin(SLAVE)`，
     *  把传输切了出去（见 app/gw_app.c 的 slave 任务）。
     *
     *  这样 SLAVE 的耗时**与帧长无关**，才能反映实时性。
     *  ⚠️ 副作用：MASTER 段的统计会多出这段发送时间（L0 无主站，无影响）。 */
    IREG_TM_SLAVE_MAX    = 36,  /* 从站处理耗时 max（us，**不含响应传输**） */
    IREG_TM_SLAVE_AVG    = 37,  /* 从站处理耗时 avg（us，不含传输） */
    IREG_TM_LOOP_MAX     = 38,  /* 一轮主循环的最大耗时（us） */
    IREG_TM_LOAD_PERMIL  = 39,  /* CPU 负载（千分比；150 = 15.0%） */

    /* ---- 原来的预留名保留为**别名** ----
     *
     * ⚠️ 为什么要留别名，而不是直接把测试里的 IREG_RESERVED_28 改掉：
     *
     *   `test_pipeline.c` 与 `test_regmap.c` 用这两个名字断言
     *   **"gw_pipeline 不许写预留区"** —— 那是一条有价值的契约（防止
     *   数据管道越界覆盖别人的寄存器）。
     *
     *   而诊断代码是**刻意**写这几个寄存器的，两者并不矛盾：
     *     数据管道（gw_pipeline）      -> 不许碰
     *     RS485 诊断（rs485_port_fill_diag）-> 专门写
     *
     *   保留别名可以同时表达这两件事，测试不需要改，
     *   而且将来把诊断代码删掉、这里还原成真正的预留时，
     *   测试不用动一个字。 */
    IREG_RESERVED_28     = IREG_DIAG_STATE,
    IREG_RESERVED_29     = IREG_DIAG_SR,
    IREG_RESERVED_30     = IREG_DIAG_RX_LEN,
    IREG_RESERVED_31     = IREG_DIAG_CNDTR
};

/* ---- IREG_DIAG_STATE 位定义 ----
 *
 * 低 3 位是 rs485_state_t 枚举，其余是布尔标志。
 * ⚠️ 特意把状态枚举**原样**放进来（不是折算成"正常/异常"），
 *    因为定位问题时需要知道它**卡在哪个具体状态**。 */
#define IREG_DIAG_ST_SHIFT         0u
#define IREG_DIAG_ST_MASK          0x0007u  /* rs485_state_t: 0=UNINIT 1=IDLE 2=RX 3=RX_READY 4=TX 5=ERROR */
#define IREG_DIAG_BIT_RX_OVERFLOW  (1u << 3) /* s_rx_overflow 置位 */
#define IREG_DIAG_BIT_HAS_RX_ERR   (1u << 4) /* 曾发生 USART 接收错误（ORE/NE/FE/PE） */
#define IREG_DIAG_BIT_EVER_ERR     (1u << 5) /* HAL_UART_ErrorCallback 曾被调用过 */
#define IREG_DIAG_BIT_TX_OK        (1u << 6) /* 至少成功发送过一帧 */
#define IREG_DIAG_BIT_RX_ARMED     (1u << 7) /* DMA 通道处于使能态（CCR.EN） */
#define IREG_DIAG_BIT_IDLEIE       (1u << 8) /* USART2 CR1 的 IDLEIE 置位 */
#define IREG_DIAG_BIT_DMAR         (1u << 9) /* USART2 CR3 的 DMAR 置位 */
#define IREG_DIAG_BIT_RE           (1u << 10)/* USART2 CR1 的 RE（接收使能）置位 */
#define IREG_DIAG_BIT_HAL_RXREADY  (1u << 11)/* huart2.RxState == READY */

/* ---- IREG_DEV_STATUS 位定义 ---- */
#define IREG_STATUS_BIT_DHT_OK      (1u << 0)   /* DHT22 最近一次采集校验通过 */
#define IREG_STATUS_BIT_CAN_OK      (1u << 1)   /* CAN 控制器处于正常态（非 Bus-Off） */
#define IREG_STATUS_BIT_WIFI_OK     (1u << 2)   /* Wi-Fi 已连接 */
#define IREG_STATUS_BIT_MQTT_OK     (1u << 3)   /* MQTT 已连接 */
#define IREG_STATUS_BIT_PARAM_OK    (1u << 4)   /* Flash 参数校验通过（非默认值启动） */
#define IREG_STATUS_BIT_OTA_BUSY    (1u << 5)   /* OTA 升级进行中 */
#define IREG_STATUS_BIT_IWDG_RESET  (1u << 6)   /* 本次启动由看门狗触发（sticky，仅复位清除） */
#define IREG_STATUS_BIT_HARDFAULT   (1u << 7)   /* 曾经发生 HardFault（sticky） */
#define IREG_STATUS_BIT_MASTER_OK   (1u << 8)   /* 主站轮询任务正常运转 */

/* ---- IREG_MQTT_STATUS 取值 ---- */
#define IREG_MQTT_ST_DISCONNECTED   0u
#define IREG_MQTT_ST_CONNECTING     1u
#define IREG_MQTT_ST_CONNECTED      2u
#define IREG_MQTT_ST_AUTH_FAILED    3u

/* ==========================================================================
 * 保持寄存器区（功能码 03/06/16，读写；标准地址 40001 起）
 * 偏移 0x0000 ~ 0x000F，共 16 个
 *
 * 写入必须经范围校验，越界返回异常码 03（非法数据值）。
 * 校验规则由 mb_regmap_validate() 集中实现，不散落在各处。
 * ========================================================================== */
enum {
    /* ---- 设备标识（32 位，高字在前） ---- */
    HREG_DEVICE_ID_HI    = 0,   /* R/W 设备 ID 高 16 位 */
    HREG_DEVICE_ID_LO    = 1,   /* R/W 设备 ID 低 16 位 */

    /* ---- 通信参数（修改后需重启生效，避免把自己"说死"） ---- */
    HREG_BAUDRATE        = 2,   /* R/W 0=9600 1=19200 2=38400 3=115200 */
    HREG_REPORT_PERIOD   = 3,   /* R/W 上报周期，单位秒，范围 1~3600 */
    HREG_SLAVE_ADDR      = 4,   /* R/W 本机 Modbus 从站地址，范围 1~247 */

    /* ---- CAN 滤波配置 ---- */
    HREG_CAN_FILT_ID_HI  = 5,   /* R/W CAN 接收滤波 ID 高 16 位（标准帧有效位 0~10） */
    HREG_CAN_FILT_ID_LO  = 6,   /* R/W CAN 接收滤波 ID 低 16 位 */
    HREG_CAN_FILT_MSK_HI = 7,   /* R/W CAN 接收滤波掩码 高 16 位 */
    HREG_CAN_FILT_MSK_LO = 8,   /* R/W CAN 接收滤波掩码 低 16 位 */

    /* ---- 主站轮询配置 ---- */
    HREG_MASTER_ENABLE   = 9,   /* R/W 0=关闭主站轮询 1=开启 */
    HREG_MASTER_TIMEOUT  = 10,  /* R/W 主站单次请求超时 ms，范围 10~5000 */
    HREG_MASTER_RETRY    = 11,  /* R/W 主站单次请求重试次数，范围 0~5 */

    /* ---- 命令类寄存器（写魔数触发，读回无意义） ---- */
    HREG_PARAM_SAVE_CMD  = 12,  /* W   写 0x5A5A 触发参数落盘 */
    HREG_PARAM_DFLT_CMD  = 13,  /* W   写 0xA5A5 恢复出厂参数 */
    HREG_REBOOT_CMD      = 14,  /* W   写 0x1234 软复位 */

    /* ---- 只读状态 ---- */
    HREG_OTA_STATE       = 15   /* R   OTA 状态机状态，见 HREG_OTA_ST_* */
};

/* ---- HREG_BAUDRATE 取值（与 CubeMX 中 UART 初始化保持一致） ---- */
#define HREG_BAUD_9600       0u
#define HREG_BAUD_19200      1u
#define HREG_BAUD_38400      2u
#define HREG_BAUD_115200     3u

/* ---- 命令魔数：用魔数而非 0/1，防止上位机误触发 ---- */
#define HREG_CMD_PARAM_SAVE  0x5A5Au
#define HREG_CMD_PARAM_DFLT  0xA5A5u
#define HREG_CMD_REBOOT      0x1234u

/* ---- HREG_OTA_STATE 取值 ---- */
#define HREG_OTA_ST_IDLE     0u   /* 空闲 */
#define HREG_OTA_ST_DOWNLOAD 1u   /* 下载中 */
#define HREG_OTA_ST_VERIFY   2u   /* 校验中 */
#define HREG_OTA_ST_READY    3u   /* 校验通过，待重启切换 */
#define HREG_OTA_ST_FAILED   4u   /* 失败（可读此值定位原因） */

#endif /* MODBUS_REGS_H */
