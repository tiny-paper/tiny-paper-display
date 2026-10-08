/* The gray partial's codes from tables (epd_gpartlut.h) are those of the per-pixel epd_gpart_row (epd_parwave.h):
 * every kind of step, every pass of both gray tables and of random matrices, random rows, byte for byte, with the
 * changed mask and the driven count. SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "epd_gpartlut.h"
#include "epd_graytab.h"
#include "epd_parwave.h"
#include "unity.h"

#define W 1872
static uint8_t old4[W / 2], new4[W / 2], P[W / 8], C[W / 8], CH[W / 8], P2[W / 8], C2[W / 8], CH2[W / 8];
static uint32_t seed = 3;

void setUp(void) {}
void tearDown(void) {}

static uint32_t rnd(void)
{
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
}

static void random_rows(int w, unsigned same_of_8)
{
    for (int i = 0; i < w / 2; i++) {
        old4[i] = (uint8_t)rnd();
        uint8_t n = (uint8_t)rnd();
        if (rnd() % 8 < same_of_8)
            n = (uint8_t)((n & 0x0f) | (old4[i] & 0xf0)); /* the first pixel of the byte unchanged */
        if (rnd() % 8 < same_of_8)
            n = (uint8_t)((n & 0xf0) | (old4[i] & 0x0f));
        new4[i] = n;
    }
}

static void same_as_per_pixel(int kind, int j, const uint8_t *m, int passes, int w)
{
    epd_gpart_lut_t lut;
    memset(P, 0x11, sizeof P), memset(P2, 0x22, sizeof P2), memset(C, 0x33, sizeof C), memset(C2, 0x44, sizeof C2);
    epd_gpart_row(kind, j, m, passes, old4, new4, w, P, C, CH);
    uint32_t changed = epd_gpart_changed(old4, new4, w, CH2);
    epd_gpart_lut(kind, j, m, passes, &lut);
    uint32_t driven = epd_gpart_row_lut(&lut, new4, CH2, w, P2, C2);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(CH, CH2, w / 8);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(P, P2, w / 8);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(C, C2, w / 8);
    TEST_ASSERT_EQUAL_UINT32(epd_gpart_driven(P, C, w), driven);
    uint32_t n = 0;
    for (int i = 0; i < w / 8; i++)
        n += (uint32_t)__builtin_popcount(CH[i]);
    TEST_ASSERT_EQUAL_UINT32(n, changed);
    if (w < W) { /* nothing written past the row */
        TEST_ASSERT_EQUAL_HEX8(0x22, P2[w / 8]);
        TEST_ASSERT_EQUAL_HEX8(0x44, C2[w / 8]);
    }
}

static void test_both_gray_tables_every_pass(void)
{
    for (int table = EPD_GRAYTAB_FASTEPD; table <= EPD_GRAYTAB_STOCK; table++) {
        epd_graytab_t t;
        epd_graytab_select(&t, table);
        TEST_ASSERT_TRUE(t.passes > 0);
        for (int j = 0; j < t.passes; j++)
            for (unsigned same = 0; same <= 8; same += 4) {
                random_rows(W, same);
                same_as_per_pixel(EPD_GP_PASS, j, t.m, t.passes, W);
            }
        random_rows(W, 3);
        same_as_per_pixel(EPD_GP_DARK, 0, t.m, t.passes, W);
        same_as_per_pixel(EPD_GP_LIGHT, 0, t.m, t.passes, W);
    }
}

static void test_random_matrices_and_widths(void)
{
    uint8_t m[16 * 16];
    for (int run = 0; run < 400; run++) {
        int passes = 1 + (int)(rnd() % 16), w = 8 * (1 + (int)(rnd() % 40));
        for (int i = 0; i < 16 * passes; i++)
            m[i] = (uint8_t)(rnd() % 4); /* 0 and 3: no drive */
        random_rows(w, rnd() % 9);
        int kind = (int)(rnd() % 3);
        same_as_per_pixel(kind, (int)(rnd() % (unsigned)passes), m, passes, w);
    }
}

static void test_an_unchanged_row_gets_no_code(void)
{
    epd_gpart_lut_t lut;
    for (int i = 0; i < W / 2; i++)
        old4[i] = new4[i] = (uint8_t)rnd();
    TEST_ASSERT_EQUAL_UINT32(0, epd_gpart_changed(old4, new4, W, CH2));
    epd_gpart_lut(EPD_GP_DARK, 0, NULL, 1, &lut);
    TEST_ASSERT_EQUAL_UINT32(0, epd_gpart_row_lut(&lut, new4, CH2, W, P2, C2));
    for (int i = 0; i < W / 8; i++)
        TEST_ASSERT_EQUAL_HEX8(0xff, P2[i] & C2[i]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_both_gray_tables_every_pass);
    RUN_TEST(test_random_matrices_and_widths);
    RUN_TEST(test_an_unchanged_row_gets_no_code);
    return UNITY_END();
}
