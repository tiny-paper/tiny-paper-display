/* Every press of the X bar, in order (src/btn_queue.h). SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/btn_queue.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define MS 1000ull
enum { LEFT = 3, MIDDLE = 1, RIGHT = 4 };

static bq_t q;

/* Hardware 2026-09-28 (fw 2031): tap left, tap middle (the app watched it: lost then), tap right 2x, touch left 1 s,
 * hold middle 3.4 s, hold right 2.4 s, hold left 2.2 s, tap right. Each press comes out once, in order, with its own
 * start and release: two right taps 0.4 s apart are two presses, and holds with releases between never merge. */
static void test_hardware_sequence(void)
{
    static const struct {
        uint64_t down_ms;
        uint32_t ms;
        uint8_t in;
    } s[] = {{0, 270, LEFT},        {500, 490, MIDDLE},    {6000, 185, RIGHT},    {6600, 548, RIGHT},
             {7500, 1002, LEFT},    {8800, 3390, MIDDLE},  {12500, 2414, RIGHT},  {15200, 2239, LEFT},
             {17700, 184, RIGHT}};
    const int n = (int)(sizeof s / sizeof s[0]);
    bq_press_t p;
    bq_reset(&q);
    TEST_ASSERT_FALSE(bq_take(&q, &p));
    for (int i = 0; i < n; i++)
        TEST_ASSERT_TRUE(bq_push(&q, s[i].down_ms * MS, (s[i].down_ms + s[i].ms) * MS, s[i].in, 0, false));
    for (int i = 0; i < n; i++) {
        TEST_ASSERT_TRUE(bq_take(&q, &p));
        TEST_ASSERT_EQUAL_UINT64(s[i].down_ms * MS, p.down);
        TEST_ASSERT_EQUAL_UINT64((s[i].down_ms + s[i].ms) * MS, p.up);
        TEST_ASSERT_EQUAL_UINT8(s[i].in, p.input);
    }
    TEST_ASSERT_FALSE(bq_take(&q, &p));
}

/* Taps 0.45 s apart stay apart (the app counts them), swipes keep their direction, a brush its flag, a zero length
 * is 1 us */
static void test_no_merge(void)
{
    bq_press_t p;
    bq_reset(&q);
    bq_push(&q, 1000 * MS, 1150 * MS, MIDDLE, 0, false);
    bq_push(&q, 1600 * MS, 1750 * MS, MIDDLE, 0, true);
    bq_push(&q, 4000 * MS, 4400 * MS, MIDDLE, -1, false);
    bq_push(&q, 5000 * MS, 5000 * MS, MIDDLE, 1, false);
    TEST_ASSERT_TRUE(bq_take(&q, &p));
    TEST_ASSERT_EQUAL_UINT64(1150 * MS, p.up);
    TEST_ASSERT_FALSE(p.brush);
    TEST_ASSERT_TRUE(bq_take(&q, &p));
    TEST_ASSERT_EQUAL_UINT64(1600 * MS, p.down);
    TEST_ASSERT_TRUE(p.brush);
    TEST_ASSERT_TRUE(bq_take(&q, &p));
    TEST_ASSERT_EQUAL_INT8(-1, p.swipe);
    TEST_ASSERT_FALSE(p.brush);
    TEST_ASSERT_TRUE(bq_take(&q, &p));
    TEST_ASSERT_EQUAL_INT8(1, p.swipe);
    TEST_ASSERT_EQUAL_UINT64(5000 * MS + 1, p.up);
    TEST_ASSERT_FALSE(bq_take(&q, &p));
}

/* Full: the older presses stay, the newest is dropped and counted; the ring wraps */
static void test_full_and_wrap(void)
{
    bq_press_t p;
    bq_reset(&q);
    for (int i = 0; i < BQ_LEN + 2; i++)
        bq_push(&q, (uint64_t)i * 10000 * MS, (uint64_t)i * 10000 * MS + 150 * MS, MIDDLE, 0, false);
    TEST_ASSERT_EQUAL_UINT32(2, q.dropped);
    TEST_ASSERT_TRUE(bq_take(&q, &p));
    TEST_ASSERT_EQUAL_UINT64(0, p.down);
    TEST_ASSERT_TRUE(bq_push(&q, 900000 * MS, 900100 * MS, MIDDLE, 1, false));
    for (int i = 1; i < BQ_LEN; i++) {
        TEST_ASSERT_TRUE(bq_take(&q, &p));
        TEST_ASSERT_EQUAL_UINT64((uint64_t)i * 10000 * MS, p.down);
    }
    TEST_ASSERT_TRUE(bq_take(&q, &p));
    TEST_ASSERT_EQUAL_INT8(1, p.swipe);
    TEST_ASSERT_FALSE(bq_take(&q, &p));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hardware_sequence);
    RUN_TEST(test_no_merge);
    RUN_TEST(test_full_and_wrap);
    return UNITY_END();
}
