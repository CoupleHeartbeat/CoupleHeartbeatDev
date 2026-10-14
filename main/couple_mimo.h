// main/couple_mimo.h —— 小米 MiMo 开放平台的语音对话客户端
//
// 三个能力都是 OpenAI 兼容协议（https://api.xiaomimimo.com/v1/chat/completions），
// 认证头为 `api-key`：语音识别 mimo-v2.5-asr、文本对话 mimo-v2.6-flash、
// 语音合成 mimo-v2.5-tts。
//
// 内存约束（本板无 PSRAM）：录音与合成结果都以流式方式收发，
// 不把整段音频放进内存 —— 详见 couple_mimo.c 顶部说明。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 语音识别：三段式流式上传。
// 需在 begin 时给出本次录音的总 PCM 字节数：整段音频（WAV 头 + PCM）
// 以 base64 直接嵌进请求体，长度可精确计算，因此不用 chunked、
// 也不需要占位 WAV 头（占位头有被服务端拒收的风险）。
esp_err_t couple_mimo_asr_begin(const char *api_key, uint32_t sample_rate,
                                uint8_t channels, uint32_t total_pcm_bytes);
esp_err_t couple_mimo_asr_feed(const uint8_t *pcm, size_t bytes);
// 结束上传并取回识别文本；失败返回错误码，out_text 置空串。
esp_err_t couple_mimo_asr_end(char *out_text, size_t out_size);

// 文本对话：out_reply 收到回复正文。
esp_err_t couple_mimo_chat(const char *api_key, const char *user_text,
                           char *out_reply, size_t out_size);

// 语音合成：把 text 合成为 WAV 并流式解码，PCM 通过 cb 逐段交给播放。
// WAV 头里的采样率/位宽会被解析并随每段回调回传，调用方据此设置 I2S 格式。
typedef esp_err_t (*couple_mimo_pcm_cb_t)(const uint8_t *pcm, size_t bytes,
                                          uint32_t sample_rate, uint8_t channels,
                                          uint8_t bits, void *ctx);
esp_err_t couple_mimo_tts_stream(const char *api_key, const char *text,
                                 couple_mimo_pcm_cb_t cb, void *ctx);

// 取消当前进行中的请求（由按键触发，让长请求尽快返回）
void couple_mimo_cancel(void);
void couple_mimo_clear_cancel(void);
bool couple_mimo_is_cancelled(void);
