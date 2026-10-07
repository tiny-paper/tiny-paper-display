/*
 * epd_gridcell.h - a labelled test cell for a grid of 1 bpp partial windows (issue #203: on the TRMNL X a
 * partial shows or not depending on where on the panel it is). Each cell is one window: white, a black frame
 * and its column and row number drawn large, so one photo of the glass tells which windows reached it and
 * whether x or y are mirrored. Hardware-free, host-tested.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_EPD_GRIDCELL_H
#define TINY_EPD_GRIDCELL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPD_GRID_FRAME 8    /* frame thickness, px */
#define EPD_GRID_SCALE 16   /* digit pixel size: a 3x5 glyph becomes 48x80 px */
#define EPD_GRID_MIN_W 200  /* two digits and a gap fit */
#define EPD_GRID_MIN_H 120

/* Draw cell (col, row) into a 1 bpp buffer of w x h pixels (w a multiple of 8, MSB first, 1 = white), rows
 * `stride` bytes apart: white, an EPD_GRID_FRAME px black frame, the two digits centred (col left, row right).
 * col and row 0..9. -1 (nothing drawn) when the cell is too small, w is not a multiple of 8 or a number is
 * out of range. */
int epd_grid_cell(uint8_t *buf, size_t stride, int w, int h, int col, int row);

/* 1 when pixel (x, y) of such a buffer is black. */
int epd_grid_black(const uint8_t *buf, size_t stride, int x, int y);

#ifdef __cplusplus
}
#endif

#endif
