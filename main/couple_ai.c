// main/couple_ai.c —— AI 语音对话页实现
//
// 设备不再自行联网：录音时边采边经 BLE 0xFFE9 推给手机，手机完成 ASR 与
// 大模型请求后通过 0xFFEA 回传文本，本页只负责录音、状态反馈与展示回复。
#include "couple_ai.h"

#include "couple_app.h"
#include "couple_fonts.h"
#include "couple_gatt.h"
#include "couple_voice.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "couple_ai";

// 手机端按此格式构造 WAV 送 ASR：8 kHz / 16 bit / 单声道。
#define COUPLE_AI_SAMPLE_RATE 8000
#define COUPLE_AI_RECORD_SECONDS 4
#define COUPLE_AI_TOTAL_PCM \
    (COUPLE_AI_SAMPLE_RATE * 2 * COUPLE_AI_RECORD_SECONDS)

#define COUPLE_AI_PCM_CHUNK 1024

#define AI_REPLY_TEXT_MAX 400
// 松手后等待手机完成 ASR 与大模型请求的上限
#define AI_REPLY_TIMEOUT_MS 30000
#define AI_REPLY_POLL_MS 200

typedef enum {
    AI_STATE_IDLE = 0,     // 待机，等按键
    AI_STATE_LISTENING,    // 录音并通过 BLE 上传中
    AI_STATE_THINKING,     // 已发完录音，等手机回传文本
    AI_STATE_ERROR,        // 出错
} ai_state_t;

typedef enum {
    AI_CMD_START = 0,   // 开始一轮对话
    AI_CMD_STOP,        // 结束录音
} ai_cmd_t;

// 手机回传的一条回复：0xFFEA 写入内容去掉首字节 status 后的文本
typedef struct {
    uint8_t status;
    char text[AI_REPLY_TEXT_MAX];
} ai_reply_msg_t;

// ============================================================================
// 状态
// ============================================================================
static bool s_active;               // 页面是否在前台
static ai_state_t s_state = AI_STATE_IDLE;
static volatile bool s_stop_requested;
static volatile bool s_abort;       // 页面退出：打断录音/等待
static volatile int s_listen_seconds_left;
static char s_error_text[48];
static char s_reply_text[AI_REPLY_TEXT_MAX];

static QueueHandle_t s_cmd_queue;
static QueueHandle_t s_reply_queue;
static TaskHandle_t s_worker;

static lv_obj_t *s_scr;
static lv_obj_t *s_state_label;
static lv_obj_t *s_reply_label;
static lv_obj_t *s_hint_label;
static lv_timer_t *s_timer;
static int s_dot_phase;

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
        lv_label_set_text(s_hint_label, "按住OK说话\n单击OK返回");
        break;
    case AI_STATE_LISTENING: {
        char buf[32];
        static const char *dots[] = { "", ".", "..", "..." };
        snprintf(buf, sizeof(buf), "聆听中%s %d", dots[s_dot_phase & 3],
                 s_listen_seconds_left > 0 ? s_listen_seconds_left : 0);
        lv_label_set_text(s_state_label, buf);
        lv_obj_set_style_text_color(s_state_label, lv_color_hex(0xff6b9d), 0);
        lv_label_set_text(s_hint_label, "松手发送");
        break;
    }
    case AI_STATE_THINKING:
        lv_label_set_text(s_state_label, "思考中...");
        lv_obj_set_style_text_color(s_state_label, lv_color_hex(0x6bb5ff), 0);
        lv_label_set_text(s_hint_label, "长按OK返回");
        break;
    case AI_STATE_ERROR:
        lv_label_set_text(s_state_label, s_error_text[0] ? s_error_text : "连接失败");
        lv_obj_set_style_text_color(s_state_label, lv_color_hex(0xff6b6b), 0);
        lv_label_set_text(s_hint_label, "按住OK重试\n单击OK返回");
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
    if (!text) text = "连接失败";
    size_t n = strlen(text);
    if (n > sizeof(s_error_text) - 1) n = sizeof(s_error_text) - 1;
    memcpy(s_error_text, text, n);
    s_error_text[n] = '\0';
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

// 边录边发：不缓存整段音频（本板无 PSRAM，64KB 缓冲会吃掉全部空闲堆）。
// END 包的 offset 就是实际发送的字节数，用户提前松手时不做静音补齐。
static esp_err_t ai_do_record_and_send(void)
{
    if (bsp_audio_set_format(COUPLE_AI_SAMPLE_RATE, 16, 1) != ESP_OK) {
        ESP_LOGE(TAG, "麦克风格式设置失败");
        return ESP_FAIL;
    }

    esp_err_t err = couple_voice_stream_begin(COUPLE_AI_TOTAL_PCM);
    if (err != ESP_OK) return err;

    ai_set_state(AI_STATE_LISTENING);
    s_listen_seconds_left = COUPLE_AI_RECORD_SECONDS;

    static uint8_t buf[COUPLE_AI_PCM_CHUNK];
    uint32_t sent = 0;
    while (sent < COUPLE_AI_TOTAL_PCM) {
        if (s_abort) {
            couple_voice_stream_cancel();
            return ESP_ERR_INVALID_STATE;
        }
        if (s_stop_requested) break;

        uint32_t want = COUPLE_AI_TOTAL_PCM - sent;
        if (want > sizeof(buf)) want = sizeof(buf);
        if (bsp_audio_read(buf, want) != ESP_OK) {
            ESP_LOGW(TAG, "读取麦克风失败，提前结束录音");
            break;
        }
        err = couple_voice_stream_send(buf, want);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "语音上传失败: %s", esp_err_to_name(err));
            return err;
        }
        sent += want;
        s_listen_seconds_left =
            (int)((COUPLE_AI_TOTAL_PCM - sent) / (COUPLE_AI_SAMPLE_RATE * 2));
    }

    if (sent == 0) {
        // 松手太快、没采到有效音频：让手机直接丢弃，省一次 ASR 请求
        couple_voice_stream_cancel();
        return ESP_ERR_INVALID_STATE;
    }
    return couple_voice_stream_end();
}

static bool ai_wait_reply(uint32_t timeout_ms)
{
    ai_reply_msg_t msg;
    uint32_t waited = 0;

    while (waited < timeout_ms) {
        if (s_abort) return false;
        if (xQueueReceive(s_reply_queue, &msg,
                          pdMS_TO_TICKS(AI_REPLY_POLL_MS)) == pdTRUE) {
            if (msg.status != COUPLE_VOICE_REPLY_OK) {
                ai_set_error(msg.text[0] ? msg.text : "手机处理失败");
                return false;
            }
            if (msg.text[0] == '\0') {
                ai_set_error("没听清，请再说一次");
                return false;
            }
            snprintf(s_reply_text, sizeof(s_reply_text), "%s", msg.text);
            ai_set_state(AI_STATE_IDLE);
            ai_publish();
            return true;
        }
        waited += AI_REPLY_POLL_MS;
    }

    ai_set_error("手机无响应");
    return false;
}

static void ai_run_turn(void)
{
    s_reply_text[0] = '\0';
    s_error_text[0] = '\0';
    s_abort = false;
    // s_stop_requested 不在这里清除：松手可能发生在 START 入队后、
    // 录音开始前的窗口，在按键入口（长按投递 START 时）清除更安全。
    ai_publish();

    if (couple_gatt_voice_mtu() == 0) {
        ai_set_error("请用App连接");
        return;
    }

    // 丢弃上一轮可能残留的回复，避免把旧文本当成这一轮的结果
    ai_reply_msg_t stale;
    while (xQueueReceive(s_reply_queue, &stale, 0) == pdTRUE) {
    }

    esp_err_t err = ai_do_record_and_send();
    if (s_abort) return;
    if (err != ESP_OK) {
        if (couple_gatt_voice_mtu() == 0) {
            ai_set_error("请用App连接");
        } else if (err == ESP_ERR_INVALID_STATE) {
            ai_set_error("没听清，请再说一次");
        } else {
            ai_set_error("连接失败");
        }
        return;
    }

    ai_set_state(AI_STATE_THINKING);
    ai_publish();
    ai_wait_reply(AI_REPLY_TIMEOUT_MS);
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
// App 回传的回复（由 GATT 写回调调用，运行在 NimBLE host 任务）
//
// 这里只做拷贝入队，绝不能解析或阻塞：host 任务栈小，且会拖慢整个 BLE 协议栈。
// ============================================================================
void couple_ai_on_remote_reply(uint8_t status, const char *text, size_t len)
{
    if (!s_reply_queue) return;

    ai_reply_msg_t msg;
    msg.status = status;

    size_t n = len;
    if (n > sizeof(msg.text) - 1) n = sizeof(msg.text) - 1;
    if (text && n > 0) {
        memcpy(msg.text, text, n);
    } else {
        n = 0;
    }
    msg.text[n] = '\0';

    // 一轮对话只等一条回复；队列满说明是重复/过期回复，直接丢弃
    (void)xQueueSend(s_reply_queue, &msg, 0);
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
    s_abort = false;
    s_error_text[0] = '\0';
    s_dot_phase = 0;
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

    s_reply_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_reply_label, font_small, 0);
    lv_obj_set_style_text_color(s_reply_label, lv_color_hex(0x9ad8ff), 0);
    lv_obj_set_style_text_align(s_reply_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_reply_label, 200);
    lv_label_set_long_mode(s_reply_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_reply_label, LV_ALIGN_TOP_MID, 0, 96);

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
    // 让进行中的录音/等待尽快收尾；工作任务的检查点负责发 CANCEL
    s_abort = true;
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
    if (btn != BSP_BTN_OK) return;

    switch (ev) {
    case BSP_BTN_LONG:
        // 按住超过阈值（长按）：待机/出错时开始录音；思考中长按返回
        switch (s_state) {
        case AI_STATE_IDLE:
        case AI_STATE_ERROR:
            // 清掉上一轮残留的停止请求，保证新录音不会刚开始就被结束
            s_stop_requested = false;
            (void)ai_post(AI_CMD_START);
            break;
        case AI_STATE_THINKING:
            // 长按返回工牌页；模式切换会调用 couple_ai_exit 释放本页
            couple_mode_switch(COUPLE_MODE_BADGE);
            break;
        case AI_STATE_LISTENING:
            // 录音中忽略长按：必须松手才发送
            break;
        }
        break;

    case BSP_BTN_RELEASE:
        // 松手发送：立即置位，让录音循环在下一个检查点收尾；
        // 即使录音尚未开始（按键入口已清位），进入录音循环也会立刻结束。
        // 同时投递 STOP 兜底（worker 空闲时同样会置位该标志）。
        s_stop_requested = true;
        (void)ai_post(AI_CMD_STOP);
        break;

    case BSP_BTN_CLICK:
        // 短按（未达长按阈值）：待机/出错时返回工牌页
        if (s_state == AI_STATE_IDLE || s_state == AI_STATE_ERROR) {
            couple_mode_switch(COUPLE_MODE_BADGE);
        }
        break;

    default:
        // BSP_BTN_PRESS 等事件不处理：按下不开始录音，等长按判定
        break;
    }
}

// ============================================================================
// 初始化
// ============================================================================
esp_err_t couple_ai_init(void)
{
    if (s_cmd_queue && s_reply_queue) return ESP_OK;

    s_cmd_queue = xQueueCreate(4, sizeof(ai_cmd_t));
    s_reply_queue = xQueueCreate(2, sizeof(ai_reply_msg_t));
    if (!s_cmd_queue || !s_reply_queue) {
        if (s_cmd_queue) vQueueDelete(s_cmd_queue);
        if (s_reply_queue) vQueueDelete(s_reply_queue);
        s_cmd_queue = NULL;
        s_reply_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    // 录音 + BLE 分包发送，无 TLS/JSON，4KB 足够
    if (xTaskCreate(ai_worker, "couple_ai", 4096, NULL, 4, &s_worker) != pdPASS) {
        vQueueDelete(s_cmd_queue);
        vQueueDelete(s_reply_queue);
        s_cmd_queue = NULL;
        s_reply_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
