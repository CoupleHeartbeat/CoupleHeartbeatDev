// main/couple_wifi_scan.c —— Wi-Fi 扫描结果整理的纯逻辑实现
#include "couple_wifi_scan.h"

#include <stdio.h>
#include <string.h>

void couple_wifi_scan_clear(couple_wifi_scan_list_t *list)
{
    if (!list) return;
    memset(list, 0, sizeof(*list));
}

size_t couple_wifi_scan_escaped_len(const char *ssid)
{
    size_t n = 0;
    for (const char *p = ssid; p && *p; p++) {
        n += (*p == '"' || *p == '\\') ? 2 : 1;
    }
    return n;
}

size_t couple_wifi_scan_entry_bytes(const char *ssid, bool first)
{
    // {"ssid":" =10 + 转义后的 ssid + ","rssi": =9 + 最多 4 位含负号 + } =1 + 前导逗号
    return 10 + couple_wifi_scan_escaped_len(ssid) + 9 + 4 + 1 + (first ? 0 : 1);
}

bool couple_wifi_scan_add(couple_wifi_scan_list_t *list, const char *ssid, int8_t rssi)
{
    if (!list || !ssid || ssid[0] == '\0') return false;
    if (strlen(ssid) >= COUPLE_WIFI_SCAN_SSID_MAX) return false;

    // 控制字符只转义 " 和 \ 是不够的，这类 SSID 直接不收
    for (const char *p = ssid; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7F) return false;
    }

    uint8_t idx = list->count;
    for (uint8_t i = 0; i < list->count; i++) {
        if (strcmp(list->aps[i].ssid, ssid) == 0) {
            idx = i;
            break;
        }
    }

    if (idx == list->count) {
        if (list->count == COUPLE_WIFI_SCAN_MAX) {
            // 已满：只有强过当前最弱的一条才值得挤掉它
            if (rssi <= list->aps[list->count - 1].rssi) return false;
            idx = list->count - 1;
        } else {
            list->count++;
        }
        snprintf(list->aps[idx].ssid, sizeof(list->aps[idx].ssid), "%s", ssid);
        list->aps[idx].rssi = rssi;
    } else if (rssi <= list->aps[idx].rssi) {
        return false;   // 同名已有更强的一条
    } else {
        list->aps[idx].rssi = rssi;
    }

    // 冒泡到降序位置（列表本身始终有序，最坏移动一格到几格）
    while (idx > 0 && list->aps[idx].rssi > list->aps[idx - 1].rssi) {
        couple_wifi_ap_t tmp = list->aps[idx - 1];
        list->aps[idx - 1] = list->aps[idx];
        list->aps[idx] = tmp;
        idx--;
    }
    return true;
}

#define SCAN_ENVELOPE "{\"scanning\":false,\"aps\":[]}"

size_t couple_wifi_scan_envelope_bytes(void)
{
    return sizeof(SCAN_ENVELOPE) - 1;
}

uint8_t couple_wifi_scan_fit(const couple_wifi_scan_list_t *list, size_t budget)
{
    if (!list) return 0;

    size_t used = couple_wifi_scan_envelope_bytes();
    uint8_t fit = 0;
    for (uint8_t i = 0; i < list->count; i++) {
        size_t n = couple_wifi_scan_entry_bytes(list->aps[i].ssid, fit == 0);
        if (used + n > budget) break;
        used += n;
        fit++;
    }
    return fit;
}
