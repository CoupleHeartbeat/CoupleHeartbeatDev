// main/couple_badge.c —— 工牌模式：时间 + 姓名 + 电量
#include "couple_app.h"
#include "couple_fonts.h"
#include "couple_gatt.h"
#include "bsp_display.h"
#include "bsp_battery.h"
#include "esp_log.h"
#include "lvgl.h"
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
static lv_timer_t *s_timer;
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
static int s_menu_sel;     // 0=关于设备 1=恢复出厂设置
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
        lv_label_set_text_fmt(s_panel_items, "%s 关于设备\n%s 恢复出厂设置",
                              s_menu_sel == 0 ? ">" : " ",
                              s_menu_sel == 1 ? ">" : " ");
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

    // 更新日期
    if (s_date_label) {
        char buf[32];
        const char *weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
        snprintf(buf, sizeof(buf), "%d月%d日 周%s",
                 timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 weekdays[timeinfo.tm_wday]);
        lv_label_set_text(s_date_label, buf);
    }

    // 每 30 秒更新一次电量
    static int count = 0;
    if (++count >= 30) {
        count = 0;
        update_battery();
    }

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
    lv_label_set_text(s_date_label, "--月--日");

    // 分隔线
    lv_obj_t *line = lv_obj_create(s_scr);
    lv_obj_set_size(line, 160, 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x3a3a5e), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 145);

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

    lv_scr_load(s_scr);

    // 初始更新
    update_battery();

    // 启动定时器（1秒刷新）
    s_timer = lv_timer_create(tick, 1000, NULL);
}

void couple_badge_exit(void)
{
    ESP_LOGI(TAG, "退出工牌模式");

    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
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
            s_menu_sel = !s_menu_sel;
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

// 设置菜单（含关于设备 / 恢复出厂确认）打开时，靠近自动切换须让路
bool couple_badge_menu_open(void)
{
    return s_ui != UI_IDLE;
}
