/*
 * epd_parwave.h - 1 bpp partial waveform for the FastEPD glue (TRMNL X, epd_par.h, hal_esp panel_par.c).
 * Hardware-free, host-tested.
 *
 * FastEPD 8dc8c74 bbepPartialUpdate drives, in every one of its passes, the same code per pixel from
 * previous (P) and current (C) frames (1 = white): P=1,C=0 black, P=0,C=1 white, P==C neutral, then one
 * neutral frame. So a partial is N identical frames of plain "go black"/"go white". On the X (2026-09-25,
 * devices/trmnl-x/partial-black-passes12.jpg) pixels made black that way are not as black as pixels made
 * black by a full refresh (which shakes them with 8 dark + 8 light frames and drives the target after
 * white frames), even with 12 passes, and they are never driven again (P == C in later partials).
 *
 * The glue therefore runs a partial as up to three FastEPD partial updates (rails kept on), each with its
 * own frame count and a P'/C' pair built here:
 *   kick   k frames: new blacks (W->B) are pushed white first (DC balance and unsticking, as the full
 *          refresh's white frames before black); with redrive, earlier partial blacks still black are
 *          pushed black in the same frames (no extra time)
 *   main   n frames: FastEPD's own partial (W->B black, B->W white): the "passes"
 *   extra  x frames: new blacks only, black again (more black without over-driving the whites)
 * Each extra update costs one neutral frame on top of its frames.
 * "fresh" holds, per pixel, the redrives left for a black made by a partial (not turned white since): two
 * full-panel bit planes of n bytes each (EPD_FRESH_PLANES, low bit plane first), a count 0..3. A new black
 * starts at the budget (epd_wave_t.redrive), each partial with a redrive kick takes one off, 0 = no longer
 * driven. The glue clears it at every full refresh.
 * Why a budget (TRMNL X 2026-09-25, firmware 1134): with the bitmap kept
 * across deep sleep, every minute re-drove every partial black since the last full refresh (the day, date,
 * hour digits: hundreds of times), always black, never balanced. Each of those frames disturbs the white
 * pixels of the same rows a little at the rows where the driven load jumps (shape edges, epd_rowtime.h), and
 * a disturbance repeated hundreds of times in the same direction adds up to thin gray full-width lines at
 * the edges of those shapes. Before (0939) the bitmap was lost at every deep sleep, so on battery nothing
 * was re-driven and the lines were gone. A bounded budget re-drives each new black a few times and then
 * leaves its rows alone, as before.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_EPD_PARWAVE_H
#define TINY_EPD_PARWAVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPD_WAVE_MAX_FRAMES 14 /* per step (FastEPD setPasses limit) */
#define EPD_WAVE_REDRIVE_MAX 3 /* redrives per partial black (the fresh count is 2 bits) */
#define EPD_FRESH_PLANES 2     /* fresh: this many bit planes of n bytes each */

typedef struct {
    uint8_t kick;    /* 0..14 */
    uint8_t main;    /* 1..14 */
    uint8_t extra;   /* 0..14 */
    uint8_t redrive; /* 0..EPD_WAVE_REDRIVE_MAX: kicks that push a partial black black again (0 = off) */
} epd_wave_t;

enum { EPD_WAVE_KICK = 0, EPD_WAVE_MAIN = 1, EPD_WAVE_EXTRA = 2 };

typedef struct {
    uint8_t phase, frames;
} epd_wave_step_t;

/* Settings in range. */
bool epd_wave_valid(const epd_wave_t *w);

/* Steps for one partial, in order, into out[3]; returns their count. any_wb: some pixel goes white to
 * black; any_fresh: the fresh bitmap has a pixel set. Steps with nothing to drive are left out. */
int epd_wave_plan(const epd_wave_t *w, bool any_wb, bool any_fresh, epd_wave_step_t out[3]);

/* Frames the plan drives, neutral frames included (one per step). */
int epd_wave_frames(const epd_wave_step_t *steps, int n);

/* Scan n bytes of P/C (and fresh, EPD_FRESH_PLANES * n bytes, may be NULL): any W->B pixel, any pixel with
 * redrives left. */
void epd_wave_scan(const uint8_t *P, const uint8_t *C, const uint8_t *fresh, size_t n, bool *any_wb, bool *any_fresh);

/* FastEPD's previous/current frames for one step (n bytes each). fresh may be NULL (no redrive); redrive:
 * the kick pushes pixels with redrives left black. */
void epd_wave_masks(int phase, const uint8_t *P, const uint8_t *C, const uint8_t *fresh, bool redrive, uint8_t *prev_out,
                    uint8_t *cur_out, size_t n);

/* After the partial: if `redriven` (a kick with redrive ran), every count goes down by one; then W->B pixels
 * get `budget` (0..EPD_WAVE_REDRIVE_MAX) and B->W pixels 0. True if fresh changed. */
bool epd_wave_track(uint8_t *fresh, const uint8_t *P, const uint8_t *C, size_t n, bool redriven, uint8_t budget);

/* Pixels with redrives left and their row span ([y0, y1], -1 -1 if none), rows of row_bytes; for the log. */
typedef struct {
    uint32_t px;
    int y0, y1;
} epd_fresh_info_t;
void epd_wave_fresh_info(const uint8_t *fresh, size_t n, size_t row_bytes, epd_fresh_info_t *out);

/* Byte run-length code for keeping fresh across deep sleep (hal_esp panel_par.c): tokens of a LEB128 varint
 * (len << 1 | literal) followed by one byte (a run of len equal bytes) or len literal bytes. */
size_t epd_rle_bound(size_t n);
/* Encode n bytes into out (cap bytes); the encoded length, 0 if it does not fit. */
size_t epd_rle_enc(const uint8_t *in, size_t n, uint8_t *out, size_t cap);
/* Decode into exactly n bytes: 0, or -1 on a malformed, short or long stream. */
int epd_rle_dec(const uint8_t *in, size_t len, uint8_t *out, size_t n);

/* Gray partial (TRMNL X, "!tp gpart"): the 16-gray waveform on changed pixels only, no clearing flash, run as
 * FastEPD 1 bpp partials. Each step gives every changed pixel (old gray != new gray) a code: dark (P=1,C=0),
 * light (P=0,C=1) or none (P=C=1); unchanged pixels get none. Steps: `dark` frames all dark, `light` frames all
 * light (a clear of the changed pixels only), then the gray table's passes, pass j giving each pixel
 * matrix[gray * passes + j] (1 dark, 2 light, else none: FastEPD's codes). */
enum { EPD_GP_DARK = 0, EPD_GP_LIGHT = 1, EPD_GP_PASS = 2 };

/* One row of w pixels (w % 8 == 0): old4/new4 4 bpp (high nibble first, 0 black .. 15 white), P/C w/8 bytes
 * (1 bpp, MSB first), changed (w/8 bytes, 1 = changed) may be NULL. j: the pass for EPD_GP_PASS. */
void epd_gpart_row(int kind, int j, const uint8_t *matrix, int passes, const uint8_t *old4, const uint8_t *new4, int w,
                   uint8_t *P, uint8_t *C, uint8_t *changed);

/* Consecutive table passes with the same code for every gray run as one FastEPD partial of several frames:
 * the groups' first pass and length into first[]/len[] (room for `passes`); returns their count. */
int epd_gpart_groups(const uint8_t *matrix, int passes, uint8_t *first, uint8_t *len);

#ifdef __cplusplus
}
#endif

#endif
