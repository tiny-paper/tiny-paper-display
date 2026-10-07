/*
 * epd_par.h - parallel e-paper panels through bitbank2/FastEPD (Apache-2.0), ESP32-S3 only
 * (esp/epd_fastepd.cpp). First user: TRMNL X, 1872x1404, 16 grays.
 *
 * Framebuffer: FastEPD's own buffer in PSRAM, rows packed at the active depth:
 *   4 bpp: 2 px per byte, high nibble first, 0 = black .. 15 = white (FastEPD convention)
 *   1 bpp: 8 px per byte, MSB first, 1 = white, 0 = black
 * Every call powers the panel rails off when it returns (keep_on is never used), so a failed or
 * interrupted command never leaves the panel driven.
 * C11 API. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_EPD_PAR_H
#define TINY_EPD_PAR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "epd_parwave.h"
#include "epd_rowtime.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { EPDP_OK = 0, EPDP_E_INIT = -1, EPDP_E_STATE = -2, EPDP_E_IO = -3, EPDP_E_ARG = -4 };
enum { EPDP_CLEAR_NONE = 0, EPDP_CLEAR_FAST = 1, EPDP_CLEAR_SLOW = 2, EPDP_CLEAR_WHITE = 3 };

/* Init the TRMNL X panel. The caller creates I2C bus 0 first (i2c_new_master_bus, zeroed config); FastEPD
 * then reuses it. EPDP_E_INIT without it, EPDP_E_IO if the TCA9535 does not answer, EPDP_E_INIT if FastEPD
 * drives a flash/PSRAM pin (epd_pins.h). epdp_full and epdp_partial also return EPDP_E_IO when the
 * expander stops answering, before FastEPD is called. */
int epdp_open_trmnl_x(int partial_passes, int full_passes);
bool epdp_is_open(void);
/* 1 or 4 bits per pixel (the buffer is not converted or cleared). */
int epdp_set_depth(int bpp);
uint8_t *epdp_buffer(void);        /* current framebuffer (PSRAM) */
int epdp_width(void);
int epdp_height(void);
int epdp_stride(void);             /* bytes per row at the active depth */
/* Full update (16-gray waveform at 4 bpp) with a clear pass first. */
int epdp_full(int clear_mode);
/* After a 4 bpp full update: switch to 1 bpp with the shown image as the base. Both the current and
 * the previous 1 bpp frame get the thresholded image (gray < 8 = black), so a later partial only
 * drives the pixels the caller changes and the grays elsewhere stay as they are. */
int epdp_gray_to_1bit(void);
/* 1 bpp only: update the pixels that differ from the previous frame; [y0, y1] are the changed rows (FastEPD
 * gets every row, epd_parwin.h). Runs the partial waveform (epdp_set_wave; epd_parwave.h). */
int epdp_partial(int y0, int y1);
/* What the last partial handed to FastEPD (for the log, issue #203: the panel must prove what it was given):
 * a fingerprint (FNV-1a) of the previous and of the current 1 bpp frame at the drive call, the pixels that differ (white->black,
 * black->white), the rows that hold one (-1 -1 if none), and FastEPD's own state: mode, previous mode, rails on
 * before the first step and after the last. */
typedef struct {
    uint32_t crc_prev, crc_cur, diff_px, wb_px, bw_px;
    int y0, y1;
    uint8_t mode, prev_mode, pwr_before, pwr_after;
} epdp_given_t;
const epdp_given_t *epdp_last_given(void);
/* FastEPD passes, applied from the next update on (no re-init). */
int epdp_set_passes(int partial_passes, int full_passes);
/* 1 bpp partial waveform around the passes (epd_parwave.h): kick 0..14 frames pushing new blacks white
 * first, extra 0..14 black-only frames after the passes, redrive 0..EPD_WAVE_REDRIVE_MAX: how many later
 * partials push a black made by a partial (since the last full refresh) black again during their kick
 * frames (0 = off). From the next partial on. */
int epdp_set_wave(int kick, int extra, int redrive);
/* The pixels the last partial re-drove and their rows (px 0 when none). */
const epd_fresh_info_t *epdp_last_redrive(void);
/* The last partial: frames and ms per step (index EPD_WAVE_KICK, _MAIN, _EXTRA; 0 = step skipped). */
typedef struct {
    uint8_t frames[3];
    uint16_t ms[3];
} epdp_wave_ms_t;
const epdp_wave_ms_t *epdp_last_wave(void);
/* Gray partial (epd_parwave.h EPD_GP_*): the gray table on the changed pixels of these windows only, no
 * clearing flash. Windows: x, w multiples of 8; old4/new4 4 bpp (0 black .. 15 white, high nibble first),
 * `stride` bytes per row. matrix: 16 grays x passes (the table of the full refresh). dark/light 0..14: frames
 * pushing the changed pixels dark, then light, before the table. Call after epdp_partial_begin (1 bpp). */
typedef struct {
    int x, y, w, h;
    const uint8_t *old4, *new4;
    size_t stride;
} epdp_gwin_t;
int epdp_gray_partial(const epdp_gwin_t *win, int n, const uint8_t *matrix, int passes, int dark, int light);
/* The last gray partial: FastEPD partials run and frames driven (neutral frames not counted). */
typedef struct {
    uint8_t steps, frames;
} epdp_gpart_info_t;
const epdp_gpart_info_t *epdp_last_gpart(void);
/* The fresh bitmap (epd_parwave.h: redrives left per black made by a partial since the last full refresh,
 * EPD_FRESH_PLANES bit planes of w*h/8 bytes, *n bytes in all), allocated on first use; NULL if the panel is
 * not open or no PSRAM. The caller may load it (hal_esp keeps it across deep sleep). epdp_full clears it. */
uint8_t *epdp_fresh(size_t *n);
/* True once after a partial changed the fresh bitmap. */
bool epdp_fresh_take_dirty(void);
/* Row step delays of partial updates (epd_rowtime.h): settle, hold 0..EPD_ROW_DELAY_MAX us, from the next
 * partial on; full updates keep FastEPD's timing. */
int epdp_set_rows(int settle_us, int hold_us);
/* Row periods of the last update (partial or full): the longest and the stalled rows (epd_rowtime.h). */
const epd_row_stats_t *epdp_last_rows(void);
/* Fixed row drive for partials (epd_rowtime.h): CKV high exactly drive_us per row from an RMT pulse, 0 = FastEPD
 * as is. From the next partial. */
int epdp_set_drive(int drive_us);
/* The drive times of the last update (all rows measured in free mode; in fixed mode the rows whose step came before
 * the pulse ended), and the drive set for partials (0 when free or without the RMT). */
const epd_row_drive_stats_t *epdp_last_drive(uint32_t *drive_us);
bool epdp_drive_ok(void); /* partial rows use the fixed CKV pulse (drive_us > 0) */
/* LCD bus before epdp_open_trmnl_x: hz with hardware chip select (default EPDP_BUS_HW_CS, FastEPD 8f76e90's TRMNL X
 * setting), 10..40 MHz, or 0 = FastEPD 8dc8c74 as stock pins it (20 MHz, SPH by software from the DMA interrupt). */
#define EPDP_BUS_HW_CS 26666666u
int epdp_set_bus_hz(uint32_t hz);
uint32_t epdp_bus_hz(bool *soft_cs); /* the open bus (0: not open) */
/* 16-gray waveform: 16 rows (gray 0..15) of n/16 passes each (FastEPD setCustomMatrix; the table must
 * outlive the panel: FastEPD keeps the pointer). */
int epdp_set_matrix(const uint8_t *matrix, int n);
/* Start a 1 bpp partial frame filled by the caller: both the current and the previous 1 bpp frames are
 * set to white, so rows and columns the caller leaves alone are "unchanged" and not driven. Then write
 * the new pixels to epdp_buffer() and the old ones to epdp_prev_buffer() (same layout, epdp_stride()). */
int epdp_partial_begin(void);
uint8_t *epdp_prev_buffer(void);
/* Panel rails off, IO lines to high impedance. */
void epdp_close(void);
uint32_t epdp_last_ms(void);       /* duration of the last update */

#ifdef __cplusplus
}
#endif
#endif
