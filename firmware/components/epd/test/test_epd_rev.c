/* Panel ID parse (epd_rev.h). SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_rev.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_parse_takes_last_four_bytes(void)
{
    const uint8_t raw[7] = {0xff, 0xff, 0xff, 0x00, 0x12, 0xab, 0x34};
    TEST_ASSERT_EQUAL_HEX32(0x0012ab34, epd_rev_parse(raw));
    char s[9];
    epd_rev_str(0x0012ab34, s);
    TEST_ASSERT_EQUAL_STRING("0012ab34", s);
}

static void test_parse_unknown(void)
{
    const uint8_t none[7] = {0xff, 0xff, 0xff, 0, 0, 0, 0};
    const uint8_t floating[7] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const uint8_t filler[7] = {0, 0, 0, 0, 1, 2, 3};
    TEST_ASSERT_EQUAL_HEX32(0, epd_rev_parse(none));
    TEST_ASSERT_EQUAL_HEX32(0, epd_rev_parse(floating));
    TEST_ASSERT_EQUAL_HEX32(0, epd_rev_parse(filler));
    char s[9] = "x";
    epd_rev_str(0, s);
    TEST_ASSERT_EQUAL_STRING("", s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_takes_last_four_bytes);
    RUN_TEST(test_parse_unknown);
    return UNITY_END();
}
