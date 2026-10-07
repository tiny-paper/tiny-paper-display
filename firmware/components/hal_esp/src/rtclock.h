/*
 * rtclock.h - hal_rtc_us from the raw RTC slow-clock tick counter and our own calibration.
 * Hardware-free, host-tested; esp/sys.c keeps the state in RTC_NOINIT memory.
 *
 * Why: ESP-IDF recalibrates the slow clock before every sleep with only 10 cycles (sleep_modes.c
 * RTC_CLK_SRC_CAL_CYCLES) and times the whole sleep with that value. On the internal 150 kHz RC
 * (every board) that single 67 us measurement is off by up to ~0.5 %, and the error is biased:
 * 15 s behind within an hour on the TRMNL OG, drift estimates of -5.6e6 ppb that change with every
 * sync. Here the ticks are counted with the boot calibration (CONFIG_RTC_CLK_CAL_CYCLES, 1024),
 * smoothed over wakes; the remaining (slow, temperature) drift is the timekeeper's job.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_RTCLOCK_H
#define TINY_RTCLOCK_H

#include <stdbool.h>
#include <stdint.h>

#define RTCLOCK_MAGIC 0x52434C4Bu /* "RCLK" */
#define RTCLOCK_FRACT 19          /* cal: us per tick in Q13.19, as esp_clk_slowclk_cal_get */

typedef struct {
    uint32_t magic;
    uint32_t cal;      /* smoothed period, us << 19 */
    uint64_t base_us;  /* time at ticks0 */
    uint64_t ticks0;
} rtclock_t;

/* The state is usable for this tick count (magic set, counter not reset since). */
bool rtclock_ok(const rtclock_t *c, uint64_t ticks);
/* Start at now_us (continuity with the previous clock) with calibration cal. */
void rtclock_init(rtclock_t *c, uint64_t ticks, uint64_t now_us, uint32_t cal);
uint64_t rtclock_us(const rtclock_t *c, uint64_t ticks);
/* A boot's calibration: rebase at ticks with the old one, then blend cal in (1/4 weight; a value
 * off by more than 20 % replaces it). */
void rtclock_recal(rtclock_t *c, uint64_t ticks, uint32_t cal);

#endif
