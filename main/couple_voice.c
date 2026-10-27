// main/couple_voice.c —— BLE 语音上行分包实现
#include "couple_voice.h"

#include "couple_gatt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "couple_voice";

// 本机 MTU 上限（与 CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU 一致）。
// notify 的单包上限是 MTU-3（ATT 头 3 字节），再减去本协议 6 字节头。
#define VOICE_MTU_MAX      512
#define VOICE_PKT_MAX      VOICE_MTU_MAX
#define VOICE_PAYLOAD_MAX  (VOICE_MTU_MAX - 3 - COUPLE_VOICE_HDR_LEN)

// 只有 AI 任务会调用本模块，单包缓冲静态复用即可。
static uint8_t s_pkt[VOICE_PKT_MAX];
static uint32_t s_offset;
static uint32_t s_total;

// NimBLE 的 mbuf 池很小（移除 Wi-Fi 后会恢复默认），瞬时发送过快会返回
// ENOMEM。这里短暂重试，而不是直接丢包——丢一个包会让手机端的 PCM 出现空洞。
static esp_err_t notify_retry(size_t len)
{
    for (int i = 0; i < 20; i++) {
        esp_err_t err = couple_gatt_notify_voice(s_pkt, len);
        if (err != ESP_ERR_NO_MEM) return err;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_ERR_NO_MEM;
}

static esp_err_t send_packet(uint8_t type, uint32_t offset,
                             const void *data, size_t len)
{
    s_pkt[0] = type;
    s_pkt[1] = 0;
    s_pkt[2] = (uint8_t)(offset & 0xff);
    s_pkt[3] = (uint8_t)((offset >> 8) & 0xff);
    s_pkt[4] = (uint8_t)(s_total & 0xff);
    s_pkt[5] = (uint8_t)((s_total >> 8) & 0xff);
    if (len > 0) {
        memcpy(s_pkt + COUPLE_VOICE_HDR_LEN, data, len);
    }
    return notify_retry(len + COUPLE_VOICE_HDR_LEN);
}

esp_err_t couple_voice_stream_begin(uint32_t total_bytes)
{
    if (couple_gatt_voice_mtu() < 23) {
        ESP_LOGW(TAG, "未连接手机，无法开始语音流");
        return ESP_ERR_INVALID_STATE;
    }
    s_offset = 0;
    s_total = total_bytes;
    return send_packet(COUPLE_VOICE_TYPE_BEGIN, 0, NULL, 0);
}

esp_err_t couple_voice_stream_send(const void *pcm, size_t len)
{
    const uint8_t *p = (const uint8_t *)pcm;

    uint16_t mtu = couple_gatt_voice_mtu();
    if (mtu < 23) return ESP_ERR_INVALID_STATE;

    size_t payload_max = (size_t)mtu - 3 - COUPLE_VOICE_HDR_LEN;
    if (payload_max > VOICE_PAYLOAD_MAX) payload_max = VOICE_PAYLOAD_MAX;
    if (payload_max == 0) return ESP_ERR_INVALID_SIZE;

    while (len > 0) {
        size_t n = len < payload_max ? len : payload_max;
        esp_err_t err = send_packet(COUPLE_VOICE_TYPE_DATA, s_offset, p, n);
        if (err != ESP_OK) return err;
        s_offset += (uint32_t)n;
        p += n;
        len -= n;
    }
    return ESP_OK;
}

esp_err_t couple_voice_stream_end(void)
{
    return send_packet(COUPLE_VOICE_TYPE_END, s_offset, NULL, 0);
}

void couple_voice_stream_cancel(void)
{
    if (couple_gatt_voice_mtu() < 23) return;
    (void)send_packet(COUPLE_VOICE_TYPE_CANCEL, s_offset, NULL, 0);
}
