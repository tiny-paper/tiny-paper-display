/* Reset reporting across USB wake cycles. SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "../src/reset_report.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_watchdog_is_reported_once_then_timer_wakes_are_clean(void)
{
    reset_report_t r;
    reset_report_boot(&r, "task_wdt", "http");
    TEST_ASSERT_EQUAL_STRING("task_wdt", reset_report_summary(&r));
    reset_report_logged(&r); /* hal boot reset=task_wdt */
    TEST_ASSERT_EQUAL_STRING("deepsleep", reset_report_summary(&r));
    /* The first boot cycle can still report the fault to health and update rollback diagnostics. */
    TEST_ASSERT_EQUAL_STRING("task_wdt", r.reason);
    TEST_ASSERT_EQUAL_STRING("http", r.step);
    reset_report_cycle_done(&r);
    /* Reproduce cycles 18-20 and beyond, without a real boot between timer wakes. */
    for (unsigned i = 0; i < 32; i++) {
        TEST_ASSERT_EQUAL_STRING("deepsleep", reset_report_summary(&r));
        TEST_ASSERT_EQUAL_STRING("deepsleep", r.reason);
        TEST_ASSERT_EQUAL_STRING("", r.step);
        reset_report_cycle_done(&r);
    }
    /* A later genuine crash must be visible again, even if it hit the same step. */
    reset_report_boot(&r, "task_wdt", "http");
    TEST_ASSERT_EQUAL_STRING("task_wdt", reset_report_summary(&r));
    TEST_ASSERT_EQUAL_STRING("http", r.step);
    TEST_ASSERT_FALSE(r.boot_logged);
}

static void test_other_boots_and_wake_causes_do_not_repeat_faults(void)
{
    const char *reasons[] = {"poweron", "sw", "panic", "int_wdt", "wdt", "brownout", "deepsleep"};
    for (unsigned i = 0; i < sizeof reasons / sizeof reasons[0]; i++) {
        reset_report_t r;
        reset_report_boot(&r, reasons[i], "");
        TEST_ASSERT_EQUAL_STRING(reasons[i], reset_report_summary(&r));
        reset_report_logged(&r);
        TEST_ASSERT_EQUAL_STRING("deepsleep", reset_report_summary(&r));
        /* Cycle completion applies equally to timer, button and link wakes on USB. */
        reset_report_cycle_done(&r);
        TEST_ASSERT_EQUAL_STRING("deepsleep", r.reason);
        TEST_ASSERT_EQUAL_STRING("", r.step);
    }
}

static uint64_t monotonic_ns(void)
{
    struct timespec t;
    TEST_ASSERT_EQUAL(0, clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static void test_reset_reporting_step_stays_under_two_seconds(void)
{
    uint64_t worst = 0;
    for (unsigned i = 0; i < 10000; i++) {
        reset_report_t r;
        uint64_t start = monotonic_ns();
        reset_report_boot(&r, "task_wdt", "http");
        reset_report_logged(&r);
        const char *summary = reset_report_summary(&r);
        reset_report_cycle_done(&r);
        uint64_t elapsed = monotonic_ns() - start;
        if (elapsed > worst)
            worst = elapsed;
        TEST_ASSERT_EQUAL_STRING("deepsleep", summary);
        TEST_ASSERT_EQUAL_STRING("deepsleep", r.reason);
    }
    TEST_ASSERT_LESS_THAN_UINT64(2000000000ull, worst);
    printf("step name=reset_report host_max_ns=%llu samples=10000 (state handling only)\n",
           (unsigned long long)worst);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_watchdog_is_reported_once_then_timer_wakes_are_clean);
    RUN_TEST(test_other_boots_and_wake_causes_do_not_repeat_faults);
    RUN_TEST(test_reset_reporting_step_stays_under_two_seconds);
    return UNITY_END();
}
