/* ==================================================================
 *  host 侧 HAL 桩件（只给 PC 上的 UI 渲染测试用）
 *
 *  目的：让 app/gw_ui.c 能在 PC 上编译 —— 这样我们可以在**烧板之前**
 *  就把界面渲染成图看一眼，抓出编译查不到的布局/几何/残字问题。
 *
 *  ⚠️ 这里只提供 gw_ui.c 真正用到的那一点点东西：
 *     - types: GPIO_TypeDef / SPI_HandleTypeDef（gw_config.h 要用）
 *     - 宏:    GPIOA/GPIOB/GPIOG、GPIO_PIN_n
 *     - 别的（HAL_Delay 等）在 test_ui_render.c 里以函数形式提供
 *  真正的驱动（lcd_ili9341.c / touch_xpt2046.c）**不参与**本测试，
 *  它们的显示与触摸接口由测试自己实现（画进内存帧缓冲）。
 * ================================================================== */
#ifndef HOST_HAL_STUB_H
#define HOST_HAL_STUB_H

#include <stdint.h>

typedef struct { uint32_t dummy; } GPIO_TypeDef;
typedef struct { uint32_t dummy; } SPI_TypeDef;
typedef struct { uint32_t dummy; } SPI_HandleTypeDef;
typedef enum { HAL_OK = 0, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;

/* 只用得到这几个端口 */
extern GPIO_TypeDef *const GPIOA;
extern GPIO_TypeDef *const GPIOB;
extern GPIO_TypeDef *const GPIOG;

#define GPIO_PIN_0   ((uint16_t)0x0001)
#define GPIO_PIN_1   ((uint16_t)0x0002)
#define GPIO_PIN_2   ((uint16_t)0x0004)
#define GPIO_PIN_3   ((uint16_t)0x0008)
#define GPIO_PIN_4   ((uint16_t)0x0010)
#define GPIO_PIN_5   ((uint16_t)0x0020)
#define GPIO_PIN_6   ((uint16_t)0x0040)
#define GPIO_PIN_7   ((uint16_t)0x0080)
#define GPIO_PIN_8   ((uint16_t)0x0100)
#define GPIO_PIN_9   ((uint16_t)0x0200)
#define GPIO_PIN_10  ((uint16_t)0x0400)
#define GPIO_PIN_11  ((uint16_t)0x0800)
#define GPIO_PIN_12  ((uint16_t)0x1000)

/* gw_ui.c 里用到（真实实现由测试文件提供）*/
void HAL_Delay(uint32_t ms);

#endif /* HOST_HAL_STUB_H */
