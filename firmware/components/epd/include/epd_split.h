/*
 * epd_split.h - where the time of one panel update went (TRMNL X, epd_par.h, hal_esp panel_par.c): a few counters
 * filled from timestamps the update takes anyway, and the log line made of them. Hardware-free, host-tested. Cheap
 * enough to stay on: one clock read per part, no pass over a frame.
 *
 * Parts, in the order of an update (each in microseconds, the line prints whole milliseconds, rounded):
 *   begin   the driver's start of the update (frames set white, tables handed over)
 *   write   the caller's rows copied into the frames
 *   up      panel rails up, until the supply reports good (0 when they were up already; a full refresh powers up
 *           inside the library: there it is part of lib)
 *   prep    whole-update work before the first scan: frame copies, the scan for blacks, fingerprints
 *   masks   per step: the frames of the step (codes per pixel), what it drives
 *   lib     inside the library before the first scan of each of its calls: its codes per row, the task it runs on
 *   scans   the scans themselves: first frame start to the return of each call
 *   down    panel rails down (0 when they are kept up)
 *   after   bookkeeping after the last scan: redrive counts, the frames kept
 *   save    the redrive file (0 when kept in RAM)
 *   log     writing the update's other device log lines (this line is written last)
 * `total` is measured by the caller from its begin to the end of its refresh; other = total less the parts.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_EPD_SPLIT_H
#define TINY_EPD_SPLIT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    EPD_SPLIT_BEGIN = 0,
    EPD_SPLIT_WRITE,
    EPD_SPLIT_UP,
    EPD_SPLIT_PREP,
    EPD_SPLIT_MASKS,
    EPD_SPLIT_LIB,
    EPD_SPLIT_SCANS,
    EPD_SPLIT_DOWN,
    EPD_SPLIT_AFTER,
    EPD_SPLIT_SAVE,
    EPD_SPLIT_LOG,
    EPD_SPLIT_PARTS
};

typedef struct epd_split {
    uint32_t us[EPD_SPLIT_PARTS];
    uint16_t calls; /* library calls (one per step, plus the prime scan) */
} epd_split_t;

void epd_split_reset(epd_split_t *s);
/* `us` more for `part`; a part outside the list is ignored, a sum that would wrap stays at the top. */
void epd_split_add(epd_split_t *s, int part, uint32_t us);
/* One library call that started at t0, began its first scan at first (0: it never scanned) and returned at t1
 * (microsecond clock, may wrap): lib and scans. */
void epd_split_call(epd_split_t *s, uint32_t t0, uint32_t first, uint32_t t1);
/* The parts of `from` added to `s` (the driver's parts and the caller's). */
void epd_split_merge(epd_split_t *s, const epd_split_t *from);
/* "total=<ms> begin=<ms> write=<ms> up=<ms> prep=<ms> masks=<ms> lib=<ms> scans=<ms> down=<ms> after=<ms> save=<ms>
 * log=<ms> other=<ms> calls=<n>" into b; b. */
char *epd_split_str(const epd_split_t *s, uint32_t total_us, char *b, size_t n);

#ifdef __cplusplus
}
#endif

#endif
