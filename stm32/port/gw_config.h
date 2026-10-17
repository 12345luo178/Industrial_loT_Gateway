/**
 * @file    gw_config.h
 * @brief   项目级编译期配置（引脚、外设、HAL 能力开关）
 *
 * ============================================================================
 *  为什么需要这个文件：
 *
 *  1) CubeMX 重新生成代码时，会覆盖 gpio.c / usart.c / main.h 里的宏。
 *     如果把依赖集中在 main.h 上，一旦 CubeMX 配置改了、User Label 忘了设，
 *     就会以"未定义符号"的形式在编译期爆炸，而不是给出可读的提示。
 *     这里做一次显式映射，把"配置问题"变成"编译期断言"。
 *
 *  2) F1 的 HAL 有两个代际：V1.8.0 没有 HAL_UARTEx_ReceiveToIdle_DMA，
 *     V1.8.7 才有。这个差异必须有一个开关来隔离（见 GW_HAL_HAS_UARTEX_IDLE），
 *     否则同一份代码换固件包就编译不过。
 *
 *  3) 引脚号集中在此，避免散落在各驱动文件里 —— 改板子只改一处。
 * ============================================================================
 */
#ifndef GW_CONFIG_H
#define GW_CONFIG_H

/* ==========================================================================
 * 包含顺序要求（重要）
 *
 * 本文件使用 HAL 的 GPIO_PIN_n 宏来做引脚分配与编译期检查，
 * 因此 **必须先包含 stm32f1xx_hal.h（或 main.h）**。
 *
 * 下面的守卫把"顺序错误"变成一条清晰的编译期提示，
 * 而不是一堆 "'GPIO_PIN_1' undeclared" 的噪声报错。
 *
 * ⚠️ 这里**不要**再补 `#include "stm32f1xx.h"`。
 *    移植层的写法是"先 stm32f1xx_hal.h、再 gw_config.h"，
 *    而 `stm32f1xx_hal.h` 第 59 行就用了 `__IO`，
 *    那时候 HAL 的模块头（提供 `HAL_StatusTypeDef`）还没轮到。
 *    补 CMSIS 设备头这件事只能在 `stm32f1xx_hal_conf.h` 里做 ——
 *    那是 hal.h 展开过程中的第 29 行，是唯一来得及的位置。
 *    详见 `tools/hal_conf_check/stm32f1xx_hal_conf.h` 里的注释。
 * ========================================================================== */
#ifndef GPIO_PIN_0
  #error "包含顺序错误：请先 #include \"stm32f1xx_hal.h\"（或 \"main.h\"），再 #include \"gw_config.h\""
#endif

/* ==========================================================================
 * 一、HAL 能力探测
 *
 * F1 固件包版本与 IDLE 接收 API 的对应关系（已在本机核实）：
 *   STM32Cube_FW_F1_V1.8.0 —— 无 HAL_UARTEx_ReceiveToIdle_DMA
 *   STM32Cube_FW_F1_V1.8.7 —— 有 HAL_UARTEx_ReceiveToIdle_DMA
 *
 * 解决办法二选一（详见 doc/HAL版本与IDLE接收方案.md）：
 *   方案 A（推荐）：CubeIDE 里把固件包切到 V1.8.7，IDLE+DMA 一行搞定
 *   方案 B：留在 V1.8.0，用"首次 Receive_IT 触发 + 纯 IDLE 中断"实现
 *
 * 这里用一个开关隔离差异，两种固件包都能编过。
 *
 * ⚠️⚠️ 默认值改过，原因值得记下来
 *
 *   原来是 **0**（按 V1.8.0 保守编译）。那个选择在上板时暴露了问题：
 *
 *       CubeIDE 工程里**没有定义**这个宏 -> 走 0 的路径 -> 用
 *       `HAL_UART_Receive_IT` 逐字节中断收帧。
 *       而工程实际用的是 V1.8.7（有 IDLE+DMA）
 *       -> **主方案被静默绕过了**，跑的是回退方案。
 *
 *   后果不是"不能用"，而是"用的不是设计的那条路"：
 *       - 逐字节中断：115200 下每字节约 87us 一次中断，CPU 开销明显
 *       - 而且回退路径**没有** `__HAL_DMA_DISABLE_IT(HT)` 那一步，
 *         所以"一帧超过半个缓冲区被切成两帧"的风险也没被消除
 *
 *   教训：**"保守的默认值"在集成时是坑** ——
 *   它让"配置漏了"表现为"静默降级"，而不是"编译报错"。
 *
 *   现在默认 **1**，与本项目实际使用的 V1.8.7 一致。
 *   如果你确实在用 V1.8.0 或更早，**必须显式定义 0**：
 *       预处理器里加  GW_HAL_HAS_UARTEX_IDLE=0
 * ========================================================================== */
#ifndef GW_HAL_HAS_UARTEX_IDLE
  #define GW_HAL_HAS_UARTEX_IDLE   1   /* V1.8.7+：IDLE + DMA（本项目实际使用） */
#endif

/* ==========================================================================
 * 二、RS485 通信口（USART2）—— 与移植层强相关
 *
 * 注意：本文件只负责"引脚号与协议参数"，
 *      UART 外设本身的初始化仍由 CubeMX 生成的 MX_USART2_UART_Init() 完成。
 *      两边必须一致，不一致的表现为"全是乱码"或"完全收不到"。
 * ========================================================================== */
/* ⚠️ 这个值必须与 CubeMX 里 USART2 的 Baud Rate 一致。
 *
 *   为什么：它**只决定上电瞬间的硬件状态**；
 *   运行时的波特率由 gw_baud 状态机管，
 *   而它的初值是从 gw_baud_port_actual()（读 BRR 反算）得来的 ——
 *   也就是说代码会“承认你配的就是你配的”，
 *   两边不一致时**不会有任何报错**，只会“上位机连不上”。
 *
 *   ✅ 现取 115200：与本项目配套工程 CubeMX 里
 *      USART2 已配的值一致（实测核对过）。
 *   ⚠️ 短线下 115200 没问题；但长线/强干扰环境下
 *      9600 的噪声容忍度更好，那时应同时改这里和 CubeMX。 */
#define GW_RS485_UART_BAUD        115200u

/* 本机 Modbus 从站地址（上电默认值，运行时可由保持寄存器 HREG_SLAVE_ADDR 修改） */
#define GW_RS485_DEFAULT_ADDR     1u

/* ==========================================================================
 * 三、引脚分配表（集中定义，改板只改这里）
 *
 * 这张表同时是对外文档，面试时可用来讲"引脚是怎么分配的、为什么这么分"。
 *
 * 分配原则：
 *   - 按外设功能域隔离：调试口 / 现场总线 / 扩展总线 / 采集 / 显示 各占独立资源
 *   - 定时器与 DMA 通道不冲突（见下表"关键资源"列）
 *   - 预留调试串口与测试点，便于产测与现场排障
 * ========================================================================== */

/* ---- USART1：调试串口（115200 8N1，接 USB-TTL） ---- */
#define GW_DEBUG_UART_BAUD        115200u

/* ---- USART2：RS485 现场总线（115200 8N1，接 MAX3485；与 GW_RS485_UART_BAUD 一致，勿只改其一） ---- */
#define GW_RS485_TX_PORT          GPIOA
#define GW_RS485_TX_PIN           GPIO_PIN_2      /* USART2_TX -> MAX3485 DI */
#define GW_RS485_RX_PORT          GPIOA
#define GW_RS485_RX_PIN           GPIO_PIN_3      /* USART2_RX <- MAX3485 RO */
#define GW_RS485_DE_PORT          GPIOA
#define GW_RS485_DE_PIN           GPIO_PIN_1      /* DE 与 RE 并联：高=发送 低=接收 */

/* ---- CAN1：扩展总线（500 kbps，接 TJA1051T/3） ---- */
#define GW_CAN_RX_PORT            GPIOB
#define GW_CAN_RX_PIN             GPIO_PIN_8      /* 需开启 CAN1 重映射 */
#define GW_CAN_TX_PORT            GPIOB
#define GW_CAN_TX_PIN             GPIO_PIN_9
/* 位时序的实现在 gw_can_port.c（含编译期校验），此处只记参数：
   APB1=36MHz, Prescaler=4, BS1=15tq, BS2=2tq, 总18tq, 采样点88.9%
   ⚠️ BS1 的硬件上限是 16，别写 20（CubeMX 里选不出来）*/
#define GW_CAN_BAUDRATE           500000u

/* ---- DHT22：单总线温湿度（PG11） ---- */
#define GW_DHT22_PORT             GPIOG
#define GW_DHT22_PIN              GPIO_PIN_11
#define GW_DHT22_RETRY            3u              /* 校验失败重读次数 */
#define GW_DHT22_FILTER_WIN       5u              /* 滑动均值滤波窗口 N=5 */

/* ---- ESP32：联网模组（USART3，115200 8N1） ----
   注意：ESP32 自己跑 MQTT，STM32 只通过串口帧协议收发数据。 */
#define GW_ESP32_UART_BAUD        115200u
#define GW_ESP32_TX_PORT          GPIOB
#define GW_ESP32_TX_PIN           GPIO_PIN_10     /* USART3_TX -> ESP32 RX */
#define GW_ESP32_RX_PORT          GPIOB
#define GW_ESP32_RX_PIN           GPIO_PIN_11     /* USART3_RX <- ESP32 TX */
#define GW_ESP32_EN_PORT          GPIOB
#define GW_ESP32_EN_PIN           GPIO_PIN_12     /* ESP32 复位/使能，可选 */

/* ---- ILI9341 触摸屏（SPI1）----
   本次开发不写 UI，但引脚已被屏幕占用，配置在此登记避免与其他外设冲突。
   如需释放 SPI1，拔掉屏幕并注释本节即可。 */
#define GW_LCD_SPI_SCK_PORT       GPIOA
#define GW_LCD_SPI_SCK_PIN        GPIO_PIN_5
#define GW_LCD_SPI_MISO_PORT      GPIOA
#define GW_LCD_SPI_MISO_PIN       GPIO_PIN_6
#define GW_LCD_SPI_MOSI_PORT      GPIOA
#define GW_LCD_SPI_MOSI_PIN       GPIO_PIN_7
#define GW_LCD_CS_PORT            GPIOA
#define GW_LCD_CS_PIN             GPIO_PIN_4
#define GW_LCD_DC_PORT            GPIOB
#define GW_LCD_DC_PIN             GPIO_PIN_0
#define GW_LCD_RST_PORT           GPIOB
#define GW_LCD_RST_PIN            GPIO_PIN_1
#define GW_LCD_BL_PORT            GPIOB
#define GW_LCD_BL_PIN             GPIO_PIN_5      /* 背光，PWM 可调 */

/* ---- XPT2046 电阻触摸（与 LCD 共用 SPI1）----
 *  2026-10-11 与用户确认：模块是 **XPT2046**（SPI），所以只多要两个 GPIO。
 *  ⚠️ 这两根**不在 CubeMX 工程里**：由我们的驱动自己配置
 *     （见 port/touch_xpt2046.c 的 touch_hw_init），
 *     这样 CubeMX 重新生成也不会把它们的配置冲掉。
 *  ⚠️ T_IRQ 必须**上拉输入**：模块上的 PENIRQ 是开漏输出，悬空会读到随机值
 *     —— 与 PB8(CAN_RX) 当初不加上拉就读成显性，是同一类坑。
 *  ⚠️ 这两个引脚正好也是 I²C1 的 SCL/SDA。若将来换成电容屏（FT6236），
 *     就是在这里改成 I²C 用途，不需要挪引脚。 */
#define GW_TOUCH_CS_PORT          GPIOB
#define GW_TOUCH_CS_PIN           GPIO_PIN_6
#define GW_TOUCH_IRQ_PORT         GPIOB
#define GW_TOUCH_IRQ_PIN          GPIO_PIN_7

/* ==========================================================================
 * 四、通信参数
 * ========================================================================== */
#define GW_MQTT_KEEPALIVE_S       60u      /* MQTT 心跳周期 */
#define GW_RECONNECT_BACKOFF_MAX_S 60u     /* 重连退避上限 */
#define GW_OFFLINE_QUEUE_DEPTH    512u     /* 断网缓存条数（约 512 × 40B ≈ 20KB） */
#define GW_REPORT_PERIOD_S_DEFAULT 10u     /* 默认上报周期 */

/* ==========================================================================
 * 五、编译期一致性检查
 *
 * 作用：把"配置错误"提前到编译期暴露，而不是等到现场联调。
 *
 * 注意：这里用 _Static_assert 而不是 #if。
 *      原因（实测踩到的坑）：HAL 的端口宏 GPIOA 展开为指针类型表达式
 *      `((GPIO_TypeDef *)(0x40000000UL + ...))`，
 *      在 #if 里对含指针转换的表达式做相等比较属于未定义行为，
 *      会导致断言恒为真而误报。_Static_assert 是真正的 C 表达式，
 *      类型安全、结果可靠。
 * ========================================================================== */

/* 把引脚宏转成可参与常量表达式比较的枚举值 */
enum {
    GW_CFG_RS485_DE_PIN_NUM   = (int)GW_RS485_DE_PIN,
    GW_CFG_RS485_TX_PIN_NUM   = (int)GW_RS485_TX_PIN,
    GW_CFG_RS485_RX_PIN_NUM   = (int)GW_RS485_RX_PIN,
    GW_CFG_RS485_ADDR_NUM     = (int)GW_RS485_DEFAULT_ADDR,
    GW_CFG_DHT22_FILTER_NUM   = (int)GW_DHT22_FILTER_WIN,
    GW_CFG_OFFLINE_DEPTH_NUM  = (int)GW_OFFLINE_QUEUE_DEPTH
};

/* Modbus 从站地址必须落在协议允许范围内（0 是广播地址，不能作为从站地址） */
_Static_assert(GW_CFG_RS485_ADDR_NUM >= 1 && GW_CFG_RS485_ADDR_NUM <= 247,
               "GW_RS485_DEFAULT_ADDR 必须在 1~247");

/* DE 与 TX 不能是同一个引脚，否则方向控制与数据输出会互相冲突 */
_Static_assert(GW_CFG_RS485_DE_PIN_NUM != GW_CFG_RS485_TX_PIN_NUM,
               "RS485 DE 与 TX 不能共用同一引脚");

/* TX 与 RX 也不能相同 */
_Static_assert(GW_CFG_RS485_TX_PIN_NUM != GW_CFG_RS485_RX_PIN_NUM,
               "RS485 TX 与 RX 不能共用同一引脚");

/* 滤波窗口必须 >= 2，否则均值没有意义 */
_Static_assert(GW_CFG_DHT22_FILTER_NUM >= 2,
               "GW_DHT22_FILTER_WIN 至少为 2");

/* 断网缓存深度不能为 0，否则补传机制失去意义 */
_Static_assert(GW_CFG_OFFLINE_DEPTH_NUM > 0,
               "GW_OFFLINE_QUEUE_DEPTH 必须大于 0");

/* 波特率必须是 gw_baud 状态机支持的取值。
   ⚠️ 这里只能做**编译期**的范围检查；
      “是否与 CubeMX 一致”只能在**配置期**检查 ——
      那件事由 tools/check_doc_config.py 做（它会读用户工程的 usart.c）。
      两者各司其职：这里防“写了个狗都不认的值”，
      脚本防“代码与硬件配置不一致”。 */
#if (GW_RS485_UART_BAUD != 9600u)   && (GW_RS485_UART_BAUD != 19200u) && \
    (GW_RS485_UART_BAUD != 38400u)  && (GW_RS485_UART_BAUD != 115200u)
  #warning "GW_RS485_UART_BAUD 不在 gw_baud 支持的 9600/19200/38400/115200 之列"
#endif

#endif /* GW_CONFIG_H */
