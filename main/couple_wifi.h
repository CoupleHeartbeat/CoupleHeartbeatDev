// main/couple_wifi.h —— Wi-Fi STA 配网（App 经 BLE 下发凭据）
//
// 设备侧只负责：保存凭据、以 STA 身份连接、把连接状态回给 App。
// 凭据本体（SSID/密码）只在设备与手机之间经 BLE 传输，不写入日志。
//
// 线程约定：request_* 接口只做校验与入队，可在 GATT 回调中安全调用；
// 真正的 Wi-Fi 初始化与 NVS 提交由模块内部工作任务串行执行。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define COUPLE_WIFI_SSID_MAX 33   // 32 字节 SSID + '\0'
#define COUPLE_WIFI_PASS_MAX 65   // 64 字节密码 + '\0'

typedef enum {
    COUPLE_WIFI_STATE_IDLE = 0,     // 未配置
    COUPLE_WIFI_STATE_CONNECTING,   // 连接中（含重试）
    COUPLE_WIFI_STATE_CONNECTED,    // 已连接并取得 IP
    COUPLE_WIFI_STATE_FAILED,       // 重试耗尽
} couple_wifi_state_t;

// 准备 NVS / netif / 默认事件循环，并创建配网工作任务。
// 不启动 Wi-Fi 射频：未配网的设备不占用射频与内存。
esp_err_t couple_wifi_init(void);

// 如已保存凭据则发起连接（异步）；无凭据返回 ESP_ERR_NOT_FOUND。
esp_err_t couple_wifi_autoconnect(void);

// 下发凭据并连接（异步）。ssid 不能为空；password 传 NULL 或空串表示开放网络。
// 参数非法返回 ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_SIZE。
esp_err_t couple_wifi_request_credentials(const char *ssid, const char *password);

// 清除已保存凭据并断开（异步）。
esp_err_t couple_wifi_request_clear(void);

couple_wifi_state_t couple_wifi_get_state(void);
bool couple_wifi_is_configured(void);
const char *couple_wifi_get_ssid(void);   // 已保存的 SSID，未配置返回 ""
const char *couple_wifi_get_ip(void);     // 未连接返回 ""
int couple_wifi_get_rssi(void);           // 未连接返回 0
