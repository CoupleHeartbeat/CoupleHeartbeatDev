// main/couple_heartbeat.c —— 心动模式：RSSI 距离感知 + 光影动画
#include "couple_app.h"
#include "couple_fonts.h"
#include "esp_log.h"
#include "lvgl.h"
#include <string.h>

static const char *TAG = "couple_heartbeat";

static lv_obj_t *s_scr;
static lv_obj_t *s_glow;          // 背景光晕
static lv_obj_t *s_heart;         // 中心心形
static lv_obj_t *s_dist_label;    // 距离文字
static lv_obj_t *s_rssi_label;    // RSSI 文字
static lv_obj_t *s_status_label;  // 状态文字
static lv_timer_t *s_update_timer;
static lv_anim_t s_heart_anim;    // 心跳动画
static lv_anim_t s_glow_anim;     // 光晕动画

static int8_t s_current_rssi = -100;
static bool s_heart_anim_running = false;

// 根据 RSSI 计算距离（简化对数模型）
static float rssi_to_distance(int8_t rssi)
{
    if (rssi >= -40) return 0.3f;
    if (rssi >= -50) return 0.8f;
    if (rssi >= -60) return 2.0f;
    if (rssi >= -70) return 5.0f;
    if (rssi >= -80) return 10.0f;
    return 20.0f;
}

// 获取距离等级（0~3，越近等级越高）
static int get_distance_level(int8_t rssi)
{
    if (rssi >= -45) return 3;
    if (rssi >= -58) return 2;
    if (rssi >= -70) return 1;
    return 0;
}

// 心跳动画回调 - 缩放
static void heart_scale_anim_cb(void *var, int32_t v)
{
    lv_obj_set_style_transform_scale(var, v, 0);
}

// 光晕动画回调 - 透明度
static void glow_opa_anim_cb(void *var, int32_t v)
{
    lv_obj_set_style_bg_opa(var, v, 0);
}

// 更新心率（距离越近跳得越快）
static void update_heartbeat_rate(int level)
{
    uint16_t period_ms;
    switch (level) {
        case 3: period_ms = 400; break;
        case 2: period_ms = 600; break;
        case 1: period_ms = 800; break;
        default: period_ms = 1200; break;
    }

    if (s_heart_anim_running) {
        lv_anim_del(s_heart, heart_scale_anim_cb);
        lv_anim_del(s_glow, glow_opa_anim_cb);
    }

    // 心跳缩放动画（基础大小 2.0x，跳动幅度随等级增加）
    int32_t base_scale = 512;  // 2.0x
    int32_t pulse_scale = 512 + 60 + level * 40;  // 2.23x ~ 2.73x

    lv_anim_init(&s_heart_anim);
    lv_anim_set_var(&s_heart_anim, s_heart);
    lv_anim_set_exec_cb(&s_heart_anim, heart_scale_anim_cb);
    lv_anim_set_values(&s_heart_anim, base_scale, pulse_scale);
    lv_anim_set_time(&s_heart_anim, period_ms / 3);
    lv_anim_set_playback_time(&s_heart_anim, period_ms * 2 / 3);
    lv_anim_set_repeat_count(&s_heart_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&s_heart_anim, lv_anim_path_ease_in_out);
    lv_anim_start(&s_heart_anim);

    // 光晕呼吸动画
    int glow_max_opa = 30 + level * 50;
    lv_anim_init(&s_glow_anim);
    lv_anim_set_var(&s_glow_anim, s_glow);
    lv_anim_set_exec_cb(&s_glow_anim, glow_opa_anim_cb);
    lv_anim_set_values(&s_glow_anim, 10, glow_max_opa);
    lv_anim_set_time(&s_glow_anim, period_ms / 2);
    lv_anim_set_playback_time(&s_glow_anim, period_ms / 2);
    lv_anim_set_repeat_count(&s_glow_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&s_glow_anim, lv_anim_path_ease_in_out);
    lv_anim_start(&s_glow_anim);

    s_heart_anim_running = true;
}

// 更新 UI
static void update_ui(lv_timer_t *timer)
{
    (void)timer;

    int8_t rssi = couple_ble_get_rssi();
    s_current_rssi = rssi;

    int level = get_distance_level(rssi);
    float dist = rssi_to_distance(rssi);

    // 更新信号强度显示
    char rssi_buf[32];
    if (rssi > -100) {
        snprintf(rssi_buf, sizeof(rssi_buf), "信号 %d", rssi);
    } else {
        snprintf(rssi_buf, sizeof(rssi_buf), "搜索中...");
    }
    lv_label_set_text(s_rssi_label, rssi_buf);

    // 更新距离显示
    char dist_buf[32];
    if (rssi > -100) {
        if (dist < 1.0f) {
            snprintf(dist_buf, sizeof(dist_buf), "距离 %.0f cm", dist * 100);
        } else {
            snprintf(dist_buf, sizeof(dist_buf), "距离 %.1f m", dist);
        }
    } else {
        snprintf(dist_buf, sizeof(dist_buf), "距离 --");
    }
    lv_label_set_text(s_dist_label, dist_buf);

    // 更新状态
    const char *status_text;
    lv_color_t heart_color;
    switch (level) {
        case 3:
            status_text = "贴贴中 <3<3<3";
            heart_color = lv_color_hex(0xFF4060);
            break;
        case 2:
            status_text = "很近了 <3<3";
            heart_color = lv_color_hex(0xFF6080);
            break;
        case 1:
            status_text = "感受到你 <3";
            heart_color = lv_color_hex(0xFF80A0);
            break;
        default:
            status_text = "等待对方";
            heart_color = lv_color_hex(0x808080);
            break;
    }
    lv_label_set_text(s_status_label, status_text);
    lv_obj_set_style_text_color(s_heart, heart_color, 0);

    // 更新光晕颜色
    lv_color_t glow_color;
    switch (level) {
        case 3: glow_color = lv_color_hex(0xFF2040); break;
        case 2: glow_color = lv_color_hex(0xFF4060); break;
        case 1: glow_color = lv_color_hex(0xFF6080); break;
        default: glow_color = lv_color_hex(0x404040); break;
    }
    lv_obj_set_style_bg_color(s_glow, glow_color, 0);

    // 更新心跳速率
    update_heartbeat_rate(level);

    // 更新心跳音效
    couple_audio_play_heartbeat(level);
}

void couple_heartbeat_enter(void)
{
    ESP_LOGI(TAG, "进入心动模式");

    s_heart_anim_running = false;

    lv_font_t *font_small = couple_font_get(COUPLE_FONT_SMALL);

    // 创建屏幕
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x0A0A12), 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);

    // 背景光晕（圆形）
    s_glow = lv_obj_create(s_scr);
    lv_obj_set_size(s_glow, 280, 280);
    lv_obj_center(s_glow);
    lv_obj_set_style_radius(s_glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_glow, lv_color_hex(0x404040), 0);
    lv_obj_set_style_bg_opa(s_glow, 20, 0);
    lv_obj_set_style_border_width(s_glow, 0, 0);
    lv_obj_set_style_pad_all(s_glow, 0, 0);

    // 中心心形（使用 <3 符号，放大 2x）
    s_heart = lv_label_create(s_scr);
    lv_label_set_text(s_heart, "<3");
    lv_obj_set_style_text_font(s_heart, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_heart, lv_color_hex(0x808080), 0);
    lv_obj_center(s_heart);
    lv_obj_set_style_transform_scale(s_heart, 512, 0);  // 2.0x 放大

    // 距离标签
    s_dist_label = lv_label_create(s_scr);
    lv_label_set_text(s_dist_label, "距离 --");
    lv_obj_set_style_text_color(s_dist_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(s_dist_label, font_small, 0);
    lv_obj_align(s_dist_label, LV_ALIGN_TOP_MID, 0, 30);

    // 信号强度标签
    s_rssi_label = lv_label_create(s_scr);
    lv_label_set_text(s_rssi_label, "搜索中...");
    lv_obj_set_style_text_color(s_rssi_label, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_font(s_rssi_label, font_small, 0);
    lv_obj_align(s_rssi_label, LV_ALIGN_TOP_MID, 0, 55);

    // 状态标签
    s_status_label = lv_label_create(s_scr);
    lv_label_set_text(s_status_label, "等待对方");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xFF80A0), 0);
    lv_obj_set_style_text_font(s_status_label, font_small, 0);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -50);

    // 提示标签
    lv_obj_t *hint_label = lv_label_create(s_scr);
    lv_label_set_text(hint_label, "OK返回");
    lv_obj_set_style_text_color(hint_label, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(hint_label, font_small, 0);
    lv_obj_align(hint_label, LV_ALIGN_BOTTOM_MID, 0, -15);

    // 启动更新定时器（500ms）
    s_update_timer = lv_timer_create(update_ui, 500, NULL);

    // 显示屏幕
    lv_scr_load(s_scr);
}

void couple_heartbeat_exit(void)
{
    ESP_LOGI(TAG, "退出心动模式");

    if (s_heart_anim_running) {
        lv_anim_del(s_heart, heart_scale_anim_cb);
        lv_anim_del(s_glow, glow_opa_anim_cb);
        s_heart_anim_running = false;
    }

    if (s_update_timer) {
        lv_timer_del(s_update_timer);
        s_update_timer = NULL;
    }

    if (s_scr) {
        lv_obj_del(s_scr);
        s_scr = NULL;
    }

    // 停止心跳音效
    couple_audio_play_heartbeat(0);
}

void couple_heartbeat_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_PRESS) return;

    switch (btn) {
        case BSP_BTN_OK:
            // 用户主动退出：对方离开前不再被自动触发拉回心动模式
            couple_ble_mark_user_exit();
            couple_mode_switch(COUPLE_MODE_BADGE);
            break;
        case BSP_BTN_UP:
            // 需求B：心动模式无手动入口，靠近自动触发贴贴
            break;
        case BSP_BTN_DOWN:
            // 预留：切换显示信息
            break;
        default:
            break;
    }
}
