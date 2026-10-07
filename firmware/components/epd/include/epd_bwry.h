/*
 * epd_bwry.h - 4-color black/white/yellow/red panel driver, GDEY042F51 family, 400x300
 * (PicPak: panel id 0x060401, UC81xx-class command set, controller named HX8717 by GxEPD2).
 * ESP-IDF only (esp/epd_bwry.c). Full refresh ~19 s; windowed partial experimental (below).
 *
 * Init profiles:
 *   BWRY_INIT_STOCK: the register values the PicPak stock firmware sends for panel 0x060401, as documented
 *     and hardware-tested by GottZ/open-PicPak 7b76daa (firmware/main/epd.c, documentation/device.md 4).
 *     Values only (interoperability facts); this code is our own. Its PSR scans bottom-up: rows go out last first.
 *   BWRY_INIT_GD: Good Display's demo sequence for GDEY042F51 as used by ZinggJM/GxEPD2 de82887
 *     (src/epd4c/GxEPD2_420c_GDEY0420F51.cpp, GPL-3.0). PSR scans top-down.
 *
 * Fast (experimental, default off; see bwry_fast_t): the OTP picks its waveform by temperature, a hotter
 * reading picks a shorter one. Neither variant is documented for this controller; judge on hardware.
 *
 * Partial refresh (experimental, bwry_part_*): HX8717-A datasheet (Preliminary V01, 6.1.22) has PTLW (0x83)
 * with PMODE; GxEPD2 de82887 GDEY0420F51 sets hasPartialUpdate (hasFastPartialUpdate false, partial_refresh_time
 * = full 30 s): 0x83 with PMODE 0x01, DTM 0x10 for the window only, CDI 0x97 (floating border), DRF 0x12 0x00.
 * Horizontal granularity 4 px (HRST[1:0] = 00, HRED[1:0] = 11). The gates scan the whole panel (datasheet), so
 * it takes as long as a full refresh and flashes the window only; pixels outside it are not driven. GxEPD2
 * notes for GDEY0579F51 (same controller) that the refresh modifies controller RAM and the window refresh ended
 * up full screen there: judge on hardware. Window math: epd_bwpart.h.
 *
 * Framebuffer: 2 bits per pixel, 4 pixels per byte, leftmost pixel in bits 7:6, rows top to bottom,
 * 100 bytes per row, 30000 bytes. Codes: 0 black, 1 white, 2 yellow, 3 red (panel native, same as
 * GxEPD2 and the PicPak image format).
 *
 * Lifecycle: bwry_open -> bwry_show or bwry_stream_* (1..n) -> bwry_sleep -> bwry_close.
 * bwry_show = [reset if asleep] init, write, power on, refresh, power off (data before PON as stock, epd_bwseq.h).
 * Every BUSY wait has a timeout; on a timeout the driver powers off and resets the controller, so the panel is never
 * left driving.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_EPD_BWRY_H
#define TINY_EPD_BWRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BWRY_W 400
#define BWRY_H 300
#define BWRY_STRIDE (BWRY_W / 4)
#define BWRY_FB_BYTES (BWRY_STRIDE * BWRY_H)

enum { BWRY_BLACK = 0, BWRY_WHITE = 1, BWRY_YELLOW = 2, BWRY_RED = 3 };
enum { BWRY_OK = 0, BWRY_E_TIMEOUT = -1, BWRY_E_STATE = -2, BWRY_E_ARG = -3, BWRY_E_IO = -4 };

typedef enum { BWRY_INIT_STOCK = 0, BWRY_INIT_GD = 1 } bwry_init_t;

/* Experimental faster refresh through a hotter temperature for the OTP waveform choice, sent after the init
 * profile and before power on:
 *   BWRY_FAST_TSE: TSE (0x41) = 0x07, the documented sensor offset of +7 C (HX8717-A datasheet Preliminary V01,
 *     6.1.14), then TSC (0x40) to measure again. Small shift, may change nothing.
 *   BWRY_FAST_E6: 0xE0 = 0x02, 0xE6 = 0x5C, 0xA5 then wait for BUSY: the fast full update of GxEPD2 de82887
 *     src/epd4c/GxEPD2_350c_GDEM035F51.cpp _InitDisplay() (`_use_fast_update`), Good Display demo values for the
 *     JD79667 controller (GDEM075F52 uses 0x5A, GDEM0154F51H 0x5D). Not documented for this panel: Good Display's
 *     own GDEY042F51 samples (A32-/S-GDEY042F51-20230921) only send 0xE0 = 0x00 and have no fast init, and the
 *     HX8717-A datasheet lists neither 0xE6 nor 0xA5.
 *   BWRY_FAST_E6P: the same after PON with 0xE6 = 0x5A: Good Display's A32-GDEM042F52-20240618 EPD_init_Fast()
 *     ("Fast update mode (12s)", JD79668, the 4.2" BWRY successor of GDEY042F51) and Waveshare epd3in5g_V2
 *     init_Fast send it after PON. 0xE6 looks like the forced temperature (0x5A = 90 C: the shortest OTP
 *     waveform, still 4-color); 0xA5 reloads the waveform. */
typedef enum { BWRY_FAST_OFF = 0, BWRY_FAST_TSE = 1, BWRY_FAST_E6 = 2, BWRY_FAST_E6P = 3 } bwry_fast_t;

typedef struct {
    int8_t sck, mosi, miso, cs, dc, rst, busy;
    int8_t other_cs;       /* another device's CS on the same bus (PicPak IMU): held high, -1 = none */
    uint32_t spi_hz;
} bwry_pins_t;

typedef struct {
    bwry_init_t init;
    int8_t flip_y;         /* -1 = as the profile needs, 0 = rows top first, 1 = rows bottom first */
    uint32_t refresh_timeout_ms; /* 0 = default 45000 */
    uint8_t fast;          /* bwry_fast_t */
    const uint8_t *lut;    /* register LUT (BWRY_LUT_BYTES, epd_bwspeed.h) in place of the OTP waveform; NULL = OTP */
    uint8_t pll;           /* experiment: PLL (0x30) frame rate in place of the profile's / LUT's (bwq_cfg_t.pll); 0 = as is */
} bwry_cfg_t;

/* Row source for bwry_show_rows: fill `row` (BWRY_STRIDE bytes, codes as in the framebuffer) with image row y,
 * 0 = top. Called once per row, in panel order (bottom-up when the profile flips). */
typedef void (*bwry_row_fn)(void *ctx, int y, uint8_t *row);

typedef struct {
    uint32_t reset_ms, power_on_ms, refresh_ms, power_off_ms; /* last BUSY wait durations */
    uint32_t bytes, refreshes, timeouts;
    int last_err;
} bwry_stats_t;

/* Claim SPI2 and the pins (other_cs high), hardware-reset the controller, check BUSY goes idle. */
int bwry_open(const bwry_pins_t *pins, const bwry_cfg_t *cfg);
/* Show a full BWRY_FB_BYTES framebuffer with a full refresh. Blocks for the refresh (~19 s). */
int bwry_show(const uint8_t *fb);
/* Same, rows from a callback (no full framebuffer in RAM). */
int bwry_show_rows(bwry_row_fn fn, void *ctx);
/* Streaming (no framebuffer; hal_esp renders band by band into it): bwry_stream_begin does
 * [reset if asleep] init and opens the DTM data stream; bwry_stream_rows sends the next
 * `n` rows in panel order (bottom-up when bwry_rows_bottom_up), calling fn with the image row y of
 * each; bwry_stream_end fills rows not sent with white, refreshes and powers off. Nothing else may
 * use SPI2 between begin and end (other_cs stays high). bwry_stream_abort powers off without a
 * refresh. bwry_show_rows = begin + all rows + end. */
int bwry_stream_begin(void);
int bwry_stream_rows(bwry_row_fn fn, void *ctx, int n);
int bwry_stream_end(void);
void bwry_stream_abort(void);
int bwry_stream_sent(void);             /* rows sent since bwry_stream_begin */
/* The configured profile sends rows last first (stock PSR, or flip_y = 1). */
bool bwry_rows_bottom_up(const bwry_cfg_t *cfg);
/* Windowed partial refresh (experimental): bwry_part_begin does [reset if asleep] init;
 * bwry_part_window sends one window (image coordinates, x and w multiples of 8) as its own RAM window, rows in
 * panel order, fn filling w / 4 bytes (the window's columns) of image row y; bwry_part_end refreshes the union
 * window (one solid rectangle of the windows sent) and powers off. bwry_stream_abort also ends a partial. */
typedef void (*bwry_wrow_fn)(void *ctx, int y, uint8_t *row, int x, int w);
int bwry_part_begin(void);
int bwry_part_window(int x, int y, int w, int h, bwry_wrow_fn fn, void *ctx);
int bwry_part_end(int x, int y, int w, int h);
/* Show one color everywhere (clear). */
int bwry_fill(uint8_t color);
/* Power off (if on) and controller deep sleep. The panel keeps its image. */
int bwry_sleep(void);
/* Sleep if needed, release SPI; CS and RST stay driven high. */
void bwry_close(void);

bool bwry_is_open(void);
/* Inside a BUSY wait (epd_wait_hook runs): every other entry is refused (epd_bwseq.h bwq_allowed). */
bool bwry_busy(void);
const bwry_stats_t *bwry_stats(void);
const char *bwry_init_name(bwry_init_t i);
const char *bwry_fast_name(uint8_t f);
/* Register LUT blob: 528 waveform bytes as 11 x 48 (column-major), then 7 timing bytes: VGH/VGL/VSH/VSL/VSHR
 * power levels (PWR 0x01 gets 0x07, t[0], t[1], t[3], t[2], t[4]), VCOM + 0x80 (VDCS 0x82), PLL (0x30). The
 * layout of tesserae's epd_lut_*s.h. */
#define BWRY_LUT_BYTES 535
/* Speed of the following refreshes (open or not): an OTP variant, or a register LUT (stock profile only). */
void bwry_set_speed(uint8_t fast, const uint8_t *lut);
/* Experiment: the PLL override (bwry_cfg_t.pll) from the next refresh; 0 = as the profile or LUT says. */
void bwry_set_pll(uint8_t pll);

/* Bit-banged register read while closed: reset, send `cmd`, read `n` bytes on MISO (use_miso) or on MOSI
 * (3-wire), then deep sleep again. Returns BWRY_E_STATE if BUSY stays low after reset. */
int bwry_read_reg(const bwry_pins_t *pins, uint8_t cmd, uint8_t *out, size_t n, bool use_miso);
/* Panel temperature while closed: reset, TSC (0x40, internal sensor), wait for BUSY, read 2 bytes on SDA (3-wire),
 * deep sleep. UC81xx format D[10:0]: raw[0] = whole degrees C (two's complement), raw[1] bits 7:5 = eighths. Stock
 * never reads it (V1.1.13 .. V1.1.24 send no 0x40); unverified on the PicPak: the caller treats an implausible value as
 * a failure. temp_c = -128 on failure. */
int bwry_read_temp(const bwry_pins_t *pins, int16_t *temp_c, uint8_t raw[2]);

static inline void bwry_px(uint8_t *fb, int x, int y, uint8_t c)
{
    if ((unsigned)x >= BWRY_W || (unsigned)y >= BWRY_H) return;
    uint8_t *b = &fb[y * BWRY_STRIDE + x / 4];
    int sh = 6 - 2 * (x & 3);
    *b = (uint8_t)((*b & ~(3u << sh)) | ((c & 3u) << sh));
}

static inline uint8_t bwry_fill_byte(uint8_t c) { return (uint8_t)((c & 3) * 0x55); }

#endif
