/**
 * @file    rs485_port.c
 * @brief   RS485 物理层实现（UART 收发 + DE 方向切换 + IDLE 帧边界判定）
 *
 * 依赖 CubeMX 生成的外设初始化（本文件不做外设初始化，只使用它们）：
 *   huart2                 —— USART2 句柄
 *   hdma_usart2_rx         —— USART2_RX 的 DMA 句柄
 *
 * 引脚由 port/gw_config.h 集中定义，不依赖 main.h 的 User Label。
 *
 * ----------------------------------------------------------------------------
 *  HAL 版本差异的处理（必读）
 *
 *  F1 固件包有两个代际，API 不同：
 *    V1.8.0：无 HAL_UARTEx_ReceiveToIdle_DMA
 *    V1.8.7：有
 *  本文件用 GW_HAL_HAS_UARTEX_IDLE 宏隔离两者，两个版本都能编译：
 *
 *    宏 = 1（V1.8.7，推荐）：用 HAL_UARTEx_ReceiveToIdle_DMA，
 *                           IDLE 与 DMA 传输完成都会回调
 *                           HAL_UARTEx_RxEventCallback(huart, Size)
 *    宏 = 0（V1.8.0）：用 HAL_UART_Receive_IT 接收首字节，
 *                       之后纯靠 UART IDLE 中断判定帧结束，
 *                       在 USART2_IRQHandler 里调用 rs485_port_irq_handler()
 *
 *  切换方法见 doc/HAL版本与IDLE接收方案.md。
 * ----------------------------------------------------------------------------
 */
#include "rs485_port.h"

/* 注意包含顺序：gw_config.h 使用 HAL 的 GPIO_PIN_n 宏与 GPIOA/GPIOB 端口宏
   （用于引脚分配和编译期断言），因此必须先包含 HAL 头、再包含它。
   gw_config.h 里有守卫会检查这一点并给出明确提示。 */
#include "stm32f1xx_hal.h"
#include "gw_config.h"

/* 诊断用的 IREG_DIAG_* 位定义在这里（见 rs485_port_fill_diag）。
   ⚠️ modbus_regs.h 是纯定义头，不依赖 HAL，放在 gw_config.h 之后安全。 */
#include "modbus_regs.h"

/* ESP32 端口的三个中断处理入口。
 *
 * 本文件持有**全工程唯一**的 HAL_UARTEx_RxEventCallback、
 * HAL_UART_ErrorCallback、HAL_UART_RxCpltCallback
 * （HAL 的 weak 函数一个工程只能覆盖一次），
 * 收到 USART3 的事件时转发给 esp32_port.c 实现。
 *
 * ⚠️ 这三个函数在 esp32_port.c 里**两版 HAL 路径下都有定义**
 *    （其中一版是空实现）。这样本文件的分派代码不需要条件编译，
 *    减少"条件组合漏改导致某个 HAL 版本编不过"的风险。 */
void esp32_port_on_rx_event(uint16_t size);
void esp32_port_on_rx_byte(uint8_t byte);
void esp32_port_on_error(void);

/* ------------------------------------------------------------------
 * 外部依赖：CubeMX 生成的对象
 *
 * 如果链接时提示 huart2 / hdma_usart2_rx 未定义，说明 CubeMX 里
 * USART2（或它的 RX DMA 通道）没有使能，回去检查 .ioc 配置。
 * ------------------------------------------------------------------ */
extern UART_HandleTypeDef huart2;
#if (GW_HAL_HAS_UARTEX_IDLE == 1)
extern DMA_HandleTypeDef  hdma_usart2_rx;
#endif

/* ------------------------------------------------------------------
 * 内部状态
 * ------------------------------------------------------------------ */
static mb_slave_t     g_slave;
static mb_regs_t      g_regs;
static rs485_stats_t  g_stats;

static uint8_t              s_rx_dma_buf[MB_MAX_ADU];

static volatile rs485_state_t s_state      = RS485_ST_UNINIT;
static volatile uint16_t      s_rx_len     = 0u;
static volatile uint8_t       s_rx_overflow = 0u;

/* ---------------- 总线仲裁状态 ----------------
 *
 * 决策逻辑在独立的纯逻辑模块里（inc/rs485_arbiter.h + src/rs485_arbiter.c），
 * 已被 36 项单元测试穷举过所有状态组合。
 * 本文件只保留"按决策结果操作硬件"这一层 ——
 * 这样"决策错了"和"硬件操作错了"可以分开排查。 */
static rs485_arb_t s_arb;

/* 发送计数由移植层维护：仲裁器只关心"谁能发"，不关心"发了多少"。
   对外查询时把两者合并成一个视图（见 rs485_bus_stats）。 */
static uint32_t s_tx_slave  = 0u;
static uint32_t s_tx_master = 0u;
static gw_bus_stat_t s_bus_view;

/* ==================================================================
 * 内部：GPIO 方向控制
 * ================================================================== */
static inline void de_set_tx(void)
{
    HAL_GPIO_WritePin(GW_RS485_DE_PORT, GW_RS485_DE_PIN, GPIO_PIN_SET);
}

static inline void de_set_rx(void)
{
    HAL_GPIO_WritePin(GW_RS485_DE_PORT, GW_RS485_DE_PIN, GPIO_PIN_RESET);
}

/* ==================================================================
 * 内部：武装接收
 *
 * ⚠️ 这里的守卫**必须**配合 RxEventCallback 里的 HAL_UART_AbortReceive()
 *    才真正有效 —— 两者是一对，缺一不可：
 *
 *      回调：拿到一帧 -> AbortReceive()（把接收停掉）
 *      这里：有帧待取就不武装 -> 所以不会重新启动 DMA
 *
 *    ⚠️ 如果只在回调里 AbortReceive 而这里不判，take_frame 还没被调用时
 *       若有别的路径调本函数，就会重新武装并覆盖待取的那一帧。
 *    ⚠️ 如果只在这里判而回调里不 AbortReceive，则**完全无效** ——
 *       因为"不重新武装"阻止不了已经启动的 DMA 继续写缓冲。
 *       这正是本文件曾经的真实缺陷，详见 RxEventCallback 里的完整说明。
 * ================================================================== */
static void rs485_arm_receive(void)
{
    if (s_state == RS485_ST_RX_READY) {
        return;                     /* 上一帧还没被取走，等应用层处理完再武装 */
    }

    s_rx_len      = 0u;
    s_rx_overflow = 0u;
    s_state       = RS485_ST_IDLE;

#if (GW_HAL_HAS_UARTEX_IDLE == 1)
    /* V1.8.7 路径：IDLE 或 DMA 收满都会触发 RxEventCallback */
    {
        HAL_StatusTypeDef hs = HAL_UARTEx_ReceiveToIdle_DMA(
                                   &huart2, s_rx_dma_buf,
                                   (uint16_t)sizeof(s_rx_dma_buf));

        /* ⚠️ HAL_BUSY 是**正常状态**，不是错误 —— 别把它记成 hal_errors。
         *
         * HAL 在"已经有一次接收在进行中"时返回 HAL_BUSY（`RxState != READY`）。
         * 而"接收一直在进行中"恰恰是本模块的常态：武装一次之后就等帧，
         * 直到 IDLE 事件把 RxState 复位为 READY。
         *
         * 什么时候会撞上它：**发送发生在收到任何帧之前**。
         * 主站路径就是这种情形（下游设备还没应答，甚至根本没接），
         * 于是每次发送后的"重新武装"都返回 HAL_BUSY。
         *
         * 2026-10-10 实测：主站发 54 帧 -> hal_errors 也变成 54，
         * 而 s_state 被置成 ERROR。两个数字都像"接收坏了"，
         * 实际接收一直是好的 —— 纯粹是误报，而且在排查现场那条链路上
         * 正好会把人带偏（我当时就在读这两个值）。
         *
         * 处理：HAL_BUSY 直接返回，保持 IDLE；只有真的 HAL_ERROR/TIMEOUT
         * 才算错误。 */
        if (hs == HAL_BUSY) {
            return;                 /* 已经在接收中，无需重新武装 */
        }
        if (hs != HAL_OK) {
            g_stats.hal_errors++;
            s_state = RS485_ST_ERROR;
            return;
        }
    }
    /* 关掉 DMA 半传输中断。否则一帧超过半个缓冲区时会多触发一次回调，
       把一帧切成两帧 —— 在 Modbus Poll 高频率轮询时表现为随机 CRC 错误。 */
    __HAL_DMA_DISABLE_IT(&hdma_usart2_rx, DMA_IT_HT);
#else
    /* V1.8.0 路径：先用 Receive_IT 收第一个字节（它会顺带清 IDLE 标志），
       之后靠 USART2_IRQHandler 里的 IDLE 判定帧结束。 */
    if (HAL_UART_Receive_IT(&huart2, s_rx_dma_buf, 1u) != HAL_OK) {
        g_stats.hal_errors++;
        s_state = RS485_ST_ERROR;
        return;
    }
    __HAL_UART_ENABLE_IT(&huart2, UART_IT_IDLE);
#endif
}

/* ==================================================================
 * 内部：总线仲裁
 *
 * 决策本身已被抽到纯逻辑模块（36 项测试穷举）。
 * 这里只做一层薄封装，把 rs485_arb_* 的结果翻译成本模块的返回码。
 * ================================================================== */
static rs485_acq_result_t bus_try_acquire(rs485_role_t role)
{
    return rs485_arb_acquire(&s_arb, role);
}

static void bus_release(void)
{
    rs485_arb_release(&s_arb);
}

/* ==================================================================
 * 对外接口
 * ================================================================== */

rs485_status_t rs485_port_init(uint8_t slave_addr)
{
    if (slave_addr < MB_ADDR_MIN || slave_addr > MB_ADDR_MAX) {
        return RS485_ERR_PARAM;
    }

    mb_slave_init(&g_slave, slave_addr, &g_regs);

    g_stats.rx_frames   = 0u;
    g_stats.rx_bytes    = 0u;
    g_stats.rx_overflow = 0u;
    g_stats.tx_frames   = 0u;
    g_stats.tx_timeout  = 0u;
    g_stats.hal_errors  = 0u;

    /* 总线仲裁状态复位 */
    rs485_arb_init(&s_arb);
    s_tx_slave  = 0u;
    s_tx_master = 0u;

    /* 上电默认接收态。否则 DE 一直为高会占着总线，整个网络都通信不了。 */
    de_set_rx();

    rs485_arm_receive();
    return RS485_OK;
}

rs485_status_t rs485_port_take_frame(uint8_t *buf, uint16_t buf_size, uint16_t *out_len)
{
    uint16_t n;

    if (buf == 0 || out_len == 0) {
        return RS485_ERR_PARAM;
    }

    if (s_state != RS485_ST_RX_READY || s_rx_len == 0u) {
        return RS485_ERR_BUSY;
    }

    n = s_rx_len;
    if (n > buf_size) {
        n = buf_size;               /* 防御：理论上不会发生，buf_size 应 >= MB_MAX_ADU */
    }

    /* 先搬数据，再清状态并重新武装 —— 顺序反了会丢帧 */
    {
        uint16_t i;
        for (i = 0u; i < n; i++) {
            buf[i] = s_rx_dma_buf[i];
        }
    }
    *out_len = n;

    if (s_rx_overflow != 0u) {
        g_stats.rx_overflow++;
    }
    s_rx_len = 0u;
    s_state  = RS485_ST_IDLE;

    rs485_arm_receive();            /* 处理完立即为下一帧做准备 */
    return RS485_OK;
}

/* ==================================================================
 *  内部：只把一帧发出去 —— **不做任何仲裁/申请**
 *
 *  ⚠️ 前提（调用者必须保证）：总线已经归自己所有。
 *     从站：刚在 rs485_port_send_as() 里申请到
 *     主站：应用层已经用 rs485_bus_acquire_master() 申请到
 *
 *  抽出来的原因见头文件那段长注释：原来"申请+发送"揉在一个函数里，
 *  导致主站路径申请两次（第二次必然 BUSY），一帧都发不出去。
 * ================================================================== */
static rs485_status_t tx_frame_now(const uint8_t *buf, uint16_t len)
{
    uint32_t guard;

    s_state = RS485_ST_TX;

    /* ---- 切到发送态 ---- */
    de_set_tx();

    /* ---- 阻塞发送。
       响应帧通常 8~20 字节，9600bps 下约 8~21ms；
       最长 256 字节约 266ms。这是在任务上下文调用，不是在中断里。 ---- */
    if (HAL_UART_Transmit(&huart2, (uint8_t *)buf, len, 1000u) != HAL_OK) {
        g_stats.hal_errors++;
        de_set_rx();
        s_state = RS485_ST_IDLE;
        return RS485_ERR_TIMEOUT;
    }

    /* ---- 等 TC（发送完成），不是 TXE。
       带超时保护：万一 TC 一直不置位（时钟异常等），不能让任务永远卡死。 ---- */
    guard = 0u;
    while (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_TC) == RESET) {
        if (++guard > 2000000u) {
            g_stats.tx_timeout++;
            break;
        }
    }
    __HAL_UART_CLEAR_FLAG(&huart2, UART_FLAG_TC);

    /* ---- 切回接收态 ---- */
    de_set_rx();

    g_stats.tx_frames++;
    s_state = RS485_ST_IDLE;

    /* 发送完成后必须重新武装接收，否则"只能通一次" */
    rs485_arm_receive();
    return RS485_OK;
}

rs485_status_t rs485_port_send_as(rs485_role_t role, const uint8_t *buf, uint16_t len)
{
    rs485_acq_result_t acq;
    rs485_status_t     st;

    if (buf == 0 || len == 0u) {
        return RS485_ERR_PARAM;
    }
    if (role != RS485_ROLE_SLAVE && role != RS485_ROLE_MASTER) {
        return RS485_ERR_PARAM;
    }

    /* ---- 1) 申请发送权（仲裁在这里发生） ---- */
    acq = bus_try_acquire(role);
    if (acq != RS485_ACQ_OK) {
        /* 拿不到就立刻返回，不阻塞等待。
           理由：主站轮询是周期性的，下一轮再试代价极小；
           而在这里自旋等待会让主循环被拖住，反而影响从站响应。 */
        return RS485_ERR_BUSY;
    }

    /* ---- 2) 发送 ---- */
    st = tx_frame_now(buf, len);
    if (st != RS485_OK) {
        /* 发失败也必须把总线还回去，否则整条总线被永久占住 */
        bus_release();
        return st;
    }

    if (role == RS485_ROLE_SLAVE) {
        s_tx_slave++;
        /* 从站：响应发完就没事了，立即释放 */
        bus_release();
    } else {
        s_tx_master++;
        /* 主站：发送权要继续持有到"收到响应或超时" ——
           由调用方通过 rs485_bus_enter_wait()/exit_wait() 管理。
           ⚠️ 但注意：走主站路径的调用方应该用 rs485_port_send_master_held()，
              本分支只服务于"图省事、一次调用搞定申请+发送"的用法。 */
    }

    return RS485_OK;
}

int rs485_bus_acquire_master(void)
{
    return (bus_try_acquire(RS485_ROLE_MASTER) == RS485_ACQ_OK) ? 0 : -1;
}

rs485_status_t rs485_port_send_master_held(const uint8_t *buf, uint16_t len)
{
    rs485_status_t st;

    if (buf == 0 || len == 0u) {
        return RS485_ERR_PARAM;
    }

    /* ⚠️ 只在"确实持有主站身份"时才发。
       这条检查是 2026-10-10 加的：当时模板把申请角色配成了从站，
       于是这里会永远 BUSY —— 而现象只是"主站安静地不发请求"，
       四万次重试也看不出原因。现在它有一个**专门的计数**，
       读 rs485_get_stats()->tx_role_mismatch 就能一眼定位。 */
    if (rs485_arb_owner(&s_arb) != RS485_ROLE_MASTER) {
        g_stats.tx_role_mismatch++;
        return RS485_ERR_BUSY;
    }

    st = tx_frame_now(buf, len);
    if (st == RS485_OK) {
        s_tx_master++;
    }
    return st;
}

rs485_status_t rs485_port_send(const uint8_t *buf, uint16_t len)
{
    return rs485_port_send_as(RS485_ROLE_SLAVE, buf, len);
}

/* ==================================================================
 * 主站等待窗口管理
 *
 * 为什么要有"等待窗口"这个显式状态：
 *   主站发完请求后到收到响应之间，总线上不能有别的东西在发。
 *   仲裁层必须知道这个窗口存在，才能在收到针对本机的请求时
 *   做出"打断等待、优先响应"的决策（见头文件的场景 A）。
 * ================================================================== */

void rs485_bus_enter_wait(void)
{
    (void)rs485_arb_enter_wait(&s_arb);
}

void rs485_bus_exit_wait(void)
{
    rs485_arb_exit_wait(&s_arb);
}

int rs485_bus_in_wait(void)
{
    return rs485_arb_in_wait(&s_arb);
}

int rs485_bus_release_master(void)
{
    /* 只在"当前是主站持权"时释放。
       若不判断就无条件 release，会把从站的持权也一并释放掉 ——
       而那个持权可能正是某次响应发送所依赖的。 */
    if (rs485_arb_owner(&s_arb) != RS485_ROLE_MASTER) {
        return -1;
    }
    bus_release();
    return 0;
}

int rs485_master_holds_bus(void)
{
    return (rs485_arb_owner(&s_arb) == RS485_ROLE_MASTER) ? 1 : 0;
}

int rs485_bus_acquire(rs485_role_t role)
{
    return (int)bus_try_acquire(role);
}

rs485_role_t rs485_bus_owner(void)
{
    return rs485_arb_owner(&s_arb);
}

const gw_bus_stat_t *rs485_bus_stats(void)
{
    const rs485_arb_stat_t *as = rs485_arb_stats(&s_arb);

    /* 合并成对外视图：仲裁器的决策统计 + 移植层的发送计数。
       这样调用方只查一个结构体就能拿到全部总线状态，
       而两个模块各自只维护自己那份职责内的数据。 */
    if (as != 0) {
        s_bus_view = *as;
    }
    s_bus_view.tx_slave  = s_tx_slave;
    s_bus_view.tx_master = s_tx_master;
    return &s_bus_view;
}

/* ⚠️ `rs485_role_str()` 的实现**不在这里**。
 *
 * 它只把一个枚举转成字符串，是**纯逻辑**，实现在 `src/rs485_arbiter.c`；
 * 声明在 `inc/rs485_arbiter.h`。
 *
 * 本文件原来也定义了一份 —— 那时移植层与纯逻辑层同时进链接，
 * 链接器报 `multiple definition of 'rs485_role_str'`。
 *
 * 教训：移植层只应该做"纯逻辑层做不到的事"（碰寄存器、碰 DMA、碰中断）。
 *       任何能在一台 PC 上算出来的东西都不该在这里再写一遍 ——
 *       写两遍不只是重复，它会**各自演化**，最后两个版本行为不一致，
 *       而且只有链接到同一个可执行文件时才会暴露。
 */

int rs485_port_restart_rx(void)
{
    /* 停掉当前的接收（DMA 或 IT 两条路径都要处理） */
    (void)HAL_UART_AbortReceive(&huart2);

    /* 复位帧状态：切换波特率后旧数据一定是垃圾，不能留着 */
    s_rx_len      = 0u;
    s_rx_overflow = 0u;
    s_state       = RS485_ST_IDLE;

    rs485_arm_receive();
    return (s_state == RS485_ST_ERROR) ? -1 : 0;
}

rs485_status_t rs485_port_set_addr(uint8_t slave_addr)
{
    if (slave_addr < MB_ADDR_MIN || slave_addr > MB_ADDR_MAX) {
        return RS485_ERR_PARAM;
    }
    g_slave.slave_addr = slave_addr;
    return RS485_OK;
}

rs485_state_t rs485_port_state(void)
{
    return s_state;
}

const rs485_stats_t *rs485_get_stats(void)
{
    return &g_stats;
}

mb_slave_t *rs485_get_slave(void)
{
    return &g_slave;
}

mb_regs_t *rs485_get_regs(void)
{
    return &g_regs;
}

/* ==================================================================
 * 中断侧
 *
 * 纪律：中断里只做"记录长度 + 置状态"，绝不做协议解析或发送。
 *      原因见 rs485_port.h 开头的"为什么 2"。
 * ================================================================== */

#if (GW_HAL_HAS_UARTEX_IDLE == 1)

/**
 * V1.8.7 路径：IDLE 事件与 DMA 传输完成事件都会进这里。
 *
 * ⚠️ 这是**全工程唯一**的一份 HAL_UARTEx_RxEventCallback。
 *
 * HAL 把这两个回调声明为 __weak，所以每个工程只能提供一份非 weak 定义。
 * 本工程有两个串口（RS485 用 USART2、ESP32 用 USART3）都需要这个回调，
 * 如果两个移植层各写一份，链接时会报 multiple definition。
 *
 * 解决办法就是这里：**按 Instance 分派**。
 * 每个端口各自提供一个"处理函数"，由这一份统一回调转发。
 * 这样既满足"只能有一份定义"的约束，又保持两个端口互不干扰。
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if (huart->Instance == USART3) {
        /* ESP32 端口：数据进环形缓冲，立即重新武装 */
        esp32_port_on_rx_event(Size);
        return;
    }
    if (huart->Instance != USART2) {
        return;
    }

    /* ==================================================================
     *  ⚠️⚠️ 关于"接收停掉"的两条路径 —— 必须分清
     *
     *  本回调会被**两种**事件触发，HAL 对它们的处理**不一样**：
     *
     *    ① HAL_UART_RXEVENT_IDLE —— 线路静默，帧结束（正常情形）
     *       HAL 在 UART ISR 里**已经**做完了收尾（见 stm32f1xx_hal_uart.c 的
     *       UART_IRQHandler 里 IDLE 分支）：
     *           ATOMIC_CLEAR_BIT(CR1, USART_CR1_PEIE)
     *           ATOMIC_CLEAR_BIT(CR3, USART_CR3_DMAR)   <- 断开 DMA 请求
     *           huart->RxState = HAL_UART_STATE_READY
     *           ATOMIC_CLEAR_BIT(CR1, USART_CR1_IDLEIE)
     *           HAL_DMA_Abort(huart->hdmarx)
     *       **注意最后一句：HAL 自己已经 Abort 了 DMA。**
     *
     *    ② HAL_UART_RXEVENT_TC —— DMA 把缓冲区收**满**了（Size == 缓冲大小）
     *       这条路径 HAL **不 Abort**（它认为传输正常完成），
     *       而 DMA 通道可能仍挂在 RX 请求上。
     *
     *  ----------------------------------------------------------------------------
     *  ⚠️ 原来的写法（只有一个 `s_rx_overflow++` 计数）是**假的保护**
     *
     *      它以为"回调里不重新武装"就能让应用层先取走数据。但那不成立：
     *      **不重新武装只保证不再启动新 DMA，并不让已经启动的 DMA 停下来。**
     *
     *      于是"帧 1 还没被 take_frame 取走、帧 2 又到了"时，
     *      DMA 会直接写进同一个 s_rx_dma_buf，**帧 1 被就地覆盖**；
     *      若主循环恰在两次回调之间取数据，会取到
     *      "帧 1 的长度 + 帧 2 的内容"（长度与内容不匹配）。
     *      计数器不会保护数据。
     *
     *  ----------------------------------------------------------------------------
     *  修法：两条路径都保证"拿完一帧后接收是停的"
     *
     *      ① IDLE 路径：HAL 已 Abort，这里再调一次是**幂等**的
     *         （它只是再清一遍位、再关一次已关的通道）。
     *         保留这一句是为了**两条路径的最终状态一致**，
     *         不必依赖"HAL 内部实现细节"。
     *      ② TC 路径：HAL 没 Abort，**这一句是必需的**。
     *
     *      然后 rs485_port_take_frame() 搬走数据后再重新武装。
     * ================================================================== */
    if (s_state == RS485_ST_RX_READY) {
        /* 过载：上一帧还没被取走。计数 + 让新数据进不来，而不是覆盖旧数据。 */
        g_stats.rx_overflow++;
    }

    s_rx_len = Size;
    g_stats.rx_bytes += Size;

    /* ⚠️ 只有"缓冲区被收满"（TC 事件）才说明可能有更多字节没进来 ——
     *    那才是真正的溢出/截断。
     *
     *    原来写的是 `if (Size >= sizeof(s_rx_dma_buf)) s_rx_overflow = 1u;`
     *    但 **MB_MAX_ADU 恰好就是 256**，也就是"合法的最大 Modbus 帧"
     *    正好等于缓冲区大小 —— 于是一个完全合法的 256 字节帧
     *    会被误标成"溢出"。
     *
     *    用 RxEventType 区分之后语义就准确了：
     *        IDLE 事件 + Size==256 -> 帧完整，没有溢出
     *        TC   事件 + Size==256 -> 缓冲满了，后面可能还有字节（真溢出）
     *
     *    ⚠️ 这个区分只有 V1.8.7 的 ReceiveToIdle 路径才有，
     *       所以用 #if 包起来，老固件包退回原来的保守判断。 */
#if (GW_HAL_HAS_UARTEX_IDLE == 1)
    if (Size >= (uint16_t)sizeof(s_rx_dma_buf)
        && HAL_UARTEx_GetRxEventType(huart) != HAL_UART_RXEVENT_IDLE) {
        s_rx_overflow = 1u;         /* 缓冲区被收满：帧可能被截断 */
    }
#else
    if (Size >= (uint16_t)sizeof(s_rx_dma_buf)) {
        s_rx_overflow = 1u;
    }
#endif

    s_state = RS485_ST_RX_READY;

    /* 两条路径的最终状态都保证"接收已停"。见上方说明。 */
    (void)HAL_UART_AbortReceive(&huart2);

    /* 仍然故意不重新武装：等 rs485_port_take_frame() 取走数据后再武装。
       现在"不重新武装"是真的有效了 —— 因为接收确实已经停了。 */
}

/**
 * 全工程唯一的一份 HAL_UART_ErrorCallback，按 Instance 分派。
 * 见上面 HAL_UARTEx_RxEventCallback 的说明。
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3) {
        esp32_port_on_error();
        return;
    }
    if (huart->Instance != USART2) {
        return;
    }
    g_stats.hal_errors++;
    /* 出错后重新武装，否则接收会永久停摆 */
    s_state = RS485_ST_IDLE;
    rs485_arm_receive();
}

#else  /* ---------------- V1.8.0 路径 ---------------- */

/**
 * V1.8.0 路径的中断处理。
 *
 * 用法：在 CubeMX 生成的 stm32f1xx_it.c 里，USART2_IRQHandler() 内调用
 *          rs485_port_irq_handler();
 *       然后由它统一处理 IDLE 标志与接收中断。
 *
 * 思路：HAL_UART_Receive_IT 每收一个字节进一次中断。
 *       检测到 IDLE 标志时说明总线空闲了一个字符时间，一帧结束。
 *       此刻用 DMA 剩余计数或已收字节数算出帧长。
 *       这里采用的是"每字节进中断、收到 IDLE 判定结束"的方式，
 *       CPU 参与度高于 V1.8.7 的 DMA 方案，但兼容老固件包。
 */
void rs485_port_irq_handler(void)
{
    /* ---- 先判 IDLE：总线空闲一个字符时间 = 一帧结束 ---- */
    if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_IDLE) != RESET) {
        __HAL_UART_CLEAR_IDLEFLAG(&huart2);

        if (s_state == RS485_ST_RX_READY) {
            g_stats.rx_overflow++;
        } else if (s_rx_len > 0u) {
            g_stats.rx_bytes += s_rx_len;
            s_state = RS485_ST_RX_READY;
        }
        return;
    }

    /* ---- 其余中断交给 HAL 处理（它会调用 RxCpltCallback） ---- */
    HAL_UART_IRQHandler(&huart2);
}

/**
 * V1.8.0 路径：每收到一个字节触发一次。
 *
 * ⚠️ 这是**全工程唯一**的一份 HAL_UART_RxCpltCallback，
 * 同时服务 USART2（RS485）与 USART3（ESP32），按 Instance 分派。
 * 理由与 HAL_UARTEx_RxEventCallback 相同（weak 函数只能覆盖一次）。
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3) {
        /* 中断时 pRxBuffPtr 已被 HAL 推进到下一个位置，
           所以刚收到的那个字节在它前面一格。
           这是 HAL 的标准做法，比自己维护一个私有变量可靠。 */
        esp32_port_on_rx_byte(*(huart->pRxBuffPtr - 1));
        return;
    }
    if (huart->Instance != USART2) {
        return;
    }

    /* HAL_UART_Receive_IT 每次只收 1 字节，所以每进一次中断长度 +1 */
    if (s_rx_len < (uint16_t)(sizeof(s_rx_dma_buf) - 1u)) {
        s_rx_len++;
    } else {
        s_rx_overflow = 1u;
    }
    s_state = RS485_ST_RX;

    /* 继续收下一字节（覆盖式接收，位置由 HAL 内部 pRxBuffPtr 推进） */
    if (s_rx_len < (uint16_t)sizeof(s_rx_dma_buf)) {
        (void)HAL_UART_Receive_IT(&huart2, &s_rx_dma_buf[s_rx_len], 1u);
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART2) {
        return;
    }
    g_stats.hal_errors++;
    s_state = RS485_ST_IDLE;
    rs485_arm_receive();
}

#endif /* GW_HAL_HAS_UARTEX_IDLE */

/* ==================================================================
 *  接收路径诊断 —— 定位"高速连续请求后从站永久停止响应"
 *
 *  ⚠️ 这是一段**专门为排查真实故障加的临时代码**，
 *     问题定位后应当删掉（或改成编译开关）。
 *
 *  ----------------------------------------------------------------------------
 *  已确认的故障现象（上板实测）
 *
 *    1) 连续几百次高速 Modbus 请求后，从站**永久不再应答**
 *    2) **按复位键无效** —— 连 [BOOT] 横幅都不打印
 *    3) **拔插 USB（断电）后恢复**，且收帧计数归 0
 *       -> 证明是断电，不是软复位
 *
 *    结论：外设（USART2 / DMA1）被卡在了**软复位清不掉的硬件状态**。
 *
 *  ----------------------------------------------------------------------------
 *  为什么必须把状态映射到寄存器
 *
 *    卡死之后串口不通、调试口也不出东西，**看不到任何内部状态**，
 *    只能靠猜。把关键变量映射到输入寄存器 28~31 之后，
 *    就能在"还没完全死"的窗口里读到它究竟卡在哪一步。
 *
 *  ⚠️ 读寄存器这个动作本身会经过 USART2 的接收路径，
 *     所以如果接收已经彻底死掉，这些寄存器也读不到。
 *     真正有用的场景是"**读取响应开始变得不稳定**"的那段时间 ——
 *     那时还能读到状态，能看出是哪个标志先变坏。
 * ================================================================== */
void rs485_port_fill_diag(uint16_t *iregs, uint16_t count)
{
    uint32_t st = 0u;

    if (iregs == 0 || count < (uint16_t)(IREG_DIAG_CNDTR + 1)) {
        return;
    }

    /* ---- 状态枚举（原样放进去，不折算） ---- */
    st |= ((uint32_t)s_state << IREG_DIAG_ST_SHIFT) & IREG_DIAG_ST_MASK;

    if (s_rx_overflow != 0u) { st |= IREG_DIAG_BIT_RX_OVERFLOW; }
    if ((g_stats.hal_errors != 0u)
        || ((huart2.Instance->SR & (USART_SR_ORE | USART_SR_NE
                                    | USART_SR_FE | USART_SR_PE)) != 0u)) {
        st |= IREG_DIAG_BIT_HAS_RX_ERR;
    }
    if (g_stats.tx_frames != 0u) { st |= IREG_DIAG_BIT_TX_OK; }

    /* ---- HAL / USART / DMA 寄存器的实时状态 ---- */
    if ((huart2.Instance->CR1 & USART_CR1_IDLEIE) != 0u) {
        st |= IREG_DIAG_BIT_IDLEIE;
    }
    if ((huart2.Instance->CR3 & USART_CR3_DMAR) != 0u) {
        st |= IREG_DIAG_BIT_DMAR;
    }
    if ((huart2.Instance->CR1 & USART_CR1_RE) != 0u) {
        st |= IREG_DIAG_BIT_RE;
    }
    if (huart2.RxState == HAL_UART_STATE_READY) {
        st |= IREG_DIAG_BIT_HAL_RXREADY;
    }

#if (GW_HAL_HAS_UARTEX_IDLE == 1)
    if ((hdma_usart2_rx.Instance->CCR & DMA_CCR_EN) != 0u) {
        st |= IREG_DIAG_BIT_RX_ARMED;
    }
    iregs[IREG_DIAG_CNDTR] = (uint16_t)(hdma_usart2_rx.Instance->CNDTR);
#else
    iregs[IREG_DIAG_CNDTR] = 0u;
#endif

    iregs[IREG_DIAG_STATE]  = (uint16_t)(st & 0xFFFFu);
    iregs[IREG_DIAG_RX_LEN] = s_rx_len;

    /* ⚠️ 读 SR 有个副作用：**读 SR 再读 DR 会清掉 ORE/NE/FE/PE**。
       这里是诊断代码，读到 SR 就能把"曾经出过接收错误"的信息捕获下来
       （见上面的 IREG_DIAG_BIT_HAS_RX_ERR），
       但也**可能因此清掉一个真实的错误标志**。
       诊断期间可以接受，但这也是它不该长期留在生产代码里的原因之一。 */
    iregs[IREG_DIAG_SR] = (uint16_t)(huart2.Instance->SR & 0xFFFFu);
}

