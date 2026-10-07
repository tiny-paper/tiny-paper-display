/*
 * epd_bwcard.h - test card for the BWRY panel (epd_bwry.h, 400x300, codes 0 black, 1 white, 2 yellow, 3 red), for the
 * hardware session of docs/hardware/picpak-refresh.md: one photo per refresh mode shows color strength and ghosting.
 * Hardware-free, host-tested; hal_esp "!tp panel testcard" streams it.
 *
 *   y   0.. 79  solid white, black, red, yellow (100 px each)
 *   y  88..167  50% mixes (checkerboard): black/white, red/white, yellow/white, red/yellow, black/red, black/yellow
 *   y 176..199  gradient black -> white (4x4 ordered dither)
 *   y 204..227  gradient white -> yellow
 *   y 232..255  gradient white -> red
 *   y 262..299  fine text (3x5 font at 1x and 2x) and the label (the refresh mode)
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_EPD_BWCARD_H
#define TINY_EPD_BWCARD_H

#include <stdint.h>

#define BWC_W 400
#define BWC_H 300
/* left edge of the text: the PicPak bezel covers the first columns (hardware 2322: "HE QUICK", "PEED" at x 4) */
#define BWC_TEXT_X 16

/* Image row y (0 = top) as panel codes, 4 pixels per byte, leftmost in bits 7:6 (BWC_W / 4 bytes). label: upper case,
 * digits, space and - . = : / (others as space), at most 60 characters, drawn at 2x. */
void bwc_row(const char *label, int y, uint8_t *row);
/* One pixel of it (tests). */
uint8_t bwc_px(const char *label, int x, int y);

#endif
