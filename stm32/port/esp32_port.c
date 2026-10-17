/**
 * @file    esp32_port.c
 * @brief   ESP32 联网模组通信层实现（USART3，115200 8N1）
 *
 * ============================================================================
 *  接收路径设计：中断只搬字节，切帧在主循环
 *
 *    USART3 中断
 *        |  只做一件事：把收到的字节写进环形缓冲
 *        v
 *    [s_rx_ring 环形缓冲]
 *        |  esp32_port_tick() 在主循环里把字节喂给帧解析器
 *        v
 *    [gw_frame_parser 状态机]  处理粘包/半包/噪声重同步
 *        |  esp32_port_take_frame() 逐帧取出
 *        v
 *    上层业务（gw_uplink 处理链路状态与 ACK；gw_app 处理下行指令）
 *
 *  ----------------------------------------------------------------------------
 *  为什么中断里只搬字节
 *
 *  ESP32 的帧可能一下子来 133 字节。若在中断里做 CRC 校验与载荷拷贝，
 *  中断会占用几百微秒 —— 而 RS485 那边有 3.5 字符时间的帧界要求
 *  （9600 波特率下约 3.6ms，留给中断的余量很小）。
 *  中断里拖太久会让 Modbus 从站丢帧。
 *
 *  所以这里用环形缓冲解耦：中断里只做"存一个字节"（几百纳秒），
 *  剩下的活全部搬到主循环。代价是需要一个环形缓冲，
 *  收益是 Modbus 的实时性不受 ESP32 流量影响。
 *
 *  ----------------------------------------------------------------------------
 *  环形缓冲满时丢最旧还是丢最新？
 *
 *  丢**最旧**。理由：串口帧协议是有序的，丢中间某几个字节会让
 *  当前帧 CRC 失败并触发重同步（丢失整帧）；
 *  而丢最旧的整块字节等价于"丢掉了最早的那一帧"，
 *  后续帧的边界仍然是完好的。
 *
 *  两种做法都会丢数据，但丢最旧能让**后面的数据保持可解析**，
 *  这在"链路打满"的场景下更重要。
 * ============================================================================
 */
#include "esp32_port.h"

/* 注意包含顺序：gw_config.h 使用 HAL 的 GPIO_PIN_n 宏与 GPIOB 端口宏
   （用于引脚分配和编译期断言），因此必须先包含 HAL 头、再包含它。
   gw_config.h 里有守卫会检查这一点并给出明确提示。 */
#include "stm32f1xx_hal.h"
#include "gw_config.h"

/* ------------------------------------------------------------------
 * 外部依赖：CubeMX 生成的对象
 *
 * 如果链接时提示 huart3 / hdma_usart3_rx 未定义，说明 CubeMX 里
 * USART3（或它的 RX DMA 通道）没使能，回去检查 .ioc 配置。
 * ------------------------------------------------------------------ */
extern UART_HandleTypeDef huart3;
#if (GW_HAL_HAS_UARTEX_IDLE == 1)
extern DMA_HandleTypeDef  hdma_usart3_rx;
#endif

/* ------------------------------------------------------------------
 * 环形缓冲
 *
 * 容量取 256：够放下连着的两帧最大帧（133 × 2 = 266 略超，
 * 但实际很少出现背靠背的最大帧）。取值本身就是权衡：
 * 太小会在突发时丢数据，太大浪费 RAM。
 * 溢出会计数并上报，所以"够不够用"可以从统计里看出来，
 * 而不是靠猜。
 * ------------------------------------------------------------------ */
#define ESP32_RX_RING_SIZE  256u
#define ESP32_RX_RING_MASK  (ESP32_RX_RING_SIZE - 1u)

/* 编译期检查：环形缓冲容量必须是 2 的幂，否则取模运算会出错 */
_Static_assert((ESP32_RX_RING_SIZE & ESP32_RX_RING_MASK) == 0u,
               "ESP32_RX_RING_SIZE 必须是 2 的幂");

static uint8_t           s_ring[ESP32_RX_RING_SIZE];
static volatile uint16_t s_ring_head = 0u;      /* 中断侧写入位置 */
static volatile uint16_t s_ring_tail = 0u;      /* 主循环侧读取位置 */

/* 硬件直收缓冲。
 *
 * V1.8.7 路径：DMA 直接往这里写，中断时把这段拷进环形缓冲。
 *             大小取 GW_FRAME_RX_BUF_SIZE，保证一帧不会被 DMA 截断。
 * V1.8.0 路径：HAL_UART_Receive_IT 每次收 1 字节到 s_dma_buf[0]，
 *             所以只需要 1 字节，但保留完整尺寸不影响（浪费 256 字节 RAM，
 *             换来两版路径共用同一份声明，少一处条件编译就少一处出错机会）。
 *
 * ⚠️ 早期版本把它写在 `#if (GW_HAL_HAS_UARTEX_IDLE == 1)` 里面，
 *    结果 V1.8.0 路径编译时报"未声明" —— 两版路径都要用它。 */
static uint8_t s_dma_buf[GW_FRAME_RX_BUF_SIZE];

/* 帧解析器：处理粘包/半包/重同步 */
static gw_frame_parser_t s_parser;

static esp32_stats_t s_stats;
static uint8_t       s_inited = 0u;

/* 取帧输出（复用缓冲，避免在栈上放大数组） */
static uint8_t  s_tx_buf[GW_FRAME_MAX_TOTAL];

/* ==================================================================
 * 内部：环形缓冲
 * ================================================================== */

/**
 * 往环形缓冲塞一个字节（中断侧调用）
 * @return 1 成功；0 缓冲满（已丢最旧一个字节并计数）
 */
static int ring_put(uint8_t b)
{
    uint16_t next = (uint16_t)((s_ring_head + 1u) & ESP32_RX_RING_MASK);

    if (next == s_ring_tail) {
        /* 满了：丢弃最旧的一个字节，腾出位置。
           见文件开头关于"丢最旧"的说明。 */
        s_ring_tail = (uint16_t)((s_ring_tail + 1u) & ESP32_RX_RING_MASK);
        s_stats.rx_overflows++;
    }
    s_ring[s_ring_head] = b;
    s_ring_head = next;
    return 1;
}

/** 从环形缓冲取一个字节（主循环侧调用） */
static int ring_get(uint8_t *out)
{
    if (s_ring_tail == s_ring_head) {
        return 0;                   /* 空 */
    }
    *out = s_ring[s_ring_tail];
    s_ring_tail = (uint16_t)((s_ring_tail + 1u) & ESP32_RX_RING_MASK);
    return 1;
}

/* ==================================================================
 * 内部：武装接收
 * ================================================================== */
static void esp32_arm_receive(void)
{
#if (GW_HAL_HAS_UARTEX_IDLE == 1)
    (void)HAL_UARTEx_ReceiveToIdle_DMA(&huart3, s_dma_buf,
                                       (uint16_t)sizeof(s_dma_buf));
    /* 关掉 DMA 半满中断：半满只表示"收了一半"，
       对按帧处理没有意义，反而会让中断次数翻倍。 */
    if (huart3.hdmarx != 0) {
        __HAL_DMA_DISABLE_IT(huart3.hdmarx, DMA_IT_HT);
    }
#else
    /* V1.8.0 路径：逐字节中断接收 */
    (void)HAL_UART_Receive_IT(&huart3, s_dma_buf, 1u);
#endif
}

/* ==================================================================
 * 对外接口
 * ================================================================== */

esp32_status_t esp32_port_init(void)
{
    s_ring_head = 0u;
    s_ring_tail = 0u;

    s_stats.tx_frames     = 0u;
    s_stats.tx_bytes      = 0u;
    s_stats.tx_errors     = 0u;
    s_stats.rx_bytes      = 0u;
    s_stats.rx_frames     = 0u;
    s_stats.rx_crc_errors = 0u;
    s_stats.rx_overflows  = 0u;
    s_stats.hal_errors    = 0u;

    gw_frame_parser_init(&s_parser);

    /* EN 引脚：若接了，先给模组一个稳定的复位时序。
       没接也不影响 —— HAL_GPIO_WritePin 对一个未配置的引脚是无害的。 */
    HAL_GPIO_WritePin(GW_ESP32_EN_PORT, GW_ESP32_EN_PIN, GPIO_PIN_SET);

    s_inited = 1u;
    esp32_arm_receive();
    return ESP32_OK;
}

esp32_status_t esp32_port_send(const uint8_t *buf, uint16_t len)
{
    if (s_inited == 0u || buf == 0 || len == 0u) {
        return ESP32_ERR_PARAM;
    }
    if (len > (uint16_t)sizeof(s_tx_buf)) {
        return ESP32_ERR_PARAM;
    }

    /* 超时按波特率估算：115200 下一字节约 87us，
       给 5 倍余量。硬编码 1000ms 会让"模组不在"时卡死一秒，
       对本项目的 10ms 节拍是不可接受的。 */
    {
        uint32_t timeout = (uint32_t)len * 87u * 5u / 1000u + 10u;

        if (HAL_UART_Transmit(&huart3, (uint8_t *)buf, len, timeout) != HAL_OK) {
            s_stats.tx_errors++;
            return ESP32_ERR_HAL;
        }
    }

    s_stats.tx_frames++;
    s_stats.tx_bytes += len;
    return ESP32_OK;
}

void esp32_port_tick(void)
{
    uint8_t chunk[32];
    uint16_t n = 0u;

    if (s_inited == 0u) {
        return;
    }

    /* 从环形缓冲批量取出，再一次性喂给解析器。
       批量而不是逐字节喂，是为了减少函数调用次数 ——
       tick 每 10ms 调一次，逐字节会让调用开销超过解析本身。 */
    while (ring_get(&chunk[n]) != 0) {
        n++;
        if (n >= (uint16_t)sizeof(chunk)) {
            s_stats.rx_bytes += n;
            gw_frame_feed(&s_parser, chunk, n);
            n = 0u;
        }
    }
    if (n > 0u) {
        s_stats.rx_bytes += n;
        gw_frame_feed(&s_parser, chunk, n);
    }
}

esp32_status_t esp32_port_take_frame(uint8_t *out_type, uint8_t *payload,
                                     uint16_t cap, uint16_t *out_len)
{
    gw_frame_err_t e;

    if (out_type == 0 || payload == 0 || out_len == 0) {
        return ESP32_ERR_PARAM;
    }

    e = gw_frame_take(&s_parser, out_type, payload, cap, out_len);
    switch (e) {
    case GW_FRAME_OK:
        s_stats.rx_frames++;
        return ESP32_OK;

    case GW_FRAME_ERR_NO_SPACE:
        return ESP32_ERR_NO_FRAME;

    case GW_FRAME_ERR_CRC:
        /* CRC 错：解析器已自行重同步，调用方可以继续取下一帧。
           这里计入统计而不是当成致命错误 —— 线路上偶发坏帧是正常的。 */
        s_stats.rx_crc_errors++;
        return ESP32_ERR_SIZE;

    case GW_FRAME_ERR_TOO_BIG:
        /* 载荷比调用方缓冲区还大：说明两边的契约不一致（代码 bug），
           不是线路噪声，必须计数以便暴露出来。 */
        s_stats.rx_overflows++;
        return ESP32_ERR_TOO_BIG;

    default:
        return ESP32_ERR_SIZE;
    }
}

const esp32_stats_t *esp32_port_stats(void)
{
    return &s_stats;
}

const gw_frame_stat_t *esp32_port_frame_stats(void)
{
    return gw_frame_stats(&s_parser);
}

void esp32_port_reset_module(uint32_t hold_ms)
{
    uint32_t start = HAL_GetTick();

    HAL_GPIO_WritePin(GW_ESP32_EN_PORT, GW_ESP32_EN_PIN, GPIO_PIN_RESET);

    /* 忙等而不是 HAL_Delay：HAL_Delay 依赖 SysTick 中断，
       在某些初始化阶段（中断未开）会死等。
       这里的等待时间很短（通常 10~100ms），忙等可接受。 */
    while ((HAL_GetTick() - start) < hold_ms) {
        /* 空转 */
    }

    HAL_GPIO_WritePin(GW_ESP32_EN_PORT, GW_ESP32_EN_PIN, GPIO_PIN_SET);

    /* 复位后清空接收状态：模组重启后可能吐出半截旧数据 */
    s_ring_head = 0u;
    s_ring_tail = 0u;
    gw_frame_parser_init(&s_parser);
}

/* ==================================================================
 * 中断侧
 *
 * 纪律：中断里只做"搬字节 + 计数"，绝不做切帧或 CRC 校验。
 *      原因见文件开头的说明（不能影响 Modbus 从站的实时性）。
 *
 * ⚠️ 本文件**不定义** HAL_UARTEx_RxEventCallback 与 HAL_UART_ErrorCallback。
 *
 * HAL 把这两个回调声明为 __weak，每个工程只能提供一份非 weak 定义。
 * 本工程有两个串口（USART2 给 RS485、USART3 给 ESP32）都需要它们，
 * 所以统一定义在 rs485_port.c 里，由它按 Instance 分派。
 * 本文件只提供两个"被分派到"的处理函数。
 *
 * 踩过的坑：最初两个移植层各写了一份回调，
 * 编译能过、链接时报 multiple definition ——
 * 而且报错信息指向 HAL 库文件，第一眼看不出是自己的问题。
 * ================================================================== */

#if (GW_HAL_HAS_UARTEX_IDLE == 1)

/** 由 rs485_port.c 的统一回调转发：USART3 收到了一段数据 */
void esp32_port_on_rx_event(uint16_t size)
{
    uint16_t i;

    if (size > (uint16_t)sizeof(s_dma_buf)) {
        size = (uint16_t)sizeof(s_dma_buf);     /* 防御：不该发生 */
    }
    for (i = 0u; i < size; i++) {
        (void)ring_put(s_dma_buf[i]);
    }

    /* 立刻重新武装：与 RS485 不同，ESP32 这边不需要"等上层取走"，
       因为数据已经进了环形缓冲，DMA 缓冲可以马上复用。
       不重新武装会让模组后续所有数据都收不到。 */
    esp32_arm_receive();
}

#else  /* ---------------- V1.8.0 路径 ---------------- */

/**
 * V1.8.0 路径：由 rs485_port.c 的统一回调转发一段数据。
 *
 * DMA 路径的 on_rx_event 在 V1.8.0 下没有对应物（那时是逐字节中断），
 * 但 rs485_port.c 的分派器引用了它，所以给一个不做事的实现，
 * 让分派器代码在两版路径下保持一致（理由同 esp32_port_on_rx_byte）。
 */
void esp32_port_on_rx_event(uint16_t size)
{
    (void)size;
}

/**
 * V1.8.0 路径的中断处理
 *
 * 用法：在 stm32f1xx_it.c 的 USART3_IRQHandler() 内调用
 *          esp32_port_irq_handler();
 */
void esp32_port_irq_handler(void)
{
    if (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_IDLE) != RESET) {
        __HAL_UART_CLEAR_IDLEFLAG(&huart3);
    }
    HAL_UART_IRQHandler(&huart3);
}

#endif /* GW_HAL_HAS_UARTEX_IDLE */

/**
 * V1.8.0 路径：由 rs485_port.c 里唯一的 HAL_UART_RxCpltCallback 转发一个字节。
 *
 * ⚠️ 本函数在 V1.8.7 路径下定义为**空操作**。
 *    为什么要有这个空定义，而不是用 #if 把它整个去掉：
 *      rs485_port.c 里的分派器需要无条件引用它（那边也有一层条件编译，
 *      两边的条件必须严格对应，任何一处漏改都会变成"某个 HAL 版本编不过"）。
 *      给一个空的实现可以让"分派器"的代码在两版路径下完全一致，
 *      减少条件编译的组合数 —— 而条件编译的组合数正是这类代码最容易出错的地方。
 *
 *    代价是 V1.8.7 路径下多一个空函数的调用点（编译器会内联掉，无实际开销）。
 */
void esp32_port_on_rx_byte(uint8_t byte)
{
#if (GW_HAL_HAS_UARTEX_IDLE == 0)
    (void)ring_put(byte);
    (void)HAL_UART_Receive_IT(&huart3, s_dma_buf, 1u);
#else
    (void)byte;                 /* V1.8.7 走 DMA 路径，不需要逐字节处理 */
#endif
}

/** 由 rs485_port.c 的统一错误回调转发：USART3 出错 */
void esp32_port_on_error(void)
{
    s_stats.hal_errors++;
    s_ring_head = 0u;
    s_ring_tail = 0u;
    gw_frame_parser_init(&s_parser);
    esp32_arm_receive();
}

