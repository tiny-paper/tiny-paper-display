/*
 * btn_queue.h - every press of the TRMNL X touch bar, in order, until the app takes it (hal_button_take): the bar is
 * read by its own task (the app may be busy: render, panel, WiFi, a task; hardware 2026-09-28: every touch in a 35 s
 * refresh was lost), and every press that ended by a release comes here, also the ones the app watched (it takes its
 * own press back at the release). Hardware-free, host-tested; the sim uses it too.
 * Rules (docs/spec/firmware.md "Buttons"):
 *   - a press: its start and release (hal_rtc_us), its input (TINY_INPUT_*: the X bar's zone), its swipe (-1 left,
 *     +1 right, 0 none) and whether it was a brush (touch_btn.h, hal_button_brush); presses are never merged (the app
 *     counts them, APP_COUNT_MS)
 *   - BQ_LEN presses at most; one more is dropped (counted in `dropped`), the older ones stay
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_BTN_QUEUE_H
#define TINY_BTN_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#define BQ_LEN 16

typedef struct {
    uint64_t down, up; /* hal_rtc_us of the start and the release */
    int8_t swipe;
    uint8_t input; /* TINY_INPUT_* */
    bool brush;    /* 2 or more zones, or the slider moved (tbtn_brush) */
} bq_press_t;

typedef struct {
    bq_press_t p[BQ_LEN];
    uint8_t head, n;
    uint32_t dropped;
} bq_t;

void bq_reset(bq_t *q);
/* A press that ended; false: the queue was full (dropped). */
bool bq_push(bq_t *q, uint64_t down, uint64_t up, uint8_t input, int8_t swipe, bool brush);
/* The oldest press; false: none. */
bool bq_take(bq_t *q, bq_press_t *out);

#endif
