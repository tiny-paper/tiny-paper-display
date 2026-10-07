/* BWRY refresh speed (epd_bwspeed.h). SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_bwry.h"
#include "epd_bwspeed.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_native_keeps_the_board_setting(void)
{
    bws_pick_t p = bws_pick(BWS_NATIVE, BWRY_FAST_OFF, true, true);
    TEST_ASSERT_EQUAL_UINT8(BWRY_FAST_OFF, p.fast);
    TEST_ASSERT_EQUAL_UINT8(BWS_LUT_NONE, p.lut);
    TEST_ASSERT_EQUAL_UINT32(0, p.ms); /* the board's own figure */
    TEST_ASSERT_EQUAL_STRING("otp", bws_pick_name(&p));
    p = bws_pick(BWS_NATIVE, BWRY_FAST_E6P, false, false); /* "!tp bwry fast e6p" */
    TEST_ASSERT_EQUAL_UINT32(BWS_E6P_MS, p.ms);
    TEST_ASSERT_EQUAL_STRING("e6p", bws_pick_name(&p));
}

static void test_fast_modes_without_luts_force_the_hot_waveform(void)
{
    for (uint8_t s = BWS_FAST; s <= BWS_FASTEST; s++) {
        bws_pick_t p = bws_pick(s, BWRY_FAST_TSE, false, false);
        TEST_ASSERT_EQUAL_UINT8(BWRY_FAST_E6P, p.fast);
        TEST_ASSERT_EQUAL_UINT8(BWS_LUT_NONE, p.lut);
        TEST_ASSERT_EQUAL_UINT32(12000, p.ms);
    }
}

static void test_luts_when_built_in(void)
{
    bws_pick_t p = bws_pick(BWS_FAST, BWRY_FAST_OFF, true, true);
    TEST_ASSERT_EQUAL_UINT8(BWS_LUT_10S, p.lut);
    TEST_ASSERT_EQUAL_UINT32(10000, p.ms);
    p = bws_pick(BWS_FASTEST, BWRY_FAST_OFF, true, true);
    TEST_ASSERT_EQUAL_UINT8(BWS_LUT_5S, p.lut);
    TEST_ASSERT_EQUAL_UINT32(5000, p.ms);
    TEST_ASSERT_EQUAL_STRING("lut5", bws_pick_name(&p));
    p = bws_pick(BWS_FASTEST, BWRY_FAST_OFF, true, false); /* only the 10 s one */
    TEST_ASSERT_EQUAL_UINT8(BWS_LUT_10S, p.lut);
    p = bws_pick(BWS_FAST, BWRY_FAST_OFF, false, true); /* fast never takes the 5 s one */
    TEST_ASSERT_EQUAL_UINT8(BWRY_FAST_E6P, p.fast);
    TEST_ASSERT_EQUAL_UINT8(BWS_LUT_NONE, p.lut);
    TEST_ASSERT_EQUAL_STRING("fastest", bws_name(BWS_FASTEST));
    TEST_ASSERT_EQUAL_STRING("native", bws_name(7));
}

/* Stock V1.1.21 / V1.1.24: vendor LUT only for REV 06 01 01, else the OTP waveform. */
static void test_lut_rev_gate(void)
{
    static const uint8_t ok[3] = {0x06, 0x01, 0x01}, other[3] = {0x06, 0x04, 0x01}, none[3] = {0, 0, 0};
    TEST_ASSERT_TRUE(bws_lut_allowed(BWS_REV_AUTO, true, ok));
    TEST_ASSERT_FALSE(bws_lut_allowed(BWS_REV_AUTO, true, other));
    TEST_ASSERT_FALSE(bws_lut_allowed(BWS_REV_AUTO, true, none));
    TEST_ASSERT_FALSE(bws_lut_allowed(BWS_REV_AUTO, false, ok)); /* not read (read failed) */
    TEST_ASSERT_TRUE(bws_lut_allowed(BWS_REV_FORCE, false, none));
    TEST_ASSERT_FALSE(bws_lut_allowed(BWS_REV_OFF, true, ok));
    TEST_ASSERT_FALSE(bws_lut_allowed(7, true, ok));
}

static bws_in_t auto_in(int16_t temp)
{
    return (bws_in_t){BWS_AUTO, BWRY_FAST_OFF, true, true, BWS_REV_AUTO, true, {0x06, 0x01, 0x01}, temp};
}

static void check(const bws_in_t *in, uint8_t lut, uint8_t why)
{
    bws_pick_t p = bws_choose(in);
    TEST_ASSERT_EQUAL_UINT8(lut, p.lut);
    TEST_ASSERT_EQUAL_UINT8(why, p.why);
}

/* auto: warm >= 15 C the 5 s LUT, cool 5..14 C the 10 s LUT, colder the OTP waveform */
static void test_auto_bands(void)
{
    bws_in_t in = auto_in(25);
    check(&in, BWS_LUT_5S, BWS_WHY_TEMP);
    in.temp_c = BWS_BAND_WARM_C;
    check(&in, BWS_LUT_5S, BWS_WHY_TEMP);
    in.temp_c = BWS_BAND_WARM_C - 1;
    check(&in, BWS_LUT_10S, BWS_WHY_TEMP);
    in.temp_c = BWS_BAND_COOL_C;
    check(&in, BWS_LUT_10S, BWS_WHY_TEMP);
    in.temp_c = BWS_BAND_COOL_C - 1;
    check(&in, BWS_LUT_NONE, BWS_WHY_TEMP);
    in.temp_c = -10;
    bws_pick_t p = bws_choose(&in);
    TEST_ASSERT_EQUAL_UINT32(0, p.ms); /* the native waveform */
    TEST_ASSERT_EQUAL_STRING("otp", bws_lut_name(&p));
    TEST_ASSERT_EQUAL_STRING("temp", bws_why_name(p.why));
    TEST_ASSERT_TRUE(bws_wants_temp(&in));
}

static void test_auto_sensor_failure_is_otp(void)
{
    bws_in_t in = auto_in(BWS_TEMP_NONE);
    check(&in, BWS_LUT_NONE, BWS_WHY_TEMP);
    in.temp_c = BWS_TEMP_MAX + 1; /* implausible (e.g. 0x7f read as 127) */
    check(&in, BWS_LUT_NONE, BWS_WHY_TEMP);
    in.temp_c = BWS_TEMP_MIN - 1;
    check(&in, BWS_LUT_NONE, BWS_WHY_TEMP);
}

static void test_auto_rev_and_off(void)
{
    bws_in_t in = auto_in(25);
    in.rev[1] = 0x04; /* REV 06 04 01 */
    check(&in, BWS_LUT_NONE, BWS_WHY_REV);
    TEST_ASSERT_FALSE(bws_wants_temp(&in));
    in.rev_read = false;
    check(&in, BWS_LUT_NONE, BWS_WHY_REV);
    in.lut_mode = BWS_REV_FORCE; /* testing: neither REV nor temperature */
    in.temp_c = -20;
    check(&in, BWS_LUT_5S, BWS_WHY_SET);
    in.lut_mode = BWS_REV_OFF;
    check(&in, BWS_LUT_NONE, BWS_WHY_OFF);
    in = auto_in(25);
    in.lut5 = in.lut10 = false; /* a build without LUTs: as native */
    check(&in, BWS_LUT_NONE, BWS_WHY_OFF);
    TEST_ASSERT_FALSE(bws_wants_temp(&in));
}

static void test_auto_missing_lut(void)
{
    bws_in_t in = auto_in(25);
    in.lut5 = false; /* warm without the 5 s LUT: the 10 s one */
    check(&in, BWS_LUT_10S, BWS_WHY_TEMP);
    in = auto_in(10);
    in.lut10 = false; /* cool without the 10 s LUT: OTP, not the 5 s one */
    check(&in, BWS_LUT_NONE, BWS_WHY_TEMP);
}

/* fast / fastest keep their LUT at any temperature (the REV gate still applies) */
static void test_set_speeds_ignore_temperature(void)
{
    bws_in_t in = auto_in(-10);
    in.speed = BWS_FASTEST;
    check(&in, BWS_LUT_5S, BWS_WHY_SET);
    in.speed = BWS_FAST;
    check(&in, BWS_LUT_10S, BWS_WHY_SET);
    in.rev[0] = 0;
    bws_pick_t p = bws_choose(&in);
    TEST_ASSERT_EQUAL_STRING("e6p", bws_pick_name(&p));
    TEST_ASSERT_EQUAL_UINT8(BWS_WHY_REV, p.why);
    in.speed = BWS_NATIVE;
    check(&in, BWS_LUT_NONE, BWS_WHY_SET);
    TEST_ASSERT_EQUAL_STRING("auto", bws_name(BWS_AUTO));
}

/* this refresh's reading, else a recent one (PicPak: the read right after a boot failed) */
static void test_temp_cache(void)
{
    TEST_ASSERT_EQUAL_INT16(27, bws_temp_use(27, 20, 10));
    TEST_ASSERT_EQUAL_INT16(20, bws_temp_use(BWS_TEMP_NONE, 20, 10));
    TEST_ASSERT_EQUAL_INT16(20, bws_temp_use(BWS_TEMP_MAX + 40, 20, BWS_TEMP_CACHE_S));
    TEST_ASSERT_EQUAL_INT16(BWS_TEMP_NONE, bws_temp_use(BWS_TEMP_NONE, 20, BWS_TEMP_CACHE_S + 1));
    TEST_ASSERT_EQUAL_INT16(BWS_TEMP_NONE, bws_temp_use(BWS_TEMP_NONE, BWS_TEMP_NONE, 0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_native_keeps_the_board_setting);
    RUN_TEST(test_fast_modes_without_luts_force_the_hot_waveform);
    RUN_TEST(test_luts_when_built_in);
    RUN_TEST(test_lut_rev_gate);
    RUN_TEST(test_auto_bands);
    RUN_TEST(test_auto_sensor_failure_is_otp);
    RUN_TEST(test_auto_rev_and_off);
    RUN_TEST(test_auto_missing_lut);
    RUN_TEST(test_set_speeds_ignore_temperature);
    RUN_TEST(test_temp_cache);
    return UNITY_END();
}
