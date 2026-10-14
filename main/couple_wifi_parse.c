// main/couple_wifi_parse.c —— Wi-Fi 凭据 JSON 解析的纯逻辑实现
#include "couple_wifi_parse.h"

#include <string.h>

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

// 跳过我们不关心的字段值（数字/布尔/null/嵌套结构）。
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

// 把带转义的字符串解码到 out。返回 0 成功，-1 缓冲不足，-2 不支持的转义。
static int decode_string(const char *raw, size_t raw_len,
                         char *out, size_t out_size)
{
    size_t w = 0;
    for (size_t i = 0; i < raw_len; i++) {
        char c = raw[i];
        if (c == '\\') {
            if (i + 1 >= raw_len) return -2;
            char esc = raw[++i];
            switch (esc) {
            case '"':  c = '"';  break;
            case '\\': c = '\\'; break;
            case '/':  c = '/';  break;
            case 'b':  c = '\b'; break;
            case 'f':  c = '\f'; break;
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            case 't':  c = '\t'; break;
            default:   return -2;
            }
        }
        if (w + 1 >= out_size) return -1;
        out[w++] = c;
    }
    out[w] = '\0';
    return 0;
}

couple_wifi_parse_result_t couple_wifi_parse_credentials(
    const char *json, size_t len,
    char *ssid, size_t ssid_size,
    char *password, size_t pass_size)
{
    if (!json || len == 0 || !ssid || ssid_size == 0 || !password || pass_size == 0) {
        return COUPLE_WIFI_PARSE_BAD_JSON;
    }

    ssid[0] = '\0';
    password[0] = '\0';

    const char *end = json + len;
    const char *p = skip_ws(json, end);
    if (p >= end || *p != '{') return COUPLE_WIFI_PARSE_BAD_JSON;
    p++;

    const char *ssid_raw = NULL;
    size_t ssid_raw_len = 0;
    const char *pass_raw = NULL;
    size_t pass_raw_len = 0;

    for (;;) {
        p = skip_ws(p, end);
        if (p >= end) return COUPLE_WIFI_PARSE_BAD_JSON;
        if (*p == '}') {
            p++;
            break;
        }

        const char *key;
        size_t key_len;
        if (!read_raw_string(&p, end, &key, &key_len)) {
            return COUPLE_WIFI_PARSE_BAD_JSON;
        }

        p = skip_ws(p, end);
        if (p >= end || *p != ':') return COUPLE_WIFI_PARSE_BAD_JSON;
        p++;

        if (key_len == 4 && memcmp(key, "ssid", 4) == 0) {
            if (!read_raw_string(&p, end, &ssid_raw, &ssid_raw_len)) {
                return COUPLE_WIFI_PARSE_BAD_JSON;
            }
        } else if (key_len == 8 && memcmp(key, "password", 8) == 0) {
            if (!read_raw_string(&p, end, &pass_raw, &pass_raw_len)) {
                return COUPLE_WIFI_PARSE_BAD_JSON;
            }
        } else if (!skip_value(&p, end)) {
            return COUPLE_WIFI_PARSE_BAD_JSON;
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
        return COUPLE_WIFI_PARSE_BAD_JSON;
    }

    if (!ssid_raw || ssid_raw_len == 0) return COUPLE_WIFI_PARSE_NO_SSID;

    int rc = decode_string(ssid_raw, ssid_raw_len, ssid, ssid_size);
    if (rc == -1) return COUPLE_WIFI_PARSE_SSID_TOO_LONG;
    if (rc != 0) return COUPLE_WIFI_PARSE_BAD_JSON;

    if (pass_raw) {
        rc = decode_string(pass_raw, pass_raw_len, password, pass_size);
        if (rc == -1) return COUPLE_WIFI_PARSE_PASS_TOO_LONG;
        if (rc != 0) return COUPLE_WIFI_PARSE_BAD_JSON;
    }

    return COUPLE_WIFI_PARSE_OK;
}
