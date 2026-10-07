/*
 * epd_bwrymap.h - framebuffer rows (assets.md) to the codes of the BWRY panel (epd_bwry.h: 0 black, 1 white,
 * 2 yellow, 3 red), 4 pixels per byte, leftmost in bits 7:6. Hardware-free (src/epd_bwrymap.c), host-tested; used by
 * hal_esp panel_bwry.c and the sim's PicPak panel.
 *
 *   depth 1                0 black, 1 white
 *   depth 2, gray frame    0 black, 3 white, the grays 1 and 2 as a 4x4 ordered dither on panel coordinates
 *                          (5/16 and 11/16 white): the panel has no grays
 *   depth 2, palette frame (GFX_PAL_BWRY, assets.md "Palette frames") 0 black, 1 red, 2 yellow, 3 white
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_EPD_BWRYMAP_H
#define TINY_EPD_BWRYMAP_H

#include <stdbool.h>
#include <stdint.h>

/* Pixels [x, x + w) of panel row y: src points at pixel x of the framebuffer row (x a multiple of 8, w of 8 at depth 1
 * and of 4 at depth 2), out gets w / 4 bytes. */
void bwry_map_row(const uint8_t *src, uint8_t depth, bool palette, int x, int w, int y, uint8_t *out);

/* One framebuffer pixel value to its panel code (the same rules). */
uint8_t bwry_map_px(unsigned v, uint8_t depth, bool palette, int x, int y);

#endif
