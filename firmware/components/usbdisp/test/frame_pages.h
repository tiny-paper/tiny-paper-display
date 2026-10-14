/* usbdisp tests: test pages as real PNGs (zlib deflate at any level and window, every row filter, IDAT split at
 * random places), the reference frame of the per-pixel conversion and the reference fingerprint. Shared by
 * frame_exact.c (the guard) and frame_bench.c (the timing). SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "tiny/util.h"

static uint32_t fp_rng = 0x9E3779B9u;
static inline uint32_t fp_rnd(void)
{
    fp_rng ^= fp_rng << 13, fp_rng ^= fp_rng >> 17, fp_rng ^= fp_rng << 5;
    return fp_rng;
}

enum { PAGE_LEVELS, PAGE_NOISE, PAGE_DOC, PAGE_MIX };

/* One level (0 .. 2^bits - 1) a pixel. LEVELS: every one of the 16 levels in bands and steps (scaled to the depth);
 * NOISE: random; DOC: white with black and gray blocks and lines (what a dashboard looks like); MIX: DOC with noise. */
static inline void page_levels(uint8_t *px, uint16_t w, uint16_t h, uint8_t bits, int kind)
{
    const unsigned m = (1u << bits) - 1;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            unsigned v;
            if (kind == PAGE_LEVELS) {
                v = ((x / 7 + y / 5) & 15) * m / 15;
            } else if (kind == PAGE_NOISE) {
                v = fp_rnd() & m;
            } else {
                v = m; /* white */
                if ((y / 40) % 3 == 1 && (x / 60) % 2 == 0)
                    v = 0;
                if ((y % 97) < 3 || (x % 211) < 2)
                    v = ((x / 113) & 15) * m / 15;
                if (y % 40 > 10 && y % 40 < 30 && (x * 7 + y * 3) % 11 < 3)
                    v = 0; /* text-like specks */
                if (kind == PAGE_MIX && x > w / 2 && y > h / 2)
                    v = fp_rnd() & m;
            }
            px[y * w + x] = (uint8_t)v;
        }
}

static inline uint8_t *fp_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
    return p + 4;
}

static inline uint8_t *fp_chunk(uint8_t *p, const char *kind, const uint8_t *data, size_t n)
{
    p = fp_put32(p, (uint32_t)n);
    memcpy(p, kind, 4);
    if (n)
        memcpy(p + 4, data, n);
    return fp_put32(p + 4 + n, tiny_crc32(0, p, 4 + n));
}

static inline uint8_t fp_paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

/* A gray PNG (ct 0) of px. filter: 0..4 every row, -1 a random filter a row. level: zlib 0..9; wbits: 9..15.
 * split: 0 one IDAT, else IDAT chunks of 1..split bytes. Returns a malloc'd PNG, its length in *n. */
static inline uint8_t *page_png(const uint8_t *px, uint16_t w, uint16_t h, uint8_t bits, int filter, int level,
                                int wbits, unsigned split, size_t *n)
{
    size_t stride = ((size_t)w * bits + 7) / 8, raw_n = (stride + 1) * h;
    uint8_t *raw = calloc(1, raw_n), *row = calloc(1, stride), *prev = calloc(1, stride);
    for (uint32_t y = 0; y < h; y++) {
        memset(row, 0, stride);
        for (uint32_t x = 0; x < w; x++) {
            unsigned bit = x * bits;
            row[bit / 8] |= (uint8_t)(px[y * w + x] << (8 - bits - bit % 8));
        }
        if (fp_rnd() & 1) /* padding bits are unspecified in PNG: put ones there on some rows */
            row[stride - 1] |= (uint8_t)((1u << ((8 - (w * bits) % 8) % 8)) - 1);
        int f = filter < 0 ? (int)(fp_rnd() % 5) : filter;
        uint8_t *o = raw + y * (stride + 1);
        size_t bpp = 1; /* gray at 1, 2, 4 bits: one byte */
        o[0] = (uint8_t)f;
        for (size_t i = 0; i < stride; i++) {
            int a = i >= bpp ? row[i - bpp] : 0, b = y ? prev[i] : 0, c = i >= bpp && y ? prev[i - bpp] : 0, p = 0;
            p = f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) >> 1 : f == 4 ? fp_paeth(a, b, c) : 0;
            o[1 + i] = (uint8_t)(row[i] - p);
        }
        memcpy(prev, row, stride);
    }
    uLong zcap = compressBound(raw_n) + 64;
    uint8_t *z = malloc(zcap);
    z_stream s;
    memset(&s, 0, sizeof s);
    deflateInit2(&s, level, Z_DEFLATED, wbits, 9, Z_DEFAULT_STRATEGY);
    s.next_in = raw, s.avail_in = (uInt)raw_n, s.next_out = z, s.avail_out = (uInt)zcap;
    deflate(&s, Z_FINISH);
    size_t zn = s.total_out;
    deflateEnd(&s);
    uint8_t *out = malloc(zn + zn / 1 * (split ? 12 : 0) + 256), *p = out;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    uint8_t ih[13];
    fp_put32(ih, w), fp_put32(ih + 4, h);
    ih[8] = bits, ih[9] = 0, ih[10] = ih[11] = ih[12] = 0;
    memcpy(p, sig, 8);
    p = fp_chunk(p + 8, "IHDR", ih, 13);
    for (size_t off = 0; off < zn;) {
        size_t k = split ? 1 + fp_rnd() % split : zn;
        if (k > zn - off)
            k = zn - off;
        p = fp_chunk(p, "IDAT", z + off, k);
        off += k;
    }
    p = fp_chunk(p, "IEND", NULL, 0);
    free(raw), free(row), free(prev), free(z);
    *n = (size_t)(p - out);
    return out;
}

/* The frame the per-pixel conversion wrote (the code before the packed-row copy): for each row of the picture at
 * `at`, the bytes of its width set to zero, then every level ORed in, MSB first. bwry: 1-bit gray as codes 0 / 3. */
static inline void page_reference(const uint8_t *px, uint16_t w, uint16_t h, uint8_t bits, bool bwry, uint8_t *at,
                                  size_t stride)
{
    uint8_t depth = bwry ? 2 : bits;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = at + y * stride;
        memset(row, 0, ((size_t)w * depth + 7) / 8);
        for (uint32_t x = 0; x < w; x++) {
            unsigned v = bwry ? (px[y * w + x] ? 3u : 0u) : px[y * w + x], bit = x * depth;
            row[bit / 8] |= (uint8_t)(v << (8 - depth - bit % 8));
        }
    }
}

/* The fingerprint as usb-display.md defines it, byte by byte (FNV-1a 64 over the head and the masked rows). */
static inline void page_fingerprint(uint16_t W, uint16_t H, uint8_t depth, bool bwry, const uint8_t *rows,
                                    char out[17])
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    uint8_t head[] = {(uint8_t)(W >> 8), (uint8_t)W, (uint8_t)(H >> 8), (uint8_t)H, depth, bwry};
    for (size_t i = 0; i < sizeof head; i++)
        hash = (hash ^ head[i]) * UINT64_C(0x100000001b3);
    size_t stride = ((size_t)W * depth + 7) / 8;
    unsigned padding = (8 - (W * (unsigned)depth) % 8) % 8;
    for (size_t y = 0; y < H; y++)
        for (size_t x = 0; x < stride; x++) {
            uint8_t b = rows ? rows[y * stride + x] : 0xFF;
            if (x + 1 == stride)
                b &= (uint8_t)(0xFFu << padding);
            hash = (hash ^ b) * UINT64_C(0x100000001b3);
        }
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; i++)
        out[i] = hex[(hash >> (60 - 4 * i)) & 15];
    out[16] = 0;
}
