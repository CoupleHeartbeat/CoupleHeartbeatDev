// tests/test_couple_wifi_parse.c —— Wi-Fi 凭据 JSON 解析的边界主机测试
#include "couple_wifi_parse.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static couple_wifi_parse_result_t parse(const char *json,
                                        char *ssid, size_t ssid_size,
                                        char *pass, size_t pass_size)
{
    return couple_wifi_parse_credentials(json, strlen(json), ssid, ssid_size,
                                        pass, pass_size);
}

static void test_normal(void)
{
    char ssid[33];
    char pass[65];
    const char *json = "{\"ssid\":\"HomeNet\",\"password\":\"s3cret!\"}";
    assert(parse(json, ssid, sizeof(ssid), pass, sizeof(pass)) == COUPLE_WIFI_PARSE_OK);
    assert(strcmp(ssid, "HomeNet") == 0);
    assert(strcmp(pass, "s3cret!") == 0);
}

static void test_empty_password_is_open_network(void)
{
    char ssid[33];
    char pass[65];
    assert(parse("{\"ssid\":\"Open\",\"password\":\"\"}",
                 ssid, sizeof(ssid), pass, sizeof(pass)) == COUPLE_WIFI_PARSE_OK);
    assert(strcmp(ssid, "Open") == 0);
    assert(strcmp(pass, "") == 0);
}

static void test_missing_password_is_open_network(void)
{
    char ssid[33];
    char pass[65];
    assert(parse("{\"ssid\":\"Open\"}",
                 ssid, sizeof(ssid), pass, sizeof(pass)) == COUPLE_WIFI_PARSE_OK);
    assert(strcmp(pass, "") == 0);
}

static void test_field_order_and_extra_fields(void)
{
    char ssid[33];
    char pass[65];
    const char *json =
        "{\"extra\":123,\"password\":\"pw\",\"nested\":{\"a\":1},\"ssid\":\"X\"}";
    assert(parse(json, ssid, sizeof(ssid), pass, sizeof(pass)) == COUPLE_WIFI_PARSE_OK);
    assert(strcmp(ssid, "X") == 0);
    assert(strcmp(pass, "pw") == 0);
}

static void test_whitespace_tolerated(void)
{
    char ssid[33];
    char pass[65];
    const char *json = "  {  \"ssid\" : \"A B\" , \"password\" : \"p\" }  ";
    assert(parse(json, ssid, sizeof(ssid), pass, sizeof(pass)) == COUPLE_WIFI_PARSE_OK);
    assert(strcmp(ssid, "A B") == 0);
}

static void test_escapes_decoded(void)
{
    char ssid[33];
    char pass[65];
    const char *json = "{\"ssid\":\"a\\\"b\\\\c\",\"password\":\"p\\/q\"}";
    assert(parse(json, ssid, sizeof(ssid), pass, sizeof(pass)) == COUPLE_WIFI_PARSE_OK);
    assert(strcmp(ssid, "a\"b\\c") == 0);
    assert(strcmp(pass, "p/q") == 0);
}

static void test_unsupported_escape_rejected(void)
{
    char ssid[33];
    char pass[65];
    const char *json = "{\"ssid\":\"a\\u4e2d\",\"password\":\"p\"}";
    assert(parse(json, ssid, sizeof(ssid), pass, sizeof(pass)) == COUPLE_WIFI_PARSE_BAD_JSON);
}

static void test_missing_ssid(void)
{
    char ssid[33];
    char pass[65];
    assert(parse("{\"password\":\"p\"}",
                 ssid, sizeof(ssid), pass, sizeof(pass)) == COUPLE_WIFI_PARSE_NO_SSID);
    assert(parse("{\"ssid\":\"\"}",
                 ssid, sizeof(ssid), pass, sizeof(pass)) == COUPLE_WIFI_PARSE_NO_SSID);
}

static void test_malformed_json(void)
{
    char ssid[33];
    char pass[65];
    assert(parse("not json", ssid, sizeof(ssid), pass, sizeof(pass)) == COUPLE_WIFI_PARSE_BAD_JSON);
    assert(parse("{\"ssid\":\"a\"", ssid, sizeof(ssid), pass, sizeof(pass)) == COUPLE_WIFI_PARSE_BAD_JSON);
    assert(parse("{\"ssid\":123}", ssid, sizeof(ssid), pass, sizeof(pass)) == COUPLE_WIFI_PARSE_BAD_JSON);
    assert(parse("[]", ssid, sizeof(ssid), pass, sizeof(pass)) == COUPLE_WIFI_PARSE_BAD_JSON);
    assert(parse("{\"ssid\":\"a\" \"password\":\"b\"}",
                 ssid, sizeof(ssid), pass, sizeof(pass)) == COUPLE_WIFI_PARSE_BAD_JSON);
}

static void test_length_limits(void)
{
    char ssid[6];
    char pass[4];
    // 缓冲需容纳内容 + '\0'：5 字节内容放进 6 字节正好，6 字节内容超限
    assert(parse("{\"ssid\":\"12345\"}", ssid, sizeof(ssid), pass, sizeof(pass)) == COUPLE_WIFI_PARSE_OK);
    assert(parse("{\"ssid\":\"123456\"}", ssid, sizeof(ssid), pass, sizeof(pass)) == COUPLE_WIFI_PARSE_SSID_TOO_LONG);

    char ssid2[33];
    assert(parse("{\"ssid\":\"n\",\"password\":\"123\"}", ssid2, sizeof(ssid2), pass, sizeof(pass)) == COUPLE_WIFI_PARSE_OK);
    assert(parse("{\"ssid\":\"n\",\"password\":\"1234\"}", ssid2, sizeof(ssid2), pass, sizeof(pass)) == COUPLE_WIFI_PARSE_PASS_TOO_LONG);
}

static void test_null_and_empty_input(void)
{
    char ssid[33];
    char pass[65];
    assert(couple_wifi_parse_credentials(NULL, 0, ssid, sizeof(ssid), pass, sizeof(pass)) == COUPLE_WIFI_PARSE_BAD_JSON);
    assert(couple_wifi_parse_credentials("", 0, ssid, sizeof(ssid), pass, sizeof(pass)) == COUPLE_WIFI_PARSE_BAD_JSON);
}

int main(void)
{
    test_normal();
    test_empty_password_is_open_network();
    test_missing_password_is_open_network();
    test_field_order_and_extra_fields();
    test_whitespace_tolerated();
    test_escapes_decoded();
    test_unsupported_escape_rejected();
    test_missing_ssid();
    test_malformed_json();
    test_length_limits();
    test_null_and_empty_input();
    printf("test_couple_wifi_parse: PASS\n");
    return 0;
}
