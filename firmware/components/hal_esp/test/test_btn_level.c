/* Shared button / battery pin (src/btn_level.h). SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/btn_level.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define DIV 145 /* PicPak: battery = pin x 1.45 */
#define MAX 1000

/* Released at any battery the C3 runs on reads released through the ADC; pressed (~0 V) reads pressed. */
static void test_levels(void)
{
    static const int batt[] = {4200, 3700, 3600, 3300, 3000, 2800};
    for (unsigned i = 0; i < sizeof batt / sizeof batt[0]; i++) {
        int released = batt[i] * 100 / DIV;
        TEST_ASSERT_FALSE(btn_pressed_mv(released, batt[i], DIV, MAX));
        TEST_ASSERT_FALSE(btn_pressed_mv(released - 150, batt[i], DIV, MAX)); /* ADC error, WiFi noise */
        TEST_ASSERT_TRUE(btn_pressed_mv(40, batt[i], DIV, MAX));
    }
    /* battery unknown (first boot, pressed during the reading): the board threshold */
    TEST_ASSERT_TRUE(btn_pressed_mv(999, -1, DIV, MAX));
    TEST_ASSERT_FALSE(btn_pressed_mv(1000, 0, DIV, MAX));
    /* a battery so low that 1000 mV is near the released level: half of it */
    TEST_ASSERT_EQUAL(655, btn_threshold_mv(1900, DIV, MAX));
    TEST_ASSERT_EQUAL(MAX, btn_threshold_mv(4200, DIV, MAX));
}

/* Research #14: released pin = Vbat / 1.45 below VIH (0.75 x 3.3 V = 2475 mV) under ~3.59 V. */
static void test_digital_spec(void)
{
    TEST_ASSERT_TRUE(btn_digital_in_spec(4200, DIV));
    TEST_ASSERT_TRUE(btn_digital_in_spec(3600, DIV));
    TEST_ASSERT_FALSE(btn_digital_in_spec(3580, DIV));
    TEST_ASSERT_FALSE(btn_digital_in_spec(3300, DIV));
    TEST_ASSERT_FALSE(btn_digital_in_spec(-1, DIV));
}

/* PicPak 2026-09-26/27 (release 1434): the pin read below 1000 mV after a watchdog and a software reset: "held" at
 * boot (safe mode), batt=-1, and a 10 s "hold" that factory-reset. A press shorts the node to ~0 V, which the
 * digital input reads low; an ADC reading of a pressed level with the digital input high is not a press. */
static void test_adc_cross_check(void)
{
    /* a press: both low, at any battery (and before the first battery reading: -1, 0) */
    static const int batt[] = {4200, 3600, 3300, -1, 0};
    for (unsigned i = 0; i < sizeof batt / sizeof batt[0]; i++) {
        TEST_ASSERT_EQUAL(BTN_LVL_DOWN, btn_level_read(30, true, batt[i], DIV, MAX));
        /* the bad reading: ADC 0 (or anything under the threshold), digital high */
        TEST_ASSERT_EQUAL(BTN_LVL_ADC_BAD, btn_level_read(0, false, batt[i], DIV, MAX));
        TEST_ASSERT_EQUAL(BTN_LVL_ADC_BAD, btn_level_read(900, false, batt[i], DIV, MAX));
    }
    /* released: up, also when a weak cell leaves the released level under VIH (digital low) */
    TEST_ASSERT_EQUAL(BTN_LVL_UP, btn_level_read(4100 * 100 / DIV, false, 4100, DIV, MAX));
    TEST_ASSERT_EQUAL(BTN_LVL_UP, btn_level_read(3300 * 100 / DIV, true, 3300, DIV, MAX));
    /* the ADC failed: the digital level */
    TEST_ASSERT_EQUAL(BTN_LVL_DOWN, btn_level_read(-5, true, 4000, DIV, MAX));
    TEST_ASSERT_EQUAL(BTN_LVL_UP, btn_level_read(-5, false, 4000, DIV, MAX));
    /* a supply sag cannot fake a press: a released pin at 3.0 V (brownout is lower) is 2068 mV */
    TEST_ASSERT_EQUAL(BTN_LVL_UP, btn_level_read(3000 * 100 / DIV, false, 4100, DIV, MAX));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_adc_cross_check);
    RUN_TEST(test_levels);
    RUN_TEST(test_digital_spec);
    return UNITY_END();
}
