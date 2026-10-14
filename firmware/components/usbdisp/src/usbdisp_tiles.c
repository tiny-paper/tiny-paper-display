/* usbdisp: the dirty map of tiles and the clean of the tiles that are due ("Clean of the changed area" in usb-display.md).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "usbdisp_priv.h"

/* About 8 columns by 6 rows. A tile edge is a multiple of 8 px (USBDISP_ALIGN: a rectangle starts and ends on a byte at every
 * depth, and the panel takes nothing finer), so the grid is the panel's size over 8 and 6, rounded up to 8: 240 x 240 px on the
 * TRMNL X (1872 x 1404), small enough that a clock, a graph and a block of numbers are different tiles, large enough that a
 * tile's clean is a short drive (a few hundred ms) and the count stays 48 bytes. */
static uint16_t tile_edge(uint16_t len, unsigned parts)
{
    unsigned e = (len + parts - 1) / parts;
    e = (e + USBDISP_ALIGN - 1) / USBDISP_ALIGN * USBDISP_ALIGN;
    return (uint16_t)(e ? e : USBDISP_ALIGN);
}

void usbdisp_tiles_init(usbdisp_t *d)
{
    d->tile_w = tile_edge(d->c.w, 8);
    d->tile_h = tile_edge(d->c.h, 6);
    d->tile_cols = (uint8_t)((d->c.w + d->tile_w - 1) / d->tile_w);
    d->tile_rows = (uint8_t)((d->c.h + d->tile_h - 1) / d->tile_h);
    usbdisp_tiles_reset(d);
}

void usbdisp_tiles_reset(usbdisp_t *d) { memset(d->tile_cnt, 0, sizeof d->tile_cnt); }

uint64_t usbdisp_tiles_diff(const usbdisp_t *d, const uint8_t *a, const uint8_t *b, size_t stride, uint8_t depth)
{
    uint64_t mask = 0;
    const size_t tb = (size_t)d->tile_w * depth / 8;
    for (uint16_t y = 0; y < d->c.h; y++) {
        const uint8_t *pa = a + (size_t)y * stride, *pb = b + (size_t)y * stride;
        if (!memcmp(pa, pb, stride))
            continue;
        const unsigned row = y / d->tile_h;
        for (unsigned c = 0; c < d->tile_cols; c++) {
            size_t from = c * tb, len = from + tb > stride ? stride - from : tb;
            if (memcmp(pa + from, pb + from, len))
                mask |= 1ull << (row * d->tile_cols + c);
        }
    }
    return mask;
}

/* The strongest the depth takes that leaves no trace but needs no inversion: the gentlest first. passes twice drives each
 * pixel towards the value it has; inverse flips it through the opposite (a visible flash of the tile); partial_all once. */
static usbdisp_mode_t clean_mode(const usbdisp_t *d, uint8_t depth)
{
    static const usbdisp_mode_t ORDER[] = {USBDISP_MODE_PASSES, USBDISP_MODE_INVERSE, USBDISP_MODE_PARTIAL_ALL};
    const char *why;
    for (size_t i = 0; i < sizeof ORDER / sizeof *ORDER; i++)
        if (!usbdisp_mode_check(d, ORDER[i], false, depth, &why))
            return ORDER[i];
    return USBDISP_MODE_NONE;
}

static tiny_rect_t tile_rect(const usbdisp_t *d, unsigned i)
{
    unsigned c = i % d->tile_cols, r = i / d->tile_cols;
    tiny_rect_t t = {(int16_t)(c * d->tile_w), (int16_t)(r * d->tile_h), d->tile_w, d->tile_h};
    if (t.x + t.w > d->c.w)
        t.w = (uint16_t)(d->c.w - t.x);
    if (t.y + t.h > d->c.h)
        t.h = (uint16_t)(d->c.h - t.y);
    if (t.h < 2) { /* a panel takes no rectangle one row high: the row above it too */
        t.y = (int16_t)(d->c.h - 2);
        t.h = 2;
    }
    return t;
}

/* The most partial updates among the due tiles (the lowest index at a tie); -1: none is due. */
static int next_due(const usbdisp_t *d)
{
    int best = -1;
    for (unsigned i = 0; i < (unsigned)d->tile_cols * d->tile_rows; i++)
        if (d->tile_cnt[i] >= d->clean_every && (best < 0 || d->tile_cnt[i] > d->tile_cnt[best]))
            best = (int)i;
    return best;
}

int usbdisp_clean_after(usbdisp_t *d, uint8_t depth, size_t stride, bool whole_full, uint64_t mask, usbdisp_clean_t *cl,
                        size_t *n, const char **why)
{
    *n = 0;
    if (whole_full) {
        usbdisp_tiles_reset(d);
        return 0;
    }
    const unsigned tiles = (unsigned)d->tile_cols * d->tile_rows;
    for (unsigned i = 0; i < tiles; i++)
        if ((mask >> i & 1) && d->tile_cnt[i] < UINT8_MAX)
            d->tile_cnt[i]++;
    if (!d->clean_every || (d->full_every && d->since_full >= d->full_every))
        return 0; /* nothing to clean, or the full refresh is due and does the work */
    const usbdisp_mode_t m = clean_mode(d, depth);
    if (m == USBDISP_MODE_NONE) { /* left to full_every */
        for (unsigned i = 0; i < tiles; i++)
            if (d->tile_cnt[i] >= d->clean_every)
                d->tile_cnt[i] = 0;
        return 0;
    }
    const uint64_t t0 = d->c.now_us(d->c.ctx);
    int i;
    while (*n < USBDISP_CLEAN_TILES && (i = next_due(d)) >= 0) {
        if (*n && (d->c.now_us(d->c.ctx) - t0) / 1000 >= USBDISP_CLEAN_MS)
            break;
        const tiny_rect_t u = tile_rect(d, (unsigned)i);
        const size_t at = usbdisp_rect_at(&u, stride, depth);
        uint32_t ms = 0;
        int rc = 0;
        if (m == USBDISP_MODE_INVERSE) {
            rc = usbdisp_inverse(d, &u, d->prev, stride, &ms, why);
            if (rc == USBDISP_E_NOMEM)
                return 0; /* the tile stays due: tried again at the next update */
        } else {
            for (uint8_t k = 0; !rc && k < (m == USBDISP_MODE_PASSES ? 2 : 1); k++)
                rc = usbdisp_panel_area(d, depth, &u, d->prev + at, d->prev + at, stride, true, &ms, why) ? USBDISP_E_IO : 0;
        }
        if (rc)
            return rc;
        d->tile_cnt[i] = 0;
        cl[(*n)++] = (usbdisp_clean_t){u, m, ms};
    }
    return 0;
}
