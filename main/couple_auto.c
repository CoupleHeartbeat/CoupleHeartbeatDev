// main/couple_auto.c —— 靠近感知的纯逻辑判定
#include "couple_auto.h"

couple_glow_level_t couple_auto_decide(const couple_auto_input_t *in)
{
    if (!in || !in->paired || !in->partner_present) return COUPLE_GLOW_OFF;

    if (in->rssi >= COUPLE_AUTO_RSSI_TOUCH) {
        // 极近还要连续若干拍才算贴贴，否则停在“很近了”
        return in->very_close_count >= COUPLE_AUTO_TOUCH_THRESHOLD
                   ? COUPLE_GLOW_TOUCH
                   : COUPLE_GLOW_CLOSE;
    }
    if (in->rssi >= COUPLE_AUTO_RSSI_CLOSE) return COUPLE_GLOW_CLOSE;
    if (in->rssi >= COUPLE_AUTO_RSSI_NEAR) return COUPLE_GLOW_NEAR;
    return COUPLE_GLOW_OFF;
}
