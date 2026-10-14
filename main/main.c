// main/main.c —— FoloToy AI Passport 情侣互动应用主入口
//
// 按键语义：
//   OK 短按   切换模式 / 确认
//   UP 短按   上一页 / 增加
//   DOWN 短按 下一页 / 减少
//   OK 长按   返回（各模式自定义）
#include "couple_app.h"
#include "couple_ai.h"
#include "couple_fonts.h"
#include "couple_wifi.h"
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_battery.h"
#include "bsp_pins.h"
#include "key_guard.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include <time.h>
#include <sys/time.h>
#include <string.h>

static const char *TAG = "couple_main";

#define INPUT_QUEUE_DEPTH 8

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static TaskHandle_t s_tick_task;
static volatile bool s_input_ready;

#define COUPLE_TICK_PERIOD_MS 200

// 深睡按键唤醒后的按键动作守卫（与上游 main.c 保持同一契约，见 key_guard.h）。
static key_guard_t s_key_guard;

// ============================================================================
// 按键事件派发
// ============================================================================
static void dispatch_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!bsp_lvgl_lock(500)) return;

    switch (g_couple_state.mode) {
    case COUPLE_MODE_BADGE:
        couple_badge_key(btn, ev);
        break;
    case COUPLE_MODE_HEARTBEAT:
        couple_heartbeat_key(btn, ev);
        break;
    case COUPLE_MODE_TOUCH:
        couple_touch_key(btn, ev);
        break;
    default:
        break;
    }

    bsp_lvgl_unlock();
}

static void input_task(void *arg)
{
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) == pdTRUE) {
            dispatch_key(input.btn, input.event);
        }
    }
}

static esp_err_t input_dispatch_init(void)
{
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(input_task, "couple_input", 3072, NULL, 5,
                    &s_input_task) != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// ============================================================================
// 周期任务：心跳超时检测 + 靠近自动切换模式
// 不能在 LVGL 任务中调用（couple_ble_tick 内部会取 bsp_lvgl_lock）。
// ============================================================================
static void couple_tick_task(void *arg)
{
    (void)arg;
    for (;;) {
        couple_ble_tick();
        vTaskDelay(pdMS_TO_TICKS(COUPLE_TICK_PERIOD_MS));
    }
}

// ============================================================================
// 按键回调（运行在 button 组件任务中，必须非阻塞）
// ============================================================================
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    // 必须在就绪检查之前消费守卫：就绪前到达的松手事件若被丢弃，
    // 守卫会一直处于上防状态并吞掉下一次真实按键。
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键已松手,按键恢复响应");
        }
        return;
    }
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    xQueueSend(s_input_queue, &input, 0);
}

// ============================================================================
// 设置默认时间（没有 RTC 电池时的初始值）
// ============================================================================
static void set_default_time(void)
{
    struct timeval tv = {
        .tv_sec = 1767225600,  // 2026-01-01 00:00:00 UTC
        .tv_usec = 0,
    };
    settimeofday(&tv, NULL);
    setenv("TZ", "CST-8", 1);
    tzset();
}

// ============================================================================
// 显示启动画面（确保屏幕先点亮）
// ============================================================================
static void show_boot_screen(void)
{
    lv_font_t *font_normal = couple_font_get(COUPLE_FONT_NORMAL);
    lv_font_t *font_large = couple_font_get(COUPLE_FONT_LARGE);

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, "情侣互动");
    lv_obj_set_style_text_font(label, font_large, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xff6b9d), 0);
    lv_obj_center(label);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "加载中...");
    lv_obj_set_style_text_font(sub, font_normal, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x808080), 0);
    lv_obj_align(sub, LV_ALIGN_BOTTOM_MID, 0, -40);

    lv_scr_load(scr);
}

// ============================================================================
// 主函数
// ============================================================================
void app_main(void)
{
    ESP_LOGI(TAG, "FoloToy AI Passport 情侣互动应用启动");

    // 设置默认时间
    set_default_time();

    // 先初始化 I2C（电量计、音频 codec 用）
    bsp_i2c_init();

    // 初始化显示 + LVGL（最优先，确保屏幕能亮）
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败");
        return;
    }
    bsp_display_backlight(100);

    // 初始化中文字体（必须在创建任何 UI 之前）
    couple_fonts_init();

    // 先显示启动画面，证明屏幕工作正常
    if (bsp_lvgl_lock(500)) {
        show_boot_screen();
        bsp_lvgl_unlock();
    }

    ESP_LOGI(TAG, "显示初始化完成");

    // 初始化存储（NVS）
    esp_err_t storage_err = couple_storage_init();
    if (storage_err != ESP_OK) {
        ESP_LOGE(TAG, "存储初始化失败: %s", esp_err_to_name(storage_err));
    }

    // 加载配对信息
    couple_pair_info_t pair_info;
    memset(&pair_info, 0, sizeof(pair_info));
    if (storage_err == ESP_OK) {
        couple_storage_load_pair(&pair_info);
    }

    // 初始化加密模块
    couple_crypto_init();

    // 初始化模式管理器
    couple_mode_init();
    memcpy(&g_couple_state.pair_info, &pair_info, sizeof(pair_info));

    // 初始化按键
    esp_err_t input_err = input_dispatch_init();
    esp_err_t button_err = input_err == ESP_OK
                         ? bsp_button_init(on_key, NULL)
                         : ESP_ERR_INVALID_STATE;
    if (input_err != ESP_OK) {
        ESP_LOGE(TAG, "按键任务创建失败: %s", esp_err_to_name(input_err));
    } else if (button_err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(button_err));
    }

    // 初始化电池（失败不阻塞）
    esp_err_t batt_err = bsp_battery_init();
    if (batt_err != ESP_OK) {
        ESP_LOGW(TAG, "电池初始化失败: %s", esp_err_to_name(batt_err));
    }

    // 初始化音频（失败不阻塞，心跳声功能不可用）
    g_couple_state.volume = 60;  // 默认音量 60%
    g_couple_state.muted = false;
    couple_audio_init();

    // 初始化 GATT 配置服务（App 配对用，独占 NimBLE host）
    esp_err_t gatt_err = couple_gatt_init();
    if (gatt_err != ESP_OK) {
        ESP_LOGW(TAG, "GATT 服务初始化失败: %s", esp_err_to_name(gatt_err));
    }

    // 初始化 Wi-Fi 配网（App 经 BLE 下发凭据；此处不启动射频）
    esp_err_t wifi_err = couple_wifi_init();
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi 配网初始化失败: %s", esp_err_to_name(wifi_err));
    } else if (couple_wifi_autoconnect() == ESP_OK) {
        ESP_LOGI(TAG, "使用已保存的 Wi-Fi 凭据自动连接");
    }

    // 初始化 AI 对话工作任务（长按 OK 进入页面后才会真正发请求）
    esp_err_t ai_err = couple_ai_init();
    if (ai_err != ESP_OK) {
        ESP_LOGW(TAG, "AI 对话初始化失败: %s", esp_err_to_name(ai_err));
    }

    // 启动心跳扫描（依赖 GATT 持有的 NimBLE host；host 未同步时由 tick 重试）
    if (gatt_err == ESP_OK) {
        esp_err_t ble_err = couple_ble_start();
        if (ble_err != ESP_OK) {
            ESP_LOGW(TAG, "心跳扫描暂未启动（等待 NimBLE 同步）: %s",
                     esp_err_to_name(ble_err));
        }
    }

    // 进入工牌模式
    if (bsp_lvgl_lock(1000)) {
        couple_badge_enter();
        bsp_lvgl_unlock();
        s_input_ready = true;
    }

    // 周期性任务：心跳超时检测 + 靠近自动切换模式
    if (xTaskCreate(couple_tick_task, "couple_tick", 4096, NULL, 4,
                    &s_tick_task) != pdPASS) {
        ESP_LOGE(TAG, "周期任务创建失败，靠近自动切换不可用");
    }

    ESP_LOGI(TAG, "就绪: 配对=%d 角色=%c 电池=%s GATT=%s",
             g_couple_state.pair_info.paired,
             g_couple_state.pair_info.role,
             batt_err == ESP_OK ? "OK" : "FAIL",
             gatt_err == ESP_OK ? "OK" : "FAIL");
    // 本板无 PSRAM，Wi-Fi + BLE + TLS 同时驻留是主要风险，启动后先记录堆余量
    ESP_LOGI(TAG, "空闲堆: %u 字节 (历史最小 %u)",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());
}
