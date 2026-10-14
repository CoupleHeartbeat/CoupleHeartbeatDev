// main/couple_wifi.c —— Wi-Fi STA 配网实现
//
// 关键约束：GATT 写入回调运行在 NimBLE host 任务上，esp_wifi_init/start、
// NVS 提交都是百毫秒级操作，直接在回调里执行会阻塞 BLE 协议栈。
// 因此回调只做参数校验并入队，真正的连接动作由本模块的工作任务完成。
#include "couple_wifi.h"

#include "couple_gatt.h"
#include "couple_wifi_scan.h"
#include "demo_radio.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "couple_wifi";

#define WIFI_NVS_NAMESPACE "couple_wifi"
#define WIFI_NVS_KEY_SSID  "ssid"
#define WIFI_NVS_KEY_PASS  "pass"

// 断开后的重试次数。耗尽后进入 FAILED，等 App 重新下发凭据再试，
// 避免错误密码导致无限重连拖垮电量与射频。
#define WIFI_MAX_RETRY 5

#define WIFI_REQ_QUEUE_DEPTH 2

typedef struct {
    bool clear;
    bool scan;
    char ssid[COUPLE_WIFI_SSID_MAX];
    char password[COUPLE_WIFI_PASS_MAX];
} wifi_request_t;

static bool s_netif_ready;
static bool s_wifi_inited;    // netif + esp_wifi_init + 事件注册已完成
static bool s_wifi_started;   // esp_wifi_start() 已调用
static bool s_creds_loaded;
static bool s_has_creds;
// 是否有凭据且希望保持连接。STA_START / DISCONNECTED 只在这个标志为真时才发起
// 连接，否则会出现“射频刚起来就拿着空配置去连”的竞态。
static bool s_want_connect;
static int s_retry;
static QueueHandle_t s_req_queue;
static TaskHandle_t s_worker;

// 扫描结果（供 App 读取选择网络）
static couple_wifi_scan_list_t s_scan_list;
static volatile bool s_scanning;
static volatile bool s_scan_pending;   // Wi-Fi 尚未启动完成时挂起的一次扫描
static volatile bool s_scan_done;

static couple_wifi_state_t s_state = COUPLE_WIFI_STATE_IDLE;
static char s_ssid[COUPLE_WIFI_SSID_MAX];
static char s_pass[COUPLE_WIFI_PASS_MAX];
static char s_ip[16];
static int s_rssi;

// ============================================================================
// 内部工具
// ============================================================================
static void set_state(couple_wifi_state_t st)
{
    if (s_state == st) return;
    s_state = st;
    ESP_LOGI(TAG, "Wi-Fi 状态 -> %d", (int)st);
    // 通知已订阅的 App（无客户端时内部直接返回）
    couple_gatt_notify_wifi_status();
}

static esp_err_t load_creds(void)
{
    if (s_creds_loaded) return s_has_creds ? ESP_OK : ESP_ERR_NOT_FOUND;
    s_creds_loaded = true;

    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        s_has_creds = false;
        return ESP_ERR_NOT_FOUND;
    }

    size_t len = sizeof(s_ssid);
    err = nvs_get_str(h, WIFI_NVS_KEY_SSID, s_ssid, &len);
    if (err == ESP_OK) {
        len = sizeof(s_pass);
        if (nvs_get_str(h, WIFI_NVS_KEY_PASS, s_pass, &len) != ESP_OK) {
            s_pass[0] = '\0';
        }
        s_has_creds = (s_ssid[0] != '\0');
    } else {
        s_has_creds = false;
    }

    nvs_close(h);
    return s_has_creds ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t save_creds(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, WIFI_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, WIFI_NVS_KEY_PASS, password ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t erase_creds(void)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return ESP_OK;
    nvs_erase_all(h);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void update_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_rssi = ap.rssi;
    } else {
        s_rssi = 0;
    }
}

// ============================================================================
// 扫描：把附近 AP 整理后交给 App 选择
// ============================================================================
static void start_scan_now(void)
{
    wifi_scan_config_t sc = {0};
    sc.show_hidden = false;

    esp_err_t err = esp_wifi_scan_start(&sc, false);   // 非阻塞，结果在 SCAN_DONE 取
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "启动扫描失败: %s", esp_err_to_name(err));
        s_scanning = false;
        s_scan_pending = false;
        return;
    }
    s_scan_pending = false;
    s_scanning = true;
    ESP_LOGI(TAG, "开始扫描附近网络");
}

// SCAN_DONE：把驱动里的结果搬进 s_scan_list（去重 + 按信号排序）
static void scan_collect(void)
{
    s_scanning = false;

    uint16_t num = 0;
    if (esp_wifi_scan_get_ap_num(&num) != ESP_OK || num == 0) {
        s_scan_list.count = 0;
        s_scan_done = true;
        esp_wifi_clear_ap_list();
        ESP_LOGI(TAG, "扫描完成：未发现网络");
        return;
    }
    if (num > 32) num = 32;

    wifi_ap_record_t *recs = malloc((size_t)num * sizeof(wifi_ap_record_t));
    if (!recs) {
        s_scan_list.count = 0;
        s_scan_done = true;
        esp_wifi_clear_ap_list();
        ESP_LOGW(TAG, "扫描结果缓冲不足");
        return;
    }

    uint16_t got = num;
    couple_wifi_scan_clear(&s_scan_list);
    if (esp_wifi_scan_get_ap_records(&got, recs) == ESP_OK) {
        for (uint16_t i = 0; i < got; i++) {
            couple_wifi_scan_add(&s_scan_list, (const char *)recs[i].ssid,
                                 (int8_t)recs[i].rssi);
        }
    }
    free(recs);
    esp_wifi_clear_ap_list();
    s_scan_done = true;
    // 只记录数量，不记录 SSID 列表
    ESP_LOGI(TAG, "扫描完成：%u 个网络", (unsigned)s_scan_list.count);
}

// ============================================================================
// 事件处理（运行在默认事件循环任务）
// ============================================================================
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // 只有确实希望连接时才发起：射频刚起来时配置还没写，贸然 connect 会用空 SSID
        if (s_want_connect) {
            set_state(COUPLE_WIFI_STATE_CONNECTING);
            esp_wifi_connect();
        }
        if (s_scan_pending) {
            start_scan_now();
        }
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        scan_collect();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        s_rssi = 0;
        s_ip[0] = '\0';

        if (!s_want_connect || !s_has_creds) {
            set_state(COUPLE_WIFI_STATE_IDLE);
            return;
        }
        if (s_retry < WIFI_MAX_RETRY) {
            s_retry++;
            // 只记录断开原因码，不记录 SSID/密码
            ESP_LOGW(TAG, "Wi-Fi 断开(reason=%d)，第 %d 次重试", d->reason, s_retry);
            set_state(COUPLE_WIFI_STATE_CONNECTING);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "Wi-Fi 连接失败(reason=%d)，重试耗尽", d->reason);
            set_state(COUPLE_WIFI_STATE_FAILED);
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_retry = 0;
        // 射频开机与握手会占用大量堆，这里记录一次便于评估共存余量
        ESP_LOGI(TAG, "Wi-Fi 已连接，IP=%s 空闲堆=%u", s_ip,
                 (unsigned)esp_get_free_heap_size());
        set_state(COUPLE_WIFI_STATE_CONNECTED);
        update_rssi();
        return;
    }
}

// ============================================================================
// 初始化并启动 Wi-Fi
//
// 顺序很关键：esp_wifi_set_config() 必须在 esp_wifi_start() 之前完成。
// 否则 STA_START 回调会在配置仍为空时就去 connect，紧接着真正的连接请求又把
// 它打断（两次 esp_wifi_connect 互相冲突），表现就是“配网一直失败”。
// 因此这里把初始化与启动拆开，连接统一由 connect_with() 编排。
// ============================================================================
static esp_err_t ensure_wifi_inited(void)
{
    if (s_wifi_inited) return ESP_OK;

    esp_err_t err = demo_radio_network_prepare();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "网络服务初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    if (!s_netif_ready) {
        if (esp_netif_create_default_wifi_sta() == NULL) {
            ESP_LOGE(TAG, "创建 STA netif 失败");
            return ESP_FAIL;
        }
        s_netif_ready = true;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init 失败: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    s_wifi_inited = true;
    return ESP_OK;
}

// 启动射频（幂等）。只有需要连接或需要扫描时才启动，未配网的设备不占射频。
static esp_err_t wifi_start(void)
{
    if (s_wifi_started) return ESP_OK;

    esp_err_t err = ensure_wifi_inited();
    if (err != ESP_OK) return err;

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start 失败: %s", esp_err_to_name(err));
        return err;
    }
    s_wifi_started = true;
    return ESP_OK;
}

static esp_err_t set_sta_config(void)
{
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, s_pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;  // 开放网络也允许
    wc.sta.pmf_cfg.capable = true;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写入 Wi-Fi 配置失败: %s", esp_err_to_name(err));
    }
    return err;
}

// 同步执行一次配网（只由工作任务调用）
static esp_err_t connect_with(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;

    esp_err_t err = save_creds(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "保存 Wi-Fi 凭据失败: %s", esp_err_to_name(err));
        return err;
    }

    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    if (password) {
        strncpy(s_pass, password, sizeof(s_pass) - 1);
        s_pass[sizeof(s_pass) - 1] = '\0';
    } else {
        s_pass[0] = '\0';
    }
    s_creds_loaded = true;
    s_has_creds = true;
    s_want_connect = true;
    s_retry = 0;
    s_ip[0] = '\0';

    err = ensure_wifi_inited();
    if (err != ESP_OK) return err;

    err = set_sta_config();
    if (err != ESP_OK) return err;

    set_state(COUPLE_WIFI_STATE_CONNECTING);

    if (!s_wifi_started) {
        // 首次：配置已经写好，启动后由 STA_START 回调发起连接
        return wifi_start();
    }

    // 已在运行：断开会触发 DISCONNECTED，回调会按新配置重连；
    // 当前没连着时不会产生该事件，这里直接发起连接。
    err = esp_wifi_disconnect();
    if (err == ESP_OK) return ESP_OK;
    return esp_wifi_connect();
}

static void clear_sync(void)
{
    erase_creds();

    // 先撤销连接意图，否则断开产生的 DISCONNECTED 会触发重连
    s_want_connect = false;

    if (s_wifi_started) {
        esp_wifi_disconnect();
    }

    s_has_creds = false;
    s_creds_loaded = true;
    s_retry = 0;
    s_ssid[0] = '\0';
    s_pass[0] = '\0';
    s_ip[0] = '\0';
    s_rssi = 0;
    set_state(COUPLE_WIFI_STATE_IDLE);
    ESP_LOGI(TAG, "Wi-Fi 凭据已清除");
}

// 扫描一次附近网络（只由工作任务调用）
static void scan_sync(void)
{
    couple_wifi_scan_clear(&s_scan_list);
    s_scan_done = false;
    s_scan_pending = true;

    esp_err_t err = wifi_start();
    if (err != ESP_OK) {
        s_scan_pending = false;
        s_scan_done = true;
        return;
    }

    // 已经在运行时不会被 STA_START 回调带起来，这里直接发起
    if (!s_scanning && s_scan_pending) {
        start_scan_now();
    }
}

// ============================================================================
// 工作任务：串行执行慢操作
// ============================================================================
static void wifi_worker(void *arg)
{
    (void)arg;
    wifi_request_t req;
    for (;;) {
        if (xQueueReceive(s_req_queue, &req, portMAX_DELAY) != pdTRUE) continue;
        if (req.scan) {
            scan_sync();
        } else if (req.clear) {
            clear_sync();
        } else {
            connect_with(req.ssid, req.password);
            // 立即把密码字段擦掉，缩小明文驻留窗口
            memset(req.password, 0, sizeof(req.password));
        }
    }
}

// ============================================================================
// 公开 API
// ============================================================================
esp_err_t couple_wifi_init(void)
{
    esp_err_t err = demo_radio_nvs_prepare();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 未就绪: %s", esp_err_to_name(err));
    }

    load_creds();

    s_req_queue = xQueueCreate(WIFI_REQ_QUEUE_DEPTH, sizeof(wifi_request_t));
    if (!s_req_queue) return ESP_ERR_NO_MEM;

    if (xTaskCreate(wifi_worker, "couple_wifi", 4096, NULL, 4, &s_worker) != pdPASS) {
        vQueueDelete(s_req_queue);
        s_req_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t couple_wifi_autoconnect(void)
{
    if (load_creds() != ESP_OK) return ESP_ERR_NOT_FOUND;
    if (!s_req_queue) return ESP_ERR_INVALID_STATE;

    wifi_request_t req = {0};
    strncpy(req.ssid, s_ssid, sizeof(req.ssid) - 1);
    strncpy(req.password, s_pass, sizeof(req.password) - 1);
    if (xQueueSend(s_req_queue, &req, 0) != pdTRUE) return ESP_ERR_TIMEOUT;

    ESP_LOGI(TAG, "使用已保存凭据连接 Wi-Fi");
    return ESP_OK;
}

esp_err_t couple_wifi_request_credentials(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (strlen(ssid) >= COUPLE_WIFI_SSID_MAX) return ESP_ERR_INVALID_SIZE;
    if (password && strlen(password) >= COUPLE_WIFI_PASS_MAX) return ESP_ERR_INVALID_SIZE;
    if (!s_req_queue) return ESP_ERR_INVALID_STATE;

    wifi_request_t req = {0};
    strncpy(req.ssid, ssid, sizeof(req.ssid) - 1);
    if (password) {
        strncpy(req.password, password, sizeof(req.password) - 1);
    }
    if (xQueueSend(s_req_queue, &req, 0) != pdTRUE) return ESP_ERR_TIMEOUT;

    memset(req.password, 0, sizeof(req.password));
    // 入队即视为进入连接流程，避免 App 读到的状态滞后于实际动作
    set_state(COUPLE_WIFI_STATE_CONNECTING);
    return ESP_OK;
}

esp_err_t couple_wifi_request_clear(void)
{
    if (!s_req_queue) return ESP_ERR_INVALID_STATE;
    wifi_request_t req = {0};
    req.clear = true;
    if (xQueueSend(s_req_queue, &req, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    return ESP_OK;
}

couple_wifi_state_t couple_wifi_get_state(void)
{
    return s_state;
}

bool couple_wifi_is_configured(void)
{
    return s_has_creds;
}

const char *couple_wifi_get_ssid(void)
{
    return s_ssid;
}

const char *couple_wifi_get_ip(void)
{
    return s_ip;
}

int couple_wifi_get_rssi(void)
{
    return s_rssi;
}

// ============================================================================
// 扫描结果（供 App 读取）
// ============================================================================
esp_err_t couple_wifi_request_scan(void)
{
    if (!s_req_queue) return ESP_ERR_INVALID_STATE;
    if (s_scanning) return ESP_OK;   // 正在扫，别叠加请求

    wifi_request_t req = {0};
    req.scan = true;
    if (xQueueSend(s_req_queue, &req, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    return ESP_OK;
}

bool couple_wifi_is_scanning(void)
{
    return s_scanning;
}

bool couple_wifi_scan_done(void)
{
    return s_scan_done;
}

const couple_wifi_scan_list_t *couple_wifi_scan_results(void)
{
    return &s_scan_list;
}
