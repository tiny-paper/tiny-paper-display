/* The display firmware's stored panel settings (src/panel_cfg.h): a gray partial switch stored by another firmware under
 * its key does not switch grays off here. SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/panel_cfg.h"
#include "tiny/hal.h"
#include "unity.h"

void setUp(void)
{
    hal_kv_set("xgpart", "", 0); /* the stub's store: a zero length removes the key */
    hal_kv_set(PANEL_KEY_GPART, "", 0);
}
void tearDown(void) {}

static const gpart_cfg_t ON = {1, 0, 3};

static void test_nothing_stored_is_the_default(void)
{
    gpart_cfg_t g = panel_gpart_load(ON);
    TEST_ASSERT_EQUAL_UINT8(1, g.on);
}

static void test_other_firmwares_key_is_ignored(void)
{
    const gpart_cfg_t old = {0, 0, 3}; /* an experiment of the firmware this driver came from: gpart off */
    TEST_ASSERT_EQUAL_INT((int)sizeof old, hal_kv_set("xgpart", &old, sizeof old));
    gpart_cfg_t g = panel_gpart_load(ON);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, g.on, "a stored xgpart of on = 0 must not switch the grays off");
}

static void test_own_key_is_kept(void)
{
    const gpart_cfg_t off = {0, 2, 5};
    panel_gpart_save(&off);
    gpart_cfg_t g = panel_gpart_load(ON);
    TEST_ASSERT_EQUAL_UINT8(0, g.on);
    TEST_ASSERT_EQUAL_UINT8(2, g.dark);
    TEST_ASSERT_EQUAL_UINT8(5, g.light);
}

static void test_invalid_own_value_is_the_default(void)
{
    const gpart_cfg_t bad = {2, 99, 3};
    panel_gpart_save(&bad);
    TEST_ASSERT_EQUAL_UINT8(1, panel_gpart_load(ON).on);
    hal_kv_set(PANEL_KEY_GPART, "ab", 2); /* a value of the wrong size */
    TEST_ASSERT_EQUAL_UINT8(1, panel_gpart_load(ON).on);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_nothing_stored_is_the_default);
    RUN_TEST(test_other_firmwares_key_is_ignored);
    RUN_TEST(test_own_key_is_kept);
    RUN_TEST(test_invalid_own_value_is_the_default);
    return UNITY_END();
}
