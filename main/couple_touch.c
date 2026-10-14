// main/couple_touch.c —— 贴贴模式：5秒爱意动画
#include "couple_app.h"
#include "couple_fonts.h"
#include "esp_log.h"
#include "lvgl.h"
#include <math.h>
#include <string.h>

static const char *TAG = "couple_touch";

static lv_obj_t *s_scr;
static lv_obj_t *s_heart_left;
static lv_obj_t *s_heart_right;
static lv_obj_t *s_heart_big;
static lv_obj_t *s_particles[12];
static lv_obj_t *s_msg_label;
static lv_timer_t *s_anim_timer;
static int s_anim_phase;
static uint32_t s_anim_start_ms;

#define PARTICLE_COUNT 12
#define PI 3.14159265358979f

static void anim_tick(lv_timer_t *timer)
{
    (void)timer;
    uint32_t elapsed = lv_tick_elaps(s_anim_start_ms);

    if (elapsed < 1000) {
        // 阶段 0: 两颗心飞入
        if (s_anim_phase != 0) {
            s_anim_phase = 0;
            lv_obj_add_flag(s_heart_big, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_heart_left, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_heart_right, LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < PARTICLE_COUNT; i++) {
                lv_obj_add_flag(s_particles[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        float t = (float)elapsed / 1000.0f;
        float ease = t * t * (3 - 2 * t);
        int left_x = -40 + (int)((120 + 40) * ease);
        int right_x = 280 - (int)((280 - 120) * ease);
        lv_obj_set_x(s_heart_left, left_x);
        lv_obj_set_x(s_heart_right, right_x);

    } else if (elapsed < 2000) {
        // 阶段 1: 碰撞 + 融合
        if (s_anim_phase != 1) {
            s_anim_phase = 1;
            lv_obj_add_flag(s_heart_left, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_heart_right, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_heart_big, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_transform_scale(s_heart_big, 700, 0);
        }
        float t = (float)(elapsed - 1000) / 1000.0f;
        int scale = 700 - (int)((700 - 400) * t);
        lv_obj_set_style_transform_scale(s_heart_big, scale, 0);

    } else if (elapsed < 4000) {
        // 阶段 2: 大心跳动 + 粒子
        if (s_anim_phase != 2) {
            s_anim_phase = 2;
            for (int i = 0; i < PARTICLE_COUNT; i++) {
                lv_obj_clear_flag(s_particles[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        float beat_t = fmodf((float)(elapsed - 2000) / 500.0f, 1.0f);
        float beat_scale;
        if (beat_t < 0.3f) {
            beat_scale = 1.0f + 0.2f * (beat_t / 0.3f);
        } else {
            beat_scale = 1.2f - 0.2f * ((beat_t - 0.3f) / 0.7f);
        }
        lv_obj_set_style_transform_scale(s_heart_big, (int)(beat_scale * 400), 0);

        // 粒子向外扩散
        float particle_t = (float)(elapsed - 2000) / 2000.0f;
        for (int i = 0; i < PARTICLE_COUNT; i++) {
            float angle = (float)i / PARTICLE_COUNT * 2 * PI;
            float dist = 20.0f + particle_t * 100.0f;
            int px = 120 + (int)(cosf(angle) * dist) - 4;
            int py = 160 + (int)(sinf(angle) * dist) - 4;
            lv_obj_set_pos(s_particles[i], px, py);
            int opa = (int)((1.0f - particle_t) * 200);
            lv_obj_set_style_bg_opa(s_particles[i], opa, 0);
        }

    } else if (elapsed < 5000) {
        // 阶段 3: 渐隐
        if (s_anim_phase != 3) {
            s_anim_phase = 3;
            lv_label_set_text(s_msg_label, "<3 贴贴 <3");
        }
        float t = (float)(elapsed - 4000) / 1000.0f;
        int opa = (int)((1.0f - t) * 255);
        lv_obj_set_style_text_opa(s_heart_big, opa, 0);
        lv_obj_set_style_text_opa(s_msg_label, opa, 0);

    } else {
        // 动画结束，返回心动模式
        ESP_LOGI(TAG, "贴贴动画完成，返回心动模式");
        lv_timer_del(s_anim_timer);
        s_anim_timer = NULL;
        couple_mode_switch(COUPLE_MODE_HEARTBEAT);
    }
}

void couple_touch_enter(void)
{
    ESP_LOGI(TAG, "进入贴贴模式");

    s_anim_phase = -1;

    // 创建屏幕
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x1A0A1A), 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);

    // 左心
    s_heart_left = lv_label_create(s_scr);
    lv_label_set_text(s_heart_left, "<3");
    lv_obj_set_style_text_font(s_heart_left, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_heart_left, lv_color_hex(0xFF6080), 0);
    lv_obj_set_size(s_heart_left, 60, 40);
    lv_obj_set_pos(s_heart_left, -40, 140);
    lv_obj_set_style_text_align(s_heart_left, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_transform_scale(s_heart_left, 384, 0);

    // 右心
    s_heart_right = lv_label_create(s_scr);
    lv_label_set_text(s_heart_right, "<3");
    lv_obj_set_style_text_font(s_heart_right, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_heart_right, lv_color_hex(0xFF80A0), 0);
    lv_obj_set_size(s_heart_right, 60, 40);
    lv_obj_set_pos(s_heart_right, 260, 140);
    lv_obj_set_style_text_align(s_heart_right, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_transform_scale(s_heart_right, 384, 0);

    // 融合大心
    s_heart_big = lv_label_create(s_scr);
    lv_label_set_text(s_heart_big, "<3");
    lv_obj_set_style_text_font(s_heart_big, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_heart_big, lv_color_hex(0xFF4060), 0);
    lv_obj_center(s_heart_big);
    lv_obj_set_style_transform_scale(s_heart_big, 400, 0);
    lv_obj_add_flag(s_heart_big, LV_OBJ_FLAG_HIDDEN);

    // 粒子
    for (int i = 0; i < PARTICLE_COUNT; i++) {
        s_particles[i] = lv_obj_create(s_scr);
        lv_obj_set_size(s_particles[i], 8, 8);
        lv_obj_set_style_radius(s_particles[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_particles[i], lv_color_hex(0xFF80A0), 0);
        lv_obj_set_style_bg_opa(s_particles[i], 200, 0);
        lv_obj_set_style_border_width(s_particles[i], 0, 0);
        lv_obj_set_style_pad_all(s_particles[i], 0, 0);
        lv_obj_set_pos(s_particles[i], 116, 156);
        lv_obj_add_flag(s_particles[i], LV_OBJ_FLAG_HIDDEN);
    }

    // 文字提示
    s_msg_label = lv_label_create(s_scr);
    lv_label_set_text(s_msg_label, "贴贴中...");
    lv_obj_set_style_text_color(s_msg_label, lv_color_hex(0xFFB0C0), 0);
    lv_obj_set_style_text_font(s_msg_label, couple_font_get(COUPLE_FONT_LARGE), 0);
    lv_obj_align(s_msg_label, LV_ALIGN_BOTTOM_MID, 0, -80);

    // 启动动画定时器
    s_anim_start_ms = lv_tick_get();
    s_anim_timer = lv_timer_create(anim_tick, 33, NULL);

    // 显示屏幕
    lv_scr_load(s_scr);
}

void couple_touch_exit(void)
{
    ESP_LOGI(TAG, "退出贴贴模式");

    if (s_anim_timer) {
        lv_timer_del(s_anim_timer);
        s_anim_timer = NULL;
    }

    if (s_scr) {
        lv_obj_del(s_scr);
        s_scr = NULL;
    }
}

void couple_touch_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_PRESS) return;

    // 贴贴模式下按 OK 键可提前退出
    if (btn == BSP_BTN_OK) {
        couple_mode_switch(COUPLE_MODE_HEARTBEAT);
    }
}
