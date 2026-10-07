/*
 * epd_parwin.h - 1 bpp partial window math for the FastEPD glue (TRMNL X, epd_par.h, hal_esp panel_par.c).
 * Hardware-free, host-tested.
 *
 * FastEPD 8dc8c74 bbepPartialUpdate drives rows outside [iStartLine, iEndLine] as "neutral" through two
 * DMA row buffers used in turn, but only zeroes the buffer of the FIRST row of a skipped run. Every
 * second skipped row goes out with whatever the other buffer last held: the last row of the window
 * (in pass 1 below the window, in later passes above it too). On hardware (TRMNL X 2026-09-25, partial
 * rows 769..1232) that drew fine horizontal stripes from the window edge to the panel edge wherever the
 * last window row had changed pixels. So the glue asks FastEPD for every row: rows the caller left alone
 * have the same old and new pixels and compute to neutral, and each pass writes every row anyway (the
 * extra cost is the per-row diff, a few ms).
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_EPD_PARWIN_H
#define TINY_EPD_PARWIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The row range to hand to FastEPD's partial update for changed rows [y0, y1] of an h-row panel: all of it. */
void epd_par_rows(int h, int y0, int y1, int *first, int *last);

/* A window of the panel: x and w multiples of 8. */
typedef struct {
    int x, y, w, h;
} epd_win_t;

/* Copy a 1 bpp window into FastEPD's current (next pixels) and previous (old pixels) frames, both w x h at
 * ds bytes per row, 8 px per byte MSB first. The window inside the panel. Source rows are `stride` bytes
 * apart, pixel r->x at bit 7 of byte 0. all: the previous frame gets the new pixels inverted instead of `prev`,
 * so every pixel of the window is driven to its new value (the press mark's shape, issue #203 "!tp win all").
 * -1 (nothing copied) on a bad window. */
int epd_par_copy_1bpp(uint8_t *cur, uint8_t *old, size_t ds, int w, int h, const epd_win_t *r, const uint8_t *next,
                      const uint8_t *prev, size_t stride, bool all);

#ifdef __cplusplus
}
#endif

#endif
