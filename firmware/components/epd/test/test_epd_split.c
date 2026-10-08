/* The time split of a panel update (epd_split.h): the parts add up, a library call is split at its first scan, the
 * line names every part in milliseconds and what the parts do not cover. SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "epd_split.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_the_line_names_every_part(void)
{
    epd_split_t s;
    char b[200];
    epd_split_reset(&s);
    TEST_ASSERT_EQUAL_STRING("total=0 begin=0 write=0 up=0 prep=0 masks=0 lib=0 scans=0 down=0 after=0 save=0 log=0 other=0 calls=0",
                             epd_split_str(&s, 0, b, sizeof b));
    for (int i = 0; i < EPD_SPLIT_PARTS; i++)
        epd_split_add(&s, i, (uint32_t)(i + 1) * 1000u);
    epd_split_add(&s, EPD_SPLIT_UP, 40000);
    /* the parts sum to 106 ms; of 110 ms in all 4 are not covered */
    TEST_ASSERT_EQUAL_STRING("total=110 begin=1 write=2 up=43 prep=4 masks=5 lib=6 scans=7 down=8 after=9 save=10 log=11 other=4 calls=0",
                             epd_split_str(&s, 110000, b, sizeof b));
    /* a total below the parts (clocks read in another order) is no negative number */
    TEST_ASSERT_NOT_NULL(strstr(epd_split_str(&s, 1000, b, sizeof b), " other=0 "));
}

static void test_milliseconds_are_rounded(void)
{
    epd_split_t s;
    char b[200];
    epd_split_reset(&s);
    epd_split_add(&s, EPD_SPLIT_PREP, 499);
    epd_split_add(&s, EPD_SPLIT_DOWN, 500);
    epd_split_add(&s, EPD_SPLIT_AFTER, 1499);
    epd_split_str(&s, 2498, b, sizeof b);
    TEST_ASSERT_NOT_NULL(strstr(b, " prep=0 "));
    TEST_ASSERT_NOT_NULL(strstr(b, " down=1 "));
    TEST_ASSERT_NOT_NULL(strstr(b, " after=1 "));
    TEST_ASSERT_NOT_NULL(strstr(b, "total=2 "));
}

static void test_a_call_is_split_at_its_first_scan(void)
{
    epd_split_t s;
    epd_split_reset(&s);
    epd_split_call(&s, 1000, 4000, 124000);
    TEST_ASSERT_EQUAL_UINT32(3000, s.us[EPD_SPLIT_LIB]);
    TEST_ASSERT_EQUAL_UINT32(120000, s.us[EPD_SPLIT_SCANS]);
    /* a call that never scanned (the library refused): all of it is the library's */
    epd_split_call(&s, 200000, 0, 200700);
    TEST_ASSERT_EQUAL_UINT32(3700, s.us[EPD_SPLIT_LIB]);
    TEST_ASSERT_EQUAL_UINT32(120000, s.us[EPD_SPLIT_SCANS]);
    /* the clock wraps inside a call */
    epd_split_call(&s, 0xfffffc00u, 0xffffff00u, 0x00000400u);
    TEST_ASSERT_EQUAL_UINT32(3700 + 0x300, s.us[EPD_SPLIT_LIB]);
    TEST_ASSERT_EQUAL_UINT32(120000 + 0x500, s.us[EPD_SPLIT_SCANS]);
    /* a first scan stamp that is not inside the call (left from another one) is not believed */
    epd_split_call(&s, 5000, 4000, 6000);
    TEST_ASSERT_EQUAL_UINT32(3700 + 0x300 + 1000, s.us[EPD_SPLIT_LIB]);
    TEST_ASSERT_EQUAL_UINT16(4, s.calls);
}

static void test_bad_parts_and_sums_that_wrap(void)
{
    epd_split_t s, t;
    epd_split_reset(&s);
    epd_split_add(&s, -1, 5);
    epd_split_add(&s, EPD_SPLIT_PARTS, 5);
    for (int i = 0; i < EPD_SPLIT_PARTS; i++)
        TEST_ASSERT_EQUAL_UINT32(0, s.us[i]);
    epd_split_add(&s, EPD_SPLIT_SCANS, 0xfffffff0u);
    epd_split_add(&s, EPD_SPLIT_SCANS, 0x20);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, s.us[EPD_SPLIT_SCANS]);
    epd_split_reset(&t);
    epd_split_add(&t, EPD_SPLIT_BEGIN, 7);
    epd_split_add(&t, EPD_SPLIT_SAVE, 9);
    t.calls = 3;
    s.calls = 2;
    epd_split_merge(&s, &t);
    TEST_ASSERT_EQUAL_UINT32(7, s.us[EPD_SPLIT_BEGIN]);
    TEST_ASSERT_EQUAL_UINT32(9, s.us[EPD_SPLIT_SAVE]);
    TEST_ASSERT_EQUAL_UINT16(5, s.calls);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_line_names_every_part);
    RUN_TEST(test_milliseconds_are_rounded);
    RUN_TEST(test_a_call_is_split_at_its_first_scan);
    RUN_TEST(test_bad_parts_and_sums_that_wrap);
    return UNITY_END();
}
