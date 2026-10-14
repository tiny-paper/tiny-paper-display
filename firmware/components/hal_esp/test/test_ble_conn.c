/* The GAP connect event: a live link with a nonzero status is a connection (src/ble_conn.h).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/ble_conn.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void ok_status(void) { TEST_ASSERT_EQUAL(BLE_CONN_TAKE, ble_conn_action(0, true)); }
/* seen 2026-10-14: status 26 (0x1A) with handle 1 from a Windows central; the X dropped every notification */
static void live_link_bad_status_is_taken(void)
{
    TEST_ASSERT_EQUAL(BLE_CONN_TAKE, ble_conn_action(0x1A, true));
    TEST_ASSERT_EQUAL(BLE_CONN_TAKE, ble_conn_action(13, true));
}
static void no_link_restarts_advertising(void)
{
    TEST_ASSERT_EQUAL(BLE_CONN_RETRY_ADV, ble_conn_action(0x3E, false));
    TEST_ASSERT_EQUAL(BLE_CONN_RETRY_ADV, ble_conn_action(1, false));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(ok_status);
    RUN_TEST(live_link_bad_status_is_taken);
    RUN_TEST(no_link_restarts_advertising);
    return UNITY_END();
}
