// main/couple_fonts.h —— 情侣应用字体管理（中文 + ASCII）
#pragma once

#include "lvgl.h"

// 字体大小枚举
typedef enum {
    COUPLE_FONT_SMALL = 0,   // 小字 ~14px（状态、提示）
    COUPLE_FONT_NORMAL,      // 正常 ~16px（日期、姓名、中文正文）
    COUPLE_FONT_LARGE,       // 大字 ~20px（时间、标题）
    COUPLE_FONT_COUNT,
} couple_font_size_t;

// 初始化字体（必须在 LVGL 初始化后、创建 UI 前调用）
void couple_fonts_init(void);

// 获取指定大小的字体
lv_font_t *couple_font_get(couple_font_size_t size);
