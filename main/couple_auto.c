// main/couple_auto.c —— 靠近自动切换模式的纯逻辑判定
#include "couple_auto.h"

couple_auto_action_t couple_auto_decide(const couple_auto_input_t *in)
{
    if (!in || !in->paired) return COUPLE_AUTO_NONE;

    if (!in->partner_present) {
        // 对方离开：心动/贴贴自动退回工牌；已在工牌则无事可做。
        return in->mode == COUPLE_AUTO_MODE_BADGE ? COUPLE_AUTO_NONE
                                                  : COUPLE_AUTO_EXIT_HEARTBEAT;
    }

    switch (in->mode) {
    case COUPLE_AUTO_MODE_BADGE:
        // 设置菜单打开时不得打断；用户主动退出后不再自动重入，
        // 直到对方离开（心跳超时）后重新靠近。
        if (in->menu_open || in->user_dismissed) return COUPLE_AUTO_NONE;
        return COUPLE_AUTO_ENTER_HEARTBEAT;

    case COUPLE_AUTO_MODE_HEARTBEAT:
        if (in->very_close_count < COUPLE_AUTO_TOUCH_THRESHOLD) {
            return COUPLE_AUTO_NONE;
        }
        if (in->last_touch_ms != 0 &&
            (uint32_t)(in->now_ms - in->last_touch_ms) < COUPLE_AUTO_TOUCH_COOLDOWN_MS) {
            return COUPLE_AUTO_NONE;
        }
        return COUPLE_AUTO_ENTER_TOUCH;

    default:
        return COUPLE_AUTO_NONE;
    }
}
