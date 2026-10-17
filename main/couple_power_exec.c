// main/couple_power_exec.c —— 低功耗执行层：背光切换、警告屏、深睡序列
//
// 深睡序列复用 demo_low_power.c 已在硬件上验证过的契约：
//   先拒绝"按键被按住"（此时按键还活着）→ 心跳扫描/GATT 停机 →
//   bsp_button_prepare_deep_sleep 把按键脚交回数字输入并武装 GPIO 低电平唤醒 →
//   CW2017 -> ES8311 -> I2S 引脚 -> 共享 I2C 引脚 依序停机 →
//   持有 LVGL 锁阻止刷屏，bsp_display_prepare_deep_sleep 让 ST7789 Sleep In →
//   esp_deep_sleep_start()（不返回）。
#include "couple_power.h"
#include "couple_app.h"
#include "couple_fonts.h"
#include "couple_gatt.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "couple_power";

static couple_power_state_t s_state = COUPLE_POWER_STATE_ACTIVE;
static bool s_manual;                  // 警告屏由"立即休眠"触发（不可取消）
static uint32_t s_last_activity_ms;    // 最后一次按键/活跃时刻
static uint32_t s_warn_start_ms;       // 警告屏出现时刻
static lv_obj_t *s_warn_panel;         // 叠加在顶层 layer 的全屏警告遮罩

couple_power_state_t couple_power_get_state(void)
{
    return s_state;
}

// ---------------------------------------------------------------------------
// 警告遮罩：放在 lv_layer_top()，不与工牌页/AI 页的屏幕对象纠缠；
// 取消即删除，深睡时随整个显示子系统一并停机。
// ---------------------------------------------------------------------------
static void warn_overlay_show(bool manual)
{
    if (s_warn_panel) return;
    if (!bsp_lvgl_lock(500)) return;

    s_warn_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_warn_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_warn_panel, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_bg_opa(s_warn_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_warn_panel, 0, 0);
    lv_obj_set_style_pad_all(s_warn_panel, 0, 0);
    lv_obj_clear_flag(s_warn_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_warn_panel, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = lv_label_create(s_warn_panel);
    lv_label_set_text(label, manual ? "即将休眠" : "即将休眠\n按任意键取消");
    lv_obj_set_style_text_font(label, couple_font_get(COUPLE_FONT_NORMAL), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xff6b9d), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);

    bsp_lvgl_unlock();
}

static void warn_overlay_close(void)
{
    if (!s_warn_panel) return;
    if (!bsp_lvgl_lock(500)) return;
    lv_obj_delete(s_warn_panel);
    s_warn_panel = NULL;
    bsp_lvgl_unlock();
}

// ---------------------------------------------------------------------------
// 深睡执行（不返回；被按键按住等情况拒绝时回到 ACTIVE）
// ---------------------------------------------------------------------------
static void enter_deep_sleep(void)
{
    // 按键被按住时 GPIO 低电平唤醒条件在入睡瞬间就成立，会立刻醒来——拒绝入睡。
    int mv = bsp_button_read_mv();
    if (mv >= 0 && mv < BSP_BTN_MV_RELEASED_MIN) {
        ESP_LOGW(TAG, "按键被按住(%d mV)，取消休眠", mv);
        s_state = COUPLE_POWER_STATE_ACTIVE;
        s_manual = false;
        s_last_activity_ms = couple_now_ms();  // 静默计时重开，避免立刻再次触发
        warn_overlay_close();
        bsp_display_backlight(100);
        return;
    }

    ESP_LOGI(TAG, "进入深度睡眠，按任意键唤醒");

    couple_audio_stop();

    // 心跳扫描与 GATT 优雅停机；失败不阻塞——深睡会复位整个射频。
    couple_ble_stop();
    if (couple_gatt_deinit() != ESP_OK) {
        ESP_LOGW(TAG, "GATT 停机失败，继续休眠");
    }

    // 把按键脚交回普通数字输入 + 上拉（调用后按键本次运行不可用）。
    int level = 0;
    esp_err_t err = bsp_button_prepare_deep_sleep(&level);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "按键交回失败，重启恢复外设: %s", esp_err_to_name(err));
        esp_restart();
    }
    if (level != 1) {
        ESP_LOGW(TAG, "入睡前按键仍被按住，这次深睡可能立即返回");
    }
    // 任一键把该脚拉到低电平，故按低电平武装；第一个参数是位掩码不是引脚号。
    esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO,
                                      ESP_GPIO_WAKEUP_GPIO_LOW);

    // CW2017 与 ES8311 共用 I2C，按 电池 -> codec -> I2S 引脚 -> I2C 引脚 顺序停。
    if (bsp_battery_sleep() != ESP_OK) ESP_LOGW(TAG, "CW2017 suspend 失败");
    if (bsp_audio_sleep() != ESP_OK) ESP_LOGW(TAG, "ES8311 suspend 失败");
    if (bsp_audio_prepare_deep_sleep() != ESP_OK) {
        ESP_LOGW(TAG, "I2S 引脚释放失败");
    }
    if (bsp_i2c_prepare_deep_sleep() != ESP_OK) {
        ESP_LOGW(TAG, "I2C 引脚释放失败");
    }

    // 持锁阻止 LVGL 在 LCD 关闭后再刷屏；锁必须持有到 esp_deep_sleep_start。
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "深睡前无法停止 LVGL 刷屏，重启恢复外设");
        esp_restart();
    }
    if (bsp_display_prepare_deep_sleep() != ESP_OK) {
        ESP_LOGW(TAG, "显示休眠准备失败，继续深睡");
    }
    esp_deep_sleep_start();

    // 不应到达；到达说明外设已停机且睡眠失败，只能重启恢复。
    esp_restart();
}

// ---------------------------------------------------------------------------
// 公开入口
// ---------------------------------------------------------------------------
void couple_power_init(void)
{
    s_state = COUPLE_POWER_STATE_ACTIVE;
    s_manual = false;
    s_warn_panel = NULL;
    s_last_activity_ms = couple_now_ms();
    s_warn_start_ms = 0;
}

void couple_power_tick(const couple_power_inputs_t *in)
{
    if (!in) return;
    uint32_t now = couple_now_ms();

    // 互动即使用：对方在附近或 AI 对话中视为活跃，静默计时清零
    if (in->partner_present || in->mode_ai) {
        s_last_activity_ms = now;
    }

    uint32_t idle = now - s_last_activity_ms;
    uint32_t warn_elapsed = now - s_warn_start_ms;
    couple_power_snap_t snap = { .state = s_state, .manual = s_manual };
    couple_power_action_t act =
        couple_power_next(snap, in, idle, warn_elapsed);

    switch (act) {
    case COUPLE_POWER_ACT_SCREEN_OFF:
        // 低功耗屏：不再全黑，改为调暗背光 + 只显示姓名(大字)/时间/年月日
        bsp_display_backlight(COUPLE_POWER_DIM_PERCENT);
        if (bsp_lvgl_lock(500)) {
            couple_badge_set_minimal(true);
            bsp_lvgl_unlock();
        }
        s_state = COUPLE_POWER_STATE_SCREEN_OFF;
        ESP_LOGI(TAG, "无操作息屏，进入低功耗屏（背光 %d%%），心跳检测继续",
                 COUPLE_POWER_DIM_PERCENT);
        break;
    case COUPLE_POWER_ACT_SCREEN_ON:
        s_state = COUPLE_POWER_STATE_ACTIVE;
        s_manual = false;
        s_last_activity_ms = now;
        warn_overlay_close();
        if (bsp_lvgl_lock(500)) {
            couple_badge_set_minimal(false);
            bsp_lvgl_unlock();
        }
        bsp_display_backlight(100);
        ESP_LOGI(TAG, "唤醒屏幕");
        break;
    case COUPLE_POWER_ACT_WARN:
        s_state = COUPLE_POWER_STATE_SLEEP_WARN;
        s_warn_start_ms = now;
        bsp_display_backlight(100);
        warn_overlay_show(s_manual);
        break;
    case COUPLE_POWER_ACT_SLEEP:
        enter_deep_sleep();
        break;
    default:
        // 手动请求直接置状态，遮罩延迟到本拍补画（调用方持有 LVGL 锁，不能重入）
        if (s_state == COUPLE_POWER_STATE_SLEEP_WARN && !s_warn_panel) {
            warn_overlay_show(s_manual);
        }
        break;
    }
}

bool couple_power_notify_key(void)
{
    s_last_activity_ms = couple_now_ms();

    if (s_state == COUPLE_POWER_STATE_SCREEN_OFF) {
        s_state = COUPLE_POWER_STATE_ACTIVE;
        if (bsp_lvgl_lock(500)) {
            couple_badge_set_minimal(false);
            bsp_lvgl_unlock();
        }
        bsp_display_backlight(100);
        ESP_LOGI(TAG, "按键唤醒屏幕");
        return true;
    }
    if (s_state == COUPLE_POWER_STATE_SLEEP_WARN && !s_manual) {
        s_state = COUPLE_POWER_STATE_ACTIVE;
        warn_overlay_close();
        bsp_display_backlight(100);
        ESP_LOGI(TAG, "按键取消休眠");
        return true;
    }
    return false;
}

void couple_power_request_sleep_now(void)
{
    if (s_state == COUPLE_POWER_STATE_SLEEP_WARN && s_manual) return;
    s_manual = true;
    s_state = COUPLE_POWER_STATE_SLEEP_WARN;
    s_warn_start_ms = couple_now_ms();
    ESP_LOGI(TAG, "手动请求休眠");
}
