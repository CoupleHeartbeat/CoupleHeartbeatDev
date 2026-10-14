// main/couple_auto.h —— 靠近自动切换模式的纯逻辑判定（不依赖 ESP-IDF / LVGL，可主机测试）
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 与 couple_mode_t 的取值一一对应，此处独立声明以避免引入 bsp 头文件。
typedef enum {
    COUPLE_AUTO_MODE_BADGE = 0,
    COUPLE_AUTO_MODE_HEARTBEAT = 1,
    COUPLE_AUTO_MODE_TOUCH = 2,
} couple_auto_mode_t;

typedef enum {
    COUPLE_AUTO_NONE = 0,
    COUPLE_AUTO_ENTER_HEARTBEAT,  // 对方在周边 → 自动进入心动模式
    COUPLE_AUTO_ENTER_TOUCH,      // 持续极近 → 自动播放贴贴动画
    COUPLE_AUTO_EXIT_HEARTBEAT,   // 对方离开 → 退回工牌模式
} couple_auto_action_t;

typedef struct {
    bool paired;                   // 本机已配对
    bool partner_present;          // 仍在收到对方心跳
    couple_auto_mode_t mode;       // 当前模式
    bool menu_open;                // 工牌设置菜单处于打开状态
    bool user_dismissed;           // 用户已主动退出心动模式
    uint8_t very_close_count;      // 连续极近心跳计数
    uint32_t now_ms;
    uint32_t last_touch_ms;        // 上次自动贴贴时刻，0 表示从未触发
} couple_auto_input_t;

#define COUPLE_AUTO_TOUCH_THRESHOLD   5
#define COUPLE_AUTO_TOUCH_COOLDOWN_MS 15000

couple_auto_action_t couple_auto_decide(const couple_auto_input_t *in);
