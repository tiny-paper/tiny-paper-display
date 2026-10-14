/*
 * ble_conn.h - what a GAP connect event means (esp/ble_link.c gap_event). Hardware-free, host-tested
 * (test/test_ble_conn.c). C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_BLE_CONN_H
#define TINY_BLE_CONN_H

#include <stdbool.h>

typedef enum { BLE_CONN_TAKE, BLE_CONN_RETRY_ADV } ble_conn_action_t;

/* status: ev->connect.status. link_exists: the host still holds that connection handle (ble_gap_conn_find).
 * NimBLE sends the connect event after the remote-features read, and passes that read's HCI status on: a
 * central whose controller lacks the feature (Windows with a Bluetooth 4.0 dongle: 0x1A, unsupported remote
 * feature) gives a nonzero status for a live link. A live link is taken whatever the status; advertising is
 * restarted only when there is no link (a restart with a link up fails: out of memory, one connection). */
static inline ble_conn_action_t ble_conn_action(int status, bool link_exists)
{
    return status == 0 || link_exists ? BLE_CONN_TAKE : BLE_CONN_RETRY_ADV;
}

#endif
