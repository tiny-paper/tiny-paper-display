/*
 * epd_bwfast.h - experimental fast black/white partial waveform for the BWRY panel (PicPak, epd_bwry.h), hardware-free,
 * host-tested. Off by default, reachable only through `!tp bwfast on` (hal_esp panel_bwfast.c), never set by a release.
 * Issue #190 holds the desk research; docs/hardware/picpak-refresh.md "Fast black/white partial" the device check list.
 *
 * Register LUT layout (0x20, decoded from the vendor "5s" table and the SSD2683 black/white sibling, issue #190):
 * 7 analog bytes (power levels, VCOM, PLL), then 6 tables of 8 records of 11 bytes: table 0 VCOM, 1..4 the pixel codes
 * 00 black, 01 white, 10 yellow, 11 red, 5 border. Record: byte 0 a carrier (0xEF), bytes 1..2 eight drive symbols of
 * 2 bits (11 idle, 01 white-going, 10 black-going, 00 a third level never used here), bytes 3..10 the timing skeleton.
 * This file works on the repository layout of epd_bwry.h (11 columns x 48 rows, column-major, then the 7 analog bytes):
 * byte c of record k of table t is lut[(t * 8 + k) + 48 * c].
 *
 * The built table: black and white tables are exact polarity mirrors with p pulses in r records, every other table
 * holds (no drive symbol in bytes 1..2), the skeleton is the same in all six tables, the analog bytes are those of the
 * full-refresh LUT in use (no new voltage for the panel). Hard limits, checked on the bytes before every send
 * (bwf_check): at most BWF_MAX_RECORDS active records and BWF_MAX_DRIVE drive symbols per table, the mirror rule, no
 * drive in the hold tables. Policy (bwf_why): a full refresh after `every` partials and after any failure, never
 * below min_c or without a plausible panel temperature, never without a register LUT for the full refreshes, and the
 * window is refused when it holds a yellow or red code (bwf_row_has_colour).
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_EPD_BWFAST_H
#define TINY_EPD_BWFAST_H

#include <stdbool.h>
#include <stdint.h>

#include "epd_bwpart.h"

#define BWF_LUT_BYTES 535
#define BWF_ANALOG 7
#define BWF_TABLES 6
#define BWF_RECORDS 8
#define BWF_RECORD 11
#define BWF_WAVE (BWF_TABLES * BWF_RECORDS * BWF_RECORD) /* 528 */
enum { BWF_T_VCOM = 0, BWF_T_BLACK = 1, BWF_T_WHITE = 2, BWF_T_YELLOW = 3, BWF_T_RED = 4, BWF_T_BORDER = 5 };
enum { BWF_SYM_THIRD = 0, BWF_SYM_WHITE = 1, BWF_SYM_BLACK = 2, BWF_SYM_IDLE = 3 };
#define BWF_CARRIER 0xef

/* caps: 2 records are 88 fields, under 2 s at the slowest plausible field rate (issue #190) */
#define BWF_MAX_RECORDS 2
#define BWF_MAX_PULSES 8
#define BWF_MAX_DRIVE 16 /* drive symbols per table (pulses x records) */

enum { BWF_SKEL_PLAIN = 0, BWF_SKEL_FULL = 1 }; /* bytes 0, 3..10: carrier + idle, or record 1 of the full LUT's table */

typedef struct {
    uint8_t pulses;   /* 1..BWF_MAX_PULSES drive symbols per record */
    uint8_t records;  /* 1..BWF_MAX_RECORDS active records per table */
    uint8_t skeleton; /* BWF_SKEL_* */
} bwf_cfg_t;

enum { BWF_OK = 0, BWF_E_CFG = -1, BWF_E_LONG = -2, BWF_E_UNBALANCED = -3, BWF_E_HOLD = -4, BWF_E_SKEL = -5 };

/* Build the LUT (repository layout) from the full-refresh LUT in use (BWF_LUT_BYTES): BWF_E_CFG beyond the caps. */
int bwf_build(const bwf_cfg_t *c, const uint8_t *full, uint8_t out[BWF_LUT_BYTES]);

typedef struct {
    uint8_t w, b, third; /* symbols in bytes 1..2 of the table's records */
    uint8_t active;      /* records with any byte other than 0xFF */
} bwf_tstat_t;
typedef struct {
    bwf_tstat_t t[BWF_TABLES];
} bwf_stat_t;
void bwf_stat(const uint8_t *lut, bwf_stat_t *st);
/* The hard limits on any LUT: BWF_OK, or BWF_E_LONG (records or drive over the caps), BWF_E_UNBALANCED (black and white
 * tables not polarity mirrors, or a wrong-way or third-level symbol in them), BWF_E_HOLD (a drive symbol in the VCOM,
 * yellow, red or border table), BWF_E_SKEL (bytes 0, 3..10 differ between tables, or a hold record active without
 * the carrier). */
int bwf_check(const uint8_t *lut);
const char *bwf_err_name(int rc);

/* A panel row (4 codes per byte, epd_bwry.h) holds a yellow (2) or red (3) code. */
bool bwf_row_has_colour(const uint8_t *row, int bytes);

/* The saved setting (NVS "bwfast"): on = 0 is the default and refuses everything. */
typedef struct {
    uint8_t on;
    uint8_t pulses, records, skeleton; /* bwf_cfg_t */
    int8_t min_c;                      /* never below this panel temperature (default BWS_BAND_WARM_C) */
} bwf_set_t;
#define BWF_PULSES_DEFAULT 4
#define BWF_RECORDS_DEFAULT 1
#define BWF_MIN_C_DEFAULT 15
bool bwf_set_valid(const bwf_set_t *s);

/* Why a partial may not use the fast LUT now (BWF_WHY_OK: it may). every: a full after this many partials (0 = never
 * forced here), state as epd_bwpart.h; temp_c the panel reading (BWS_TEMP_NONE = none); full_lut: the full refreshes
 * use a register LUT (its analog bytes are inherited). */
enum { BWF_WHY_OK = 0, BWF_WHY_OFF, BWF_WHY_NEED_FULL, BWF_WHY_EVERY, BWF_WHY_TEMP, BWF_WHY_NO_LUT };
int bwf_why(const bwf_set_t *s, uint8_t every, const bwp_state_t *ps, int16_t temp_c, bool full_lut);
const char *bwf_why_name(int why);

#endif
