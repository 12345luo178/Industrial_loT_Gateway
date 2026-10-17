/**
 * @file    gw_selfcheck.c
 * @brief   上板自检框架实现（纯逻辑）
 */
#include "gw_selfcheck.h"

#include <stdio.h>
#include <string.h>

/* ==================================================================
 *  内部工具
 * ================================================================== */

static int id_valid(gw_sc_id_t id)
{
    return ((uint32_t)id < (uint32_t)GW_SC_COUNT) ? 1 : 0;
}

/**
 * 判断某个依赖项是否"满足"
 *
 * ⚠️ 只有 PASS 与 WARN 算满足。
 *    WARN 是"通过但可疑"（例如时钟偏低但在容差边缘）——
 *    它足以让下游跑起来，所以不该阻断。
 */
static int dep_satisfied(const gw_selfcheck_t *s, gw_sc_id_t dep)
{
    gw_sc_status_t st;

    if (dep == GW_SC_COUNT) {
        return 1;                   /* 无依赖 */
    }
    if (id_valid(dep) == 0) {
        return 1;                   /* 依赖写错了：不阻断（否则整个自检都跑不了） */
    }
    st = s->status[dep];
    return (st == GW_SC_PASS || st == GW_SC_WARN) ? 1 : 0;
}

/* ==================================================================
 *  对外接口
 * ================================================================== */

const char *gw_selfcheck_id_str(gw_sc_id_t id)
{
    switch (id) {
    case GW_SC_CLOCK: return "CLOCK";
    case GW_SC_GPIO:  return "GPIO";
    case GW_SC_DWT:   return "DWT";
    case GW_SC_FLASH: return "FLASH";
    case GW_SC_PARAM: return "PARAM";
    case GW_SC_DHT22: return "DHT22";
    case GW_SC_RS485: return "RS485";
    case GW_SC_CAN:   return "CAN";
    case GW_SC_ESP32: return "ESP32";
    case GW_SC_FAULT: return "FAULT";
    default:          return "?";
    }
}

const char *gw_selfcheck_status_str(gw_sc_status_t st)
{
    switch (st) {
    case GW_SC_PASS: return " OK ";
    case GW_SC_FAIL: return "FAIL";
    case GW_SC_SKIP: return "SKIP";
    case GW_SC_WARN: return "WARN";
    default:         return "????";
    }
}

int gw_selfcheck_init(gw_selfcheck_t *s, const gw_sc_item_t *items)
{
    uint8_t i;

    if (s == 0 || items == 0) {
        return -1;
    }

    /* 每一项都必须有名字与检查函数。
       少一个会导致报告里出现空行、或者运行时崩 ——
       而自检崩溃比自检失败更难查（因为它本该是排查工具）。 */
    for (i = 0u; i < (uint8_t)GW_SC_COUNT; i++) {
        if (items[i].name == 0 || items[i].run == 0) {
            return -2;
        }
    }

    memset(s, 0, sizeof(*s));
    s->items = items;

    /* 初始状态全标 SKIP：还没跑过 ——
       这样"没跑"与"跑了但跳过"在报告里不会混淆
       （value 全 0 且 status 全 SKIP）。 */
    for (i = 0u; i < (uint8_t)GW_SC_COUNT; i++) {
        s->status[i] = GW_SC_SKIP;
    }

    s->inited = 1u;
    return 0;
}

gw_sc_status_t gw_selfcheck_run_one(gw_selfcheck_t *s, gw_sc_id_t id)
{
    const gw_sc_item_t *it;
    int32_t      v = 0;
    const char  *u = "";
    gw_sc_status_t st;

    if (s == 0 || s->inited == 0u || id_valid(id) == 0) {
        return GW_SC_SKIP;
    }

    it = &s->items[id];

    /* ---- 依赖检查：不满足就跳过，**不调用 run** ---- */
    if (dep_satisfied(s, it->depends_on) == 0) {
        s->status[id] = GW_SC_SKIP;
        s->value[id]  = 0;
        s->unit[id]   = "";
        return GW_SC_SKIP;
    }

    /* ---- 执行 ---- */
    st = it->run(&v, &u);

    s->status[id] = st;
    s->value[id]  = v;
    s->unit[id]   = (u != 0) ? u : "";

    return st;
}

uint32_t gw_selfcheck_run(gw_selfcheck_t *s)
{
    uint8_t  i;
    uint32_t passed = 0u;

    if (s == 0 || s->inited == 0u) {
        return 0u;
    }

    /* 按 ID 顺序执行 —— 顺序即依赖顺序，所以一遍就能完成
       （不需要反复迭代直到收敛）。 */
    for (i = 0u; i < (uint8_t)GW_SC_COUNT; i++) {
        gw_sc_status_t st = gw_selfcheck_run_one(s, (gw_sc_id_t)i);

        if (st == GW_SC_PASS || st == GW_SC_WARN) {
            passed++;
        }
        s->ran++;
    }
    return passed;
}

gw_sc_status_t gw_selfcheck_status(const gw_selfcheck_t *s, gw_sc_id_t id)
{
    if (s == 0 || id_valid(id) == 0) {
        return GW_SC_SKIP;
    }
    return s->status[id];
}

int32_t gw_selfcheck_value(const gw_selfcheck_t *s, gw_sc_id_t id)
{
    if (s == 0 || id_valid(id) == 0) {
        return 0;
    }
    return s->value[id];
}

gw_sc_id_t gw_selfcheck_first_failure(const gw_selfcheck_t *s)
{
    uint8_t i;

    if (s == 0) {
        return GW_SC_COUNT;
    }
    for (i = 0u; i < (uint8_t)GW_SC_COUNT; i++) {
        if (s->status[i] == GW_SC_FAIL) {
            return (gw_sc_id_t)i;
        }
    }
    return GW_SC_COUNT;
}

uint32_t gw_selfcheck_count(const gw_selfcheck_t *s, gw_sc_status_t st)
{
    uint8_t  i;
    uint32_t n = 0u;

    if (s == 0) {
        return 0u;
    }
    for (i = 0u; i < (uint8_t)GW_SC_COUNT; i++) {
        if (s->status[i] == st) {
            n++;
        }
    }
    return n;
}

int gw_selfcheck_foundation_ok(const gw_selfcheck_t *s)
{
    /* 基础项：时钟、GPIO、DWT、Flash。
       这四项不过，后面的检查（DHT22 时序、Modbus 波特率、CAN 位时序）
       全都没有意义 —— 它们都建立在"时钟是对的"这个前提上。 */
    static const gw_sc_id_t base[] = {
        GW_SC_CLOCK, GW_SC_GPIO, GW_SC_DWT, GW_SC_FLASH
    };
    uint8_t i;

    if (s == 0) {
        return 0;
    }
    for (i = 0u; i < (uint8_t)(sizeof(base) / sizeof(base[0])); i++) {
        gw_sc_status_t st = s->status[base[i]];

        if (st != GW_SC_PASS && st != GW_SC_WARN) {
            return 0;
        }
    }
    return 1;
}

uint16_t gw_selfcheck_report(const gw_selfcheck_t *s, char *out, uint16_t cap)
{
    uint8_t  i;
    uint16_t n = 0u;

    if (s == 0 || out == 0 || cap < 32u) {
        return 0u;
    }

    /* 手写拼接而不是 snprintf 一串：
       报告是给人看的，格式要完全可控（对齐、缩进），
       而 snprintf 的 %-Ns 在不同 C 库上对宽字符的处理不完全一致。 */
    {
        int w = snprintf(out + n, (size_t)(cap - n),
                         "[SELFCHECK] 通过 %u / 失败 %u / 警告 %u / 跳过 %u\r\n",
                         (unsigned)gw_selfcheck_count(s, GW_SC_PASS),
                         (unsigned)gw_selfcheck_count(s, GW_SC_FAIL),
                         (unsigned)gw_selfcheck_count(s, GW_SC_WARN),
                         (unsigned)gw_selfcheck_count(s, GW_SC_SKIP));

        if (w <= 0 || (uint16_t)w >= (uint16_t)(cap - n)) {
            out[cap - 1u] = '\0';
            return (uint16_t)(cap - 1u);
        }
        n = (uint16_t)(n + w);
    }

    for (i = 0u; i < (uint8_t)GW_SC_COUNT; i++) {
        const gw_sc_item_t *it = &s->items[i];
        int w;

        /* ---- 主行：状态 + 项名 + 实测值 + 期望 ---- */
        if (s->status[i] == GW_SC_SKIP) {
            w = snprintf(out + n, (size_t)(cap - n),
                         "[SKIP] %-8s %10s  (依赖未通过)\r\n",
                         it->name, "-");
        } else {
            w = snprintf(out + n, (size_t)(cap - n),
                         "[%s] %-8s %10ld %-3s",
                         gw_selfcheck_status_str(s->status[i]),
                         it->name,
                         (long)s->value[i],
                         (s->unit[i] != 0) ? s->unit[i] : "");

            if (w > 0 && (uint16_t)w < (uint16_t)(cap - n)
                && it->expect != 0) {
                int w2 = snprintf(out + n + w, (size_t)(cap - n - (uint16_t)w),
                                  " (期望 %ld ±%ld)",
                                  (long)it->expect, (long)it->tol);

                if (w2 > 0 && (uint16_t)w2 < (uint16_t)(cap - n - (uint16_t)w)) {
                    w = (int)((uint16_t)w + (uint16_t)w2);
                }
            }
            if (w > 0 && (uint16_t)w < (uint16_t)(cap - n)) {
                int w3 = snprintf(out + n + w,
                                  (size_t)(cap - n - (uint16_t)w), "\r\n");

                if (w3 > 0 && (uint16_t)w3 < (uint16_t)(cap - n - (uint16_t)w)) {
                    w = (int)((uint16_t)w + (uint16_t)w3);
                }
            }
        }

        if (w <= 0) {
            break;                  /* 缓冲满了：不再写，也不算长度 */
        }
        if ((uint16_t)w >= (uint16_t)(cap - n)) {
            out[cap - 1u] = '\0';
            return (uint16_t)(cap - 1u);
        }
        n = (uint16_t)(n + w);

        /* ---- 失败项附加"先查什么" ---- */
        if (s->status[i] == GW_SC_FAIL && it->hint != 0) {
            const char *p = it->hint;

            while (*p != '\0' && n < (uint16_t)(cap - 4u)) {
                const char *eol = strchr(p, '\n');
                uint16_t    len = (eol != 0)
                                  ? (uint16_t)(eol - p)
                                  : (uint16_t)strlen(p);

                if ((uint16_t)(n + len + 8u) >= cap) {
                    break;
                }
                out[n++] = ' ';
                out[n++] = ' ';
                out[n++] = ' ';
                out[n++] = ' ';
                out[n++] = ' ';
                out[n++] = ' ';
                memcpy(out + n, p, len);
                n = (uint16_t)(n + len);
                out[n++] = '\r';
                out[n++] = '\n';

                if (eol == 0) {
                    break;
                }
                p = eol + 1;
            }
        }
    }

    out[n] = '\0';
    return n;
}
