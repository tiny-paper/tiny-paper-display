/* Reset classes (src/reset_class.h). SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/reset_class.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* PicPak 2026-09-25: TG1WDT_SYS_RST at portal start. A watchdog while the radio starts is a power fault like a
 * brownout (settle, no crash count); elsewhere it stays a crash. A panic is always a crash. */
static void test_classes(void)
{
    TEST_ASSERT_EQUAL(RST_CLASS_POWER, rst_classify(RST_KIND_WDT, true));
    TEST_ASSERT_EQUAL(RST_CLASS_CRASH, rst_classify(RST_KIND_WDT, false));
    TEST_ASSERT_EQUAL(RST_CLASS_POWER, rst_classify(RST_KIND_BROWNOUT, false));
    TEST_ASSERT_EQUAL(RST_CLASS_POWER, rst_classify(RST_KIND_BROWNOUT, true));
    TEST_ASSERT_EQUAL(RST_CLASS_CRASH, rst_classify(RST_KIND_PANIC, true));
    TEST_ASSERT_EQUAL(RST_CLASS_NONE, rst_classify(RST_KIND_OTHER, true));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_classes);
    return UNITY_END();
}
