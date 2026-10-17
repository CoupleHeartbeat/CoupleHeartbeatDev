// main/couple_app.h —— FoloToy AI Passport 情侣互动应用统一头文件
#pragma once

#include "bsp_button.h"
#include <stdbool.h>
#include <stdint.h>

// ============================================================================
// 应用模式
// ============================================================================
// 靠近不再单独成一个模式：对方在附近只是工牌页上的边缘光效（见 couple_auto.h）。
typedef enum {
    COUPLE_MODE_BADGE = 0,       // 工牌模式：时间 + 姓名 + 电量 + 靠近光效
    COUPLE_MODE_AI,              // AI 对话：长按 OK 进入，语音问答
} couple_mode_t;

// ============================================================================
// 配对信息（存储于 NVS）
// ============================================================================
#define COUPLE_PAIR_ID_LEN     8
#define COUPLE_PAIR_KEY_LEN    16
#define COUPLE_BADGE_NAME_LEN  20

typedef struct {
    bool paired;                              // 是否已配对
    char role;                                // 'A' 或 'B'
    uint8_t pair_id[COUPLE_PAIR_ID_LEN];      // 配对会话 ID
    uint8_t pair_key[COUPLE_PAIR_KEY_LEN];    // 预共享密钥
    char badge_name[COUPLE_BADGE_NAME_LEN];   // 工牌显示名称
    uint8_t partner_addr[6];                  // 对方 BLE MAC 地址
} couple_pair_info_t;

// ============================================================================
// RSSI 距离等级
// ============================================================================
typedef enum {
    COUPLE_DISTANCE_FAR = 0,     // > 5m，离开
    COUPLE_DISTANCE_NORMAL,      // 2~5m，感知
    COUPLE_DISTANCE_CLOSE,       // 0.5~2m，接近
    COUPLE_DISTANCE_VERY_CLOSE,  // < 0.5m，极近（贴贴候选）
} couple_distance_t;

// ============================================================================
// 全局状态（各模块共享）
// ============================================================================
typedef struct {
    couple_mode_t mode;
    couple_pair_info_t pair_info;
    int8_t partner_rssi;            // 对方设备最新 RSSI (dBm)
    bool partner_connected;         // 是否检测到对方心跳
    couple_distance_t distance;     // 估算距离等级
    uint32_t last_heartbeat_ms;     // 上次收到心跳的时间
    uint8_t volume;                 // 主音量 0-100
    bool muted;                     // 静音模式
} couple_state_t;

extern couple_state_t g_couple_state;

// ============================================================================
// 模式管理（couple_mode_manager.c）
// ============================================================================
void couple_mode_init(void);
void couple_mode_switch(couple_mode_t new_mode);
couple_mode_t couple_mode_get(void);

// ============================================================================
// 工牌模式（couple_badge.c）
// ============================================================================
void couple_badge_enter(void);
void couple_badge_exit(void);
void couple_badge_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void couple_badge_update_time(void);
// 低功耗屏：只显示姓名(大字) + 时间 + 年月日，状态栏/提示/靠近光效全部隐藏。
// 息屏（低功耗）时置 true，唤醒时置 false。必须在持有 LVGL 锁时调用。
void couple_badge_set_minimal(bool minimal);

// ============================================================================
// BLE 心跳通信（couple_ble.c）
// ============================================================================
#define COUPLE_BLE_MFG_DATA_LEN 22   // 心跳制造商数据长度

esp_err_t couple_ble_start(void);
esp_err_t couple_ble_stop(void);
bool couple_ble_is_active(void);
int8_t couple_ble_get_rssi(void);
// 连续极近计数：工牌页据此判定是否进入“贴贴”档光效
uint8_t couple_ble_very_close_count(void);
void couple_ble_tick(void);
// 生成心跳制造商数据，交由 couple_gatt.c 拼入广播；返回写入长度，未配对时为 0
uint8_t couple_ble_build_mfg(uint8_t *buf, uint8_t buf_len);

// ============================================================================
// 加密模块（couple_crypto.c）
// ============================================================================
esp_err_t couple_crypto_init(void);
esp_err_t couple_crypto_encrypt(const uint8_t *key, const uint8_t *nonce,
                                const uint8_t *plaintext, uint8_t len,
                                uint8_t *ciphertext);
esp_err_t couple_crypto_decrypt(const uint8_t *key, const uint8_t *nonce,
                                const uint8_t *ciphertext, uint8_t len,
                                uint8_t *plaintext);

// ============================================================================
// 存储管理（couple_storage.c）
// ============================================================================
esp_err_t couple_storage_init(void);
esp_err_t couple_storage_save_pair(const couple_pair_info_t *info);
esp_err_t couple_storage_load_pair(couple_pair_info_t *info);
esp_err_t couple_storage_clear_pair(void);

// ============================================================================
// 系统时钟（couple_storage.c + main.c）
// ============================================================================
// 系统时间持久化：本板无 RTC 后备电池，断电即丢时间。App 每次对时后写入 NVS，
// 开机时先恢复上次已知时间，再由 App 重新对时校准。
esp_err_t couple_storage_save_time(uint32_t unix_sec, int32_t tz_offset_sec);
esp_err_t couple_storage_load_time(uint32_t *unix_sec, int32_t *tz_offset_sec);

// 把 UTC 秒与时区偏移写入系统时钟（settimeofday + 重建 POSIX TZ）。
// 实现见 main.c；GATT 时间同步与开机恢复时间共用。
void couple_app_apply_time(long long unix_sec, long tz_offset_sec);

// ============================================================================
// GATT 配置服务（couple_gatt.c）
// ============================================================================
esp_err_t couple_gatt_init(void);
esp_err_t couple_gatt_deinit(void);
void couple_gatt_notify_status(uint8_t battery, uint8_t mode, uint8_t volume);

// ============================================================================
// 音频心跳音效（couple_audio.c）
// ============================================================================
void couple_audio_init(void);
void couple_audio_set_volume(uint8_t vol);
uint8_t couple_audio_get_volume(void);
void couple_audio_play_heartbeat(uint8_t distance_level);
void couple_audio_stop(void);

// ============================================================================
// 工具函数
// ============================================================================
uint32_t couple_now_ms(void);
