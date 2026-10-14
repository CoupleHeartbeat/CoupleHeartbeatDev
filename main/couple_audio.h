// main/couple_audio.h —— 情侣应用心跳音效：软件生成 + 音量控制
#pragma once

#include <stdint.h>

// 初始化音频 codec 并启动播放任务（初始静音，等待 play_heartbeat 触发）
void couple_audio_init(void);

// 设置主音量 0-100；为 0 时完全静音（不输出任何 PCM）
void couple_audio_set_volume(uint8_t vol);

// 获取当前主音量
uint8_t couple_audio_get_volume(void);

// 根据距离等级播放心跳声
//   level 0: 停止播放（静音）
//   level 1: 慢心跳 ~75 BPM，音量小
//   level 2: 中心跳 ~100 BPM，音量中
//   level 3: 快心跳 ~150 BPM，音量大
void couple_audio_play_heartbeat(uint8_t distance_level);

// 停止所有音频，释放播放任务
void couple_audio_stop(void);
