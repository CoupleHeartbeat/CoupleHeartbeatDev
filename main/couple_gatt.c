// main/couple_gatt.c —— App 配对配置 NimBLE GATT Server
#include "couple_gatt.h"
#include "couple_app.h"
#include "couple_ai.h"
#include "couple_time.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "demo_radio.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_att.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_bt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "couple_gatt";

// ============================================================================
// UUID 定义
// ============================================================================
// 服务: 0000ffe0-0000-1000-8000-00805f9b34fb (16-bit 简写: 0xFFE0)
// 特征值 0xFFE1: 配对配置 (Write)
// 特征值 0xFFE2: 工牌信息 (Read/Write)
// 特征值 0xFFE3: 设备状态 (Read/Notify)
// 特征值 0xFFE8: 时间同步 (Write)：写 {"unix":秒,"tz_offset":秒} 校准系统时间
// 特征值 0xFFE9: 语音上行 (Notify)：设备 -> 手机，见 couple_voice.h
// 特征值 0xFFEA: 回复下行 (Write)：手机 -> 设备，[status][UTF-8 文本]
// ============================================================================

#define GATT_SVC_UUID          0xFFE0
#define GATT_CHR_PAIR_CFG_UUID 0xFFE1
#define GATT_CHR_BADGE_UUID    0xFFE2
#define GATT_CHR_STATUS_UUID   0xFFE3
#define GATT_CHR_TIME_UUID     0xFFE8
#define GATT_CHR_VOICE_UP_UUID 0xFFE9
#define GATT_CHR_VOICE_REPLY_UUID 0xFFEA

#define COUPLE_GATT_DEV_NAME_PREFIX "AI-Passport"
#define COUPLE_GATT_DEV_NAME_MAX    20
#define COUPLE_GATT_ADV_ITVL   32  // 32 * 0.625ms = 20ms，兼顾心跳采样率

// ============================================================================
// 静态状态
// ============================================================================
static bool s_initialized;
static bool s_running;
static TaskHandle_t s_host_task;
static SemaphoreHandle_t s_host_stopped;
static uint8_t s_addr_type;
static volatile bool s_stop_in_progress;
static volatile bool s_host_done;

// 当前连接句柄（用于通知）
static uint16_t s_conn_handle;
static bool s_connected;
// App 是否已订阅语音上行通知（0xFFE9）
static bool s_voice_subscribed;

// 状态特征值句柄（用于通知）
static uint16_t s_status_val_handle;
// 语音上行特征值句柄（用于通知）
static uint16_t s_voice_val_handle;

// 缓存的工牌名称（供读取使用）
static char s_badge_name_cache[COUPLE_BADGE_NAME_LEN];

// 设备蓝牙名：前缀 + MAC 后两字节，保证每台设备唯一（如 AI-Passport-A1B2）
static char s_dev_name[COUPLE_GATT_DEV_NAME_MAX];

static void build_dev_name(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_dev_name, sizeof(s_dev_name),
             COUPLE_GATT_DEV_NAME_PREFIX "-%02X%02X", mac[4], mac[5]);
    ESP_LOGI(TAG, "本机蓝牙名: %s", s_dev_name);
}

// 配对码：未配对设备显示于屏幕，App 配对写入须携带相同配对码
static uint16_t s_pair_code;

static void generate_pair_code(void)
{
    s_pair_code = (uint16_t)(esp_random() % 10000);
    ESP_LOGI(TAG, "配对码: %04u", s_pair_code);
}

uint16_t couple_gatt_get_pair_code(void)
{
    return s_pair_code;
}

void couple_gatt_new_pair_code(void)
{
    generate_pair_code();
}

bool couple_gatt_app_connected(void)
{
    return s_connected;
}

bool couple_gatt_ready(void)
{
    return s_initialized;
}

// ============================================================================
// 前向声明
// ============================================================================
static int gap_event_cb(struct ble_gap_event *event, void *arg);
static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg);

// ============================================================================
// GATT 服务定义
// ============================================================================
static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(GATT_SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // 0xFFE1 —— 配对配置（Write）
                .uuid = BLE_UUID16_DECLARE(GATT_CHR_PAIR_CFG_UUID),
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                // 0xFFE2 —— 工牌信息（Read/Write）
                .uuid = BLE_UUID16_DECLARE(GATT_CHR_BADGE_UUID),
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {
                // 0xFFE3 —— 设备状态（Read/Notify）
                // Read：App 连接后按需读取真实电量/模式/音量，无需维持长连接
                .uuid = BLE_UUID16_DECLARE(GATT_CHR_STATUS_UUID),
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_status_val_handle,
            },
            {
                // 0xFFE8 —— 时间同步（Write）
                // 写 {"unix":UTC秒,"tz_offset":时区偏移秒} 校准系统时间
                .uuid = BLE_UUID16_DECLARE(GATT_CHR_TIME_UUID),
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                // 0xFFE9 —— 语音上行（Notify）
                // 6 字节头 + PCM，分包规则见 couple_voice.h
                .uuid = BLE_UUID16_DECLARE(GATT_CHR_VOICE_UP_UUID),
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_voice_val_handle,
            },
            {
                // 0xFFEA —— 回复下行（Write）
                // [0]=status，[1..]=UTF-8 文本；回调只做拷贝入队
                .uuid = BLE_UUID16_DECLARE(GATT_CHR_VOICE_REPLY_UUID),
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            { 0 },  // 特征值列表结束
        },
    },
    { 0 },  // 服务列表结束
};

// ============================================================================
// MAC 地址解析（格式: xx:xx:xx:xx:xx:xx）
// ============================================================================
static bool parse_mac_addr(const char *str, uint8_t addr[6])
{
    if (!str || !addr) return false;

    int vals[6];
    int n = sscanf(str, "%x:%x:%x:%x:%x:%x",
                   &vals[0], &vals[1], &vals[2],
                   &vals[3], &vals[4], &vals[5]);
    if (n != 6) return false;

    for (int i = 0; i < 6; i++) {
        addr[i] = (uint8_t)vals[i];
    }
    return true;
}

// ============================================================================
// 解析配对配置 JSON
// ============================================================================
static esp_err_t parse_pair_config(const uint8_t *data, uint16_t len,
                                   couple_pair_info_t *info)
{
    if (!data || !info) return ESP_ERR_INVALID_ARG;

    // 确保数据以 null 结尾
    char json_buf[256];
    if (len >= sizeof(json_buf)) return ESP_ERR_NO_MEM;
    memcpy(json_buf, data, len);
    json_buf[len] = '\0';

    cJSON *root = cJSON_Parse(json_buf);
    if (!root) {
        ESP_LOGE(TAG, "配对配置 JSON 解析失败");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;
    memset(info, 0, sizeof(couple_pair_info_t));

    // role
    cJSON *role_item = cJSON_GetObjectItem(root, "role");
    if (cJSON_IsString(role_item) && role_item->valuestring[0] != '\0') {
        info->role = role_item->valuestring[0];
    } else {
        ESP_LOGW(TAG, "缺少 role 字段");
    }

    // pair_id (十六进制字符串)
    cJSON *pair_id_item = cJSON_GetObjectItem(root, "pair_id");
    if (cJSON_IsString(pair_id_item) && pair_id_item->valuestring) {
        const char *pid_str = pair_id_item->valuestring;
        size_t pid_len = strlen(pid_str);
        size_t expect_len = COUPLE_PAIR_ID_LEN * 2;
        if (pid_len >= expect_len) {
            for (int i = 0; i < COUPLE_PAIR_ID_LEN; i++) {
                char byte_str[3] = { pid_str[i * 2], pid_str[i * 2 + 1], '\0' };
                info->pair_id[i] = (uint8_t)strtol(byte_str, NULL, 16);
            }
        }
    }

    // pair_key (十六进制字符串)
    cJSON *pair_key_item = cJSON_GetObjectItem(root, "pair_key");
    if (cJSON_IsString(pair_key_item) && pair_key_item->valuestring) {
        const char *pk_str = pair_key_item->valuestring;
        size_t pk_len = strlen(pk_str);
        size_t expect_len = COUPLE_PAIR_KEY_LEN * 2;
        if (pk_len >= expect_len) {
            for (int i = 0; i < COUPLE_PAIR_KEY_LEN; i++) {
                char byte_str[3] = { pk_str[i * 2], pk_str[i * 2 + 1], '\0' };
                info->pair_key[i] = (uint8_t)strtol(byte_str, NULL, 16);
            }
        }
    }

    // partner_addr (MAC 地址)
    cJSON *partner_item = cJSON_GetObjectItem(root, "partner_addr");
    if (cJSON_IsString(partner_item) && partner_item->valuestring) {
        parse_mac_addr(partner_item->valuestring, info->partner_addr);
    }

    // badge_name
    cJSON *badge_item = cJSON_GetObjectItem(root, "badge_name");
    if (cJSON_IsString(badge_item) && badge_item->valuestring) {
        strncpy(info->badge_name, badge_item->valuestring,
                COUPLE_BADGE_NAME_LEN - 1);
    }

    // 标记为已配对
    // 配对码校验：必须与设备屏幕显示的 4 位随机码一致，否则拒绝配对
    bool code_ok = false;
    cJSON *code_item = cJSON_GetObjectItem(root, "pair_code");
    if (cJSON_IsNumber(code_item)) {
        code_ok = ((uint16_t)code_item->valueint == s_pair_code);
    } else if (cJSON_IsString(code_item) && code_item->valuestring) {
        code_ok = ((uint16_t)atoi(code_item->valuestring) == s_pair_code);
    }
    if (!code_ok) {
        ESP_LOGW(TAG, "配对码校验失败，拒绝配对");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }

    info->paired = true;

    cJSON_Delete(root);
    return err;
}

// ============================================================================
// 构造工牌信息 JSON
// ============================================================================
static int build_badge_info_json(struct os_mbuf *om)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return BLE_ATT_ERR_INSUFFICIENT_RES;

    cJSON_AddStringToObject(root, "badge_name", s_badge_name_cache);
    cJSON_AddBoolToObject(root, "paired", g_couple_state.pair_info.paired);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) return BLE_ATT_ERR_INSUFFICIENT_RES;

    int rc = os_mbuf_append(om, json_str, strlen(json_str));
    cJSON_free(json_str);

    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// ============================================================================
// 构造设备状态 JSON（供 App 读取真实状态）
// ============================================================================
static int build_status_json(struct os_mbuf *om)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return BLE_ATT_ERR_INSUFFICIENT_RES;

    int soc = bsp_battery_soc();
    if (soc < 0) soc = 0;
    if (soc > 100) soc = 100;

    cJSON_AddNumberToObject(root, "battery", soc);
    cJSON_AddNumberToObject(root, "mode", (double)g_couple_state.mode);
    cJSON_AddNumberToObject(root, "volume", (double)g_couple_state.volume);
    cJSON_AddBoolToObject(root, "paired", g_couple_state.pair_info.paired);
    cJSON_AddNumberToObject(root, "rssi", (double)g_couple_state.partner_rssi);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return BLE_ATT_ERR_INSUFFICIENT_RES;

    int rc = os_mbuf_append(om, json_str, strlen(json_str));
    cJSON_free(json_str);

    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// ============================================================================
// 解析工牌名称 JSON 写入
// ============================================================================
static esp_err_t parse_badge_name(const uint8_t *data, uint16_t len,
                                  char *name_buf, size_t name_buf_len)
{
    if (!data || !name_buf) return ESP_ERR_INVALID_ARG;

    char json_buf[128];
    if (len >= sizeof(json_buf)) return ESP_ERR_NO_MEM;
    memcpy(json_buf, data, len);
    json_buf[len] = '\0';

    cJSON *root = cJSON_Parse(json_buf);
    if (!root) {
        ESP_LOGE(TAG, "工牌信息 JSON 解析失败");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_ERR_INVALID_ARG;
    cJSON *item = cJSON_GetObjectItem(root, "badge_name");
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(name_buf, item->valuestring, name_buf_len - 1);
        name_buf[name_buf_len - 1] = '\0';
        err = ESP_OK;
    }

    cJSON_Delete(root);
    return err;
}

// ============================================================================
// GATT 特征值访问回调
// ============================================================================
static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    uint16_t uuid16 = ble_uuid_u16(ctxt->chr->uuid);

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        if (uuid16 == GATT_CHR_BADGE_UUID) {
            // 读取工牌信息
            return build_badge_info_json(ctxt->om);
        }
        if (uuid16 == GATT_CHR_STATUS_UUID) {
            // 读取设备状态（电量/模式/音量）
            return build_status_json(ctxt->om);
        }
        return BLE_ATT_ERR_UNLIKELY;

    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        // 获取写入数据
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

        // 回复文本最长 400 字节，MTU 512 时单包最大 509 字节
        uint8_t data[512];
        if (len > sizeof(data)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

        int rc = os_mbuf_copydata(ctxt->om, 0, len, data);
        if (rc != 0) return BLE_ATT_ERR_UNLIKELY;

        if (uuid16 == GATT_CHR_PAIR_CFG_UUID) {
            // 已配对设备拒绝任何配对写入，重新配对必须先在设备上恢复出厂
            if (g_couple_state.pair_info.paired) {
                ESP_LOGW(TAG, "设备已配对，拒绝配对写入");
                return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
            }
            // 写入配对配置
            couple_pair_info_t info;
            esp_err_t perr = parse_pair_config(data, len, &info);
            if (perr == ESP_ERR_INVALID_STATE) {
                // 配对码错误
                return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
            }
            if (perr == ESP_OK) {
                esp_err_t err = couple_storage_save_pair(&info);
                if (err == ESP_OK) {
                    // 更新缓存
                    memcpy(s_badge_name_cache, info.badge_name,
                           COUPLE_BADGE_NAME_LEN);
                    // 更新全局状态
                    memcpy(&g_couple_state.pair_info, &info,
                           sizeof(couple_pair_info_t));
                    ESP_LOGI(TAG, "配对配置已保存: role=%c", info.role);
                    // 诊断：host 任务栈余量，用于确认回调内 JSON+NVS 未逼近栈上限
                    ESP_LOGI(TAG, "NimBLE host 任务剩余栈: %u 字节",
                             (unsigned)uxTaskGetStackHighWaterMark(NULL));
                    return 0;
                }
            }
            return BLE_ATT_ERR_UNLIKELY;

        } else if (uuid16 == GATT_CHR_BADGE_UUID) {
            // 工牌名称允许在未配对时修改：单台设备的用户只买一台，
            // 也需要设置昵称（配对流程则通过 0xFFE1 一并下发名称）。
            char new_name[COUPLE_BADGE_NAME_LEN] = {0};
            if (parse_badge_name(data, len, new_name, sizeof(new_name)) == ESP_OK) {
                // 更新缓存
                memcpy(s_badge_name_cache, new_name, COUPLE_BADGE_NAME_LEN);
                // 同步到存储
                couple_pair_info_t info;
                memset(&info, 0, sizeof(info));
                if (couple_storage_load_pair(&info) != ESP_OK) {
                    info.paired = g_couple_state.pair_info.paired;
                    info.role = g_couple_state.pair_info.role;
                }
                memcpy(info.badge_name, new_name, COUPLE_BADGE_NAME_LEN);
                couple_storage_save_pair(&info);
                // 更新全局状态
                memcpy(g_couple_state.pair_info.badge_name, new_name,
                       COUPLE_BADGE_NAME_LEN);
                ESP_LOGI(TAG, "工牌名称已更新: %s", new_name);
                return 0;
            }
            return BLE_ATT_ERR_UNLIKELY;

        } else if (uuid16 == GATT_CHR_VOICE_REPLY_UUID) {
            // 手机回传的回复：[0]=status，[1..]=UTF-8 文本。
            // 回调运行在 NimBLE host 任务，只做拷贝入队，解析交给 AI 任务。
            if (len < 2) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            couple_ai_on_remote_reply(data[0], (const char *)data + 1,
                                      (size_t)len - 1);
            return 0;

        } else if (uuid16 == GATT_CHR_TIME_UUID) {
            // 时间同步：{"unix":UTC秒,"tz_offset":时区偏移秒}
            char json_buf[64];
            if (len >= sizeof(json_buf)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            memcpy(json_buf, data, len);
            json_buf[len] = '\0';

            long long unix_sec = 0;
            long long tz_off = 0;
            couple_time_parse_result_t pr =
                couple_time_parse(json_buf, len, &unix_sec, &tz_off);
            if (pr != COUPLE_TIME_PARSE_OK) {
                ESP_LOGW(TAG, "时间同步解析失败: %d", (int)pr);
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }

            // 1) 写入系统时钟（settimeofday + 按 tz_offset 重建 POSIX TZ）
            couple_app_apply_time(unix_sec, (long)tz_off);

            // 2) 落盘：本板无 RTC 后备电池，断电后靠它恢复上次已知时间，
            //    避免日期跳回出厂默认值
            couple_storage_save_time((uint32_t)unix_sec, (int32_t)tz_off);

            // 3) 刷新工牌页时间显示（GATT 回调在 NimBLE host 任务，需加锁）
            if (bsp_lvgl_lock(500)) {
                couple_badge_update_time();
                bsp_lvgl_unlock();
            }

            ESP_LOGI(TAG, "时间已同步: unix=%lld tz=%lds", unix_sec, (long)tz_off);
            return 0;
        }

        return BLE_ATT_ERR_UNLIKELY;
    }

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

// ============================================================================
// 构造广播数据（flags + 心跳制造商数据）与扫描响应（服务 UUID + 设备名）
//
// 心跳制造商数据 22 字节 + flags 3 字节已占 25 字节，31 字节的广播包放不下
// 16 位 UUID 与设备名，因此二者放入扫描响应；App 使用主动扫描可同时收到
// 广播与扫描响应，配对设备之间用被动扫描只取广播中的心跳数据。
// ============================================================================
static const ble_uuid16_t s_svc_uuid = BLE_UUID16_INIT(GATT_SVC_UUID);
static uint8_t s_adv_mfg[COUPLE_BLE_MFG_DATA_LEN];

static int build_advertisement(struct ble_hs_adv_fields *fields)
{
    uint8_t mfg_len = couple_ble_build_mfg(s_adv_mfg, sizeof(s_adv_mfg));

    memset(fields, 0, sizeof(*fields));
    fields->flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    if (mfg_len > 0) {
        fields->mfg_data = s_adv_mfg;
        fields->mfg_data_len = mfg_len;
    }

    return 0;
}

static int build_scan_response(struct ble_hs_adv_fields *fields)
{
    memset(fields, 0, sizeof(*fields));
    fields->uuids16 = (ble_uuid16_t *)&s_svc_uuid;
    fields->num_uuids16 = 1;
    fields->uuids16_is_complete = 1;
    fields->name = (const uint8_t *)s_dev_name;
    fields->name_len = strlen(s_dev_name);
    fields->name_is_complete = 1;

    return 0;
}

// ============================================================================
// 启动广播
// ============================================================================
static int start_advertising(void)
{
    struct ble_hs_adv_fields fields;
    build_advertisement(&fields);

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置广播字段失败: %d", rc);
        return rc;
    }

    struct ble_hs_adv_fields rsp;
    build_scan_response(&rsp);
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGW(TAG, "设置扫描响应失败: %d", rc);
    }

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;  // 可连接
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;  // 通用可发现
    adv_params.itvl_min = COUPLE_GATT_ADV_ITVL;
    adv_params.itvl_max = COUPLE_GATT_ADV_ITVL + 16;

    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "启动广播失败: %d", rc);
    }
    return rc;
}

// ============================================================================
// GAP 事件回调
// ============================================================================
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_connected = true;
            ESP_LOGI(TAG, "GATT 客户端已连接, handle=%d", s_conn_handle);
        } else {
            ESP_LOGE(TAG, "连接失败: %d", event->connect.status);
            // 连接失败，重新广播
            if (s_running) {
                start_advertising();
            }
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        s_conn_handle = 0;
        s_voice_subscribed = false;
        ESP_LOGI(TAG, "GATT 客户端已断开, reason=%d",
                 event->disconnect.reason);
        // 断开后重新广播
        if (s_running) {
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_running && !s_connected) {
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_NOTIFY_TX:
        // 通知发送完成，可在此处处理
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_voice_val_handle) {
            s_voice_subscribed = event->subscribe.cur_notify != 0;
        }
        ESP_LOGI(TAG, "客户端订阅状态变化: attr_handle=%d, cur_notify=%d",
                 event->subscribe.attr_handle,
                 event->subscribe.cur_notify);
        break;

    default:
        break;
    }

    return 0;
}

// ============================================================================
// NimBLE 同步回调
// ============================================================================
static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &s_addr_type);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE 地址获取失败: %d", rc);
        return;
    }

    uint8_t addr[6];
    ble_hs_id_copy_addr(s_addr_type, addr, NULL);
    ESP_LOGI(TAG, "BLE 地址: %02X:%02X:%02X:%02X:%02X:%02X",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    if (s_running) {
        start_advertising();
    }
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset: %d", reason);
}

// ============================================================================
// Host 任务
// ============================================================================
static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    xSemaphoreGive(s_host_stopped);
    for (;;) vTaskSuspend(NULL);
}

// ============================================================================
// 公开 API
// ============================================================================

esp_err_t couple_gatt_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "GATT 服务已启动");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "初始化 GATT 配对配置服务");

    // 生成本次开机的配对码（未配对时显示在屏幕供 App 输入）
    generate_pair_code();

    s_running = true;
    s_stop_in_progress = false;
    s_host_done = false;
    s_connected = false;
    s_voice_subscribed = false;
    s_conn_handle = 0;
    s_status_val_handle = 0;
    s_voice_val_handle = 0;

    // 初始化工牌名称缓存
    memset(s_badge_name_cache, 0, sizeof(s_badge_name_cache));
    // 尝试从存储加载
    couple_pair_info_t info;
    if (couple_storage_load_pair(&info) == ESP_OK) {
        memcpy(s_badge_name_cache, info.badge_name, COUPLE_BADGE_NAME_LEN);
    }

    esp_err_t err = demo_radio_nvs_prepare();
    if (err != ESP_OK) {
        s_running = false;
        return err;
    }

    // MSYS 缓冲池（MSYS_1+MSYS_2 约 11KB）要求连续堆，init 失败时
    // 打出空闲堆与最大连续块，用于定位内存时序问题。
    ESP_LOGI(TAG, "NimBLE init 前空闲堆: %u 字节, 最大连续块: %u 字节",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    err = nimble_port_init();
    if (err != ESP_OK) {
        // 失败时打出堆状态与 controller 状态：区分"堆碎片导致大单笔分配失败"
        // 与"controller 未启用"两类根因，供下一轮修复定位。
        ESP_LOGE(TAG, "NimBLE 初始化失败: %s (空闲堆 %u, 最大连续块 %u, controller=%d)",
                 esp_err_to_name(err),
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (int)esp_bt_controller_get_status());
        s_running = false;
        return err;
    }

    s_host_stopped = xSemaphoreCreateBinary();
    if (!s_host_stopped) {
        nimble_port_deinit();
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    // 生成唯一蓝牙名（必须在 NimBLE 启动前）
    build_dev_name();

    // 初始化 GAP/GATT 服务
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(s_dev_name);

    // 注册自定义 GATT 服务
    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg 失败: %d", rc);
        vSemaphoreDelete(s_host_stopped);
        s_host_stopped = NULL;
        nimble_port_deinit();
        s_running = false;
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs 失败: %d", rc);
        vSemaphoreDelete(s_host_stopped);
        s_host_stopped = NULL;
        nimble_port_deinit();
        s_running = false;
        return ESP_FAIL;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    if (xTaskCreatePinnedToCore(host_task, "couple_gatt_host",
                                NIMBLE_HS_STACK_SIZE, NULL,
                                configMAX_PRIORITIES - 4, &s_host_task,
                                NIMBLE_CORE) != pdPASS) {
        vSemaphoreDelete(s_host_stopped);
        s_host_stopped = NULL;
        nimble_port_deinit();
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "GATT 服务已启动，设备名称: %s", s_dev_name);
    return ESP_OK;
}

esp_err_t couple_gatt_deinit(void)
{
    if (!s_initialized) return ESP_OK;

    ESP_LOGI(TAG, "停止 GATT 配对配置服务");
    s_running = false;
    s_connected = false;

    if (s_host_task && !s_stop_in_progress) {
        ble_gap_adv_stop();
        if (s_conn_handle != 0) {
            ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        int rc = nimble_port_stop();
        if (rc != 0) {
            ESP_LOGE(TAG, "nimble_port_stop 失败: %d", rc);
        }
        s_stop_in_progress = true;
    }

    if (s_host_task && !s_host_done) {
        if (!s_host_stopped ||
            xSemaphoreTake(s_host_stopped, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGE(TAG, "等待 NimBLE host 停止超时");
        }
        s_host_done = true;
    }

    if (s_host_task) {
        vTaskDelete(s_host_task);
        s_host_task = NULL;
    }

    esp_err_t err = nimble_port_deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_deinit 失败: %s", esp_err_to_name(err));
    }

    s_initialized = false;
    s_stop_in_progress = false;
    s_host_done = false;
    s_conn_handle = 0;
    s_voice_val_handle = 0;
    s_voice_subscribed = false;

    if (s_host_stopped) {
        vSemaphoreDelete(s_host_stopped);
        s_host_stopped = NULL;
    }

    return err;
}

void couple_gatt_notify_status(uint8_t battery, uint8_t mode, uint8_t volume)
{
    if (!s_initialized || !s_connected || s_status_val_handle == 0) {
        return;
    }

    // 构造 JSON: {"battery":xx,"mode":xx,"volume":xx}
    char json_buf[64];
    int len = snprintf(json_buf, sizeof(json_buf),
                       "{\"battery\":%u,\"mode\":%u,\"volume\":%u}",
                       battery, mode, volume);
    if (len <= 0 || len >= (int)sizeof(json_buf)) {
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(json_buf, len);
    if (!om) {
        ESP_LOGE(TAG, "分配通知 mbuf 失败");
        return;
    }

    int rc = ble_gatts_notify_custom(s_conn_handle, s_status_val_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "发送状态通知失败: %d", rc);
        os_mbuf_free_chain(om);
    }
}

uint16_t couple_gatt_voice_mtu(void)
{
    // App 未订阅 0xFFE9 时发送必然失败，这里直接当作链路不可用，
    // 让上层在开始录音前就能给出“请用 App 连接”的提示。
    if (!s_initialized || !s_connected || !s_voice_subscribed) return 0;

    uint16_t mtu = ble_att_mtu(s_conn_handle);
    if (mtu < 23) return 0;
    if (mtu > 512) mtu = 512;
    return mtu;
}

esp_err_t couple_gatt_notify_voice(const uint8_t *data, size_t len)
{
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;

    uint16_t mtu = couple_gatt_voice_mtu();
    if (mtu == 0 || s_voice_val_handle == 0) return ESP_ERR_INVALID_STATE;
    if (len > (size_t)mtu - 3) return ESP_ERR_INVALID_SIZE;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) return ESP_ERR_NO_MEM;

    int rc = ble_gatts_notify_custom(s_conn_handle, s_voice_val_handle, om);
    if (rc != 0) {
        os_mbuf_free_chain(om);
        if (rc == BLE_HS_ENOMEM) return ESP_ERR_NO_MEM;
        if (rc == BLE_HS_EMSGSIZE) return ESP_ERR_INVALID_SIZE;
        return ESP_FAIL;
    }
    return ESP_OK;
}
