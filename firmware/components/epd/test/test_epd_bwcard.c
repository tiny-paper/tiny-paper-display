/* BWRY test card (epd_bwcard.h). SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_bwcard.h"
#include "epd_bwry.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_swatches(void)
{
    TEST_ASSERT_EQUAL_UINT8(BWRY_WHITE, bwc_px("", 50, 40));
    TEST_ASSERT_EQUAL_UINT8(BWRY_BLACK, bwc_px("", 150, 40));
    TEST_ASSERT_EQUAL_UINT8(BWRY_RED, bwc_px("", 250, 40));
    TEST_ASSERT_EQUAL_UINT8(BWRY_YELLOW, bwc_px("", 350, 40));
    /* red/yellow mix: a checkerboard of both */
    int x = 3 * 400 / 6 + 10;
    TEST_ASSERT_NOT_EQUAL(bwc_px("", x, 100), bwc_px("", x + 1, 100));
    TEST_ASSERT_TRUE(bwc_px("", x, 100) == BWRY_RED || bwc_px("", x, 100) == BWRY_YELLOW);
}

static void test_gradients_run_from_one_color_to_the_other(void)
{
    int dark = 0, light = 0;
    for (int y = 176; y < 180; y++)
        for (int x = 0; x < 4; x++) {
            dark += bwc_px("", x, y) == BWRY_BLACK;
            light += bwc_px("", 396 + x, y) == BWRY_WHITE;
        }
    TEST_ASSERT_EQUAL_INT(16, dark);
    TEST_ASSERT_TRUE(light >= 15);
    TEST_ASSERT_EQUAL_UINT8(BWRY_YELLOW, bwc_px("", 399, 205));
    TEST_ASSERT_EQUAL_UINT8(BWRY_WHITE, bwc_px("", 0, 232));
}

static void test_text_and_row_packing(void)
{
    /* "1" at 2x at (BWC_TEXT_X, 284): glyph row 0 = 010, so the second column (2 px) is ink, the first is not */
    TEST_ASSERT_EQUAL_UINT8(BWRY_BLACK, bwc_px("1", BWC_TEXT_X + 2, 284));
    TEST_ASSERT_EQUAL_UINT8(BWRY_WHITE, bwc_px("1", BWC_TEXT_X, 284));
    TEST_ASSERT_EQUAL_UINT8(BWRY_WHITE, bwc_px("a", BWC_TEXT_X + 2, 284)); /* not in the font: blank */
    uint8_t row[BWC_W / 4];
    bwc_row("", 40, row);
    TEST_ASSERT_EQUAL_HEX8(0x55, row[0]);  /* white */
    TEST_ASSERT_EQUAL_HEX8(0x00, row[30]); /* black */
    TEST_ASSERT_EQUAL_HEX8(0xff, row[60]); /* red */
    TEST_ASSERT_EQUAL_HEX8(0xaa, row[90]); /* yellow */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_swatches);
    RUN_TEST(test_gradients_run_from_one_color_to_the_other);
    RUN_TEST(test_text_and_row_packing);
    return UNITY_END();
}
