/* BWRY test card (epd_bwcard.h). C11. SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_bwcard.h"

#include <string.h>

enum { K = 0, W = 1, Y = 2, R = 3 }; /* panel codes (epd_bwry.h) */

/* 3x5 glyphs, 15 bits: row 0 in bits 14..12 (bit 14 = left column) */
static const char GLYPH_CH[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-.=:/";
static const uint16_t GLYPH[] = {
    0x7b6f, 0x2c97, 0x73e7, 0x73cf, 0x5bc9, 0x79cf, 0x79ef, 0x7249, 0x7bef, 0x7bcf, 0x2bed, 0x6bae, 0x3923, 0x6b6e,
    0x79a7, 0x79a4, 0x396b, 0x5bed, 0x7497, 0x126a, 0x5bad, 0x4927, 0x5fed, 0x6b6d, 0x2b6a, 0x6ba4, 0x2b73, 0x6bad,
    0x388e, 0x7492, 0x5b6f, 0x5b6a, 0x5bfd, 0x5aad, 0x5a92, 0x72a7, 0x01c0, 0x0002, 0x0e38, 0x0410, 0x12a4,
};
_Static_assert(sizeof GLYPH / sizeof GLYPH[0] == sizeof GLYPH_CH - 1, "one glyph per character");

static const char *const TEXT[] = {"THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789", "TINY PAPER TEST CARD"};

static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

/* Text line: is (x, y) an ink pixel of `s` drawn at (x0, y0), scale k (4 px advance per character at 1x)? */
static int text_px(const char *s, int x0, int y0, int k, int x, int y)
{
    if (y < y0 || y >= y0 + 5 * k || x < x0)
        return 0;
    int col = (x - x0) / k, i = col / 4, c = col % 4, r = (y - y0) / k;
    if (c == 3 || i >= (int)strlen(s) || i >= 100)
        return 0;
    const char *p = s[i] ? strchr(GLYPH_CH, s[i]) : NULL;
    if (!p)
        return 0;
    return (GLYPH[p - GLYPH_CH] >> (14 - (r * 3 + c))) & 1;
}

uint8_t bwc_px(const char *label, int x, int y)
{
    if (y < 80) /* solids */
        return (const uint8_t[]){W, K, R, Y}[x / 100];
    if (y >= 88 && y < 168) { /* 50% mixes */
        static const uint8_t mix[6][2] = {{K, W}, {R, W}, {Y, W}, {R, Y}, {K, R}, {K, Y}};
        int i = x * 6 / BWC_W;
        return mix[i][(x + y) & 1];
    }
    if (y >= 176 && y < 256 && (y - 176) % 28 < 24) { /* gradients, left to right */
        static const uint8_t from[3] = {K, W, W}, to[3] = {W, Y, R};
        int g = (y - 176) / 28, level = x * 17 / BWC_W;
        return BAYER[y & 3][x & 3] < level ? to[g] : from[g];
    }
    if (y >= 262) {
        int ink = text_px(TEXT[0], BWC_TEXT_X, 262, 1, x, y) || text_px(TEXT[1], BWC_TEXT_X, 270, 2, x, y) ||
                  text_px(label ? label : "", BWC_TEXT_X, 284, 2, x, y);
        return ink ? K : W;
    }
    return W;
}

void bwc_row(const char *label, int y, uint8_t *row)
{
    memset(row, 0, BWC_W / 4);
    for (int x = 0; x < BWC_W; x++)
        row[x >> 2] |= (uint8_t)(bwc_px(label, x, y) << (6 - 2 * (x & 3)));
}
