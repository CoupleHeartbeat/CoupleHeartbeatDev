// tests/test_couple_auto.c —— 靠近判定分档的边界主机测试
#include "couple_auto.h"

#include <assert.h>
#include <stdio.h>

static couple_auto_input_t base_input(void)
{
    couple_auto_input_t in = {
        .paired = true,
        .partner_present = true,
        .rssi = COUPLE_AUTO_RSSI_NEAR,
        .very_close_count = 0,
    };
    return in;
}

static void test_unpaired_is_off(void)
{
    couple_auto_input_t in = base_input();
    in.paired = false;
    in.rssi = -30;
    in.very_close_count = 200;
    assert(couple_auto_decide(&in) == COUPLE_GLOW_OFF);
}

static void test_absent_partner_is_off(void)
{
    couple_auto_input_t in = base_input();
    in.partner_present = false;
    in.rssi = -30;
    assert(couple_auto_decide(&in) == COUPLE_GLOW_OFF);
}

static void test_too_far_is_off(void)
{
    couple_auto_input_t in = base_input();
    in.rssi = COUPLE_AUTO_RSSI_NEAR - 1;
    assert(couple_auto_decide(&in) == COUPLE_GLOW_OFF);

    in.rssi = -100;
    assert(couple_auto_decide(&in) == COUPLE_GLOW_OFF);
}

static void test_band_boundaries(void)
{
    couple_auto_input_t in = base_input();

    in.rssi = COUPLE_AUTO_RSSI_NEAR;
    assert(couple_auto_decide(&in) == COUPLE_GLOW_NEAR);

    in.rssi = COUPLE_AUTO_RSSI_CLOSE - 1;
    assert(couple_auto_decide(&in) == COUPLE_GLOW_NEAR);

    in.rssi = COUPLE_AUTO_RSSI_CLOSE;
    assert(couple_auto_decide(&in) == COUPLE_GLOW_CLOSE);

    // 极近但没有持续够拍数时停在“很近了”
    in.rssi = COUPLE_AUTO_RSSI_TOUCH;
    assert(couple_auto_decide(&in) == COUPLE_GLOW_CLOSE);

    in.rssi = -30;
    assert(couple_auto_decide(&in) == COUPLE_GLOW_CLOSE);
}

static void test_touch_requires_sustained_closeness(void)
{
    couple_auto_input_t in = base_input();
    in.rssi = COUPLE_AUTO_RSSI_TOUCH;

    in.very_close_count = COUPLE_AUTO_TOUCH_THRESHOLD - 1;
    assert(couple_auto_decide(&in) == COUPLE_GLOW_CLOSE);

    in.very_close_count = COUPLE_AUTO_TOUCH_THRESHOLD;
    assert(couple_auto_decide(&in) == COUPLE_GLOW_TOUCH);

    in.very_close_count = 255;
    assert(couple_auto_decide(&in) == COUPLE_GLOW_TOUCH);
}

static void test_touch_count_does_not_leak_into_far_bands(void)
{
    couple_auto_input_t in = base_input();
    in.very_close_count = 255;

    in.rssi = COUPLE_AUTO_RSSI_TOUCH - 1;   // -46，落回“很近了”档
    assert(couple_auto_decide(&in) == COUPLE_GLOW_CLOSE);

    in.rssi = COUPLE_AUTO_RSSI_NEAR;
    assert(couple_auto_decide(&in) == COUPLE_GLOW_NEAR);
}

static void test_null_input(void)
{
    assert(couple_auto_decide(NULL) == COUPLE_GLOW_OFF);
}

int main(void)
{
    test_unpaired_is_off();
    test_absent_partner_is_off();
    test_too_far_is_off();
    test_band_boundaries();
    test_touch_requires_sustained_closeness();
    test_touch_count_does_not_leak_into_far_bands();
    test_null_input();
    printf("test_couple_auto: PASS\n");
    return 0;
}
