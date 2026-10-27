// main/couple_power.h —— 低功耗策略（纯逻辑可主机测试）+ 设备侧入口
//
// 两级自动策略 + 设置菜单手动入口：
//   息屏：无操作 60s 且对方不在附近/AI 对话中 → 关背光；按键或对方靠近立即点亮。
//   深睡：无操作 15min 且无任何活跃会话 → 警告屏 10s（自动路径可按键取消）→ 深睡。
//   手动：设置菜单"立即休眠" → 警告屏 2s（不可取消）→ 深睡。
// 深睡期间 BLE 全停（设备不再连 Wi-Fi），按任意键唤醒并重启（重启后由 key_guard
// 吞掉唤醒键残留）。
//
// 决策核心 couple_power_next() 不依赖 ESP-IDF / LVGL；执行（背光、深睡序列）在
// couple_power_exec.c，由 firmware 构建验证。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// ============================================================================
// 时序常量（毫秒）
// ============================================================================
#define COUPLE_POWER_SCREEN_OFF_MS   60000u             // 无操作息屏
#define COUPLE_POWER_DEEP_AFTER_MS   (15u * 60u * 1000u) // 无操作深睡（含息屏期）
#define COUPLE_POWER_WARN_MS         10000u              // 自动深睡警告时长
#define COUPLE_POWER_WARN_MANUAL_MS  2000u               // 手动休眠警告时长
// 低功耗屏背光百分比：息屏后不再全黑，调暗到该亮度只显示姓名/时间/年月日。
// 深睡仍在 15 分钟无操作后执行，因此这点背光功耗可接受。
#define COUPLE_POWER_DIM_PERCENT     10

typedef enum {
    COUPLE_POWER_STATE_ACTIVE = 0,  // 正常使用，背光亮
    COUPLE_POWER_STATE_SCREEN_OFF,  // 背光灭，心跳扫描照常
    COUPLE_POWER_STATE_SLEEP_WARN,  // 深睡警告屏（自动路径可取消）
} couple_power_state_t;

typedef enum {
    COUPLE_POWER_ACT_NONE = 0,      // 维持现状
    COUPLE_POWER_ACT_SCREEN_OFF,    // 关背光
    COUPLE_POWER_ACT_SCREEN_ON,     // 开背光（唤醒/取消警告）
    COUPLE_POWER_ACT_WARN,          // 打开深睡警告屏
    COUPLE_POWER_ACT_SLEEP,         // 执行深睡（不返回）
} couple_power_action_t;

// 影响休眠决策的输入快照（调用方每拍采集）
typedef struct {
    bool mode_ai;          // AI 对话进行中（交互密集，抑制息屏与深睡）
    bool partner_present;  // 对方心跳在线（互动即使用，抑制息屏与深睡）
    bool app_connected;    // App 经 GATT 在线（抑制深睡，避免配置/对话中断）
} couple_power_inputs_t;

typedef struct {
    couple_power_state_t state;
    bool manual;  // 处于手动触发的警告屏（不可取消）
} couple_power_snap_t;

// ============================================================================
// 纯决策核心（可主机测试）
// ============================================================================
// idle_ms：距上次按键/活跃的静默时长；warn_elapsed_ms：警告屏已持续时长。
couple_power_action_t couple_power_next(couple_power_snap_t snap,
                                        const couple_power_inputs_t *in,
                                        uint32_t idle_ms,
                                        uint32_t warn_elapsed_ms);

// ============================================================================
// 设备侧入口（couple_power_exec.c 实现；tick/notify_key 均可阻塞，勿在中断调用）
// ============================================================================
void couple_power_init(void);

// 主循环每拍（200ms）调一次；内部按决策开关背光、显示警告屏或进入深睡。
void couple_power_tick(const couple_power_inputs_t *in);

// 任意按键事件先调它。返回 true 表示事件已被消费（仅用于唤醒屏幕/取消警告），
// 调用方不应再分发给页面。
bool couple_power_notify_key(void);

// 设置菜单"立即休眠"入口：进入不可取消的警告屏，短暂延时后深睡。
void couple_power_request_sleep_now(void);

couple_power_state_t couple_power_get_state(void);
