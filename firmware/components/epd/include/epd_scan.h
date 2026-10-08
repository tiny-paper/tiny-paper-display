/*
 * epd_scan.h - which rows a scan of a partial update drives, and the order of gate clocks that does it (TRMNL X,
 * epd_par.h, esp/epd_fastepd.cpp). Hardware-free, host-tested.
 *
 * A partial update is a list of steps; each step is one FastEPD partial: `frames` scans of all rows with the
 * step's codes, then one scan with every source output neutral (FastEPD's "clear to neutral", which ends the drive
 * of the pixels the step charged). Measured on a TRMNL X (2026-10-08): a 4 bit partial of 3 rows ran 22 scans for
 * 12 drive frames in 10 steps, 2987 ms; the time did not depend on the height of the window.
 *
 * Two things are switchable here, both off by default (the scans are then FastEPD's own, untouched):
 *   neutral   EACH: the neutral scan after every step (as before). END: only after the last step; between steps
 *             the next step's codes follow directly, as the passes of a full refresh do.
 *   skip      rows outside the driven span are clocked through with the source outputs latched neutral: one
 *             short gate clock per row and no data, as epdiy's skip rows (output_i2s/render_i2s.c i2s_skip_row:
 *             neutral rows first, then bare gate clocks; its comment gives 200 kHz as a gate driver's limit, a
 *             5 us period). FastEPD's own frame start clocks the X's gate driver with 7..10 us high and a
 *             back-to-back low, so the default skip pulse is 10 us high and 1 us low.
 *
 * The gate driver takes one clock per row, whatever the row gets: start pulses, then exactly `h` rising edges
 * (issue #203: two clocks per row drew every partial twice as tall and twice as low). A scan is therefore
 *   start, `lead` skipped rows, `careful` rows written by FastEPD with the timed row step, `trail` skipped rows
 * with lead + careful + trail == h always. FastEPD writes row i's data after gate clock i and latches it at the
 * next row step, so before the trail the sequencer runs one more row step (the last written row reaches its gate
 * as in a whole scan) and the row latched there must be neutral: the careful rows end with EPD_SCAN_GUARD_AFTER
 * rows below the driven span (the last written row is never latched when no trail follows). A span too close to
 * the last row for that is not skipped at all.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_EPD_SCAN_H
#define TINY_EPD_SCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPD_SCAN_SKIP_US_MIN     2
#define EPD_SCAN_SKIP_US_MAX     50
#define EPD_SCAN_SKIP_US_DEFAULT 10
#define EPD_SCAN_SKIP_LOW_US     1   /* gate clock low before each skipped row */
#define EPD_SCAN_MIN_MS_MAX      150 /* about one whole scan at the timed row step */
#define EPD_SCAN_GUARD_BEFORE    1   /* neutral rows written with the timed step above the driven span */
#define EPD_SCAN_GUARD_AFTER     2   /* and below it */

enum { EPD_SCAN_NEUTRAL_EACH = 0, EPD_SCAN_NEUTRAL_END = 1 };

typedef struct {
    uint8_t neutral; /* EPD_SCAN_NEUTRAL_* */
    uint8_t skip;    /* 0, 1 */
    uint8_t skip_us; /* gate clock high of a skipped row */
    uint8_t min_ms;  /* a scan lasts at least this long (0: as fast as it goes); only with skip */
} epd_scan_cfg_t;
#define EPD_SCAN_CFG_DEFAULT {EPD_SCAN_NEUTRAL_EACH, 0, EPD_SCAN_SKIP_US_DEFAULT, 0}

bool epd_scan_cfg_valid(const epd_scan_cfg_t *c);
/* The scans are FastEPD's own, as before these settings: neutral after each step and no row skipped. */
bool epd_scan_cfg_stock(const epd_scan_cfg_t *c);

/* The settings as tuning keys of the host protocol (docs/spec/usb-display.md "Tuning"; the shape of tiny/hal.h
 * hal_tune_t): step_neutral, row_skip, row_skip_us, scan_min_ms. words: NULL for a number, else the names of
 * min..max. */
#define EPD_SCAN_KEYS 4
typedef struct {
    const char *name;
    int16_t min, max, def;
    const char *const *words;
} epd_scan_key_t;
const epd_scan_key_t *epd_scan_key(size_t i); /* NULL from EPD_SCAN_KEYS on */
int epd_scan_key_get(const epd_scan_cfg_t *c, size_t i);
bool epd_scan_key_set(epd_scan_cfg_t *c, size_t i, int v); /* false: no such key or outside min..max, nothing changed */
void epd_scan_key_defaults(epd_scan_cfg_t *c);

/* The first and last row (of row_bytes bytes) where the n bytes of prev and cur differ: the rows a FastEPD partial
 * drives. -1 -1 when none. */
void epd_scan_span(const uint8_t *prev, const uint8_t *cur, size_t n, size_t row_bytes, int *y0, int *y1);
/* [y0, y1] grown to hold [a0, a1] (either may be -1 -1: nothing). */
void epd_scan_union(int *y0, int *y1, int a0, int a1);

typedef struct {
    int lead, careful, trail;
} epd_scan_rows_t;
/* The rows of a scan of an h-row panel that drives [y0, y1] (-1 -1: nothing). skip false, or the span within
 * EPD_SCAN_GUARD_AFTER rows of the last row: every row careful. */
void epd_scan_rows(int h, int y0, int y1, bool skip, epd_scan_rows_t *out);

/* Step `step` of `steps` ends with a neutral scan. */
bool epd_scan_step_neutral(int neutral, int step, int steps);
/* Scans of an update of `steps` steps of frames[i] drive frames each; prime: one more scan first (below). */
int epd_scan_count(const uint8_t *frames, int steps, int neutral, bool prime);

/* What the sequencer does to the panel. */
typedef struct {
    void (*frame_start)(void *ctx);            /* the start pulses of a scan, as every scan has them */
    void (*row_step)(void *ctx);               /* the timed row step: latch the row written last, clock high for the drive */
    void (*data_wait)(void *ctx);              /* until the row written last is in the source drivers */
    void (*ckv)(void *ctx, int level);         /* the gate clock */
    void (*delay_us)(void *ctx, uint32_t us);
    uint32_t (*now_us)(void *ctx);
    void (*resume)(void *ctx, int row);        /* rows were clocked through; `row` is written next (row statistics) */
} epd_scan_io_t;

typedef struct {
    uint32_t frames;  /* scans run */
    uint32_t careful; /* rows written with the timed row step */
    uint32_t skipped; /* rows clocked through */
    uint32_t left;    /* neutral scans left out */
    uint32_t stray;   /* row steps outside a scan: must be 0 */
    uint32_t gap_us;  /* the longest pause between the end of a FastEPD call and the next scan (step preparation) */
} epd_scan_stats_t;

typedef struct {
    const epd_scan_io_t *io;
    void *ctx;
    epd_scan_rows_t rows;
    uint32_t skip_us, min_us;
    uint8_t passes, frame;
    bool neutral, open, ended;
    uint32_t t0, end_us;
    epd_scan_stats_t st;
} epd_scan_seq_t;

/* A new update: statistics at 0. */
void epd_scan_seq_init(epd_scan_seq_t *s, const epd_scan_io_t *io, void *ctx);
/* Before each FastEPD partial: its scans' rows, its drive frames (0: only the neutral scan; with the rows of a
 * span of nothing this is the prime scan: it latches neutral into the source drivers before any row is skipped,
 * their latch being unknown after power-on), whether the neutral scan after them runs, the skip pulse and the
 * least scan time. lead or trail above 0 need a row step that leaves the clock low (the fixed drive). */
void epd_scan_seq_call(epd_scan_seq_t *s, const epd_scan_rows_t *rows, int passes, bool neutral, uint32_t skip_us,
                       uint32_t min_us);
/* FastEPD starts a scan (ROW_START). Closes the scan before it. False: this scan is left out, the caller makes
 * FastEPD write no row; else the start pulses and the lead rows are done and FastEPD writes `careful` rows. */
bool epd_scan_seq_start(epd_scan_seq_t *s);
/* FastEPD's row step (ROW_STEP) between two rows it writes. */
void epd_scan_seq_step(epd_scan_seq_t *s);
/* After the FastEPD call returned: the trail rows of its last scan. */
void epd_scan_seq_end(epd_scan_seq_t *s);

/* Every scan clocked every row once, by the sequencer's own count: careful + skipped == frames * h, no stray step. */
bool epd_scan_stats_ok(const epd_scan_stats_t *st, int h);
/* "neutral=<each|end> skip=<off|on/<us>us> frames=<n> careful=<n> skipped=<n> left=<n> gap=<us>us <ok|BAD>" into b; b.
 * skip: whether rows were skipped in this update (the setting may be on and not apply: free running rows). */
char *epd_scan_str(const epd_scan_cfg_t *c, bool skip, const epd_scan_stats_t *st, int h, char *b, size_t n);

#ifdef __cplusplus
}
#endif

#endif
