// main/couple_storage.c —— 情侣配对信息 NVS 存储管理
#include "couple_app.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "couple_storage";

#define STORAGE_NAMESPACE "couple_pair"

esp_err_t couple_storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS 分区被截断或版本不匹配，需要擦除
        ESP_LOGW(TAG, "NVS 需要擦除重试: %s", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    return err;
}

esp_err_t couple_storage_save_pair(const couple_pair_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败: %s", esp_err_to_name(err));
        return err;
    }

    // 写入各字段
    err = nvs_set_u8(handle, "paired", info->paired ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "role", (uint8_t)info->role);
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, "pair_id", info->pair_id, COUPLE_PAIR_ID_LEN);
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, "pair_key", info->pair_key, COUPLE_PAIR_KEY_LEN);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "badge_name", info->badge_name);
    }
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, "partner_addr", info->partner_addr, 6);
    }

    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "配对信息已保存");
    } else {
        ESP_LOGE(TAG, "配对信息保存失败: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t couple_storage_load_pair(couple_pair_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;

    memset(info, 0, sizeof(couple_pair_info_t));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 打开失败(首次运行): %s", esp_err_to_name(err));
        return err;
    }

    uint8_t paired = 0;
    size_t len;

    nvs_get_u8(handle, "paired", &paired);
    info->paired = (paired != 0);

    uint8_t role = 0;
    if (nvs_get_u8(handle, "role", &role) == ESP_OK) {
        info->role = (char)role;
    }

    len = COUPLE_PAIR_ID_LEN;
    nvs_get_blob(handle, "pair_id", info->pair_id, &len);

    len = COUPLE_PAIR_KEY_LEN;
    nvs_get_blob(handle, "pair_key", info->pair_key, &len);

    len = COUPLE_BADGE_NAME_LEN;
    nvs_get_str(handle, "badge_name", info->badge_name, &len);

    len = 6;
    nvs_get_blob(handle, "partner_addr", info->partner_addr, &len);

    nvs_close(handle);

    ESP_LOGI(TAG, "加载配对信息: paired=%d role=%c", info->paired, info->role);
    return ESP_OK;
}

esp_err_t couple_storage_clear_pair(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_erase_all(handle);
    err = nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "配对信息已清除");
    return err;
}

// ============================================================================
// 系统时间持久化
//
// 本板无 RTC 后备电池，断电即丢时间，所以把 App 对时结果落到 NVS：
// 开机先恢复到"上次已知时间"（至少日期不再跳回默认值），再由 App 重新对时校准。
// 与配对信息分开命名空间，避免"恢复出厂设置"把时间一起擦掉。
// ============================================================================
#define TIME_NAMESPACE "couple_time"

esp_err_t couple_storage_save_time(uint32_t unix_sec, int32_t tz_offset_sec)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(TIME_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "时间 NVS 打开失败: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u32(handle, "unix", unix_sec);
    if (err == ESP_OK) {
        err = nvs_set_i32(handle, "tz", tz_offset_sec);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t couple_storage_load_time(uint32_t *unix_sec, int32_t *tz_offset_sec)
{
    if (!unix_sec || !tz_offset_sec) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(TIME_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    err = nvs_get_u32(handle, "unix", unix_sec);
    if (err == ESP_OK) {
        if (nvs_get_i32(handle, "tz", tz_offset_sec) != ESP_OK) {
            *tz_offset_sec = 8 * 3600;  // 缺省东八区
        }
    }
    nvs_close(handle);
    return err;
}
