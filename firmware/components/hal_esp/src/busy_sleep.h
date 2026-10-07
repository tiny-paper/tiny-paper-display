/*
 * busy_sleep.h - light sleep through a panel BUSY wait (PicPak, hal_esp panel_bwry.c): whether to sleep and for how
 * long at most. Hardware-free, host-tested (test/test_busy_sleep.c).
 *
 * The chip wakes on BUSY going high (GPIO wake), on a press (the button's GPIO wake, only when the released pin reads
 * high and the button is up now), or at the cap: BSL_MAX_MS with the button wake, BSL_POLL_MS without (a timer wake
 * then samples the button through the ADC, as the 5 ms awake poll did). No sleep while WiFi is on (a session or the
 * portal) or while awake on USB (the link is answered through the wait), nor for less than BSL_MIN_MS.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_BUSY_SLEEP_H
#define TINY_BUSY_SLEEP_H

#include <stdbool.h>
#include <stdint.h>

#define BSL_MAX_MS 1000
#define BSL_POLL_MS 100
#define BSL_MIN_MS 5

typedef struct {
    bool wifi_on, stay_awake;
    bool button_down, button_wake_ok;
    uint32_t left_ms; /* until the BUSY timeout */
} bsl_in_t;

typedef struct {
    uint32_t ms;      /* 0 = do not sleep: poll awake */
    bool button_wake; /* arm the button's GPIO wake */
} bsl_plan_t;

bsl_plan_t bsl_plan(const bsl_in_t *in);

#endif
