// tests/test_couple_auto.c —— couple_auto_decide 的边界与状态迁移主机测试
#include "couple_auto.h"

#include <assert.h>
#include <stdio.h>

static couple_auto_input_t base_input(void)
{
    couple_auto_input_t in = {
        .paired = true,
        .partner_present = false,
        .mode = COUPLE_AUTO_MODE_BADGE,
        .menu_open = false,
        .user_dismissed = false,
        .very_close_count = 0,
        .now_ms = 100000,
        .last_touch_ms = 0,
    };
    return in;
}

static void test_unpaired_never_triggers(void)
{
    couple_auto_input_t in = base_input();
    in.paired = false;
    in.partner_present = true;
    assert(couple_auto_decide(&in) == COUPLE_AUTO_NONE);

    in.mode = COUPLE_AUTO_MODE_HEARTBEAT;
    in.very_close_count = 200;
    assert(couple_auto_decide(&in) == COUPLE_AUTO_NONE);
}

static void test_absent_partner_exits_heartbeat_modes(void)
{
    couple_auto_input_t in = base_input();
    assert(couple_auto_decide(&in) == COUPLE_AUTO_NONE);

    in.mode = COUPLE_AUTO_MODE_HEARTBEAT;
    assert(couple_auto_decide(&in) == COUPLE_AUTO_EXIT_HEARTBEAT);

    in.mode = COUPLE_AUTO_MODE_TOUCH;
    assert(couple_auto_decide(&in) == COUPLE_AUTO_EXIT_HEARTBEAT);
}

static void test_present_partner_enters_heartbeat(void)
{
    couple_auto_input_t in = base_input();
    in.partner_present = true;
    assert(couple_auto_decide(&in) == COUPLE_AUTO_ENTER_HEARTBEAT);
}

static void test_menu_open_blocks_entry(void)
{
    couple_auto_input_t in = base_input();
    in.partner_present = true;
    in.menu_open = true;
    assert(couple_auto_decide(&in) == COUPLE_AUTO_NONE);
}

static void test_dismissed_blocks_entry(void)
{
    couple_auto_input_t in = base_input();
    in.partner_present = true;
    in.user_dismissed = true;
    assert(couple_auto_decide(&in) == COUPLE_AUTO_NONE);
}

static void test_touch_requires_threshold(void)
{
    couple_auto_input_t in = base_input();
    in.partner_present = true;
    in.mode = COUPLE_AUTO_MODE_HEARTBEAT;

    in.very_close_count = COUPLE_AUTO_TOUCH_THRESHOLD - 1;
    assert(couple_auto_decide(&in) == COUPLE_AUTO_NONE);

    in.very_close_count = COUPLE_AUTO_TOUCH_THRESHOLD;
    assert(couple_auto_decide(&in) == COUPLE_AUTO_ENTER_TOUCH);
}

static void test_touch_cooldown(void)
{
    couple_auto_input_t in = base_input();
    in.partner_present = true;
    in.mode = COUPLE_AUTO_MODE_HEARTBEAT;
    in.very_close_count = COUPLE_AUTO_TOUCH_THRESHOLD + 10;
    in.last_touch_ms = 90000;
    in.now_ms = 90000 + COUPLE_AUTO_TOUCH_COOLDOWN_MS - 1;
    assert(couple_auto_decide(&in) == COUPLE_AUTO_NONE);

    in.now_ms = 90000 + COUPLE_AUTO_TOUCH_COOLDOWN_MS;
    assert(couple_auto_decide(&in) == COUPLE_AUTO_ENTER_TOUCH);
}

static void test_touch_mode_is_stable(void)
{
    couple_auto_input_t in = base_input();
    in.partner_present = true;
    in.mode = COUPLE_AUTO_MODE_TOUCH;
    in.very_close_count = 200;
    assert(couple_auto_decide(&in) == COUPLE_AUTO_NONE);
}

static void test_null_input(void)
{
    assert(couple_auto_decide(NULL) == COUPLE_AUTO_NONE);
}

int main(void)
{
    test_unpaired_never_triggers();
    test_absent_partner_exits_heartbeat_modes();
    test_present_partner_enters_heartbeat();
    test_menu_open_blocks_entry();
    test_dismissed_blocks_entry();
    test_touch_requires_threshold();
    test_touch_cooldown();
    test_touch_mode_is_stable();
    test_null_input();
    printf("test_couple_auto: PASS\n");
    return 0;
}
