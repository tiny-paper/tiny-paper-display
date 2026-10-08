/* usbdisp tests: the fake device (link, heap, clock and a panel that keeps what its glass shows), requests and a PNG
 * writer, shared by the test files of this folder. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tiny/cbor.h"
#include "tiny/util.h"
#include "unity.h"
#include "usbdisp.h"

/* ------------------------------------------------------------------ the fake device */

#define MAXPIX (64 * 64)
static struct {
    uint8_t in[1 << 16], out[1 << 16];
    size_t in_len, in_pos, out_len;
    uint64_t now;
    int allocs, live, fail_at; /* fail_at: the allocation (0-based, counted from the arm) that fails; -1 none */
    uint8_t shown[MAXPIX], pending[MAXPIX];
    uint8_t depth;
    bool bwry, image;
    int begins, refreshes, sleeps, order_errors;
    /* partial updates: the glass changes only where the driver is told to drive */
    usbdisp_drive_t drive;
    int fulls, partials;   /* refreshes of each kind */
    int stale;             /* pixels of a partial whose `prev` was not what the glass showed */
    int driven;            /* pixels the last partial drove */
    tiny_rect_t rect;      /* of the last partial */
    uint8_t hist[4][MAXPIX]; /* the glass after each of the last refreshes, newest first */
    bool gates;            /* the panel counts gate clocks */
    usbdisp_gates_t gate;
    int kv;                /* the stored full_every, -1 none */
    int kv_sets, fail_kv;
    uint16_t next_row; /* the row the next band must start at (top first) or end at (bottom first) */
    int fail_begin, fail_write, fail_refresh;
    uint32_t refresh_us;
    uint32_t alloc_us; /* the clock moves this much at every allocation */
    const uint8_t *during;
    size_t during_len;
    char log[256];
} F;
static usbdisp_t D;
static usbdisp_cfg_t C;

static inline long f_read(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    (void)ctx, (void)timeout_ms;
    size_t n = F.in_len - F.in_pos < cap ? F.in_len - F.in_pos : cap;
    memcpy(buf, F.in + F.in_pos, n);
    F.in_pos += n;
    return (long)n;
}

static inline long f_write(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    TEST_ASSERT_TRUE(F.out_len + len <= sizeof F.out);
    memcpy(F.out + F.out_len, buf, len);
    F.out_len += len;
    return (long)len;
}

static inline void *f_alloc(void *ctx, size_t n)
{
    (void)ctx;
    if (F.fail_at >= 0 && F.allocs++ == F.fail_at)
        return NULL;
    F.now += F.alloc_us; /* the work before the panel takes time too */
    F.live++;
    return malloc(n ? n : 1);
}

static inline void f_free(void *ctx, void *p)
{
    (void)ctx;
    TEST_ASSERT_NOT_NULL(p);
    F.live--;
    free(p);
}

static inline uint64_t f_now(void *ctx)
{
    (void)ctx;
    return F.now;
}

static inline int f_begin(void *ctx, uint8_t depth, bool bwry, bool image, usbdisp_drive_t drive)
{
    (void)ctx;
    F.begins++;
    F.drive = drive;
    if (drive != USBDISP_DRIVE_FULL && depth != F.depth)
        F.order_errors++; /* a partial at another depth than the glass was driven at */
    F.depth = depth;
    F.bwry = bwry;
    F.image = image;
    F.next_row = C.rows_bottom_up ? C.h : 0;
    if (drive == USBDISP_DRIVE_FULL)
        memset(F.pending, 0xEE, sizeof F.pending);
    else
        memcpy(F.pending, F.shown, sizeof F.pending);
    F.driven = 0;
    if (F.fail_begin > 1) { /* n: the first n - 1 calls pass, every later one fails */
        F.fail_begin--;
        return 0;
    }
    return F.fail_begin ? -1 : 0;
}

static inline uint8_t f_px(const uint8_t *rows, size_t stride, uint16_t i, uint16_t j)
{
    unsigned bit = (unsigned)i * F.depth;
    return (uint8_t)(rows[j * stride + bit / 8] >> (8 - F.depth - bit % 8) & ((1 << F.depth) - 1));
}

static inline int f_write_rows(void *ctx, const tiny_rect_t *r, const uint8_t *prev, const uint8_t *rows, size_t stride)
{
    (void)ctx;
    if (F.fail_write)
        return -1;
    if (F.drive != USBDISP_DRIVE_FULL) { /* as hal_panel_write: prev and rows at the rectangle's first byte */
        if (!prev || r->x % 8 || r->w % 8 || !r->w || !r->h || r->x + r->w > C.w || r->y + r->h > C.h) {
            F.order_errors++;
            return -1;
        }
        F.rect = *r;
        for (uint16_t j = 0; j < r->h; j++)
            for (uint16_t i = 0; i < r->w; i++) {
                size_t at = (size_t)(r->y + j) * C.w + (size_t)(r->x + i);
                uint8_t was = f_px(prev, stride, i, j), now = f_px(rows, stride, i, j);
                F.stale += was != F.shown[at];
                if (F.drive == USBDISP_DRIVE_PARTIAL_ALL || was != now) {
                    F.pending[at] = now;
                    F.driven++;
                }
            }
        return 0;
    }
    if (prev || r->x != 0 || r->w != C.w || (C.rows_bottom_up ? r->y + r->h != F.next_row : r->y != F.next_row))
        F.order_errors++;
    F.next_row = (uint16_t)(C.rows_bottom_up ? r->y : r->y + r->h);
    for (uint16_t j = 0; j < r->h; j++)
        for (uint16_t i = 0; i < r->w; i++) {
            unsigned bit = (unsigned)i * F.depth;
            F.pending[(size_t)(r->y + j) * C.w + i] =
                (uint8_t)(rows[j * stride + bit / 8] >> (8 - F.depth - bit % 8) & ((1 << F.depth) - 1));
        }
    return 0;
}

static inline int f_refresh(void *ctx)
{
    (void)ctx;
    if (F.during) { /* requests that arrive while the panel is busy */
        memcpy(F.in + F.in_len, F.during, F.during_len);
        F.in_len += F.during_len;
        F.during = NULL;
        usbdisp_busy(&D);
    }
    F.now += F.refresh_us;
    if (F.fail_refresh > 1)
        F.fail_refresh--;
    else if (F.fail_refresh)
        return -1;
    if (F.drive == USBDISP_DRIVE_FULL && F.next_row != (C.rows_bottom_up ? 0 : C.h))
        F.order_errors++;
    memcpy(F.shown, F.pending, sizeof F.shown);
    memmove(F.hist[1], F.hist[0], 3 * sizeof F.hist[0]);
    memcpy(F.hist[0], F.shown, sizeof F.shown);
    F.refreshes++;
    if (F.drive == USBDISP_DRIVE_FULL)
        F.fulls++;
    else
        F.partials++;
    return 0;
}

static inline bool f_gates(void *ctx, usbdisp_gates_t *g)
{
    (void)ctx;
    *g = F.gate;
    return F.gates;
}

static inline int f_kv_get(void *ctx, const char *key, void *buf, size_t cap)
{
    (void)ctx;
    if (strcmp(key, USBDISP_KV_FULL_EVERY) || F.kv < 0 || !cap)
        return -1;
    *(uint8_t *)buf = (uint8_t)F.kv;
    return 1;
}

static inline int f_kv_set(void *ctx, const char *key, const void *buf, size_t len)
{
    (void)ctx;
    if (F.fail_kv || strcmp(key, USBDISP_KV_FULL_EVERY) || len != 1)
        return -1;
    F.kv = *(const uint8_t *)buf;
    F.kv_sets++;
    return 0;
}

static inline void f_sleep(void *ctx)
{
    (void)ctx;
    F.sleeps++;
}

static inline void f_log(void *ctx, const char *line)
{
    (void)ctx;
    snprintf(F.log, sizeof F.log, "%s", line);
}

static inline void device(uint16_t w, uint16_t h, uint8_t depths, bool bwry)
{
    usbdisp_reset(&D); /* a transfer a test left open (D starts zeroed) */
    memset(&F, 0, sizeof F);
    F.fail_at = -1;
    F.kv = -1;
    memset(F.shown, 0xAA, sizeof F.shown);
    C = (usbdisp_cfg_t){.fw = "0.0.0+vec", .board = bwry ? "vec_bwry" : "vec_gray", .w = w, .h = h, .depths = depths,
                        .bwry = bwry, .band_rows = 3, .read = f_read, .write = f_write, .alloc = f_alloc,
                        .free = f_free, .now_us = f_now, .panel_begin = f_begin, .panel_write = f_write_rows,
                        .panel_refresh = f_refresh, .panel_sleep = f_sleep, .log = f_log};
    usbdisp_init(&D, &C);
}

/* A gray panel that refreshes partially at `partial_depths`; all: it can drive every pixel of a window. */
static inline void device_more(uint8_t partial_depths, bool all, uint8_t every)
{
    C.partial_depths = partial_depths;
    C.partial_all = all;
    C.full_every = every;
    C.panel_gates = f_gates;
    C.kv_get = f_kv_get;
    C.kv_set = f_kv_set;
    usbdisp_init(&D, &C);
}

static inline void device_partial(uint16_t w, uint16_t h, uint8_t depths, uint8_t partial_depths, bool all, uint8_t every)
{
    device(w, h, depths, false);
    device_more(partial_depths, all, every);
}

static inline void to_device(const uint8_t *b, size_t n)
{
    if (F.in_pos == F.in_len)
        F.in_pos = F.in_len = 0;
    TEST_ASSERT_TRUE(F.in_len + n <= sizeof F.in);
    memcpy(F.in + F.in_len, b, n);
    F.in_len += n;
}

/* One request; the answer's payload (the last frame the device wrote). */
static uint8_t s_seq;
static inline const uint8_t *request(uint8_t cmd, const uint8_t *payload, size_t len, size_t *rlen)
{
    static uint8_t fr[LINK_FRAME_MAX];
    s_seq = (uint8_t)(s_seq % 255 + 1);
    size_t n = link_frame_encode(fr, sizeof fr, s_seq, cmd, payload, len);
    TEST_ASSERT_TRUE(n > 0);
    F.out_len = 0;
    to_device(fr, n);
    usbdisp_poll(&D, 0);
    TEST_ASSERT_TRUE_MESSAGE(F.out_len >= LINK_FRAME_OVERHEAD, "no answer");
    size_t plen = (size_t)(F.out[2] | F.out[3] << 8);
    TEST_ASSERT_EQUAL_UINT(plen + LINK_FRAME_OVERHEAD, F.out_len);
    TEST_ASSERT_EQUAL_HEX8(s_seq, F.out[4]);
    TEST_ASSERT_EQUAL_HEX8(cmd | 0x80, F.out[5]);
    TEST_ASSERT_TRUE(cbor_valid(F.out + 6, plen));
    *rlen = plen;
    return F.out + 6;
}

static inline int64_t map_int(const uint8_t *m, size_t n, const char *key, int64_t def)
{
    const uint8_t *v;
    size_t vn;
    cbor_r_t r;
    cbor_item_t it;
    int64_t out;
    if (!cbor_map_get(m, n, key, strlen(key), &v, &vn))
        return def;
    cbor_r_init(&r, v, vn);
    return cbor_read(&r, &it) && cbor_as_int(&it, &out) ? out : def;
}

/* The err of a request's answer, 0 for a success map. The answer stays in s_ans. */
static const uint8_t *s_ans;
static size_t s_ans_len;
static inline int req_err(uint8_t cmd, const uint8_t *payload, size_t len)
{
    s_ans = request(cmd, payload, len, &s_ans_len);
    return (int)map_int(s_ans, s_ans_len, "err", 0);
}

/* A text value of the last answer ("" when absent). */
static inline const char *ans_str(const char *key)
{
    static char out[4][128];
    static int k;
    const uint8_t *v;
    size_t vn;
    cbor_r_t r;
    cbor_item_t it;
    char *o = out[k = (k + 1) % 4];
    o[0] = 0;
    if (cbor_map_get(s_ans, s_ans_len, key, strlen(key), &v, &vn) && (cbor_r_init(&r, v, vn), cbor_read(&r, &it)) &&
        it.type == CBOR_TSTR && it.len < sizeof out[0]) {
        memcpy(o, it.ptr, it.len);
        o[it.len] = 0;
    }
    return o;
}

/* show_begin (x < 0: no rectangle; mode NULL: none; passes 0: none), the chunks, show_end; the err of the first
 * refusal, 0 when shown. */
static inline int show_at(const uint8_t *png, size_t n, size_t chunk, int x, int y, const char *mode, int passes)
{
    static uint8_t p[LINK_MAX_PAYLOAD];
    cbor_w_t w;
    cbor_w_init(&w, p, sizeof p);
    cbor_put_map(&w, 1u + (x >= 0 ? 2 : 0) + (mode != NULL) + (passes != 0));
    cbor_put_str(&w, "size");
    cbor_put_uint(&w, n);
    if (x >= 0) {
        cbor_put_str(&w, "x");
        cbor_put_uint(&w, (uint64_t)x);
        cbor_put_str(&w, "y");
        cbor_put_uint(&w, (uint64_t)y);
    }
    if (mode) {
        cbor_put_str(&w, "mode");
        cbor_put_str(&w, mode);
    }
    if (passes) {
        cbor_put_str(&w, "passes");
        cbor_put_uint(&w, (uint64_t)passes);
    }
    int e = req_err(USBDISP_CMD_SHOW_BEGIN, p, w.len);
    for (size_t off = 0; !e && off < n; off += chunk) {
        cbor_w_init(&w, p, sizeof p);
        cbor_put_map(&w, 2);
        cbor_put_str(&w, "off");
        cbor_put_uint(&w, off);
        cbor_put_str(&w, "data");
        cbor_put_bstr(&w, png + off, n - off < chunk ? n - off : chunk);
        TEST_ASSERT_FALSE(w.err);
        e = req_err(USBDISP_CMD_SHOW_CHUNK, p, w.len);
    }
    return e ? e : req_err(USBDISP_CMD_SHOW_END, NULL, 0);
}

static inline int show(const uint8_t *png, size_t n, size_t chunk) { return show_at(png, n, chunk, -1, 0, NULL, 0); }

/* ------------------------------------------------------------------ a PNG writer (stored deflate blocks) */

static inline uint8_t *put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
    return p + 4;
}

static inline uint8_t *png_chunk(uint8_t *p, const char *kind, const uint8_t *data, size_t n)
{
    p = put32(p, (uint32_t)n);
    memcpy(p, kind, 4);
    if (n)
        memcpy(p + 4, data, n);
    return put32(p + 4 + n, tiny_crc32(0, p, 4 + n));
}

/* px: one byte a pixel (a level of `bits`, or a palette index with ct 3). Returns the PNG's length. */
static inline size_t make_png(uint8_t *out, uint16_t w, uint16_t h, uint8_t bits, uint8_t ct, const uint8_t *px)
{
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    static const uint8_t pal[12] = {0, 0, 0, 255, 0, 0, 255, 255, 0, 255, 255, 255};
    size_t stride = ((size_t)w * bits + 7) / 8, raw_n = (stride + 1) * h;
    uint8_t *raw = calloc(1, raw_n), *z = malloc(raw_n + raw_n / 65535 * 5 + 16), *zp = z, *p = out;
    for (uint16_t y = 0; y < h; y++)
        for (uint16_t x = 0; x < w; x++) {
            unsigned bit = (unsigned)x * bits;
            raw[y * (stride + 1) + 1 + bit / 8] |= (uint8_t)(px[(size_t)y * w + x] << (8 - bits - bit % 8));
        }
    *zp++ = 0x78, *zp++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++)
        a = (a + raw[i]) % 65521, b = (b + a) % 65521;
    for (size_t off = 0; off < raw_n;) {
        size_t k = raw_n - off < 65535 ? raw_n - off : 65535;
        *zp++ = off + k == raw_n;
        *zp++ = (uint8_t)k, *zp++ = (uint8_t)(k >> 8), *zp++ = (uint8_t)~k, *zp++ = (uint8_t)(~k >> 8);
        memcpy(zp, raw + off, k);
        zp += k, off += k;
    }
    zp = put32(zp, b << 16 | a);
    uint8_t ih[13];
    put32(ih, w), put32(ih + 4, h);
    ih[8] = bits, ih[9] = ct, ih[10] = ih[11] = ih[12] = 0;
    memcpy(p, sig, 8);
    p = png_chunk(p + 8, "IHDR", ih, 13);
    if (ct == 3)
        p = png_chunk(p, "PLTE", pal, sizeof pal);
    p = png_chunk(p, "IDAT", z, (size_t)(zp - z));
    p = png_chunk(p, "IEND", NULL, 0);
    free(raw);
    free(z);
    return (size_t)(p - out);
}

static uint32_t s_rng = 0x2545F491u;
static inline uint32_t rnd(void)
{
    s_rng ^= s_rng << 13, s_rng ^= s_rng >> 17, s_rng ^= s_rng << 5;
    return s_rng;
}

static uint8_t s_px[MAXPIX], s_png[8192];
static inline size_t pattern_png(uint16_t w, uint16_t h, uint8_t bits, uint8_t ct)
{
    for (size_t i = 0; i < (size_t)w * h; i++)
        s_px[i] = (uint8_t)(rnd() % (1u << bits));
    return make_png(s_png, w, h, bits, ct, s_px);
}
