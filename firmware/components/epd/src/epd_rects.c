/* See epd_rects.h. C11. SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_rects.h"

#include <stdint.h>
#include <string.h>

void epd_rects_reset(epd_rects_t *a)
{
    memset(a, 0, sizeof *a);
    a->ux0 = a->uy0 = INT16_MAX;
    a->ux1 = a->uy1 = INT16_MIN;
}

void epd_rects_add(epd_rects_t *a, const tiny_rect_t *r)
{
    if (r->x < a->ux0) a->ux0 = r->x;
    if (r->y < a->uy0) a->uy0 = r->y;
    if (r->x + r->w > a->ux1) a->ux1 = (int16_t)(r->x + r->w);
    if (r->y + r->h > a->uy1) a->uy1 = (int16_t)(r->y + r->h);
    a->writes++;
    tiny_rect_t *l = a->n ? &a->r[a->n - 1] : NULL;
    if (l && l->x == r->x && l->w == r->w && l->y + l->h == r->y) {
        l->h = (uint16_t)(l->h + r->h); /* the next band right below: no gap, one window */
    } else if (a->n < EPD_MAX_RECTS) {
        a->r[a->n++] = *r;
    } else { /* too many: grow the last one to cover this too (gap rows are written by neither: noise risk) */
        int x0 = l->x < r->x ? l->x : r->x, y0 = l->y < r->y ? l->y : r->y;
        int x1 = l->x + l->w > r->x + r->w ? l->x + l->w : r->x + r->w;
        int y1 = l->y + l->h > r->y + r->h ? l->y + l->h : r->y + r->h;
        *l = (tiny_rect_t){(int16_t)x0, (int16_t)y0, (uint16_t)(x1 - x0), (uint16_t)(y1 - y0)};
    }
}

int epd_rects_windows(const epd_rects_t *a, bool ram_valid, tiny_rect_t *out, int cap)
{
    if (!a->writes || cap < 1)
        return 0;
    if (!ram_valid && a->n > 1) {
        int n = a->n < cap ? a->n : cap;
        memcpy(out, a->r, (size_t)n * sizeof *out);
        return n;
    }
    out[0] = (tiny_rect_t){a->ux0, a->uy0, (uint16_t)(a->ux1 - a->ux0), (uint16_t)(a->uy1 - a->uy0)};
    return 1;
}
