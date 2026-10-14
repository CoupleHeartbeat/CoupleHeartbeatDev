// tests/test_couple_ai_json.c —— 大模型响应字段提取的边界主机测试
#include "couple_ai_json.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static couple_ai_json_result_t get(const char *json, const char *key,
                                  char *out, size_t out_size)
{
    return couple_ai_json_get_string(json, strlen(json), key, out, out_size);
}

static void test_chat_response(void)
{
    const char *json =
        "{\"id\":\"x\",\"choices\":[{\"finish_reason\":\"stop\",\"index\":0,"
        "\"message\":{\"content\":\"你好呀\",\"role\":\"assistant\","
        "\"tool_calls\":null}}],\"usage\":{\"total_tokens\":91}}";
    char out[128];
    assert(get(json, "content", out, sizeof(out)) == COUPLE_AI_JSON_OK);
    assert(strcmp(out, "你好呀") == 0);
}

static void test_picks_first_occurrence(void)
{
    const char *json = "{\"content\":\"first\",\"other\":{\"content\":\"second\"}}";
    char out[32];
    assert(get(json, "content", out, sizeof(out)) == COUPLE_AI_JSON_OK);
    assert(strcmp(out, "first") == 0);
}

static void test_escapes(void)
{
    const char *json = "{\"content\":\"a\\nb\\\"c\\\\d\\/e\\tf\"}";
    char out[64];
    assert(get(json, "content", out, sizeof(out)) == COUPLE_AI_JSON_OK);
    assert(strcmp(out, "a\nb\"c\\d/e\tf") == 0);
}

static void test_unicode_escape(void)
{
    const char *json = "{\"content\":\"\\u4f60\\u597d\"}";
    char out[32];
    assert(get(json, "content", out, sizeof(out)) == COUPLE_AI_JSON_OK);
    assert(strcmp(out, "你好") == 0);
}

static void test_unicode_surrogate_pair(void)
{
    // U+1F600 拆成 D83D DE00
    const char *json = "{\"content\":\"\\ud83d\\ude00\"}";
    char out[16];
    assert(get(json, "content", out, sizeof(out)) == COUPLE_AI_JSON_OK);
    assert(strcmp(out, "\xF0\x9F\x98\x80") == 0);
}

static void test_lone_surrogate_is_invalid(void)
{
    const char *json = "{\"content\":\"\\ud83d\"}";
    char out[16];
    assert(get(json, "content", out, sizeof(out)) == COUPLE_AI_JSON_INVALID);
}

static void test_key_inside_value_is_not_matched(void)
{
    // 值里出现 "content":"fake" 的转义字面量，不能命中
    const char *json =
        "{\"note\":\"\\\"content\\\":\\\"fake\\\"\",\"content\":\"real\"}";
    char out[32];
    assert(get(json, "content", out, sizeof(out)) == COUPLE_AI_JSON_OK);
    assert(strcmp(out, "real") == 0);
}

static void test_missing_key(void)
{
    const char *json = "{\"id\":\"x\",\"choices\":[]}";
    char out[32];
    assert(get(json, "content", out, sizeof(out)) == COUPLE_AI_JSON_NOT_FOUND);
}

static void test_non_string_value_is_not_found(void)
{
    const char *json = "{\"content\":123,\"other\":\"v\"}";
    char out[32];
    assert(get(json, "content", out, sizeof(out)) == COUPLE_AI_JSON_NOT_FOUND);
}

static void test_empty_string_value(void)
{
    const char *json = "{\"content\":\"\"}";
    char out[32];
    assert(get(json, "content", out, sizeof(out)) == COUPLE_AI_JSON_OK);
    assert(strcmp(out, "") == 0);
}

static void test_buffer_too_small(void)
{
    const char *json = "{\"content\":\"abcdef\"}";
    char out[4];
    assert(get(json, "content", out, sizeof(out)) == COUPLE_AI_JSON_TOO_SMALL);
}

static void test_unterminated_string_is_not_found(void)
{
    const char *json = "{\"content\":\"abc";
    char out[32];
    // 引号未闭合：找不到可用的值，按未命中处理，不得越界读取
    assert(get(json, "content", out, sizeof(out)) == COUPLE_AI_JSON_NOT_FOUND);
}

static void test_null_and_empty_input(void)
{
    char out[16];
    assert(couple_ai_json_get_string(NULL, 0, "content", out, sizeof(out)) ==
           COUPLE_AI_JSON_INVALID);
    assert(get("", "content", out, sizeof(out)) == COUPLE_AI_JSON_INVALID);
}

static void test_decode_direct(void)
{
    char out[32];
    assert(couple_ai_json_decode("plain", 5, out, sizeof(out)) == COUPLE_AI_JSON_OK);
    assert(strcmp(out, "plain") == 0);

    assert(couple_ai_json_decode("\\u0041", 6, out, sizeof(out)) == COUPLE_AI_JSON_OK);
    assert(strcmp(out, "A") == 0);

    assert(couple_ai_json_decode("\\q", 2, out, sizeof(out)) == COUPLE_AI_JSON_INVALID);
    assert(couple_ai_json_decode("ab", 2, out, 2) == COUPLE_AI_JSON_TOO_SMALL);
}

int main(void)
{
    test_chat_response();
    test_picks_first_occurrence();
    test_escapes();
    test_unicode_escape();
    test_unicode_surrogate_pair();
    test_lone_surrogate_is_invalid();
    test_key_inside_value_is_not_matched();
    test_missing_key();
    test_non_string_value_is_not_found();
    test_empty_string_value();
    test_buffer_too_small();
    test_unterminated_string_is_not_found();
    test_null_and_empty_input();
    test_decode_direct();
    printf("test_couple_ai_json: PASS\n");
    return 0;
}
