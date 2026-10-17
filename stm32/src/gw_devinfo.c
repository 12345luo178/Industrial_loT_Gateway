/**
 * @file    gw_devinfo.c
 * @brief   设备参数结构体实现（纯逻辑，可在 PC 上单元测试）
 */
#include "gw_devinfo.h"

#include <string.h>

/* ------------------------------------------------------------------
 * 内部：显式序列化 / 反序列化
 *
 * 不直接 memcpy 结构体，理由与 gw_frame / gw_param 一致：
 *   结构体有对齐填充，填充字节值不确定，会让"同一份参数"的字节
 *   表示不稳定（导致 needs_save 误判"变了"而反复写 Flash）。
 *   显式按字节拼装后，同一份参数的字节表示是唯一确定的。
 * ------------------------------------------------------------------ */
#define GW_PARAMS_WIRE_LEN  20u     /* 见下方布局 */

/* 线格式布局（全部大端）：
 *   0  u32 device_id
 *   4  u16 baudrate_code
 *   6  u16 report_period_s
 *   8  u16 slave_addr
 *   10 u16 can_filt_id
 *   12 u16 can_filt_mask
 *   14 u16 master_enable
 *   16 u16 master_timeout_ms
 *   18 u16 master_retry
 */
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* 编译期检查线格式长度与字段总和对齐 */
_Static_assert(GW_PARAMS_WIRE_LEN == 20u, "线格式长度与字段布局不一致");

/* ==================================================================
 *  基础接口
 * ================================================================== */

void gw_params_default(gw_params_t *p)
{
    if (p == 0) {
        return;
    }
    p->device_id         = GW_PARAM_DEF_DEVICE_ID;
    p->baudrate_code     = GW_PARAM_DEF_BAUDRATE;
    p->report_period_s   = GW_PARAM_DEF_REPORT_PERIOD;
    p->slave_addr        = GW_PARAM_DEF_SLAVE_ADDR;
    p->can_filt_id       = GW_PARAM_DEF_CAN_FILT_ID;
    p->can_filt_mask     = GW_PARAM_DEF_CAN_FILT_MSK;
    p->master_enable     = GW_PARAM_DEF_MASTER_ENABLE;
    p->master_timeout_ms = GW_PARAM_DEF_MASTER_TIMEOUT;
    p->master_retry      = GW_PARAM_DEF_MASTER_RETRY;
}

gw_params_err_t gw_params_validate(const gw_params_t *p)
{
    if (p == 0) {
        return GW_PARAMS_ERR_PARAM;
    }

    /* 逐项校验，返回**具体**的错误码而不是笼统的"非法" ——
       现场要靠这个错误码判断是哪个字段写错了。 */

    if (p->device_id == 0u) {
        /* 设备 ID 为 0 会导致云端无法区分设备；同时 0 也是"未初始化"的标志 */
        return GW_PARAMS_ERR_DEVICE_ID;
    }

    if (p->baudrate_code > HREG_BAUD_115200) {
        return GW_PARAMS_ERR_BAUDRATE;
    }

    if (p->report_period_s < GW_PARAM_MIN_REPORT_PERIOD
        || p->report_period_s > GW_PARAM_MAX_REPORT_PERIOD) {
        return GW_PARAMS_ERR_PERIOD;
    }

    if (p->slave_addr < GW_PARAM_MIN_SLAVE_ADDR
        || p->slave_addr > GW_PARAM_MAX_SLAVE_ADDR) {
        /* 0 是广播地址，不能作为从站自己的地址 */
        return GW_PARAMS_ERR_SLAVE_ADDR;
    }

    if (p->can_filt_id > GW_PARAM_MAX_CAN_ID
        || p->can_filt_mask > GW_PARAM_MAX_CAN_ID) {
        return GW_PARAMS_ERR_CAN_ID;
    }

    if (p->master_enable > 1u) {
        return GW_PARAMS_ERR_MASTER_EN;
    }

    if (p->master_timeout_ms < GW_PARAM_MIN_TIMEOUT
        || p->master_timeout_ms > GW_PARAM_MAX_TIMEOUT) {
        return GW_PARAMS_ERR_TIMEOUT;
    }

    if (p->master_retry > GW_PARAM_MAX_RETRY) {
        return GW_PARAMS_ERR_RETRY;
    }

    return GW_PARAMS_OK;
}

const char *gw_params_strerror(gw_params_err_t e)
{
    switch (e) {
    case GW_PARAMS_OK:             return "OK";
    case GW_PARAMS_ERR_BAUDRATE:   return "baudrate code out of range";
    case GW_PARAMS_ERR_PERIOD:     return "report period out of range (1..3600)";
    case GW_PARAMS_ERR_SLAVE_ADDR: return "slave address out of range (1..247)";
    case GW_PARAMS_ERR_TIMEOUT:    return "master timeout out of range (10..5000)";
    case GW_PARAMS_ERR_RETRY:      return "master retry out of range (0..5)";
    case GW_PARAMS_ERR_CAN_ID:     return "CAN filter id/mask out of range (0..0x7FF)";
    case GW_PARAMS_ERR_DEVICE_ID:  return "device id must not be zero";
    case GW_PARAMS_ERR_MASTER_EN:  return "master enable must be 0 or 1";
    case GW_PARAMS_ERR_PARAM:      return "invalid parameter";
    default:                       return "unknown error";
    }
}

/* ==================================================================
 *  寄存器 <-> 参数
 * ================================================================== */

void gw_params_from_regs(gw_params_t *p, const mb_regs_t *regs)
{
    if (p == 0 || regs == 0) {
        return;
    }

    /* 32 位设备 ID：高字在前，与 modbus_regs.h 的约定一致 */
    p->device_id = ((uint32_t)regs->holding[HREG_DEVICE_ID_HI] << 16)
                 | (uint32_t)regs->holding[HREG_DEVICE_ID_LO];

    p->baudrate_code     = regs->holding[HREG_BAUDRATE];
    p->report_period_s   = regs->holding[HREG_REPORT_PERIOD];
    p->slave_addr        = regs->holding[HREG_SLAVE_ADDR];

    /* CAN 滤波：寄存器表里用高低字分开（为了给上位机 16 位对齐的接口），
       但实际只有 11 位有效。拼装后保留低位。 */
    p->can_filt_id   = regs->holding[HREG_CAN_FILT_ID_LO];
    p->can_filt_mask = regs->holding[HREG_CAN_FILT_MSK_LO];

    p->master_enable     = regs->holding[HREG_MASTER_ENABLE];
    p->master_timeout_ms = regs->holding[HREG_MASTER_TIMEOUT];
    p->master_retry      = regs->holding[HREG_MASTER_RETRY];
}

void gw_params_to_regs(const gw_params_t *p, mb_regs_t *regs)
{
    if (p == 0 || regs == 0) {
        return;
    }

    regs->holding[HREG_DEVICE_ID_HI] = (uint16_t)(p->device_id >> 16);
    regs->holding[HREG_DEVICE_ID_LO] = (uint16_t)(p->device_id & 0xFFFFu);

    regs->holding[HREG_BAUDRATE]      = p->baudrate_code;
    regs->holding[HREG_REPORT_PERIOD] = p->report_period_s;
    regs->holding[HREG_SLAVE_ADDR]    = p->slave_addr;

    regs->holding[HREG_CAN_FILT_ID_LO]  = p->can_filt_id;
    regs->holding[HREG_CAN_FILT_MSK_LO] = p->can_filt_mask;
    /* 高字保持 0：11 位 ID 用不到高字，但寄存器位置保留，
       这样上位机看到的布局始终是"高低字成对"，不会因为有效位少而变。 */
    regs->holding[HREG_CAN_FILT_ID_HI]  = 0u;
    regs->holding[HREG_CAN_FILT_MSK_HI] = 0u;

    regs->holding[HREG_MASTER_ENABLE]  = p->master_enable;
    regs->holding[HREG_MASTER_TIMEOUT] = p->master_timeout_ms;
    regs->holding[HREG_MASTER_RETRY]   = p->master_retry;

    /* 刻意不写：
       HREG_PARAM_SAVE_CMD / HREG_PARAM_DFLT_CMD / HREG_REBOOT_CMD
         —— 它们语义是"写魔数触发动作"，固件回写会造成意外的动作触发
       HREG_OTA_STATE
         —— 只读状态，由 OTA 模块维护 */
}

int gw_params_changed(const gw_params_t *a, const gw_params_t *b)
{
    if (a == 0 || b == 0) {
        return 0;
    }

    /* 逐字段比较，不用 memcmp —— 结构体的对齐填充字节值不确定，
       memcmp 可能因为填充差异而误判"变了"，导致无谓的 Flash 写入
       （而 Flash 擦写寿命只有 1 万次，这种误判会实打实地消耗寿命）。 */
    if (a->device_id         != b->device_id)         { return 1; }
    if (a->baudrate_code     != b->baudrate_code)     { return 1; }
    if (a->report_period_s   != b->report_period_s)   { return 1; }
    if (a->slave_addr        != b->slave_addr)        { return 1; }
    if (a->can_filt_id       != b->can_filt_id)       { return 1; }
    if (a->can_filt_mask     != b->can_filt_mask)     { return 1; }
    if (a->master_enable     != b->master_enable)     { return 1; }
    if (a->master_timeout_ms != b->master_timeout_ms) { return 1; }
    if (a->master_retry      != b->master_retry)      { return 1; }
    return 0;
}

uint32_t gw_params_baudrate_value(uint16_t code)
{
    switch (code) {
    case HREG_BAUD_9600:   return 9600u;
    case HREG_BAUD_19200:  return 19200u;
    case HREG_BAUD_38400:  return 38400u;
    case HREG_BAUD_115200: return 115200u;
    default:               return 0u;       /* 非法代号：调用方应先用 validate 拦住 */
    }
}

uint32_t gw_params_size(void)
{
    return GW_PARAMS_WIRE_LEN;
}

/* ==================================================================
 *  线格式序列化（供 gw_param 的 Flash 读写使用）
 *
 *  放在本文件而不是 gw_param 里：
 *    序列化格式属于"参数结构体"的知识，Flash 模块只该看到字节数组。
 *    这样将来参数加字段时改本文件，Flash 模块一行不用动。
 * ================================================================== */

uint32_t gw_params_pack(const gw_params_t *p, uint8_t *out, uint32_t cap)
{
    if (p == 0 || out == 0 || cap < GW_PARAMS_WIRE_LEN) {
        return 0u;
    }

    put_u32(&out[0],  p->device_id);
    put_u16(&out[4],  p->baudrate_code);
    put_u16(&out[6],  p->report_period_s);
    put_u16(&out[8],  p->slave_addr);
    put_u16(&out[10], p->can_filt_id);
    put_u16(&out[12], p->can_filt_mask);
    put_u16(&out[14], p->master_enable);
    put_u16(&out[16], p->master_timeout_ms);
    put_u16(&out[18], p->master_retry);

    return GW_PARAMS_WIRE_LEN;
}

gw_params_err_t gw_params_unpack(const uint8_t *in, uint32_t len, gw_params_t *out)
{
    if (in == 0 || out == 0) {
        return GW_PARAMS_ERR_PARAM;
    }
    /* 长度必须精确匹配：短了会读到不存在的字段，长了说明版本不同。
       两者都不该被静默接受 —— 参数错位比参数缺失更难查。 */
    if (len != GW_PARAMS_WIRE_LEN) {
        return GW_PARAMS_ERR_PARAM;
    }

    out->device_id         = get_u32(&in[0]);
    out->baudrate_code     = get_u16(&in[4]);
    out->report_period_s   = get_u16(&in[6]);
    out->slave_addr        = get_u16(&in[8]);
    out->can_filt_id       = get_u16(&in[10]);
    out->can_filt_mask     = get_u16(&in[12]);
    out->master_enable     = get_u16(&in[14]);
    out->master_timeout_ms = get_u16(&in[16]);
    out->master_retry      = get_u16(&in[18]);

    /* 反序列化后立即校验：Flash 里的数据可能是上一个固件版本写的，
       或者被位翻转破坏过 CRC 之外的部分。宁可回退默认值也不要带着
       非法参数运行 —— 例如 slave_addr=0 会让设备变成广播地址。 */
    return gw_params_validate(out);
}

/* ==================================================================
 *  启动参数选取
 * ================================================================== */

const char *gw_param_src_str(gw_param_src_t s)
{
    switch (s) {
    case GW_PARAM_SRC_FLASH:   return "FLASH";
    case GW_PARAM_SRC_DEFAULT: return "DEFAULT";
    default:                   return "?";
    }
}

int gw_params_pick_startup(int32_t loaded, const uint8_t *buf, uint32_t len,
                           uint16_t ver, gw_param_pick_t *out)
{
    gw_params_t p;

    if (out == 0) {
        return 0;
    }

    /* 默认输出：用默认值。
       下面任何一道关卡不过，就直接返回这个 ——
       **不要在中间状态上继续**。 */
    gw_params_default(&out->params);
    out->source  = GW_PARAM_SRC_DEFAULT;
    out->version = 0u;
    out->reason  = loaded;

    /* ---- 关卡 1：Flash 读取本身成功了吗 ---- */
    if (loaded != 0) {
        /* 首次上电时这是**正常情况**（GW_PARAM_ERR_NO_VALID），
           不是错误 —— 所以 reason 里如实记录，由上层决定日志等级。 */
        return 0;
    }

    /* ---- 关卡 2：缓冲区与长度合法性 ---- */
    if (buf == 0 || len == 0u) {
        out->reason = -100;         /* 入参问题（不是 Flash 的问题） */
        return 0;
    }

    /* ---- 关卡 3：反序列化 + 校验 ----
       这两步合在一起做的理由：unpack 内部已经做了范围校验，
       但它**不检查版本**（版本是调用方的职责，因为不同版本
       可能有不同的兼容策略）。 */
    if (gw_params_unpack(buf, len, &p) != GW_PARAMS_OK) {
        out->reason = -101;         /* 数据本身不合法 */
        return 0;
    }
    if (gw_params_validate(&p) != GW_PARAMS_OK) {
        out->reason = -102;         /* 校验不过（越界字段） */
        return 0;
    }

    /* ---- 关卡 4：版本匹配 ----
       ⚠️ 版本不同就**拒绝**，而不是"尽力解析"。
       理由：结构体布局变化时，逐个字段的偏移会变，
       "尽力解析"会把 A 字段的值当成 B 字段用 ——
       那种错误比"回退默认值"危险得多
       （例如把 device_id 的低位当成 slave_addr）。

       如果将来要做版本迁移，应该在这里**显式写迁移函数**，
       而不是放宽判断。 */
    if (ver != GW_PARAMS_VERSION) {
        out->reason = -103;
        return 0;
    }

    /* 四道关卡全过：用 Flash 里的参数 */
    out->params  = p;
    out->source  = GW_PARAM_SRC_FLASH;
    out->version = ver;
    out->reason  = 0;

    /* ⚠️ 注意本函数**不写 Flash**（无论走哪条路）。
       回退默认值时不写是有意的 —— 见头文件里的说明。 */
    return 1;
}
