// main/couple_time.h —— 时间同步 JSON 解析的纯逻辑
//
// 只解析本应用自己定义的扁平对象 {"unix":1767225600,"tz_offset":28800}：
// - unix      必填，UTC 纪元秒，限制在 2020-01-01 ~ 2100-01-01 之间；
// - tz_offset 可省略（缺省 0），UTC 偏移秒数（东为正），
//             限制在 [-12h, +14h]（现实中所有时区的范围）。
// 不依赖 ESP-IDF / cJSON，因此可以在主机上直接测试
//（见 tests/test_couple_time.c）。
#pragma once

#include <stdbool.h>
#include <stddef.h>

// 解析结果。细分失败原因，便于 App 侧给出可定位的提示。
typedef enum {
    COUPLE_TIME_PARSE_OK = 0,
    COUPLE_TIME_PARSE_BAD_JSON,     // 不是合法对象、字段类型不对或缺少 unix
    COUPLE_TIME_PARSE_BAD_RANGE,    // unix 或 tz_offset 超出合理范围
} couple_time_parse_result_t;

// 从 JSON 中提取 unix 秒与时区偏移秒。
// 成功时 unix_sec / tz_offset_sec 一定被写入有效值。
couple_time_parse_result_t couple_time_parse(
    const char *json, size_t len,
    long long *unix_sec, long long *tz_offset_sec);
