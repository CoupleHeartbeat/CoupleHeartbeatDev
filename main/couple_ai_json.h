// main/couple_ai_json.h —— 从大模型 JSON 响应中取字段的纯逻辑
//
// 只做本应用需要的事：在响应里找到第一个 `"key":"value"` 并解码为 UTF-8。
// 不引入 cJSON、不依赖 ESP-IDF，因此可在主机上直接测试
// （见 tests/test_couple_ai_json.c）。TTS 的音频字段体积可能很大，
// 由流式解码处理，不经过本模块。
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    COUPLE_AI_JSON_OK = 0,
    COUPLE_AI_JSON_NOT_FOUND,   // 文档里没有该键（或以字符串形式存在）
    COUPLE_AI_JSON_TOO_SMALL,   // 输出缓冲放不下解码结果
    COUPLE_AI_JSON_INVALID,     // 引号未闭合、转义非法等
} couple_ai_json_result_t;

// 取第一个 `"key":"..."` 的字符串值并解码到 out（含 \\uXXXX → UTF-8）。
// 支持任意嵌套层级；因为按 JSON 转义规则跳过字符串，值里出现的 `"key":`
// 字面量不会被误判。成功时 out 以 '\0' 结尾；值为空串也算成功。
couple_ai_json_result_t couple_ai_json_get_string(
    const char *json, size_t len, const char *key,
    char *out, size_t out_size);

// 把一段 JSON 字符串内容（不含两端引号）解码到 out。供流式解析复用。
couple_ai_json_result_t couple_ai_json_decode(const char *raw, size_t raw_len,
                                             char *out, size_t out_size);
