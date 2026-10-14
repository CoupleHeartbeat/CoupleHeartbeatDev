// main/couple_wifi_scan.h —— Wi-Fi 扫描结果整理（纯逻辑，不依赖 ESP-IDF，可主机测试）
//
// 设备把扫描到的 AP 交给 App 选择，手机屏幕较小、ATT 负载也有限，
// 因此这里负责：同名网络只保留最强的一条、按信号从强到弱排序、
// 以及按字节预算算出能回传多少条。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COUPLE_WIFI_SCAN_SSID_MAX 33
// 与 sdkconfig 的 BT_NIMBLE_ATT_PREFERRED_MTU=512 对齐：约能装下 13 条短 SSID
#define COUPLE_WIFI_SCAN_MAX 13

typedef struct {
    char ssid[COUPLE_WIFI_SCAN_SSID_MAX];
    int8_t rssi;
} couple_wifi_ap_t;

typedef struct {
    couple_wifi_ap_t aps[COUPLE_WIFI_SCAN_MAX];
    uint8_t count;
} couple_wifi_scan_list_t;

void couple_wifi_scan_clear(couple_wifi_scan_list_t *list);

// 并入一条结果。同名只保留更强的一条；列表始终按信号降序；满了则淘汰最弱的。
// 含控制字符的 SSID 会被拒绝（无法安全内联进 JSON 给 App）。
// 返回 true 表示列表发生了变化。
bool couple_wifi_scan_add(couple_wifi_scan_list_t *list, const char *ssid, int8_t rssi);

// SSID 按 JSON 转义（" 与 \）后的长度。
size_t couple_wifi_scan_escaped_len(const char *ssid);

// 单条结果序列化后的字节数（非首条含前导逗号，用于精确估算）。
size_t couple_wifi_scan_entry_bytes(const char *ssid, bool first);

// JSON 外壳 {"scanning":false,"aps":[]} 的字节数（不含任何条目）。
size_t couple_wifi_scan_envelope_bytes(void);

// 在 budget 字节内最多能放下多少条（budget 含 JSON 外壳）。
uint8_t couple_wifi_scan_fit(const couple_wifi_scan_list_t *list, size_t budget);
