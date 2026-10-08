/* The scan plan of a TRMNL X partial update and its gate clock sequence (epd_scan.h), against a model of the panel:
 * a gate driver that moves one row per rising clock edge, source drivers that show what was latched last, and
 * FastEPD's row loop (write a row after its gate clock, latch it at the next row step). The guard is the fault of
 * issue #203, a row clocked twice or not at all, and its neighbours: a row that gets another row's data, a skipped
 * row that sees anything but neutral. SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "epd_rowtime.h"
#include "epd_scan.h"
#include "unity.h"

#define HMAX 1404
#define FMAX 48
#define EXMAX 4
enum { NEUTRAL = -1, POISON = -2 }; /* what the source drivers show: a row's data (its number), neutral, unknown */

static struct {
    int h, ckv, gate, latched, sent;
    bool in_rows;
    uint32_t edges, now, delays, waits, resumes;
    int resume_row;
    int frames;                    /* scans started */
    bool neutral_frame[FMAX];      /* set by the FastEPD model */
    uint16_t clocks[HMAX];         /* of the scan in progress */
    int bad_clocks, overflow;      /* gates of finished scans with a count other than 1; clocks past the last gate */
    int8_t nex[FMAX][64];          /* per scan and gate (small panels only): what the gate was shown, repeats folded */
    int16_t ex[FMAX][64][EXMAX];
    char trace[256];
    size_t ntrace;
} M;
static epd_scan_seq_t S;

static void tr(char c)
{
    if (M.ntrace + 1 < sizeof M.trace)
        M.trace[M.ntrace++] = c;
}

static void expose(int g, int d)
{
    if (M.h > 64 || M.frames < 1 || M.frames > FMAX)
        return;
    int f = M.frames - 1;
    int8_t *n = &M.nex[f][g];
    if (*n && M.ex[f][g][*n - 1] == d)
        return;
    TEST_ASSERT_TRUE_MESSAGE(*n < EXMAX, "a gate was shown more kinds of data than the model keeps");
    M.ex[f][g][(*n)++] = (int16_t)d;
}

static void pin_ckv(int level)
{
    if (level && !M.ckv) {
        M.edges++;
        if (M.in_rows) {
            M.gate++;
            if (M.gate < M.h) {
                M.clocks[M.gate]++;
                expose(M.gate, M.latched);
            } else {
                M.overflow++;
            }
        }
    }
    M.ckv = level;
}

static void frame_close(void)
{
    if (!M.in_rows)
        return;
    for (int g = 0; g < M.h; g++)
        if (M.clocks[g] != 1)
            M.bad_clocks++;
    M.in_rows = false;
}

/* The firmware's frame start with the fixed drive (esp/epd_fastepd.cpp frame_start): four clock pulses, then the
 * drive pulse. The first pulse is no edge when the clock was left high by the scan before. */
static void io_frame_start(void *ctx)
{
    (void)ctx;
    tr('S');
    frame_close();
    for (int i = 0; i < 4; i++) {
        pin_ckv(1);
        pin_ckv(0);
    }
    pin_ckv(1);
    pin_ckv(0);
    M.frames++;
    memset(M.clocks, 0, sizeof M.clocks);
    M.gate = -1;
    M.in_rows = true;
}

/* The firmware's row step with the fixed drive: latch, then the clock high for the drive time and low. */
static void io_row_step(void *ctx)
{
    (void)ctx;
    tr('r');
    M.latched = M.sent;
    if (M.ckv && M.in_rows && M.gate >= 0 && M.gate < M.h)
        expose(M.gate, M.latched);
    pin_ckv(1);
    M.now += 32;
    pin_ckv(0);
}

static void io_data_wait(void *ctx) { (void)ctx; tr('w'); M.waits++; }
static void io_ckv(void *ctx, int level) { (void)ctx; tr('k'); pin_ckv(level); }
static void io_delay(void *ctx, uint32_t us) { (void)ctx; tr('d'); M.now += us; M.delays += us; }
static uint32_t io_now(void *ctx) { (void)ctx; return M.now; }
static void io_resume(void *ctx, int row) { (void)ctx; tr('m'); M.resumes++; M.resume_row = row; }
static const epd_scan_io_t IO = {io_frame_start, io_row_step, io_data_wait, io_ckv, io_delay, io_now, io_resume};

static void model_reset(int h)
{
    memset(&M, 0, sizeof M);
    M.h = h;
    M.latched = POISON; /* the source drivers after power-on */
    M.sent = NEUTRAL;
    M.now = 0xffffff00u; /* the clock wraps inside the run */
    epd_scan_seq_init(&S, &IO, NULL);
}

/* FastEPD 8dc8c74 bbepPartialUpdate, as the glue calls it: `passes` scans of the rows it was given with their
 * codes, then bbepClear's scan with neutral rows. bbepWriteRow: (row step,) clock on, the row's data out. A start
 * the sequencer refuses writes no row (the glue sets FastEPD's row count to 0 for that scan). */
static void fastepd_partial(int passes, const uint8_t *driven)
{
    for (int f = 0; f <= passes; f++) {
        if (!epd_scan_seq_start(&S))
            continue;
        if (M.frames <= FMAX)
            M.neutral_frame[M.frames - 1] = f == passes;
        for (int i = 0; i < S.rows.careful; i++) {
            if (i)
                epd_scan_seq_step(&S);
            pin_ckv(1);
            M.now += 40;
            int row = S.rows.lead + i;
            M.sent = f < passes && driven[row] ? row : NEUTRAL;
        }
    }
    epd_scan_seq_end(&S);
}

static void span_of(const uint8_t *driven, int h, int *y0, int *y1)
{
    *y0 = *y1 = -1;
    for (int y = 0; y < h; y++)
        if (driven[y]) {
            if (*y0 < 0)
                *y0 = y;
            *y1 = y;
        }
}

/* One update the way esp/epd_fastepd.cpp runs it. */
static void run_update(int h, const uint8_t *driven, const uint8_t *frames, int steps, int neutral, bool skip)
{
    model_reset(h);
    int y0, y1;
    span_of(driven, h, &y0, &y1);
    epd_scan_rows_t r;
    if (skip) {
        epd_scan_rows(h, -1, -1, true, &r);
        epd_scan_seq_call(&S, &r, 0, true, 10, 0);
        fastepd_partial(0, driven);
    }
    for (int i = 0; i < steps; i++) {
        epd_scan_rows(h, y0, y1, skip, &r);
        epd_scan_seq_call(&S, &r, frames[i], epd_scan_step_neutral(neutral, i, steps), 10, 0);
        fastepd_partial(frames[i], driven);
    }
    frame_close();
}

/* What every update must show, whatever its plan: each scan clocked each gate once, the edge count is the one the
 * device's own check wants (epd_row_edges_ok: rows + 4 a scan), and the sequencer's count agrees. */
static void check_clocks(int h, const uint8_t *frames, int steps, int neutral, bool skip)
{
    unsigned per = 0;
    TEST_ASSERT_EQUAL_INT(epd_scan_count(frames, steps, neutral, skip), M.frames);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, M.bad_clocks, "a gate was clocked twice or not at all in some scan");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, M.overflow, "clocks past the last gate");
    /* the first scan's first start pulse is an edge, the later ones find the clock high: the device's count */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)M.frames * (uint32_t)(h + EPD_ROW_START_EDGES) + 1, M.edges);
    TEST_ASSERT_TRUE(epd_row_edges_ok(M.edges, (uint32_t)M.frames, h, &per));
    TEST_ASSERT_EQUAL_UINT(h + EPD_ROW_START_EDGES, per);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)M.frames, S.st.frames);
    TEST_ASSERT_TRUE(epd_scan_stats_ok(&S.st, h));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)M.frames * (uint32_t)h, S.st.careful + S.st.skipped);
}

void setUp(void) {}
void tearDown(void) {}

static void test_defaults_are_the_scans_as_before(void)
{
    epd_scan_cfg_t c = EPD_SCAN_CFG_DEFAULT;
    TEST_ASSERT_TRUE(epd_scan_cfg_valid(&c));
    TEST_ASSERT_TRUE(epd_scan_cfg_stock(&c));
    TEST_ASSERT_EQUAL_UINT8(EPD_SCAN_NEUTRAL_EACH, c.neutral);
    TEST_ASSERT_EQUAL_UINT8(0, c.skip);
    TEST_ASSERT_EQUAL_UINT8(0, c.min_ms);
    c.skip = 1;
    TEST_ASSERT_FALSE(epd_scan_cfg_stock(&c));
    c = (epd_scan_cfg_t)EPD_SCAN_CFG_DEFAULT;
    c.neutral = EPD_SCAN_NEUTRAL_END;
    TEST_ASSERT_FALSE(epd_scan_cfg_stock(&c));
}

static void test_bounds(void)
{
    epd_scan_cfg_t c = {EPD_SCAN_NEUTRAL_END, 1, EPD_SCAN_SKIP_US_MIN, EPD_SCAN_MIN_MS_MAX};
    TEST_ASSERT_TRUE(epd_scan_cfg_valid(&c));
    c.skip_us = EPD_SCAN_SKIP_US_MAX;
    TEST_ASSERT_TRUE(epd_scan_cfg_valid(&c));
    c.skip_us = EPD_SCAN_SKIP_US_MAX + 1;
    TEST_ASSERT_FALSE(epd_scan_cfg_valid(&c));
    c.skip_us = EPD_SCAN_SKIP_US_MIN - 1;
    TEST_ASSERT_FALSE(epd_scan_cfg_valid(&c));
    c = (epd_scan_cfg_t){2, 0, 10, 0};
    TEST_ASSERT_FALSE(epd_scan_cfg_valid(&c));
    c = (epd_scan_cfg_t){0, 2, 10, 0};
    TEST_ASSERT_FALSE(epd_scan_cfg_valid(&c));
    c = (epd_scan_cfg_t){0, 1, 10, EPD_SCAN_MIN_MS_MAX + 1};
    TEST_ASSERT_FALSE(epd_scan_cfg_valid(&c));
    TEST_ASSERT_FALSE(epd_scan_cfg_valid(NULL));
}

/* The four tuning keys: names, words, bounds, and defaults that are the scans as before. */
static void test_keys(void)
{
    epd_scan_cfg_t c = {EPD_SCAN_NEUTRAL_END, 1, 33, 77};
    TEST_ASSERT_EQUAL_STRING("step_neutral", epd_scan_key(0)->name);
    TEST_ASSERT_EQUAL_STRING("each", epd_scan_key(0)->words[0]);
    TEST_ASSERT_EQUAL_STRING("end", epd_scan_key(0)->words[1]);
    TEST_ASSERT_EQUAL_STRING("row_skip", epd_scan_key(1)->name);
    TEST_ASSERT_EQUAL_STRING("off", epd_scan_key(1)->words[0]);
    TEST_ASSERT_EQUAL_STRING("on", epd_scan_key(1)->words[1]);
    TEST_ASSERT_EQUAL_STRING("row_skip_us", epd_scan_key(2)->name);
    TEST_ASSERT_NULL(epd_scan_key(2)->words);
    TEST_ASSERT_EQUAL_STRING("scan_min_ms", epd_scan_key(3)->name);
    TEST_ASSERT_NULL(epd_scan_key(EPD_SCAN_KEYS));
    TEST_ASSERT_EQUAL_INT(1, epd_scan_key_get(&c, 0));
    TEST_ASSERT_EQUAL_INT(1, epd_scan_key_get(&c, 1));
    TEST_ASSERT_EQUAL_INT(33, epd_scan_key_get(&c, 2));
    TEST_ASSERT_EQUAL_INT(77, epd_scan_key_get(&c, 3));
    for (size_t i = 0; i < EPD_SCAN_KEYS; i++) { /* each key: its ends taken, one past them refused and nothing changed */
        const epd_scan_key_t *k = epd_scan_key(i);
        TEST_ASSERT_TRUE(epd_scan_key_set(&c, i, k->min));
        TEST_ASSERT_TRUE(epd_scan_cfg_valid(&c));
        TEST_ASSERT_TRUE(epd_scan_key_set(&c, i, k->max));
        TEST_ASSERT_TRUE(epd_scan_cfg_valid(&c));
        TEST_ASSERT_FALSE(epd_scan_key_set(&c, i, k->max + 1));
        TEST_ASSERT_FALSE(epd_scan_key_set(&c, i, k->min - 1));
        TEST_ASSERT_EQUAL_INT(k->max, epd_scan_key_get(&c, i));
    }
    TEST_ASSERT_FALSE(epd_scan_key_set(&c, EPD_SCAN_KEYS, 0));
    epd_scan_key_defaults(&c);
    const epd_scan_cfg_t def = EPD_SCAN_CFG_DEFAULT;
    TEST_ASSERT_EQUAL_MEMORY(&def, &c, sizeof c);
    TEST_ASSERT_TRUE(epd_scan_cfg_stock(&c)); /* the defaults leave the scans as they were */
    TEST_ASSERT_EQUAL_INT(2, epd_scan_key(2)->min);
    TEST_ASSERT_EQUAL_INT(50, epd_scan_key(2)->max);
    TEST_ASSERT_EQUAL_INT(10, epd_scan_key(2)->def);
    TEST_ASSERT_EQUAL_INT(150, epd_scan_key(3)->max);
}

static void test_span_is_the_rows_that_differ(void)
{
    enum { RB = 4, ROWS = 10 };
    uint8_t p[RB * ROWS], c[RB * ROWS];
    int y0, y1;
    memset(p, 0xff, sizeof p);
    memset(c, 0xff, sizeof c);
    epd_scan_span(p, c, sizeof p, RB, &y0, &y1);
    TEST_ASSERT_EQUAL_INT(-1, y0);
    TEST_ASSERT_EQUAL_INT(-1, y1);
    c[5 * RB + 3] = 0xfe; /* one pixel, the last byte of row 5 */
    epd_scan_span(p, c, sizeof p, RB, &y0, &y1);
    TEST_ASSERT_EQUAL_INT(5, y0);
    TEST_ASSERT_EQUAL_INT(5, y1);
    p[2 * RB] = 0x7f; /* a second window above: the span is their union, the rows between included */
    epd_scan_span(p, c, sizeof p, RB, &y0, &y1);
    TEST_ASSERT_EQUAL_INT(2, y0);
    TEST_ASSERT_EQUAL_INT(5, y1);
    c[0] = 0;
    c[sizeof c - 1] = 0; /* row 0 and the last row */
    epd_scan_span(p, c, sizeof p, RB, &y0, &y1);
    TEST_ASSERT_EQUAL_INT(0, y0);
    TEST_ASSERT_EQUAL_INT(ROWS - 1, y1);
    epd_scan_span(p, c, sizeof p, 0, &y0, &y1);
    TEST_ASSERT_EQUAL_INT(-1, y0);
}

static void test_union_of_spans(void)
{
    int y0 = -1, y1 = -1;
    epd_scan_union(&y0, &y1, -1, -1);
    TEST_ASSERT_EQUAL_INT(-1, y0);
    epd_scan_union(&y0, &y1, 700, 720);
    epd_scan_union(&y0, &y1, 100, 110);
    epd_scan_union(&y0, &y1, 705, 900);
    epd_scan_union(&y0, &y1, -1, -1);
    TEST_ASSERT_EQUAL_INT(100, y0);
    TEST_ASSERT_EQUAL_INT(900, y1);
}

static void rows_are(int h, int y0, int y1, bool skip, int lead, int careful, int trail)
{
    epd_scan_rows_t r;
    epd_scan_rows(h, y0, y1, skip, &r);
    TEST_ASSERT_EQUAL_INT(lead, r.lead);
    TEST_ASSERT_EQUAL_INT(careful, r.careful);
    TEST_ASSERT_EQUAL_INT(trail, r.trail);
}

static void test_rows_of_a_scan(void)
{
    rows_are(1404, 771, 773, false, 0, 1404, 0);  /* the setting off: every row careful, as before */
    rows_are(1404, 771, 773, true, 770, 6, 628);  /* a 3 row line: one row above, two below */
    rows_are(1404, 0, 2, true, 0, 5, 1399);       /* a window at row 0: nothing above to skip */
    rows_are(1404, 100, 900, true, 99, 804, 501); /* two windows 100..110 and 700..900: one span */
    rows_are(1404, 1390, 1401, true, 1389, 15, 0); /* the two rows below are the panel's last */
    rows_are(1404, 1390, 1402, true, 0, 1404, 0); /* no room for them: not skipped at all */
    rows_are(1404, 1400, 1403, true, 0, 1404, 0); /* a window at the last row: as before */
    rows_are(1404, 0, 1403, true, 0, 1404, 0);    /* the whole panel as a window: as before */
    rows_are(1404, -1, -1, true, 0, 3, 1401);     /* nothing driven (and the prime scan) */
    rows_are(1404, 5, 9000, true, 0, 1404, 0);    /* past the panel: clipped, so not skipped */
    rows_are(3, 0, 0, true, 0, 3, 0);
    for (int y0 = 0; y0 < 12; y0++) /* every span of a small panel: the rows always add up */
        for (int y1 = y0; y1 < 12; y1++) {
            epd_scan_rows_t r;
            epd_scan_rows(12, y0, y1, true, &r);
            TEST_ASSERT_EQUAL_INT(12, r.lead + r.careful + r.trail);
            TEST_ASSERT_TRUE(r.lead <= y0 && r.lead + r.careful > y1 && r.careful >= 1);
            if (r.lead || r.trail) /* skipped: the two neutral rows below are in */
                TEST_ASSERT_TRUE(r.lead + r.careful - 1 >= y1 + EPD_SCAN_GUARD_AFTER);
        }
}

static void test_scans_of_an_update(void)
{
    /* the measured gray partial: 12 drive frames in 10 steps, 22 scans */
    const uint8_t gray[10] = {1, 2, 1, 1, 1, 1, 2, 1, 1, 1};
    TEST_ASSERT_EQUAL_INT(22, epd_scan_count(gray, 10, EPD_SCAN_NEUTRAL_EACH, false));
    TEST_ASSERT_EQUAL_INT(13, epd_scan_count(gray, 10, EPD_SCAN_NEUTRAL_END, false));
    TEST_ASSERT_EQUAL_INT(23, epd_scan_count(gray, 10, EPD_SCAN_NEUTRAL_EACH, true));
    TEST_ASSERT_EQUAL_INT(14, epd_scan_count(gray, 10, EPD_SCAN_NEUTRAL_END, true));
    const uint8_t bw[2] = {2, 4}; /* 1 bit: kick 2, main 4 */
    TEST_ASSERT_EQUAL_INT(8, epd_scan_count(bw, 2, EPD_SCAN_NEUTRAL_EACH, false));
    TEST_ASSERT_EQUAL_INT(7, epd_scan_count(bw, 2, EPD_SCAN_NEUTRAL_END, false));
    TEST_ASSERT_EQUAL_INT(0, epd_scan_count(bw, 0, EPD_SCAN_NEUTRAL_END, false));
    TEST_ASSERT_TRUE(epd_scan_step_neutral(EPD_SCAN_NEUTRAL_EACH, 0, 3));
    TEST_ASSERT_FALSE(epd_scan_step_neutral(EPD_SCAN_NEUTRAL_END, 1, 3));
    TEST_ASSERT_TRUE(epd_scan_step_neutral(EPD_SCAN_NEUTRAL_END, 2, 3));
}

/* Every row careful and a neutral scan after the step: the sequencer does the frame start and the row steps and
 * nothing else, which is what FastEPD's own scans do. */
static void test_the_whole_panel_is_todays_sequence(void)
{
    enum { H = 6 };
    uint8_t driven[H] = {0, 1, 1, 1, 1, 1};
    const uint8_t frames[1] = {2};
    run_update(H, driven, frames, 1, EPD_SCAN_NEUTRAL_EACH, false);
    M.trace[M.ntrace] = 0;
    TEST_ASSERT_EQUAL_STRING("SrrrrrSrrrrrSrrrrr", M.trace);
    TEST_ASSERT_EQUAL_UINT32(0, S.st.skipped);
    TEST_ASSERT_EQUAL_UINT32(3 * H, S.st.careful);
    TEST_ASSERT_EQUAL_UINT32(0, M.delays);
    check_clocks(H, frames, 1, EPD_SCAN_NEUTRAL_EACH, false);
    /* the same with the span at the last row and skipping on */
    run_update(H, driven, frames, 1, EPD_SCAN_NEUTRAL_EACH, true);
    TEST_ASSERT_EQUAL_UINT32(3 * H, S.st.careful + S.st.skipped - (uint32_t)H); /* less the prime scan */
}

/* The drive scans of a skipping update show every gate what the same scans show it without skipping. */
static void same_picture_as_without_skipping(int h, const uint8_t *driven, int neutral)
{
    static int8_t nex[FMAX][64];
    static int16_t ex[FMAX][64][EXMAX];
    static bool neu[FMAX];
    const uint8_t frames[3] = {2, 1, 3};
    run_update(h, driven, frames, 3, EPD_SCAN_NEUTRAL_EACH, false);
    check_clocks(h, frames, 3, EPD_SCAN_NEUTRAL_EACH, false);
    memcpy(nex, M.nex, sizeof nex);
    memcpy(ex, M.ex, sizeof ex);
    memcpy(neu, M.neutral_frame, sizeof neu);
    int nref = M.frames;
    run_update(h, driven, frames, 3, neutral, true);
    check_clocks(h, frames, 3, neutral, true);
    /* the prime scan: only the first gate sees the unknown latch, as the first gate of any first scan does */
    for (int g = 1; g < h; g++) {
        TEST_ASSERT_EQUAL_INT8(1, M.nex[0][g]);
        TEST_ASSERT_EQUAL_INT16(NEUTRAL, M.ex[0][g][0]);
    }
    int a = 0, b = 1, drive = 0;
    for (;; a++, b++) {
        while (a < nref && neu[a])
            a++;
        while (b < M.frames && M.neutral_frame[b])
            b++;
        if (a >= nref || b >= M.frames)
            break;
        drive++;
        for (int g = 0; g < h; g++) {
            /* what the gate ends on, and every row's data it was shown on the way. The first gate of a scan is
             * first shown the latch the scan before left (without skipping: unknown in the update's first scan,
             * else the last but one row of the scan before): that one is not the picture and is left out here;
             * below it must be neutral wherever rows are skipped */
            int16_t la[EXMAX], lb[EXMAX];
            int na = 0, nb = 0;
            for (int i = 0; i < nex[a][g]; i++)
                if (ex[a][g][i] != NEUTRAL && !(g == 0 && i == 0 && ex[a][g][i] != 0))
                    la[na++] = ex[a][g][i];
            for (int i = 0; i < M.nex[b][g]; i++)
                if (M.ex[b][g][i] != NEUTRAL && !(g == 0 && i == 0 && M.ex[b][g][i] != 0))
                    lb[nb++] = M.ex[b][g][i];
            TEST_ASSERT_EQUAL_INT_MESSAGE(na, nb, "a gate was shown more or less than without skipping");
            for (int i = 0; i < na; i++)
                TEST_ASSERT_EQUAL_INT16_MESSAGE(la[i], lb[i], "a gate was shown another row's data");
            TEST_ASSERT_TRUE(nex[a][g] > 0 && M.nex[b][g] > 0);
            TEST_ASSERT_EQUAL_INT16_MESSAGE(ex[a][g][nex[a][g] - 1], M.ex[b][g][M.nex[b][g] - 1], "a gate ends on other data");
        }
    }
    TEST_ASSERT_EQUAL_INT(6, drive);
    TEST_ASSERT_TRUE(a >= nref && b >= M.frames); /* both ran the same drive scans */
    /* the update ends with a neutral scan that leaves every gate neutral */
    TEST_ASSERT_TRUE(M.neutral_frame[M.frames - 1]);
    for (int g = 0; g < h; g++)
        TEST_ASSERT_EQUAL_INT16(NEUTRAL, M.ex[M.frames - 1][g][M.nex[M.frames - 1][g] - 1]);
    /* no drive scan showed a gate outside the driven rows (and the row below the last, FastEPD's pipeline)
     * anything but neutral */
    for (int f = 1; f < M.frames; f++)
        for (int g = 0; g < h; g++)
            for (int i = 0; i < M.nex[f][g]; i++) {
                int d = M.ex[f][g][i];
                if (g == 0 && i == 0 && !S.rows.lead && !S.rows.trail)
                    continue; /* not skipped: the first gate's stale latch, as without skipping */
                TEST_ASSERT_TRUE_MESSAGE(d == NEUTRAL || d == g || d == g - 1, "a row's data on a gate far from it");
            }
}

static void test_skipping_shows_each_gate_the_same(void)
{
    enum { H = 40 };
    uint8_t d[H];
    for (int neutral = EPD_SCAN_NEUTRAL_EACH; neutral <= EPD_SCAN_NEUTRAL_END; neutral++) {
        memset(d, 0, sizeof d); /* a window in the middle */
        memset(d + 17, 1, 3);
        same_picture_as_without_skipping(H, d, neutral);
        TEST_ASSERT_TRUE(S.st.skipped > S.st.careful);
        memset(d, 0, sizeof d); /* at row 0 */
        memset(d, 1, 4);
        same_picture_as_without_skipping(H, d, neutral);
        memset(d, 0, sizeof d); /* two windows */
        memset(d + 5, 1, 2);
        memset(d + 30, 1, 3);
        same_picture_as_without_skipping(H, d, neutral);
        memset(d, 0, sizeof d); /* the last row that still leaves the two neutral rows below */
        memset(d + H - 6, 1, 4);
        same_picture_as_without_skipping(H, d, neutral);
        TEST_ASSERT_TRUE(S.st.skipped > 0);
        memset(d, 0, sizeof d); /* at the last row: not skipped, the sequence of before */
        memset(d + H - 3, 1, 3);
        same_picture_as_without_skipping(H, d, neutral);
        memset(d, 1, sizeof d); /* the whole panel */
        same_picture_as_without_skipping(H, d, neutral);
        memset(d, 0, sizeof d); /* nothing */
        same_picture_as_without_skipping(H, d, neutral);
    }
}

/* The X's size and the measured update (a line of 3 rows, 12 frames in 10 steps): the counts of the log line. */
static void test_counts_at_panel_size(void)
{
    static uint8_t d[HMAX];
    const uint8_t gray[10] = {1, 2, 1, 1, 1, 1, 2, 1, 1, 1};
    memset(d, 0, sizeof d);
    memset(d + 771, 1, 3);
    run_update(HMAX, d, gray, 10, EPD_SCAN_NEUTRAL_EACH, false);
    check_clocks(HMAX, gray, 10, EPD_SCAN_NEUTRAL_EACH, false);
    TEST_ASSERT_EQUAL_UINT32(30977, M.edges); /* the device's own count for this update (2026-10-08): ckv=30977/22 */
    TEST_ASSERT_EQUAL_UINT32(22u * 1404u, S.st.careful);
    run_update(HMAX, d, gray, 10, EPD_SCAN_NEUTRAL_END, false);
    check_clocks(HMAX, gray, 10, EPD_SCAN_NEUTRAL_END, false);
    TEST_ASSERT_EQUAL_UINT32(13, S.st.frames);
    TEST_ASSERT_EQUAL_UINT32(9, S.st.left);
    run_update(HMAX, d, gray, 10, EPD_SCAN_NEUTRAL_EACH, true);
    check_clocks(HMAX, gray, 10, EPD_SCAN_NEUTRAL_EACH, true);
    TEST_ASSERT_EQUAL_UINT32(22 * 6 + 3, S.st.careful); /* 6 rows a scan, 3 in the prime scan */
    TEST_ASSERT_EQUAL_UINT32(23u * 1404u - (22 * 6 + 3), S.st.skipped);
    TEST_ASSERT_EQUAL_INT(770, M.resume_row);
    run_update(HMAX, d, gray, 10, EPD_SCAN_NEUTRAL_END, true);
    check_clocks(HMAX, gray, 10, EPD_SCAN_NEUTRAL_END, true);
    TEST_ASSERT_EQUAL_UINT32(14, S.st.frames);
    TEST_ASSERT_EQUAL_UINT32(13 * 6 + 3, S.st.careful);
    /* skipped rows: 1 us low and 10 us high each, nothing else waits */
    TEST_ASSERT_EQUAL_UINT32(S.st.skipped * 11u, M.delays);
}

/* A row step FastEPD makes in a scan the sequencer left out (the glue failed to keep it from writing rows) is
 * counted and fails the update's check. */
static void test_a_stray_row_step_is_seen(void)
{
    enum { H = 8 };
    model_reset(H);
    epd_scan_rows_t r;
    epd_scan_rows(H, -1, -1, false, &r);
    epd_scan_seq_call(&S, &r, 1, false, 10, 0);
    TEST_ASSERT_TRUE(epd_scan_seq_start(&S));
    for (int i = 1; i < H; i++)
        epd_scan_seq_step(&S);
    TEST_ASSERT_FALSE(epd_scan_seq_start(&S)); /* the neutral scan, left out */
    TEST_ASSERT_TRUE(epd_scan_stats_ok(&S.st, H));
    epd_scan_seq_step(&S);
    TEST_ASSERT_EQUAL_UINT32(1, S.st.stray);
    TEST_ASSERT_FALSE(epd_scan_stats_ok(&S.st, H));
}

static void test_least_scan_time_and_gap(void)
{
    enum { H = 40 };
    uint8_t d[H] = {0};
    d[20] = 1;
    model_reset(H);
    epd_scan_rows_t r;
    epd_scan_rows(H, 20, 20, true, &r);
    epd_scan_seq_call(&S, &r, 1, false, 10, 20000);
    uint32_t t0 = M.now;
    fastepd_partial(1, d);
    TEST_ASSERT_EQUAL_UINT32(20000, M.now - t0); /* padded to the least time, measured from the scan's start */
    M.now += 7000;                               /* the next step is prepared */
    epd_scan_seq_call(&S, &r, 1, true, 10, 0);
    fastepd_partial(1, d);
    TEST_ASSERT_EQUAL_UINT32(7000, S.st.gap_us);
    frame_close();
    TEST_ASSERT_EQUAL_INT(0, M.bad_clocks);
}

static void test_log_fields(void)
{
    char b[128];
    epd_scan_cfg_t c = EPD_SCAN_CFG_DEFAULT;
    epd_scan_stats_t st = {22, 22 * 1404, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL_STRING("neutral=each skip=off frames=22 careful=30888 skipped=0 left=0 gap=0us ok",
                             epd_scan_str(&c, false, &st, 1404, b, sizeof b));
    c = (epd_scan_cfg_t){EPD_SCAN_NEUTRAL_END, 1, 10, 0};
    st = (epd_scan_stats_t){14, 81, 14 * 1404 - 81, 9, 0, 5300};
    TEST_ASSERT_EQUAL_STRING("neutral=end skip=on/10us frames=14 careful=81 skipped=19575 left=9 gap=5300us ok",
                             epd_scan_str(&c, true, &st, 1404, b, sizeof b));
    st.skipped--; /* a row short */
    TEST_ASSERT_NOT_NULL(strstr(epd_scan_str(&c, true, &st, 1404, b, sizeof b), " BAD"));
    st.skipped++;
    st.stray = 1;
    TEST_ASSERT_NOT_NULL(strstr(epd_scan_str(&c, true, &st, 1404, b, sizeof b), " BAD"));
    TEST_ASSERT_NOT_NULL(strstr(epd_scan_str(&c, false, &st, 1404, b, sizeof b), "skip=off")); /* set, not applied */
    b[0] = 'x';
    TEST_ASSERT_EQUAL_PTR(b, epd_scan_str(&c, true, &st, 1404, b, 0));
    TEST_ASSERT_EQUAL_CHAR('x', b[0]);
    st = (epd_scan_stats_t){0};
    TEST_ASSERT_FALSE(epd_scan_stats_ok(&st, 1404));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_are_the_scans_as_before);
    RUN_TEST(test_bounds);
    RUN_TEST(test_keys);
    RUN_TEST(test_span_is_the_rows_that_differ);
    RUN_TEST(test_union_of_spans);
    RUN_TEST(test_rows_of_a_scan);
    RUN_TEST(test_scans_of_an_update);
    RUN_TEST(test_the_whole_panel_is_todays_sequence);
    RUN_TEST(test_skipping_shows_each_gate_the_same);
    RUN_TEST(test_counts_at_panel_size);
    RUN_TEST(test_a_stray_row_step_is_seen);
    RUN_TEST(test_least_scan_time_and_gap);
    RUN_TEST(test_log_fields);
    return UNITY_END();
}
