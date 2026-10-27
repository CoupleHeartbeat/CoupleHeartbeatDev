// main/couple_voice.h —— BLE 语音上行协议（设备 -> 手机）
//
// 设备不再自己连 Wi-Fi，录音改由手机 App 接收后做 ASR 与大模型请求，
// 再由 App 通过 0xFFEA 特征把回复文本写回设备。
//
// 上行（0xFFE9，Notify）包格式，固定 6 字节头 + payload：
//   [0]    类型：1=BEGIN 2=DATA 3=END 4=CANCEL
//   [1]    保留（0）
//   [2..3] offset，payload 在整段 PCM 中的字节偏移（小端）
//   [4..5] total，本次录音总字节数（小端，仅 BEGIN 有参考意义）
//   [6..]  PCM 数据（8 kHz / 16 bit / 单声道，小端有符号）
//
// END 包的 offset 就是实际有效长度：用户提前松手时不补静音，
// 由 App 按实际长度构造 WAV 头，避免白白传输几十 KB 的零。
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COUPLE_VOICE_HDR_LEN     6
#define COUPLE_VOICE_TYPE_BEGIN  1
#define COUPLE_VOICE_TYPE_DATA   2
#define COUPLE_VOICE_TYPE_END    3
#define COUPLE_VOICE_TYPE_CANCEL 4

// 下行（0xFFEA，Write）包格式：[0] status（0=成功 1=失败），[1..] UTF-8 文本。
#define COUPLE_VOICE_REPLY_OK    0
#define COUPLE_VOICE_REPLY_ERROR 1

/**
 * @brief 开始一段录音流：发送 BEGIN 包
 *
 * @param total_bytes 本次录音计划采集的总字节数（仅作参考）
 */
esp_err_t couple_voice_stream_begin(uint32_t total_bytes);

/**
 * @brief 发送一块 PCM 数据，按当前 ATT MTU 自动分包
 */
esp_err_t couple_voice_stream_send(const void *pcm, size_t len);

/**
 * @brief 结束录音流：发送 END 包，offset 为实际发送的字节数
 */
esp_err_t couple_voice_stream_end(void);

/**
 * @brief 中止当前录音流（用户退出页面/取消），通知 App 丢弃本次音频
 *
 * 失败时静默忽略：链路已经断开就没有通知对象。
 */
void couple_voice_stream_cancel(void);

#ifdef __cplusplus
}
#endif
