/* Row step timing for the FastEPD glue (epd_rowtime.h). SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_rowtime.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_delay_range(void)
{
    epd_row_delay_t d = {0, 0};
    TEST_ASSERT_TRUE(epd_row_delay_valid(&d));
    d = (epd_row_delay_t){EPD_ROW_DELAY_MAX, EPD_ROW_DELAY_MAX};
    TEST_ASSERT_TRUE(epd_row_delay_valid(&d));
    d.settle = EPD_ROW_DELAY_MAX + 1;
    TEST_ASSERT_FALSE(epd_row_delay_valid(&d));
    TEST_ASSERT_FALSE(epd_row_delay_valid(NULL));
}

/* Two frames of 100 rows, 40 us each; a 3 ms stall at row 57 of the second frame, a 200 us one at row 3. */
static void test_stats_find_the_stalled_row(void)
{
    epd_row_stats_t s;
    epd_row_stats_reset(&s);
    TEST_ASSERT_EQUAL_INT(-1, s.max_row);
    uint32_t t = 0xfffff000u; /* the clock wraps inside the run */
    for (int f = 0; f < 2; f++) {
        epd_row_stats_start(&s, t);
        for (int r = 0; r < 100; r++) {
            t += 40;
            if (f == 1 && r == 57)
                t += 3000;
            if (f == 1 && r == 3)
                t += 160;
            epd_row_stats_step(&s, t);
        }
        t += 500; /* between frames: not a row period */
    }
    TEST_ASSERT_EQUAL_UINT32(2, s.frames);
    TEST_ASSERT_EQUAL_UINT32(3040, s.max_us);
    TEST_ASSERT_EQUAL_INT(57, s.max_row);
    TEST_ASSERT_EQUAL_UINT32(2, s.slow);
    TEST_ASSERT_EQUAL_UINT8(2, s.nslow_rows);
    TEST_ASSERT_EQUAL_INT(3, s.slow_rows[0]);
    TEST_ASSERT_EQUAL_INT(57, s.slow_rows[1]);
    TEST_ASSERT_EQUAL_UINT32(200, s.slow_us[0]);
    TEST_ASSERT_EQUAL_UINT32(3040, s.slow_us[1]);
    TEST_ASSERT_EQUAL_UINT16(2, s.slow_frame[1]);
    s.slow_driven = 2;
    char b[96];
    TEST_ASSERT_EQUAL_STRING("slow=2[3:200@2,57:3040@2d]", epd_row_stats_str(&s, b, sizeof b));
    char small[12];
    TEST_ASSERT_EQUAL_STRING("slow=2[3:20", epd_row_stats_str(&s, small, sizeof small)); /* cut, terminated */
}

/* The same row slow in every frame is kept once; at most EPD_ROW_SLOW_KEEP rows. */
static void test_slow_rows_distinct_and_capped(void)
{
    epd_row_stats_t s;
    epd_row_stats_reset(&s);
    uint32_t t = 0;
    for (int f = 0; f < 3; f++) {
        epd_row_stats_start(&s, t);
        for (int r = 0; r < 20; r++) {
            t += (r == 5 || (f == 2 && r >= 10)) ? 400 : 30;
            epd_row_stats_step(&s, t);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(3 + 10, s.slow);
    TEST_ASSERT_TRUE(EPD_ROW_SLOW_KEEP < 11); /* capped below the 11 distinct rows */
    TEST_ASSERT_EQUAL_UINT8(EPD_ROW_SLOW_KEEP, s.nslow_rows);
    TEST_ASSERT_EQUAL_INT(5, s.slow_rows[0]);
    TEST_ASSERT_EQUAL_UINT16(1, s.slow_frame[0]); /* first slipped in frame 1 */
    TEST_ASSERT_EQUAL_INT(10, s.slow_rows[1]);
    TEST_ASSERT_EQUAL_UINT16(3, s.slow_frame[1]);
    TEST_ASSERT_EQUAL_INT(16, s.slow_rows[7]);
}

/* The glue's row loop (epd_fastepd.cpp row_ctl) over one 200-row frame: each row takes `prep` us from its CKV on
 * to the next step (DMA wait, copy, FastEPD's calls), plus stalls at rows 50 (3 ms, an ISR held off) and 120
 * (180 us, PSRAM). Free: CKV stays high until the step, so the drive time is the whole period. Fixed: the pulse
 * ends by hardware at on + drive; a step that comes early waits for it (epd_row_drive_step), and the measured high
 * time is the pulse. Returns the drive stats. */
static epd_row_drive_stats_t row_loop(uint32_t drive, uint32_t prep)
{
    epd_row_drive_stats_t d;
    epd_row_drive_reset(&d);
    uint32_t t = 0xffffff00u, on = t; /* wraps inside */
    for (int r = 0; r < 200; r++) {
        t = on + prep + (r == 50 ? 3000 : r == 120 ? 180 : 0);
        uint32_t left = epd_row_drive_step(&d, on, t, drive);
        if (drive) {
            bool pad_high = left > 0; /* the pulse (hardware) is still on */
            if (pad_high) {
                t += left;
                epd_row_drive_note(&d, t - on);
            }
        }
        on = t + 2; /* LE, then the next pulse */
    }
    return d;
}

static void test_fixed_drive_is_exact_whatever_the_stalls(void)
{
    TEST_ASSERT_TRUE(epd_row_drive_valid(0));
    TEST_ASSERT_TRUE(epd_row_drive_valid(EPD_ROW_DRIVE_MAX));
    TEST_ASSERT_FALSE(epd_row_drive_valid(EPD_ROW_DRIVE_MAX + 1));
    TEST_ASSERT_FALSE(epd_row_drive_valid(-1));
    TEST_ASSERT_EQUAL_UINT32(5, epd_row_drive_left(0xfffffffeu, 25, 32)); /* wraps */
    TEST_ASSERT_EQUAL_UINT32(0, epd_row_drive_left(0, 40, 32));
    TEST_ASSERT_EQUAL_UINT32(0, epd_row_drive_left(0, 10, 0));

    epd_row_drive_stats_t f = row_loop(0, 36); /* FastEPD as is */
    TEST_ASSERT_EQUAL_UINT32(200, f.n);
    TEST_ASSERT_EQUAL_UINT32(36, f.min_us);
    TEST_ASSERT_EQUAL_UINT32(3036, f.max_us); /* the stalled row is driven 84 times longer */
    TEST_ASSERT_EQUAL_UINT32(2, f.over);

    epd_row_drive_stats_t x = row_loop(40, 36); /* pulse longer than the loop: every step waits */
    TEST_ASSERT_EQUAL_UINT32(40, x.min_us);
    TEST_ASSERT_EQUAL_UINT32(40, x.max_us);
    TEST_ASSERT_EQUAL_UINT32(0, x.over);
    TEST_ASSERT_EQUAL_UINT32(2, x.absorbed); /* the stalls went to CKV low */
    TEST_ASSERT_EQUAL_UINT32(198, x.waited);

    epd_row_drive_stats_t y = row_loop(EPD_ROW_DRIVE_DEFAULT, 36); /* shorter: nothing waits, nothing overdriven */
    TEST_ASSERT_EQUAL_UINT32(0, y.n);
    TEST_ASSERT_EQUAL_UINT32(0, y.over);
    TEST_ASSERT_EQUAL_UINT32(2, y.absorbed);
    char b[96];
    TEST_ASSERT_EQUAL_STRING("drive=40us high=40/40/40 n=198 waited=198 absorbed=2 over=0",
                             epd_row_drive_str(&x, 40, b, sizeof b));
    TEST_ASSERT_EQUAL_STRING("drive=0us high=36/51/3036 n=200 waited=0 absorbed=0 over=2",
                             epd_row_drive_str(&f, 0, b, sizeof b));
}

/* issue #203: the TRMNL X drove every partial row with two gate clocks; the hardware edge count must show it */
static void test_edges_per_frame_must_match_rows(void)
{
    unsigned per = 99;
    TEST_ASSERT_TRUE(epd_row_edges_ok(1408u * 7, 7, 1404, &per));
    TEST_ASSERT_EQUAL_UINT(1408, per);
    TEST_ASSERT_TRUE(epd_row_edges_ok(1409u * 7 + 3, 7, 1404, &per)); /* a stray edge or two */
    TEST_ASSERT_FALSE(epd_row_edges_ok(2816u * 7, 7, 1404, &per));    /* doubled, as seen on the glass */
    TEST_ASSERT_EQUAL_UINT(2816, per);
    TEST_ASSERT_FALSE(epd_row_edges_ok(1404u * 7 - 50, 7, 1404, &per)); /* rows lost */
    TEST_ASSERT_FALSE(epd_row_edges_ok(1408, 0, 1404, &per));         /* no frames counted */
    TEST_ASSERT_EQUAL_UINT(0, per);
    epd_row_drive_stats_t d = {0};
    char b[112];
    TEST_ASSERT_EQUAL_STRING("drive=0us high=0/0/0 n=0 waited=0 absorbed=0 over=0", epd_row_drive_str(&d, 0, b, sizeof b));
    d.edges = 2816 * 2;
    d.frames = 2;
    TEST_ASSERT_EQUAL_STRING("drive=32us high=0/0/0 n=0 waited=0 absorbed=0 over=0 ckv=5632/2",
                             epd_row_drive_str(&d, 32, b, sizeof b));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_edges_per_frame_must_match_rows);
    RUN_TEST(test_delay_range);
    RUN_TEST(test_stats_find_the_stalled_row);
    RUN_TEST(test_slow_rows_distinct_and_capped);
    RUN_TEST(test_fixed_drive_is_exact_whatever_the_stalls);
    return UNITY_END();
}
