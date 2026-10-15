// main/couple_power.c —— 低功耗策略决策核心（纯逻辑，可主机测试）
#include "couple_power.h"

couple_power_action_t couple_power_next(couple_power_snap_t snap,
                                        const couple_power_inputs_t *in,
                                        uint32_t idle_ms,
                                        uint32_t warn_elapsed_ms)
{
    if (!in) return COUPLE_POWER_ACT_NONE;

    // 互动即使用：AI 对话或对方在附近时抑制息屏与深睡
    const bool interacting = in->mode_ai || in->partner_present;
    // 深睡额外要求：没有进行中的配置会话（配网/App 连接）
    const bool session_busy = in->wifi_busy || in->app_connected;

    switch (snap.state) {
    case COUPLE_POWER_STATE_ACTIVE:
        if (!interacting && !session_busy &&
            idle_ms >= COUPLE_POWER_DEEP_AFTER_MS) {
            return COUPLE_POWER_ACT_WARN;
        }
        if (!interacting && idle_ms >= COUPLE_POWER_SCREEN_OFF_MS) {
            return COUPLE_POWER_ACT_SCREEN_OFF;
        }
        return COUPLE_POWER_ACT_NONE;

    case COUPLE_POWER_STATE_SCREEN_OFF:
        // 靠近伴侣自动点亮：心跳到达本身就是交互
        if (in->partner_present) return COUPLE_POWER_ACT_SCREEN_ON;
        if (!in->mode_ai && !session_busy &&
            idle_ms >= COUPLE_POWER_DEEP_AFTER_MS) {
            return COUPLE_POWER_ACT_WARN;
        }
        return COUPLE_POWER_ACT_NONE;

    case COUPLE_POWER_STATE_SLEEP_WARN:
        if (snap.manual) {
            // 手动路径不可取消，仅按时限进入深睡
            if (warn_elapsed_ms >= COUPLE_POWER_WARN_MANUAL_MS) {
                return COUPLE_POWER_ACT_SLEEP;
            }
            return COUPLE_POWER_ACT_NONE;
        }
        // 自动路径：等待期间环境恢复活跃则取消休眠
        if (interacting || session_busy) return COUPLE_POWER_ACT_SCREEN_ON;
        if (warn_elapsed_ms >= COUPLE_POWER_WARN_MS) {
            return COUPLE_POWER_ACT_SLEEP;
        }
        return COUPLE_POWER_ACT_NONE;

    default:
        return COUPLE_POWER_ACT_NONE;
    }
}
