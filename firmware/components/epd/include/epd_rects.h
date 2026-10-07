/*
 * epd_rects.h - written areas of one UC8179 update and the windows to refresh (epd_uc8179.c).
 * Hardware-free, host-tested. After the controller's deep sleep its RAM outside the written areas is
 * garbage: refreshing the union of separate areas showed that as a noise band on the real OG
 * (2026-09-24), so then each area gets its own window. A band right below the previous one with the
 * same x range merges into it (one window, no steps).
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_EPD_RECTS_H
#define TINY_EPD_RECTS_H

#include <stdbool.h>

#include "tiny/types.h"

#define EPD_MAX_RECTS 12

typedef struct {
    tiny_rect_t r[EPD_MAX_RECTS];
    int n;                         /* areas after merging */
    int writes;                    /* epd_rects_add calls */
    int16_t ux0, uy0, ux1, uy1;    /* union, [x0, x1) */
} epd_rects_t;

void epd_rects_reset(epd_rects_t *a);
/* Record a written area. Past EPD_MAX_RECTS the last area grows to cover it. */
void epd_rects_add(epd_rects_t *a, const tiny_rect_t *r);
/* Windows to refresh into out[] (cap >= EPD_MAX_RECTS): the union, or each area when the controller
 * RAM is not valid. Returns the count (0 when nothing was written). */
int epd_rects_windows(const epd_rects_t *a, bool ram_valid, tiny_rect_t *out, int cap);

#endif
