// main/couple_audio.c —— 情侣应用心跳音效：软件生成"咚-哒"双音拍 + FreeRTOS 播放任务
#include "couple_audio.h"
#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

static const char *TAG = "couple_audio";

// ============================================================================
// 音频参数
// ============================================================================
#define SAMPLE_RATE         16000   // 采样率 Hz
#define SAMPLE_BITS         16      // 位深
#define CHANNELS            1       // 声道数

// 心跳音音色参数
#define LUB_FREQ            60.0f   // "咚" 基频 Hz（第一心音 S1）
#define DUB_FREQ            85.0f   // "哒" 基频 Hz（第二心音 S2）
#define HARMONIC_2_GAIN     0.4f    // 2 次谐波幅度比例
#define HARMONIC_3_GAIN     0.2f    // 3 次谐波幅度比例

// 心跳音时长（毫秒）
#define LUB_DURATION_MS     80      // "咚" 持续时间
#define DUB_DURATION_MS     60      // "哒" 持续时间
#define LUB_ATTACK_RATIO    0.1f    // "咚" 起音占比（快速起音）
#define DUB_ATTACK_RATIO    0.15f   // "哒" 起音占比

// 各距离等级的 BPM 与音量系数（相对主音量的百分比）
#define LEVEL1_BPM          75
#define LEVEL2_BPM          100
#define LEVEL3_BPM          150

#define LEVEL1_VOL_RATIO    30      // level 1 音量为 主音量的 30%
#define LEVEL2_VOL_RATIO    60      // level 2 音量为 主音量的 60%
#define LEVEL3_VOL_RATIO    90      // level 3 音量为 主音量的 90%

// 播放缓冲：最长单音采样数（咚 80ms @ 16k = 1280），留余量
#define MAX_TONE_SAMPLES    ((SAMPLE_RATE * LUB_DURATION_MS) / 1000 + 64)

// 任务栈大小
#define AUDIO_TASK_STACK    4096

// ============================================================================
// 运行时状态
// ============================================================================
static TaskHandle_t s_task = NULL;
static volatile bool s_running = false;
static volatile uint8_t s_volume = 50;      // 主音量 0-100
static volatile uint8_t s_level = 0;        // 当前距离等级 0-3

static int16_t s_tone_buf[MAX_TONE_SAMPLES]; // 单音 PCM 生成缓冲

// ============================================================================
// 心跳音生成
// ============================================================================

// 生成单个心跳音（带 ADSR 包络，这里简化为 attack + decay 两段）
//   buf:        输出缓冲区
//   freq:       基频 Hz
//   peak_amp:   峰值振幅归一化系数 (0~1)
//   duration_ms:持续时间 ms
//   attack_ratio:起音占比 (0~1)
static void generate_tone(int16_t *buf, int *out_samples,
                          float freq, float peak_amp,
                          int duration_ms, float attack_ratio)
{
    int samples = (SAMPLE_RATE * duration_ms) / 1000;
    if (samples > MAX_TONE_SAMPLES) samples = MAX_TONE_SAMPLES;

    int attack_samples = (int)(samples * attack_ratio);
    if (attack_samples < 1) attack_samples = 1;
    int decay_samples = samples - attack_samples;

    float phase = 0.0f;
    float phase_step = 2.0f * (float)M_PI * freq / SAMPLE_RATE;
    float norm = 1.0f + HARMONIC_2_GAIN + HARMONIC_3_GAIN;

    // 峰值振幅（16bit 有符号，留约 1/4 余量避免削波）
    float peak = peak_amp * 24000.0f / norm;

    for (int i = 0; i < samples; i++) {
        // 包络：attack 线性上升 + decay 指数衰减
        float env;
        if (i < attack_samples) {
            env = (float)i / (float)attack_samples;
        } else {
            // 指数衰减：从 1.0 衰减到 ~0.02
            float t = (float)(i - attack_samples) / (float)decay_samples;
            env = expf(-4.0f * t);
        }

        // 基频 + 2 次 + 3 次谐波，模拟心音的"噗"质感
        float s = sinf(phase)
                + HARMONIC_2_GAIN * sinf(2.0f * phase)
                + HARMONIC_3_GAIN * sinf(3.0f * phase);

        buf[i] = (int16_t)(s * peak * env);

        phase += phase_step;
        if (phase >= 2.0f * (float)M_PI) {
            phase -= 2.0f * (float)M_PI;
        }
    }

    *out_samples = samples;
}

// ============================================================================
// 播放控制
// ============================================================================

// 计算当前 level 下的一个完整心跳周期（毫秒）
static int get_cycle_ms(uint8_t level)
{
    switch (level) {
        case 1: return 60000 / LEVEL1_BPM;
        case 2: return 60000 / LEVEL2_BPM;
        case 3: return 60000 / LEVEL3_BPM;
        default: return 0;
    }
}

// 计算当前 level 下的有效音量（结合主音量与 level 系数）
static uint8_t get_effective_volume(uint8_t level)
{
    uint8_t ratio;
    switch (level) {
        case 1: ratio = LEVEL1_VOL_RATIO; break;
        case 2: ratio = LEVEL2_VOL_RATIO; break;
        case 3: ratio = LEVEL3_VOL_RATIO; break;
        default: return 0;
    }
    return (uint8_t)((uint16_t)s_volume * ratio / 100);
}

// 播放一个完整的心跳周期（咚 + 间隔 + 哒 + 舒张期静默）
// 返回是否成功播放（false 表示中途被停止/静音）
static bool play_one_cycle(uint8_t level)
{
    int cycle_ms = get_cycle_ms(level);
    if (cycle_ms <= 0) return false;

    uint8_t eff_vol = get_effective_volume(level);
    if (eff_vol == 0 || s_volume == 0) return false;

    bsp_audio_set_volume(eff_vol);

    // 心跳相位划分：
    //   lub (咚) → lub-dub gap → dub (哒) → diastolic silence (舒张期)
    int lub_ms = LUB_DURATION_MS;
    int dub_ms = DUB_DURATION_MS;

    // 咚-哒间隔约为周期的 22%，模拟真实心跳 S1-S2 间距
    int lub_dub_gap_ms = cycle_ms * 22 / 100;
    if (lub_dub_gap_ms < 40) lub_dub_gap_ms = 40;

    // 舒张期 = 总周期 - 咚 - 间隔 - 哒
    int diastolic_ms = cycle_ms - lub_ms - lub_dub_gap_ms - dub_ms;
    if (diastolic_ms < 30) diastolic_ms = 30;

    // ---- 播放"咚" ----
    int lub_samples;
    generate_tone(s_tone_buf, &lub_samples, LUB_FREQ, 1.0f,
                  lub_ms, LUB_ATTACK_RATIO);
    if (bsp_audio_write(s_tone_buf, (size_t)lub_samples * sizeof(int16_t)) != ESP_OK) {
        ESP_LOGW(TAG, "lub write failed");
        return false;
    }

    // 检查是否需要停止
    if (!s_running || s_level == 0 || s_volume == 0) return false;

    // ---- 咚-哒间隔（静音）----
    vTaskDelay(pdMS_TO_TICKS(lub_dub_gap_ms));
    if (!s_running || s_level == 0 || s_volume == 0) return false;

    // ---- 播放"哒" ----
    int dub_samples;
    generate_tone(s_tone_buf, &dub_samples, DUB_FREQ, 0.72f,
                  dub_ms, DUB_ATTACK_RATIO);
    if (bsp_audio_write(s_tone_buf, (size_t)dub_samples * sizeof(int16_t)) != ESP_OK) {
        ESP_LOGW(TAG, "dub write failed");
        return false;
    }

    if (!s_running || s_level == 0 || s_volume == 0) return false;

    // ---- 舒张期静默 ----
    vTaskDelay(pdMS_TO_TICKS(diastolic_ms));

    return true;
}

// ============================================================================
// 播放任务
// ============================================================================

static void audio_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "audio task started");

    // 确保采样格式已设置
    if (bsp_audio_set_format(SAMPLE_RATE, SAMPLE_BITS, CHANNELS) != ESP_OK) {
        ESP_LOGE(TAG, "set_format failed, task exit");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (s_running) {
        uint8_t level = s_level;

        // 未到播放条件时短暂等待
        if (level == 0 || s_volume == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // 播放一个心跳周期；失败或被打断则重新评估状态
        (void)play_one_cycle(level);
    }

    ESP_LOGI(TAG, "audio task stopping");
    s_task = NULL;
    vTaskDelete(NULL);
}

// ============================================================================
// 公开 API
// ============================================================================

void couple_audio_init(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "already initialized");
        return;
    }

    esp_err_t ret = bsp_audio_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init failed: %d", ret);
        return;
    }

    ret = bsp_audio_set_format(SAMPLE_RATE, SAMPLE_BITS, CHANNELS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_set_format failed: %d", ret);
        return;
    }

    s_running = true;
    s_level = 0;

    BaseType_t xret = xTaskCreate(audio_task, "couple_audio",
                                  AUDIO_TASK_STACK, NULL, 5, &s_task);
    if (xret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        s_running = false;
        s_task = NULL;
    }
}

void couple_audio_set_volume(uint8_t vol)
{
    if (vol > 100) vol = 100;
    s_volume = vol;
}

uint8_t couple_audio_get_volume(void)
{
    return s_volume;
}

void couple_audio_play_heartbeat(uint8_t distance_level)
{
    if (distance_level > 3) distance_level = 3;
    s_level = distance_level;
}

void couple_audio_stop(void)
{
    s_level = 0;
    s_running = false;

    // 等待任务自行退出（最多等一个心跳周期 + 余量）
    int wait_ms = 60000 / LEVEL1_BPM + 200;
    int elapsed = 0;
    while (s_task != NULL && elapsed < wait_ms) {
        vTaskDelay(pdMS_TO_TICKS(20));
        elapsed += 20;
    }

    if (s_task != NULL) {
        ESP_LOGW(TAG, "task did not exit in time, forcing delete");
        vTaskDelete(s_task);
        s_task = NULL;
    }

    // 让 codec 进入低功耗
    bsp_audio_sleep();
}
