/**
 * @file    esp32_json.c
 * @brief   ESP32 侧 JSON 封装与解析实现（纯 C，零依赖）
 */
#include "esp32_json.h"

#include "gw_frame.h"       /* gw_telemetry_t / gw_telemetry_unpack() */

#include <string.h>

/* ==================================================================
 *  内部：固定缓冲拼接器
 *
 *  用一个"游标 + 剩余容量"的小结构，而不是到处传 (buf, cap, &n)：
 *  后者在插入一个字段时要改三个地方，很容易漏改一处导致越界。
 * ================================================================== */
typedef struct {
    char    *buf;
    uint16_t cap;
    uint16_t n;
    uint8_t  overflow;
} jw_t;

static void jw_init(jw_t *w, char *buf, uint16_t cap)
{
    w->buf      = buf;
    w->cap      = cap;
    w->n        = 0u;
    w->overflow = (buf == 0 || cap == 0u) ? 1u : 0u;
}

/** 追加原始字节（不转义）。缓冲不足时置 overflow 并停止写入。 */
static void jw_raw(jw_t *w, const char *s, uint16_t len)
{
    if (w->overflow != 0u) {
        return;
    }
    /* ⚠️ 留 1 字节给结尾的 '\0'。不预留的话最后一次写入会把
       '\0' 挤掉，调用方读到的是"没有终止符的字符串"——
       而那在串口/网络发送时会导致越界读。 */
    if ((uint32_t)w->n + (uint32_t)len + 1u > (uint32_t)w->cap) {
        w->overflow = 1u;
        return;
    }
    memcpy(w->buf + w->n, s, len);
    w->n = (uint16_t)(w->n + len);
}

/** 追加 C 字符串 */
static void jw_str(jw_t *w, const char *s)
{
    if (s == 0) {
        return;
    }
    jw_raw(w, s, (uint16_t)strlen(s));
}

/** 追加无符号十进制 */
static void jw_u32(jw_t *w, uint32_t v)
{
    char     tmp[11];
    uint8_t  i = 0u;
    uint8_t  j;

    /* 特殊处理 0：do-while 会正确产生 "0"，
       但如果写成 while(v) 就会产生空串。 */
    do {
        tmp[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u && i < (uint8_t)sizeof(tmp));

    for (j = i; j > 0u; j--) {
        jw_raw(w, &tmp[j - 1u], 1u);
    }
}

/** 追加有符号十进制 */
static void jw_i32(jw_t *w, int32_t v)
{
    if (v < 0) {
        jw_raw(w, "-", 1u);
        /* ⚠️ 不能写 `-v`：INT32_MIN 取负会溢出（UB）。
           用 uint32_t 做无符号取负是定义良好的。 */
        jw_u32(w, (uint32_t)(-(int64_t)v));
    } else {
        jw_u32(w, (uint32_t)v);
    }
}

/** 追加字段名的引号与冒号：`"name":` */
static void jw_key(jw_t *w, const char *name)
{
    jw_raw(w, "\"", 1u);
    jw_str(w, name);
    jw_raw(w, "\":", 2u);
}

/** 取结果字符串；溢出时调用方应丢弃 */
static uint16_t jw_done(jw_t *w)
{
    if (w->overflow != 0u || w->buf == 0) {
        if (w->buf != 0 && w->cap > 0u) {
            w->buf[0] = '\0';       /* 失败时也给一个合法空串 */
        }
        return 0u;
    }
    w->buf[w->n] = '\0';
    return w->n;
}

/* ==================================================================
 *  ×10 定点格式化
 * ================================================================== */

uint16_t esp32_json_fixed1(int32_t v, int is_signed, char *out, uint16_t cap)
{
    uint32_t mag;
    uint32_t ip;            /* 整数部分 */
    uint32_t fp;            /* 小数部分（一位） */
    uint8_t  digits[12];
    uint8_t  nd = 0u;
    uint8_t  i;
    uint16_t n = 0u;
    int      neg = 0;

    /* 最短输出是 "0.0" = 3 字符 + '\0' = 4。
       最长是 "-214748364.8" = 13 字符 + '\0' = 14。
       所以 cap < 4 时肯定放不下。 */
    if (out == 0 || cap < 4u) {
        return 0u;
    }

    /* ⚠️ 取绝对值要在 uint32 域做：
       `-v` 在 v == INT32_MIN 时溢出（有符号溢出是未定义行为）。 */
    if (is_signed != 0 && v < 0) {
        neg = 1;
        mag = (uint32_t)(-(int64_t)v);
    } else {
        mag = (uint32_t)v;
    }

    ip = mag / 10u;
    fp = mag % 10u;

    /* 整数部分转十进制（逆序，稍后反转）。
       ip 最大 214748364（9 位），digits[12] 够。 */
    do {
        digits[nd++] = (uint8_t)('0' + (ip % 10u));
        ip /= 10u;
    } while (ip != 0u && nd < (uint8_t)sizeof(digits));

    /* 预估总长：负号 + 整数位 + '.' + 1 位小数。
       ⚠️ 一次算清楚再写，而不是每次写入前都查一遍边界 ——
          后者会让"这个判断是不是恒真"变成读代码的人的负担
          （cppcheck 也为此报了两条互相矛盾的告警）。 */
    {
        uint16_t need = (uint16_t)((neg != 0 ? 1u : 0u) + nd + 2u);

        if (need >= cap) {
            return 0u;      /* 放不下：返回 0，不写半截 */
        }

        if (neg != 0) {
            out[n++] = '-';
        }
        for (i = nd; i > 0u; i--) {
            out[n++] = (char)digits[i - 1u];
        }
        out[n++] = '.';
        out[n++] = (char)('0' + fp);
    }

    out[n] = '\0';
    return n;
}

/* ==================================================================
 *  转义
 * ================================================================== */

uint16_t esp32_json_escape(const char *in, uint16_t in_len,
                           char *out, uint16_t cap)
{
    uint16_t i;
    uint16_t n = 0u;
    static const char hex[] = "0123456789abcdef";

    if (in == 0 || out == 0 || cap == 0u) {
        return 0u;
    }

    for (i = 0u; i < in_len; i++) {
        unsigned char c = (unsigned char)in[i];

        if (c == (unsigned char)'"') {
            if (n + 2u >= cap) { out[0] = '\0'; return 0u; }
            out[n++] = '\\'; out[n++] = '"';
        } else if (c == (unsigned char)'\\') {
            if (n + 2u >= cap) { out[0] = '\0'; return 0u; }
            out[n++] = '\\'; out[n++] = '\\';
        } else if (c == (unsigned char)'\n') {
            if (n + 2u >= cap) { out[0] = '\0'; return 0u; }
            out[n++] = '\\'; out[n++] = 'n';
        } else if (c == (unsigned char)'\r') {
            if (n + 2u >= cap) { out[0] = '\0'; return 0u; }
            out[n++] = '\\'; out[n++] = 'r';
        } else if (c == (unsigned char)'\t') {
            if (n + 2u >= cap) { out[0] = '\0'; return 0u; }
            out[n++] = '\\'; out[n++] = 't';
        } else if (c < 0x20u) {
            /* 其余控制字符用 \u00XX。
               JSON 规范不允许字符串里出现裸的控制字符 ——
               某些解析器（包括 cJSON 的严格模式）会直接拒绝整条消息。 */
            if (n + 6u >= cap) { out[0] = '\0'; return 0u; }
            out[n++] = '\\'; out[n++] = 'u'; out[n++] = '0'; out[n++] = '0';
            out[n++] = hex[(c >> 4) & 0x0Fu];
            out[n++] = hex[c & 0x0Fu];
        } else {
            if (n + 1u >= cap) { out[0] = '\0'; return 0u; }
            out[n++] = (char)c;
        }
    }
    out[n] = '\0';
    return n;
}

/* ==================================================================
 *  遥测 JSON
 * ================================================================== */

uint16_t esp32_json_telemetry(const uint8_t *payload, uint16_t len,
                              uint32_t ts_fix, char *out, uint16_t cap)
{
    gw_telemetry_t t;
    jw_t  w;
    char  num[16];

    /* ⚠️ 复用 STM32 侧的 gw_telemetry_unpack()，而不是在这里
       另写一份大端解析 —— 那份实现已经在 test_frame.c 里测过，
       而且"两边各写一份解析"迟早会不一致。 */
    if (gw_telemetry_unpack(payload, len, &t) != 0) {
        if (out != 0 && cap > 0u) {
            out[0] = '\0';
        }
        return 0u;
    }

    /* 时间戳：ESP32 有 NTP，STM32 没有 RTC。
       用 ESP32 的时间覆盖 STM32 的"上电秒数"，云端才能做时序分析。
       传 0 表示不覆盖（测试用）。 */
    if (ts_fix != 0u) {
        t.ts = ts_fix;
    }

    jw_init(&w, out, cap);

    jw_raw(&w, "{", 1u);

    jw_key(&w, "dev_id");  jw_u32(&w, t.dev_id);
    jw_raw(&w, ",", 1u);
    jw_key(&w, "ts");      jw_u32(&w, t.ts);
    jw_raw(&w, ",", 1u);
    jw_key(&w, "seq");     jw_u32(&w, t.seq);
    jw_raw(&w, ",", 1u);

    /* 温度：有符号，×10 */
    jw_key(&w, "temp");
    (void)esp32_json_fixed1((int32_t)t.temp_x10, 1, num, (uint16_t)sizeof(num));
    jw_str(&w, num);
    jw_raw(&w, ",", 1u);

    /* 湿度：无符号，×10 */
    jw_key(&w, "humi");
    (void)esp32_json_fixed1((int32_t)t.humi_x10, 0, num, (uint16_t)sizeof(num));
    jw_str(&w, num);
    jw_raw(&w, ",", 1u);

    /* CAN 通道：4 个 16 位无符号 */
    jw_key(&w, "can");
    jw_raw(&w, "[", 1u);
    {
        uint8_t i;
        for (i = 0u; i < 4u; i++) {
            if (i != 0u) {
                jw_raw(&w, ",", 1u);
            }
            jw_u32(&w, (uint32_t)t.can[i]);
        }
    }
    jw_raw(&w, "],", 2u);

    jw_key(&w, "status");  jw_u32(&w, (uint32_t)t.status);
    jw_raw(&w, ",", 1u);
    jw_key(&w, "uptime");  jw_u32(&w, (uint32_t)t.uptime_s);

    jw_raw(&w, "}", 1u);

    return jw_done(&w);
}

/* ==================================================================
 *  链路状态 JSON
 * ================================================================== */

uint16_t esp32_json_link_status(uint32_t dev_id,
                                esp32_link_state_t wifi,
                                esp32_link_state_t mqtt,
                                int16_t rssi_dbm,
                                uint16_t backlog,
                                char *out, uint16_t cap)
{
    jw_t w;

    jw_init(&w, out, cap);

    jw_raw(&w, "{", 1u);
    jw_key(&w, "dev_id");  jw_u32(&w, dev_id);
    jw_raw(&w, ",", 1u);
    jw_key(&w, "wifi");    jw_u32(&w, (uint32_t)wifi);
    jw_raw(&w, ",", 1u);
    jw_key(&w, "mqtt");    jw_u32(&w, (uint32_t)mqtt);
    jw_raw(&w, ",", 1u);

    /* RSSI 是有符号的（-30 ~ -90 dBm）。
       ⚠️ 用无符号输出会得到 65506 这种值，云端看不出信号好坏。 */
    jw_key(&w, "rssi");    jw_i32(&w, (int32_t)rssi_dbm);
    jw_raw(&w, ",", 1u);

    jw_key(&w, "backlog"); jw_u32(&w, (uint32_t)backlog);
    jw_raw(&w, "}", 1u);

    return jw_done(&w);
}

/* ==================================================================
 *  指令解析
 *
 *  手写的极小 JSON 扫描器。只做本项目需要的事：
 *  找 "cmd" 与 "id" 两个键的字符串/数字值。
 *
 *  ⚠️ 为什么不用完整 JSON 解析器：
 *    完整的解析要处理嵌套对象、数组、转义、Unicode、
 *    数字的指数形式……代码量是这个的 10 倍以上，
 *    而本项目用不到其中任何一项。
 *    手写版只要覆盖"云端可能发来的形状"即可，
 *    并且**有针对畸形输入的测试**保证不会越界。
 * ================================================================== */

/** 跳过空白 */
static uint16_t skip_ws(const char *s, uint16_t i, uint16_t len)
{
    while (i < len) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            i++;
        } else {
            break;
        }
    }
    return i;
}

/**
 * 在字符串里找 `"key"`，返回其后（冒号之后）的位置
 *
 * ⚠️ 这里有个真实陷阱：**不能简单地 strstr 找 "cmd"** ——
 *    一个值里可能恰好包含 `cmd` 这几个字母，例如
 *    `{"note":"use cmd here","cmd":"REPORT_NOW"}`。
 *    朴素搜索会命中前面那个 "cmd" 的位置，
 *    然后从那里往后找冒号，解析出错误的字段。
 *
 *    所以必须**按 JSON 的词法扫描**：跳过字符串值、跳过嵌套结构，
 *    只在"键的位置"上比较。
 */
static int find_key(const char *s, uint16_t len, const char *key,
                    uint16_t *out_pos)
{
    uint16_t i = 0u;
    uint16_t klen = (uint16_t)strlen(key);

    while (i < len) {
        char c = s[i];

        if (c == '"') {
            /* 读一个字符串（可能带转义） */
            uint16_t start = (uint16_t)(i + 1u);
            uint16_t j = start;
            uint16_t slen;

            while (j < len) {
                if (s[j] == '\\') {
                    j = (uint16_t)(j + 2u);     /* 跳过被转义的字符 */
                    continue;
                }
                if (s[j] == '"') {
                    break;
                }
                j++;
            }
            if (j >= len) {
                return -1;                      /* 未闭合的字符串 */
            }
            slen = (uint16_t)(j - start);

            /* 字符串结束后看下一个非空白字符是不是 ':'。
               是 -> 这个字符串是**键**，可以比较了。 */
            {
                uint16_t k = skip_ws(s, (uint16_t)(j + 1u), len);
                if (k < len && s[k] == ':') {
                    if (slen == klen && memcmp(s + start, key, klen) == 0) {
                        *out_pos = (uint16_t)(k + 1u);
                        return 0;
                    }
                }
            }
            i = (uint16_t)(j + 1u);
            continue;
        }

        i++;
    }
    return -1;
}

/** 解析一个 JSON 字符串值（处理转义），写入 out */
static int parse_string(const char *s, uint16_t len, uint16_t pos,
                        char *out, uint16_t cap)
{
    uint16_t n = 0u;

    pos = skip_ws(s, pos, len);
    if (pos >= len || s[pos] != '"') {
        return -1;
    }
    pos++;

    while (pos < len && s[pos] != '"') {
        char c = s[pos];

        if (c == '\\') {
            pos++;
            if (pos >= len) {
                return -1;
            }
            switch (s[pos]) {
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            case 't':  c = '\t'; break;
            case 'b':  c = '\b'; break;
            case 'f':  c = '\f'; break;
            case '"':  c = '"';  break;
            case '\\': c = '\\'; break;
            case '/':  c = '/';  break;
            case 'u':
                /* \uXXXX：本项目不做 Unicode 解码（指令是 ASCII 命令词）。
                   ⚠️ 但要**消费掉**这 4 个十六进制位，否则
                      `\u0041` 会被当成 'u' '0' '0' '4' '1' 五个字符。 */
                if (pos + 4u >= len) {
                    return -1;
                }
                pos += 4u;
                c = '?';        /* 用一个可见的替代字符，不静默丢弃 */
                break;
            default:
                return -1;      /* 非法转义：拒绝整条，不猜测 */
            }
        }

        if (n + 1u >= cap) {
            /* ⚠️ 超出缓冲必须**报错**而不是截断。
               截断会让 `SET_PERIOD 3000` 变成 `SET_PERIOD 3` ——
               一个静默生效的错误配置。 */
            return -2;
        }
        out[n++] = c;
        pos++;
    }

    if (pos >= len) {
        return -1;              /* 未闭合 */
    }
    out[n] = '\0';
    return 0;
}

/** 解析一个 JSON 无符号整数值 */
static int parse_u32(const char *s, uint16_t len, uint16_t pos, uint32_t *out)
{
    uint32_t v = 0u;
    uint8_t  digits = 0u;

    pos = skip_ws(s, pos, len);
    while (pos < len && s[pos] >= '0' && s[pos] <= '9') {
        v = v * 10u + (uint32_t)(s[pos] - '0');
        digits++;
        pos++;
        if (digits > 10u) {
            return -1;          /* 溢出保护 */
        }
    }
    if (digits == 0u) {
        return -1;
    }
    *out = v;
    return 0;
}

int esp32_json_parse_command(const char *json, uint16_t len,
                             esp32_cmd_t *out)
{
    uint16_t pos;
    int      r;

    if (json == 0 || out == 0 || len == 0u) {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    /* ---- cmd（必需） ---- */
    if (find_key(json, len, "cmd", &pos) != 0) {
        return -2;              /* 没有 cmd 字段 */
    }
    r = parse_string(json, len, pos, out->cmd, (uint16_t)sizeof(out->cmd));
    if (r != 0) {
        return (r == -2) ? -4 : -3;     /* -4 = 指令过长 */
    }
    if (out->cmd[0] == '\0') {
        return -5;              /* 空指令：明确拒绝，不静默忽略 */
    }

    /* ---- id（可选） ---- */
    if (find_key(json, len, "id", &pos) == 0) {
        if (parse_u32(json, len, pos, &out->req_id) == 0) {
            out->has_req_id = 1u;
        }
        /* id 解析失败不算错误：它是可选的辅助字段，
           没有它只是回执无法关联，指令本身仍可执行。 */
    }

    return 0;
}

/* ==================================================================
 *  指令回执 JSON
 * ================================================================== */

uint16_t esp32_json_cmd_reply(uint32_t req_id, int has_req_id,
                              const char *ack_text, int ok,
                              char *out, uint16_t cap)
{
    jw_t     w;
    char     esc[160];
    uint16_t elen;

    if (ack_text == 0) {
        ack_text = "";
    }

    /* ⚠️ 先转义再嵌入。当前 STM32 只会生成安全字符，
       但依赖"对端只会发安全字符"是脆弱的假设。 */
    elen = esp32_json_escape(ack_text, (uint16_t)strlen(ack_text),
                             esc, (uint16_t)sizeof(esc));
    if (elen == 0u && ack_text[0] != '\0') {
        return 0u;              /* 转义后放不下（不该发生，防御） */
    }

    jw_init(&w, out, cap);

    jw_raw(&w, "{", 1u);
    if (has_req_id != 0) {
        jw_key(&w, "id");   jw_u32(&w, req_id);
        jw_raw(&w, ",", 1u);
    }
    jw_key(&w, "ok");       jw_u32(&w, (ok != 0) ? 1u : 0u);
    jw_raw(&w, ",", 1u);
    jw_key(&w, "result");   jw_raw(&w, "\"", 1u);
    jw_raw(&w, esc, elen);
    jw_raw(&w, "\"", 1u);
    jw_raw(&w, "}", 1u);

    return jw_done(&w);
}

/* ==================================================================
 *  ★ OneNET 物模型（OneJSON）
 *
 *  这一节的代码**只负责拼/解报文**，不碰 topic、不碰 MQTT ——
 *  所以它能在 PC 上零依赖跑单元测试，和上面几节一样。
 *
 *  ⚠️ 一个刻意的取舍：本节所有 JSON 字面量都用 `jw_str()`（内部算 strlen），
 *     而不是像上面那样写 `jw_raw(w, "...", 13u)` 手动数长度。
 *     手动数字符在字段多的时候非常容易错一位，而错了之后
 *     表现是"报文少一个引号"—— 平台报"格式错误"，却不说是哪里。
 *     这几个字面量都很短，strlen 的开销可以忽略。
 * ================================================================== */

/** 追加无符号 64 位十进制（时间戳是毫秒，超 32 位） */
static void jw_u64(jw_t *w, uint64_t v)
{
    char    tmp[21];
    uint8_t i = 0u;
    uint8_t j;

    do {
        tmp[i++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v != 0u && i < (uint8_t)sizeof(tmp));

    for (j = i; j > 0u; j--) {
        jw_raw(w, &tmp[j - 1u], 1u);
    }
}

/** 写一个物模型属性：`"name":{"value":<v>[,"time":<ts>]}` */
static void jw_prop_u32(jw_t *w, const char *name, uint32_t v,
                        uint64_t ts_ms, int with_time)
{
    jw_key(w, name);
    jw_str(w, "{\"value\":");
    jw_u32(w, v);
    if (with_time != 0) {
        jw_str(w, ",\"time\":");
        jw_u64(w, ts_ms);
    }
    jw_str(w, "}");
}

/** 有符号整数版本（RSSI 是负的） */
static void jw_prop_i32(jw_t *w, const char *name, int32_t v,
                        uint64_t ts_ms, int with_time)
{
    jw_key(w, name);
    jw_str(w, "{\"value\":");
    jw_i32(w, v);
    if (with_time != 0) {
        jw_str(w, ",\"time\":");
        jw_u64(w, ts_ms);
    }
    jw_str(w, "}");
}

/** ×10 定点版本（温度/湿度要输出 23.5 而不是 235） */
static void jw_prop_fixed1(jw_t *w, const char *name, int32_t v,
                           int is_signed, uint64_t ts_ms, int with_time)
{
    char num[16];

    if (esp32_json_fixed1(v, is_signed, num, (uint16_t)sizeof(num)) == 0u) {
        /* 格式化失败（缓冲不够）——不该发生，但宁可写 null 也不要写半截。
           写半截会产出非法 JSON，平台整条丢弃，而现象是"数据没上来"。 */
        num[0] = '0'; num[1] = '\0';
    }

    jw_key(w, name);
    jw_str(w, "{\"value\":");
    jw_str(w, num);
    if (with_time != 0) {
        jw_str(w, ",\"time\":");
        jw_u64(w, ts_ms);
    }
    jw_str(w, "}");
}

/** 字符串属性（已转义的片段） */
static void jw_prop_esc(jw_t *w, const char *name, const char *esc,
                        uint16_t elen, uint64_t ts_ms, int with_time)
{
    jw_key(w, name);
    jw_str(w, "{\"value\":\"");
    jw_raw(w, esc, elen);
    jw_str(w, "\"");
    if (with_time != 0) {
        jw_str(w, ",\"time\":");
        jw_u64(w, ts_ms);
    }
    jw_str(w, "}");
}

/** 写 OneJSON 报文头：`{"id":"<id>","version":"1.0","params":{` */
static void jw_onenet_head(jw_t *w, uint32_t msg_id)
{
    jw_str(w, "{\"id\":\"");
    jw_u32(w, msg_id);
    jw_str(w, "\",\"version\":\"");
    jw_str(w, ESP32_ONENET_VERSION);
    jw_str(w, "\",\"params\":{");
}

/** 4 个 CAN 通道的标识符表（物模型没有数组类型，只能拆成 4 个属性）
 *
 *  以前是手写拼 `can1`..`can4`（避免 snprintf 把 stdio 拖进固件）。
 *  改成引用 ESP32_ONENET_ID_CAN* 宏后，手写拼接就没必要了 ——
 *  一张静态指针表同样不需要 stdio，而且**只有一处**定义。 */
static const char *const k_onenet_can_ids[4] = {
    ESP32_ONENET_ID_CAN1,
    ESP32_ONENET_ID_CAN2,
    ESP32_ONENET_ID_CAN3,
    ESP32_ONENET_ID_CAN4,
};

uint16_t esp32_json_onenet_property(const uint8_t *payload, uint16_t len,
                                    uint32_t msg_id, uint64_t ts_ms,
                                    char *out, uint16_t cap)
{
    gw_telemetry_t t;
    jw_t  w;
    int   with_time = (ts_ms != 0u) ? 1 : 0;

    /* 与通用路径一样复用 gw_telemetry_unpack()，不另写一份大端解析 */
    if (gw_telemetry_unpack(payload, len, &t) != 0) {
        if (out != 0 && cap > 0u) {
            out[0] = '\0';
        }
        return 0u;
    }

    jw_init(&w, out, cap);
    jw_onenet_head(&w, msg_id);

    /* ⚠️ 标识符一律走 esp32_json.h 里的 ESP32_ONENET_ID_* 宏，
       不在这里写字符串字面量 —— 那些是**云端契约**，
       与 STM32↔ESP32 内部线格式的字段名是两回事。
       拼错一个字母平台回 code=2306（"identifier not exist"），
       而且一次只报一个，靠试错要来回十几轮。
       实测见 doc/OneNET接入说明.md §8.7。
       tools/check_onenet_ids.py 会把宏与探针/物模型三处比对。 */
    jw_prop_fixed1(&w, ESP32_ONENET_ID_TEMP,
                   (int32_t)t.temp_x10, 1, ts_ms, with_time);
    jw_str(&w, ",");
    jw_prop_fixed1(&w, ESP32_ONENET_ID_HUMI,
                   (int32_t)t.humi_x10, 0, ts_ms, with_time);
    jw_str(&w, ",");
    jw_prop_u32(&w, ESP32_ONENET_ID_SEQ, t.seq, ts_ms, with_time);
    jw_str(&w, ",");
    jw_prop_u32(&w, ESP32_ONENET_ID_STATUS, (uint32_t)t.status, ts_ms, with_time);
    jw_str(&w, ",");
    jw_prop_u32(&w, ESP32_ONENET_ID_UPTIME, (uint32_t)t.uptime_s, ts_ms, with_time);

    {
        uint8_t i;
        for (i = 0u; i < 4u; i++) {
            jw_str(&w, ",");
            jw_prop_u32(&w, k_onenet_can_ids[i], (uint32_t)t.can[i],
                        ts_ms, with_time);
        }
    }

    jw_str(&w, "}}");
    return jw_done(&w);
}

uint16_t esp32_json_onenet_link(uint32_t msg_id, uint64_t ts_ms,
                                esp32_link_state_t wifi,
                                esp32_link_state_t mqtt,
                                int16_t rssi_dbm, int has_rssi,
                                uint16_t backlog,
                                char *out, uint16_t cap)
{
    jw_t w;
    int  with_time = (ts_ms != 0u) ? 1 : 0;

    jw_init(&w, out, cap);
    jw_onenet_head(&w, msg_id);

    jw_prop_u32(&w, ESP32_ONENET_ID_WIFI, (uint32_t)wifi, ts_ms, with_time);
    jw_str(&w, ",");
    jw_prop_u32(&w, ESP32_ONENET_ID_MQTT, (uint32_t)mqtt, ts_ms, with_time);
    jw_str(&w, ",");

    /* ⚠️ 取不到 AP 信息时**不上报** rssi，而不是上报 0。
       0 dBm 表示"贴着路由器"，是个很强的信号 ——
       上报 0 会让运维以为信号极好，而真相是"读不到"。 */
    if (has_rssi != 0) {
        jw_prop_i32(&w, ESP32_ONENET_ID_RSSI, (int32_t)rssi_dbm,
                    ts_ms, with_time);
        jw_str(&w, ",");
    }
    jw_prop_u32(&w, ESP32_ONENET_ID_BACKLOG, (uint32_t)backlog,
                ts_ms, with_time);

    jw_str(&w, "}}");
    return jw_done(&w);
}

int esp32_json_onenet_parse_set(const char *json, uint16_t len,
                                esp32_cmd_t *out,
                                char *out_id, uint16_t id_cap)
{
    uint16_t pos;
    int      r;

    if (json == 0 || out == 0 || len == 0u) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    if (out_id != 0 && id_cap > 0u) {
        out_id[0] = '\0';
    }

    /* ---- id（平台给的，**字符串**）----
       ⚠️ 先取 id：即使后面发现没有 cmd 字段，调用方也要能用这个 id
          回一条 code!=200 的 set_reply。不回的话平台会一直等超时，
          而设备这边看起来"收到了但什么都没做"。 */
    if (out_id != 0 && id_cap > 0u
        && find_key(json, len, "id", &pos) == 0) {
        if (parse_string(json, len, pos, out_id, id_cap) != 0) {
            out_id[0] = '\0';
        }
    }

    /* ---- cmd（指令文本，位于 params 里）----
       find_key 是按 JSON 词法扫的（跳过字符串值、只在键的位置比较），
       所以它天然能穿到 params 内部，不会误命中某个值里的 "cmd"。
       注意这里查的是**物模型属性** cmd（ESP32_ONENET_ID_CMD），
       与上面那个报文信封字段 "id" 不是一回事。 */
    if (find_key(json, len, ESP32_ONENET_ID_CMD, &pos) != 0) {
        return -2;              /* 没有 cmd 属性（可能是别的属性设置） */
    }
    r = parse_string(json, len, pos, out->cmd, (uint16_t)sizeof(out->cmd));
    if (r != 0) {
        return (r == -2) ? -4 : -3;     /* -4 = 指令过长 */
    }
    if (out->cmd[0] == '\0') {
        return -5;              /* 空指令：明确拒绝，不静默忽略 */
    }

    /* OneNET 的 id 是字符串，没有数字 req_id 的概念。
       保持 has_req_id = 0，回执关联走 out_id。 */
    return 0;
}

uint16_t esp32_json_onenet_set_reply(const char *msg_id, int code,
                                     const char *msg,
                                     char *out, uint16_t cap)
{
    jw_t        w;
    char        id_esc[24];
    char        msg_esc[160];
    const char *id_in;
    uint16_t    id_len;
    uint16_t    msg_len;

    if (msg == 0) {
        msg = "";
    }

    /* ⚠️ 空串与 NULL 都要兜底成 "0"。
       只判 NULL 是不够的 —— 调用方在"解析失败"时传进来的
       往往是 `out_id`（一个已初始化的空串），而不是 NULL。
       漏了这一条会拼出 `{"id":"",...}`，而平台**不接受空 id**，
       表现为"设备回了应答但平台还是超时"。

       id 是从平台报文里解析出来的，虽然理论上只含数字，
       但"理论上"不是"一定" —— 转义一遍成本可以忽略。 */
    id_in  = (msg_id != 0 && msg_id[0] != '\0') ? msg_id : "0";
    id_len = esp32_json_escape(id_in, (uint16_t)strlen(id_in),
                               id_esc, (uint16_t)sizeof(id_esc));
    if (id_len == 0u && id_in[0] != '\0') {
        return 0u;              /* id 长得离谱：宁可失败也不拼坏 JSON */
    }

    msg_len = esp32_json_escape(msg, (uint16_t)strlen(msg),
                                msg_esc, (uint16_t)sizeof(msg_esc));
    if (msg_len == 0u && msg[0] != '\0') {
        return 0u;
    }

    jw_init(&w, out, cap);
    jw_str(&w, "{\"id\":\"");
    jw_raw(&w, id_esc, id_len);
    jw_str(&w, "\",\"code\":");
    jw_i32(&w, code);
    jw_str(&w, ",\"msg\":\"");
    jw_raw(&w, msg_esc, msg_len);
    jw_str(&w, "\"}");

    return jw_done(&w);
}

uint16_t esp32_json_onenet_result(uint32_t msg_id, uint64_t ts_ms,
                                  const char *result, int ok,
                                  char *out, uint16_t cap)
{
    jw_t     w;
    char     esc[160];
    char     text[176];
    uint16_t elen;
    uint16_t tlen = 0u;

    if (result == 0) {
        result = "";
    }

    elen = esp32_json_escape(result, (uint16_t)strlen(result),
                             esc, (uint16_t)sizeof(esc));
    if (elen == 0u && result[0] != '\0') {
        return 0u;
    }

    /* NACK 时加 "FAIL: " 前缀 —— 让控制台上"失败"一眼可见，
       而不是要人去比对命令词。前缀是安全字面量，不需要转义。
       先拼成完整文本，再整体交给 jw_prop_esc()。 */
    if (ok == 0) {
        memcpy(text, "FAIL: ", 6u);
        tlen = 6u;
    }
    if (elen > 0u) {
        if ((uint32_t)tlen + (uint32_t)elen + 1u > (uint32_t)sizeof(text)) {
            return 0u;
        }
        memcpy(text + tlen, esc, elen);
        tlen = (uint16_t)(tlen + elen);
    }
    text[tlen] = '\0';

    jw_init(&w, out, cap);
    jw_onenet_head(&w, msg_id);
    jw_prop_esc(&w, ESP32_ONENET_ID_CMD_RESULT, text, tlen, ts_ms,
                (ts_ms != 0u) ? 1 : 0);
    jw_str(&w, "}}");

    return jw_done(&w);
}
