/* usbdisp: host timing of the show path's steps on a 1872 x 1404 page (TRMNL X) at 1, 2 and 4 bit, as the API sends
 * it (filter None, zlib level 9): inflate, convert (usbdisp_png_frame's own split) and fingerprint, the best of 5 runs
 * in ms. Relative only: the device numbers are the ones that count. Not a test; build target usbdisp_frame_bench
 * (Release, no sanitizers: cmake -S host -B build/bench -DCMAKE_BUILD_TYPE=Release -DTINY_SANITIZE=OFF).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 199309L /* clock_gettime */
#include <stdio.h>
#include <time.h>

#include "frame_pages.h"
#include "usbdisp_priv.h"

static void *b_alloc(void *ctx, size_t n) { return (void)ctx, malloc(n ? n : 1); }
static void b_free(void *ctx, void *p) { (void)ctx, free(p); }
static uint64_t b_now(void *ctx)
{
    (void)ctx;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

int main(void)
{
    const uint16_t W = 1872, H = 1404;
    usbdisp_cfg_t c;
    memset(&c, 0, sizeof c);
    c.w = W, c.h = H, c.depths = 7, c.alloc = b_alloc, c.free = b_free, c.now_us = b_now;
    usbdisp_t d;
    memset(&d, 0, sizeof d);
    d.c = c;
    uint8_t *px = malloc((size_t)W * H);
    printf("depth  png_kb  inflate_ms  convert_ms  fingerprint_ms\n");
    for (uint8_t bits = 1; bits <= 4; bits *= 2) {
        page_levels(px, W, H, bits, PAGE_MIX);
        size_t n;
        uint8_t *png = page_png(px, W, H, bits, 0, 9, 15, 0, &n);
        usbdisp_pnginfo_t pi;
        const char *why = "";
        if (usbdisp_png_check(&c, png, n, false, &pi, &why))
            return printf("check: %s\n", why), 1;
        size_t stride = ((size_t)W * bits + 7) / 8;
        uint8_t *frame = malloc(stride * H);
        double inf = 1e9, conv = 1e9, fpt = 1e9;
        for (int run = 0; run < 5; run++) {
            usbdisp_png_time_t t;
            if (usbdisp_png_frame(&c, png, n, &pi, frame, stride, &why, &t))
                return printf("frame: %s\n", why), 1;
            char fp[17];
            uint64_t t0 = b_now(NULL);
            usbdisp_fingerprint(&d, bits, frame, fp);
            double f = (double)(b_now(NULL) - t0) / 1000.0;
            inf = (double)t.inflate_us / 1000.0 < inf ? (double)t.inflate_us / 1000.0 : inf;
            conv = (double)t.convert_us / 1000.0 < conv ? (double)t.convert_us / 1000.0 : conv;
            fpt = f < fpt ? f : fpt;
        }
        printf("%5u  %6zu  %10.1f  %10.1f  %14.1f\n", bits, n / 1024, inf, conv, fpt);
        free(png), free(frame);
    }
    free(px);
    return 0;
}
