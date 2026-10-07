/* Flash/PSRAM pin guard (epd_pins.h). SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_pins.h"
#include "unity.h"

#include <stddef.h>

void setUp(void) {}
void tearDown(void) {}

/* TRMNL X bus, control and I2C pins (boards/trmnl_x/board.h, FastEPD BB_PANEL_TRMNL_X): all clear. */
static void test_trmnl_x_pins_are_clear(void)
{
    const int p[] = { 8, 18, 17, 16, 15, 7, 6, 5, 47, 21, 14, 13, 12, 11, 10, 9, 4, 41, 45, 48, 42, 46, 39, 40 };
    TEST_ASSERT_EQUAL_INT(-1, epd_pins_first_reserved(p, (int)(sizeof p / sizeof p[0]), true));
}

/* FastEPD's epdInky P4 table uses 26, 35 and 37: fatal on an octal S3. */
static void test_octal_range(void)
{
    const int p[] = { 25, 38, -1, 37, 26 };
    TEST_ASSERT_EQUAL_INT(3, epd_pins_first_reserved(p, 5, true));
    TEST_ASSERT_EQUAL_INT(4, epd_pins_first_reserved(p, 5, false));
    const int q[] = { 33, 36 };
    TEST_ASSERT_EQUAL_INT(0, epd_pins_first_reserved(q, 2, true));
    TEST_ASSERT_EQUAL_INT(-1, epd_pins_first_reserved(q, 2, false));
}

static void test_edges(void)
{
    TEST_ASSERT_FALSE(EPD_S3_PIN_RESERVED(25, true));
    TEST_ASSERT_TRUE(EPD_S3_PIN_RESERVED(26, false));
    TEST_ASSERT_TRUE(EPD_S3_PIN_RESERVED(32, false));
    TEST_ASSERT_FALSE(EPD_S3_PIN_RESERVED(33, false));
    TEST_ASSERT_TRUE(EPD_S3_PIN_RESERVED(37, true));
    TEST_ASSERT_FALSE(EPD_S3_PIN_RESERVED(38, true));
    TEST_ASSERT_FALSE(EPD_S3_PIN_RESERVED(-1, true));
    TEST_ASSERT_EQUAL_INT(-1, epd_pins_first_reserved(NULL, 0, true));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_trmnl_x_pins_are_clear);
    RUN_TEST(test_octal_range);
    RUN_TEST(test_edges);
    return UNITY_END();
}
