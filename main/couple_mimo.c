// main/couple_mimo.c —— 小米 MiMo 语音对话客户端实现
//
// 内存策略（这块板子没有 PSRAM，Wi-Fi + BLE + LVGL 之后堆已经不宽裕）：
//   * 识别上传：录音总长在开始时确定，整段 WAV（头 + PCM）以 base64 直接
//     嵌进请求体，Content-Length 可精确计算，因此按 3 字节一组边编码边写，
//     只占几 KB 的栈缓冲，不缓存整段音频。
//   * 合成下载：请求 format=wav，响应里 `"data":"<base64>"` 按字节流扫描，
//     base64 边解码边解析 WAV 头，解出的 PCM 立刻回调播放。
// 两个方向都不需要“先收完再处理”，因此 5~10 秒的语音也只需常量级内存。
//
// 注意：请求体的提示词保持英文，避免把非 UI 中文混进 main/*.c —— 字体门禁
// 会把 main/*.c 里所有字符串字面量的中文都当成待上屏文案来校验。
#include "couple_mimo.h"

#include "couple_ai_json.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "couple_mimo";

#define MIMO_URL        "https://api.xiaomimimo.com/v1/chat/completions"
#define MIMO_ASR_MODEL  "mimo-v2.5-asr"
#define MIMO_CHAT_MODEL "mimo-v2.6-flash"
#define MIMO_TTS_MODEL  "mimo-v2.5-tts"
#define MIMO_TTS_VOICE  "mimo_default"

// 回复要短：语音播报越长越占用等待时间，也越费流量
#define MIMO_CHAT_SYSTEM \
    "You are a warm, playful voice companion inside a couple's matching gadget. " \
    "Reply in the same language the user used, in at most 40 characters, " \
    "spoken style, no emoji, no markdown."
#define MIMO_TTS_STYLE \
    "Speak in a warm, gentle and slightly playful tone, at a natural slow pace."

#define MIMO_HTTP_TIMEOUT_MS 30000
#define HTTP_RX_BUF 1024
#define HTTP_TX_BUF 1024

#define ASR_TEXT_MAX   1536
#define CHAT_REPLY_MAX 3072

static volatile bool s_cancel;

void couple_mimo_cancel(void) { s_cancel = true; }
void couple_mimo_clear_cancel(void) { s_cancel = false; }
bool couple_mimo_is_cancelled(void) { return s_cancel; }

// ============================================================================
// 小工具
// ============================================================================
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

// JSON 字符串转义：转义 " \ 与控制字符，UTF-8 多字节原样透传。
// 返回写入 out 的长度（不含 '\0'），缓冲不足返回 -1。
static int json_escape(const char *in, char *out, size_t out_size)
{
    size_t w = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        unsigned char c = *p;
        const char *esc = NULL;
        char tmp[8];
        if (c == '"') esc = "\\\"";
        else if (c == '\\') esc = "\\\\";
        else if (c == '\n') esc = "\\n";
        else if (c == '\r') esc = "\\r";
        else if (c == '\t') esc = "\\t";
        else if (c < 0x20) {
            snprintf(tmp, sizeof(tmp), "\\u%04x", c);
            esc = tmp;
        }
        if (esc) {
            size_t n = strlen(esc);
            if (w + n + 1 > out_size) return -1;
            memcpy(out + w, esc, n);
            w += n;
        } else {
            if (w + 2 > out_size) return -1;
            out[w++] = (char)c;
        }
    }
    out[w] = '\0';
    return (int)w;
}

// ============================================================================
// 流式 base64（编码用于上传，解码用于合成结果）
// ============================================================================
typedef struct {
    uint8_t carry[2];
    int carry_len;
} b64_enc_t;

static const char B64_ALPHA[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// 把 3 字节组编码为 4 字符
static void b64_triple(const uint8_t *s, char *o)
{
    uint32_t v = ((uint32_t)s[0] << 16) | ((uint32_t)s[1] << 8) | s[2];
    o[0] = B64_ALPHA[(v >> 18) & 0x3F];
    o[1] = B64_ALPHA[(v >> 12) & 0x3F];
    o[2] = B64_ALPHA[(v >> 6) & 0x3F];
    o[3] = B64_ALPHA[v & 0x3F];
}

// 编码 src（可累加调用），写出完整三字节组的 base64；余数留在 carry。
// out 需至少 4 * ((2 + len) / 3) 字节。
static size_t b64_enc(b64_enc_t *st, const uint8_t *src, size_t len, char *out)
{
    size_t w = 0;
    uint8_t buf[3];
    int n = st->carry_len;
    memcpy(buf, st->carry, (size_t)n);

    for (size_t i = 0; i < len; i++) {
        buf[n++] = src[i];
        if (n == 3) {
            b64_triple(buf, out + w);
            w += 4;
            n = 0;
        }
    }
    memcpy(st->carry, buf, (size_t)n);
    st->carry_len = n;
    return w;
}

// 收尾：把不足 3 字节的余数补 '=' 输出
static size_t b64_finish(b64_enc_t *st, char *out)
{
    int n = st->carry_len;
    if (n == 0) return 0;
    uint8_t buf[3] = { st->carry[0], n > 1 ? st->carry[1] : 0, 0 };
    char quad[4];
    b64_triple(buf, quad);
    out[0] = quad[0];
    out[1] = quad[1];
    out[2] = (n >= 2) ? quad[2] : '=';
    out[3] = '=';
    st->carry_len = 0;
    return 4;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

typedef struct {
    int quad[4];
    int n;
} b64_dec_t;

// 解码 base64 片段（可累加调用）。返回写入 out 的字节数。
static size_t b64_dec(b64_dec_t *st, const char *src, size_t len,
                      uint8_t *out, size_t out_cap)
{
    size_t w = 0;
    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        if (c == '=') continue;          // 填充，直接跳过
        int v = b64_val(c);
        if (v < 0) continue;             // 忽略换行等无关字符
        st->quad[st->n++] = v;
        if (st->n == 4) {
            uint32_t x = ((uint32_t)st->quad[0] << 18) |
                         ((uint32_t)st->quad[1] << 12) |
                         ((uint32_t)st->quad[2] << 6) |
                         (uint32_t)st->quad[3];
            if (w + 3 > out_cap) return w;   // 缓冲保守，直接停下
            out[w++] = (uint8_t)((x >> 16) & 0xFF);
            out[w++] = (uint8_t)((x >> 8) & 0xFF);
            out[w++] = (uint8_t)(x & 0xFF);
            st->n = 0;
        }
    }
    return w;
}

// ============================================================================
// WAV 头解析（流式）
// ============================================================================
typedef struct {
    bool parsed;
    size_t need;
    uint8_t buf[512];
    size_t pcm_off;
    uint32_t rate;
    uint8_t channels;
    uint8_t bits;
} wav_ctx_t;

// 返回 1 已解析、0 需要更多字节、-1 头部非法
static int wav_try_parse(wav_ctx_t *w)
{
    if (w->need < 12) return 0;
    if (memcmp(w->buf, "RIFF", 4) != 0 || memcmp(w->buf + 8, "WAVE", 4) != 0) {
        return -1;
    }

    size_t pos = 12;
    bool have_fmt = false;
    while (pos + 8 <= w->need) {
        const uint8_t *id = w->buf + pos;
        uint32_t sz = rd32(w->buf + pos + 4);
        if (memcmp(id, "fmt ", 4) == 0) {
            if (pos + 8 + 16 > w->need) return 0;       // fmt 头还没收全
            w->channels = (uint8_t)rd16(w->buf + pos + 10);
            w->rate = rd32(w->buf + pos + 12);
            w->bits = (uint8_t)rd16(w->buf + pos + 22);
            have_fmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            if (!have_fmt) return -1;
            w->pcm_off = pos + 8;
            return 1;
        }
        size_t next = pos + 8 + sz + (sz & 1);
        if (next <= pos || next > sizeof(w->buf)) return -1;
        pos = next;
    }
    return 0;
}

// ============================================================================
// HTTP 基础
// ============================================================================
static esp_http_client_handle_t http_new(const char *api_key)
{
    esp_http_client_config_t cfg = {
        .url = MIMO_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = MIMO_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = HTTP_RX_BUF,
        .buffer_size_tx = HTTP_TX_BUF,
        // keep-alive 关闭：连接池会常驻一整条 TLS 上下文（约 20KB），
        // 本板握手后只剩不到 10KB 堆，池化会让下一次请求直接 NO_MEM。
        .keep_alive_enable = false,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return NULL;

    esp_http_client_set_header(client, "api-key", api_key);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    return client;
}

// 分块写出请求体，遇到取消立即返回。
// 单笔 1024 字节：正好一条 TLS 记录（OUT_CONTENT_LEN=1024），既不会超过
// 一个 TCP 段，也能把 write 调用次数比 512 字节方案减半。
#define MIMO_TX_CHUNK 1024

static esp_err_t http_write_all(esp_http_client_handle_t client,
                                const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        if (s_cancel) return ESP_ERR_INVALID_STATE;
        size_t chunk = len - sent;
        if (chunk > MIMO_TX_CHUNK) chunk = MIMO_TX_CHUNK;
        int n = esp_http_client_write(client, data + sent, (int)chunk);
        if (n <= 0) return ESP_FAIL;
        sent += (size_t)n;
    }
    return ESP_OK;
}

// 读取响应体到 buf（含 '\0'），返回实际字节数；超出缓冲会被截断。
static int http_read_body(esp_http_client_handle_t client, char *buf, size_t cap)
{
    size_t total = 0;
    while (total + 1 < cap) {
        if (s_cancel) break;
        int n = esp_http_client_read(client, buf + total, (int)(cap - 1 - total));
        if (n > 0) {
            total += (size_t)n;
            continue;
        }
        if (n == -ESP_ERR_HTTP_EAGAIN) continue;
        break;
    }
    buf[total] = '\0';
    return (int)total;
}

// 记录失败原因（不打印请求体，避免把 api key 写进日志）
static void log_http_failure(esp_http_client_handle_t client, const char *what)
{
    int status = esp_http_client_get_status_code(client);
    ESP_LOGW(TAG, "%s 失败: HTTP %d", what, status);
}

// ============================================================================
// 文本对话
// ============================================================================
esp_err_t couple_mimo_chat(const char *api_key, const char *user_text,
                           char *out_reply, size_t out_size)
{
    if (!api_key || !api_key[0] || !user_text || !out_reply || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out_reply[0] = '\0';

    char esc_user[1024];
    if (json_escape(user_text, esc_user, sizeof(esc_user)) < 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t need = strlen(esc_user) + 512;
    char *body = malloc(need);
    if (!body) return ESP_ERR_NO_MEM;
    int body_len = snprintf(body, need,
        "{\"model\":\"%s\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"%s\"},"
        "{\"role\":\"user\",\"content\":\"%s\"}],"
        "\"max_completion_tokens\":128,\"stream\":false,"
        "\"thinking\":{\"type\":\"disabled\"}}",
        MIMO_CHAT_MODEL, MIMO_CHAT_SYSTEM, esc_user);
    if (body_len <= 0 || (size_t)body_len >= need) {
        free(body);
        return ESP_ERR_INVALID_SIZE;
    }

    int64_t t0 = esp_timer_get_time();
    esp_http_client_handle_t client = http_new(api_key);
    if (!client) {
        free(body);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, body_len);
    if (err == ESP_OK) err = http_write_all(client, body, (size_t)body_len);
    free(body);

    if (err == ESP_OK) {
        if (esp_http_client_fetch_headers(client) < 0) err = ESP_FAIL;
    }

    char *resp = NULL;
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            log_http_failure(client, "chat");
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            resp = malloc(CHAT_REPLY_MAX + 1);
            if (!resp) {
                err = ESP_ERR_NO_MEM;
            } else {
                int n = http_read_body(client, resp, CHAT_REPLY_MAX + 1);
                if (n <= 0) {
                    err = ESP_FAIL;
                } else {
                    couple_ai_json_result_t jr = couple_ai_json_get_string(
                        resp, (size_t)n, "content", out_reply, out_size);
                    if (jr != COUPLE_AI_JSON_OK) {
                        ESP_LOGW(TAG, "对话响应缺少 content: %d", (int)jr);
                        err = ESP_ERR_INVALID_RESPONSE;
                    }
                }
            }
        }
    }

    free(resp);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "对话回复已取回 用时 %lld ms",
                 (long long)((esp_timer_get_time() - t0) / 1000));
    }
    return err;
}

// ============================================================================
// 语音合成（流式解码播放）
// ============================================================================
esp_err_t couple_mimo_tts_stream(const char *api_key, const char *text,
                                 couple_mimo_pcm_cb_t cb, void *ctx)
{
    if (!api_key || !api_key[0] || !text || !text[0] || !cb) {
        return ESP_ERR_INVALID_ARG;
    }

    // 这些缓冲放在静态区而不是栈上：TTS 与 mbedTLS 握手共用 AI 任务栈，
    // 实测 8KB 栈会在起播阶段溢出（Stack protection fault）。TTS 只由
    // AI 任务串行调用，静态缓冲不会重入。
    static char esc_text[768];
    static char esc_style[256];
    if (json_escape(text, esc_text, sizeof(esc_text)) < 0 ||
        json_escape(MIMO_TTS_STYLE, esc_style, sizeof(esc_style)) < 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t need = strlen(esc_text) + strlen(esc_style) + 512;
    char *body = malloc(need);
    if (!body) return ESP_ERR_NO_MEM;
    int body_len = snprintf(body, need,
        "{\"model\":\"%s\",\"messages\":["
        "{\"role\":\"user\",\"content\":\"%s\"},"
        "{\"role\":\"assistant\",\"content\":\"%s\"}],"
        "\"audio\":{\"format\":\"wav\",\"voice\":\"%s\"},\"stream\":false}",
        MIMO_TTS_MODEL, esc_style, esc_text, MIMO_TTS_VOICE);
    if (body_len <= 0 || (size_t)body_len >= need) {
        free(body);
        return ESP_ERR_INVALID_SIZE;
    }

    int64_t t0 = esp_timer_get_time();
    int64_t t_first = 0;
    ESP_LOGI(TAG, "TTS 开始 空闲堆=%u 最大块=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    esp_http_client_handle_t client = http_new(api_key);
    if (!client) {
        free(body);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, body_len);
    if (err == ESP_OK) err = http_write_all(client, body, (size_t)body_len);
    free(body);

    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) err = ESP_FAIL;
    if (err == ESP_OK && esp_http_client_get_status_code(client) != 200) {
        log_http_failure(client, "tts");
        err = ESP_ERR_INVALID_RESPONSE;
    }

    // 扫描响应字节流：先找到 `"data":"`，再一直解码到下一个引号
    enum { SCAN, DECODE, DONE } state = SCAN;
    char pending[16];
    size_t pending_len = 0;
    static const char MARKER[] = "\"data\":\"";
    const size_t MARKER_LEN = sizeof(MARKER) - 1;

    b64_dec_t dec = {0};
    wav_ctx_t wav = {0};
    static uint8_t pcm[2048];
    static char rx[HTTP_RX_BUF];

// 首次交付 PCM 时记录起播延迟（含 TLS 建连、请求上传与首包等待）
#define TTS_MARK_FIRST()                                                        \
    do {                                                                        \
        if (t_first == 0) {                                                     \
            t_first = esp_timer_get_time();                                     \
            ESP_LOGI(TAG, "TTS 首包延迟 %lld ms",                               \
                     (long long)((t_first - t0) / 1000));                       \
        }                                                                       \
    } while (0)

    while (err == ESP_OK) {
        if (s_cancel) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        int n = esp_http_client_read(client, rx, sizeof(rx));
        if (n == -ESP_ERR_HTTP_EAGAIN) continue;
        if (n <= 0) break;   // 读取结束

        size_t i = 0;
        while (i < (size_t)n && err == ESP_OK) {
            if (state == SCAN) {
                // 逐个字符推进，跨读取边界的标记也能命中
                if (pending_len < sizeof(pending)) {
                    pending[pending_len++] = rx[i++];
                } else {
                    memmove(pending, pending + 1, sizeof(pending) - 1);
                    pending[sizeof(pending) - 1] = rx[i++];
                }
                if (pending_len >= MARKER_LEN &&
                    memcmp(pending + pending_len - MARKER_LEN, MARKER,
                           MARKER_LEN) == 0) {
                    state = DECODE;
                }
                continue;
            }

            if (state == DECODE) {
                if (rx[i] == '"') {
                    state = DONE;
                    i++;
                    continue;
                }
                size_t got = b64_dec(&dec, &rx[i], 1, pcm, sizeof(pcm));
                i++;
                if (got == 0) continue;

                if (!wav.parsed) {
                    size_t k = 0;
                    while (k < got) {
                        if (wav.need >= sizeof(wav.buf)) {
                            ESP_LOGE(TAG, "WAV 头超过缓冲");
                            err = ESP_ERR_INVALID_RESPONSE;
                            break;
                        }
                        wav.buf[wav.need++] = pcm[k++];
                        int pr = wav_try_parse(&wav);
                        if (pr < 0) {
                            ESP_LOGE(TAG, "WAV 头非法");
                            err = ESP_ERR_INVALID_RESPONSE;
                            break;
                        }
                        if (pr == 1) {
                            wav.parsed = true;
                            size_t rest = wav.need - wav.pcm_off;
                            if (rest > 0) {
                                TTS_MARK_FIRST();
                                err = cb(wav.buf + wav.pcm_off, rest, wav.rate,
                                         wav.channels, wav.bits, ctx);
                            }
                            // 头在同一批里解析完成时，本批剩余字节也是 PCM，别丢
                            if (err == ESP_OK && k < got) {
                                err = cb(pcm + k, got - k, wav.rate,
                                         wav.channels, wav.bits, ctx);
                            }
                            break;
                        }
                    }
                    continue;
                }

                TTS_MARK_FIRST();
                err = cb(pcm, got, wav.rate, wav.channels, wav.bits, ctx);
                continue;
            }

            i++;   // DONE：丢弃剩余字节
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && !wav.parsed) {
        ESP_LOGW(TAG, "响应里没有可播放的音频");
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "TTS 结束 用时 %lld ms",
                 (long long)((esp_timer_get_time() - t0) / 1000));
    }
#undef TTS_MARK_FIRST
    return err;
}

// ============================================================================
// 语音识别（流式上传）
// ============================================================================
static esp_http_client_handle_t s_asr_client;
static b64_enc_t s_asr_enc;
static char s_asr_suffix[64];
static size_t s_asr_suffix_len;
static size_t s_asr_expect_pcm;
static size_t s_asr_fed_pcm;
static int64_t s_asr_t0;

esp_err_t couple_mimo_asr_begin(const char *api_key, uint32_t sample_rate,
                                uint8_t channels, uint32_t total_pcm_bytes)
{
    if (!api_key || !api_key[0] || total_pcm_bytes == 0) return ESP_ERR_INVALID_ARG;
    if (channels != 1) return ESP_ERR_NOT_SUPPORTED;   // 本板只做单声道 16bit
    if (s_asr_client) return ESP_ERR_INVALID_STATE;

    // 44 字节标准 16bit PCM WAV 头，长度已知，不需要占位
    uint8_t hdr[44];
    uint32_t data_size = total_pcm_bytes;
    uint32_t riff_size = 36 + data_size;
    memcpy(hdr, "RIFF", 4);
    hdr[4] = (uint8_t)(riff_size);       hdr[5] = (uint8_t)(riff_size >> 8);
    hdr[6] = (uint8_t)(riff_size >> 16); hdr[7] = (uint8_t)(riff_size >> 24);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    hdr[16] = 16; hdr[17] = 0; hdr[18] = 0; hdr[19] = 0;   // fmt 块大小
    hdr[20] = 1;  hdr[21] = 0;                             // PCM
    hdr[22] = channels; hdr[23] = 0;
    uint32_t bps = sample_rate * channels * 2;
    hdr[24] = (uint8_t)(sample_rate);       hdr[25] = (uint8_t)(sample_rate >> 8);
    hdr[26] = (uint8_t)(sample_rate >> 16); hdr[27] = (uint8_t)(sample_rate >> 24);
    hdr[28] = (uint8_t)(bps);        hdr[29] = (uint8_t)(bps >> 8);
    hdr[30] = (uint8_t)(bps >> 16);  hdr[31] = (uint8_t)(bps >> 24);
    hdr[32] = (uint8_t)(channels * 2); hdr[33] = 0;        // block align
    hdr[34] = 16; hdr[35] = 0;                             // 位宽
    memcpy(hdr + 36, "data", 4);
    hdr[40] = (uint8_t)(data_size);       hdr[41] = (uint8_t)(data_size >> 8);
    hdr[42] = (uint8_t)(data_size >> 16); hdr[43] = (uint8_t)(data_size >> 24);

    const char *prefix =
        "{\"model\":\"" MIMO_ASR_MODEL "\",\"messages\":[{\"role\":\"user\","
        "\"content\":[{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"";
    const char *suffix = "\",\"format\":\"wav\"}}]}]}";

    uint32_t wav_total = 44 + total_pcm_bytes;
    size_t b64_len = 4 * ((wav_total + 2) / 3);
    size_t plen = strlen(prefix);
    size_t slen = strlen(suffix);
    size_t total = plen + b64_len + slen;

    snprintf(s_asr_suffix, sizeof(s_asr_suffix), "%s", suffix);
    s_asr_suffix_len = slen;
    s_asr_expect_pcm = total_pcm_bytes;
    s_asr_fed_pcm = 0;

    esp_http_client_handle_t client = http_new(api_key);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, (int)total);
    if (err == ESP_OK) {
        err = http_write_all(client, prefix, plen);
    }
    if (err == ESP_OK) {
        s_asr_enc.carry_len = 0;
        char out[64];
        size_t w = b64_enc(&s_asr_enc, hdr, sizeof(hdr), out);
        err = http_write_all(client, out, w);
    }
    if (err != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    s_asr_client = client;
    s_asr_t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "开始上传语音(%u 字节 PCM)", (unsigned)total_pcm_bytes);
    return ESP_OK;
}

esp_err_t couple_mimo_asr_feed(const uint8_t *pcm, size_t bytes)
{
    if (!s_asr_client) return ESP_ERR_INVALID_STATE;
    if (!pcm || bytes == 0) return ESP_OK;
    if (s_asr_fed_pcm + bytes > s_asr_expect_pcm) return ESP_ERR_INVALID_SIZE;
    if (s_cancel) return ESP_ERR_INVALID_STATE;

    char out[4096];
    size_t w = b64_enc(&s_asr_enc, pcm, bytes, out);
    s_asr_fed_pcm += bytes;
    if (w == 0) return ESP_OK;
    return http_write_all(s_asr_client, out, w);
}

esp_err_t couple_mimo_asr_end(char *out_text, size_t out_size)
{
    if (!s_asr_client || !out_text || out_size == 0) return ESP_ERR_INVALID_ARG;
    out_text[0] = '\0';

    esp_err_t err = ESP_OK;
    if (s_asr_fed_pcm != s_asr_expect_pcm) {
        ESP_LOGW(TAG, "录音长度不足(%u/%u)，仍按实际长度上报可能失败",
                 (unsigned)s_asr_fed_pcm, (unsigned)s_asr_expect_pcm);
    }

    // 补齐 base64 余数，再写 JSON 收尾
    char tail[8];
    size_t tw = b64_finish(&s_asr_enc, tail);
    if (tw) err = http_write_all(s_asr_client, tail, tw);
    if (err == ESP_OK) err = http_write_all(s_asr_client, s_asr_suffix, s_asr_suffix_len);

    if (err == ESP_OK && esp_http_client_fetch_headers(s_asr_client) < 0) {
        err = ESP_FAIL;
    }

    char *resp = NULL;
    if (err == ESP_OK) {
        if (esp_http_client_get_status_code(s_asr_client) != 200) {
            log_http_failure(s_asr_client, "asr");
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            resp = malloc(ASR_TEXT_MAX + 1);
            if (!resp) {
                err = ESP_ERR_NO_MEM;
            } else {
                int n = http_read_body(s_asr_client, resp, ASR_TEXT_MAX + 1);
                if (n <= 0) {
                    err = ESP_FAIL;
                } else {
                    couple_ai_json_result_t jr = couple_ai_json_get_string(
                        resp, (size_t)n, "content", out_text, out_size);
                    if (jr != COUPLE_AI_JSON_OK) {
                        ESP_LOGW(TAG, "识别响应缺少 content: %d", (int)jr);
                        err = ESP_ERR_INVALID_RESPONSE;
                    }
                }
            }
        }
    }

    free(resp);
    esp_http_client_close(s_asr_client);
    esp_http_client_cleanup(s_asr_client);
    s_asr_client = NULL;
    s_asr_fed_pcm = 0;
    s_asr_expect_pcm = 0;

    ESP_LOGI(TAG, "语音识别%s 用时 %lld ms", err == ESP_OK ? "完成" : "失败",
             (long long)((esp_timer_get_time() - s_asr_t0) / 1000));
    return err;
}
