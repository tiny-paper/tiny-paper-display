/* See epd_scan.h. C11. SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_scan.h"

#include <stdio.h>
#include <string.h>

bool epd_scan_cfg_valid(const epd_scan_cfg_t *c)
{
    return c && c->neutral <= EPD_SCAN_NEUTRAL_END && c->skip <= 1 && c->skip_us >= EPD_SCAN_SKIP_US_MIN &&
           c->skip_us <= EPD_SCAN_SKIP_US_MAX && c->min_ms <= EPD_SCAN_MIN_MS_MAX;
}

bool epd_scan_cfg_stock(const epd_scan_cfg_t *c) { return c->neutral == EPD_SCAN_NEUTRAL_EACH && !c->skip; }

static const char *const W_NEUTRAL[] = {"each", "end"};
static const char *const W_ONOFF[] = {"off", "on"};
static const epd_scan_key_t KEYS[EPD_SCAN_KEYS] = {
    {"step_neutral", EPD_SCAN_NEUTRAL_EACH, EPD_SCAN_NEUTRAL_END, EPD_SCAN_NEUTRAL_EACH, W_NEUTRAL},
    {"row_skip", 0, 1, 0, W_ONOFF},
    {"row_skip_us", EPD_SCAN_SKIP_US_MIN, EPD_SCAN_SKIP_US_MAX, EPD_SCAN_SKIP_US_DEFAULT, NULL},
    {"scan_min_ms", 0, EPD_SCAN_MIN_MS_MAX, 0, NULL},
};

static uint8_t *key_field(epd_scan_cfg_t *c, size_t i)
{
    return i == 0 ? &c->neutral : i == 1 ? &c->skip : i == 2 ? &c->skip_us : &c->min_ms;
}

const epd_scan_key_t *epd_scan_key(size_t i) { return i < EPD_SCAN_KEYS ? &KEYS[i] : NULL; }

int epd_scan_key_get(const epd_scan_cfg_t *c, size_t i)
{
    epd_scan_cfg_t t = *c;
    return i < EPD_SCAN_KEYS ? *key_field(&t, i) : 0;
}

bool epd_scan_key_set(epd_scan_cfg_t *c, size_t i, int v)
{
    if (i >= EPD_SCAN_KEYS || v < KEYS[i].min || v > KEYS[i].max)
        return false;
    *key_field(c, i) = (uint8_t)v;
    return true;
}

void epd_scan_key_defaults(epd_scan_cfg_t *c)
{
    for (size_t i = 0; i < EPD_SCAN_KEYS; i++)
        *key_field(c, i) = (uint8_t)KEYS[i].def;
}

void epd_scan_span(const uint8_t *prev, const uint8_t *cur, size_t n, size_t row_bytes, int *y0, int *y1)
{
    *y0 = *y1 = -1;
    if (!row_bytes)
        return;
    const size_t rows = n / row_bytes;
    size_t a = 0, b = rows;
    while (a < rows && !memcmp(prev + a * row_bytes, cur + a * row_bytes, row_bytes))
        a++;
    if (a == rows)
        return;
    while (b > a + 1 && !memcmp(prev + (b - 1) * row_bytes, cur + (b - 1) * row_bytes, row_bytes))
        b--;
    *y0 = (int)a;
    *y1 = (int)b - 1;
}

void epd_scan_union(int *y0, int *y1, int a0, int a1)
{
    if (a0 < 0 || a1 < a0)
        return;
    if (*y0 < 0 || a0 < *y0)
        *y0 = a0;
    if (*y1 < 0 || a1 > *y1)
        *y1 = a1;
}

void epd_scan_rows(int h, int y0, int y1, bool skip, epd_scan_rows_t *out)
{
    *out = (epd_scan_rows_t){0, h, 0};
    if (!skip || h <= EPD_SCAN_GUARD_AFTER + 1)
        return;
    if (y0 < 0 || y1 < y0)
        y0 = y1 = 0; /* nothing driven: the first rows carry the neutral latch */
    if (y1 >= h)
        y1 = h - 1;
    int first = y0 > EPD_SCAN_GUARD_BEFORE ? y0 - EPD_SCAN_GUARD_BEFORE : 0;
    int last = y1 + EPD_SCAN_GUARD_AFTER;
    if (last > h - 1)
        return; /* no room for the neutral rows below: the row latched last would be a driven one */
    out->lead = first;
    out->careful = last - first + 1;
    out->trail = h - 1 - last;
}

bool epd_scan_step_neutral(int neutral, int step, int steps)
{
    return neutral != EPD_SCAN_NEUTRAL_END || step + 1 >= steps;
}

int epd_scan_count(const uint8_t *frames, int steps, int neutral, bool prime)
{
    int n = prime ? 1 : 0;
    for (int i = 0; i < steps; i++)
        n += frames[i] + (epd_scan_step_neutral(neutral, i, steps) ? 1 : 0);
    return n;
}

void epd_scan_seq_init(epd_scan_seq_t *s, const epd_scan_io_t *io, void *ctx)
{
    memset(s, 0, sizeof *s);
    s->io = io;
    s->ctx = ctx;
}

void epd_scan_seq_call(epd_scan_seq_t *s, const epd_scan_rows_t *rows, int passes, bool neutral, uint32_t skip_us,
                       uint32_t min_us)
{
    s->rows = *rows;
    s->passes = (uint8_t)passes;
    s->neutral = neutral;
    s->skip_us = skip_us;
    s->min_us = min_us;
    s->frame = 0;
}

/* `n` rows clocked through: one rising edge each. keep_high: the clock stays high after the last one, as it does
 * after the last row FastEPD writes (the next scan's first start pulse is then no edge, in every kind of scan). */
static void skip_rows(epd_scan_seq_t *s, int n, bool keep_high)
{
    const epd_scan_io_t *io = s->io;
    for (int i = 0; i < n; i++) {
        io->delay_us(s->ctx, EPD_SCAN_SKIP_LOW_US);
        io->ckv(s->ctx, 1);
        io->delay_us(s->ctx, s->skip_us);
        if (!keep_high || i + 1 < n)
            io->ckv(s->ctx, 0);
    }
    s->st.skipped += (uint32_t)n;
}

void epd_scan_seq_end(epd_scan_seq_t *s)
{
    if (!s->open)
        return;
    const epd_scan_io_t *io = s->io;
    s->open = false;
    if (s->rows.trail > 0) {
        io->data_wait(s->ctx);
        io->row_step(s->ctx); /* the row written last reaches its gate; it is a neutral one (epd_scan_rows) */
        skip_rows(s, s->rows.trail, true);
    }
    if (s->min_us) {
        uint32_t el = io->now_us(s->ctx) - s->t0; /* wraps fine */
        if (el < s->min_us)
            io->delay_us(s->ctx, s->min_us - el);
    }
    s->end_us = io->now_us(s->ctx);
    s->ended = true;
}

bool epd_scan_seq_start(epd_scan_seq_t *s)
{
    const epd_scan_io_t *io = s->io;
    epd_scan_seq_end(s);
    const bool is_neutral = s->frame == s->passes;
    if (s->frame < UINT8_MAX)
        s->frame++;
    if (is_neutral && !s->neutral) {
        s->st.left++;
        return false;
    }
    s->t0 = io->now_us(s->ctx);
    if (s->ended && s->t0 - s->end_us > s->st.gap_us)
        s->st.gap_us = s->t0 - s->end_us;
    io->frame_start(s->ctx);
    s->st.frames++;
    if (s->rows.lead > 0) {
        skip_rows(s, s->rows.lead, false);
        io->resume(s->ctx, s->rows.lead);
    }
    s->open = true;
    s->st.careful++; /* the row FastEPD writes now; each further one comes with a row step */
    return true;
}

void epd_scan_seq_step(epd_scan_seq_t *s)
{
    if (!s->open)
        s->st.stray++;
    s->io->row_step(s->ctx);
    s->st.careful++;
}

bool epd_scan_stats_ok(const epd_scan_stats_t *st, int h)
{
    return h > 0 && st->frames && !st->stray && (uint64_t)st->careful + st->skipped == (uint64_t)st->frames * (uint32_t)h;
}

char *epd_scan_str(const epd_scan_cfg_t *c, bool skip, const epd_scan_stats_t *st, int h, char *b, size_t n)
{
    if (!n)
        return b;
    char sk[16] = "off";
    if (skip)
        snprintf(sk, sizeof sk, "on/%uus", (unsigned)c->skip_us);
    snprintf(b, n, "neutral=%s skip=%s frames=%u careful=%u skipped=%u left=%u gap=%uus %s",
             c->neutral == EPD_SCAN_NEUTRAL_END ? "end" : "each", sk, (unsigned)st->frames, (unsigned)st->careful,
             (unsigned)st->skipped, (unsigned)st->left, (unsigned)st->gap_us, epd_scan_stats_ok(st, h) ? "ok" : "BAD");
    return b;
}
