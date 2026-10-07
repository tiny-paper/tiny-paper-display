/*
 * wakecal.h - deep-sleep timer wakes that land before the planned time (hardware-free, host-tested).
 *
 * ESP-IDF converts a deep-sleep time to slow-clock ticks with a 10-cycle calibration taken right before the
 * sleep (sleep_modes.c RTC_CLK_SRC_CAL_CYCLES, no Kconfig): on the internal RC the timer then fires up to
 * ~0.5 % early or late (0.3 s on a minute). The HAL asks for a timer shorter by a margin, measures each
 * timer wake against the plan in its own clock (rtclock.h: raw ticks, long calibration) and waits (light
 * sleep) for the rest. The margin adapts: mean error (late only) + 3 x its deviation + a floor.
 * Units: the HAL's raw clock in us. C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_WAKECAL_H
#define TINY_WAKECAL_H

#include <stdbool.h>
#include <stdint.h>

#define WAKECAL_MAGIC     0x57434C31u /* "WCL1" */
#define WAKECAL_INIT_PPM  7000        /* before the first sample: 0.7 % */
#define WAKECAL_FLOOR_PPM 1000
#define WAKECAL_MIN_US    20000       /* fixed part: sleep entry and boot spread */
#define WAKECAL_BAD_PPM   30000       /* a sample beyond 3 % is not the calibration: dropped */
#define WAKECAL_MAX_PPM   20000       /* margin cap */

typedef struct {
    uint32_t magic;
    int32_t mean_ppm;  /* smoothed timer error, + = fired late */
    uint32_t dev_ppm;  /* smoothed deviation (rises fast, decays slowly) */
    uint32_t n;        /* samples */
    uint64_t start_us; /* timer started */
    uint64_t asked_us; /* duration asked */
    uint64_t plan_us;  /* when the caller wanted to wake (0: no timer pending) */
} wakecal_t;

void wakecal_reset(wakecal_t *c);
/* Usable (magic set); otherwise the caller resets it. */
bool wakecal_ok(const wakecal_t *c);
/* Margin in ppm of the sleep for the next timer. */
uint32_t wakecal_margin_ppm(const wakecal_t *c);
/* A deep sleep from now_us until target_us: the timer duration to ask for (the margin taken off), and the plan
 * kept for wakecal_woke. */
uint64_t wakecal_timer(wakecal_t *c, uint64_t now_us, uint64_t target_us);
/* The timer fired at wake_us (HAL clock, from the alarm tick). Learns from it; err_ppm (may be NULL) gets the
 * error of this wake. False when no timer was pending or the sample was dropped (the plan is kept either way). */
bool wakecal_woke(wakecal_t *c, uint64_t wake_us, int32_t *err_ppm);
/* The planned wake of the pending timer (0: none). */
uint64_t wakecal_plan(const wakecal_t *c);
/* The plan is served (or the wake was not the timer). */
void wakecal_clear(wakecal_t *c);

#endif
