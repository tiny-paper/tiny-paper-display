/* RTC clock from raw ticks (src/rtclock.h). SPDX-License-Identifier: GPL-3.0-or-later */
#include <math.h>
#include <stdlib.h>

#include "../src/rtclock.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static uint32_t cal_of(double period_us) { return (uint32_t)llround(period_us * (1 << RTCLOCK_FRACT)); }

/* deterministic noise in [-1, 1] */
static uint32_t rng = 12345;
static double noise(void)
{
    rng = rng * 1103515245u + 12345u;
    return ((rng >> 8) & 0xFFFF) / 32767.5 - 1.0;
}

static void test_basic_and_rebase(void)
{
    rtclock_t c = {0};
    TEST_ASSERT_FALSE(rtclock_ok(&c, 100));
    rtclock_init(&c, 1000, 5000000, cal_of(7.0));
    TEST_ASSERT_TRUE(rtclock_ok(&c, 1000));
    TEST_ASSERT_FALSE(rtclock_ok(&c, 999)); /* counter reset (power-on) */
    TEST_ASSERT_EQUAL_UINT64(5000000, rtclock_us(&c, 1000));
    TEST_ASSERT_EQUAL_UINT64(5000000 + 7000, rtclock_us(&c, 2000));
    /* rebase: continuous, then the new calibration applies from there */
    rtclock_recal(&c, 2000, cal_of(7.0));
    TEST_ASSERT_EQUAL_UINT64(5007000, rtclock_us(&c, 2000));
    /* a value off by > 20 % replaces the smoothed one */
    rtclock_recal(&c, 2000, cal_of(10.0));
    TEST_ASSERT_EQUAL_UINT32(cal_of(10.0), c.cal);
    /* long sleeps: no overflow (a year at 150 kHz) */
    rtclock_init(&c, 0, 0, cal_of(1e6 / 150000.0));
    uint64_t year_ticks = 150000ull * 86400 * 365;
    uint64_t want = (uint64_t)((long double)year_ticks * c.cal / (1 << RTCLOCK_FRACT));
    TEST_ASSERT_UINT64_WITHIN(1, want, rtclock_us(&c, year_ticks));
}

/* TRMNL OG, 2026-09-26: 15 s behind within an hour, drift estimates of -5.6e6 ppb jumping between syncs.
 * ESP-IDF times each deep sleep with a 10-cycle calibration taken just before it (biased and noisy on the
 * 150 kHz RC); the boot calibration (1024 cycles) is good. 24 h of 1 min cycles (5 s awake, 55 s asleep):
 * the ticks counted with the smoothed boot calibration stay within 0.5 s of the oscillator's true time
 * (the oscillator's own drift from nominal is the timekeeper's job), the IDF way is minutes off. */
static void test_day_of_minute_wakes(void)
{
    const double period = 1e6 / 136000.0; /* the RC's true period (us), 136 kHz */
    rtclock_t c;
    rtclock_init(&c, 0, 0, cal_of(period * (1 + 150e-6 * noise())));
    uint64_t ticks = 0;
    double true_us = 0, idf_us = 0;
    double worst = 0;
    for (int m = 0; m < 24 * 60; m++) {
        /* boot: 1024-cycle calibration, +-150 ppm noise */
        rtclock_recal(&c, ticks, cal_of(period * (1 + 150e-6 * noise())));
        uint64_t awake = (uint64_t)llround(5e6 / period), asleep = (uint64_t)llround(55e6 / period);
        ticks += awake + asleep;
        true_us += (double)(awake + asleep) * period;
        /* ESP-IDF: awake part with the boot value, the sleep with a 10-cycle one (-0.5 % bias, +-0.3 % noise) */
        idf_us += awake * period + asleep * period * (1 - 5e-3 + 3e-3 * noise());
        double e = fabs((double)rtclock_us(&c, ticks) - true_us);
        if (e > worst)
            worst = e;
    }
    TEST_ASSERT_TRUE(worst < 500000);
    TEST_ASSERT_TRUE(fabs(idf_us - true_us) > 60e6); /* the old way: over a minute per day */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_basic_and_rebase);
    RUN_TEST(test_day_of_minute_wakes);
    return UNITY_END();
}
