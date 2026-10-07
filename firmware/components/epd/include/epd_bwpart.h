/*
 * epd_bwpart.h - BWRY (PicPak) partial window math and policy, hardware-free, host-tested.
 * The driver side is epd_bwry.h bwry_part_*; hal_esp panel_bwry.c glues it to hal.h.
 *
 * Model (experimental, see epd_bwry.h "Partial refresh"): the controller refreshes a window with the same OTP
 * waveform as a full refresh. Every pixel of the window is driven from controller RAM (new data only, no old
 * data), the rest of the panel is not driven. The app writes the window as bands, top first (hal.h); each band
 * goes out as its own RAM window (data order within it as the profile scans), then the refresh runs on the
 * union of the bands.
 *
 * Controller coordinates: x as on screen. Rows: a bottom-up profile (stock PSR) takes image row H-1 first,
 * so image row y is controller row H-1-y; top-down (gd) keeps y.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_EPD_BWPART_H
#define TINY_EPD_BWPART_H

#include <stdbool.h>
#include <stdint.h>

/* Window in controller coordinates, inclusive */
typedef struct {
    uint16_t x0, x1, y0, y1;
} bwp_win_t;

enum { BWP_OK = 0, BWP_E_ARG = -1, BWP_E_ORDER = -2, BWP_E_GAP = -3 };

/* Image rect (x, y, w, h) -> controller window. x and w must be multiples of `align` (8: one byte of a 1-bit
 * row, two bytes of controller data), inside a pw x ph panel, w and h > 0. */
int bwp_map(int x, int y, int w, int h, int pw, int ph, bool bottom_up, int align, bwp_win_t *out);

/* The 9 parameter bytes of PTLW (0x83): HRST, HRED, VRST, VRED as 16-bit big endian, then PMODE. */
void bwp_ptlw_bytes(const bwp_win_t *w, uint8_t pmode, uint8_t out[9]);
/* The 9 parameter bytes of PTL (0x90) as in GxEPD2's UC81xx-style 4-color drivers: HRST, HRED, VRST, VRED
 * 16-bit big endian, then PT_SCAN 0x01 (gates outside the window not scanned). */
void bwp_ptl_bytes(const bwp_win_t *w, uint8_t out[9]);

/* One partial refresh being written: the bands must form one solid window, the same x range and every row once.
 * Order does not matter (each band is its own RAM window; the app's band-hash pass writes the unchanged bands
 * between two changed ones after the second). add: BWP_E_ORDER on another x range or a row written twice;
 * bwp_acc_solid: no row of the union left out. Else the caller drops the partial (full next). */
#define BWP_MAX_ROWS 320
typedef struct {
    int x, w;       /* x range of the bands */
    int y0, y1;     /* union rows [y0, y1); y1 = 0: empty */
    int rows;       /* rows written */
    uint32_t seen[BWP_MAX_ROWS / 32];
} bwp_acc_t;

void bwp_acc_reset(bwp_acc_t *a);
int bwp_acc_add(bwp_acc_t *a, int x, int y, int w, int h);
bool bwp_acc_solid(const bwp_acc_t *a);

/* Box partial (the press overlay on the PicPak, hal.h hal_panel_box_partial, firmware.md "Press overlay"): one
 * refresh drives one window, so a set of boxes goes out as their union. Rects in native px, x and w multiples of 8,
 * inside pw x ph. BWP_OK: the boxes cover every pixel of the union (one box, or boxes that touch): drivable from
 * the boxes alone, no frame needed (a wake press). BWP_E_GAP: the union holds pixels of no box (the bar layouts):
 * the frame under the boxes must fill them, rendered at its own depth and palette. BWP_E_ARG: n out of 1..
 * BWP_MAX_BOXES or a bad rect. `win` is the union in both OK and GAP. */
#define BWP_MAX_BOXES 8
typedef struct {
    int x, y, w, h;
} bwp_rect_t;
int bwp_boxes_window(const bwp_rect_t *r, int n, int pw, int ph, bwp_rect_t *win);

/* Policy across ticks (kept in RTC memory by the HAL). */
typedef struct {
    uint8_t mode;       /* bwp_mode_t, 0 = off */
    uint8_t every;      /* a full refresh after this many partials (0 = never forced here) */
} bwp_cfg_t;

typedef struct {
    uint16_t since_full;  /* partial refreshes since the last full one */
    bool need_full;       /* the last partial failed, or the panel state is unknown (cold boot) */
} bwp_state_t;

/* Partial may be offered to the app now (hal_board()->partial_refresh). */
bool bwp_allowed(const bwp_cfg_t *c, const bwp_state_t *s);
/* A box partial may go out now: without "!tp part" mode, not after a cold boot or a failed refresh, and a full
 * after `every` partials of any kind (0 = never forced here). */
bool bwp_boxes_allowed(uint8_t every, const bwp_state_t *s);
/* The finished refresh: full or partial, ok or failed. */
void bwp_note(bwp_state_t *s, bool full, bool ok);

#endif
