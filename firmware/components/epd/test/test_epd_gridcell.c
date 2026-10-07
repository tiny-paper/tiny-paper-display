/* Labelled grid cell for position tests of partial windows (epd_gridcell.h). SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "epd_gridcell.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define W 232
#define H 234
#define S (W / 8)

static uint8_t cell[S * H];

static int count_black(const uint8_t *b)
{
    int n = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            n += epd_grid_black(b, S, x, y);
    return n;
}

static void test_frame_black_interior_white(void)
{
    TEST_ASSERT_EQUAL_INT(0, epd_grid_cell(cell, S, W, H, 0, 0));
    TEST_ASSERT_TRUE(epd_grid_black(cell, S, 0, 0));
    TEST_ASSERT_TRUE(epd_grid_black(cell, S, W - 1, H - 1));
    TEST_ASSERT_TRUE(epd_grid_black(cell, S, W / 2, EPD_GRID_FRAME - 1));
    TEST_ASSERT_FALSE(epd_grid_black(cell, S, W / 2, EPD_GRID_FRAME));
    TEST_ASSERT_FALSE(epd_grid_black(cell, S, EPD_GRID_FRAME, H / 2)); /* left margin, before the digits */
    TEST_ASSERT_FALSE(epd_grid_black(cell, S, W - EPD_GRID_FRAME - 1, H / 2));
}

static void test_digits_are_where_the_label_says(void)
{
    TEST_ASSERT_EQUAL_INT(0, epd_grid_cell(cell, S, W, H, 1, 7));
    const int gw = 3 * EPD_GRID_SCALE, gh = 5 * EPD_GRID_SCALE, gap = 2 * EPD_GRID_SCALE;
    const int x0 = (W - (2 * gw + gap)) / 2, y0 = (H - gh) / 2;
    /* "1": the middle column is black, the top-left pixel of the glyph white */
    TEST_ASSERT_TRUE(epd_grid_black(cell, S, x0 + EPD_GRID_SCALE + 1, y0 + gh / 2));
    TEST_ASSERT_FALSE(epd_grid_black(cell, S, x0 + 1, y0 + 1));
    /* "7": the top row is black, the bottom-left white */
    const int x1 = x0 + gw + gap;
    TEST_ASSERT_TRUE(epd_grid_black(cell, S, x1 + 1, y0 + 1));
    TEST_ASSERT_FALSE(epd_grid_black(cell, S, x1 + 1, y0 + gh - 1));
    /* the gap between the digits is white */
    TEST_ASSERT_FALSE(epd_grid_black(cell, S, x0 + gw + gap / 2, y0 + gh / 2));
}

static void test_cells_differ_and_a_mirror_is_not_a_cell(void)
{
    static uint8_t a[S * H], m[S * H];
    TEST_ASSERT_EQUAL_INT(0, epd_grid_cell(a, S, W, H, 2, 3));
    TEST_ASSERT_EQUAL_INT(0, epd_grid_cell(cell, S, W, H, 3, 2));
    TEST_ASSERT_TRUE(memcmp(a, cell, sizeof a) != 0);
    /* a horizontal mirror of cell (2, 3) matches no cell (c, r): a mirrored panel is told apart on the photo */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (epd_grid_black(a, S, W - 1 - x, y))
                m[(size_t)y * S + (size_t)(x >> 3)] &= (uint8_t)~(0x80u >> (x & 7));
            else
                m[(size_t)y * S + (size_t)(x >> 3)] |= (uint8_t)(0x80u >> (x & 7));
    for (int c = 0; c < 10; c++)
        for (int r = 0; r < 10; r++) {
            TEST_ASSERT_EQUAL_INT(0, epd_grid_cell(cell, S, W, H, c, r));
            TEST_ASSERT_TRUE(memcmp(m, cell, sizeof m) != 0);
        }
    (void)count_black;
}

static void test_rejects_small_or_odd_cells(void)
{
    memset(cell, 0x55, sizeof cell);
    TEST_ASSERT_EQUAL_INT(-1, epd_grid_cell(cell, S, W, EPD_GRID_MIN_H - 1, 0, 0));
    TEST_ASSERT_EQUAL_INT(-1, epd_grid_cell(cell, S, EPD_GRID_MIN_W - 8, H, 0, 0));
    TEST_ASSERT_EQUAL_INT(-1, epd_grid_cell(cell, S, W + 4, H, 0, 0));
    TEST_ASSERT_EQUAL_INT(-1, epd_grid_cell(cell, S - 1, W, H, 0, 0));
    TEST_ASSERT_EQUAL_INT(-1, epd_grid_cell(cell, S, W, H, 10, 0));
    TEST_ASSERT_EQUAL_INT(-1, epd_grid_cell(cell, S, W, H, 0, -1));
    TEST_ASSERT_EQUAL_INT(-1, epd_grid_cell(NULL, S, W, H, 0, 0));
    TEST_ASSERT_EQUAL_HEX8(0x55, cell[0]); /* nothing drawn */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_frame_black_interior_white);
    RUN_TEST(test_digits_are_where_the_label_says);
    RUN_TEST(test_cells_differ_and_a_mirror_is_not_a_cell);
    RUN_TEST(test_rejects_small_or_odd_cells);
    return UNITY_END();
}
