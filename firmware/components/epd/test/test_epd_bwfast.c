/* Experimental fast black/white partial LUT (epd_bwfast.h): caps, balance, hold, policy. Issue #190.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "epd_bwfast.h"
#include "epd_bwspeed.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define AT(t, k, c) (((t) * BWF_RECORDS + (k)) + 48 * (c))

/* A stand-in for the vendor full LUT in the repository layout: 8 active records in every table, the same bytes 0,
 * 3..10 in record 1 of all tables (as the vendor table has), its own analog bytes. */
static void full_lut(uint8_t L[BWF_LUT_BYTES])
{
    static const uint8_t analog[BWF_ANALOG] = {0x00, 0x1b, 0x78, 0x18, 0x1b, 0x14, 0x06};
    static const uint8_t skel1[BWF_RECORD] = {0xef, 0x9f, 0x9f, 0x7f, 0xef, 0xbd, 0x7f, 0xbd, 0xef, 0x7f, 0x6f};
    memset(L, 0xef, BWF_WAVE);
    for (int t = 0; t < BWF_TABLES; t++)
        for (int b = 0; b < BWF_RECORD; b++)
            L[AT(t, 1, b)] = skel1[b];
    memcpy(L + BWF_WAVE, analog, BWF_ANALOG);
}

static void build(const bwf_cfg_t *c, uint8_t out[BWF_LUT_BYTES])
{
    uint8_t full[BWF_LUT_BYTES];
    full_lut(full);
    TEST_ASSERT_EQUAL_INT(BWF_OK, bwf_build(c, full, out));
}

static void test_build_plain(void)
{
    uint8_t out[BWF_LUT_BYTES];
    bwf_cfg_t c = {BWF_PULSES_DEFAULT, BWF_RECORDS_DEFAULT, BWF_SKEL_PLAIN};
    build(&c, out);
    static const uint8_t analog[BWF_ANALOG] = {0x00, 0x1b, 0x78, 0x18, 0x1b, 0x14, 0x06};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(analog, out + BWF_WAVE, BWF_ANALOG); /* the full LUT's voltages, VCOM, PLL */
    /* black: four black-going symbols (10 10 10 10 = 0xaa), white the mirror (0x55); byte 2 idle */
    TEST_ASSERT_EQUAL_HEX8(0xaa, out[AT(BWF_T_BLACK, 0, 1)]);
    TEST_ASSERT_EQUAL_HEX8(0xff, out[AT(BWF_T_BLACK, 0, 2)]);
    TEST_ASSERT_EQUAL_HEX8(0x55, out[AT(BWF_T_WHITE, 0, 1)]);
    TEST_ASSERT_EQUAL_HEX8(0xff, out[AT(BWF_T_WHITE, 0, 2)]);
    for (int t = 0; t < BWF_TABLES; t++) {
        TEST_ASSERT_EQUAL_HEX8(BWF_CARRIER, out[AT(t, 0, 0)]);
        for (int b = 3; b < BWF_RECORD; b++)
            TEST_ASSERT_EQUAL_HEX8(0xff, out[AT(t, 0, b)]);
        for (int k = 1; k < BWF_RECORDS; k++)
            for (int b = 0; b < BWF_RECORD; b++)
                TEST_ASSERT_EQUAL_HEX8(0xff, out[AT(t, k, b)]); /* one record, then the terminator */
    }
    bwf_stat_t st;
    bwf_stat(out, &st);
    TEST_ASSERT_EQUAL(4, st.t[BWF_T_BLACK].b);
    TEST_ASSERT_EQUAL(0, st.t[BWF_T_BLACK].w);
    TEST_ASSERT_EQUAL(4, st.t[BWF_T_WHITE].w);
    TEST_ASSERT_EQUAL(0, st.t[BWF_T_WHITE].b);
    for (int t = 0; t < BWF_TABLES; t++) {
        TEST_ASSERT_EQUAL(1, st.t[t].active);
        TEST_ASSERT_EQUAL(0, st.t[t].third);
    }
    TEST_ASSERT_EQUAL(0, st.t[BWF_T_YELLOW].b + st.t[BWF_T_RED].b + st.t[BWF_T_VCOM].b + st.t[BWF_T_BORDER].b);
    TEST_ASSERT_EQUAL_INT(BWF_OK, bwf_check(out));
}

static void test_build_full_skeleton_two_records(void)
{
    uint8_t out[BWF_LUT_BYTES], full[BWF_LUT_BYTES];
    full_lut(full);
    bwf_cfg_t c = {8, 2, BWF_SKEL_FULL};
    build(&c, out);
    for (int t = 0; t < BWF_TABLES; t++)
        for (int k = 0; k < 2; k++)
            for (int b = 0; b < BWF_RECORD; b++)
                if (b != 1 && b != 2)
                    TEST_ASSERT_EQUAL_HEX8(full[AT(t, 1, b)], out[AT(t, k, b)]);
    TEST_ASSERT_EQUAL_HEX8(0xaa, out[AT(BWF_T_BLACK, 1, 2)]);
    TEST_ASSERT_EQUAL_HEX8(0x55, out[AT(BWF_T_WHITE, 1, 2)]);
    TEST_ASSERT_EQUAL_HEX8(0xff, out[AT(BWF_T_RED, 1, 1)]); /* the hold tables carry the skeleton, no drive */
    bwf_stat_t st;
    bwf_stat(out, &st);
    TEST_ASSERT_EQUAL(BWF_MAX_DRIVE, st.t[BWF_T_BLACK].b);
    TEST_ASSERT_EQUAL(2, st.t[BWF_T_RED].active);
    TEST_ASSERT_EQUAL_INT(BWF_OK, bwf_check(out));
}

static void test_build_refuses_beyond_the_caps(void)
{
    uint8_t out[BWF_LUT_BYTES], full[BWF_LUT_BYTES];
    full_lut(full);
    bwf_cfg_t c = {9, 1, BWF_SKEL_PLAIN};
    TEST_ASSERT_EQUAL_INT(BWF_E_CFG, bwf_build(&c, full, out));
    c = (bwf_cfg_t){1, 3, BWF_SKEL_PLAIN};
    TEST_ASSERT_EQUAL_INT(BWF_E_CFG, bwf_build(&c, full, out));
    c = (bwf_cfg_t){0, 1, BWF_SKEL_PLAIN};
    TEST_ASSERT_EQUAL_INT(BWF_E_CFG, bwf_build(&c, full, out));
    c = (bwf_cfg_t){1, 1, 2};
    TEST_ASSERT_EQUAL_INT(BWF_E_CFG, bwf_build(&c, full, out));
    c = (bwf_cfg_t){1, 1, BWF_SKEL_PLAIN};
    TEST_ASSERT_EQUAL_INT(BWF_E_CFG, bwf_build(&c, NULL, out));
}

/* the vendor-shaped table (8 active records) and a third active record are too long */
static void test_check_refuses_a_long_table(void)
{
    uint8_t out[BWF_LUT_BYTES];
    full_lut(out);
    TEST_ASSERT_EQUAL_INT(BWF_E_LONG, bwf_check(out));
    bwf_cfg_t c = {4, 2, BWF_SKEL_PLAIN};
    build(&c, out);
    out[AT(BWF_T_BLACK, 2, 0)] = BWF_CARRIER; /* a third record */
    TEST_ASSERT_EQUAL_INT(BWF_E_LONG, bwf_check(out));
}

static void test_check_refuses_unbalanced(void)
{
    uint8_t out[BWF_LUT_BYTES];
    bwf_cfg_t c = {4, 1, BWF_SKEL_PLAIN};
    build(&c, out);
    out[AT(BWF_T_WHITE, 0, 1)] = 0x57; /* 01 01 01 11: one white pulse fewer than the black table has */
    TEST_ASSERT_EQUAL_INT(BWF_E_UNBALANCED, bwf_check(out));
    build(&c, out);
    out[AT(BWF_T_BLACK, 0, 2)] = 0x7f; /* a white-going symbol in the black table */
    TEST_ASSERT_EQUAL_INT(BWF_E_UNBALANCED, bwf_check(out));
    build(&c, out);
    out[AT(BWF_T_BLACK, 0, 2)] = 0x3f; /* the third level */
    TEST_ASSERT_EQUAL_INT(BWF_E_UNBALANCED, bwf_check(out));
    build(&c, out);
    out[AT(BWF_T_WHITE, 0, 1)] = 0xaa; /* both tables black-going */
    TEST_ASSERT_EQUAL_INT(BWF_E_UNBALANCED, bwf_check(out));
}

static void test_check_refuses_drive_in_a_hold_table(void)
{
    static const int hold[] = {BWF_T_VCOM, BWF_T_YELLOW, BWF_T_RED, BWF_T_BORDER};
    for (unsigned i = 0; i < sizeof hold / sizeof hold[0]; i++) {
        uint8_t out[BWF_LUT_BYTES];
        bwf_cfg_t c = {4, 1, BWF_SKEL_FULL};
        build(&c, out);
        out[AT(hold[i], 0, 1)] = 0xbf; /* one black-going symbol */
        TEST_ASSERT_EQUAL_INT(BWF_E_HOLD, bwf_check(out));
    }
}

static void test_check_refuses_a_skeleton_that_differs_or_lacks_the_carrier(void)
{
    uint8_t out[BWF_LUT_BYTES];
    bwf_cfg_t c = {4, 1, BWF_SKEL_FULL};
    build(&c, out);
    out[AT(BWF_T_YELLOW, 0, 5)] ^= 0x10; /* a timing byte differs in one table */
    TEST_ASSERT_EQUAL_INT(BWF_E_SKEL, bwf_check(out));
    c.skeleton = BWF_SKEL_PLAIN;
    build(&c, out);
    for (int t = 0; t < BWF_TABLES; t++)
        out[AT(t, 0, 0)] = 0xff; /* no carrier: an all-0xFF record ends the hold tables at record 0 */
    TEST_ASSERT_EQUAL_INT(BWF_E_SKEL, bwf_check(out));
}

static void test_row_colour(void)
{
    uint8_t row[8];
    memset(row, 0x55, sizeof row); /* white */
    TEST_ASSERT_FALSE(bwf_row_has_colour(row, sizeof row));
    row[3] = 0x05; /* black black black white */
    TEST_ASSERT_FALSE(bwf_row_has_colour(row, sizeof row));
    row[7] = 0x56; /* white white white yellow */
    TEST_ASSERT_TRUE(bwf_row_has_colour(row, sizeof row));
    row[7] = 0xd5; /* red white white white */
    TEST_ASSERT_TRUE(bwf_row_has_colour(row, sizeof row));
    TEST_ASSERT_FALSE(bwf_row_has_colour(row, 7)); /* not in the bytes given */
}

/* the default (no "!tp bwfast on", a zeroed or missing NVS blob) refuses everything, even the best state */
static void test_policy_default_is_off(void)
{
    bwf_set_t s = {0};
    bwp_state_t ps = {2, false};
    TEST_ASSERT_EQUAL_INT(BWF_WHY_OFF, bwf_why(&s, 5, &ps, 25, true));
    TEST_ASSERT_FALSE(bwf_set_valid(&s));
    bwf_set_t off = {0, BWF_PULSES_DEFAULT, BWF_RECORDS_DEFAULT, BWF_SKEL_PLAIN, BWF_MIN_C_DEFAULT}; /* "!tp bwfast off" */
    TEST_ASSERT_TRUE(bwf_set_valid(&off));
    TEST_ASSERT_EQUAL_INT(BWF_WHY_OFF, bwf_why(&off, 5, &ps, 25, true));
    bwf_set_t on = {1, BWF_PULSES_DEFAULT, BWF_RECORDS_DEFAULT, BWF_SKEL_PLAIN, BWF_MIN_C_DEFAULT};
    TEST_ASSERT_TRUE(bwf_set_valid(&on));
    TEST_ASSERT_EQUAL_INT(BWF_WHY_OK, bwf_why(&on, 5, &ps, 25, true));
    on.pulses = 9; /* a corrupt blob counts as off */
    TEST_ASSERT_EQUAL_INT(BWF_WHY_OFF, bwf_why(&on, 5, &ps, 25, true));
    on.pulses = 1;
    on.min_c = 70;
    TEST_ASSERT_FALSE(bwf_set_valid(&on));
    TEST_ASSERT_EQUAL_INT(BWF_WHY_OFF, bwf_why(NULL, 5, &ps, 25, true));
}

static void test_policy_forces_a_full_after_n_and_after_a_failure(void)
{
    bwf_set_t on = {1, BWF_PULSES_DEFAULT, BWF_RECORDS_DEFAULT, BWF_SKEL_PLAIN, BWF_MIN_C_DEFAULT};
    bwp_state_t ps = {4, false};
    TEST_ASSERT_EQUAL_INT(BWF_WHY_OK, bwf_why(&on, 5, &ps, 25, true));
    ps.since_full = 5;
    TEST_ASSERT_EQUAL_INT(BWF_WHY_EVERY, bwf_why(&on, 5, &ps, 25, true));
    ps.since_full = 100;
    TEST_ASSERT_EQUAL_INT(BWF_WHY_OK, bwf_why(&on, 0, &ps, 25, true)); /* 0: only the app's full_every */
    bwp_note(&ps, true, true);
    TEST_ASSERT_EQUAL_INT(BWF_WHY_OK, bwf_why(&on, 5, &ps, 25, true));
    bwp_note(&ps, false, false); /* a failed partial */
    TEST_ASSERT_EQUAL_INT(BWF_WHY_NEED_FULL, bwf_why(&on, 5, &ps, 25, true));
    bwp_state_t cold = {0, true};
    TEST_ASSERT_EQUAL_INT(BWF_WHY_NEED_FULL, bwf_why(&on, 5, &cold, 25, true));
}

static void test_policy_temperature_and_full_lut(void)
{
    bwf_set_t on = {1, BWF_PULSES_DEFAULT, BWF_RECORDS_DEFAULT, BWF_SKEL_PLAIN, BWF_MIN_C_DEFAULT};
    bwp_state_t ps = {0, false};
    TEST_ASSERT_EQUAL_INT(BWF_WHY_TEMP, bwf_why(&on, 5, &ps, BWS_TEMP_NONE, true));
    TEST_ASSERT_EQUAL_INT(BWF_WHY_TEMP, bwf_why(&on, 5, &ps, 14, true));
    TEST_ASSERT_EQUAL_INT(BWF_WHY_OK, bwf_why(&on, 5, &ps, 15, true));
    TEST_ASSERT_EQUAL_INT(BWF_WHY_NO_LUT, bwf_why(&on, 5, &ps, 25, false)); /* full refreshes on the OTP waveform */
    on.min_c = -25;
    TEST_ASSERT_EQUAL_INT(BWF_WHY_OK, bwf_why(&on, 5, &ps, -20, true));
    TEST_ASSERT_EQUAL_INT(BWF_WHY_TEMP, bwf_why(&on, 5, &ps, 61, true)); /* implausible reading */
    TEST_ASSERT_EQUAL_STRING("temp", bwf_why_name(BWF_WHY_TEMP));
    TEST_ASSERT_EQUAL_STRING("unbalanced", bwf_err_name(BWF_E_UNBALANCED));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_build_plain);
    RUN_TEST(test_build_full_skeleton_two_records);
    RUN_TEST(test_build_refuses_beyond_the_caps);
    RUN_TEST(test_check_refuses_a_long_table);
    RUN_TEST(test_check_refuses_unbalanced);
    RUN_TEST(test_check_refuses_drive_in_a_hold_table);
    RUN_TEST(test_check_refuses_a_skeleton_that_differs_or_lacks_the_carrier);
    RUN_TEST(test_row_colour);
    RUN_TEST(test_policy_default_is_off);
    RUN_TEST(test_policy_forces_a_full_after_n_and_after_a_failure);
    RUN_TEST(test_policy_temperature_and_full_lut);
    return UNITY_END();
}
