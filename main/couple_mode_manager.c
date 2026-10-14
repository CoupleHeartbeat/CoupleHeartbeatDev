// main/couple_mode_manager.c —— 情侣互动应用模式状态机
#include "couple_app.h"
#include "couple_ai.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "couple_mode";

couple_state_t g_couple_state;

void couple_mode_init(void)
{
    g_couple_state.mode = COUPLE_MODE_BADGE;
    g_couple_state.partner_rssi = -100;
    g_couple_state.partner_connected = false;
    g_couple_state.distance = COUPLE_DISTANCE_FAR;
    g_couple_state.last_heartbeat_ms = 0;
}

couple_mode_t couple_mode_get(void)
{
    return g_couple_state.mode;
}

void couple_mode_switch(couple_mode_t new_mode)
{
    if (new_mode == g_couple_state.mode) return;

    ESP_LOGI(TAG, "模式切换: %d -> %d", g_couple_state.mode, new_mode);

    // 退出当前模式
    switch (g_couple_state.mode) {
    case COUPLE_MODE_BADGE:
        couple_badge_exit();
        break;
    case COUPLE_MODE_HEARTBEAT:
        couple_heartbeat_exit();
        break;
    case COUPLE_MODE_TOUCH:
        couple_touch_exit();
        break;
    case COUPLE_MODE_AI:
        couple_ai_exit();
        break;
    default:
        break;
    }

    g_couple_state.mode = new_mode;

    // 进入新模式
    switch (new_mode) {
    case COUPLE_MODE_BADGE:
        couple_badge_enter();
        break;
    case COUPLE_MODE_HEARTBEAT:
        couple_heartbeat_enter();
        break;
    case COUPLE_MODE_TOUCH:
        couple_touch_enter();
        break;
    case COUPLE_MODE_AI:
        couple_ai_enter();
        break;
    default:
        break;
    }
}

uint32_t couple_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}
