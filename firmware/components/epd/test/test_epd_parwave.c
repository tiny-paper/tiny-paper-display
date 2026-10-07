/* 1 bpp partial waveform for the FastEPD glue (epd_parwave.h). SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>

#include "epd_parwave.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* One byte, one pixel class per bit (1 = white; bit 7 first). */
enum { WW, WB, BW, BB, FR_OUT, FR_BW, FR_BB, NCLASS };
static const char *const NAMES[] = {"WW", "WB", "BW", "BB", "fresh outside", "fresh to white", "fresh stays"};
/*                               WW WB BW BB FO FW FB */
static const int P_BITS[NCLASS] = {1, 1, 0, 0, 1, 0, 0};
static const int C_BITS[NCLASS] = {1, 0, 1, 0, 1, 1, 0};
static const int F_BITS[NCLASS] = {0, 0, 0, 0, 1, 1, 1};

static uint8_t pack(const int *bits)
{
    uint8_t b = 0;
    for (int i = 0; i < 8; i++)
        b = (uint8_t)(b << 1 | (i < NCLASS ? bits[i] : 1));
    return b;
}

/* FastEPD 8dc8c74 bbepPartialUpdate per pixel: diffw = prev & ~cur -> black, diffb = ~prev & cur -> white. */
static char drive(uint8_t prev, uint8_t cur, int bit)
{
    int p = prev >> (7 - bit) & 1, c = cur >> (7 - bit) & 1;
    return p && !c ? 'B' : !p && c ? 'W' : '.';
}

/* Per class: the drive sequence of a whole partial (one letter per frame, the neutral frame as '.'). */
static void run(const epd_wave_t *w, bool with_fresh, char seq[NCLASS][64])
{
    uint8_t P = pack(P_BITS), C = pack(C_BITS), F[EPD_FRESH_PLANES] = {with_fresh ? pack(F_BITS) : 0, 0};
    bool wb, fr;
    epd_wave_scan(&P, &C, with_fresh ? F : NULL, 1, &wb, &fr);
    epd_wave_step_t st[3];
    int n = epd_wave_plan(w, wb, fr, st);
    memset(seq, 0, NCLASS * 64);
    for (int s = 0; s < n; s++) {
        uint8_t po, co;
        epd_wave_masks(st[s].phase, &P, &C, with_fresh ? F : NULL, w->redrive, &po, &co, 1);
        for (int k = 0; k < NCLASS; k++) {
            size_t l = strlen(seq[k]);
            for (int f = 0; f < st[s].frames; f++)
                seq[k][l++] = drive(po, co, k);
            seq[k][l] = '.';
        }
    }
}

static void expect(char seq[NCLASS][64], const char *const want[NCLASS])
{
    for (int k = 0; k < NCLASS; k++)
        TEST_ASSERT_EQUAL_STRING_MESSAGE(want[k], seq[k], NAMES[k]);
}

static void test_main_only_is_fastepd_partial(void)
{
    epd_wave_t w = {0, 3, 0, 1};
    char seq[NCLASS][64];
    run(&w, true, seq);
    const char *const want[] = {"....", "BBB.", "WWW.", "....", "....", "WWW.", "...."};
    expect(seq, want);
}

static void test_full_wave(void)
{
    epd_wave_t w = {2, 4, 1, 1};
    char seq[NCLASS][64];
    run(&w, true, seq);
    /*                           kick   main    extra */
    const char *const want[] = {"..........", "WW.BBBB.B.", "...WWWW...", "..........",
                                "BB........", "...WWWW...", "BB........"};
    expect(seq, want);
    TEST_ASSERT_EQUAL_INT(10, (int)strlen(seq[0]));
}

static void test_no_redrive_leaves_fresh_alone(void)
{
    epd_wave_t w = {2, 4, 0, 0};
    char seq[NCLASS][64];
    run(&w, true, seq);
    const char *const want[] = {"........", "WW.BBBB.", "...WWWW.", "........",
                                "........", "...WWWW.", "........"};
    expect(seq, want);
}

static void test_plan_skips_idle_steps(void)
{
    epd_wave_step_t st[3];
    epd_wave_t w = {2, 5, 3, 1};
    /* nothing turns black and nothing fresh: main only */
    TEST_ASSERT_EQUAL_INT(1, epd_wave_plan(&w, false, false, st));
    TEST_ASSERT_EQUAL_INT(EPD_WAVE_MAIN, st[0].phase);
    TEST_ASSERT_EQUAL_INT(5, st[0].frames);
    /* only fresh pixels: kick (redrive) + main */
    TEST_ASSERT_EQUAL_INT(2, epd_wave_plan(&w, false, true, st));
    TEST_ASSERT_EQUAL_INT(EPD_WAVE_KICK, st[0].phase);
    w.redrive = 0;
    TEST_ASSERT_EQUAL_INT(1, epd_wave_plan(&w, false, true, st));
    TEST_ASSERT_EQUAL_INT(3, epd_wave_plan(&w, true, false, st));
    TEST_ASSERT_EQUAL_INT(2 + 1 + 5 + 1 + 3 + 1, epd_wave_frames(st, 3));
}

static void test_plan_rejects_bad_settings(void)
{
    epd_wave_step_t st[3];
    const epd_wave_t bad[] = {{0, 0, 0, 0}, {15, 3, 0, 0}, {0, 15, 0, 0}, {0, 3, 15, 0}, {0, 3, 0, EPD_WAVE_REDRIVE_MAX + 1}};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_FALSE(epd_wave_valid(&bad[i]));
        TEST_ASSERT_EQUAL_INT(0, epd_wave_plan(&bad[i], true, true, st));
    }
}

static void test_track_fresh(void)
{
    uint8_t P = pack(P_BITS), C = pack(C_BITS), F[2] = {pack(F_BITS), 0}; /* fresh classes: 1 redrive left */
    TEST_ASSERT_TRUE(epd_wave_track(F, &P, &C, 1, false, 1));
    TEST_ASSERT_FALSE(epd_wave_track(F, &P, &C, 1, false, 1)); /* nothing left to change */
    /* WB joins, fresh turning white leaves, the rest stays */
    const int want[NCLASS] = {0, 1, 0, 0, 1, 0, 1};
    for (int k = 0; k < NCLASS; k++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(want[k], F[0] >> (7 - k) & 1, NAMES[k]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, F[1] >> (7 - k) & 1, NAMES[k]);
    }
}

/* count of pixel k (bit 7 - k) in a one-byte, two-plane fresh */
static int cnt(const uint8_t F[2], int k) { return (F[0] >> (7 - k) & 1) | (F[1] >> (7 - k) & 1) << 1; }

/* Regression, TRMNL X 2026-09-25 (firmware 1134, thin gray lines at the edges of the day, date and hour
 * shapes): a partial black was re-driven in every later partial until the next full refresh. Now each new
 * black gets `budget` redrives, then its rows are left alone. */
static void test_redrive_budget_runs_out(void)
{
    const uint8_t WHITE = 0xff;
    uint8_t P = 0xff, C = 0x7f, F[2] = {0, 0}; /* pixel 0 turns black */
    TEST_ASSERT_TRUE(epd_wave_track(F, &P, &C, 1, false, 3));
    TEST_ASSERT_EQUAL_INT(3, cnt(F, 0));
    const epd_wave_t w = {2, 4, 0, 3};
    int kicks = 0;
    for (int tick = 0; tick < 60; tick++) { /* an hour of minute partials that do not touch the pixel */
        bool wb, fr;
        epd_wave_scan(&WHITE, &WHITE, F, 1, &wb, &fr);
        epd_wave_step_t st[3];
        int n = epd_wave_plan(&w, wb, fr, st);
        bool redriven = false;
        for (int s = 0; s < n; s++) {
            uint8_t po, co;
            epd_wave_masks(st[s].phase, &WHITE, &WHITE, F, w.redrive, &po, &co, 1);
            if (st[s].phase == EPD_WAVE_KICK && drive(po, co, 0) == 'B') {
                kicks++;
                redriven = true;
            }
        }
        epd_wave_track(F, &WHITE, &WHITE, 1, redriven, w.redrive);
    }
    TEST_ASSERT_EQUAL_INT(3, kicks);
    TEST_ASSERT_EQUAL_INT(0, cnt(F, 0));
}

static void test_track_counts(void)
{
    /* px 0..3 counts 0..3; a redrive takes one off each; px 4 turns black (budget 2), px 3 turns white */
    uint8_t F[2] = {0x50, 0x30}; /* lo: px 1, 3; hi: px 2, 3 */
    TEST_ASSERT_EQUAL_INT(0, cnt(F, 0));
    TEST_ASSERT_EQUAL_INT(1, cnt(F, 1));
    TEST_ASSERT_EQUAL_INT(2, cnt(F, 2));
    TEST_ASSERT_EQUAL_INT(3, cnt(F, 3));
    uint8_t P = 0xef, C = 0xf7; /* px 3 black -> white, px 4 white -> black */
    TEST_ASSERT_TRUE(epd_wave_track(F, &P, &C, 1, true, 2));
    const int want[8] = {0, 0, 1, 0, 2, 0, 0, 0};
    for (int k = 0; k < 8; k++)
        TEST_ASSERT_EQUAL_INT(want[k], cnt(F, k));
    /* budget 0: new blacks are never re-driven */
    uint8_t G[2] = {0, 0}, p = 0xff, c = 0x00;
    TEST_ASSERT_FALSE(epd_wave_track(G, &p, &c, 1, false, 0));
    TEST_ASSERT_EQUAL_HEX8(0, G[0] | G[1]);
}

static void test_fresh_info(void)
{
    uint8_t F[2 * 12]; /* 4 rows of 3 bytes */
    memset(F, 0, sizeof F);
    epd_fresh_info_t fi;
    epd_wave_fresh_info(F, 12, 3, &fi);
    TEST_ASSERT_EQUAL_UINT32(0, fi.px);
    TEST_ASSERT_EQUAL_INT(-1, fi.y0);
    F[1 * 3 + 2] = 0x81;      /* row 1, lo plane */
    F[12 + 3 * 3 + 0] = 0x0f; /* row 3, hi plane */
    F[3 * 3 + 0] = 0x01;      /* row 3, lo plane, same pixel as a hi bit */
    epd_wave_fresh_info(F, 12, 3, &fi);
    TEST_ASSERT_EQUAL_UINT32(6, fi.px);
    TEST_ASSERT_EQUAL_INT(1, fi.y0);
    TEST_ASSERT_EQUAL_INT(3, fi.y1);
}

static void test_scan(void)
{
    uint8_t P[4] = {0xff, 0xff, 0x00, 0xff}, C[4] = {0xff, 0xff, 0xff, 0xff}, F[2 * 4] = {0};
    bool wb, fr;
    epd_wave_scan(P, C, F, 4, &wb, &fr);
    TEST_ASSERT_FALSE(wb);
    TEST_ASSERT_FALSE(fr);
    C[3] = 0xfe;
    F[1] = 0x10;
    epd_wave_scan(P, C, F, 4, &wb, &fr);
    TEST_ASSERT_TRUE(wb);
    TEST_ASSERT_TRUE(fr);
    epd_wave_scan(P, C, NULL, 4, &wb, &fr);
    TEST_ASSERT_FALSE(fr);
    F[1] = 0;
    F[4 + 2] = 0x01; /* a count in the high plane only */
    epd_wave_scan(P, C, F, 4, &wb, &fr);
    TEST_ASSERT_TRUE(fr);
}

/* ---- FastEPD 8dc8c74 bbepPartialUpdate row codes, whole panel, with the glue's frames per step ---- */
#define MW 64
#define MH 40
#define MDS (MW / 8)

static uint16_t lutw[256], lutb[256];

/* FastEPD's LUT init (BB_PANEL_FLAG_MIRROR_X, as the TRMNL X): bit clear -> w = 2, b = 1; set -> 3, 3. */
static void fastepd_luts(void)
{
    for (int i = 0; i < 256; i++) {
        uint16_t w = 0, b = 0;
        for (int j = 0; j < 8; j++) {
            bool clear = !(i & (1 << (7 - j)));
            w |= (uint16_t)((clear ? 2 : 3) << (j * 2));
            b |= (uint16_t)((clear ? 1 : 3) << (j * 2));
        }
        lutw[i] = (uint16_t)(w >> 8 | w << 8);
        lutb[i] = (uint16_t)(b >> 8 | b << 8);
    }
}

/* One partialUpdate(0, MH - 1): the 2-bit codes FastEPD sends for each row (MIRROR_X loop), every pass. */
static void fastepd_row_codes(const uint8_t *prev, const uint8_t *cur, uint8_t out[MH][MW / 4])
{
    for (int i = 0; i < MH; i++) {
        const uint8_t *pc = cur + i * MDS + MDS - 1, *pp = prev + i * MDS + MDS - 1;
        uint8_t *d = out[i];
        for (int n = 0; n < MW / 16; n++) {
            for (int k = 0; k < 2; k++) {
                uint8_t c = *pc--, p = *pp--;
                uint16_t v = (uint16_t)(lutw[(uint8_t)(p & ~c)] & lutb[(uint8_t)(~p & c)]);
                memcpy(d + 2 * k, &v, 2);
            }
            d += 4;
        }
    }
}

/* The steps of one partial as the glue runs them, per row: 1 if any code sent is not neutral (00). */
static void glue_rows(const epd_wave_t *w, const uint8_t *P, const uint8_t *C, const uint8_t *F, int driven[3][MH],
                      int *nsteps)
{
    bool wb, fr;
    epd_wave_scan(P, C, F, MDS * MH, &wb, &fr);
    epd_wave_step_t st[3];
    *nsteps = epd_wave_plan(w, wb, fr, st);
    memset(driven, 0, sizeof(int) * 3 * MH);
    for (int s = 0; s < *nsteps; s++) {
        uint8_t po[MDS * MH], co[MDS * MH], codes[MH][MW / 4];
        epd_wave_masks(st[s].phase, P, C, F, w->redrive, po, co, MDS * MH);
        fastepd_row_codes(po, co, codes);
        for (int i = 0; i < MH; i++)
            for (int x = 0; x < MW / 4; x++)
                driven[st[s].phase][i] |= codes[i][x] != 0;
    }
}

/* A partial as panel_par builds it: both frames white, a window with a shape turning black (rows 10..14),
 * one turning white (rows 20..21), unchanged black inside the window (row 25), and blacks from an earlier
 * partial outside the window (fresh, rows 30..31; FastEPD's frames are white there). */
static void test_rows_outside_changes_neutral_in_every_step(void)
{
    fastepd_luts();
    uint8_t P[MDS * MH], C[MDS * MH], F[EPD_FRESH_PLANES * MDS * MH];
    memset(P, 0xff, sizeof P);
    memset(C, 0xff, sizeof C);
    memset(F, 0, sizeof F);
    for (int y = 10; y <= 14; y++)
        C[y * MDS + 2] = C[y * MDS + 3] = 0x00;
    for (int y = 20; y <= 21; y++)
        P[y * MDS + 4] = 0x00;
    P[25 * MDS + 1] = C[25 * MDS + 1] = 0x0f;
    F[30 * MDS + 6] = F[31 * MDS + 6] = 0xf0;
    const epd_wave_t w = {2, 4, 1, 1};
    int driven[3][MH], n;
    glue_rows(&w, P, C, F, driven, &n);
    TEST_ASSERT_EQUAL_INT(3, n);
    for (int i = 0; i < MH; i++) {
        bool wb = i >= 10 && i <= 14, bw = i == 20 || i == 21, fresh = i == 30 || i == 31;
        TEST_ASSERT_EQUAL_INT_MESSAGE(wb || fresh, driven[EPD_WAVE_KICK][i], "kick");
        TEST_ASSERT_EQUAL_INT_MESSAGE(wb || bw, driven[EPD_WAVE_MAIN][i], "main");
        TEST_ASSERT_EQUAL_INT_MESSAGE(wb, driven[EPD_WAVE_EXTRA][i], "extra");
    }
}

/* Nothing changed and nothing fresh: one main step and no row driven (the blank frame is 00 anyway). */
static void test_unchanged_partial_drives_nothing(void)
{
    fastepd_luts();
    uint8_t P[MDS * MH], C[MDS * MH];
    for (size_t i = 0; i < sizeof P; i++)
        P[i] = C[i] = (uint8_t)(i * 37);
    const epd_wave_t w = {2, 4, 1, 1};
    int driven[3][MH], n;
    glue_rows(&w, P, C, NULL, driven, &n);
    TEST_ASSERT_EQUAL_INT(1, n);
    for (int i = 0; i < MH; i++)
        TEST_ASSERT_EQUAL_INT(0, driven[EPD_WAVE_MAIN][i]);
}

/* ---- fresh across deep sleep: run-length code ---- */
static void roundtrip(const uint8_t *in, size_t n, size_t *enc_len)
{
    static uint8_t enc[400000], dec[400000];
    TEST_ASSERT_TRUE(epd_rle_bound(n) <= sizeof enc);
    size_t l = epd_rle_enc(in, n, enc, epd_rle_bound(n));
    TEST_ASSERT_TRUE(n == 0 || l > 0);
    memset(dec, 0xa5, n);
    TEST_ASSERT_EQUAL_INT(0, epd_rle_dec(enc, l, dec, n));
    if (n)
        TEST_ASSERT_EQUAL_MEMORY(in, dec, n);
    if (enc_len)
        *enc_len = l;
}

static void test_rle_roundtrip(void)
{
    static uint8_t buf[250000];
    uint32_t x = 12345;
    for (size_t i = 0; i < sizeof buf; i++) {
        x = x * 1103515245u + 12345u;
        buf[i] = (uint8_t)(x >> 24);
    }
    size_t l;
    roundtrip(buf, sizeof buf, &l); /* noise: literal, bound holds */
    TEST_ASSERT_TRUE(l <= epd_rle_bound(sizeof buf));
    for (size_t i = 0; i < sizeof buf; i++) /* runs of 1..9 of a few values */
        buf[i] = (uint8_t)((i / (1 + i % 9)) % 3 ? 0 : 0xff);
    roundtrip(buf, sizeof buf, NULL);
    const uint8_t one[] = {7};
    roundtrip(one, 1, NULL);
    roundtrip(one, 0, NULL);
}

/* A TRMNL X fresh bitmap (1872x1404) with the black segments of four big slanted digits: a few KB. */
static void test_rle_fresh_is_small(void)
{
    enum { W8 = 1872 / 8, HH = 1404 };
    static uint8_t f[W8 * HH];
    memset(f, 0, sizeof f);
    for (int d = 0; d < 4; d++)
        for (int y = 500; y < 950; y++) {
            int x0 = 400 + d * 330 + (950 - y) / 8; /* slant */
            for (int x = x0; x < x0 + 60; x++) /* vertical segment */
                f[y * W8 + x / 8] |= (uint8_t)(0x80 >> (x % 8));
            if (y >= 700 && y < 760) /* middle bar */
                for (int x = x0; x < x0 + 250; x++)
                    f[y * W8 + x / 8] |= (uint8_t)(0x80 >> (x % 8));
        }
    size_t l;
    roundtrip(f, sizeof f, &l);
    char m[64];
    snprintf(m, sizeof m, "fresh %u bytes -> %u", (unsigned)sizeof f, (unsigned)l);
    TEST_MESSAGE(m);
    TEST_ASSERT_TRUE(l < 16384);
}

static void test_rle_rejects_bad_streams(void)
{
    uint8_t out[8];
    const uint8_t over[] = {9 << 1, 0}; /* run of 9 into 8 */
    const uint8_t short_[] = {4 << 1, 0}; /* 4 of 8 */
    const uint8_t cut[] = {8 << 1 | 1, 1, 2}; /* literal cut short */
    const uint8_t zero[] = {0, 0};
    const uint8_t var[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    TEST_ASSERT_EQUAL_INT(-1, epd_rle_dec(over, sizeof over, out, 8));
    TEST_ASSERT_EQUAL_INT(-1, epd_rle_dec(short_, sizeof short_, out, 8));
    TEST_ASSERT_EQUAL_INT(-1, epd_rle_dec(cut, sizeof cut, out, 8));
    TEST_ASSERT_EQUAL_INT(-1, epd_rle_dec(zero, sizeof zero, out, 8));
    TEST_ASSERT_EQUAL_INT(-1, epd_rle_dec(var, sizeof var, out, 8));
    const uint8_t ok[] = {8 << 1, 0x5a};
    TEST_ASSERT_EQUAL_INT(0, epd_rle_dec(ok, sizeof ok, out, 8));
    TEST_ASSERT_EQUAL_HEX8(0x5a, out[7]);
}

/* ---- gray partial ---- */
static const uint8_t TAB[16 * 3] = {
    /* 3 passes per gray; gray 0: dark dark dark, 15: light light light, the rest mixed */
    1, 1, 1, 2, 1, 1, 2, 1, 1, 2, 2, 1, 2, 2, 1, 2, 2, 1, 2, 2, 1, 2, 2, 1,
    2, 2, 1, 2, 2, 1, 2, 2, 1, 2, 2, 0, 2, 2, 0, 2, 2, 0, 2, 2, 3, 2, 2, 2,
};

static void test_gpart_row_codes(void)
{
    /* 16 px: px 0 black->white, px 1 white->black, px 2 gray 5->gray 11, px 3 unchanged gray 7, px 4 15->9 (code 0),
     * px 5 15->14 (code 3), rest white unchanged */
    uint8_t o4[8], n4[8];
    memset(o4, 0xff, sizeof o4);
    memset(n4, 0xff, sizeof n4);
    o4[0] = 0x0f; n4[0] = 0xf0;
    o4[1] = 0x57; n4[1] = 0xb7;
    o4[2] = 0xff; n4[2] = 0x9e;
    uint8_t P[2], C[2], ch[2];
    epd_gpart_row(EPD_GP_PASS, 2, TAB, 3, o4, n4, 16, P, C, ch);
    /* pass 2: gray 15 light (px 0), gray 0 dark (px 1), gray 11 code 0 (px 2), gray 9 dark (px 4), gray 14
     * code 3 (px 5): only 1 and 2 drive */
    TEST_ASSERT_EQUAL_HEX8(0xec, ch[0]); /* px 0, 1, 2, 4, 5 */
    TEST_ASSERT_EQUAL_HEX8(0x00, ch[1]);
    TEST_ASSERT_EQUAL_HEX8(0x7f, P[0]);
    TEST_ASSERT_EQUAL_HEX8(0xb7, C[0]);
    TEST_ASSERT_EQUAL_HEX8(0xff, P[1]);
    TEST_ASSERT_EQUAL_HEX8(0xff, C[1]);
    epd_gpart_row(EPD_GP_DARK, 0, TAB, 3, o4, n4, 16, P, C, NULL);
    TEST_ASSERT_EQUAL_HEX8(0xff, P[0]);
    TEST_ASSERT_EQUAL_HEX8(0x13, C[0]); /* every changed px dark, px 3 (unchanged) not */
    epd_gpart_row(EPD_GP_LIGHT, 0, TAB, 3, o4, n4, 16, P, C, NULL);
    TEST_ASSERT_EQUAL_HEX8(0x13, P[0]);
    TEST_ASSERT_EQUAL_HEX8(0xff, C[0]);
}

static void test_gpart_groups(void)
{
    uint8_t first[3], len[3];
    /* passes 0 and 1 differ for gray 1 (2 vs 1), 1 and 2 differ for gray 3 */
    TEST_ASSERT_EQUAL_INT(3, epd_gpart_groups(TAB, 3, first, len));
    uint8_t same[16 * 4];
    for (int g = 0; g < 16; g++) {
        same[g * 4 + 0] = same[g * 4 + 1] = 2;
        same[g * 4 + 2] = same[g * 4 + 3] = (uint8_t)(g < 8 ? 1 : 2);
    }
    uint8_t f4[4], l4[4];
    TEST_ASSERT_EQUAL_INT(2, epd_gpart_groups(same, 4, f4, l4));
    TEST_ASSERT_EQUAL_UINT8(0, f4[0]);
    TEST_ASSERT_EQUAL_UINT8(2, l4[0]);
    TEST_ASSERT_EQUAL_UINT8(2, f4[1]);
    TEST_ASSERT_EQUAL_UINT8(2, l4[1]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_main_only_is_fastepd_partial);
    RUN_TEST(test_full_wave);
    RUN_TEST(test_no_redrive_leaves_fresh_alone);
    RUN_TEST(test_plan_skips_idle_steps);
    RUN_TEST(test_plan_rejects_bad_settings);
    RUN_TEST(test_track_fresh);
    RUN_TEST(test_redrive_budget_runs_out);
    RUN_TEST(test_track_counts);
    RUN_TEST(test_fresh_info);
    RUN_TEST(test_scan);
    RUN_TEST(test_rows_outside_changes_neutral_in_every_step);
    RUN_TEST(test_unchanged_partial_drives_nothing);
    RUN_TEST(test_rle_roundtrip);
    RUN_TEST(test_rle_fresh_is_small);
    RUN_TEST(test_rle_rejects_bad_streams);
    RUN_TEST(test_gpart_row_codes);
    RUN_TEST(test_gpart_groups);
    return UNITY_END();
}
