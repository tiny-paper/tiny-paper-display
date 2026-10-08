/* The panel rails policy (epd_rails.h) against a model of the supply and of a one-shot timer: with no hold the
 * calls are those of before; with a hold the next update skips the power up; and the rails come down when the time
 * passes, on a failed update, a failed power up, no timer, a hold set to 0, and before sleep. The invariant checked
 * after every event of long random runs: rails up means an update is running or a timer is armed that will bring
 * them down. Also: every sleep entry of the firmware powers them down first (the source is read).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>

#include "epd_rails.h"
#include "unity.h"

static struct {
    bool up;        /* the supply */
    bool timer;     /* armed */
    uint32_t timer_ms;
    int fail_up;    /* the next power up fails with this */
    bool no_timer;
    int ups, downs, arms, disarms;
} M;
static epd_rails_t R;

static int m_power(void *ctx, bool on)
{
    (void)ctx;
    if (on) {
        M.ups++;
        if (M.fail_up) {
            int rc = M.fail_up;
            M.fail_up = 0;
            M.up = true; /* the worst case: it reports a failure and is up */
            return rc;
        }
        M.up = true;
    } else {
        M.downs++;
        M.up = false;
    }
    return 0;
}
static bool m_arm(void *ctx, uint32_t ms)
{
    (void)ctx;
    if (M.no_timer)
        return false;
    M.arms++;
    M.timer = true;
    M.timer_ms = ms;
    return true;
}
static void m_disarm(void *ctx)
{
    (void)ctx;
    M.disarms++;
    M.timer = false;
}
static const epd_rails_io_t IO = {m_power, m_arm, m_disarm};

/* the timer of the model fires */
static void fire(void)
{
    if (!M.timer)
        return;
    M.timer = false;
    epd_rails_timer(&R);
}

void setUp(void)
{
    memset(&M, 0, sizeof M);
    epd_rails_init(&R, &IO, NULL);
}
void tearDown(void) {}

static void update(bool ok)
{
    bool kept;
    if (epd_rails_begin(&R, &kept))
        return;
    TEST_ASSERT_TRUE(M.up); /* an update never runs with the rails down */
    epd_rails_end(&R, ok);
}

static void test_no_hold_is_one_up_and_one_down_per_update(void)
{
    for (int i = 0; i < 3; i++) {
        update(true);
        TEST_ASSERT_FALSE(M.up);
    }
    TEST_ASSERT_EQUAL_INT(3, M.ups);
    TEST_ASSERT_EQUAL_INT(3, M.downs);
    TEST_ASSERT_EQUAL_INT(0, M.arms);
}

static void test_a_hold_keeps_them_up_and_the_timer_brings_them_down(void)
{
    bool kept = true;
    TEST_ASSERT_TRUE(epd_rails_set_hold(&R, 1500));
    TEST_ASSERT_EQUAL_INT(0, epd_rails_begin(&R, &kept));
    TEST_ASSERT_FALSE(kept);
    epd_rails_end(&R, true);
    TEST_ASSERT_TRUE(M.up);
    TEST_ASSERT_TRUE(M.timer);
    TEST_ASSERT_EQUAL_UINT32(1500, M.timer_ms);
    TEST_ASSERT_EQUAL_INT(0, epd_rails_begin(&R, &kept)); /* the next update inside the time: no power up */
    TEST_ASSERT_TRUE(kept);
    TEST_ASSERT_EQUAL_INT(1, M.ups);
    TEST_ASSERT_FALSE(M.timer); /* and no timer while it runs */
    epd_rails_end(&R, true);
    TEST_ASSERT_TRUE(M.up);
    fire();
    TEST_ASSERT_FALSE(M.up);
    TEST_ASSERT_EQUAL_INT(1, M.downs);
    TEST_ASSERT_EQUAL_UINT32(1, R.timer_downs);
    update(true); /* after the time: a power up again */
    TEST_ASSERT_EQUAL_INT(2, M.ups);
}

static void test_down_at_once_on_any_error(void)
{
    epd_rails_set_hold(&R, 2000);
    update(false); /* the update failed */
    TEST_ASSERT_FALSE(M.up);
    TEST_ASSERT_FALSE(M.timer);
    update(true);
    TEST_ASSERT_TRUE(M.up);
    update(false); /* a failure of an update that found them up */
    TEST_ASSERT_FALSE(M.up);
    TEST_ASSERT_FALSE(M.timer);
    M.fail_up = -3; /* the supply did not come up, or says so */
    bool kept;
    TEST_ASSERT_EQUAL_INT(-3, epd_rails_begin(&R, &kept));
    TEST_ASSERT_FALSE(M.up);
    TEST_ASSERT_FALSE(R.busy);
    M.no_timer = true; /* no timer: nothing would bring them down later */
    update(true);
    TEST_ASSERT_FALSE(M.up);
}

static void test_down_before_sleep_and_when_the_hold_is_taken_away(void)
{
    epd_rails_set_hold(&R, 2000);
    update(true);
    TEST_ASSERT_TRUE(M.up);
    epd_rails_off(&R);
    TEST_ASSERT_FALSE(M.up);
    TEST_ASSERT_FALSE(M.timer);
    update(true);
    TEST_ASSERT_TRUE(epd_rails_set_hold(&R, 0));
    TEST_ASSERT_FALSE(M.up);
    TEST_ASSERT_FALSE(M.timer);
    epd_rails_off(&R); /* down already: no second power down */
    TEST_ASSERT_EQUAL_INT(2, M.downs);
    TEST_ASSERT_FALSE(epd_rails_set_hold(&R, -1));
    TEST_ASSERT_FALSE(epd_rails_set_hold(&R, EPD_RAILS_HOLD_MAX_MS + 1));
    TEST_ASSERT_TRUE(epd_rails_set_hold(&R, EPD_RAILS_HOLD_MAX_MS));
    TEST_ASSERT_EQUAL_UINT16(EPD_RAILS_HOLD_MAX_MS, R.hold_ms);
}

static void test_the_timer_during_an_update_does_nothing(void)
{
    bool kept;
    epd_rails_set_hold(&R, 100);
    update(true);
    epd_rails_begin(&R, &kept);
    epd_rails_timer(&R); /* a callback that was already on its way */
    TEST_ASSERT_TRUE(M.up);
    epd_rails_off(&R); /* nor does a sleep request in the middle of an update */
    TEST_ASSERT_TRUE(M.up);
    epd_rails_end(&R, true);
    TEST_ASSERT_TRUE(M.up);
    TEST_ASSERT_TRUE(M.timer);
    fire();
    TEST_ASSERT_FALSE(M.up);
}

/* Random runs: after every event, rails up means a timer is armed (no update is running between events). */
static void test_never_up_without_a_way_down(void)
{
    uint32_t s = 7;
    for (int i = 0; i < 20000; i++) {
        s = s * 1664525u + 1013904223u;
        unsigned e = (s >> 10) % 12;
        M.no_timer = ((s >> 20) % 9) == 0;
        if (e < 4)
            update(true);
        else if (e == 4)
            update(false);
        else if (e == 5)
            M.fail_up = -1, update(true);
        else if (e == 6)
            fire();
        else if (e == 7)
            epd_rails_off(&R);
        else if (e == 8)
            epd_rails_set_hold(&R, 0);
        else if (e == 9)
            epd_rails_timer(&R); /* a stray callback */
        else
            epd_rails_set_hold(&R, 1 + (int)((s >> 4) % 3000));
        TEST_ASSERT_TRUE_MESSAGE(!M.up || M.timer, "rails up and nothing to bring them down");
        TEST_ASSERT_EQUAL_INT(M.up, R.up);
        if (!R.hold_ms)
            TEST_ASSERT_FALSE(M.up);
    }
    TEST_ASSERT_TRUE(R.kept > 100 && R.timer_downs > 100);
}

/* Every sleep entry of the firmware (deep and light sleep, hal_esp esp/sys.c) powers the rails down first. */
static void test_every_sleep_entry_powers_down_first(void)
{
    char path[512], line[512], before[4][512] = {{0}};
    snprintf(path, sizeof path, "%s", __FILE__);
    char *cut = strstr(path, "components/epd/test/");
    TEST_ASSERT_NOT_NULL(cut);
    snprintf(cut, sizeof path - (size_t)(cut - path), "components/hal_esp/esp/sys.c");
    FILE *f = fopen(path, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    int entries = 0;
    while (fgets(line, sizeof line, f)) {
        if (strstr(line, "esp_deep_sleep_start();") || strstr(line, "esp_light_sleep_start();")) {
            bool off = false;
            for (int i = 0; i < 4; i++)
                off |= strstr(before[i], "hal_esp_panel_rails_off();") != NULL;
            TEST_ASSERT_TRUE_MESSAGE(off, line);
            entries++;
        }
        memmove(before[0], before[1], sizeof before - sizeof before[0]);
        snprintf(before[3], sizeof before[3], "%s", line);
    }
    fclose(f);
    TEST_ASSERT_EQUAL_INT(3, entries);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_no_hold_is_one_up_and_one_down_per_update);
    RUN_TEST(test_a_hold_keeps_them_up_and_the_timer_brings_them_down);
    RUN_TEST(test_down_at_once_on_any_error);
    RUN_TEST(test_down_before_sleep_and_when_the_hold_is_taken_away);
    RUN_TEST(test_the_timer_during_an_update_does_nothing);
    RUN_TEST(test_never_up_without_a_way_down);
    RUN_TEST(test_every_sleep_entry_powers_down_first);
    return UNITY_END();
}
