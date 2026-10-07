/*
 * epd.h - UC8179 7.5" 800x480 panel driver (TRMNL OG: GDEY075T7 or GEDY075-D2 "GEN2").
 * ESP-IDF only (esp/epd_uc8179.c). Init values from bitbank2/bb_epaper a9863c9 (EP75_800x480,
 * EP75_800x480_GEN2) and ZinggJM/GxEPD2 GxEPD2_750_GDEY075T7, both GPL-3.0.
 *
 * Pixel data: 1 bit per pixel, rows MSB-first, 1 = white, 0 = black (docs/spec/assets.md).
 * EPD_GRAY4: 2 bits per pixel, 0 = black .. 3 = white, split into the DTM1/DTM2 planes (epd_gray.h).
 * Lifecycle, one refresh:
 *   epd_open -> epd_begin(mode) -> epd_write (1..n) -> epd_refresh -> [epd_begin ...] -> epd_sleep -> epd_close
 * epd_refresh always powers the charge pumps off (POF) when done, so the panel is never left driving.
 * Every BUSY wait has a timeout; on timeout the driver powers off and resets the controller.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_EPD_H
#define TINY_EPD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tiny/types.h"

#define EPD_W 800
#define EPD_H 480
#define EPD_STRIDE (EPD_W / 8)

enum { EPD_OK = 0, EPD_E_TIMEOUT = -1, EPD_E_STATE = -2, EPD_E_ARG = -3, EPD_E_IO = -4 };

typedef enum {
    EPD_GDEY075T7 = 0,  /* bb_epaper EP75_800x480 (older OG panels), GxEPD2 GDEY075T7 */
    EPD_GEN2 = 1,       /* bb_epaper EP75_800x480_GEN2 (GEDY075-D2): different booster soft start */
} epd_variant_t;

typedef enum {
    EPD_FULL = 0,       /* OTP waveform, temperature from the internal sensor (slow, cleanest) */
    EPD_FULL_FAST = 1,  /* OTP waveform at forced temperature 90 (GxEPD2 fast full, ~1.2 s) */
    EPD_PARTIAL = 2,    /* partial update, waveform per epd_part_wave_t; needs old + new data */
    EPD_GRAY4 = 3,      /* 4-gray full refresh (flashes), 2-bit data, waveform per epd_gray_wave_t */
} epd_mode_t;

typedef enum {
    EPD_GRAY_AUTO = 0,  /* per variant: GEN2 OTP, GDEY075T7 LUT (as bb_epaper) */
    EPD_GRAY_OTP = 1,   /* bb_epaper EP75_800x480_4GRAY_GEN2: OTP waveform at forced temperature (TSSET) */
    EPD_GRAY_LUT = 2,   /* bb_epaper EP75_800x480_4GRAY: register LUTs (epd_gray_luts), VCOM DC 0x12 */
} epd_gray_wave_t;

typedef enum {
    EPD_PART_OTP = 0,   /* OTP waveform at forced temperature 110 (GxEPD2 default for GDEY075T7) */
    EPD_PART_LUT = 1,   /* register LUTs from bb_epaper epd75_init_sequence_partial (30 frames), epd_lut.h */
    EPD_PART_GX = 2,    /* register LUTs + partial registers from GxEPD2 GDEY075T7 (30/5/30/5, VCOM DC -2.5 V) */
} epd_part_wave_t;

typedef struct {
    int8_t sck, mosi, cs, dc, rst, busy;
    uint32_t spi_hz;
} epd_pins_t;

typedef struct {
    epd_variant_t variant;
    epd_part_wave_t part_wave;
    bool window_refresh;   /* partial: refresh only the written window (PTIN/PTL) instead of the whole panel */
    uint8_t lut_reps;      /* register LUTs: frame count multiplier (epd_lut_build), 0 = 1 */
    uint8_t lut_vcom;      /* register LUTs: VCOM DC (0x82) value, EPD_VCOM_AUTO = per table */
    epd_gray_wave_t gray_wave;
    uint8_t gray_temp;     /* EPD_GRAY_OTP: forced temperature (TSSET), 0 = EPD_GRAY_TEMP_DEFAULT */
} epd_cfg_t;

typedef struct {
    uint32_t reset_ms, power_on_ms, refresh_ms, power_off_ms; /* last BUSY wait durations */
    uint32_t bytes;        /* data bytes sent since epd_begin */
    uint32_t timeouts;     /* since epd_open */
    int last_err;
} epd_stats_t;

/* Claim the SPI bus and pins, hardware-reset the controller. */
int epd_open(const epd_pins_t *pins, const epd_cfg_t *cfg);
/* Called on every poll while waiting for BUSY (weak no-op; hal_esp answers quick link requests). */
void epd_wait_hook(void);
/* While BUSY is low: sleep until it goes high or up to max_ms, true if it slept (weak: false, the driver polls).
 * hal_esp light-sleeps the chip here on battery: the ~1.7 s of a partial refresh no longer run the CPU. */
bool epd_wait_sleep(int busy_pin, uint32_t max_ms);
/* Init registers for `mode` and power on (PON). Resets first if the controller is asleep. */
int epd_begin(epd_mode_t mode);
/* Write one area. r->x, r->w multiples of 8, inside the panel. prev/next point at the area's
 * first byte in buffers with `stride` bytes per row. prev goes to the OLD plane (DTM1), next to
 * the NEW plane (DTM2). EPD_GRAY4: next is 2-bit data split into both planes, prev is unused. PARTIAL needs prev unless the controller RAM still holds it (see
 * epd_ram_valid). FULL: prev may be NULL (OLD is then written as all 0). */
int epd_write(const tiny_rect_t *r, const uint8_t *prev, const uint8_t *next, size_t stride);
/* Fill both planes of the whole panel with constant bytes (0xff = white). */
int epd_fill(uint8_t old_byte, uint8_t new_byte);
/* Refresh, wait for BUSY (timeout), then power off. In PARTIAL mode, if cfg.window_refresh is set or
 * the controller RAM outside the written areas is not known (after sleep/reset), only the union of
 * the areas written since epd_begin is refreshed. */
int epd_refresh(void);
/* Power off (if on) and controller deep sleep (DSLP). The panel keeps its image; RAM is lost. */
int epd_sleep(void);
/* Sleep if needed, release SPI; CS and RST stay driven high. */
void epd_close(void);

/* True while the controller RAM holds the last refreshed image in both planes. */
bool epd_ram_valid(void);
bool epd_is_open(void);
const epd_stats_t *epd_stats(void);
const char *epd_variant_name(epd_variant_t v);
const char *epd_part_wave_name(epd_part_wave_t w);
/* The gray waveform `w` resolves to for `v` (never EPD_GRAY_AUTO), and its name. */
epd_gray_wave_t epd_gray_wave(epd_variant_t v, epd_gray_wave_t w);
const char *epd_gray_wave_name(epd_gray_wave_t w);
/* Bit-banged 3-wire register read, as stock trmnl-firmware does for REV (0x70): reset, send `cmd`,
 * read `n` bytes on MOSI. Only while closed. Returns EPD_E_STATE if BUSY is low after reset
 * (not a UC81xx). */
int epd_read_reg(const epd_pins_t *pins, uint8_t cmd, uint8_t *out, size_t n);

#endif
