/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "epd_lut.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static uint8_t L[EPD_LUT_N][EPD_LUT_LEN];

static void test_bb_reps1_matches_source(void)
{
    /* bb_epaper epd75_init_sequence_partial, first group of each LUT */
    static const uint8_t first[EPD_LUT_N] = {0x00, 0x00, 0x80, 0x04, 0x00, 0x00};
    TEST_ASSERT_EQUAL_UINT(1, epd_lut_build(EPD_LUTSET_BB, 1, L));
    for (int i = 0; i < EPD_LUT_N; i++) {
        uint8_t want[EPD_LUT_LEN] = {first[i], 30, 1, 30, 1, 1};
        TEST_ASSERT_EQUAL_HEX8_ARRAY(want, L[i], EPD_LUT_LEN);
    }
}

static void test_gx_reps1_matches_source(void)
{
    /* GxEPD2_750_GDEY075T7 lut_20..lut_25 _partial, zero padded to 42 */
    static const uint8_t first[EPD_LUT_N] = {0x00, 0x00, 0x5A, 0x84, 0x00, 0x00};
    TEST_ASSERT_EQUAL_UINT(1, epd_lut_build(EPD_LUTSET_GX, 1, L));
    for (int i = 0; i < EPD_LUT_N; i++) {
        uint8_t want[EPD_LUT_LEN] = {first[i], 30, 5, 30, 5, 1};
        TEST_ASSERT_EQUAL_HEX8_ARRAY(want, L[i], EPD_LUT_LEN);
    }
    TEST_ASSERT_EQUAL_INT(0x30, epd_lut_vcom(EPD_LUTSET_GX, EPD_VCOM_AUTO));
    TEST_ASSERT_EQUAL_INT(-1, epd_lut_vcom(EPD_LUTSET_BB, EPD_VCOM_AUTO));
}

static void test_reps_scale_frames_and_keep_balance(void)
{
    for (unsigned r = 1; r <= EPD_LUT_REPS_MAX; r++) {
        TEST_ASSERT_EQUAL_UINT(r, epd_lut_build(EPD_LUTSET_GX, r, L));
        TEST_ASSERT_EQUAL_UINT8(30 * r, L[3][1]);
        TEST_ASSERT_EQUAL_UINT8(5 * r, L[3][2]);
        TEST_ASSERT_EQUAL_UINT8(1, L[3][5]);
        TEST_ASSERT_EQUAL_UINT(70 * r, epd_lut_frames(L[2]));
        for (int i = 0; i < EPD_LUT_N; i++)
            TEST_ASSERT_EQUAL_INT(0, epd_lut_dc(L[i])); /* GxEPD2's tables are charge balanced */
        epd_lut_build(EPD_LUTSET_BB, r, L);
        TEST_ASSERT_EQUAL_UINT8(0x80, L[2][0]);
        TEST_ASSERT_EQUAL_UINT8(30 * r, L[2][1]);
        TEST_ASSERT_EQUAL_UINT8(r, L[2][2]);
        TEST_ASSERT_EQUAL_INT(-30 * (int)r, epd_lut_dc(L[2])); /* as unbalanced as the source, scaled */
        TEST_ASSERT_EQUAL_INT(30 * (int)r, epd_lut_dc(L[3]));
    }
}

static void test_clamps(void)
{
    TEST_ASSERT_EQUAL_UINT(1, epd_lut_build(EPD_LUTSET_GX, 0, L));
    TEST_ASSERT_EQUAL_UINT(EPD_LUT_REPS_MAX, epd_lut_build(EPD_LUTSET_GX, 99, L));
    TEST_ASSERT_EQUAL_UINT8(240, L[3][1]);
    TEST_ASSERT_EQUAL_INT(EPD_VCOM_MIN, epd_lut_vcom(EPD_LUTSET_GX, 0));
    TEST_ASSERT_EQUAL_INT(EPD_VCOM_MAX, epd_lut_vcom(EPD_LUTSET_BB, 0x90));
    TEST_ASSERT_EQUAL_INT(0x1c, epd_lut_vcom(EPD_LUTSET_BB, 0x1c));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bb_reps1_matches_source);
    RUN_TEST(test_gx_reps1_matches_source);
    RUN_TEST(test_reps_scale_frames_and_keep_balance);
    RUN_TEST(test_clamps);
    return UNITY_END();
}
