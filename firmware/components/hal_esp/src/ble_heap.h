/*
 * ble_heap.h - may the BLE stack come up on this heap (esp/ble_link.c hal_ble_open)? Hardware-free, host-tested
 * (app/test/test_membudget.c); the number and the decision are membudget's (MB_BLE_OPEN).
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_BLE_HEAP_H
#define TINY_BLE_HEAP_H

#include <stdbool.h>
#include <stddef.h>

#include "../../app/include/membudget.h"

/* free_bytes: the free heap before the open. false: refuse the open (HAL_E_NOSPACE), nothing allocated yet. */
static inline bool ble_open_fits(size_t free_bytes)
{
    mb_have_t h = {false, 0, free_bytes};
    return membudget(MB_BLE_OPEN, mb_need(NULL, MB_BLE_OPEN, 0, 0), &h) == MB_RUN;
}

#endif
