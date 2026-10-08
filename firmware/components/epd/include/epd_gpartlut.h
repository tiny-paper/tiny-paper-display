/*
 * epd_gpartlut.h - the codes of a gray partial update (epd_parwave.h EPD_GP_*, epd_gpart_row) from tables built once
 * a step, instead of a lookup per pixel per row per step (measured on a TRMNL X, 2026-10-08: about 160 us a row a
 * step, a window of 876 rows took 1.4 s more than one of 3). Same frames as epd_gpart_row, byte for byte
 * (test_epd_gpartlut). Hardware-free, host-tested.
 *
 * Which pixels changed does not depend on the step: one bit mask a row, computed once an update. What a changed
 * pixel gets in a step depends only on its new gray: for every value of a byte of two pixels (4 bpp, high nibble
 * first) the table has two bits "dark" and two bits "light" (bit 1 the first pixel).
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_EPD_GPARTLUT_H
#define TINY_EPD_GPARTLUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t dark[256], light[256];
} epd_gpart_lut_t;

/* The tables of one step: kind EPD_GP_DARK, EPD_GP_LIGHT or EPD_GP_PASS with pass j of the 16 x passes matrix. */
void epd_gpart_lut(int kind, int j, const uint8_t *matrix, int passes, epd_gpart_lut_t *lut);
/* The pixels of a row of w (w % 8 == 0) whose gray changed: w/8 bytes, MSB first, 1 = changed. Returns their count. */
uint32_t epd_gpart_changed(const uint8_t *old4, const uint8_t *new4, int w, uint8_t *changed);
/* One row of a step: P and C (w/8 bytes each) as epd_gpart_row writes them. Returns the pixels with a code. */
uint32_t epd_gpart_row_lut(const epd_gpart_lut_t *lut, const uint8_t *new4, const uint8_t *changed, int w, uint8_t *P,
                           uint8_t *C);

#ifdef __cplusplus
}
#endif

#endif
