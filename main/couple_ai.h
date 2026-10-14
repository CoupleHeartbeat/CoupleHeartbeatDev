// main/couple_ai.h —— AI 语音对话页（长按 OK 进入）
//
// 链路：短按 OK 开始录音 → 松手/录满后上传识别 → 文本对话 → 语音合成播放。
// 语音请求全部在独立工作任务里串行执行，按键回调只投递命令，不阻塞 LVGL。
#pragma once

#include "bsp_button.h"
#include "esp_err.h"
#include <stdbool.h>

// 页面生命周期（由 couple_mode_switch 调用，调用方须持有 LVGL 锁）
void couple_ai_enter(void);
void couple_ai_exit(void);
void couple_ai_key(bsp_btn_t btn, bsp_btn_ev_t ev);

// 页面是否在前台：靠近自动切换据此让路，避免打断对话
bool couple_ai_is_active(void);

// 创建 AI 工作任务（开机调用一次）
esp_err_t couple_ai_init(void);

// ============================================================================
// API Key 存储（仅存设备本地 NVS；开源仓库与日志中都不得出现明文）
// ============================================================================
esp_err_t couple_ai_set_api_key(const char *key);
esp_err_t couple_ai_clear_api_key(void);
bool couple_ai_has_api_key(void);
