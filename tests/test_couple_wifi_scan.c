// tests/test_couple_wifi_scan.c —— Wi-Fi 扫描结果整理的边界主机测试
#include "couple_wifi_scan.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static couple_wifi_scan_list_t list_of(const char **ssids, const int8_t *rssis, int n)
{
    couple_wifi_scan_list_t l;
    couple_wifi_scan_clear(&l);
    for (int i = 0; i < n; i++) {
        couple_wifi_scan_add(&l, ssids[i], rssis[i]);
    }
    return l;
}

static void test_rejects_bad_input(void)
{
    couple_wifi_scan_list_t l;
    couple_wifi_scan_clear(&l);

    assert(!couple_wifi_scan_add(&l, NULL, -50));
    assert(!couple_wifi_scan_add(&l, "", -50));          // 隐藏 SSID
    assert(l.count == 0);
    assert(!couple_wifi_scan_add(&l, NULL, -50));

    char too_long[COUPLE_WIFI_SCAN_SSID_MAX + 4];
    memset(too_long, 'a', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    assert(!couple_wifi_scan_add(&l, too_long, -50));
    assert(l.count == 0);
}

static void test_sorted_by_signal(void)
{
    const char *ssids[] = {"weak", "strong", "mid"};
    const int8_t rssis[] = {-80, -40, -60};
    couple_wifi_scan_list_t l = list_of(ssids, rssis, 3);

    assert(l.count == 3);
    assert(strcmp(l.aps[0].ssid, "strong") == 0);
    assert(strcmp(l.aps[1].ssid, "mid") == 0);
    assert(strcmp(l.aps[2].ssid, "weak") == 0);
}

static void test_duplicate_keeps_strongest(void)
{
    const char *ssids[] = {"home", "home", "home"};
    const int8_t rssis[] = {-70, -45, -60};
    couple_wifi_scan_list_t l = list_of(ssids, rssis, 3);

    assert(l.count == 1);
    assert(strcmp(l.aps[0].ssid, "home") == 0);
    assert(l.aps[0].rssi == -45);
}

static void test_duplicate_weaker_is_ignored(void)
{
    couple_wifi_scan_list_t l;
    couple_wifi_scan_clear(&l);
    assert(couple_wifi_scan_add(&l, "home", -45));
    assert(!couple_wifi_scan_add(&l, "home", -70));
    assert(l.count == 1);
    assert(l.aps[0].rssi == -45);
}

static void test_duplicate_stronger_reorders(void)
{
    const char *ssids[] = {"a", "b", "b"};
    const int8_t rssis[] = {-70, -60, -40};
    couple_wifi_scan_list_t l = list_of(ssids, rssis, 3);

    assert(l.count == 2);
    assert(strcmp(l.aps[0].ssid, "b") == 0);
    assert(l.aps[0].rssi == -40);
    assert(strcmp(l.aps[1].ssid, "a") == 0);
}

static void test_capacity_drops_weakest(void)
{
    couple_wifi_scan_list_t l;
    couple_wifi_scan_clear(&l);

    char name[16];
    // 先塞满：信号从 -40 递减，最弱在末尾
    for (int i = 0; i < COUPLE_WIFI_SCAN_MAX; i++) {
        snprintf(name, sizeof(name), "ap%d", i);
        assert(couple_wifi_scan_add(&l, name, (int8_t)(-40 - i)));
    }
    assert(l.count == COUPLE_WIFI_SCAN_MAX);

    // 比最弱的一条更弱：应被丢弃
    assert(!couple_wifi_scan_add(&l, "new-weak", -120));
    assert(l.count == COUPLE_WIFI_SCAN_MAX);

    // 强于最弱的一条：挤掉最弱的
    assert(couple_wifi_scan_add(&l, "new-strong", -30));
    assert(l.count == COUPLE_WIFI_SCAN_MAX);
    assert(strcmp(l.aps[0].ssid, "new-strong") == 0);
    for (uint8_t i = 0; i < l.count; i++) {
        assert(strcmp(l.aps[i].ssid, "new-weak") != 0);
        assert(strcmp(l.aps[i].ssid, "ap12") != 0);   // 原最弱
    }
}

static void test_clear(void)
{
    couple_wifi_scan_list_t l;
    couple_wifi_scan_clear(&l);
    assert(couple_wifi_scan_add(&l, "x", -50));
    couple_wifi_scan_clear(&l);
    assert(l.count == 0);
}

static void test_entry_bytes_leading_comma(void)
{
    assert(couple_wifi_scan_entry_bytes("ab", true) ==
           couple_wifi_scan_entry_bytes("ab", false) - 1);
    assert(couple_wifi_scan_entry_bytes("", true) == 24);
}

static void test_fit_respects_budget(void)
{
    const char *ssids[] = {"aaaaaaaa", "bbbbbbbb", "cccccccc", "dddddddd"};
    const int8_t rssis[] = {-40, -50, -60, -70};
    couple_wifi_scan_list_t l = list_of(ssids, rssis, 4);

    // 预算小于外壳长度：一条也放不下
    assert(couple_wifi_scan_fit(&l, 10) == 0);

    // 按真实外壳与条目长度算边界：刚好放下 1 条、少 1 字节就放不下
    const size_t env = couple_wifi_scan_envelope_bytes();
    const size_t first = couple_wifi_scan_entry_bytes("aaaaaaaa", true);
    assert(couple_wifi_scan_fit(&l, env + first) == 1);
    assert(couple_wifi_scan_fit(&l, env + first - 1) == 0);

    // 预算放大后条数单调不减，且不超过总数
    uint8_t prev = 0;
    for (size_t budget = env; budget <= env + 400; budget += 7) {
        uint8_t fit = couple_wifi_scan_fit(&l, budget);
        assert(fit >= prev);
        assert(fit <= l.count);
        prev = fit;
    }
}

static void test_escape_and_control_chars(void)
{
    couple_wifi_scan_list_t l;
    couple_wifi_scan_clear(&l);

    // 引号与反斜杠按 JSON 转义后计入长度
    assert(couple_wifi_scan_escaped_len("ab") == 2);
    assert(couple_wifi_scan_escaped_len("a\"b") == 4);
    assert(couple_wifi_scan_escaped_len("a\\b") == 4);
    assert(couple_wifi_scan_entry_bytes("a\"b", true) ==
           couple_wifi_scan_entry_bytes("aab", true) + 1);

    // 控制字符无法安全内联进 JSON，直接不收
    assert(!couple_wifi_scan_add(&l, "bad\nname", -40));
    assert(!couple_wifi_scan_add(&l, "bad\tname", -40));
    assert(l.count == 0);

    // 含引号的 SSID 可以收，交给序列化侧转义
    assert(couple_wifi_scan_add(&l, "quote\"net", -40));
    assert(l.count == 1);
}

int main(void)
{
    test_rejects_bad_input();
    test_sorted_by_signal();
    test_duplicate_keeps_strongest();
    test_duplicate_weaker_is_ignored();
    test_duplicate_stronger_reorders();
    test_capacity_drops_weakest();
    test_clear();
    test_entry_bytes_leading_comma();
    test_escape_and_control_chars();
    test_fit_respects_budget();
    printf("test_couple_wifi_scan: PASS\n");
    return 0;
}
