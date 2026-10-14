// main/couple_ai.c —— AI 语音对话页实现
#include "couple_ai.h"

#include "couple_app.h"
#include "couple_fonts.h"
#include "couple_mimo.h"
#include "couple_wifi.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "couple_ai";

// 识别采样率与单次录音上限。提前结束的录音用静音补齐到声明长度，
// 这样请求体长度始终与 WAV 头一致（服务端按声明长度收流）。
#define COUPLE_AI_SAMPLE_RATE 16000
#define COUPLE_AI_RECORD_SECONDS 6
#define COUPLE_AI_TOTAL_PCM \
    (COUPLE_AI_SAMPLE_RATE * 2 * COUPLE_AI_RECORD_SECONDS)

#define COUPLE_AI_PCM_CHUNK 2048

#define AI_NVS_NAMESPACE "couple_ai"
#define AI_NVS_KEY       "api_key"
#define AI_KEY_MAX       256

#define AI_USER_TEXT_MAX  160
#define AI_REPLY_TEXT_MAX 400

typedef enum {
    AI_STATE_IDLE = 0,     // 待机，等按键
    AI_STATE_LISTENING,    // 录音中
    AI_STATE_THINKING,     // 识别 + 对话请求中
    AI_STATE_SPEAKING,     // 语音播报中
    AI_STATE_ERROR,        // 出错
} ai_state_t;

typedef enum {
    AI_CMD_START = 0,   // 开始一轮对话
    AI_CMD_STOP,        // 结束录音
} ai_cmd_t;

// ============================================================================
// 状态
// ============================================================================
static bool s_active;               // 页面是否在前台
static ai_state_t s_state = AI_STATE_IDLE;
static volatile bool s_stop_requested;
static volatile int s_listen_seconds_left;
static char s_error_text[48];

static char s_user_text[AI_USER_TEXT_MAX];
static char s_reply_text[AI_REPLY_TEXT_MAX];

static QueueHandle_t s_cmd_queue;
static TaskHandle_t s_worker;

static char s_api_key[AI_KEY_MAX];
static bool s_key_loaded;

static lv_obj_t *s_scr;
static lv_obj_t *s_state_label;
static lv_obj_t *s_user_label;
static lv_obj_t *s_reply_label;
static lv_obj_t *s_hint_label;
static lv_timer_t *s_timer;
static int s_dot_phase;

// ============================================================================
// API Key 存储
// ============================================================================
static esp_err_t key_load(void)
{
    if (s_key_loaded) return s_api_key[0] ? ESP_OK : ESP_ERR_NOT_FOUND;
    s_key_loaded = true;

    nvs_handle_t h;
    if (nvs_open(AI_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t len = sizeof(s_api_key);
    esp_err_t err = nvs_get_str(h, AI_NVS_KEY, s_api_key, &len);
    nvs_close(h);
    if (err != ESP_OK || s_api_key[0] == '\0') {
        s_api_key[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t couple_ai_set_api_key(const char *key)
{
    if (!key || key[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (strlen(key) >= sizeof(s_api_key)) return ESP_ERR_INVALID_SIZE;

    nvs_handle_t h;
    esp_err_t err = nvs_open(AI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, AI_NVS_KEY, key);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    snprintf(s_api_key, sizeof(s_api_key), "%s", key);
    s_key_loaded = true;
    ESP_LOGI(TAG, "AI Key 已保存（长度 %u）", (unsigned)strlen(key));
    return ESP_OK;
}

esp_err_t couple_ai_clear_api_key(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(AI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, AI_NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    memset(s_api_key, 0, sizeof(s_api_key));
    s_key_loaded = true;
    ESP_LOGI(TAG, "AI Key 已清除");
    return err;
}

bool couple_ai_has_api_key(void)
{
    return key_load() == ESP_OK;
}

// ============================================================================
// 页面渲染（调用方须持有 LVGL 锁）
// ============================================================================
static void ai_render_state(void)
{
    if (!s_state_label) return;

    switch (s_state) {
    case AI_STATE_IDLE:
        lv_label_set_text(s_state_label, "待机");
        lv_obj_set_style_text_color(s_state_label, lv_color_hex(0x9a9ab8), 0);
        lv_label_set_text(s_hint_label, "短按OK说话\n长按OK返回");
        break;
    case AI_STATE_LISTENING: {
        char buf[32];
        static const char *dots[] = { "", ".", "..", "..." };
        snprintf(buf, sizeof(buf), "聆听中%s %d", dots[s_dot_phase & 3],
                 s_listen_seconds_left > 0 ? s_listen_seconds_left : 0);
        lv_label_set_text(s_state_label, buf);
        lv_obj_set_style_text_color(s_state_label, lv_color_hex(0xff6b9d), 0);
        lv_label_set_text(s_hint_label, "再短按OK结束");
        break;
    }
    case AI_STATE_THINKING:
        lv_label_set_text(s_state_label, "思考中...");
        lv_obj_set_style_text_color(s_state_label, lv_color_hex(0x6bb5ff), 0);
        lv_label_set_text(s_hint_label, "长按OK返回");
        break;
    case AI_STATE_SPEAKING:
        lv_label_set_text(s_state_label, "播放中...");
        lv_obj_set_style_text_color(s_state_label, lv_color_hex(0x7bd88f), 0);
        lv_label_set_text(s_hint_label, "长按OK返回");
        break;
    case AI_STATE_ERROR:
        lv_label_set_text(s_state_label, s_error_text[0] ? s_error_text : "连接失败");
        lv_obj_set_style_text_color(s_state_label, lv_color_hex(0xff6b6b), 0);
        lv_label_set_text(s_hint_label, "短按OK重试\n长按OK返回");
        break;
    }
}

// 页面在前台时刷新界面；否则只更新数据，等下次进入再渲染
static void ai_publish(void)
{
    if (!s_active) return;
    if (!bsp_lvgl_lock(500)) return;
    // 取锁期间页面可能已退出并删除了控件，必须再确认一次
    if (!s_active) {
        bsp_lvgl_unlock();
        return;
    }

    if (s_user_label) {
        lv_label_set_text(s_user_label, s_user_text[0] ? s_user_text : " ");
    }
    if (s_reply_label) {
        lv_label_set_text(s_reply_label, s_reply_text[0] ? s_reply_text : " ");
    }
    ai_render_state();
    bsp_lvgl_unlock();
}

static void ai_set_state(ai_state_t st)
{
    s_state = st;
    ai_publish();
}

static void ai_set_error(const char *text)
{
    snprintf(s_error_text, sizeof(s_error_text), "%s", text ? text : "连接失败");
    ai_set_state(AI_STATE_ERROR);
}

// ============================================================================
// 停顿动画 / 倒计时（LVGL 任务内）
// ============================================================================
static void ai_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_state != AI_STATE_LISTENING) return;
    s_dot_phase = (s_dot_phase + 1) & 3;
    ai_render_state();
}

// ============================================================================
// 语音管线（工作任务内执行）
// ============================================================================
static esp_err_t ai_pcm_out(const uint8_t *pcm, size_t bytes, uint32_t rate,
                            uint8_t channels, uint8_t bits, void *ctx)
{
    bool *format_set = (bool *)ctx;

    if (!*format_set && rate && channels && bits) {
        if (bsp_audio_set_format(rate, bits, channels) != ESP_OK) {
            ESP_LOGE(TAG, "设置播放格式失败");
            return ESP_FAIL;
        }
        bsp_audio_set_volume(g_couple_state.volume);
        *format_set = true;
        ai_set_state(AI_STATE_SPEAKING);
    }
    if (bytes == 0) return ESP_OK;
    return bsp_audio_write(pcm, bytes);
}

static esp_err_t ai_do_record_and_asr(void)
{
    uint32_t total = COUPLE_AI_TOTAL_PCM;

    if (bsp_audio_set_format(COUPLE_AI_SAMPLE_RATE, 16, 1) != ESP_OK) {
        ESP_LOGE(TAG, "麦克风格式设置失败");
        return ESP_FAIL;
    }

    esp_err_t err = couple_mimo_asr_begin(s_api_key, COUPLE_AI_SAMPLE_RATE, 1, total);
    if (err != ESP_OK) return err;

    ai_set_state(AI_STATE_LISTENING);
    s_listen_seconds_left = COUPLE_AI_RECORD_SECONDS;

    static uint8_t buf[COUPLE_AI_PCM_CHUNK];
    uint32_t fed = 0;
    while (fed < total) {
        if (couple_mimo_is_cancelled()) break;
        if (s_stop_requested) break;
        uint32_t want = total - fed;
        if (want > sizeof(buf)) want = sizeof(buf);
        if (bsp_audio_read(buf, want) != ESP_OK) {
            ESP_LOGW(TAG, "读取麦克风失败，提前结束录音");
            break;
        }
        if (couple_mimo_asr_feed(buf, want) != ESP_OK) {
            ESP_LOGW(TAG, "上传录音失败");
            break;
        }
        fed += want;
        s_listen_seconds_left =
            (int)((total - fed) / (COUPLE_AI_SAMPLE_RATE * 2));
    }

    // 提前结束时补静音，保证请求体长度与 WAV 头声明一致
    if (fed < total) {
        memset(buf, 0, sizeof(buf));
        while (fed < total) {
            uint32_t want = total - fed;
            if (want > sizeof(buf)) want = sizeof(buf);
            if (couple_mimo_asr_feed(buf, want) != ESP_OK) break;
            fed += want;
        }
        s_listen_seconds_left = 0;
    }

    ai_set_state(AI_STATE_THINKING);
    char text[AI_USER_TEXT_MAX];
    err = couple_mimo_asr_end(text, sizeof(text));
    if (err == ESP_OK && text[0] != '\0') {
        snprintf(s_user_text, sizeof(s_user_text), "%s", text);
        ai_publish();
    }
    return err;
}

static void ai_run_turn(void)
{
    s_user_text[0] = '\0';
    s_reply_text[0] = '\0';
    s_error_text[0] = '\0';
    s_stop_requested = false;
    couple_mimo_clear_cancel();
    ai_publish();

    if (!couple_ai_has_api_key()) {
        ai_set_error("请先在App配置Key");
        return;
    }
    if (couple_wifi_get_state() != COUPLE_WIFI_STATE_CONNECTED) {
        ai_set_error("请先在App配置Wi-Fi");
        return;
    }

    esp_err_t err = ai_do_record_and_asr();
    if (couple_mimo_is_cancelled()) return;
    if (err == ESP_OK && s_user_text[0] == '\0') {
        ai_set_error("没听清，请再说一次");
        return;
    }
    if (err != ESP_OK) {
        ai_set_error("连接失败");
        return;
    }

    ai_set_state(AI_STATE_THINKING);
    char reply[AI_REPLY_TEXT_MAX];
    err = couple_mimo_chat(s_api_key, s_user_text, reply, sizeof(reply));
    if (couple_mimo_is_cancelled()) return;
    if (err != ESP_OK) {
        ai_set_error("连接失败");
        return;
    }
    snprintf(s_reply_text, sizeof(s_reply_text), "%s", reply);
    ai_publish();

    bool format_set = false;
    err = couple_mimo_tts_stream(s_api_key, reply, ai_pcm_out, &format_set);
    if (couple_mimo_is_cancelled()) return;
    if (err != ESP_OK) {
        // 文本已经拿到并显示了，播报失败不算整轮失败
        ESP_LOGW(TAG, "语音播报失败: %s", esp_err_to_name(err));
    }

    ai_set_state(AI_STATE_IDLE);
}

static void ai_worker(void *arg)
{
    (void)arg;
    ai_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        if (cmd == AI_CMD_START) {
            ai_run_turn();
        } else if (cmd == AI_CMD_STOP) {
            s_stop_requested = true;
        }
    }
}

static esp_err_t ai_post(ai_cmd_t cmd)
{
    if (!s_cmd_queue) return ESP_ERR_INVALID_STATE;
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    return ESP_OK;
}

// ============================================================================
// 页面生命周期
// ============================================================================
void couple_ai_enter(void)
{
    ESP_LOGI(TAG, "进入 AI 对话页");

    s_active = true;
    s_state = AI_STATE_IDLE;
    s_stop_requested = false;
    s_error_text[0] = '\0';
    s_dot_phase = 0;
    s_user_text[0] = '\0';
    s_reply_text[0] = '\0';

    lv_font_t *font_small = couple_font_get(COUPLE_FONT_SMALL);
    lv_font_t *font_normal = couple_font_get(COUPLE_FONT_NORMAL);
    lv_font_t *font_large = couple_font_get(COUPLE_FONT_LARGE);

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "AI 对话");
    lv_obj_set_style_text_font(title, font_normal, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xff6b9d), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    s_state_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_state_label, font_large, 0);
    lv_obj_align(s_state_label, LV_ALIGN_TOP_MID, 0, 46);

    s_user_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_user_label, font_small, 0);
    lv_obj_set_style_text_color(s_user_label, lv_color_hex(0xe0e0e0), 0);
    lv_obj_set_style_text_align(s_user_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_user_label, 200);
    lv_label_set_long_mode(s_user_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_user_label, LV_ALIGN_TOP_MID, 0, 96);

    s_reply_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_reply_label, font_small, 0);
    lv_obj_set_style_text_color(s_reply_label, lv_color_hex(0x9ad8ff), 0);
    lv_obj_set_style_text_align(s_reply_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_reply_label, 200);
    lv_label_set_long_mode(s_reply_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_reply_label, LV_ALIGN_TOP_MID, 0, 140);

    s_hint_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_hint_label, font_small, 0);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0x606080), 0);
    lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, -16);

    ai_render_state();
    lv_scr_load(s_scr);

    s_timer = lv_timer_create(ai_tick, 400, NULL);
}

void couple_ai_exit(void)
{
    ESP_LOGI(TAG, "退出 AI 对话页");

    s_active = false;
    // 让进行中的请求尽快收尾；任务自己会在下一次检查点返回
    couple_mimo_cancel();
    s_stop_requested = true;
    (void)ai_post(AI_CMD_STOP);

    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
    s_state_label = NULL;
    s_user_label = NULL;
    s_reply_label = NULL;
    s_hint_label = NULL;
    s_state = AI_STATE_IDLE;
}

bool couple_ai_is_active(void)
{
    return s_active;
}

// ============================================================================
// 按键（在 LVGL 锁内运行，只投递命令）
// ============================================================================
void couple_ai_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    (void)btn;

    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        // 长按返回工牌页；模式切换会调用 couple_ai_exit 释放本页
        couple_mode_switch(COUPLE_MODE_BADGE);
        return;
    }
    if (ev != BSP_BTN_CLICK || btn != BSP_BTN_OK) return;

    switch (s_state) {
    case AI_STATE_IDLE:
    case AI_STATE_ERROR:
        (void)ai_post(AI_CMD_START);
        break;
    case AI_STATE_LISTENING:
        (void)ai_post(AI_CMD_STOP);
        break;
    case AI_STATE_THINKING:
    case AI_STATE_SPEAKING:
        // 请求/播放中不做打断，避免半截请求；长按可返回
        break;
    }
}

// ============================================================================
// 初始化
// ============================================================================
esp_err_t couple_ai_init(void)
{
    key_load();

    if (s_cmd_queue) return ESP_OK;
    s_cmd_queue = xQueueCreate(4, sizeof(ai_cmd_t));
    if (!s_cmd_queue) return ESP_ERR_NO_MEM;

    // HTTP + JSON 拼装 + 1.5KB 文本缓冲都在这条任务上，栈要给足
    if (xTaskCreate(ai_worker, "couple_ai", 8192, NULL, 4, &s_worker) != pdPASS) {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
