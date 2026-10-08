/* See epd_parjob.h. C11. SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_parjob.h"

#include <string.h>

void epd_parjob_init(epd_parjob_t *j)
{
    j->f0 = j->f1 = -1;
    j->white_ok = j->whited = false;
    j->w0 = j->w1 = -1;
    j->a = 0;
    j->b = -1;
    j->off = j->len = 0;
    j->ns = 0;
}

void epd_parjob_fresh_unknown(epd_parjob_t *j)
{
    j->f0 = 0;
    j->f1 = j->h - 1;
}

void epd_parjob_fresh_none(epd_parjob_t *j) { j->f0 = j->f1 = -1; }

void epd_parjob_touched(epd_parjob_t *j) { j->white_ok = j->whited = false; }

void epd_parjob_is_white(epd_parjob_t *j)
{
    j->whited = false;
    j->white_ok = true;
    j->w0 = j->w1 = -1;
}

void epd_parjob_white(epd_parjob_t *j, bool rows)
{
    if (rows && j->white_ok) {
        if (j->w1 >= j->w0 && j->w0 >= 0) {
            size_t off = (size_t)j->w0 * j->rb, len = (size_t)(j->w1 - j->w0 + 1) * j->rb;
            memset(j->prev + off, 0xff, len);
            memset(j->cur + off, 0xff, len);
        }
    } else {
        memset(j->prev, 0xff, j->n);
        memset(j->cur, 0xff, j->n);
    }
    j->white_ok = false; /* the caller writes its rows now; epd_parjob_end knows them again */
    j->whited = true;
}

void epd_parjob_span(int h, int y0, int y1, bool whole, bool redrive, int f0, int f1, int *a, int *b)
{
    if (whole) {
        *a = 0;
        *b = h - 1;
        return;
    }
    int lo = -1, hi = -1;
    if (y0 >= 0 && y1 >= y0)
        lo = y0, hi = y1;
    if (redrive && f0 >= 0 && f1 >= f0) {
        lo = lo < 0 || f0 < lo ? f0 : lo;
        hi = f1 > hi ? f1 : hi;
    }
    if (hi > h - 1)
        hi = h - 1;
    if (lo < 0 || hi < lo) {
        *a = 0;
        *b = -1;
        return;
    }
    *a = lo;
    *b = hi;
}

void epd_parjob_begin(epd_parjob_t *j, const epd_wave_t *wave, int y0, int y1, bool rows)
{
    j->wave = *wave;
    j->rows = rows;
    epd_parjob_span(j->h, y0, y1, !rows, wave->redrive != 0, j->f0, j->f1, &j->a, &j->b);
    j->off = j->b < j->a ? 0 : (size_t)j->a * j->rb;
    j->len = j->b < j->a ? 0 : (size_t)(j->b - j->a + 1) * j->rb;
    memcpy(j->P + j->off, j->prev + j->off, j->len);
    memcpy(j->C + j->off, j->cur + j->off, j->len);
    epd_wave_scan_at(j->P, j->C, j->fresh, j->n, j->off, j->len, &j->any_wb, &j->any_fresh);
    j->ns = epd_wave_plan(&j->wave, j->any_wb, j->any_fresh, j->st);
    j->redriven = j->wave.redrive && j->any_fresh && j->ns && j->st[0].phase == EPD_WAVE_KICK;
    epd_wave_fresh_info_at(j->redriven ? j->fresh : NULL, j->n, j->rb, j->off, j->len, &j->redrive);
}

void epd_parjob_step(epd_parjob_t *j, int i, epd_wave_drive_t *drv)
{
    /* the library copies current into previous as it scans, so every step builds both from the saved frames */
    epd_wave_masks_at(j->st[i].phase, j->P, j->C, j->fresh, j->wave.redrive, j->prev, j->cur, j->n, j->off, j->len);
    if (drv)
        epd_wave_drive(j->P + j->off, j->C + j->off, j->prev + j->off, j->cur + j->off, j->len, drv);
}

bool epd_parjob_end(epd_parjob_t *j, bool ok)
{
    bool ch = ok && epd_wave_track_at(j->fresh, j->P, j->C, j->n, j->off, j->len, j->redriven, j->wave.redrive);
    /* the rows where fresh may be set now: those of the worked rows that hold one, with the old ones unless the
     * worked rows covered them */
    epd_fresh_info_t f;
    epd_wave_fresh_info_at(j->fresh, j->n, j->rb, j->off, j->len, &f);
    const bool covered = j->f0 < 0 || (j->len && j->a <= j->f0 && j->b >= j->f1);
    if (covered) {
        j->f0 = f.y0;
        j->f1 = f.y1;
    } else if (f.y0 >= 0) {
        j->f0 = f.y0 < j->f0 ? f.y0 : j->f0;
        j->f1 = f.y1 > j->f1 ? f.y1 : j->f1;
    }
    memcpy(j->cur + j->off, j->C + j->off, j->len);
    memcpy(j->prev + j->off, j->C + j->off, j->len);
    /* outside the worked rows the frames are as epd_parjob_white left them (the caller wrote inside [y0, y1] only) */
    j->white_ok = j->whited; /* an update that did not start from white frames says nothing about them */
    j->whited = false;
    j->w0 = j->len ? j->a : -1;
    j->w1 = j->len ? j->b : -1;
    return ch;
}
