/*
 * epd_parjob.h - the frames of one 1 bpp partial update of the FastEPD glue (TRMNL X, epd_par.h): what is copied,
 * searched and built before, between and after the scans (epd_parwave.h has the waveform). Hardware-free,
 * host-tested.
 *
 * The caller's update: the library's two frames set white (epd_parjob_white), the changed rows [y0, y1] written
 * into them (old pixels in prev, new in cur), then begin, per step `step` and the library's scans, then end.
 * A frame is 328 KB on the X and each pass over one costs tens of ms (measured 2026-10-08: about 100 ms a step
 * and 200 ms an update that were no scan). Two ways of working, same result:
 *   whole   every pass covers the whole frame, as before this file
 *   rows    every pass covers only the rows that can hold anything: the written rows, joined with the rows where
 *           a black still has redrives left when the redrive is on (the kick drives those too)
 * Why the result is the same: outside those rows both frames are white, so nothing is driven, nothing changes and
 * the redrive counts are 0; the steps' frames there are white in both ways. test_epd_parjob runs both on the same
 * updates and compares every frame the library is handed, the frames kept and the redrive counts, byte for byte.
 * What differs: the saved copies P and C hold only the worked rows (the rest is not read), so a fingerprint of
 * them covers those rows.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_EPD_PARJOB_H
#define TINY_EPD_PARJOB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epd_parwave.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* set by the caller before the first update, kept by it */
    uint8_t *prev, *cur; /* the library's frames, n bytes each */
    uint8_t *P, *C;      /* scratch: the caller's old and new frames during an update, n bytes each */
    uint8_t *fresh;      /* redrives left, EPD_FRESH_PLANES * n bytes */
    size_t n, rb;        /* bytes of a frame and of a row */
    int h;               /* rows: n == rb * h */
    /* state across updates */
    int f0, f1;          /* rows where fresh may be set (-1 -1: none); epd_parjob_fresh_unknown: all */
    bool white_ok;       /* both library frames are white outside rows [w0, w1] */
    bool whited;         /* epd_parjob_white ran for the update in progress */
    int w0, w1;
    /* this update */
    bool rows;           /* worked on rows [a, b] only */
    int a, b;            /* b < a: nothing to work on */
    size_t off, len;     /* the same in bytes */
    epd_wave_t wave;
    epd_wave_step_t st[3];
    int ns;
    bool any_wb, any_fresh, redriven;
    epd_fresh_info_t redrive; /* the pixels the kick drives again */
} epd_parjob_t;

/* Once, after the buffers are set (fresh all 0, the frames unknown). */
void epd_parjob_init(epd_parjob_t *j);
/* The fresh planes were written by someone else (loaded from a file): any row may be set. All cleared (a full
 * refresh): none. The library's frames were touched by someone else: they are white nowhere for sure. */
void epd_parjob_fresh_unknown(epd_parjob_t *j);
void epd_parjob_fresh_none(epd_parjob_t *j);
void epd_parjob_touched(epd_parjob_t *j);
/* Both library frames white for a new update. rows: only where the last update left them not white, when that is
 * known; else all of both. Counts as a touch: the caller writes into them next. */
void epd_parjob_white(epd_parjob_t *j, bool rows);
/* Both library frames are white everywhere now (the caller set them). */
void epd_parjob_is_white(epd_parjob_t *j);
/* The rows an update works on: [y0, y1] (the rows written; -1 -1 or y1 < y0: none), with the rows of fresh when
 * the redrive is on; whole: every row. b < a when there is nothing. */
void epd_parjob_span(int h, int y0, int y1, bool whole, bool redrive, int f0, int f1, int *a, int *b);
/* Start: the caller's frames saved to P and C, the plan of steps (st, ns), what the kick drives again. */
void epd_parjob_begin(epd_parjob_t *j, const epd_wave_t *wave, int y0, int y1, bool rows);
/* Before the scans of step i: the library's frames for it. drv: what it drives (epd_wave_drive). */
void epd_parjob_step(epd_parjob_t *j, int i, epd_wave_drive_t *drv);
/* After the last scan (ok: every step ran): the redrive counts, and both library frames to the new picture in the
 * worked rows. True if fresh changed. */
bool epd_parjob_end(epd_parjob_t *j, bool ok);

#ifdef __cplusplus
}
#endif

#endif
