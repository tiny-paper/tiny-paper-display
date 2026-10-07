/* Early timer wakes (wakecal.h). SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdlib.h>

#include "unity.h"
#include "wakecal.h"

void setUp(void) {}
void tearDown(void) {}

/* One minute cycle: sleep from now to the next minute with a timer that fires err_ppm off; returns how early
 * (+) or late (-) the wake is against the plan, in us. */
static int64_t cycle(wakecal_t *c, uint64_t *now, double err_ppm)
{
    uint64_t target = (*now / 60000000 + 1) * 60000000;
    uint64_t ask = wakecal_timer(c, *now, target);
    uint64_t wake = *now + (uint64_t)((double)ask * (1 + err_ppm / 1e6));
    TEST_ASSERT_EQUAL_UINT64(target, wakecal_plan(c));
    wakecal_woke(c, wake, NULL);
    wakecal_clear(c);
    *now = target + 500000; /* awake half a second after the minute */
    return (int64_t)target - (int64_t)wake;
}

static void test_first_sleep_uses_the_initial_margin(void)
{
    wakecal_t c;
    wakecal_reset(&c);
    TEST_ASSERT_TRUE(wakecal_ok(&c));
    TEST_ASSERT_EQUAL_UINT32(WAKECAL_INIT_PPM + WAKECAL_FLOOR_PPM, wakecal_margin_ppm(&c));
    uint64_t ask = wakecal_timer(&c, 1000000, 61000000);
    TEST_ASSERT_EQUAL_UINT64(60000000 - 60 * 8000 - WAKECAL_MIN_US, ask);
    /* short sleeps: never more than half taken off */
    TEST_ASSERT_EQUAL_UINT64(10000, wakecal_timer(&c, 0, 20000));
    TEST_ASSERT_EQUAL_UINT64(0, wakecal_timer(&c, 5, 5));
}

static void test_constant_late_error_is_learned(void)
{
    wakecal_t c;
    wakecal_reset(&c);
    uint64_t now = 30000000;
    for (int i = 0; i < 40; i++)
        TEST_ASSERT_TRUE(cycle(&c, &now, 5000) > 0); /* never late */
    TEST_ASSERT_INT_WITHIN(100, 5000, c.mean_ppm);
    /* the margin came down from the initial one: 5000 ppm + a small deviation + floor */
    TEST_ASSERT_TRUE(wakecal_margin_ppm(&c) < 7000);
    int64_t early = cycle(&c, &now, 5000);
    TEST_ASSERT_TRUE(early > 0 && early < 150000); /* < 150 ms of a minute awake early */
}

static void test_early_error_needs_no_margin_beyond_the_floor(void)
{
    wakecal_t c;
    wakecal_reset(&c);
    uint64_t now = 0;
    for (int i = 0; i < 40; i++)
        cycle(&c, &now, -5000);
    TEST_ASSERT_INT_WITHIN(100, -5000, c.mean_ppm);
    TEST_ASSERT_TRUE(wakecal_margin_ppm(&c) < 3000);
    TEST_ASSERT_TRUE(cycle(&c, &now, -5000) > 0);
}

static void test_random_half_percent_never_late(void)
{
    wakecal_t c;
    wakecal_reset(&c);
    srand(7);
    uint64_t now = 0;
    int64_t max_early = 0;
    for (int i = 0; i < 2000; i++) {
        double e = ((double)rand() / RAND_MAX * 2 - 1) * 5000;
        int64_t early = cycle(&c, &now, e);
        TEST_ASSERT_TRUE_MESSAGE(early > -20000, "late by more than 20 ms");
        if (early > max_early)
            max_early = early;
    }
    TEST_ASSERT_TRUE(max_early < 1600000); /* at most ~1.6 s of a minute early (in light sleep) */
}

static void test_bad_samples_are_dropped(void)
{
    wakecal_t c;
    wakecal_reset(&c);
    int32_t e;
    TEST_ASSERT_FALSE(wakecal_woke(&c, 5, &e)); /* nothing pending */
    wakecal_timer(&c, 1000000, 61000000);
    TEST_ASSERT_FALSE(wakecal_woke(&c, 500000, &e)); /* before the timer started */
    TEST_ASSERT_FALSE(wakecal_woke(&c, 1000000 + 90000000, &e)); /* 50 % late: not a calibration error */
    TEST_ASSERT_EQUAL_UINT32(0, c.n);
    TEST_ASSERT_EQUAL_UINT64(61000000, wakecal_plan(&c));
    TEST_ASSERT_TRUE(wakecal_woke(&c, 1000000 + c.asked_us + c.asked_us / 1000, &e));
    TEST_ASSERT_INT_WITHIN(2, 1000, e);
    wakecal_clear(&c);
    TEST_ASSERT_EQUAL_UINT64(0, wakecal_plan(&c));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_first_sleep_uses_the_initial_margin);
    RUN_TEST(test_constant_late_error_is_learned);
    RUN_TEST(test_early_error_needs_no_margin_beyond_the_floor);
    RUN_TEST(test_random_half_percent_never_late);
    RUN_TEST(test_bad_samples_are_dropped);
    return UNITY_END();
}
