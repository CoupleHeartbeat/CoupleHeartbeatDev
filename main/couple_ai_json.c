// main/couple_ai_json.c —— 大模型 JSON 响应字段提取的纯逻辑实现
#include "couple_ai_json.h"

#include <string.h>

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
        p++;
    }
    return p;
}

// 读取一个字符串字面量，返回其原始内容（含转义，不含两端引号）。
static bool read_raw(const char **pp, const char *end,
                     const char **out, size_t *out_len)
{
    const char *p = skip_ws(*pp, end);
    if (p >= end || *p != '"') return false;

    const char *start = ++p;
    while (p < end) {
        if (*p == '\\') {
            if (p + 1 >= end) return false;
            p += 2;
            continue;
        }
        if (*p == '"') break;
        p++;
    }
    if (p >= end) return false;

    *out = start;
    *out_len = (size_t)(p - start);
    *pp = p + 1;
    return true;
}

static int hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
        else return -1;
        v = (v << 4) | d;
    }
    *out = v;
    return 0;
}

// 把一个码点写成 UTF-8，返回写入字节数；缓冲不足返回 -1。
static int put_utf8(unsigned cp, char *out, size_t out_size, size_t w)
{
    if (cp < 0x80) {
        if (w + 1 >= out_size) return -1;
        out[w++] = (char)cp;
    } else if (cp < 0x800) {
        if (w + 2 >= out_size) return -1;
        out[w++] = (char)(0xC0 | (cp >> 6));
        out[w++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        if (w + 3 >= out_size) return -1;
        out[w++] = (char)(0xE0 | (cp >> 12));
        out[w++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[w++] = (char)(0x80 | (cp & 0x3F));
    } else {
        if (w + 4 >= out_size) return -1;
        out[w++] = (char)(0xF0 | (cp >> 18));
        out[w++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[w++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[w++] = (char)(0x80 | (cp & 0x3F));
    }
    return (int)w;
}

couple_ai_json_result_t couple_ai_json_decode(const char *raw, size_t raw_len,
                                             char *out, size_t out_size)
{
    if (!raw || !out || out_size == 0) return COUPLE_AI_JSON_INVALID;

    size_t w = 0;
    for (size_t i = 0; i < raw_len; i++) {
        char c = raw[i];
        if (c != '\\') {
            if (w + 1 >= out_size) return COUPLE_AI_JSON_TOO_SMALL;
            out[w++] = c;
            continue;
        }
        if (i + 1 >= raw_len) return COUPLE_AI_JSON_INVALID;
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
        case 'u': {
            if (i + 4 >= raw_len) return COUPLE_AI_JSON_INVALID;
            unsigned cp;
            if (hex4(&raw[i + 1], &cp) != 0) return COUPLE_AI_JSON_INVALID;
            i += 4;
            // 合并 UTF-16 代理对，避免中文以外的补充平面字符解成乱码
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (i + 6 >= raw_len || raw[i + 1] != '\\' || raw[i + 2] != 'u') {
                    return COUPLE_AI_JSON_INVALID;
                }
                unsigned lo;
                if (hex4(&raw[i + 3], &lo) != 0) return COUPLE_AI_JSON_INVALID;
                if (lo < 0xDC00 || lo > 0xDFFF) return COUPLE_AI_JSON_INVALID;
                i += 6;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                return COUPLE_AI_JSON_INVALID;  // 落单的低代理
            }
            int nw = put_utf8(cp, out, out_size, w);
            if (nw < 0) return COUPLE_AI_JSON_TOO_SMALL;
            w = (size_t)nw;
            continue;
        }
        default:
            return COUPLE_AI_JSON_INVALID;
        }
        if (w + 1 >= out_size) return COUPLE_AI_JSON_TOO_SMALL;
        out[w++] = c;
    }
    out[w] = '\0';
    return COUPLE_AI_JSON_OK;
}

// 在文档中找第一个 "key":"string"。因为按转义规则跳过每个字符串字面量，
// 值内部的 `"key":` 文本不会被当成键。
static bool find_string(const char *p, const char *end,
                        const char *key, size_t key_len,
                        const char **out_raw, size_t *out_len)
{
    while (p < end) {
        if (*p != '"') {
            p++;
            continue;
        }

        const char *ks;
        size_t kl;
        if (!read_raw(&p, end, &ks, &kl)) return false;

        const char *q = skip_ws(p, end);
        if (q >= end || *q != ':') continue;   // 只是普通字符串值

        q = skip_ws(q + 1, end);
        if (kl != key_len || memcmp(ks, key, key_len) != 0) continue;
        if (q >= end || *q != '"') continue;   // 键命中但值不是字符串

        const char *vs;
        size_t vl;
        if (!read_raw(&q, end, &vs, &vl)) return false;
        *out_raw = vs;
        *out_len = vl;
        return true;
    }
    return false;
}

couple_ai_json_result_t couple_ai_json_get_string(
    const char *json, size_t len, const char *key,
    char *out, size_t out_size)
{
    if (!json || len == 0 || !key || !out || out_size == 0) {
        return COUPLE_AI_JSON_INVALID;
    }
    out[0] = '\0';

    const char *raw;
    size_t raw_len;
    if (!find_string(json, json + len, key, strlen(key), &raw, &raw_len)) {
        return COUPLE_AI_JSON_NOT_FOUND;
    }
    return couple_ai_json_decode(raw, raw_len, out, out_size);
}
