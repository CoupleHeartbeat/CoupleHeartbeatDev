// main/couple_userfs.c —— 用户数据分区挂载（FATFS + wear levelling）
#include "couple_userfs.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

static const char *TAG = "couple_userfs";

static wl_handle_t s_wl = WL_INVALID_HANDLE;
static bool s_mounted;

esp_err_t couple_userfs_init(void)
{
    if (s_mounted) return ESP_OK;

    // 首次烧写或分区损坏时自动格式化；分区在应用之后，正常刷机不会触发
    const esp_vfs_fat_mount_config_t cfg = {
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(
        COUPLE_USERFS_MOUNT, "user", &cfg, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "用户数据分区挂载失败(%s)，文件功能不可用",
                 esp_err_to_name(err));
        s_wl = WL_INVALID_HANDLE;
        return err;
    }
    s_mounted = true;

    uint64_t total = 0, free_bytes = 0;
    if (esp_vfs_fat_info(COUPLE_USERFS_MOUNT, &total, &free_bytes) == ESP_OK) {
        ESP_LOGI(TAG, "用户数据分区就绪: %s 总计 %llu KB 可用 %llu KB",
                 COUPLE_USERFS_MOUNT, total / 1024, free_bytes / 1024);
    } else {
        ESP_LOGI(TAG, "用户数据分区就绪: %s", COUPLE_USERFS_MOUNT);
    }
    return ESP_OK;
}

bool couple_userfs_mounted(void)
{
    return s_mounted;
}
