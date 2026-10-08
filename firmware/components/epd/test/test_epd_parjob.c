/* The frames of a 1 bpp partial update (epd_parjob.h): working on the rows that can hold anything gives the library
 * the same frames, keeps the same frames and the same redrive counts as working on whole frames, over runs of
 * random updates with the redrive on and off, full refreshes, a loaded redrive bitmap, failed updates and empty
 * ones. The rows way never reads its saved copies outside the worked rows (they hold rubbish here).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdlib.h>
#include <string.h>

#include "epd_parjob.h"
#include "unity.h"

#define RB 6
#define H 48
#define N (RB * H)

typedef struct {
    uint8_t prev[N], cur[N], P[N], C[N], fresh[EPD_FRESH_PLANES * N];
    epd_parjob_t j;
} side_t;
static side_t A, B; /* A: whole frames, B: rows */
static uint32_t seed;

static uint32_t rnd(void)
{
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
}

static void side_init(side_t *s, uint8_t rubbish)
{
    memset(s, 0, sizeof *s);
    memset(s->P, rubbish, N);
    memset(s->C, rubbish, N);
    s->j.prev = s->prev, s->j.cur = s->cur, s->j.P = s->P, s->j.C = s->C, s->j.fresh = s->fresh;
    s->j.n = N, s->j.rb = RB, s->j.h = H;
    epd_parjob_init(&s->j);
}

void setUp(void)
{
    side_init(&A, 0xff);
    side_init(&B, 0x5a);
}
void tearDown(void) {}

static int popcount_n(const uint8_t *p, size_t n)
{
    int c = 0;
    for (size_t i = 0; i < n; i++)
        c += __builtin_popcount(p[i]);
    return c;
}

/* The library's part of a step: it drives prev -> cur and leaves previous = current. */
static void lib_scans(side_t *s) { memcpy(s->prev, s->cur, N); }

/* One update on both sides: rows [y0, y1] get random old and new pixels (y1 < y0: nothing written). */
static void update(const epd_wave_t *w, int y0, int y1, bool ok, bool some_same)
{
    epd_parjob_white(&A.j, false);
    epd_parjob_white(&B.j, true);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(A.prev, B.prev, N); /* white the cheap way is white */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(A.cur, B.cur, N);
    for (int y = y0; y <= y1; y++)
        for (int x = 0; x < RB; x++) {
            uint8_t o = (uint8_t)rnd(), n = some_same && (rnd() & 1) ? o : (uint8_t)rnd();
            A.prev[y * RB + x] = B.prev[y * RB + x] = o;
            A.cur[y * RB + x] = B.cur[y * RB + x] = n;
        }
    static uint8_t old[N], new_[N]; /* the pictures themselves: what the main step drives and what is kept */
    memcpy(old, A.prev, N);
    memcpy(new_, A.cur, N);
    epd_parjob_begin(&A.j, w, y0, y1, false);
    epd_parjob_begin(&B.j, w, y0, y1, true);
    TEST_ASSERT_EQUAL_INT(A.j.ns, B.j.ns);
    TEST_ASSERT_EQUAL_INT(A.j.any_wb, B.j.any_wb);
    TEST_ASSERT_EQUAL_INT(A.j.redriven, B.j.redriven);
    TEST_ASSERT_EQUAL_UINT32(A.j.redrive.px, B.j.redrive.px);
    TEST_ASSERT_EQUAL_INT(A.j.redrive.y0, B.j.redrive.y0);
    TEST_ASSERT_EQUAL_INT(A.j.redrive.y1, B.j.redrive.y1);
    TEST_ASSERT_TRUE(B.j.b < B.j.a || (B.j.a >= 0 && B.j.b < H));
    for (int i = 0; i < A.j.ns; i++) {
        epd_wave_drive_t da, db;
        TEST_ASSERT_EQUAL_INT(A.j.st[i].phase, B.j.st[i].phase);
        TEST_ASSERT_EQUAL_INT(A.j.st[i].frames, B.j.st[i].frames);
        epd_parjob_step(&A.j, i, &da);
        epd_parjob_step(&B.j, i, &db);
        /* what the library is handed, every byte of both frames */
        TEST_ASSERT_EQUAL_HEX8_ARRAY(A.prev, B.prev, N);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(A.cur, B.cur, N);
        if (A.j.st[i].phase == EPD_WAVE_MAIN) {
            TEST_ASSERT_EQUAL_HEX8_ARRAY(old, B.prev, N);
            TEST_ASSERT_EQUAL_HEX8_ARRAY(new_, B.cur, N);
        }
        TEST_ASSERT_EQUAL_UINT32(da.driven, db.driven);
        TEST_ASSERT_EQUAL_UINT32(da.same, db.same);
        lib_scans(&A);
        lib_scans(&B);
    }
    bool ca = epd_parjob_end(&A.j, ok), cb = epd_parjob_end(&B.j, ok);
    TEST_ASSERT_EQUAL_INT(ca, cb);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(new_, B.prev, N); /* both frames hold the new picture */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(new_, B.cur, N);
    /* the frames kept and the redrive counts */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(A.prev, B.prev, N);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(A.cur, B.cur, N);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(A.fresh, B.fresh, sizeof A.fresh);
    /* the rows side knows where fresh is: nothing set outside its span */
    for (int y = 0; y < H; y++)
        if (B.j.f0 < 0 || y < B.j.f0 || y > B.j.f1)
            TEST_ASSERT_EQUAL_INT(0, popcount_n(B.fresh + y * RB, RB) + popcount_n(B.fresh + N + y * RB, RB));
}

static void full_refresh(void)
{
    for (side_t *s = &A; s; s = s == &A ? &B : NULL) {
        memset(s->fresh, 0, sizeof s->fresh);
        epd_parjob_fresh_none(&s->j);
        memset(s->prev, (int)(rnd() & 0xff), N); /* a full refresh leaves its own picture in the frames */
        memset(s->cur, 0x3c, N);
        epd_parjob_touched(&s->j);
    }
}

static void fresh_loaded(void)
{
    for (size_t i = 0; i < sizeof A.fresh; i++)
        A.fresh[i] = B.fresh[i] = (rnd() % 5) ? 0 : (uint8_t)rnd();
    epd_parjob_fresh_unknown(&A.j);
    epd_parjob_fresh_unknown(&B.j);
}

static void run(uint32_t s, int updates)
{
    seed = s;
    setUp();
    for (int k = 0; k < updates; k++) {
        epd_wave_t w = {(uint8_t)(rnd() % 3), (uint8_t)(1 + rnd() % 4), (uint8_t)(rnd() % 2), (uint8_t)(rnd() % 4)};
        if (rnd() % 3 == 0)
            w.redrive = 0;
        int y0 = (int)(rnd() % H), y1 = y0 + (int)(rnd() % 5);
        if (y1 >= H)
            y1 = H - 1;
        unsigned what = rnd() % 16;
        if (what == 0)
            full_refresh();
        else if (what == 1)
            fresh_loaded();
        else if (what == 2)
            update(&w, 0, -1, true, false); /* nothing written */
        else if (what == 3)
            update(&w, y0, y1, false, true); /* a step failed: the counts stay */
        else
            update(&w, y0, y1, true, what & 1);
    }
}

static void test_rows_and_whole_frames_agree(void)
{
    for (uint32_t s = 1; s <= 300; s++)
        run(s, 40);
}

static void test_the_first_and_the_last_row(void)
{
    const epd_wave_t w = {2, 4, 1, 2};
    update(&w, 0, 0, true, false);
    update(&w, H - 1, H - 1, true, false);
    update(&w, 0, H - 1, true, true);
    update(&w, 20, 20, true, false);
}

/* The span worked on: the written rows; with the redrive on, joined with the rows of fresh; nothing when neither. */
static void test_span(void)
{
    int a, b;
    epd_parjob_span(100, 10, 12, true, false, -1, -1, &a, &b);
    TEST_ASSERT_EQUAL_INT(0, a);
    TEST_ASSERT_EQUAL_INT(99, b);
    epd_parjob_span(100, 10, 12, false, false, 40, 50, &a, &b);
    TEST_ASSERT_EQUAL_INT(10, a);
    TEST_ASSERT_EQUAL_INT(12, b);
    epd_parjob_span(100, 10, 12, false, true, 40, 50, &a, &b);
    TEST_ASSERT_EQUAL_INT(10, a);
    TEST_ASSERT_EQUAL_INT(50, b);
    epd_parjob_span(100, 60, 70, false, true, 40, 50, &a, &b);
    TEST_ASSERT_EQUAL_INT(40, a);
    TEST_ASSERT_EQUAL_INT(70, b);
    epd_parjob_span(100, -1, -1, false, true, 40, 50, &a, &b);
    TEST_ASSERT_EQUAL_INT(40, a);
    TEST_ASSERT_EQUAL_INT(50, b);
    epd_parjob_span(100, -1, -1, false, true, -1, -1, &a, &b);
    TEST_ASSERT_TRUE(b < a);
    epd_parjob_span(100, 5, 4, false, false, 40, 50, &a, &b);
    TEST_ASSERT_TRUE(b < a);
    epd_parjob_span(100, 90, 250, false, false, -1, -1, &a, &b); /* rows past the panel are not worked on */
    TEST_ASSERT_EQUAL_INT(90, a);
    TEST_ASSERT_EQUAL_INT(99, b);
}

/* Setting the frames white by rows is taken only from an update that itself started from white frames; anything
 * else that wrote them (a full refresh) makes the next one whole. */
static void test_white_by_rows_needs_a_known_frame(void)
{
    const epd_wave_t w = {0, 2, 0, 0};
    memset(B.prev, 0x11, N);
    memset(B.cur, 0x22, N);
    epd_parjob_white(&B.j, true); /* nothing known yet: all of both */
    for (size_t i = 0; i < N; i++)
        TEST_ASSERT_EQUAL_HEX8(0xff, B.prev[i] & B.cur[i]);
    B.cur[7 * RB] = 0x0f;
    epd_parjob_begin(&B.j, &w, 7, 7, true);
    epd_parjob_step(&B.j, 0, NULL);
    epd_parjob_end(&B.j, true);
    TEST_ASSERT_TRUE(B.j.white_ok);
    TEST_ASSERT_EQUAL_INT(7, B.j.w0);
    TEST_ASSERT_EQUAL_INT(7, B.j.w1);
    /* an update without white first */
    epd_parjob_begin(&B.j, &w, 9, 9, true);
    epd_parjob_end(&B.j, true);
    TEST_ASSERT_FALSE(B.j.white_ok);
    epd_parjob_white(&B.j, true);
    epd_parjob_begin(&B.j, &w, 9, 9, true);
    epd_parjob_end(&B.j, true);
    TEST_ASSERT_TRUE(B.j.white_ok);
    epd_parjob_touched(&B.j);
    TEST_ASSERT_FALSE(B.j.white_ok);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rows_and_whole_frames_agree);
    RUN_TEST(test_the_first_and_the_last_row);
    RUN_TEST(test_span);
    RUN_TEST(test_white_by_rows_needs_a_known_frame);
    return UNITY_END();
}
