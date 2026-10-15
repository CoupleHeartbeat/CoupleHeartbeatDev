// main/couple_gatt.h —— App 配对配置 NimBLE GATT Server
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// 公开 API
// ============================================================================

/**
 * @brief 初始化并启动 GATT 服务
 *
 * 注册配对配置服务、启动广播，设备名称为 "AI-Passport"。
 *
 * @return ESP_OK 成功，其他失败
 */
esp_err_t couple_gatt_init(void);

/**
 * @brief 停止 GATT 服务并释放资源
 *
 * @return ESP_OK 成功，其他失败
 */
esp_err_t couple_gatt_deinit(void);

/**
 * @brief 主动推送设备状态通知（0xFFE3 特征值）
 *
 * @param battery 电量百分比 (0-100)
 * @param mode    当前模式 (couple_mode_t)
 * @param volume  音量等级 (0-100)
 */
void couple_gatt_notify_status(uint8_t battery, uint8_t mode, uint8_t volume);

/**
 * @brief 主动推送 Wi-Fi 配网状态通知（0xFFE5 特征值）
 *
 * 无已连接客户端或 GATT 未启动时直接返回，不产生副作用。
 */
void couple_gatt_notify_wifi_status(void);

/**
 * @brief 获取当前配对码（4 位十进制，0000-9999）
 *
 * 未配对（或恢复出厂）设备生成随机配对码并显示在屏幕上，
 * App 配对写入必须携带相同配对码，否则被拒绝。
 */
uint16_t couple_gatt_get_pair_code(void);

/**
 * @brief 重新生成配对码（恢复出厂设置后调用）
 */
void couple_gatt_new_pair_code(void);

/**
 * @brief App 是否正通过 GATT 连接（低功耗决策用：连接中抑制深睡）
 */
bool couple_gatt_app_connected(void);

/**
 * @brief NimBLE host 是否已初始化成功
 *
 * couple_gatt_init 失败（如堆不足导致 hci inits failed）时返回 false。
 * 依赖 NimBLE host 的调用方（如心跳扫描）必须先查询本接口，
 * 避免对未初始化的 host 调用 API 触发 Load access fault。
 */
bool couple_gatt_ready(void);

#ifdef __cplusplus
}
#endif
