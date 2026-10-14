// main/couple_fonts.c —— 情侣应用字体管理（自定义 Noto Sans SC 子集）
#include "couple_fonts.h"
#include "esp_log.h"

static const char *TAG = "couple_fonts";

// 自定义字体声明（由 lv_font_conv 生成，存于 assets/fonts/）
LV_FONT_DECLARE(couple_font_16);
LV_FONT_DECLARE(couple_font_20);

// 内置 Montserrat 字体（补充 ASCII 图标符号等）
LV_FONT_DECLARE(lv_font_montserrat_14);

// 我们的字体描述符
static lv_font_t s_fonts[COUPLE_FONT_COUNT];
static bool s_initialized = false;

void couple_fonts_init(void)
{
    if (s_initialized) return;

    // SMALL: 自定义 16px 中文为主（所有文字都能显示）
    // Montserrat 14 作为 fallback 补充 ASCII 图标
    s_fonts[COUPLE_FONT_SMALL] = couple_font_16;
    s_fonts[COUPLE_FONT_SMALL].fallback = &lv_font_montserrat_14;

    // NORMAL: 自定义 16px 中文为主
    s_fonts[COUPLE_FONT_NORMAL] = couple_font_16;
    s_fonts[COUPLE_FONT_NORMAL].fallback = &lv_font_montserrat_14;

    // LARGE: 自定义 20px 中文为主
    s_fonts[COUPLE_FONT_LARGE] = couple_font_20;
    s_fonts[COUPLE_FONT_LARGE].fallback = &lv_font_montserrat_14;

    s_initialized = true;
    ESP_LOGI(TAG, "自定义字体初始化完成: small=16, normal=16, large=20");
}

lv_font_t *couple_font_get(couple_font_size_t size)
{
    if (!s_initialized || size >= COUPLE_FONT_COUNT) {
        return &lv_font_montserrat_14;
    }
    return &s_fonts[size];
}
