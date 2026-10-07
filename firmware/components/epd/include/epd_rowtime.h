/*
 * epd_rowtime.h - row step timing for the FastEPD glue (TRMNL X, epd_par.h). Hardware-free, host-tested.
 *
 * FastEPD 8dc8c74 EPDiyV7RowControl steps a row as CKV off, LE pulse, and bbepWriteRow turns CKV on right
 * after: the gate of the row just written turns off while the source outputs already swing to the next
 * row's data, and the next gate turns on during that swing. In a 1 bpp partial the number of driven pixels
 * per row jumps from none to many (and back) at the edges of changed shapes; every frame then disturbs the
 * row being latched there across the whole panel width, and over many partials that adds up to thin
 * full-width lines (TRMNL X 2026-09-25, devices/trmnl-x/partial-lines-0704.jpg; a full refresh clears them:
 * its gray frames drive every pixel, so the load never jumps). The glue's own row control waits `settle`
 * us after CKV off (gate off before the swing) and `hold` us after LE (sources settled before the next
 * gate). A stall inside a frame (CKV stays high) would draw a line too: the glue records row periods.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_EPD_ROWTIME_H
#define TINY_EPD_ROWTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPD_ROW_DELAY_MAX 20 /* us, each of settle and hold */
#define EPD_ROW_SLOW_US   150 /* a row period above this is a stall (normal ~40 us) */
#define EPD_ROW_SLOW_KEEP 8

typedef struct {
    uint8_t settle; /* us between CKV off and LE */
    uint8_t hold;   /* us between LE and CKV on (the next row) */
} epd_row_delay_t;

bool epd_row_delay_valid(const epd_row_delay_t *d);

/* Row periods of one update (all its frames). */
typedef struct {
    uint32_t last_us;  /* time of the last start/step */
    int row;           /* row being written (0 after a start) */
    uint32_t frames;
    uint32_t max_us;   /* longest row period */
    int max_row;
    uint32_t slow;     /* periods above EPD_ROW_SLOW_US */
    /* The first distinct rows of those (row = panel y, top first, no mirror on the X), each with its longest
     * period and the frame (1-based, all steps of the update counted, neutral frames included) it first
     * slipped in. slow_driven: bit i set by the glue when row slow_rows[i] had pixels driven. */
    int slow_rows[EPD_ROW_SLOW_KEEP];
    uint32_t slow_us[EPD_ROW_SLOW_KEEP];
    uint16_t slow_frame[EPD_ROW_SLOW_KEEP];
    uint8_t nslow_rows;
    uint8_t slow_driven;
} epd_row_stats_t;

void epd_row_stats_reset(epd_row_stats_t *s);
/* A frame starts (ROW_START) at time now_us. */
void epd_row_stats_start(epd_row_stats_t *s, uint32_t now_us);
/* A row step at now_us: the period of the row written before it (s->row) ends. */
void epd_row_stats_step(epd_row_stats_t *s, uint32_t now_us);

/* The slow rows as " slow=<n>" plus "[row:us@frame(d),...]" ("d": the row had driven pixels) into b; b. */
char *epd_row_stats_str(const epd_row_stats_t *s, char *b, size_t n);

/*
 * Fixed row drive (epdiy's approach, idea only: its CKV high time is exact, the slack goes to CKV low). FastEPD
 * turns CKV on before the row's DMA and off only at the next row step, so every stall between rows (DMA ISR
 * latency, PSRAM, another core's critical section) keeps that gate on longer: an overdriven, darker row. With a
 * drive time set, the glue drives CKV from an RMT pulse of exactly that length (hardware, stalls cannot stretch
 * it) and the row step first waits for the pulse to end. 0 = FastEPD as is (CKV high until the next step).
 */
#define EPD_ROW_DRIVE_MAX     200 /* us */
#define EPD_ROW_DRIVE_DEFAULT 32  /* ~ FastEPD's own at ~37.7 us per row (53 ms per 1404-row frame) */

typedef struct {
    uint32_t n;              /* rows with a measured drive time */
    uint32_t min_us, max_us; /* CKV high, CKV on to off */
    uint64_t sum_us;
    uint32_t waited;         /* fixed: the step came before the pulse ended and waited for it */
    uint32_t absorbed;       /* fixed: row periods above EPD_ROW_SLOW_US (stalls) that went to CKV low */
    uint32_t over;           /* drive times above EPD_ROW_SLOW_US (overdriven rows) */
    uint32_t edges;          /* CKV rising edges counted in hardware over the update (0: no counter) */
    uint32_t frames;         /* ROW_STARTs of the update */
} epd_row_drive_stats_t;

/* CKV edges a frame of `rows` rows must show: the frame start's 4 pulses, then one per row (issue #203: on the
 * TRMNL X a partial came out twice as tall and twice as far down as commanded, two gate clocks per row). */
#define EPD_ROW_START_EDGES 4
#define EPD_ROW_EDGES_SLACK 2
/* false when the update's frames did not clock the gate driver once per row (*per_frame: edges per frame, 0
 * without frames). */
bool epd_row_edges_ok(uint32_t edges, uint32_t frames, int rows, unsigned *per_frame);

bool epd_row_drive_valid(int drive_us);
/* Time left of a pulse of drive_us started at on_us (0 when over, or drive_us 0). Wraps fine. */
uint32_t epd_row_drive_left(uint32_t on_us, uint32_t now_us, uint32_t drive_us);
void epd_row_drive_reset(epd_row_drive_stats_t *d);
/* One row's measured drive time. */
void epd_row_drive_note(epd_row_drive_stats_t *d, uint32_t high_us);
/* A row step at now_us, the row's CKV on at on_us. Free (drive_us 0): notes now - on as the drive time, returns 0.
 * Fixed: counts a stall that CKV low absorbed and returns the time left of the pulse (> 0: counted as waited; the
 * glue then waits for the pad to go low and notes the measured high time). */
uint32_t epd_row_drive_step(epd_row_drive_stats_t *d, uint32_t on_us, uint32_t now_us, uint32_t drive_us);
/* "drive=<set>us high=<min>/<avg>/<max> n=<n> waited=<w> absorbed=<a> over=<o>" into b; b. */
char *epd_row_drive_str(const epd_row_drive_stats_t *d, uint32_t drive_us, char *b, size_t n);

#ifdef __cplusplus
}
#endif

#endif
