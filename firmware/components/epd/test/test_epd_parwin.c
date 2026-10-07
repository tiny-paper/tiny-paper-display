/* 1 bpp partial window math for the FastEPD glue (epd_parwin.h). SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdbool.h>
#include <string.h>

#include "epd_parwin.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define H 1404
#define NEUTRAL (-1)

/* Model of FastEPD 8dc8c74 bbepPartialUpdate's row loop (1 bpp): which pTemp row each panel row is driven
 * with in each pass. Two DMA halves used in turn; only the first row of a skipped run zeroes its half. */
static int driven[3][H];

static void fastepd_passes(int start, int end, int passes)
{
    int buf[2] = {1234567, 1234567}; /* whatever the halves held before (not neutral) */
    for (int pass = 0; pass < passes; pass++) {
        int off = 0, skipped = 0;
        for (int i = 0; i < H; i++) {
            if (i >= start && i <= end) {
                buf[off] = i;
                skipped = 0;
            } else {
                if (skipped == 0)
                    buf[off] = NEUTRAL;
                skipped++;
            }
            driven[pass][i] = buf[off];
            off ^= 1;
        }
    }
}

/* The hardware bug: the window 769..1232 as is drives rows outside it with window rows. */
static void test_window_range_leaks_rows(void)
{
    fastepd_passes(769, 1232, 3);
    int leaks_below = 0, leaks_above = 0;
    for (int p = 0; p < 3; p++)
        for (int i = 0; i < H; i++)
            if ((i < 769 || i > 1232) && driven[p][i] != NEUTRAL) {
                leaks_below += i > 1232;
                leaks_above += i < 769;
            }
    TEST_ASSERT_GREATER_THAN(0, leaks_below);
    TEST_ASSERT_GREATER_THAN(0, leaks_above);
    TEST_ASSERT_EQUAL_INT(1232, driven[0][1234]); /* every other row below repeats the last window row */
    TEST_ASSERT_EQUAL_INT(NEUTRAL, driven[0][1233]);
}

/* The glue's range: every row driven with its own data (neutral where old == new), in every pass. */
static void test_full_range_drives_each_row_with_its_own(void)
{
    int a, b;
    epd_par_rows(H, 769, 1232, &a, &b);
    TEST_ASSERT_EQUAL_INT(0, a);
    TEST_ASSERT_EQUAL_INT(H - 1, b);
    fastepd_passes(a, b, 3);
    for (int p = 0; p < 3; p++)
        for (int i = 0; i < H; i++)
            TEST_ASSERT_EQUAL_INT(i, driven[p][i]);
}

#define W 64
#define DS (W / 8)

static void test_copy_places_rows_and_bytes(void)
{
    uint8_t cur[DS * 8], old[DS * 8], next[3 * 4], prev[3 * 4];
    memset(cur, 0xff, sizeof cur);
    memset(old, 0xff, sizeof old);
    for (int i = 0; i < 12; i++) {
        next[i] = (uint8_t)(0x10 + i);
        prev[i] = (uint8_t)(0x80 + i);
    }
    epd_win_t r = {16, 2, 16, 3}; /* bytes 2..3 of rows 2..4; source stride 4 (2 bytes used) */
    TEST_ASSERT_EQUAL_INT(0, epd_par_copy_1bpp(cur, old, DS, W, 8, &r, next, prev, 4, false));
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < DS; x++) {
            bool in = y >= 2 && y <= 4 && x >= 2 && x <= 3;
            int k = (y - 2) * 4 + (x - 2);
            TEST_ASSERT_EQUAL_HEX8(in ? 0x10 + k : 0xff, cur[y * DS + x]);
            TEST_ASSERT_EQUAL_HEX8(in ? 0x80 + k : 0xff, old[y * DS + x]);
        }
}

/* all ("!tp win all", issue #203): the previous frame is the new one inverted inside the window, so every pixel
 * of it differs and is driven; outside the window nothing changes. */
static void test_copy_all_inverts_previous_inside_the_window(void)
{
    uint8_t cur[DS * 8], old[DS * 8], next[3 * 4], prev[3 * 4];
    memset(cur, 0xff, sizeof cur);
    memset(old, 0xff, sizeof old);
    for (int i = 0; i < 12; i++) {
        next[i] = (uint8_t)(0x10 + i);
        prev[i] = (uint8_t)(0x10 + i); /* same as next: without all, nothing would be driven */
    }
    epd_win_t r = {16, 2, 16, 3};
    TEST_ASSERT_EQUAL_INT(0, epd_par_copy_1bpp(cur, old, DS, W, 8, &r, next, prev, 4, true));
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < DS; x++) {
            bool in = y >= 2 && y <= 4 && x >= 2 && x <= 3;
            int k = (y - 2) * 4 + (x - 2);
            TEST_ASSERT_EQUAL_HEX8(in ? 0x10 + k : 0xff, cur[y * DS + x]);
            TEST_ASSERT_EQUAL_HEX8(in ? (uint8_t)~(0x10 + k) : 0xff, old[y * DS + x]);
        }
}

static void test_copy_rejects_bad_rects(void)
{
    uint8_t cur[DS * 8], old[DS * 8], src[DS * 8] = {0};
    memset(cur, 0xff, sizeof cur);
    const epd_win_t bad[] = {{4, 0, 8, 1}, {0, 0, 12, 1}, {56, 0, 16, 1}, {0, 7, 8, 2}, {-8, 0, 8, 1}};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        TEST_ASSERT_EQUAL_INT(-1, epd_par_copy_1bpp(cur, old, DS, W, 8, &bad[i], src, src, DS, false));
    for (size_t i = 0; i < sizeof cur; i++)
        TEST_ASSERT_EQUAL_HEX8(0xff, cur[i]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_window_range_leaks_rows);
    RUN_TEST(test_full_range_drives_each_row_with_its_own);
    RUN_TEST(test_copy_places_rows_and_bytes);
    RUN_TEST(test_copy_all_inverts_previous_inside_the_window);
    RUN_TEST(test_copy_rejects_bad_rects);
    return UNITY_END();
}
