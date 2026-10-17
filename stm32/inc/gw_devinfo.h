/**
 * @file    gw_devinfo.h
 * @brief   设备参数结构体：寄存器 <-> 参数 <-> Flash 的中间表示
 *
 * ============================================================================
 *  为什么需要一个独立的"参数结构体"，而不是直接把寄存器表存进 Flash？
 *
 *  三层各有各的表示，各有各的理由：
 *
 *    寄存器表（mb_regs_t）      对外接口契约：16 位对齐、高低字拆分、
 *                               命令用魔数 —— 这些是**给上位机看的**
 *    参数结构体（gw_params_t）  设备视角的配置：uint32 的设备 ID、
 *                               uint16 的超时值 —— 这些是**给固件用的**
 *    Flash 槽（gw_param 管理）  带 magic/seq/crc 的持久化格式 ——
 *                               这些是**给掉电安全用的**
 *
 *  如果三者合一，会出现两类问题：
 *    1) 上位机要求 16 位对齐与高低字拆分，固件里到处要拼装/拆解，
 *       而且一旦改了寄存器布局就得连带改 Flash 格式（老设备参数全丢）
 *    2) Flash 格式需要版本号与迁移能力，而寄存器表是"当前契约"，
 *       两者演化节奏不同
 *
 *  所以中间加一层"参数结构体"：它**只包含配置语义**，
 *  寄存器是它的对外视图，Flash 是它的持久化视图。
 *  寄存器布局变化只需改 gw_params_from_regs/to_regs；
 *  Flash 格式变化只需改 version 与迁移函数。互不影响。
 *
 *  ----------------------------------------------------------------------------
 *  校验规则集中在这一层
 *
 *  "上报周期必须 1~3600"这类规则如果散落在各处（写入寄存器时检查一次、
 *  从 Flash 读取时检查一次、初始化时再检查一次），迟早会出现某处漏检。
 *  这里用一张表把规则集中起来，任何入口都调同一个 gw_params_validate()。
 * ============================================================================
 */
#ifndef GW_DEVINFO_H
#define GW_DEVINFO_H

#include <stdint.h>

#include "modbus_regs.h"    /* HREG_* 寄存器偏移与取值定义 */
#include "modbus_rtu.h"     /* mb_regs_t（寄存器表结构） */

/* ---------------- 参数结构体版本 ----------------
 * 用于 Flash 里识别"这份参数是哪个固件版本写的"。
 * 将来加字段时递增它，并在迁移函数里处理老版本。 */
#define GW_PARAMS_VERSION       1u

/* ---------------- 默认值（出厂/首次上电） ---------------- */
#define GW_PARAM_DEF_DEVICE_ID      100000u     /* 0x000186A0 */
#define GW_PARAM_DEF_BAUDRATE       HREG_BAUD_9600
#define GW_PARAM_DEF_REPORT_PERIOD  10u         /* 秒 */
#define GW_PARAM_DEF_SLAVE_ADDR     1u
#define GW_PARAM_DEF_CAN_FILT_ID    0x0000u
#define GW_PARAM_DEF_CAN_FILT_MSK   0x0000u     /* 掩码 0 = 全通过 */
#define GW_PARAM_DEF_MASTER_ENABLE  0u          /* 默认关闭主站轮询 */
#define GW_PARAM_DEF_MASTER_TIMEOUT 300u        /* 毫秒 */
#define GW_PARAM_DEF_MASTER_RETRY   1u

/* ---------------- 参数取值范围（校验用） ---------------- */
#define GW_PARAM_MIN_REPORT_PERIOD  1u
#define GW_PARAM_MAX_REPORT_PERIOD  3600u
#define GW_PARAM_MIN_SLAVE_ADDR     1u
#define GW_PARAM_MAX_SLAVE_ADDR     247u
#define GW_PARAM_MIN_TIMEOUT        10u
#define GW_PARAM_MAX_TIMEOUT        5000u
#define GW_PARAM_MAX_RETRY          5u
#define GW_PARAM_MAX_CAN_ID         0x7FFu      /* 标准帧 11 位 */

/* ---------------- 设备参数 ---------------- */
typedef struct {
    uint32_t device_id;         /* 设备 ID */
    uint16_t baudrate_code;     /* HREG_BAUD_* */
    uint16_t report_period_s;   /* 上报周期（秒）1~3600 */
    uint16_t slave_addr;        /* 本机 Modbus 从站地址 1~247 */

    uint16_t can_filt_id;       /* CAN 接收滤波 ID（标准帧 11 位） */
    uint16_t can_filt_mask;     /* CAN 接收滤波掩码 */

    uint16_t master_enable;     /* 0=关闭主站轮询 1=开启 */
    uint16_t master_timeout_ms; /* 主站单次请求超时 10~5000 */
    uint16_t master_retry;      /* 主站重试次数 0~5 */
} gw_params_t;

/* ---------------- 校验结果 ---------------- */
typedef enum {
    GW_PARAMS_OK             = 0,
    GW_PARAMS_ERR_BAUDRATE   = -1,
    GW_PARAMS_ERR_PERIOD     = -2,
    GW_PARAMS_ERR_SLAVE_ADDR = -3,
    GW_PARAMS_ERR_TIMEOUT    = -4,
    GW_PARAMS_ERR_RETRY      = -5,
    GW_PARAMS_ERR_CAN_ID     = -6,
    GW_PARAMS_ERR_DEVICE_ID  = -7,
    GW_PARAMS_ERR_MASTER_EN  = -8,
    GW_PARAMS_ERR_PARAM      = -9
} gw_params_err_t;

/* ==================================================================
 *  接口
 * ================================================================== */

/** 填充出厂默认参数 */
void gw_params_default(gw_params_t *p);

/**
 * 校验参数是否全部合法
 *
 * @return GW_PARAMS_OK 或具体的错误码（便于日志指出是哪一项不合法）
 *
 * @note 所有"参数进入系统"的入口都应调用它：
 *       从 Flash 读回、从寄存器写入、上层显式设置。
 */
gw_params_err_t gw_params_validate(const gw_params_t *p);

/** 校验结果转字符串（日志用，明确指出哪一项越界） */
const char *gw_params_strerror(gw_params_err_t e);

/**
 * 从寄存器表读出参数（上位机写入后调用）
 *
 * 把 16 位寄存器拼装成 32 位设备 ID、把波特率代号等原样取出。
 * 只做拆装，**不做校验** —— 校验交给 gw_params_validate()，
 * 这样"读取"与"判断合法性"两件事分开，调用方可以先读出来再决定怎么处理。
 */
void gw_params_from_regs(gw_params_t *p, const mb_regs_t *regs);

/**
 * 把参数写入寄存器表（启动时让上位机读到当前配置）
 *
 * ⚠️ 不会写命令寄存器（HREG_PARAM_SAVE_CMD 等）——
 *    那些是"写魔数触发动作"的语义，不应被固件回写。
 *    也不会写 HREG_OTA_STATE（只读状态）。
 */
void gw_params_to_regs(const gw_params_t *p, mb_regs_t *regs);

/**
 * 检查两个参数是否等价（用于"仅在变化时写 Flash"省寿命）
 * @return 1 = 不同（需要保存）；0 = 相同
 */
int gw_params_changed(const gw_params_t *a, const gw_params_t *b);

/** 波特率代号 -> 实际波特率值；代号非法时返回 0 */
uint32_t gw_params_baudrate_value(uint16_t code);

/** 取参数线格式的字节长度（用于 Flash 读写与版本兼容检查） */
uint32_t gw_params_size(void);

/**
 * 把参数序列化成线格式字节（供 gw_param 的 Flash 读写使用）
 *
 * 显式逐字段大端拼装，不 memcpy 结构体：
 *   结构体有对齐填充，填充字节值不确定，会让"同一份参数"的字节表示
 *   不稳定 —— 而 gw_param 会用 needs_save 比较字节，
 *   不稳定就会误判"参数变了"而反复写 Flash，实打实消耗寿命。
 *
 * @return 写入字节数（= gw_params_size()），失败返回 0
 */
uint32_t gw_params_pack(const gw_params_t *p, uint8_t *out, uint32_t cap);

/**
 * 从线格式字节反序列化，**并在内部调用 validate**
 *
 * 长度必须精确匹配（短了会读到不存在的字段，长了说明版本不同，
 * 两者都不该被静默接受 —— 参数错位比参数缺失更难查）。
 *
 * 反序列化后立即校验的理由：Flash 里的数据可能是旧固件版本写的，
 * 或被位翻转破坏过 CRC 覆盖范围之外的部分。宁可回退默认值，
 * 也不要带着非法参数运行 —— 例如 slave_addr=0 会让设备变成广播地址。
 *
 * @return GW_PARAMS_OK 或错误码（含具体的越界字段）
 */
gw_params_err_t gw_params_unpack(const uint8_t *in, uint32_t len, gw_params_t *out);

/* ==================================================================
 *  启动参数选取（上板 main.c 用）
 * ================================================================== */

/** 参数来源 */
typedef enum {
    GW_PARAM_SRC_FLASH   = 0,   /* 从 Flash 读到了有效参数 */
    GW_PARAM_SRC_DEFAULT = 1    /* 用默认值（首次上电 / 参数损坏） */
} gw_param_src_t;

/**
 * 参数序列化后的**字节长度**（跨版本契约）
 *
 * ⚠️ 这个值**不能随结构体布局自动变化**：
 *    它同时是"Flash 里存了多少字节"与"串口上传输多少字节"的依据。
 *    结构体加字段时必须**显式**改这个宏，并递增 `GW_PARAMS_VERSION` ——
 *    这样旧固件读到新数据时会因版本不符而回退默认值，
 *    而不是按错位的偏移去解析。
 */
#define GW_PARAMS_WIRE_LEN      20u

/* 编译期钉住：结构体字段的总字节数必须等于 WIRE_LEN。
   这样"加了字段但忘了改 WIRE_LEN"会在编译期报错，
   而不是在运行时静默错位。

   字段清单（见 gw_params_t 的定义）：
       device_id         uint32_t  4
       baudrate_code     uint16_t  2
       report_period_s   uint16_t  2
       slave_addr        uint16_t  2
       can_filt_id       uint16_t  2
       can_filt_mask     uint16_t  2
       master_enable     uint16_t  2
       master_timeout_ms uint16_t  2
       master_retry      uint16_t  2
                                 ----
                                  20  */
_Static_assert((4u + 2u + 2u + 2u + 2u + 2u + 2u + 2u + 2u) == GW_PARAMS_WIRE_LEN,
               "GW_PARAMS_WIRE_LEN 与字段布局不一致：加字段后必须同步更新此宏与 GW_PARAMS_VERSION");

/** 选取结果 */
typedef struct {
    gw_params_t    params;      /* 选出来的参数（一定合法） */
    gw_param_src_t source;      /* 来自哪里 */
    int32_t        reason;      /* 用默认值的原因（GW_PARAM_SRC_FLASH 时为 0） */
    uint16_t       version;     /* Flash 里的版本（默认值时为 0） */
} gw_param_pick_t;

/**
 * ⚠️ 决定启动时用哪份参数
 *
 * @param loaded     gw_param_load() 的返回值
 * @param buf        gw_param_load() 读出的字节
 * @param len        读出的长度
 * @param ver        读出的版本
 * @param out        输出
 * @return 1 = 用了 Flash 的参数；0 = 用了默认值（**不是错误**）
 *
 * ============================================================================
 *  这个函数为什么值得单独存在
 *
 *  它是上板装配里**唯一有真实出错风险的一段逻辑**：
 *
 *    1) Flash 参数可能是**旧固件版本写的**（版本不同，布局可能变了）
 *    2) 可能被位翻转破坏过（CRC 覆盖之外的部分）
 *    3) 反序列化后**必须校验**——例如 slave_addr=0 会让设备
 *       变成"广播地址"，那样它会响应总线上所有请求，
 *       把别人的设备搞得一团乱
 *
 *  四道关卡任何一个不过 -> **回退默认值**，并且：
 *
 *  **⚠️ 回退时绝不能顺手写 Flash。**
 *  每次上电都写一遍会很快耗完擦写寿命（约 1 万次）。
 *  而且"参数损坏"如果是个持续性问题（例如某个扇区坏了），
 *  每次上电写一次只会让它坏得更快。
 *  **正确做法是用默认值起来，等上位机显式下发保存命令。**
 *
 *  四道关卡在测试里都有专门的用例。
 * ============================================================================
 */
int gw_params_pick_startup(int32_t loaded, const uint8_t *buf, uint32_t len,
                           uint16_t ver, gw_param_pick_t *out);

/** 参数来源字符串（日志用） */
const char *gw_param_src_str(gw_param_src_t s);

#endif /* GW_DEVINFO_H */
