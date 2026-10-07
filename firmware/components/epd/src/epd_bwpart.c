/* BWRY partial window math and policy (epd_bwpart.h). C11. SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_bwpart.h"

#include <string.h>

int bwp_map(int x, int y, int w, int h, int pw, int ph, bool bottom_up, int align, bwp_win_t *out)
{
    if (align < 1 || w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > pw || y + h > ph || x % align || w % align)
        return BWP_E_ARG;
    out->x0 = (uint16_t)x;
    out->x1 = (uint16_t)(x + w - 1);
    out->y0 = (uint16_t)(bottom_up ? ph - (y + h) : y);
    out->y1 = (uint16_t)(bottom_up ? ph - 1 - y : y + h - 1);
    return BWP_OK;
}

static void be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

void bwp_ptlw_bytes(const bwp_win_t *w, uint8_t pmode, uint8_t out[9])
{
    be16(out, w->x0);
    be16(out + 2, w->x1);
    be16(out + 4, w->y0);
    be16(out + 6, w->y1);
    out[8] = pmode;
}

void bwp_ptl_bytes(const bwp_win_t *w, uint8_t out[9])
{
    bwp_ptlw_bytes(w, 0x01, out);
}

void bwp_acc_reset(bwp_acc_t *a)
{
    memset(a, 0, sizeof *a);
}

int bwp_acc_add(bwp_acc_t *a, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0 || y < 0 || y + h > BWP_MAX_ROWS)
        return BWP_E_ARG;
    if (a->rows && (x != a->x || w != a->w))
        return BWP_E_ORDER;
    for (int r = y; r < y + h; r++)
        if (a->seen[r / 32] & (1u << (r % 32)))
            return BWP_E_ORDER;
    for (int r = y; r < y + h; r++)
        a->seen[r / 32] |= 1u << (r % 32);
    if (!a->rows) {
        a->x = x;
        a->w = w;
        a->y0 = y;
        a->y1 = y + h;
    } else {
        if (y < a->y0)
            a->y0 = y;
        if (y + h > a->y1)
            a->y1 = y + h;
    }
    a->rows += h;
    return BWP_OK;
}

bool bwp_acc_solid(const bwp_acc_t *a)
{
    return a->rows > 0 && a->rows == a->y1 - a->y0;
}

int bwp_boxes_window(const bwp_rect_t *r, int n, int pw, int ph, bwp_rect_t *win)
{
    if (n < 1 || n > BWP_MAX_BOXES)
        return BWP_E_ARG;
    int x0 = pw, y0 = ph, x1 = 0, y1 = 0;
    for (int i = 0; i < n; i++) {
        const bwp_rect_t *b = &r[i];
        if (b->w <= 0 || b->h <= 0 || b->x < 0 || b->y < 0 || b->x + b->w > pw || b->y + b->h > ph || b->x % 8 ||
            b->w % 8)
            return BWP_E_ARG;
        x0 = b->x < x0 ? b->x : x0;
        y0 = b->y < y0 ? b->y : y0;
        x1 = b->x + b->w > x1 ? b->x + b->w : x1;
        y1 = b->y + b->h > y1 ? b->y + b->h : y1;
    }
    *win = (bwp_rect_t){x0, y0, x1 - x0, y1 - y0};
    /* every row of the union: the boxes on it must cover [x0, x1) (<= 8 intervals: sweep from x0) */
    for (int y = y0; y < y1; y++) {
        int at = x0;
        for (bool grew = true; grew && at < x1;) {
            grew = false;
            for (int i = 0; i < n; i++)
                if (y >= r[i].y && y < r[i].y + r[i].h && r[i].x <= at && r[i].x + r[i].w > at) {
                    at = r[i].x + r[i].w;
                    grew = true;
                }
        }
        if (at < x1)
            return BWP_E_GAP;
    }
    return BWP_OK;
}

bool bwp_boxes_allowed(uint8_t every, const bwp_state_t *s)
{
    return !s->need_full && (!every || s->since_full < every);
}

bool bwp_allowed(const bwp_cfg_t *c, const bwp_state_t *s)
{
    if (!c->mode || s->need_full)
        return false;
    return !c->every || s->since_full < c->every;
}

void bwp_note(bwp_state_t *s, bool full, bool ok)
{
    if (!ok) {
        s->need_full = true;
        return;
    }
    if (full) {
        s->since_full = 0;
        s->need_full = false;
    } else if (s->since_full < UINT16_MAX) {
        s->since_full++;
    }
}
