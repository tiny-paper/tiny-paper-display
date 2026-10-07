/* TRMNL X clear choice (epd_clear.h). SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_clear.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_image_is_slow(void)
{
    const char *why = "";
    for (uint8_t n = 0; n < 16; n++) {
        TEST_ASSERT_EQUAL_INT(EPDP_CLEAR_SLOW, epd_clear_auto(n, true, &why));
        TEST_ASSERT_EQUAL_STRING("image", why);
    }
}

static void test_ui_is_fast_and_every_8th_is_the_backstop(void)
{
    const char *why = NULL;
    for (uint8_t n = 1; n < 8; n++) {
        TEST_ASSERT_EQUAL_INT(EPDP_CLEAR_FAST, epd_clear_auto(n, false, &why));
        TEST_ASSERT_EQUAL_STRING("", why);
    }
    TEST_ASSERT_EQUAL_INT(EPDP_CLEAR_SLOW, epd_clear_auto(8, false, &why));
    TEST_ASSERT_EQUAL_STRING("every8", why);
    TEST_ASSERT_EQUAL_INT(EPDP_CLEAR_SLOW, epd_clear_auto(0, false, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_image_is_slow);
    RUN_TEST(test_ui_is_fast_and_every_8th_is_the_backstop);
    return UNITY_END();
}
