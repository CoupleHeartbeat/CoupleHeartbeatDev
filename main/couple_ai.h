// main/couple_ai.h —— AI 语音对话页（长按 OK 进入）
//
// 链路：长按 OK 录音 → 边录边经 BLE 0xFFE9 上传 → 手机做 ASR 与大模型请求
// → 手机经 0xFFEA 回传文本 → 本页展示。录音与等待都在独立工作任务里串行执行，
// 按键回调只投递命令，不阻塞 LVGL。
#pragma once

#include "bsp_button.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 页面生命周期（由 couple_mode_switch 调用，调用方须持有 LVGL 锁）
void couple_ai_enter(void);
void couple_ai_exit(void);
void couple_ai_key(bsp_btn_t btn, bsp_btn_ev_t ev);

// 页面是否在前台：靠近自动切换据此让路，避免打断对话
bool couple_ai_is_active(void);

// 创建 AI 工作任务（开机调用一次）
esp_err_t couple_ai_init(void);

// ============================================================================
// App 回传的回复（由 GATT 0xFFEA 写回调调用，运行在 NimBLE host 任务）
//
// 只做拷贝入队，不阻塞、不解析；一轮对话只取第一条。
// status: 0 = 成功，1 = 失败（此时 text 为提示信息，可为空）。
// ============================================================================
void couple_ai_on_remote_reply(uint8_t status, const char *text, size_t len);
