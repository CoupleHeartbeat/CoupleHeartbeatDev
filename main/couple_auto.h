// main/couple_auto.h —— 靠近感知的纯逻辑判定（不依赖 ESP-IDF / LVGL，可主机测试）
//
// 靠近不再切换页面：工牌页常驻，判定结果只决定工牌页四周边缘光效的档位。
// 档位越高，光效越亮、呼吸越快，同时心跳音效越急。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    COUPLE_GLOW_OFF = 0,   // 对方不在（或本机未配对）
    COUPLE_GLOW_NEAR,      // 感受到你
    COUPLE_GLOW_CLOSE,     // 很近了
    COUPLE_GLOW_TOUCH,     // 贴贴：持续极近
} couple_glow_level_t;

typedef struct {
    bool paired;                // 本机已配对
    bool partner_present;       // 仍在收到对方心跳
    int8_t rssi;                // 对方最新 RSSI(dBm)，无心跳时约定为 -100
    uint8_t very_close_count;   // 连续极近的心跳计数
} couple_auto_input_t;

// 连续极近多少拍才算“贴贴”：RSSI 在边界上抖动时不该来回切档。
#define COUPLE_AUTO_TOUCH_THRESHOLD 5

// RSSI 分档阈值（dBm）。数值来自原心动页的分级，保持既有手感不变。
#define COUPLE_AUTO_RSSI_TOUCH (-45)
#define COUPLE_AUTO_RSSI_CLOSE (-58)
#define COUPLE_AUTO_RSSI_NEAR  (-70)

couple_glow_level_t couple_auto_decide(const couple_auto_input_t *in);
