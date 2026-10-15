// tests/test_couple_time.c —— couple_time_parse 主机单元测试
#include "couple_time.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// 便捷包装：按字符串长度调用解析器
static couple_time_parse_result_t parse(const char *json,
                                        long long *unix_sec,
                                        long long *tz)
{
    return couple_time_parse(json, strlen(json), unix_sec, tz);
}

static void test_ok_full(void)
{
    long long u = 0, t = 0;
    assert(parse("{\"unix\":1767225600,\"tz_offset\":28800}", &u, &t)
           == COUPLE_TIME_PARSE_OK);
    assert(u == 1767225600LL);
    assert(t == 28800LL);
}

static void test_ok_key_order_ignored(void)
{
    long long u = 0, t = 0;
    assert(parse("{\"tz_offset\":-18000,\"unix\":1577836800}", &u, &t)
           == COUPLE_TIME_PARSE_OK);
    assert(u == 1577836800LL);
    assert(t == -18000LL);
}

static void test_ok_tz_optional(void)
{
    long long u = 0, t = -1;
    assert(parse("{\"unix\":1700000000}", &u, &t) == COUPLE_TIME_PARSE_OK);
    assert(u == 1700000000LL);
    assert(t == 0);  // 缺省为 0（UTC）
}

static void test_ok_unknown_fields_ignored(void)
{
    long long u = 0, t = 0;
    assert(parse("{\"note\":\"x\",\"unix\":1700000001,\"tz_offset\":0,"
                 "\"extra\":{\"a\":[1,2]}}", &u, &t)
           == COUPLE_TIME_PARSE_OK);
    assert(u == 1700000001LL);
    assert(t == 0);
}

static void test_ok_whitespace(void)
{
    long long u = 0, t = 0;
    assert(parse("  { \"unix\" : 1700000002 , \"tz_offset\" : 3600 } ",
                 &u, &t) == COUPLE_TIME_PARSE_OK);
    assert(u == 1700000002LL);
    assert(t == 3600LL);
}

static void test_bad_missing_unix(void)
{
    long long u = 0, t = 0;
    assert(parse("{\"tz_offset\":28800}", &u, &t)
           == COUPLE_TIME_PARSE_BAD_JSON);
}

static void test_bad_json(void)
{
    long long u = 0, t = 0;
    assert(parse("", &u, &t) == COUPLE_TIME_PARSE_BAD_JSON);
    assert(parse("not json", &u, &t) == COUPLE_TIME_PARSE_BAD_JSON);
    assert(parse("{\"unix\":}", &u, &t) == COUPLE_TIME_PARSE_BAD_JSON);
    assert(parse("{\"unix\":1,", &u, &t) == COUPLE_TIME_PARSE_BAD_JSON);
    assert(parse("[1,2]", &u, &t) == COUPLE_TIME_PARSE_BAD_JSON);
}

static void test_bad_range(void)
{
    long long u = 0, t = 0;
    // unix 过小（早于 2020）
    assert(parse("{\"unix\":1000000000}", &u, &t)
           == COUPLE_TIME_PARSE_BAD_RANGE);
    // unix 过大（晚于 2100）
    assert(parse("{\"unix\":5000000000}", &u, &t)
           == COUPLE_TIME_PARSE_BAD_RANGE);
    // tz_offset 超出现实时区范围
    assert(parse("{\"unix\":1700000000,\"tz_offset\":999999}", &u, &t)
           == COUPLE_TIME_PARSE_BAD_RANGE);
    assert(parse("{\"unix\":1700000000,\"tz_offset\":-999999}", &u, &t)
           == COUPLE_TIME_PARSE_BAD_RANGE);
}

static void test_null_args(void)
{
    long long u = 0, t = 0;
    assert(couple_time_parse(NULL, 5, &u, &t) == COUPLE_TIME_PARSE_BAD_JSON);
    assert(couple_time_parse("{}", 2, NULL, &t) == COUPLE_TIME_PARSE_BAD_JSON);
    assert(couple_time_parse("{}", 2, &u, NULL) == COUPLE_TIME_PARSE_BAD_JSON);
}

int main(void)
{
    test_ok_full();
    test_ok_key_order_ignored();
    test_ok_tz_optional();
    test_ok_unknown_fields_ignored();
    test_ok_whitespace();
    test_bad_missing_unix();
    test_bad_json();
    test_bad_range();
    test_null_args();
    printf("test_couple_time: PASS\n");
    return 0;
}
