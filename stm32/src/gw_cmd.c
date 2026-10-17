/**
 * @file    gw_cmd.c
 * @brief   下行指令解释实现（纯逻辑，无副作用）
 */
#include "gw_cmd.h"

#include <string.h>

/* ==================================================================
 *  字符工具
 *
 *  ⚠️ 不用 <ctype.h> 的 toupper/isspace：
 *     它们对负值 char 的行为是未定义（在 ARM 上 char 默认无符号，
 *     在 x86 上默认有符号 —— 同一份代码在两个平台行为不同）。
 *     自己写小函数，行为完全确定，也不依赖 locale。
 * ================================================================== */

static char to_upper_ascii(char c)
{
    if (c >= 'a' && c <= 'z') {
        return (char)(c - 'a' + 'A');
    }
    return c;
}

static int is_space_ascii(char c)
{
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n') ? 1 : 0;
}

static int is_digit_ascii(char c)
{
    return (c >= '0' && c <= '9') ? 1 : 0;
}

/* 十六进制字符转值；非法返回 -1 */
static int hex_val(char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

/* ==================================================================
 *  词法：把文本切成 token
 *
 *  为什么要自己写而不是用 strtok：
 *    1) 输入**不必以 '\0' 结尾**（用 len 界定），strtok 要求可写字符串
 *    2) strtok 会修改输入，而我们是 const
 *    3) 自己写可以直接拿到"出错位置"，现场排查时有用
 * ================================================================== */
#define CMD_MAX_TOKENS  4u

typedef struct {
    /* （曾有一个 const char *p 字段，cppcheck 指出从未使用，已删除） */
    uint16_t    len;        /* 每个 token 的长度 */
    uint16_t    off;        /* 每个 token 在原文里的偏移 */
} tok_t;

typedef struct {
    tok_t    t[CMD_MAX_TOKENS];
    uint8_t  n;
    uint16_t stop_pos;      /* 出错/停止时的位置 */
} lexer_t;

/* 判断两个 token 是否等于给定字符串（大小写不敏感） */
static int tok_eq(const char *text, const tok_t *t, const char *lit)
{
    uint16_t i;
    uint16_t ll = (uint16_t)strlen(lit);

    if (t->len != ll) {
        return 0;
    }
    for (i = 0u; i < ll; i++) {
        if (to_upper_ascii(text[t->off + i]) != to_upper_ascii(lit[i])) {
            return 0;
        }
    }
    return 1;
}

/**
 * 解析无符号整数
 *
 * 支持 `0x` 前缀的十六进制（清理状态位时手打十六进制更方便）
 * 与前导 `+`。**不接受负数与溢出**。
 *
 * @return 1 成功；0 失败
 */
static int tok_to_u32(const char *text, const tok_t *t, uint32_t *out)
{
    uint32_t v = 0u;
    uint16_t i = 0u;
    int      base = 10;
    int      any = 0;

    if (t->len == 0u) {
        return 0;
    }

    /* 前导 '+'：允许，因为有些脚本会带符号 */
    if (text[t->off] == '+') {
        i = 1u;
    }

    if (i + 1u < t->len && text[t->off + i] == '0'
        && (text[t->off + i + 1u] == 'x' || text[t->off + i + 1u] == 'X')) {
        base = 16;
        i += 2u;
    }

    for (; i < t->len; i++) {
        int d;

        if (base == 16) {
            d = hex_val(text[t->off + i]);
        } else {
            d = is_digit_ascii(text[t->off + i])
                ? (text[t->off + i] - '0') : -1;
        }
        if (d < 0) {
            return 0;                   /* 非法字符 */
        }
        /* 溢出检查：先判断再乘，避免回绕 */
        if (v > (0xFFFFFFFFu - (uint32_t)d) / (uint32_t)base) {
            return 0;
        }
        v = v * (uint32_t)base + (uint32_t)d;
        any = 1;
    }

    if (any == 0) {
        return 0;                       /* 例如只有 "0x" */
    }
    *out = v;
    return 1;
}

/* 切词 */
static void lex(const char *text, uint16_t len, lexer_t *lx)
{
    uint16_t i = 0u;

    lx->n = 0u;
    lx->stop_pos = len;

    while (i < len) {
        /* 跳过空白 */
        while (i < len && is_space_ascii(text[i]) != 0) {
            i++;
        }
        if (i >= len) {
            break;
        }
        if (lx->n >= CMD_MAX_TOKENS) {
            lx->stop_pos = i;           /* 参数太多 */
            break;
        }

        lx->t[lx->n].off = i;
        while (i < len && is_space_ascii(text[i]) == 0) {
            i++;
        }
        lx->t[lx->n].len = (uint16_t)(i - lx->t[lx->n].off);
        lx->n++;
    }
}

/* ==================================================================
 *  各指令的解析
 * ================================================================== */

static gw_cmd_result_t parse_set_period(const char *text, const lexer_t *lx,
                                       gw_cmd_t *out)
{
    uint32_t v;

    if (lx->n < 2u) {
        out->err_pos = lx->stop_pos;
        return GW_CMD_ERR_SYNTAX;       /* 缺参数 */
    }
    if (tok_to_u32(text, &lx->t[1], &v) == 0) {
        out->err_pos = lx->t[1].off;
        return GW_CMD_ERR_SYNTAX;       /* 参数不是数字 */
    }
    if (v < GW_CMD_PERIOD_MIN || v > GW_CMD_PERIOD_MAX) {
        out->err_pos = lx->t[1].off;
        return GW_CMD_ERR_RANGE;
    }
    out->arg0 = v;
    return GW_CMD_OK;
}

static gw_cmd_result_t parse_clear_sticky(const char *text, const lexer_t *lx,
                                          gw_cmd_t *out)
{
    uint32_t v;

    if (lx->n < 2u) {
        out->err_pos = lx->stop_pos;
        return GW_CMD_ERR_SYNTAX;
    }
    if (tok_to_u32(text, &lx->t[1], &v) == 0) {
        out->err_pos = lx->t[1].off;
        return GW_CMD_ERR_SYNTAX;
    }
    out->arg0 = v;
    return GW_CMD_OK;
}

static gw_cmd_result_t parse_set_uplink(const char *text, const lexer_t *lx,
                                        gw_cmd_t *out)
{
    if (lx->n < 2u) {
        out->err_pos = lx->stop_pos;
        return GW_CMD_ERR_SYNTAX;
    }
    if (tok_eq(text, &lx->t[1], "on") != 0 || tok_eq(text, &lx->t[1], "1") != 0) {
        out->flag = 1u;
        return GW_CMD_OK;
    }
    if (tok_eq(text, &lx->t[1], "off") != 0 || tok_eq(text, &lx->t[1], "0") != 0) {
        out->flag = 0u;
        return GW_CMD_OK;
    }
    out->err_pos = lx->t[1].off;
    return GW_CMD_ERR_SYNTAX;
}

/* 危险指令：解析出令牌（令牌本身合法性由 gw_cmd_check_token 判） */
static gw_cmd_result_t parse_with_token(const char *text, const lexer_t *lx,
                                        gw_cmd_t *out)
{
    uint32_t v;

    if (lx->n < 2u) {
        out->err_pos = lx->stop_pos;
        /* ⚠️ 缺令牌返回 ERR_TOKEN 而不是 ERR_SYNTAX：
           这样上位机收到的提示是"需要确认令牌"，
           而不是笼统的"语法错误" —— 前者能直接指导操作员怎么做。 */
        return GW_CMD_ERR_TOKEN;
    }
    if (tok_to_u32(text, &lx->t[1], &v) == 0) {
        out->err_pos = lx->t[1].off;
        return GW_CMD_ERR_TOKEN;
    }
    out->token = v;
    return GW_CMD_OK;
}

/* ==================================================================
 *  对外接口
 * ================================================================== */

gw_cmd_result_t gw_cmd_parse(const char *text, uint16_t len, gw_cmd_t *out)
{
    lexer_t lx;

    if (out == 0) {
        return GW_CMD_ERR_SYNTAX;
    }

    memset(out, 0, sizeof(*out));
    out->id = GW_CMD_NONE;

    if (text == 0 || len == 0u) {
        return GW_CMD_OK;               /* 空消息：静默忽略 */
    }
    if (len > GW_CMD_MAX_LEN) {
        out->err_pos = GW_CMD_MAX_LEN;
        return GW_CMD_ERR_SYNTAX;       /* 超长：拒绝，不当成有效指令 */
    }

    lex(text, len, &lx);

    if (lx.n == 0u) {
        return GW_CMD_OK;               /* 全是空白：同空消息 */
    }

    /* ---- 指令名分派 ---- */
    if (tok_eq(text, &lx.t[0], "REPORT") != 0
        || tok_eq(text, &lx.t[0], "REPORT_NOW") != 0) {
        out->id = GW_CMD_REPORT_NOW;
        return GW_CMD_OK;
    }
    if (tok_eq(text, &lx.t[0], "PERIOD") != 0) {
        out->id = GW_CMD_SET_PERIOD;
        return parse_set_period(text, &lx, out);
    }
    if (tok_eq(text, &lx.t[0], "CLEAR_STICKY") != 0
        || tok_eq(text, &lx.t[0], "CLEAR") != 0) {
        out->id = GW_CMD_CLEAR_STICKY;
        return parse_clear_sticky(text, &lx, out);
    }
    if (tok_eq(text, &lx.t[0], "STATUS") != 0) {
        out->id = GW_CMD_GET_STATUS;
        return GW_CMD_OK;
    }
    if (tok_eq(text, &lx.t[0], "UPLINK") != 0) {
        out->id = GW_CMD_SET_UPLINK;
        return parse_set_uplink(text, &lx, out);
    }
    if (tok_eq(text, &lx.t[0], "SAVE") != 0) {
        out->id = GW_CMD_SAVE;
        return parse_with_token(text, &lx, out);
    }
    if (tok_eq(text, &lx.t[0], "RESTORE") != 0
        || tok_eq(text, &lx.t[0], "FACTORY") != 0) {
        out->id = GW_CMD_RESTORE;
        return parse_with_token(text, &lx, out);
    }
    if (tok_eq(text, &lx.t[0], "REBOOT") != 0
        || tok_eq(text, &lx.t[0], "RESET") != 0) {
        out->id = GW_CMD_REBOOT;
        return parse_with_token(text, &lx, out);
    }

    out->id = GW_CMD_UNKNOWN;
    out->err_pos = lx.t[0].off;
    return GW_CMD_ERR_UNKNOWN;
}

int gw_cmd_needs_token(gw_cmd_id_t id)
{
    switch (id) {
    case GW_CMD_SAVE:
    case GW_CMD_RESTORE:
    case GW_CMD_REBOOT:
        return 1;
    default:
        return 0;
    }
}

uint32_t gw_cmd_expected_token(gw_cmd_id_t id)
{
    switch (id) {
    case GW_CMD_SAVE:    return GW_CMD_TOKEN_SAVE;
    case GW_CMD_RESTORE: return GW_CMD_TOKEN_RESTORE;
    case GW_CMD_REBOOT:  return GW_CMD_TOKEN_REBOOT;
    default:             return 0u;
    }
}

gw_cmd_result_t gw_cmd_check_token(const gw_cmd_t *cmd)
{
    if (cmd == 0) {
        return GW_CMD_ERR_SYNTAX;
    }
    if (gw_cmd_needs_token(cmd->id) == 0) {
        return GW_CMD_OK;
    }
    if (cmd->token != gw_cmd_expected_token(cmd->id)) {
        return GW_CMD_ERR_TOKEN;
    }
    return GW_CMD_OK;
}

const char *gw_cmd_id_str(gw_cmd_id_t id)
{
    switch (id) {
    case GW_CMD_NONE:         return "NONE";
    case GW_CMD_REPORT_NOW:   return "REPORT";
    case GW_CMD_SET_PERIOD:   return "PERIOD";
    case GW_CMD_SAVE:         return "SAVE";
    case GW_CMD_RESTORE:      return "RESTORE";
    case GW_CMD_REBOOT:       return "REBOOT";
    case GW_CMD_CLEAR_STICKY: return "CLEAR_STICKY";
    case GW_CMD_GET_STATUS:   return "STATUS";
    case GW_CMD_SET_UPLINK:   return "UPLINK";
    case GW_CMD_UNKNOWN:      return "UNKNOWN";
    default:                  return "?";
    }
}

const char *gw_cmd_result_str(gw_cmd_result_t r)
{
    switch (r) {
    case GW_CMD_OK:           return "OK";
    case GW_CMD_ERR_SYNTAX:   return "SYNTAX_ERROR";
    case GW_CMD_ERR_RANGE:    return "RANGE_ERROR";
    case GW_CMD_ERR_UNKNOWN:  return "UNKNOWN_CMD";
    case GW_CMD_ERR_TOKEN:    return "BAD_TOKEN";
    case GW_CMD_ERR_STATE:    return "BAD_STATE";
    case GW_CMD_ERR_INTERNAL: return "INTERNAL_ERROR";
    default:                  return "?";
    }
}

uint16_t gw_cmd_usage(gw_cmd_id_t id, char *out, uint16_t cap)
{
    const char *s;
    uint16_t    n;

    switch (id) {
    case GW_CMD_REPORT_NOW:   s = "report"; break;
    case GW_CMD_SET_PERIOD:   s = "period <1..3600>"; break;
    case GW_CMD_SAVE:         s = "save 0x5A5A"; break;
    case GW_CMD_RESTORE:      s = "restore 0xA5A5"; break;
    case GW_CMD_REBOOT:       s = "reboot 0x1234"; break;
    case GW_CMD_CLEAR_STICKY: s = "clear_sticky <mask>"; break;
    case GW_CMD_GET_STATUS:   s = "status"; break;
    case GW_CMD_SET_UPLINK:   s = "uplink <on|off>"; break;
    default:
        s = "report|period|save|restore|reboot|clear_sticky|status|uplink";
        break;
    }

    if (out == 0 || cap == 0u) {
        return 0u;
    }

    n = (uint16_t)strlen(s);
    if (n >= cap) {
        n = (uint16_t)(cap - 1u);
    }
    memcpy(out, s, n);
    out[n] = '\0';
    return n;
}
