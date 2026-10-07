/* epd_bwrymap.c: framebuffer rows to BWRY panel codes (epd_bwrymap.h).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_bwrymap.h"

#include "epd_bwry.h"

/* 4x4 Bayer thresholds 0..15 (the dither of gfx palette frames is the same, assets.md "Palette frames") */
static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

/* palette code (0 black, 1 red, 2 yellow, 3 white) -> panel code */
static const uint8_t PAL[4] = {BWRY_BLACK, BWRY_RED, BWRY_YELLOW, BWRY_WHITE};

uint8_t bwry_map_px(unsigned v, uint8_t depth, bool palette, int x, int y)
{
    if (depth == 1)
        return v ? BWRY_WHITE : BWRY_BLACK;
    if (palette)
        return PAL[v & 3u];
    return 32u * v > 3u * (2u * BAYER[y & 3][x & 3] + 1u) ? BWRY_WHITE : BWRY_BLACK;
}

void bwry_map_row(const uint8_t *src, uint8_t depth, bool palette, int x, int w, int y, uint8_t *out)
{
    static const uint8_t NIB[16] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15,
                                    0x40, 0x41, 0x44, 0x45, 0x50, 0x51, 0x54, 0x55};
    if (depth == 1) {
        for (int i = 0; i < w / 8; i++) {
            out[2 * i] = NIB[src[i] >> 4];
            out[2 * i + 1] = NIB[src[i] & 15];
        }
        return;
    }
    for (int i = 0; i < w; i += 4) {
        unsigned v4 = src[i >> 2], o = 0;
        for (int k = 0; k < 4; k++)
            o = o << 2 | bwry_map_px((v4 >> (6 - 2 * k)) & 3u, 2, palette, x + i + k, y);
        out[i >> 2] = (uint8_t)o;
    }
}
