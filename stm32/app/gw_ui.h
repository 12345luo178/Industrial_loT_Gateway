/* ==================================================================
 *  本地 UI（4 个页面，手绘，不用 LVGL）
 *
 *  为什么不用 LVGL：见 2026-10-11 的决策记录 —— 本机 flash 余量约 85KB、
 *  RAM 余量约 27KB，而 LVGL + 驱动 + 中文字库 + 触摸约需 70-100KB/20-30KB，
 *  会顶穿预算；而顶穿的代价是动 OTA 双槽/golden 分区，那是这套固件最值钱的部分。
 *
 *  ⚠️ 与固件其余部分的**唯一接口**就是这个 vtable：
 *    · 只**读**寄存器表（get_iregs / get_hregs）—— 表由 HOUSE 周期刷新，
 *      所以屏幕上看到的就是主站看到的那一份数据；
 *    · 改参数只**写保持寄存器**（set_hreg）—— 与 Modbus 主站写的是同一份存储，
 *      因此不存在"屏幕一份、主站一份"的状态分裂。
 *  本模块**不**直接碰任何驱动内部状态，也不 include 业务模块的头。
 *
 *  ⚠️ 每次 tick 的工作量有界（内容重绘 200ms 一次、触摸每拍都读），
 *     所以放在最低优先级任务里也不会拖慢采集与上云链路。
 * ================================================================== */
#ifndef GW_UI_H
#define GW_UI_H

#include <stdint.h>

typedef struct {
    /** 取 40 个输入寄存器的当前快照（只读，指针在调用期间有效）*/
    const uint16_t *(*get_iregs)(void);
    /** 取 16 个保持寄存器的当前快照（只读）*/
    const uint16_t *(*get_hregs)(void);
    /** 写一个保持寄存器（UI 改参数的**唯一**出口）*/
    void            (*set_hreg)(uint16_t idx, uint16_t val);
} gw_ui_port_t;

/** 初始化（含 lcd_init / touch_init）。@return 0 成功；<0 屏幕没接上 */
int  gw_ui_init(const gw_ui_port_t *port);

/** 周期调用（建议 20~50ms）。内部自行限速：内容重绘 200ms 一次、
 *  触摸每拍都读。工作量有界，返回即代表本轮做完。 */
void gw_ui_tick(uint32_t dt_ms);

/** 强制下一次重绘（例如从别的状态回来）*/
void gw_ui_invalidate(void);

#endif /* GW_UI_H */
