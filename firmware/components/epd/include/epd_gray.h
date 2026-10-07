/*
 * epd_gray.h - UC8179 4-gray (2-bit) full refresh: data layout and register LUTs. Hardware-free, host-tested.
 * Sources (GPL-3.0):
 *   bitbank2/bb_epaper a9863c9, bb_ep.inl: epd75_gray_init (EP75_800x480_4GRAY_GEN2, OTP 4-gray waveform
 *   at forced temperature 0x5f), epd75_old_gray_init (EP75_800x480_4GRAY, register LUTs for GDEY075T7),
 *   bbepSetPixel4Gray / bbepStartWrite (plane bits and DTM commands on UC81xx).
 *   usetrmnl/trmnl-firmware f5f87b7, src/display.cpp png_draw: a 2-bit PNG (0 black .. 3 white) is
 *   inverted, bit 0 goes to PLANE_0 (DTM2 0x13), bit 1 to PLANE_1 (DTM1 0x10); CDI DDX = 00.
 * Pixel data: 2 bits per pixel, MSB-first, 0 = black .. 3 = white (docs/spec/assets.md).
 * Planes per pixel (DTM1, DTM2): white 3 = (0,0), light gray 2 = (0,1), dark gray 1 = (1,0), black 0 = (1,1).
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_EPD_GRAY_H
#define TINY_EPD_GRAY_H

#include <stddef.h>
#include <stdint.h>

#include "epd_lut.h"

#define EPD_GRAY_TEMP_DEFAULT 0x5f   /* bb_epaper GEN2: TSSET 0x5f selects the OTP 4-gray waveform */
#define EPD_GRAY_VCOM_LUT 0x12       /* bb_epaper epd75_old_gray_init VDCS */

/* One row of `px` 2-bit pixels (px multiple of 8, src byte aligned) to the DTM1 and DTM2 bit planes
 * (px / 8 bytes each). Either output may be NULL. */
void epd_gray_planes(const uint8_t *src, size_t px, uint8_t *dtm1, uint8_t *dtm2);

/* The register LUT set of bb_epaper epd75_old_gray_init: 0x20 VCOM, 0x21..0x24, 0x25 border. */
void epd_gray_luts(uint8_t out[EPD_LUT_N][EPD_LUT_LEN]);

#endif
