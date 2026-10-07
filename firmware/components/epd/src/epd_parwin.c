/* See epd_parwin.h. C11. SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_parwin.h"

#include <string.h>

void epd_par_rows(int h, int y0, int y1, int *first, int *last)
{
    (void)y0;
    (void)y1;
    *first = 0;
    *last = h - 1;
}

int epd_par_copy_1bpp(uint8_t *cur, uint8_t *old, size_t ds, int w, int h, const epd_win_t *r, const uint8_t *next,
                      const uint8_t *prev, size_t stride, bool all)
{
    if (!cur || !old || !next || !prev || r->x < 0 || r->y < 0 || r->x % 8 || r->w % 8 || r->x + r->w > w ||
        r->y + r->h > h || (size_t)r->w / 8u > ds || (size_t)r->w / 8u > stride)
        return -1;
    size_t x0 = (size_t)r->x / 8u, n = (size_t)r->w / 8u;
    for (int j = 0; j < r->h; j++) {
        size_t o = (size_t)(r->y + j) * ds + x0;
        memcpy(cur + o, next + (size_t)j * stride, n);
        if (all)
            for (size_t i = 0; i < n; i++)
                old[o + i] = (uint8_t)~next[(size_t)j * stride + i];
        else
            memcpy(old + o, prev + (size_t)j * stride, n);
    }
    return 0;
}
