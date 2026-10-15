// main/couple_time.c —— 时间同步 JSON 解析的纯逻辑实现
#include "couple_time.h"

#include <string.h>

// 合理范围：unix 约 2020-01-01(1577836800) ~ 2100-01-01(4102444800)；
// tz_offset 为 UTC 偏移秒数，[-12*3600, +14*3600]（现实中所有时区）。
#define TIME_UNIX_MIN  1577836800LL
#define TIME_UNIX_MAX  4102444800LL
#define TIME_TZ_MIN    (-12LL * 3600)
#define TIME_TZ_MAX    (14LL * 3600)

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        p++;
    }
    return p;
}

// 读取一个 JSON 字符串字面量，返回其"原始"内容（含转义，不含两端引号）。
static bool read_raw_string(const char **pp, const char *end,
                            const char **out, size_t *out_len)
{
    const char *p = skip_ws(*pp, end);
    if (p >= end || *p != '"') return false;

    const char *start = ++p;
    while (p < end) {
        if (*p == '\\') {
            if (p + 1 >= end) return false;  // 悬空转义
            p += 2;
            continue;
        }
        if (*p == '"') break;
        p++;
    }
    if (p >= end) return false;  // 引号未闭合

    *out = start;
    *out_len = (size_t)(p - start);
    *pp = p + 1;
    return true;
}

// 读取一个整数字面量（可带负号；不接受小数/指数，App 侧只发整数）。
static bool read_number(const char **pp, const char *end, long long *out)
{
    const char *p = skip_ws(*pp, end);
    bool neg = false;
    if (p < end && (*p == '-' || *p == '+')) {
        neg = (*p == '-');
        p++;
    }
    if (p >= end || *p < '0' || *p > '9') return false;

    long long v = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        // 上界保护：unix 最大 4102444800，远小于该哨兵，溢出即非法
        if (v > TIME_UNIX_MAX * 10) return false;
        p++;
    }
    *out = neg ? -v : v;
    *pp = p;
    return true;
}

// 跳过我们不关心的字段值（字符串/数字/布尔/null/嵌套结构）。
static bool skip_value(const char **pp, const char *end)
{
    const char *p = skip_ws(*pp, end);
    if (p >= end) return false;

    if (*p == '"') {
        const char *raw;
        size_t raw_len;
        if (!read_raw_string(&p, end, &raw, &raw_len)) return false;
        *pp = p;
        return true;
    }

    // 嵌套对象/数组按括号配对整体跳过，避免把内层 '}' 当成外层结束。
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (p < end) {
            if (*p == '"') {
                const char *raw;
                size_t raw_len;
                if (!read_raw_string(&p, end, &raw, &raw_len)) return false;
                continue;
            }
            if (*p == '{' || *p == '[') {
                depth++;
            } else if (*p == '}' || *p == ']') {
                depth--;
                p++;
                if (depth == 0) {
                    *pp = p;
                    return true;
                }
                continue;
            }
            p++;
        }
        return false;
    }

    while (p < end && *p != ',' && *p != '}') {
        p++;
    }
    *pp = p;
    return true;
}

couple_time_parse_result_t couple_time_parse(
    const char *json, size_t len,
    long long *unix_sec, long long *tz_offset_sec)
{
    if (!json || len == 0 || !unix_sec || !tz_offset_sec) {
        return COUPLE_TIME_PARSE_BAD_JSON;
    }

    *unix_sec = 0;
    *tz_offset_sec = 0;
    bool have_unix = false;

    const char *end = json + len;
    const char *p = skip_ws(json, end);
    if (p >= end || *p != '{') return COUPLE_TIME_PARSE_BAD_JSON;
    p++;

    for (;;) {
        p = skip_ws(p, end);
        if (p >= end) return COUPLE_TIME_PARSE_BAD_JSON;
        if (*p == '}') {
            p++;
            break;
        }

        const char *key;
        size_t key_len;
        if (!read_raw_string(&p, end, &key, &key_len)) {
            return COUPLE_TIME_PARSE_BAD_JSON;
        }

        p = skip_ws(p, end);
        if (p >= end || *p != ':') return COUPLE_TIME_PARSE_BAD_JSON;
        p++;

        if (key_len == 4 && memcmp(key, "unix", 4) == 0) {
            if (!read_number(&p, end, unix_sec)) return COUPLE_TIME_PARSE_BAD_JSON;
            have_unix = true;
        } else if (key_len == 9 && memcmp(key, "tz_offset", 9) == 0) {
            if (!read_number(&p, end, tz_offset_sec)) {
                return COUPLE_TIME_PARSE_BAD_JSON;
            }
        } else if (!skip_value(&p, end)) {
            return COUPLE_TIME_PARSE_BAD_JSON;
        }

        p = skip_ws(p, end);
        if (p < end && *p == ',') {
            p++;
            continue;
        }
        if (p < end && *p == '}') {
            p++;
            break;
        }
        return COUPLE_TIME_PARSE_BAD_JSON;
    }

    if (!have_unix) return COUPLE_TIME_PARSE_BAD_JSON;

    if (*unix_sec < TIME_UNIX_MIN || *unix_sec > TIME_UNIX_MAX) {
        return COUPLE_TIME_PARSE_BAD_RANGE;
    }
    if (*tz_offset_sec < TIME_TZ_MIN || *tz_offset_sec > TIME_TZ_MAX) {
        return COUPLE_TIME_PARSE_BAD_RANGE;
    }
    return COUPLE_TIME_PARSE_OK;
}
