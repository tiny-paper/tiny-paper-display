/* usbdisp: the frame a PNG becomes, byte for byte as the per-pixel conversion wrote it, and its fingerprint as
 * usb-display.md defines it: 1, 2 and 4 bit pages (all 16 levels, noise, a dashboard) through real deflate streams
 * (zlib levels 0..9, windows 2^9..2^15, every row filter, IDAT split anywhere), whole panels and rectangles at odd rows
 * and widths, the color panel's 1-bit gray. SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>

#include "frame_pages.h"
#include "unity.h"
#include "usbdisp_priv.h"

void setUp(void) {}
void tearDown(void) {}

static void *t_alloc(void *ctx, size_t n) { return (void)ctx, malloc(n ? n : 1); }
static void t_free(void *ctx, void *p) { (void)ctx, free(p); }
static uint64_t t_now(void *ctx) { return (void)ctx, 0; }

static usbdisp_cfg_t cfg(uint16_t W, uint16_t H, bool bwry)
{
    usbdisp_cfg_t c;
    memset(&c, 0, sizeof c);
    c.w = W, c.h = H, c.depths = bwry ? 1 : 7, c.bwry = bwry;
    c.alloc = t_alloc, c.free = t_free, c.now_us = t_now;
    return c;
}

/* A w x h picture of `kind` at (x, y) of a W x H panel whose frame holds random bytes; the frame must equal the
 * reference everywhere, and (whole panel) the fingerprint the reference fingerprint. */
static void check(uint16_t W, uint16_t H, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t bits, bool bwry,
                  int kind, int filter, int level, int wbits, unsigned split)
{
    char what[160];
    snprintf(what, sizeof what, "%ux%u at %u,%u of %ux%u, %u bit%s, page %d, filter %d, zlib %d/%d, split %u", w, h, x,
             y, W, H, bits, bwry ? " bwry" : "", kind, filter, level, wbits, split);
    usbdisp_cfg_t c = cfg(W, H, bwry);
    uint8_t *px = malloc((size_t)w * h);
    page_levels(px, w, h, bits, kind);
    size_t n;
    uint8_t *png = page_png(px, w, h, bits, filter, level, wbits, split, &n);
    usbdisp_pnginfo_t pi;
    const char *why = "";
    bool area = w != W || h != H;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, usbdisp_png_check(&c, png, n, area, &pi, &why), what);
    uint8_t depth = pi.depth;
    size_t stride = ((size_t)W * depth + 7) / 8, bytes = stride * H;
    uint8_t *got = malloc(bytes), *want = malloc(bytes);
    for (size_t i = 0; i < bytes; i++)
        got[i] = want[i] = (uint8_t)fp_rnd();
    size_t at = (size_t)y * stride + (size_t)x * depth / 8;
    page_reference(px, w, h, bits, bwry, want + at, stride);
    usbdisp_png_time_t t;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, usbdisp_png_frame(&c, png, n, &pi, got + at, stride, &why, &t), what);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(want, got, bytes, what);
    if (!area) {
        usbdisp_t d;
        memset(&d, 0, sizeof d);
        d.c = c;
        char fa[17], fb[17], fw[17], fwhite[17];
        usbdisp_fingerprint(&d, depth, got, fa);
        page_fingerprint(W, H, depth, bwry, want, fb);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(fb, fa, what);
        usbdisp_fingerprint(&d, depth, NULL, fw); /* white, as after a clear */
        page_fingerprint(W, H, depth, bwry, NULL, fwhite);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(fwhite, fw, what);
    }
    free(px), free(png), free(got), free(want);
}

static void test_whole_panel_every_depth(void)
{
    static const uint8_t bits[] = {1, 2, 4};
    for (int b = 0; b < 3; b++)
        for (int kind = PAGE_LEVELS; kind <= PAGE_MIX; kind++)
            check(203, 77, 0, 0, 203, 77, bits[b], false, kind, -1, kind == PAGE_NOISE ? 6 : 9, 15, 0);
}

static void test_deflate_kinds(void)
{
    static const uint8_t bits[] = {1, 2, 4};
    for (int b = 0; b < 3; b++)
        for (int level = 0; level <= 9; level++)
            for (int wbits = 9; wbits <= 15; wbits += 3)
                check(131, 41, 0, 0, 131, 41, bits[b], false, level % 2 ? PAGE_MIX : PAGE_LEVELS, level % 5 ? -1 : 0,
                      level, wbits, level % 3 ? 0 : 7);
}

static void test_every_filter(void)
{
    for (int f = 0; f <= 4; f++)
        for (uint8_t bits = 1; bits <= 4; bits *= 2)
            check(97, 33, 0, 0, 97, 33, bits, false, PAGE_MIX, f, 9, 15, 0);
}

static void test_rectangles(void)
{
    /* x in steps of 8 (usb-display.md), odd rows, odd and even widths, one pixel wide or high */
    static const uint16_t r[][4] = {{8, 3, 37, 11}, {0, 1, 1, 1}, {16, 5, 63, 1}, {24, 7, 1, 19}, {40, 0, 88, 17},
                                    {8, 13, 121, 9}};
    for (size_t i = 0; i < sizeof r / sizeof r[0]; i++)
        for (uint8_t bits = 1; bits <= 4; bits *= 2)
            for (int kind = PAGE_LEVELS; kind <= PAGE_NOISE; kind++)
                check(203, 77, r[i][0], r[i][1], r[i][2], r[i][3], bits, false, kind, -1, 9, 15, 0);
}

static void test_color_panel_gray(void)
{
    check(203, 77, 0, 0, 203, 77, 1, true, PAGE_MIX, -1, 9, 15, 0);
}

static void test_full_size_pages(void)
{
    for (uint8_t bits = 1; bits <= 4; bits *= 2)
        check(1872, 1404, 0, 0, 1872, 1404, bits, false, PAGE_MIX, 0, 9, 15, 0);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_whole_panel_every_depth);
    RUN_TEST(test_deflate_kinds);
    RUN_TEST(test_every_filter);
    RUN_TEST(test_rectangles);
    RUN_TEST(test_color_panel_gray);
    RUN_TEST(test_full_size_pages);
    return UNITY_END();
}
