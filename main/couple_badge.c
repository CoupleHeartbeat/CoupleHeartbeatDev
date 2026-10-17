// main/couple_badge.c —— 工牌模式：时间 + 姓名 + 电量 + 靠近光效
#include "couple_app.h"
#include "couple_auto.h"
#include "couple_fonts.h"
#include "couple_gatt.h"
#include "couple_power.h"
#include "couple_wifi.h"
#include "bsp_display.h"
#include "bsp_battery.h"
#include "esp_log.h"
#include "lvgl.h"
#include <stdio.h>
#include <time.h>
#include <string.h>

static const char *TAG = "couple_badge";

static lv_obj_t *s_scr;
static lv_obj_t *s_time_label;
static lv_obj_t *s_date_label;
static lv_obj_t *s_name_label;
static lv_obj_t *s_battery_label;
static lv_obj_t *s_status_label;
static lv_obj_t *s_hint_label;      // 底部提示（菜单打开时隐藏）
static lv_obj_t *s_prox_label;      // 靠近时的一行距离/信号
static lv_obj_t *s_glow_frame;      // 四周边框光效
static lv_obj_t *s_wifi_icon;       // 右上角 Wi-Fi 图标容器
static lv_obj_t *s_wifi_arc1;       // 外圈
static lv_obj_t *s_wifi_arc2;       // 内圈
static lv_obj_t *s_wifi_dot;        // 扇形焦点
static lv_obj_t *s_divider;         // 时间/姓名之间的分隔线（低功耗屏需隐藏）
static lv_timer_t *s_timer;
static lv_timer_t *s_prox_timer;
static lv_anim_t s_glow_anim;
static bool s_glow_anim_running;
static couple_glow_level_t s_glow_level = COUPLE_GLOW_OFF;
static int s_audio_level = -1;      // 仅档位变化时才重设心跳音效
static int s_battery_soc;
static int s_status_restore_ticks;  // >0：临时提示倒计时，到 0 后恢复状态显示

// 设置菜单 UI 状态机（仅工牌页内的层级，不改变 couple_mode_t）
typedef enum {
    UI_IDLE = 0,  // 工牌首页
    UI_MENU,      // 设置菜单：关于设备 / 恢复出厂设置
    UI_ABOUT,     // 关于设备
    UI_CONFIRM,   // 恢复出厂二次确认
} ui_state_t;

static ui_state_t s_ui = UI_IDLE;
static int s_menu_sel;     // 0=关于设备 1=立即休眠 2=恢复出厂设置
static int s_confirm_sel;  // 0=确认 1=取消（默认取消，防误触）
static lv_obj_t *s_panel;
static lv_obj_t *s_panel_title;
static lv_obj_t *s_panel_items;
static lv_obj_t *s_panel_hint;

// 恢复状态栏正常内容（角色 / 未配对）
static void refresh_status(void)
{
    if (!s_status_label) return;
    if (g_couple_state.pair_info.paired) {
        lv_label_set_text_fmt(s_status_label, "%c角色",
                              g_couple_state.pair_info.role);
    } else {
        lv_label_set_text(s_status_label, "未配对");
    }
}

// 中央名称区域：已配对显示工牌名，未配对显示配对码（供 App 输入）
static void refresh_name(void)
{
    if (!s_name_label) return;
    if (g_couple_state.pair_info.paired &&
        g_couple_state.pair_info.badge_name[0] != '\0') {
        lv_label_set_text(s_name_label, g_couple_state.pair_info.badge_name);
    } else if (g_couple_state.pair_info.paired) {
        lv_label_set_text(s_name_label, "AI PASSPORT");
    } else {
        lv_label_set_text_fmt(s_name_label, "配对码:%04u",
                              couple_gatt_get_pair_code());
    }
}

// 显示临时提示文字，几秒后由 tick 恢复
static void set_status_hint(const char *text)
{
    if (s_status_label) {
        lv_label_set_text(s_status_label, text);
    }
    s_status_restore_ticks = 3;
}

static void update_battery(void)
{
    int soc = bsp_battery_soc();
    if (soc < 0) soc = 0;
    if (soc > 100) soc = 100;
    s_battery_soc = soc;

    if (s_battery_label) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", soc);
        lv_label_set_text(s_battery_label, buf);
    }
}

// ============================================================================
// 右上角 Wi-Fi 图标：三段扇形 + 焦点圆点，颜色表示联网状态
//
// 图标用 LVGL 的 arc 画（字体里没有 Wi-Fi 字形，不能靠文字）。
// 三段对象同心，圆心都落在容器的底边中点，只画上方 225°~315° 的扇形，
// 因此实际墨迹全部落在容器内部。
// ============================================================================
static lv_obj_t *make_wifi_arc(lv_obj_t *parent, int size)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_bg_angles(arc, 225, 315);
    lv_obj_set_style_arc_width(arc, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(arc, 0, LV_PART_MAIN);
    // 指示弧整段隐藏：只留 LV_PART_MAIN 那一段圆弧
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_size(arc, size, size);
    lv_obj_align(arc, LV_ALIGN_BOTTOM_MID, 0, size / 2);
    return arc;
}

static void build_wifi_icon(lv_obj_t *parent)
{
    s_wifi_icon = lv_obj_create(parent);
    lv_obj_set_size(s_wifi_icon, 24, 14);
    lv_obj_align(s_wifi_icon, LV_ALIGN_TOP_RIGHT, -56, 10);
    lv_obj_set_style_bg_opa(s_wifi_icon, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wifi_icon, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_icon, 0, 0);
    lv_obj_clear_flag(s_wifi_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_wifi_icon, LV_OBJ_FLAG_CLICKABLE);

    s_wifi_arc1 = make_wifi_arc(s_wifi_icon, 20);
    s_wifi_arc2 = make_wifi_arc(s_wifi_icon, 12);

    s_wifi_dot = lv_obj_create(s_wifi_icon);
    lv_obj_set_size(s_wifi_dot, 4, 4);
    lv_obj_set_style_radius(s_wifi_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_wifi_dot, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_dot, 0, 0);
    lv_obj_clear_flag(s_wifi_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_wifi_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_wifi_dot, LV_ALIGN_BOTTOM_MID, 0, 0);
}

// 按配网状态给图标上色：已连接=主题粉，连接中=灰蓝，失败=红，未配置=暗灰
static void update_wifi_icon(void)
{
    if (!s_wifi_arc1) return;

    uint32_t color;
    lv_opa_t opa;
    switch (couple_wifi_get_state()) {
    case COUPLE_WIFI_STATE_CONNECTED:
        color = 0xff6b9d; opa = LV_OPA_COVER;
        break;
    case COUPLE_WIFI_STATE_CONNECTING:
        color = 0x9a9ab8; opa = LV_OPA_80;
        break;
    case COUPLE_WIFI_STATE_FAILED:
        color = 0xff6b6b; opa = LV_OPA_COVER;
        break;
    default:
        color = 0x50506a; opa = LV_OPA_50;
        break;
    }

    lv_obj_t *parts[2] = { s_wifi_arc1, s_wifi_arc2 };
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_arc_color(parts[i], lv_color_hex(color), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(parts[i], opa, LV_PART_MAIN);
    }
    if (s_wifi_dot) {
        lv_obj_set_style_bg_color(s_wifi_dot, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(s_wifi_dot, opa, 0);
    }
}

// ============================================================================
// 靠近光效：靠近不切页，只在工牌页四周点亮呼吸边框
// ============================================================================
static float rssi_to_meters(int8_t rssi)
{
    if (rssi >= -40) return 0.3f;
    if (rssi >= -50) return 0.8f;
    if (rssi >= -60) return 2.0f;
    if (rssi >= -70) return 5.0f;
    if (rssi >= -80) return 10.0f;
    return 20.0f;
}

static void glow_opa_anim_cb(void *var, int32_t v)
{
    lv_obj_set_style_border_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void stop_glow_anim(void)
{
    if (s_glow_anim_running) {
        lv_anim_del(s_glow_frame, glow_opa_anim_cb);
        s_glow_anim_running = false;
    }
}

// 按档位调整边框宽度/颜色/呼吸节奏；档位不变时不做任何事
static void apply_glow(couple_glow_level_t level)
{
    if (level == s_glow_level) return;
    s_glow_level = level;
    if (!s_glow_frame) return;

    stop_glow_anim();

    if (level == COUPLE_GLOW_OFF) {
        lv_obj_set_style_border_opa(s_glow_frame, LV_OPA_TRANSP, 0);
        return;
    }

    uint32_t period_ms;
    int32_t width;
    uint32_t color;
    int32_t opa_min;
    int32_t opa_max;
    switch (level) {
    case COUPLE_GLOW_TOUCH:
        period_ms = 600;  width = 8; color = 0xFF4060; opa_min = 110; opa_max = 255;
        break;
    case COUPLE_GLOW_CLOSE:
        period_ms = 1000; width = 6; color = 0xFF6080; opa_min = 80;  opa_max = 220;
        break;
    default:
        period_ms = 1600; width = 5; color = 0xFF80A0; opa_min = 50;  opa_max = 160;
        break;
    }

    lv_obj_set_style_border_width(s_glow_frame, width, 0);
    lv_obj_set_style_border_color(s_glow_frame, lv_color_hex(color), 0);

    lv_anim_init(&s_glow_anim);
    lv_anim_set_var(&s_glow_anim, s_glow_frame);
    lv_anim_set_exec_cb(&s_glow_anim, glow_opa_anim_cb);
    lv_anim_set_values(&s_glow_anim, opa_min, opa_max);
    lv_anim_set_time(&s_glow_anim, period_ms / 2);
    lv_anim_set_playback_time(&s_glow_anim, period_ms / 2);
    lv_anim_set_repeat_count(&s_glow_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&s_glow_anim, lv_anim_path_ease_in_out);
    lv_anim_start(&s_glow_anim);
    s_glow_anim_running = true;
}

// 底部一行小字：距离 + 信号；对方不在时整行隐藏
static void update_prox_label(void)
{
    if (!s_prox_label) return;

    if (!g_couple_state.pair_info.paired || !g_couple_state.partner_connected) {
        lv_obj_add_flag(s_prox_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(s_prox_label, LV_OBJ_FLAG_HIDDEN);

    int8_t rssi = g_couple_state.partner_rssi;
    float dist = rssi_to_meters(rssi);
    char buf[48];
    if (dist < 1.0f) {
        snprintf(buf, sizeof(buf), "距离 %.0f cm  信号 %d", dist * 100.0f, rssi);
    } else {
        snprintf(buf, sizeof(buf), "距离 %.1f m  信号 %d", dist, rssi);
    }
    lv_label_set_text(s_prox_label, buf);
}

// 500ms 轮询：刷新档位、距离文字与心跳音效
static void proximity_tick(lv_timer_t *timer)
{
    (void)timer;

    couple_auto_input_t in = {
        .paired = g_couple_state.pair_info.paired,
        .partner_present = g_couple_state.partner_connected,
        .rssi = g_couple_state.partner_rssi,
        .very_close_count = couple_ble_very_close_count(),
    };
    couple_glow_level_t level = couple_auto_decide(&in);

    apply_glow(level);
    update_prox_label();

    if ((int)level != s_audio_level) {
        s_audio_level = (int)level;
        couple_audio_play_heartbeat((uint8_t)level);
    }
}

// 执行恢复出厂：清除配对、重新开放配对码
static void do_factory_reset(void)
{
    couple_storage_clear_pair();
    memset(&g_couple_state.pair_info, 0, sizeof(g_couple_state.pair_info));
    couple_gatt_new_pair_code();
    refresh_status();
    refresh_name();
    ESP_LOGI(TAG, "恢复出厂设置完成");
}

// 关闭菜单面板，回到工牌首页
static void panel_close(void)
{
    if (s_panel) {
        lv_obj_delete(s_panel);  // 标题/内容/提示均为其子对象，一并删除
        s_panel = NULL;
        s_panel_title = NULL;
        s_panel_items = NULL;
        s_panel_hint = NULL;
    }
    s_ui = UI_IDLE;
    if (s_hint_label) {
        lv_obj_clear_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
    }
}

// 按当前状态刷新面板内容（标题 / 选项 / 操作提示）
static void panel_render(void)
{
    if (!s_panel) return;
    switch (s_ui) {
    case UI_MENU:
        lv_label_set_text(s_panel_title, "设置");
        lv_label_set_text_fmt(s_panel_items,
                              "%s 关于设备\n%s 立即休眠\n%s 恢复出厂设置",
                              s_menu_sel == 0 ? ">" : " ",
                              s_menu_sel == 1 ? ">" : " ",
                              s_menu_sel == 2 ? ">" : " ");
        lv_label_set_text(s_panel_hint, "↑↓选择  OK确认\n长按OK返回");
        break;
    case UI_ABOUT: {
        lv_label_set_text(s_panel_title, "关于设备");
        const char *name = "AI PASSPORT";
        if (g_couple_state.pair_info.badge_name[0] != '\0') {
            name = g_couple_state.pair_info.badge_name;
        }
        lv_label_set_text_fmt(s_panel_items, "名称:%s\n固件: v1.0.0", name);
        lv_label_set_text(s_panel_hint, "OK返回");
        break;
    }
    case UI_CONFIRM:
        lv_label_set_text(s_panel_title, "恢复出厂设置");
        lv_label_set_text_fmt(s_panel_items, "%s 确认\n%s 取消",
                              s_confirm_sel == 0 ? ">" : " ",
                              s_confirm_sel == 1 ? ">" : " ");
        lv_label_set_text(s_panel_hint, "↑↓选择  OK确认\n长按OK返回");
        break;
    default:
        break;
    }
}

// 打开菜单面板并渲染指定状态
static void panel_open(ui_state_t st)
{
    s_ui = st;
    if (!s_panel) {
        lv_font_t *font_small = couple_font_get(COUPLE_FONT_SMALL);
        lv_font_t *font_normal = couple_font_get(COUPLE_FONT_NORMAL);

        if (s_hint_label) {
            lv_obj_add_flag(s_hint_label, LV_OBJ_FLAG_HIDDEN);
        }

        s_panel = lv_obj_create(s_scr);
        lv_obj_set_size(s_panel, 216, 190);
        lv_obj_center(s_panel);
        lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x24244a), 0);
        lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_panel, 1, 0);
        lv_obj_set_style_border_color(s_panel, lv_color_hex(0x3a3a5e), 0);
        lv_obj_set_style_radius(s_panel, 12, 0);
        lv_obj_set_style_pad_all(s_panel, 12, 0);
        lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

        s_panel_title = lv_label_create(s_panel);
        lv_obj_set_style_text_font(s_panel_title, font_normal, 0);
        lv_obj_set_style_text_color(s_panel_title, lv_color_hex(0xff6b9d), 0);
        lv_obj_align(s_panel_title, LV_ALIGN_TOP_MID, 0, 4);

        s_panel_items = lv_label_create(s_panel);
        lv_obj_set_style_text_font(s_panel_items, font_small, 0);
        lv_obj_set_style_text_color(s_panel_items, lv_color_hex(0xe0e0e0), 0);
        lv_obj_set_style_text_line_space(s_panel_items, 6, 0);
        lv_obj_align(s_panel_items, LV_ALIGN_CENTER, 0, 0);

        s_panel_hint = lv_label_create(s_panel);
        lv_obj_set_style_text_font(s_panel_hint, font_small, 0);
        lv_obj_set_style_text_color(s_panel_hint, lv_color_hex(0x606080), 0);
        lv_obj_set_style_text_align(s_panel_hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(s_panel_hint, 4, 0);
        lv_obj_align(s_panel_hint, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    panel_render();
}

static void tick(lv_timer_t *timer)
{
    (void)timer;

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // 更新时间
    if (s_time_label) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d",
                 timeinfo.tm_hour, timeinfo.tm_min);
        lv_label_set_text(s_time_label, buf);
    }

    // 更新日期（含年份）
    if (s_date_label) {
        char buf[40];
        const char *weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
        snprintf(buf, sizeof(buf), "%d年%d月%d日 周%s",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 weekdays[timeinfo.tm_wday]);
        lv_label_set_text(s_date_label, buf);
    }

    // 每 30 秒更新一次电量
    static int count = 0;
    if (++count >= 30) {
        count = 0;
        update_battery();
    }

    // Wi-Fi 状态变化（配网/断线）要尽快反映到右上角图标
    update_wifi_icon();

    // 临时提示倒计时结束后恢复状态显示
    if (s_status_restore_ticks > 0 && --s_status_restore_ticks == 0) {
        refresh_status();
    }

    // App 配对完成后刷新界面（配对由 GATT 写入完成）
    static bool s_last_paired = false;
    bool paired = g_couple_state.pair_info.paired;
    if (paired != s_last_paired) {
        s_last_paired = paired;
        refresh_status();
        refresh_name();
    }
}

void couple_badge_enter(void)
{
    ESP_LOGI(TAG, "进入工牌模式");

    s_status_restore_ticks = 0;

    lv_font_t *font_small = couple_font_get(COUPLE_FONT_SMALL);
    lv_font_t *font_normal = couple_font_get(COUPLE_FONT_NORMAL);
    lv_font_t *font_large = couple_font_get(COUPLE_FONT_LARGE);

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);

    // 顶部状态栏 - 电量
    s_battery_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_battery_label, font_small, 0);
    lv_obj_set_style_text_color(s_battery_label, lv_color_hex(0xe0e0e0), 0);
    lv_obj_align(s_battery_label, LV_ALIGN_TOP_RIGHT, -12, 8);
    lv_label_set_text(s_battery_label, "--%");

    // Wi-Fi 图标：紧邻电量左侧
    build_wifi_icon(s_scr);
    update_wifi_icon();

    // 角色标识
    s_status_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_status_label, font_small, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xff6b9d), 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, 12, 8);
    refresh_status();

    // 时间显示（大号）
    s_time_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_time_label, font_large, 0);
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(0xffffff), 0);
    lv_obj_align(s_time_label, LV_ALIGN_TOP_MID, 0, 70);
    lv_label_set_text(s_time_label, "--:--");

    // 日期
    s_date_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_date_label, font_normal, 0);
    lv_obj_set_style_text_color(s_date_label, lv_color_hex(0xa0a0a0), 0);
    lv_obj_align(s_date_label, LV_ALIGN_TOP_MID, 0, 110);
    lv_label_set_text(s_date_label, "--年--月--日");

    // 分隔线
    s_divider = lv_obj_create(s_scr);
    lv_obj_set_size(s_divider, 160, 1);
    lv_obj_set_style_bg_color(s_divider, lv_color_hex(0x3a3a5e), 0);
    lv_obj_set_style_bg_opa(s_divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_divider, 0, 0);
    lv_obj_align(s_divider, LV_ALIGN_TOP_MID, 0, 145);

    // 姓名/工牌名称
    s_name_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_name_label, font_normal, 0);
    lv_obj_set_style_text_color(s_name_label, lv_color_hex(0xff6b9d), 0);
    lv_obj_align(s_name_label, LV_ALIGN_TOP_MID, 0, 165);
    refresh_name();

    // 底部提示
    s_hint_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_hint_label, font_small, 0);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0x606080), 0);
    lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_label_set_text(s_hint_label, "短按OK设置 ↑↓音量\n长按OK对话");

    // 靠近时的一行距离/信号（默认隐藏，对方在附近才显示）
    s_prox_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_prox_label, font_small, 0);
    lv_obj_set_style_text_color(s_prox_label, lv_color_hex(0xff8fb0), 0);
    lv_obj_align(s_prox_label, LV_ALIGN_BOTTOM_MID, 0, -62);
    lv_obj_add_flag(s_prox_label, LV_OBJ_FLAG_HIDDEN);

    // 四周边框光效：整屏大小的透明对象，只画边框，靠近时呼吸
    s_glow_frame = lv_obj_create(s_scr);
    lv_obj_set_size(s_glow_frame, LV_PCT(100), LV_PCT(100));
    lv_obj_center(s_glow_frame);
    lv_obj_set_style_bg_opa(s_glow_frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_glow_frame, 0, 0);
    lv_obj_set_style_border_width(s_glow_frame, 0, 0);
    lv_obj_set_style_border_color(s_glow_frame, lv_color_hex(0xff80a0), 0);
    lv_obj_set_style_border_opa(s_glow_frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_glow_frame, 0, 0);
    lv_obj_clear_flag(s_glow_frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_glow_frame, LV_OBJ_FLAG_CLICKABLE);

    lv_scr_load(s_scr);

    // 初始更新
    update_battery();

    // 启动定时器（1秒刷新）
    s_timer = lv_timer_create(tick, 1000, NULL);

    // 靠近光效：进入时先按当前状态点亮一次，再交给 500ms 轮询
    s_glow_level = COUPLE_GLOW_OFF;
    s_audio_level = -1;
    proximity_tick(NULL);
    s_prox_timer = lv_timer_create(proximity_tick, 500, NULL);
}

void couple_badge_exit(void)
{
    ESP_LOGI(TAG, "退出工牌模式");

    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_prox_timer) {
        lv_timer_delete(s_prox_timer);
        s_prox_timer = NULL;
    }

    // 动画引用光效对象，必须在删屏之前停掉
    stop_glow_anim();
    couple_audio_play_heartbeat(0);
    s_glow_level = COUPLE_GLOW_OFF;
    s_audio_level = -1;

    if (s_scr) {
        // 菜单面板是 s_scr 的子对象，会随屏幕一并删除，这里只清指针
        s_panel = NULL;
        s_panel_title = NULL;
        s_panel_items = NULL;
        s_panel_hint = NULL;
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_time_label = NULL;
        s_date_label = NULL;
        s_name_label = NULL;
        s_battery_label = NULL;
        s_status_label = NULL;
        s_hint_label = NULL;
        s_prox_label = NULL;
        s_glow_frame = NULL;
        s_wifi_icon = NULL;
        s_wifi_arc1 = NULL;
        s_wifi_arc2 = NULL;
        s_wifi_dot = NULL;
        s_divider = NULL;
    }
    s_ui = UI_IDLE;
}

void couple_badge_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    // ---- 长按 OK ----
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        if (s_ui == UI_IDLE) {
            // 长按 OK 进入 AI 对话页（模式切换负责退出本页）
            couple_mode_switch(COUPLE_MODE_AI);
            return;
        }
        // 菜单层级内：长按 = 返回上一级（确认页等价于取消）
        if (s_ui == UI_MENU) {
            panel_close();
        } else {
            panel_open(UI_MENU);
        }
        return;
    }

    if (ev != BSP_BTN_CLICK) return;

    // ---- 上下键 ----
    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        int delta = (btn == BSP_BTN_UP) ? 1 : -1;
        if (s_ui == UI_IDLE) {
            // 首页：调音量
            int vol = g_couple_state.volume + delta * 10;
            if (vol < 0) vol = 0;
            if (vol > 100) vol = 100;
            if (vol != g_couple_state.volume) {
                g_couple_state.volume = vol;
                couple_audio_set_volume(vol);
                if (s_status_label) {
                    lv_label_set_text_fmt(s_status_label, "音量%d%%", vol);
                    s_status_restore_ticks = 3;
                }
            }
        } else if (s_ui == UI_MENU) {
            s_menu_sel = (s_menu_sel + 1) % 3;
            panel_render();
        } else if (s_ui == UI_CONFIRM) {
            s_confirm_sel = !s_confirm_sel;
            panel_render();
        }
        return;
    }

    if (btn != BSP_BTN_OK) return;

    // ---- OK 短按 ----
    switch (s_ui) {
    case UI_IDLE:
        // 进入设置菜单
        s_menu_sel = 0;
        panel_open(UI_MENU);
        break;
    case UI_MENU:
        if (s_menu_sel == 0) {
            panel_open(UI_ABOUT);
        } else if (s_menu_sel == 1) {
            // 立即休眠：先收起菜单，低功耗管理器随后显示"即将休眠"并进入深睡
            panel_close();
            couple_power_request_sleep_now();
        } else {
            s_confirm_sel = 1;  // 默认停在"取消"，防误触
            panel_open(UI_CONFIRM);
        }
        break;
    case UI_ABOUT:
        panel_open(UI_MENU);
        break;
    case UI_CONFIRM:
        if (s_confirm_sel == 0) {
            // 二次确认通过：执行恢复出厂并回首页
            do_factory_reset();
            panel_close();
            set_status_hint("已恢复出厂");
        } else {
            panel_open(UI_MENU);
        }
        break;
    }
}

void couple_badge_update_time(void)
{
    if (s_timer) {
        tick(s_timer);
    }
}

// ============================================================================
// 低功耗屏：只显示姓名(大字) + 时间 + 年月日
//
// 无操作 60s 息屏后不再全黑，而是调暗背光并切到这块极简屏；15 分钟无操作仍
// 会进入深睡，因此这点背光功耗可以接受。唤醒时恢复完整工牌页。
// 必须在持有 LVGL 锁时调用。
// ============================================================================
void couple_badge_set_minimal(bool minimal)
{
    if (!s_scr) return;

    lv_font_t *font_normal = couple_font_get(COUPLE_FONT_NORMAL);
    lv_font_t *font_large = couple_font_get(COUPLE_FONT_LARGE);

    if (s_name_label) {
        lv_obj_set_style_text_font(s_name_label,
                                   minimal ? font_large : font_normal, 0);
        lv_obj_align(s_name_label, LV_ALIGN_TOP_MID, 0, minimal ? 86 : 165);
    }
    if (s_time_label) {
        lv_obj_align(s_time_label, LV_ALIGN_TOP_MID, 0, minimal ? 140 : 70);
    }
    if (s_date_label) {
        lv_obj_align(s_date_label, LV_ALIGN_TOP_MID, 0, minimal ? 182 : 110);
    }

    lv_obj_t *others[] = {
        s_battery_label, s_status_label, s_wifi_icon, s_divider,
        s_hint_label, s_prox_label, s_glow_frame,
    };
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        if (!others[i]) continue;
        if (minimal) {
            lv_obj_add_flag(others[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            // 靠近提示/边框光效的显隐由 proximity_tick 每 500ms 重新决定，
            // 这里只需解除隐藏标志
            lv_obj_clear_flag(others[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    ESP_LOGI(TAG, "低功耗屏 %s", minimal ? "开启" : "关闭");
}
