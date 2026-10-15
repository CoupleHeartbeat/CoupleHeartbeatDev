// tests/test_couple_power.c —— 低功耗决策核心的主机测试
#include "couple_power.h"
#include <stdio.h>

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        failures++; \
        printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

static const couple_power_inputs_t IDLE = { false, false, false, false };

static couple_power_action_t eval(couple_power_state_t st, bool manual,
                                  couple_power_inputs_t in,
                                  uint32_t idle, uint32_t warn_elapsed)
{
    couple_power_snap_t snap = { .state = st, .manual = manual };
    return couple_power_next(snap, &in, idle, warn_elapsed);
}

int main(void)
{
    // ---- ACTIVE：静默不足不动作 ----
    CHECK(eval(COUPLE_POWER_STATE_ACTIVE, false, IDLE, 0, 0) ==
          COUPLE_POWER_ACT_NONE);
    CHECK(eval(COUPLE_POWER_STATE_ACTIVE, false, IDLE,
               COUPLE_POWER_SCREEN_OFF_MS - 1, 0) ==
          COUPLE_POWER_ACT_NONE);

    // ---- ACTIVE：静默到点息屏 ----
    CHECK(eval(COUPLE_POWER_STATE_ACTIVE, false, IDLE,
               COUPLE_POWER_SCREEN_OFF_MS, 0) ==
          COUPLE_POWER_ACT_SCREEN_OFF);

    // ---- ACTIVE：互动抑制息屏 ----
    couple_power_inputs_t partner = IDLE; partner.partner_present = true;
    couple_power_inputs_t ai = IDLE;      ai.mode_ai = true;
    CHECK(eval(COUPLE_POWER_STATE_ACTIVE, false, partner,
               COUPLE_POWER_SCREEN_OFF_MS, 0) == COUPLE_POWER_ACT_NONE);
    CHECK(eval(COUPLE_POWER_STATE_ACTIVE, false, ai,
               COUPLE_POWER_SCREEN_OFF_MS, 0) == COUPLE_POWER_ACT_NONE);

    // ---- ACTIVE：会话占用只抑制深睡，不抑制息屏 ----
    couple_power_inputs_t wifi = IDLE; wifi.wifi_busy = true;
    couple_power_inputs_t app = IDLE;  app.app_connected = true;
    CHECK(eval(COUPLE_POWER_STATE_ACTIVE, false, wifi,
               COUPLE_POWER_DEEP_AFTER_MS, 0) ==
          COUPLE_POWER_ACT_SCREEN_OFF);
    CHECK(eval(COUPLE_POWER_STATE_ACTIVE, false, app,
               COUPLE_POWER_DEEP_AFTER_MS, 0) ==
          COUPLE_POWER_ACT_SCREEN_OFF);

    // ---- ACTIVE：无操作到点进警告屏 ----
    CHECK(eval(COUPLE_POWER_STATE_ACTIVE, false, IDLE,
               COUPLE_POWER_DEEP_AFTER_MS, 0) == COUPLE_POWER_ACT_WARN);

    // ---- SCREEN_OFF：靠近自动点亮 ----
    CHECK(eval(COUPLE_POWER_STATE_SCREEN_OFF, false, partner,
               COUPLE_POWER_SCREEN_OFF_MS, 0) ==
          COUPLE_POWER_ACT_SCREEN_ON);

    // ---- SCREEN_OFF：静默到点进警告屏；会话占用时不进 ----
    CHECK(eval(COUPLE_POWER_STATE_SCREEN_OFF, false, IDLE,
               COUPLE_POWER_DEEP_AFTER_MS, 0) == COUPLE_POWER_ACT_WARN);
    CHECK(eval(COUPLE_POWER_STATE_SCREEN_OFF, false, wifi,
               COUPLE_POWER_DEEP_AFTER_MS, 0) == COUPLE_POWER_ACT_NONE);

    // ---- SLEEP_WARN（自动）：环境恢复活跃即取消 ----
    CHECK(eval(COUPLE_POWER_STATE_SLEEP_WARN, false, partner, 0, 0) ==
          COUPLE_POWER_ACT_SCREEN_ON);
    CHECK(eval(COUPLE_POWER_STATE_SLEEP_WARN, false, wifi, 0, 0) ==
          COUPLE_POWER_ACT_SCREEN_ON);
    CHECK(eval(COUPLE_POWER_STATE_SLEEP_WARN, false, app, 0, 0) ==
          COUPLE_POWER_ACT_SCREEN_ON);

    // ---- SLEEP_WARN（自动）：时限未到维持，到点深睡 ----
    CHECK(eval(COUPLE_POWER_STATE_SLEEP_WARN, false, IDLE, 0,
               COUPLE_POWER_WARN_MS - 1) == COUPLE_POWER_ACT_NONE);
    CHECK(eval(COUPLE_POWER_STATE_SLEEP_WARN, false, IDLE, 0,
               COUPLE_POWER_WARN_MS) == COUPLE_POWER_ACT_SLEEP);

    // ---- SLEEP_WARN（手动）：不可被环境取消，时限更短 ----
    CHECK(eval(COUPLE_POWER_STATE_SLEEP_WARN, true, partner, 0, 0) ==
          COUPLE_POWER_ACT_NONE);
    CHECK(eval(COUPLE_POWER_STATE_SLEEP_WARN, true, wifi, 0, 0) ==
          COUPLE_POWER_ACT_NONE);
    CHECK(eval(COUPLE_POWER_STATE_SLEEP_WARN, true, IDLE, 0,
               COUPLE_POWER_WARN_MANUAL_MS - 1) == COUPLE_POWER_ACT_NONE);
    CHECK(eval(COUPLE_POWER_STATE_SLEEP_WARN, true, IDLE, 0,
               COUPLE_POWER_WARN_MANUAL_MS) == COUPLE_POWER_ACT_SLEEP);

    if (failures == 0) {
        printf("test_couple_power: PASS\n");
        return 0;
    }
    printf("test_couple_power: %d failures\n", failures);
    return 1;
}
