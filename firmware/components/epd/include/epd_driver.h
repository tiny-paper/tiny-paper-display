/*
 * epd_driver.h - who drives the glass of the TRMNL X (epd_par.h, esp/epd_fastepd.cpp): tuning key `driver`.
 * Hardware-free, host-tested: the glue asks here what it may install and installs nothing else.
 *
 *   ours    as before this key: our row control in the library's place, the bus set up again at our speed, a 1 bit
 *           partial as several library calls (kick, main, extra, redrive), our scans, our rails, gray partials
 *   plain   the panel library's public calls only, as the stock firmware uses them: one fullUpdate for a full
 *           refresh (the gray table through setCustomMatrix), one partialUpdate for a 1 bit partial, with the
 *           library's own row control, bus, timing and power up and down (with a rails hold time: its own
 *           keep-on, brought down by the policy of epd_rails.h). No gray partial; a rectangle at any depth is
 *           one fullUpdate with that rectangle.
 * Rule (PLAN.md "Driving the glass: library calls only"): `plain` is the way; `ours` stays until a person has
 * compared both on a panel.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_EPD_DRIVER_H
#define TINY_EPD_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { EPD_DRIVER_OURS = 0, EPD_DRIVER_PLAIN = 1, EPD_DRIVER_COUNT };
#define EPD_DRIVER_KEY     "driver"
#define EPD_DRIVER_DEFAULT EPD_DRIVER_OURS
extern const char *const EPD_DRIVER_WORDS[EPD_DRIVER_COUNT]; /* the key's words, by value */

bool epd_driver_valid(int driver);
const char *epd_driver_name(int driver); /* "ours", "plain"; "?" for anything else */

/* What of our own panel driving a driver uses. An unknown driver uses none of it. */
typedef struct epd_driver_parts {
    bool row_hook;     /* our row control in the library's place: row timing, the fixed drive, its counters */
    bool bus;          /* the LCD bus set up again at our speed with hardware chip select */
    bool sequencer;    /* our scans: rows clocked through, neutral scans left out (epd_scan.h) */
    bool wave;         /* a 1 bit partial as kick, main and extra steps, one library call each (epd_parwave.h) */
    bool redrive;      /* blacks of earlier partials pushed again, the fresh bitmap */
    bool rails;        /* rails brought up by us before every partial (plain: only with a hold time, as keep-on) */
    bool gray_partial; /* gray partial updates: the gray table step by step through 1 bit partials */
} epd_driver_parts_t;
epd_driver_parts_t epd_driver_parts(int driver);

/* The row control to install: ours only when the driver has the row hook, else the library's own (lib, as its
 * initPanel set it). */
typedef void epd_driver_rows_fn(void *state, int type);
epd_driver_rows_fn *epd_driver_rows(int driver, epd_driver_rows_fn *ours, epd_driver_rows_fn *lib);
/* The bus clock to run at: ours only when the driver sets the bus up again and a speed is set (ours_hz 0: the
 * library's), else the library's own. */
uint32_t epd_driver_bus_hz(int driver, uint32_t ours_hz, uint32_t lib_hz);
/* The depths (bit0 = 1, bit1 = 2, bit2 = 4 bit) of `depths` a partial update takes with this driver. */
uint8_t epd_driver_partial_depths(int driver, uint8_t depths);

/* The depths of `depths` at which a full refresh of a rectangle alone is on offer: the library's fullUpdate takes a
 * rectangle (its clearing and its passes stay inside it), so under plain every depth; ours has none. */
uint8_t epd_driver_full_area_depths(int driver, uint8_t depths);
/* The x to hand the library for a rectangle x, w of a panel panel_w wide. Read in FastEPD 8dc8c74 (bbepFixRect,
 * bbepClear, bbepFullUpdate): the rectangle masks the row data by its position in the data, and a panel with
 * BB_PANEL_FLAG_MIRROR_X (the TRMNL X) sends its rows last pixel first, so the mask lands mirrored unless the x
 * is. Not seen on a panel. */
int epd_driver_lib_rect_x(int panel_w, bool mirror_x, int x, int w);

/* The scans one library call makes, counted in FastEPD 8dc8c74 (bbepPartialUpdate, bbepFullUpdate, 1 and 4 bit):
 * a partial is its passes and one neutral scan; a full refresh its clearing scans (clear: EPDP_CLEAR_* of
 * epd_par.h, none 0, fast 16, slow 32, white 8), a neutral one, its passes and a neutral one. Only for the log's
 * check of the gate clocks under `plain`, where no hook of ours counts the scans: a library that scans otherwise
 * shows as a W line, nothing else changes. 0: unknown clear mode or passes below 1. */
uint32_t epd_driver_lib_scans(bool partial, int clear, int passes);

#ifdef __cplusplus
}
#endif
#endif
