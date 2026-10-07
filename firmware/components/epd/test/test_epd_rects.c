/* Windows the UC8179 driver refreshes (epd_rects.h). SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_rects.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static epd_rects_t a;
static tiny_rect_t w[EPD_MAX_RECTS];

static void add(int x, int y, int wd, int h)
{
    tiny_rect_t r = {(int16_t)x, (int16_t)y, (uint16_t)wd, (uint16_t)h};
    epd_rects_add(&a, &r);
}

static void assert_rect(int x, int y, int wd, int h, tiny_rect_t r)
{
    TEST_ASSERT_EQUAL_INT(x, r.x);
    TEST_ASSERT_EQUAL_INT(y, r.y);
    TEST_ASSERT_EQUAL_INT(wd, r.w);
    TEST_ASSERT_EQUAL_INT(h, r.h);
}

/* The app writes the union's rows band by band (80 rows on the OG): one window, no steps. */
static void test_stacked_bands_are_one_window(void)
{
    epd_rects_reset(&a);
    add(304, 150, 192, 10);
    add(304, 160, 192, 80);
    add(304, 240, 192, 30);
    TEST_ASSERT_EQUAL(1, a.n);
    TEST_ASSERT_EQUAL(3, a.writes);
    TEST_ASSERT_EQUAL(1, epd_rects_windows(&a, false, w, EPD_MAX_RECTS));
    assert_rect(304, 150, 192, 120, w[0]);
}

/* Real OG, 2026-09-24: separate areas after the controller's deep sleep. Their union holds rows
 * nobody wrote (garbage RAM, a noise band), so each area is refreshed in its own window. */
static void test_separate_areas_without_valid_ram(void)
{
    epd_rects_reset(&a);
    add(96, 40, 64, 80);
    add(400, 300, 128, 40);
    TEST_ASSERT_EQUAL(2, epd_rects_windows(&a, false, w, EPD_MAX_RECTS));
    assert_rect(96, 40, 64, 80, w[0]);
    assert_rect(400, 300, 128, 40, w[1]);
    /* RAM valid: the rest of the union is the shown image, one window is fine */
    TEST_ASSERT_EQUAL(1, epd_rects_windows(&a, true, w, EPD_MAX_RECTS));
    assert_rect(96, 40, 432, 300, w[0]);
}

/* Same rows but another x range, or a gap below: not merged. */
static void test_merge_needs_same_x_and_no_gap(void)
{
    epd_rects_reset(&a);
    add(0, 0, 64, 80);
    add(8, 80, 64, 80);   /* other x */
    add(8, 168, 64, 8);   /* 8-row gap */
    TEST_ASSERT_EQUAL(3, a.n);
    TEST_ASSERT_EQUAL(3, epd_rects_windows(&a, false, w, EPD_MAX_RECTS));
}

/* More areas than slots: the last one grows to cover the rest, every written pixel still refreshed. */
static void test_overflow_grows_the_last(void)
{
    epd_rects_reset(&a);
    for (int i = 0; i < EPD_MAX_RECTS + 3; i++)
        add(i * 16, i * 20, 8, 10);
    TEST_ASSERT_EQUAL(EPD_MAX_RECTS, a.n);
    int n = epd_rects_windows(&a, false, w, EPD_MAX_RECTS);
    TEST_ASSERT_EQUAL(EPD_MAX_RECTS, n);
    tiny_rect_t l = w[n - 1];
    int last = EPD_MAX_RECTS + 2;
    assert_rect((EPD_MAX_RECTS - 1) * 16, (EPD_MAX_RECTS - 1) * 20, last * 16 + 8 - (EPD_MAX_RECTS - 1) * 16,
                last * 20 + 10 - (EPD_MAX_RECTS - 1) * 20, l);
}

static void test_nothing_written(void)
{
    epd_rects_reset(&a);
    TEST_ASSERT_EQUAL(0, epd_rects_windows(&a, false, w, EPD_MAX_RECTS));
    add(0, 0, 800, 480);
    TEST_ASSERT_EQUAL(1, epd_rects_windows(&a, false, w, EPD_MAX_RECTS));
    assert_rect(0, 0, 800, 480, w[0]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_stacked_bands_are_one_window);
    RUN_TEST(test_separate_areas_without_valid_ram);
    RUN_TEST(test_merge_needs_same_x_and_no_gap);
    RUN_TEST(test_overflow_grows_the_last);
    RUN_TEST(test_nothing_written);
    return UNITY_END();
}
