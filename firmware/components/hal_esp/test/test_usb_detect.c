/* USB host detection from the SOF frame counter (src/usb_detect.h). SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/usb_detect.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* Fake chip: time since boot, a host that starts sending SOFs at `enum_us` (never when < 0), and
 * a frame index that is stuck before that (at a value the counter may hold from an earlier session). */
typedef struct {
    int64_t t, enum_us, gone_us;
    uint32_t base;
    int frames, sleeps;
} fake_t;
static fake_t F;

static uint32_t f_frame(void *c)
{
    F.frames++;
    bool on = F.enum_us >= 0 && F.t >= F.enum_us && (F.gone_us < 0 || F.t < F.gone_us);
    /* extra bits above the 11-bit index must be ignored */
    return 0xfffff800u | (on ? (uint32_t)(F.base + F.t / 1000) : F.base);
}
static int64_t f_now(void *c) { return F.t; }
static void f_delay(void *c, uint32_t us) { F.t += us; }
static void f_sleep(void *c, uint32_t ms)
{
    F.sleeps++;
    F.t += (int64_t)ms * 1000;
}

static usbdet_t ud;

static void boot(int64_t t0, int64_t enum_us)
{
    F = (fake_t){.t = t0, .enum_us = enum_us, .gone_us = -1, .base = 0x123};
    ud = (usbdet_t){.frame = f_frame, .now_us = f_now, .delay_us = f_delay, .sleep_ms = f_sleep};
    usbdet_init(&ud);
}

/* Real OG, 2026-09-24: plugged into a laptop, the app went to deep sleep 4 s after boot because the
 * host "was not there" at the first look. The host enumerates ~1.2 s after a reset: wait for it. */
static void test_host_enumerating_after_reset(void)
{
    boot(40000, 1200000);
    TEST_ASSERT_TRUE(usbdet_present(&ud, false));
    TEST_ASSERT_TRUE(F.t >= 1200000 && F.t < 1250000);
}

/* A deep-sleep timer wake (battery) must not wait 1.5 s for a host that is not there. */
static void test_quick_on_timer_wake(void)
{
    boot(20000, -1);
    TEST_ASSERT_FALSE(usbdet_present(&ud, true));
    TEST_ASSERT_TRUE(F.t <= USBDET_QUICK_US + USBDET_POLL_MS * 1000 + USBDET_SAMPLE_US);
    /* and with a host already there, one sample */
    boot(20000, 0);
    TEST_ASSERT_TRUE(usbdet_present(&ud, true));
    TEST_ASSERT_EQUAL(0, F.sleeps);
}

/* No host after a reset: gives up after the enumeration window. */
static void test_no_host_after_reset(void)
{
    boot(40000, -1);
    TEST_ASSERT_FALSE(usbdet_present(&ud, false));
    TEST_ASSERT_TRUE(F.t >= USBDET_ENUM_US && F.t < USBDET_ENUM_US + 30000);
}

/* The 11-bit index wraps (2.048 s): 0x7ff -> 0x000 is still movement. */
static void test_wrap_is_movement(void)
{
    boot(0, 0);
    F.base = 0x7fe;
    F.t = 0;
    TEST_ASSERT_TRUE(usbdet_present(&ud, true));
}

/* Later calls: cached for 100 ms, then sampled again; an unplug is seen after the grace of still samples. */
static void test_recheck_and_unplug(void)
{
    boot(40000, 0);
    TEST_ASSERT_TRUE(usbdet_present(&ud, false));
    int n = F.frames;
    F.t += 50000;
    TEST_ASSERT_TRUE(usbdet_present(&ud, false));
    TEST_ASSERT_EQUAL(n, F.frames);
    F.gone_us = F.t;
    F.t += USBDET_LOST_US + 200000;
    TEST_ASSERT_TRUE(usbdet_present(&ud, false)); /* the first still sample: the grace starts here */
    for (int i = 0; i < 30; i++)
        F.t += 150000, (void)usbdet_present(&ud, false);
    TEST_ASSERT_FALSE(usbdet_present(&ud, false));
    TEST_ASSERT_EQUAL(0, F.sleeps); /* no second enumeration wait */
}

/* A host stall shorter than the grace (suspend, busy host) is not an unplug; a stall past it is; SOFs back = present. */
static void test_short_stall_is_not_unplug(void)
{
    boot(40000, 0);
    TEST_ASSERT_TRUE(usbdet_present(&ud, false));
    F.gone_us = F.t;
    for (int i = 0; i < 10; i++) { /* 1.5 s of checks with no SOF */
        F.t += 150000;
        TEST_ASSERT_TRUE(usbdet_present(&ud, false));
    }
    F.gone_us = -1;
    F.t += 150000;
    TEST_ASSERT_TRUE(usbdet_present(&ud, false));
    F.gone_us = F.t;
    for (int i = 0; i < 30; i++)
        F.t += 150000, (void)usbdet_present(&ud, false);
    TEST_ASSERT_FALSE(usbdet_present(&ud, false));
}

/* The device keeps the state in static storage and never calls usbdet_init (esp/link.c): a zeroed state must be a
 * valid start. Issue #118: `seen` = 0 read as "a SOF at boot", so every battery wake had a host for its first 3 s
 * and the app kept the link's ~25 KiB for the whole wake (OG heap low 21.8 KB -> 2.7 KB, 2026-10-05). */
static void test_zero_state_battery_wake_has_no_host(void)
{
    F = (fake_t){.t = 100000, .enum_us = -1, .gone_us = -1, .base = 0x123}; /* first look after the quick window */
    ud = (usbdet_t){.frame = f_frame, .now_us = f_now, .delay_us = f_delay, .sleep_ms = f_sleep};
    for (int i = 0; i < 30; i++) { /* the whole grace and past it */
        TEST_ASSERT_FALSE(usbdet_present(&ud, true));
        F.t += 150000;
    }
    F = (fake_t){.t = 40000, .enum_us = -1, .gone_us = -1, .base = 0x123}; /* and after a reset: the full wait, then no */
    ud = (usbdet_t){.frame = f_frame, .now_us = f_now, .delay_us = f_delay, .sleep_ms = f_sleep};
    TEST_ASSERT_FALSE(usbdet_present(&ud, false));
    F.t += 150000;
    TEST_ASSERT_FALSE(usbdet_present(&ud, false));
}

/* A long stretch without a look (a 40 s photo download, a WiFi session): the grace runs from the first still sample,
 * not from the last SOF seen before the stretch. Before: one still sample after such a stretch was an unplug at once
 * (TRMNL X 2026-10-07: deep sleep with the browser's port open, the link back only at a hold 30 s later; issue #133). */
static void test_still_sample_after_a_long_gap_starts_the_grace(void)
{
    boot(40000, 0);
    TEST_ASSERT_TRUE(usbdet_present(&ud, false));
    F.gone_us = F.t; /* SOFs stop right after that look */
    F.t += 40000000; /* 40 s without a look */
    TEST_ASSERT_TRUE(usbdet_present(&ud, false)); /* the first still sample is not an unplug */
    for (int i = 0; i < 10; i++) { /* 1.5 s of still samples: still within the grace */
        F.t += 150000;
        TEST_ASSERT_TRUE(usbdet_present(&ud, false));
    }
    F.gone_us = -1; /* SOFs back (a host that suspended the bus and resumed) */
    F.t += 150000;
    TEST_ASSERT_TRUE(usbdet_present(&ud, false));
    /* still past the whole grace after the gap: gone */
    F.gone_us = F.t;
    F.t += 40000000;
    for (int i = 0; i < 30; i++)
        F.t += 150000, (void)usbdet_present(&ud, false);
    TEST_ASSERT_FALSE(usbdet_present(&ud, false));
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_host_enumerating_after_reset);
    RUN_TEST(test_quick_on_timer_wake);
    RUN_TEST(test_no_host_after_reset);
    RUN_TEST(test_wrap_is_movement);
    RUN_TEST(test_recheck_and_unplug);
    RUN_TEST(test_short_stall_is_not_unplug);
    RUN_TEST(test_zero_state_battery_wake_has_no_host);
    RUN_TEST(test_still_sample_after_a_long_gap_starts_the_grace);
    return UNITY_END();
}
