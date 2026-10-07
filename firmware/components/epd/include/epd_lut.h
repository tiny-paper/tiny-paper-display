/*
 * epd_lut.h - UC8179 register LUTs for 1-bit partial refresh (KW mode, PSR 0x3f). Hardware-free, host-tested.
 * Tables:
 *   EPD_LUTSET_BB: bitbank2/bb_epaper a9863c9 epd75_init_sequence_partial (GPL-3.0):
 *                  one group 30,1,30,1 frames; KW = VDL in phase A, WK = VDH in phase C.
 *   EPD_LUTSET_GX: ZinggJM/GxEPD2 de82887 GxEPD2_750_GDEY075T7 lut_20..lut_25 "_partial" (GPL-3.0):
 *                  one group T1,T2,T3,T4 = 30,5,30,5 frames; KW 0x5A (01 01 10 10), WK 0x84 (10 00 01 00),
 *                  charge balanced; VCOM DC 0x30 (-2.5 V), CDI 0x39 0x07, PWR adds VDHR 0x09.
 * Group layout (6 bytes): levels (2 bits per phase A..D, A in bits 7:6; 00 GND, 01 VDH, 10 VDL, 11 VDHR),
 * frames A, B, C, D, repeat. "reps" multiplies every frame count of the group, so the ratio of the
 * phases (and with it the DC balance of the source table) stays as in the source.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_EPD_LUT_H
#define TINY_EPD_LUT_H

#include <stdint.h>

#define EPD_LUT_LEN 42        /* bytes sent per LUT register */
#define EPD_LUT_N 6           /* 0x20 LUTC, 0x21 WW, 0x22 KW (R), 0x23 WK (W), 0x24 KK, 0x25 BD */
#define EPD_LUT_REPS_MIN 1
#define EPD_LUT_REPS_MAX 8    /* 8 x 30 = 240 frames per phase at most (~4.8 s at 50 Hz) */
#define EPD_VCOM_AUTO 0xff    /* per set: GX 0x30, BB not sent (controller default) */
#define EPD_VCOM_MIN 0x08     /* -0.50 V */
#define EPD_VCOM_MAX 0x40     /* -3.30 V (VDCS: -0.10 V - 0.05 V per step) */

typedef enum { EPD_LUTSET_BB = 0, EPD_LUTSET_GX = 1 } epd_lutset_t;

/* Clamp reps into [EPD_LUT_REPS_MIN, EPD_LUT_REPS_MAX]. */
unsigned epd_lut_reps_clamp(unsigned reps);
/* Fill the six LUTs of `set` with every frame count times `reps` (clamped). A frame count that would
 * exceed 255 limits the factor for the whole table, so all phases keep their ratio. Returns the factor used. */
unsigned epd_lut_build(epd_lutset_t set, unsigned reps, uint8_t out[EPD_LUT_N][EPD_LUT_LEN]);
/* VCOM DC register (0x82) value for `set` and the user setting (EPD_VCOM_AUTO or a value, clamped),
 * or -1 for "do not send". */
int epd_lut_vcom(epd_lutset_t set, uint8_t setting);
/* Net drive of one LUT: frames at VDH minus frames at VDL over all groups and repeats (0 = balanced). */
int epd_lut_dc(const uint8_t lut[EPD_LUT_LEN]);
/* Frames the LUT drives in total (all phases, groups, repeats). */
unsigned epd_lut_frames(const uint8_t lut[EPD_LUT_LEN]);
/* Frames the LUT drives at a non-GND level (0 = the transition leaves the pixel alone). */
unsigned epd_lut_drive_frames(const uint8_t lut[EPD_LUT_LEN]);

#endif
