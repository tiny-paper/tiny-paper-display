/* See epd_rowtime.h. C11. SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_rowtime.h"

#include <stdio.h>
#include <string.h>

bool epd_row_delay_valid(const epd_row_delay_t *d)
{
    return d && d->settle <= EPD_ROW_DELAY_MAX && d->hold <= EPD_ROW_DELAY_MAX;
}

void epd_row_stats_reset(epd_row_stats_t *s)
{
    memset(s, 0, sizeof *s);
    s->max_row = -1;
}

void epd_row_stats_start(epd_row_stats_t *s, uint32_t now_us)
{
    s->last_us = now_us;
    s->row = 0;
    s->frames++;
}

void epd_row_stats_step(epd_row_stats_t *s, uint32_t now_us)
{
    uint32_t p = now_us - s->last_us; /* wraps fine */
    if (p > s->max_us) {
        s->max_us = p;
        s->max_row = s->row;
    }
    if (p > EPD_ROW_SLOW_US) {
        s->slow++;
        int seen = -1;
        for (int i = 0; i < s->nslow_rows; i++)
            if (s->slow_rows[i] == s->row)
                seen = i;
        if (seen >= 0) {
            if (p > s->slow_us[seen])
                s->slow_us[seen] = p;
        } else if (s->nslow_rows < EPD_ROW_SLOW_KEEP) {
            s->slow_rows[s->nslow_rows] = s->row;
            s->slow_us[s->nslow_rows] = p;
            s->slow_frame[s->nslow_rows] = (uint16_t)s->frames;
            s->nslow_rows++;
        }
    }
    s->last_us = now_us;
    s->row++;
}

char *epd_row_stats_str(const epd_row_stats_t *s, char *b, size_t n)
{
    if (!n)
        return b;
    int k = snprintf(b, n, "slow=%u", (unsigned)s->slow);
    for (int i = 0; i < s->nslow_rows && k > 0 && (size_t)k < n; i++)
        k += snprintf(b + k, n - (size_t)k, "%c%d:%u@%u%s", i ? ',' : '[', s->slow_rows[i], (unsigned)s->slow_us[i],
                      (unsigned)s->slow_frame[i], s->slow_driven >> i & 1 ? "d" : "");
    if (s->nslow_rows && k > 0 && (size_t)k < n)
        snprintf(b + k, n - (size_t)k, "]");
    return b;
}

bool epd_row_drive_valid(int drive_us) { return drive_us >= 0 && drive_us <= EPD_ROW_DRIVE_MAX; }

uint32_t epd_row_drive_left(uint32_t on_us, uint32_t now_us, uint32_t drive_us)
{
    uint32_t el = now_us - on_us; /* wraps fine */
    return el < drive_us ? drive_us - el : 0;
}

void epd_row_drive_reset(epd_row_drive_stats_t *d)
{
    memset(d, 0, sizeof *d);
    d->min_us = UINT32_MAX;
}

void epd_row_drive_note(epd_row_drive_stats_t *d, uint32_t high_us)
{
    d->n++;
    d->sum_us += high_us;
    if (high_us < d->min_us)
        d->min_us = high_us;
    if (high_us > d->max_us)
        d->max_us = high_us;
    if (high_us > EPD_ROW_SLOW_US)
        d->over++;
}

uint32_t epd_row_drive_step(epd_row_drive_stats_t *d, uint32_t on_us, uint32_t now_us, uint32_t drive_us)
{
    uint32_t period = now_us - on_us;
    if (!drive_us) {
        epd_row_drive_note(d, period);
        return 0;
    }
    if (period > EPD_ROW_SLOW_US)
        d->absorbed++;
    uint32_t left = epd_row_drive_left(on_us, now_us, drive_us);
    if (left)
        d->waited++;
    return left;
}

char *epd_row_drive_str(const epd_row_drive_stats_t *d, uint32_t drive_us, char *b, size_t n)
{
    if (!n)
        return b;
    int k = snprintf(b, n, "drive=%uus high=%u/%u/%u n=%u waited=%u absorbed=%u over=%u", (unsigned)drive_us,
                     (unsigned)(d->n ? d->min_us : 0), (unsigned)(d->n ? d->sum_us / d->n : 0), (unsigned)d->max_us,
                     (unsigned)d->n, (unsigned)d->waited, (unsigned)d->absorbed, (unsigned)d->over);
    if (d->frames && k > 0 && (size_t)k < n)
        snprintf(b + k, n - (size_t)k, " ckv=%u/%u", (unsigned)d->edges, (unsigned)d->frames);
    return b;
}

bool epd_row_edges_ok(uint32_t edges, uint32_t frames, int rows, unsigned *per_frame)
{
    *per_frame = frames ? (unsigned)(edges / frames) : 0;
    if (!frames || rows <= 0)
        return false;
    unsigned want = (unsigned)rows + EPD_ROW_START_EDGES;
    return *per_frame + EPD_ROW_EDGES_SLACK >= want && *per_frame <= want + EPD_ROW_EDGES_SLACK;
}
