// main/couple_wifi_parse.h —— Wi-Fi 凭据 JSON 解析的纯逻辑
//
// 只做本应用自己定义的扁平对象 {"ssid":"...","password":"..."} 的解析，
// 不依赖 ESP-IDF / cJSON，因此可以在主机上直接测试（见 tests/test_couple_wifi_parse.c）。
#pragma once

#include <stdbool.h>
#include <stddef.h>

// 解析结果。细分失败原因，便于 App 侧给出可定位的提示，
// 而不是把所有失败都当成"配网失败"。
typedef enum {
    COUPLE_WIFI_PARSE_OK = 0,
    COUPLE_WIFI_PARSE_BAD_JSON,       // 不是合法对象、字段类型不对或含不支持的转义
    COUPLE_WIFI_PARSE_NO_SSID,        // 缺少 ssid，或 ssid 为空
    COUPLE_WIFI_PARSE_SSID_TOO_LONG,  // ssid 超出接收缓冲
    COUPLE_WIFI_PARSE_PASS_TOO_LONG,  // password 超出接收缓冲
} couple_wifi_parse_result_t;

// 从 JSON 中提取凭据。
// - password 允许缺省或为空字符串（开放网络）。
// - 支持的转义：\" \\ \/ \b \f \n \r \t；其余（含 \uXXXX）视为不合法。
// - 成功时 ssid/password 一定以 '\0' 结尾。
couple_wifi_parse_result_t couple_wifi_parse_credentials(
    const char *json, size_t len,
    char *ssid, size_t ssid_size,
    char *password, size_t pass_size);
