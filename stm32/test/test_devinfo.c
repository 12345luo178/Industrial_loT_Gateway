/**
 * @file    test_devinfo.c
 * @brief   设备参数结构体单元测试（取值范围 / 寄存器转换 / 线格式）
 *
 * ============================================================================
 *  本模块是"配置数据的单一真相"，所以测试重点是契约而非算法：
 *
 *    - 每个字段的取值范围是否被真正拦住（边界值要逐项验证）
 *    - 寄存器 <-> 参数的双向转换是否严格互逆
 *    - 参数结构体比较是否会因为对齐填充而误判"变了"
 *      （误判会无谓地写 Flash，而擦写寿命只有 1 万次）
 *    - 线格式反序列化是否拒绝异常长度与非法内容
 *
 *  编译运行：
 *    gcc -std=c99 -Wall -Wextra -Iinc -o test/test_devinfo.exe \
 *        src/gw_devinfo.c test/test_devinfo.c
 *    ./test/test_devinfo.exe
 * ============================================================================
 */
#include <stdio.h>
#include <string.h>

#include "gw_devinfo.h"
#include "gw_param.h"   /* GW_PARAM_ERR_NO_VALID */

static int g_pass = 0;
static int g_fail = 0;

static void ok(const char *name)
{
    g_pass++;
    printf("  [PASS] %s\n", name);
}

static void fail(const char *name, const char *detail)
{
    g_fail++;
    printf("  [FAIL] %s  %s\n", name, detail);
}

static void check_int(const char *name, int actual, int expected)
{
    if (actual == expected) {
        ok(name);
    } else {
        char d[96];
        sprintf(d, "(实际 %d, 期望 %d)", actual, expected);
        fail(name, d);
    }
}

/* ==================================================================
 * 测试 1：默认参数合法
 * ================================================================== */
static void test_default(void)
{
    gw_params_t p;

    printf("\n[1] 出厂默认参数\n");

    memset(&p, 0xFF, sizeof(p));    /* 先污染，确认 default 真的全部赋了值 */
    gw_params_default(&p);

    if (gw_params_validate(&p) == GW_PARAMS_OK) {
        ok("默认参数通过校验（不会有'出厂就不合法'的尴尬）");
    } else {
        char d[96];
        sprintf(d, "(%s)", gw_params_strerror(gw_params_validate(&p)));
        fail("默认参数通过校验", d);
    }

    check_int("默认设备 ID", (int)(p.device_id == GW_PARAM_DEF_DEVICE_ID), 1);
    check_int("默认从站地址 = 1", (int)p.slave_addr, 1);
    check_int("默认上报周期 = 10 秒", (int)p.report_period_s, 10);
    check_int("默认主站关闭", (int)p.master_enable, 0);
    check_int("默认超时 = 300ms", (int)p.master_timeout_ms, 300);

    /* 空指针安全 */
    gw_params_default(0);
    ok("default 对空指针安全");

    if (gw_params_validate(0) == GW_PARAMS_ERR_PARAM) {
        ok("validate 对空指针返回 PARAM");
    } else {
        fail("validate 对空指针返回 PARAM", "");
    }
}

/* ==================================================================
 * 测试 2：取值范围逐项边界
 * ================================================================== */
static void test_validation(void)
{
    gw_params_t p;

    printf("\n[2] 取值范围边界（每项都测边界内/边界外）\n");

    /* ---- 设备 ID ---- */
    gw_params_default(&p);
    p.device_id = 0u;
    check_int("设备 ID = 0 被拒（云端无法区分设备）",
              (int)gw_params_validate(&p), (int)GW_PARAMS_ERR_DEVICE_ID);
    p.device_id = 1u;
    check_int("设备 ID = 1 被接受", (int)gw_params_validate(&p), 0);
    p.device_id = 0xFFFFFFFFu;
    check_int("设备 ID = 最大值被接受", (int)gw_params_validate(&p), 0);

    /* ---- 波特率代号 ---- */
    gw_params_default(&p);
    p.baudrate_code = 0u;
    check_int("波特率代号 0（9600）被接受", (int)gw_params_validate(&p), 0);
    p.baudrate_code = HREG_BAUD_115200;
    check_int("波特率代号 3（115200）被接受", (int)gw_params_validate(&p), 0);
    p.baudrate_code = 4u;
    check_int("波特率代号 4 被拒", (int)gw_params_validate(&p),
              (int)GW_PARAMS_ERR_BAUDRATE);

    /* ---- 上报周期 1~3600 ---- */
    gw_params_default(&p);
    p.report_period_s = 0u;
    check_int("上报周期 0 被拒", (int)gw_params_validate(&p),
              (int)GW_PARAMS_ERR_PERIOD);
    p.report_period_s = GW_PARAM_MIN_REPORT_PERIOD;
    check_int("上报周期 1（下界）被接受", (int)gw_params_validate(&p), 0);
    p.report_period_s = GW_PARAM_MAX_REPORT_PERIOD;
    check_int("上报周期 3600（上界）被接受", (int)gw_params_validate(&p), 0);
    p.report_period_s = 3601u;
    check_int("上报周期 3601 被拒", (int)gw_params_validate(&p),
              (int)GW_PARAMS_ERR_PERIOD);

    /* ---- 从站地址 1~247 ---- */
    gw_params_default(&p);
    p.slave_addr = 0u;
    check_int("从站地址 0 被拒（0 是广播地址）",
              (int)gw_params_validate(&p), (int)GW_PARAMS_ERR_SLAVE_ADDR);
    p.slave_addr = 1u;
    check_int("从站地址 1（下界）被接受", (int)gw_params_validate(&p), 0);
    p.slave_addr = 247u;
    check_int("从站地址 247（上界）被接受", (int)gw_params_validate(&p), 0);
    p.slave_addr = 248u;
    check_int("从站地址 248 被拒", (int)gw_params_validate(&p),
              (int)GW_PARAMS_ERR_SLAVE_ADDR);

    /* ---- CAN 滤波 0~0x7FF（标准帧 11 位） ---- */
    gw_params_default(&p);
    p.can_filt_id = 0x7FFu;
    p.can_filt_mask = 0x7FFu;
    check_int("CAN 滤波 0x7FF（上界）被接受", (int)gw_params_validate(&p), 0);
    p.can_filt_id = 0x800u;
    check_int("CAN 滤波 ID 0x800 被拒（超出 11 位）",
              (int)gw_params_validate(&p), (int)GW_PARAMS_ERR_CAN_ID);
    p.can_filt_id = 0x100u;
    p.can_filt_mask = 0x800u;
    check_int("CAN 滤波掩码 0x800 被拒",
              (int)gw_params_validate(&p), (int)GW_PARAMS_ERR_CAN_ID);

    /* ---- 主站使能 0/1 ---- */
    gw_params_default(&p);
    p.master_enable = 1u;
    check_int("主站使能 = 1 被接受", (int)gw_params_validate(&p), 0);
    p.master_enable = 2u;
    check_int("主站使能 = 2 被拒", (int)gw_params_validate(&p),
              (int)GW_PARAMS_ERR_MASTER_EN);

    /* ---- 主站超时 10~5000 ---- */
    gw_params_default(&p);
    p.master_timeout_ms = 9u;
    check_int("超时 9ms 被拒", (int)gw_params_validate(&p),
              (int)GW_PARAMS_ERR_TIMEOUT);
    p.master_timeout_ms = GW_PARAM_MIN_TIMEOUT;
    check_int("超时 10ms（下界）被接受", (int)gw_params_validate(&p), 0);
    p.master_timeout_ms = GW_PARAM_MAX_TIMEOUT;
    check_int("超时 5000ms（上界）被接受", (int)gw_params_validate(&p), 0);
    p.master_timeout_ms = 5001u;
    check_int("超时 5001ms 被拒", (int)gw_params_validate(&p),
              (int)GW_PARAMS_ERR_TIMEOUT);

    /* ---- 主站重试 0~5 ---- */
    gw_params_default(&p);
    p.master_retry = 0u;
    check_int("重试 0 被接受", (int)gw_params_validate(&p), 0);
    p.master_retry = GW_PARAM_MAX_RETRY;
    check_int("重试 5（上界）被接受", (int)gw_params_validate(&p), 0);
    p.master_retry = 6u;
    check_int("重试 6 被拒", (int)gw_params_validate(&p),
              (int)GW_PARAMS_ERR_RETRY);

    /* 错误码字符串应能指出具体哪一项 */
    if (strstr(gw_params_strerror(GW_PARAMS_ERR_PERIOD), "period") != 0 &&
        strstr(gw_params_strerror(GW_PARAMS_ERR_SLAVE_ADDR), "slave") != 0 &&
        gw_params_strerror((gw_params_err_t)-999) != 0) {
        ok("错误码字符串能指出具体字段且有兜底");
    } else {
        fail("错误码字符串能指出具体字段", "");
    }
}

/* ==================================================================
 * 测试 3：寄存器 <-> 参数 双向转换
 * ================================================================== */
static void test_regs_roundtrip(void)
{
    gw_params_t src, dst;
    mb_regs_t   regs;

    printf("\n[3] 寄存器 <-> 参数 双向转换\n");

    /* 准备一份"各项都不是默认值"的参数，避免默认值掩盖转换错误 */
    src.device_id         = 0x000186A0u;    /* 100000 */
    src.baudrate_code     = HREG_BAUD_19200;
    src.report_period_s   = 30u;
    src.slave_addr        = 17u;
    src.can_filt_id       = 0x123u;
    src.can_filt_mask     = 0x7F0u;
    src.master_enable     = 1u;
    src.master_timeout_ms = 500u;
    src.master_retry      = 3u;

    memset(&regs, 0, sizeof(regs));
    gw_params_to_regs(&src, &regs);

    /* 逐项检查寄存器内容（大端拆分是否正确） */
    check_int("设备 ID 高字 = 0x0001", (int)regs.holding[HREG_DEVICE_ID_HI], 0x0001);
    check_int("设备 ID 低字 = 0x86A0", (int)regs.holding[HREG_DEVICE_ID_LO], 0x86A0);
    check_int("波特率代号写入正确", (int)regs.holding[HREG_BAUDRATE], HREG_BAUD_19200);
    check_int("上报周期写入正确", (int)regs.holding[HREG_REPORT_PERIOD], 30);
    check_int("从站地址写入正确", (int)regs.holding[HREG_SLAVE_ADDR], 17);
    check_int("CAN 滤波 ID 写在低字", (int)regs.holding[HREG_CAN_FILT_ID_LO], 0x123);
    check_int("CAN 滤波 ID 高字保持 0", (int)regs.holding[HREG_CAN_FILT_ID_HI], 0);
    check_int("CAN 掩码写在低字", (int)regs.holding[HREG_CAN_FILT_MSK_LO], 0x7F0);
    check_int("主站使能写入正确", (int)regs.holding[HREG_MASTER_ENABLE], 1);
    check_int("主站超时写入正确", (int)regs.holding[HREG_MASTER_TIMEOUT], 500);
    check_int("主站重试写入正确", (int)regs.holding[HREG_MASTER_RETRY], 3);

    /* 关键：固件不应回写命令寄存器与只读状态。
       回写命令魔数会造成意外的动作触发（例如重启）。 */
    check_int("命令寄存器未被回写（落盘）",
              (int)regs.holding[HREG_PARAM_SAVE_CMD], 0);
    check_int("命令寄存器未被回写（恢复出厂）",
              (int)regs.holding[HREG_PARAM_DFLT_CMD], 0);
    check_int("命令寄存器未被回写（重启）",
              (int)regs.holding[HREG_REBOOT_CMD], 0);
    check_int("OTA 状态未被回写（只读）",
              (int)regs.holding[HREG_OTA_STATE], 0);

    /* 反向转换应完全还原 */
    memset(&dst, 0, sizeof(dst));
    gw_params_from_regs(&dst, &regs);

    if (gw_params_changed(&src, &dst) == 0) {
        ok("参数 -> 寄存器 -> 参数 完全还原（双向转换互逆）");
    } else {
        fail("参数 -> 寄存器 -> 参数 完全还原", "");
    }

    /* 再走一遍完整往返：参数 -> 寄存器 -> 参数 -> 寄存器，两次寄存器应一致 */
    {
        mb_regs_t regs2;
        memset(&regs2, 0, sizeof(regs2));
        gw_params_to_regs(&dst, &regs2);
        if (memcmp(&regs, &regs2, sizeof(regs)) == 0) {
            ok("二次往返后寄存器内容逐字节一致");
        } else {
            fail("二次往返后寄存器内容逐字节一致", "");
        }
    }

    /* 空指针安全 */
    gw_params_to_regs(&src, 0);
    gw_params_from_regs(&dst, 0);
    gw_params_to_regs(0, &regs);
    gw_params_from_regs(0, &regs);
    ok("寄存器转换对空指针安全（不崩溃）");
}

/* ==================================================================
 * 测试 4：参数比较（省 Flash 寿命的关键）
 * ================================================================== */
static void test_changed(void)
{
    gw_params_t a, b;

    printf("\n[4] 参数比较（决定是否写 Flash）\n");

    gw_params_default(&a);
    gw_params_default(&b);

    check_int("完全相同的参数 -> 未变化", gw_params_changed(&a, &b), 0);

    /* 逐个字段改，都应被检出 */
    {
        gw_params_t t;
        int i;
        for (i = 0; i < 9; i++) {
            t = a;
            switch (i) {
            case 0: t.device_id = a.device_id + 1u; break;
            case 1: t.baudrate_code = 1u; break;
            case 2: t.report_period_s = a.report_period_s + 1u; break;
            case 3: t.slave_addr = a.slave_addr + 1u; break;
            case 4: t.can_filt_id = 0x100u; break;
            case 5: t.can_filt_mask = 0x100u; break;
            case 6: t.master_enable = 1u; break;
            case 7: t.master_timeout_ms = a.master_timeout_ms + 1u; break;
            case 8: t.master_retry = a.master_retry + 1u; break;
            default: break;
            }
            if (gw_params_changed(&a, &t) != 1) {
                char d[64];
                sprintf(d, "(字段 %d 的变化未被检出)", i);
                fail("每个字段变化都被检出", d);
                return;
            }
        }
        ok("9 个字段的任一变化都被检出");
    }

    /* 关键：结构体对齐填充不应影响比较结果。
       做法：把两份参数的内存先填成不同内容，再赋同样的值 ——
       若实现用 memcmp 就会误判"变了"，而误判会导致无谓的 Flash 写入。 */
    {
        gw_params_t x, y;
        memset(&x, 0xAA, sizeof(x));
        memset(&y, 0x55, sizeof(y));
        gw_params_default(&x);
        gw_params_default(&y);
        check_int("对齐填充不同但字段相同时判为未变化（不误写 Flash）",
                  gw_params_changed(&x, &y), 0);
    }

    if (gw_params_changed(0, &b) == 0 && gw_params_changed(&a, 0) == 0) {
        ok("changed 对空指针安全");
    } else {
        fail("changed 对空指针安全", "");
    }
}

/* ==================================================================
 * 测试 5：线格式序列化
 * ================================================================== */
static void test_wire_format(void)
{
    gw_params_t src, dst;
    uint8_t     buf[32];

    printf("\n[5] 线格式序列化（供 Flash 存储）\n");

    gw_params_default(&src);
    src.device_id   = 0x000186A0u;
    src.slave_addr  = 200u;
    src.master_retry = 5u;

    check_int("线格式长度 = 20 字节", (int)gw_params_size(), 20);
    check_int("pack 返回长度 = 20",
              (int)gw_params_pack(&src, buf, sizeof(buf)), 20);

    /* 逐字节验证大端序（这是持久化格式，字节序必须钉死） */
    check_int("设备 ID 首字节（大端）", (int)buf[0], 0x00);
    check_int("设备 ID 末字节", (int)buf[3], 0xA0);
    check_int("从站地址高字节", (int)buf[8], 0x00);
    check_int("从站地址低字节", (int)buf[9], 200);
    check_int("主站重试低字节", (int)buf[19], 5);

    /* 反序列化应完全还原 */
    memset(&dst, 0, sizeof(dst));
    if (gw_params_unpack(buf, 20u, &dst) == GW_PARAMS_OK) {
        ok("反序列化成功");
    } else {
        fail("反序列化成功", "");
        return;
    }
    check_int("往返后参数完全一致", gw_params_changed(&src, &dst) == 0 ? 1 : 0, 1);

    /* 同一份参数的字节表示必须稳定（否则 needs_save 会误判） */
    {
        uint8_t buf2[32];
        (void)gw_params_pack(&src, buf2, sizeof(buf2));
        check_int("同一份参数打包两次字节完全一致（表示稳定）",
                  memcmp(buf, buf2, 20u) == 0 ? 1 : 0, 1);
    }

    /* 长度不匹配必须被拒 —— 短了会读到不存在的字段，长了说明版本不同 */
    check_int("长度 19 被拒", (int)gw_params_unpack(buf, 19u, &dst),
              (int)GW_PARAMS_ERR_PARAM);
    check_int("长度 21 被拒", (int)gw_params_unpack(buf, 21u, &dst),
              (int)GW_PARAMS_ERR_PARAM);
    check_int("长度 0 被拒", (int)gw_params_unpack(buf, 0u, &dst),
              (int)GW_PARAMS_ERR_PARAM);

    /* 内容非法必须被拒（unpack 内部会 validate） */
    {
        uint8_t bad[20];
        memcpy(bad, buf, 20u);
        bad[8] = 0u;                /* 从站地址 = 0（广播地址，非法） */
        bad[9] = 0u;
        check_int("反序列化出的非法从站地址被拒",
                  (int)gw_params_unpack(bad, 20u, &dst),
                  (int)GW_PARAMS_ERR_SLAVE_ADDR);
    }
    {
        uint8_t bad[20];
        memcpy(bad, buf, 20u);
        bad[0] = bad[1] = bad[2] = bad[3] = 0u;     /* 设备 ID = 0 */
        check_int("反序列化出的设备 ID = 0 被拒",
                  (int)gw_params_unpack(bad, 20u, &dst),
                  (int)GW_PARAMS_ERR_DEVICE_ID);
    }

    /* 入参保护 */
    check_int("pack 空指针返回 0", (int)gw_params_pack(0, buf, sizeof(buf)), 0);
    check_int("pack 空输出返回 0", (int)gw_params_pack(&src, 0, 32u), 0);
    check_int("pack 容量不足返回 0", (int)gw_params_pack(&src, buf, 19u), 0);
    check_int("unpack 空指针返回 PARAM",
              (int)gw_params_unpack(0, 20u, &dst), (int)GW_PARAMS_ERR_PARAM);
    check_int("unpack 空输出返回 PARAM",
              (int)gw_params_unpack(buf, 20u, 0), (int)GW_PARAMS_ERR_PARAM);
}

/* ==================================================================
 * 测试 6：波特率代号映射
 * ================================================================== */
static void test_baudrate(void)
{
    printf("\n[6] 波特率代号映射\n");

    check_int("代号 0 -> 9600", (int)gw_params_baudrate_value(HREG_BAUD_9600), 9600);
    check_int("代号 1 -> 19200", (int)gw_params_baudrate_value(HREG_BAUD_19200), 19200);
    check_int("代号 2 -> 38400", (int)gw_params_baudrate_value(HREG_BAUD_38400), 38400);
    check_int("代号 3 -> 115200", (int)gw_params_baudrate_value(HREG_BAUD_115200), 115200);
    check_int("非法代号返回 0（调用方应先 validate 拦住）",
              (int)gw_params_baudrate_value(9u), 0);
}


/* ==================================================================
 * 测试 7：启动参数选取（上板装配里唯一有真实出错风险的一段）
 *
 *  四道关卡必须逐一验证，尤其是**回退默认值时不写 Flash** 这一条。
 * ================================================================== */
static void test_pick_startup(void)
{
    gw_params_t    p;
    gw_param_pick_t pick;
    uint8_t        buf[GW_PARAMS_WIRE_LEN];
    uint32_t       n;

    printf("\n[7] 启动参数选取\n");

    /* ---- 正常路径：Flash 里有有效参数 ---- */
    gw_params_default(&p);
    p.device_id       = 123456u;
    p.report_period_s = 30u;
    p.slave_addr      = 7u;
    n = gw_params_pack(&p, buf, sizeof(buf));
    check_int("（前置）打包成功", (int)(n > 0u ? 1 : 0), 1);

    check_int("有效参数 -> 用 Flash 的",
              gw_params_pick_startup(0, buf, n, GW_PARAMS_VERSION, &pick), 1);
    check_int("来源标为 FLASH",
              (int)pick.source, (int)GW_PARAM_SRC_FLASH);
    check_int("设备 ID 正确", (int)pick.params.device_id, 123456);
    check_int("周期正确", (int)pick.params.report_period_s, 30);
    check_int("从站地址正确", (int)pick.params.slave_addr, 7);
    check_int("无回退原因", (int)pick.reason, 0);
    ok("正常路径：Flash 参数被采用");

    /* ---- 关卡 1：Flash 读取失败（首次上电） ---- */
    check_int("首次上电（NO_VALID）-> 用默认值",
              gw_params_pick_startup((int)GW_PARAM_ERR_NO_VALID,
                                     buf, n, GW_PARAMS_VERSION, &pick), 0);
    check_int("来源标为 DEFAULT",
              (int)pick.source, (int)GW_PARAM_SRC_DEFAULT);
    check_int("默认值的设备 ID 是 100000",
              (int)pick.params.device_id, 100000);
    check_int("原因如实记录（NO_VALID）",
              (int)pick.reason, (int)GW_PARAM_ERR_NO_VALID);
    ok("首次上电是**正常情况**，不是错误 —— reason 里如实记录由上层决定日志等级");

    check_int("Flash 读失败（ERR_FLASH）-> 用默认值",
              gw_params_pick_startup((int)GW_PARAM_ERR_FLASH,
                                     buf, n, GW_PARAMS_VERSION, &pick), 0);
    check_int("原因如实记录（ERR_FLASH）",
              (int)pick.reason, (int)GW_PARAM_ERR_FLASH);

    /* ---- 关卡 2：缓冲区/长度非法 ---- */
    check_int("空缓冲 -> 用默认值",
              gw_params_pick_startup(0, 0, n, GW_PARAMS_VERSION, &pick), 0);
    check_int("长度为 0 -> 用默认值",
              gw_params_pick_startup(0, buf, 0u, GW_PARAMS_VERSION, &pick), 0);

    /* ---- 关卡 3：数据不合法 ---- */
    {
        uint8_t bad[GW_PARAMS_WIRE_LEN];

        memcpy(bad, buf, n);
        /* 把从站地址改成 0 —— 那是广播地址，绝对非法。
           如果放它过去，设备会响应总线上所有请求。 */
        bad[8] = 0u;
        bad[9] = 0u;
        check_int("从站地址=0（广播）-> 用默认值",
                  gw_params_pick_startup(0, bad, n, GW_PARAMS_VERSION, &pick), 0);
        check_int("回退原因是'数据不合法'", (int)(pick.reason < 0 ? 1 : 0), 1);
        ok("非法参数必须回退 —— slave_addr=0 会让设备变广播地址，把总线搞乱");
    }

    /* 长度不对（旧版本写的、或者被截断） */
    check_int("长度短了 -> 用默认值",
              gw_params_pick_startup(0, buf, n - 1u, GW_PARAMS_VERSION, &pick), 0);
    check_int("长度长了 -> 用默认值",
              gw_params_pick_startup(0, buf, GW_PARAMS_WIRE_LEN + 4u,
                                     GW_PARAMS_VERSION, &pick), 0);
    ok("长度必须精确匹配 —— 参数错位比参数缺失更难查");

    /* ---- 关卡 4：版本不同 ---- */
    check_int("版本不同 -> 用默认值（不做'尽力解析'）",
              gw_params_pick_startup(0, buf, n,
                                     (uint16_t)(GW_PARAMS_VERSION + 1u),
                                     &pick), 0);
    check_int("版本不符的原因被记录",
              (int)(pick.reason == -103 ? 1 : 0), 1);
    ok("版本不同就拒绝 —— '尽力解析'会把 A 字段当 B 字段用，比回退危险得多");

    /* ---- 边界：空输出指针 ---- */
    check_int("空输出指针返回 0",
              gw_params_pick_startup(0, buf, n, GW_PARAMS_VERSION, 0), 0);

    /* ---- 字符串 ---- */
    check_int("FLASH 字符串正确",
              strcmp(gw_param_src_str(GW_PARAM_SRC_FLASH), "FLASH") == 0 ? 1 : 0, 1);
    check_int("DEFAULT 字符串正确",
              strcmp(gw_param_src_str(GW_PARAM_SRC_DEFAULT), "DEFAULT") == 0 ? 1 : 0, 1);
    check_int("越界来源有兜底",
              gw_param_src_str((gw_param_src_t)99) != 0 ? 1 : 0, 1);
}

/* ==================================================================
 * 测试 8：⚠️ 回退默认值时**不写 Flash**
 *
 *  这一条单独测，因为它的后果很严重：
 *  每次上电都写一遍会很快耗完擦写寿命（约 1 万次）。
 * ================================================================== */
static void test_pick_no_flash_write(void)
{
    gw_param_pick_t pick;
    gw_params_t     before;
    gw_params_t     after;

    printf("\n[8] ⚠️ 回退默认值时不写 Flash（保护擦写寿命）\n");

    /* 连续调用 1000 次"回退"路径，验证输出结构体是纯粹的计算，
       不涉及任何持久化副作用（本函数签名里没有 Flash 接口，
       所以这一条是**结构性保证** —— 测试用来把这个约束固化下来）。 */
    {
        int i;
        int all_default = 1;

        for (i = 0; i < 1000; i++) {
            (void)gw_params_pick_startup((int)GW_PARAM_ERR_NO_VALID,
                                         0, 0u, 0u, &pick);
            if (pick.source != GW_PARAM_SRC_DEFAULT) {
                all_default = 0;
                break;
            }
        }
        check_int("反复回退 1000 次都得到默认值（无状态残留）",
                  all_default, 1);
        ok("本函数**没有 Flash 接口参数** —— 结构上就不可能写 Flash");
    }

    /* 默认值必须是稳定的（不随时间/调用次数变化） */
    gw_params_default(&before);
    (void)gw_params_pick_startup((int)GW_PARAM_ERR_NO_VALID, 0, 0u, 0u, &pick);
    after = pick.params;
    check_int("回退得到的默认值与 gw_params_default 完全一致",
              (int)(gw_params_changed(&before, &after) == 0 ? 1 : 0), 1);
    ok("回退路径与默认值定义只有一份 —— 不会出现两处默认值不一致");
}
int main(void)
{
    printf("========== 设备参数结构体单元测试 ==========\n");

    test_default();
    test_validation();
    test_regs_roundtrip();
    test_changed();
    test_wire_format();
    test_baudrate();
    test_pick_startup();
    test_pick_no_flash_write();

    printf("\n======================================================\n");
    printf("  通过 %d 项, 失败 %d 项", g_pass, g_fail);
    printf(g_fail == 0 ? "  --> ALL PASS\n" : "  --> 存在失败项\n");
    printf("======================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
