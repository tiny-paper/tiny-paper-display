/* Light sleep through a panel BUSY wait (src/busy_sleep.h). SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/busy_sleep.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static bsl_in_t battery(void) { return (bsl_in_t){false, false, false, true, 18000}; }

static void test_battery_sleeps_with_the_button_wake(void)
{
    bsl_in_t in = battery();
    bsl_plan_t p = bsl_plan(&in);
    TEST_ASSERT_EQUAL_UINT32(BSL_MAX_MS, p.ms);
    TEST_ASSERT_TRUE(p.button_wake);
    in.left_ms = 300; /* near the timeout: no longer than that */
    TEST_ASSERT_EQUAL_UINT32(300, bsl_plan(&in).ms);
}

/* the button through the ADC only (low battery: the released pin reads low), or held now: short timer wakes read it */
static void test_button_by_timer_wakes(void)
{
    bsl_in_t in = battery();
    in.button_wake_ok = false;
    bsl_plan_t p = bsl_plan(&in);
    TEST_ASSERT_EQUAL_UINT32(BSL_POLL_MS, p.ms);
    TEST_ASSERT_FALSE(p.button_wake);
    in = battery();
    in.button_down = true; /* a held button would wake at once, again and again */
    p = bsl_plan(&in);
    TEST_ASSERT_EQUAL_UINT32(BSL_POLL_MS, p.ms);
    TEST_ASSERT_FALSE(p.button_wake);
}

static void test_no_sleep_with_wifi_usb_or_little_time(void)
{
    bsl_in_t in = battery();
    in.wifi_on = true;
    TEST_ASSERT_EQUAL_UINT32(0, bsl_plan(&in).ms);
    in = battery();
    in.stay_awake = true;
    TEST_ASSERT_EQUAL_UINT32(0, bsl_plan(&in).ms);
    in = battery();
    in.left_ms = BSL_MIN_MS - 1;
    bsl_plan_t p = bsl_plan(&in);
    TEST_ASSERT_EQUAL_UINT32(0, p.ms);
    TEST_ASSERT_FALSE(p.button_wake);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_battery_sleeps_with_the_button_wake);
    RUN_TEST(test_button_by_timer_wakes);
    RUN_TEST(test_no_sleep_with_wifi_usb_or_little_time);
    return UNITY_END();
}
