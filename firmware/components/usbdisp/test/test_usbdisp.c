/* usbdisp: the USB display protocol against a fake link, heap, clock and panel, replaying docs/spec/vectors/
 * usb-display.json; the failure paths (allocation, a quiet transfer, half a frame, the panel driver), a mutation fuzz of
 * the PNG entry, and a picture pixel for pixel through the simulator's HAL. SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if TINY_HAVE_SIM_HAL /* set by hal/sim/host.cmake: a tree with only this component's fakes has no simulator */
#include "sim.h"
#endif
#include "sim_json.h"
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
    uint16_t next_row; /* the row the next band must start at (top first) or end at (bottom first) */
    int fail_begin, fail_write, fail_refresh;
    uint32_t refresh_us;
    const uint8_t *during;
    size_t during_len;
    char log[256];
} F;
static usbdisp_t D;
static usbdisp_cfg_t C;

static long f_read(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    (void)ctx, (void)timeout_ms;
    size_t n = F.in_len - F.in_pos < cap ? F.in_len - F.in_pos : cap;
    memcpy(buf, F.in + F.in_pos, n);
    F.in_pos += n;
    return (long)n;
}

static long f_write(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    TEST_ASSERT_TRUE(F.out_len + len <= sizeof F.out);
    memcpy(F.out + F.out_len, buf, len);
    F.out_len += len;
    return (long)len;
}

static void *f_alloc(void *ctx, size_t n)
{
    (void)ctx;
    if (F.fail_at >= 0 && F.allocs++ == F.fail_at)
        return NULL;
    F.live++;
    return malloc(n ? n : 1);
}

static void f_free(void *ctx, void *p)
{
    (void)ctx;
    TEST_ASSERT_NOT_NULL(p);
    F.live--;
    free(p);
}

static uint64_t f_now(void *ctx)
{
    (void)ctx;
    return F.now;
}

static int f_begin(void *ctx, uint8_t depth, bool bwry, bool image)
{
    (void)ctx;
    F.begins++;
    F.depth = depth;
    F.bwry = bwry;
    F.image = image;
    F.next_row = C.rows_bottom_up ? C.h : 0;
    memset(F.pending, 0xEE, sizeof F.pending);
    return F.fail_begin ? -1 : 0;
}

static int f_write_rows(void *ctx, const tiny_rect_t *r, const uint8_t *rows, size_t stride)
{
    (void)ctx;
    if (F.fail_write)
        return -1;
    if (r->x != 0 || r->w != C.w || (C.rows_bottom_up ? r->y + r->h != F.next_row : r->y != F.next_row))
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

static int f_refresh(void *ctx)
{
    (void)ctx;
    if (F.during) { /* requests that arrive while the panel is busy */
        memcpy(F.in + F.in_len, F.during, F.during_len);
        F.in_len += F.during_len;
        F.during = NULL;
        usbdisp_busy(&D);
    }
    F.now += F.refresh_us;
    if (F.fail_refresh)
        return -1;
    if (F.next_row != (C.rows_bottom_up ? 0 : C.h))
        F.order_errors++;
    memcpy(F.shown, F.pending, sizeof F.shown);
    F.refreshes++;
    return 0;
}

static void f_sleep(void *ctx)
{
    (void)ctx;
    F.sleeps++;
}

static void f_log(void *ctx, const char *line)
{
    (void)ctx;
    snprintf(F.log, sizeof F.log, "%s", line);
}

static void device(uint16_t w, uint16_t h, uint8_t depths, bool bwry)
{
    usbdisp_reset(&D); /* a transfer a test left open (D starts zeroed) */
    memset(&F, 0, sizeof F);
    F.fail_at = -1;
    memset(F.shown, 0xAA, sizeof F.shown);
    C = (usbdisp_cfg_t){.fw = "0.0.0+vec", .board = bwry ? "vec_bwry" : "vec_gray", .w = w, .h = h, .depths = depths,
                        .bwry = bwry, .band_rows = 3, .read = f_read, .write = f_write, .alloc = f_alloc,
                        .free = f_free, .now_us = f_now, .panel_begin = f_begin, .panel_write = f_write_rows,
                        .panel_refresh = f_refresh, .panel_sleep = f_sleep, .log = f_log};
    usbdisp_init(&D, &C);
}

static void to_device(const uint8_t *b, size_t n)
{
    if (F.in_pos == F.in_len)
        F.in_pos = F.in_len = 0;
    TEST_ASSERT_TRUE(F.in_len + n <= sizeof F.in);
    memcpy(F.in + F.in_len, b, n);
    F.in_len += n;
}

/* One request; the answer's payload (the last frame the device wrote). */
static uint8_t s_seq;
static const uint8_t *request(uint8_t cmd, const uint8_t *payload, size_t len, size_t *rlen)
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

static int64_t map_int(const uint8_t *m, size_t n, const char *key, int64_t def)
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

/* The err of a request's answer, 0 for a success map. */
static int req_err(uint8_t cmd, const uint8_t *payload, size_t len)
{
    size_t n;
    const uint8_t *r = request(cmd, payload, len, &n);
    return (int)map_int(r, n, "err", 0);
}

/* show_begin, the chunks, show_end; the err of the first refusal, 0 when shown. */
static int show(const uint8_t *png, size_t n, size_t chunk)
{
    static uint8_t p[LINK_MAX_PAYLOAD];
    cbor_w_t w;
    cbor_w_init(&w, p, sizeof p);
    cbor_put_map(&w, 1);
    cbor_put_str(&w, "size");
    cbor_put_uint(&w, n);
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

/* ------------------------------------------------------------------ a PNG writer (stored deflate blocks) */

static uint8_t *put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
    return p + 4;
}

static uint8_t *png_chunk(uint8_t *p, const char *kind, const uint8_t *data, size_t n)
{
    p = put32(p, (uint32_t)n);
    memcpy(p, kind, 4);
    if (n)
        memcpy(p + 4, data, n);
    return put32(p + 4 + n, tiny_crc32(0, p, 4 + n));
}

/* px: one byte a pixel (a level of `bits`, or a palette index with ct 3). Returns the PNG's length. */
static size_t make_png(uint8_t *out, uint16_t w, uint16_t h, uint8_t bits, uint8_t ct, const uint8_t *px)
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
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13, s_rng ^= s_rng >> 17, s_rng ^= s_rng << 5;
    return s_rng;
}

static uint8_t s_px[MAXPIX], s_png[8192];
static size_t pattern_png(uint16_t w, uint16_t h, uint8_t bits, uint8_t ct)
{
    for (size_t i = 0; i < (size_t)w * h; i++)
        s_px[i] = (uint8_t)(rnd() % (1u << bits));
    return make_png(s_png, w, h, bits, ct, s_px);
}

/* ------------------------------------------------------------------ vectors */

static size_t unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = strlen(s) / 2;
    TEST_ASSERT_TRUE(n <= cap);
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
    return n;
}

static sj_t *load_vectors(void)
{
    static char text[1 << 18], err[128];
    FILE *f = fopen(TINY_ROOT "/docs/spec/vectors/usb-display.json", "r");
    TEST_ASSERT_NOT_NULL(f);
    size_t n = fread(text, 1, sizeof text - 1, f);
    text[n] = 0;
    fclose(f);
    sj_t *v = sj_parse(text, err, sizeof err);
    TEST_ASSERT_NOT_NULL_MESSAGE(v, err);
    return v;
}

static void test_vectors(void)
{
    static uint8_t tx[LINK_FRAME_MAX], want[2048], dur[1024], px[MAXPIX], before[MAXPIX];
    sj_t *v = load_vectors();
    const sj_t *sessions = sj_get(v, "sessions");
    TEST_ASSERT_EQUAL_UINT(2, sj_len(sessions));
    int steps_run = 0, shown = 0, refused = 0;
    for (size_t si = 0; si < sj_len(sessions); si++) {
        const sj_t *s = sj_at(sessions, si), *panel = sj_get(s, "panel"), *dep = sj_get(panel, "depths");
        uint8_t depths = 0;
        for (size_t i = 0; i < sj_len(dep); i++)
            depths |= (uint8_t)sj_num(sj_at(dep, i), 0);
        device((uint16_t)sj_num(sj_get(panel, "w"), 0), (uint16_t)sj_num(sj_get(panel, "h"), 0), depths,
               sj_len(sj_get(panel, "colors")) == 4);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)sj_num(sj_get(panel, "max_png"), 0), D.c.png_max);
        const sj_t *steps = sj_get(s, "steps");
        for (size_t i = 0; i < sj_len(steps); i++) {
            const sj_t *st = sj_at(steps, i), *during = sj_get(st, "during");
            const char *name = sj_str(sj_get(st, "name"), "?");
            size_t wn = 0, dn = 0;
            for (size_t k = 0; k < sj_len(during); k++) { /* answered during the refresh, before the step's answer */
                dn += unhex(sj_str(sj_get(sj_at(during, k), "tx"), ""), dur + dn, sizeof dur - dn);
                wn += unhex(sj_str(sj_get(sj_at(during, k), "rx"), ""), want + wn, sizeof want - wn);
            }
            F.during = dn ? dur : NULL;
            F.during_len = dn;
            wn += unhex(sj_str(sj_get(st, "rx"), ""), want + wn, sizeof want - wn);
            size_t tn = unhex(sj_str(sj_get(st, "tx"), ""), tx, sizeof tx);
            int refreshes = F.refreshes;
            memcpy(before, F.shown, sizeof before);
            F.out_len = 0;
            to_device(tx, tn);
            usbdisp_poll(&D, 0);
            TEST_ASSERT_EQUAL_UINT_MESSAGE(wn, F.out_len, name);
            TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(want, F.out, wn, name);
            const sj_t *pix = sj_get(st, "pixels");
            if (pix) {
                size_t pn = unhex(sj_str(pix, ""), px, sizeof px);
                TEST_ASSERT_EQUAL_UINT((size_t)C.w * C.h, pn);
                TEST_ASSERT_EQUAL_INT_MESSAGE(refreshes + 1, F.refreshes, name);
                TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)sj_num(sj_get(st, "depth"), 0), F.depth, name);
                TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(px, F.shown, pn, name);
                shown++;
            } else { /* a refusal, or a step of a transfer: the picture stays */
                TEST_ASSERT_EQUAL_INT_MESSAGE(refreshes, F.refreshes, name);
                TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(before, F.shown, sizeof before, name);
                refused += sj_get(st, "err") != NULL;
            }
            steps_run++;
        }
        TEST_ASSERT_EQUAL_INT(0, F.order_errors);
        TEST_ASSERT_EQUAL_INT(F.begins, F.sleeps); /* the panel is put to sleep after every use */
        usbdisp_reset(&D);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, F.live, "memory left after the session");
    }
    TEST_ASSERT_TRUE(steps_run > 50 && shown >= 7 && refused >= 20);
    sj_free(v);
}

/* ------------------------------------------------------------------ failure paths */

static void expect_kept(const uint8_t *before, int refreshes)
{
    TEST_ASSERT_EQUAL_INT(refreshes, F.refreshes);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(before, F.shown, sizeof F.shown);
    TEST_ASSERT_FALSE(usbdisp_transfer(&D));
    TEST_ASSERT_EQUAL_INT(0, F.live);
}

/* Every allocation of a show and of a clear fails once: NOMEM, the old picture kept, nothing leaked, and the next
 * show works. */
static void test_alloc_fails_at_each_point(void)
{
    static uint8_t before[MAXPIX];
    for (int kind = 0; kind < 3; kind++) { /* gray, indexed on the color panel, clear */
        int failures = 0;
        for (int k = 0;; k++) {
            device(40, 30, 3, kind == 1);
            size_t n = pattern_png(40, 30, 2, kind == 1 ? 3 : 0);
            TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 100)); /* a picture to keep */
            memcpy(before, F.shown, sizeof before);
            int refreshes = F.refreshes;
            n = pattern_png(40, 30, 2, kind == 1 ? 3 : 0);
            F.allocs = 0;
            F.fail_at = k;
            int e = kind == 2 ? req_err(USBDISP_CMD_CLEAR, NULL, 0) : show(s_png, n, 100);
            bool hit = F.allocs > k;
            F.fail_at = -1;
            if (!hit) { /* k is past the last allocation: the run was a clean one */
                TEST_ASSERT_EQUAL_INT(0, e);
                TEST_ASSERT_EQUAL_INT(refreshes + 1, F.refreshes);
                break;
            }
            TEST_ASSERT_EQUAL_INT(USBDISP_E_NOMEM, e);
            expect_kept(before, refreshes);
            n = pattern_png(40, 30, 2, kind == 1 ? 3 : 0);
            TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 100));
            TEST_ASSERT_EQUAL_HEX8_ARRAY(s_px, F.shown, 40 * 30);
            failures++;
        }
        /* show: the PNG, the frame and the decoder's blocks; clear: its band */
        TEST_ASSERT_TRUE_MESSAGE(failures >= (kind == 2 ? 1 : 6), "fewer allocation points than the code has");
    }
}

/* A transfer that never ends (the cable pulled, the host program gone): dropped after USBDISP_IDLE_US. */
static void test_quiet_transfer_is_dropped(void)
{
    static uint8_t before[MAXPIX];
    device(40, 30, 3, false);
    size_t n = pattern_png(40, 30, 1, 0);
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 64));
    memcpy(before, F.shown, sizeof before);
    int refreshes = F.refreshes;
    uint8_t p[32];
    cbor_w_t w;
    cbor_w_init(&w, p, sizeof p);
    cbor_put_map(&w, 1);
    cbor_put_str(&w, "size");
    cbor_put_uint(&w, n);
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_SHOW_BEGIN, p, w.len));
    TEST_ASSERT_TRUE(usbdisp_transfer(&D));
    F.now += USBDISP_IDLE_US - 1;
    usbdisp_poll(&D, 0);
    TEST_ASSERT_TRUE(usbdisp_transfer(&D));
    F.now += 1;
    usbdisp_poll(&D, 0);
    expect_kept(before, refreshes);
    TEST_ASSERT_NOT_NULL(strstr(F.log, "quiet"));
    TEST_ASSERT_EQUAL_INT(USBDISP_E_SEQUENCE, req_err(USBDISP_CMD_SHOW_END, NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 64));
}

/* Half a frame (a host program that stopped mid-request), then a new program says hello: answered, not read as the
 * old frame's payload. */
static void test_half_frame_is_forgotten(void)
{
    device(40, 30, 3, false);
    static uint8_t big[5000], fr[LINK_FRAME_MAX];
    size_t n = link_frame_encode(fr, sizeof fr, 9, USBDISP_CMD_SHOW_CHUNK, big, sizeof big);
    to_device(fr, 300);
    usbdisp_poll(&D, 0);
    F.now += USBDISP_FRAME_IDLE_US;
    usbdisp_poll(&D, 0);
    TEST_ASSERT_EQUAL_UINT(0, F.out_len);
    size_t rn;
    const uint8_t *r = request(USBDISP_CMD_HELLO, NULL, 0, &rn);
    TEST_ASSERT_EQUAL_INT(40, map_int(r, rn, "w", 0));
    (void)n;
}

static void test_panel_failures(void)
{
    static uint8_t before[MAXPIX];
    for (int step = 0; step < 3; step++) {
        device(40, 30, 3, false);
        size_t n = pattern_png(40, 30, 2, 0);
        TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 4096));
        memcpy(before, F.shown, sizeof before);
        int refreshes = F.refreshes;
        F.fail_begin = step == 0, F.fail_write = step == 1, F.fail_refresh = step == 2;
        n = pattern_png(40, 30, 2, 0);
        TEST_ASSERT_EQUAL_INT(USBDISP_E_IO, show(s_png, n, 4096));
        expect_kept(before, refreshes);
        TEST_ASSERT_EQUAL_INT(USBDISP_E_IO, req_err(USBDISP_CMD_CLEAR, NULL, 0));
        expect_kept(before, refreshes);
        TEST_ASSERT_EQUAL_INT(F.begins, F.sleeps);
        F.fail_begin = F.fail_write = F.fail_refresh = 0;
        TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 4096));
        TEST_ASSERT_EQUAL_HEX8_ARRAY(s_px, F.shown, 40 * 30);
    }
}

static void test_bottom_up_bands(void)
{
    for (uint16_t rows = 0; rows < 40; rows += 7) {
        device(40, 30, 3, true);
        C.rows_bottom_up = D.c.rows_bottom_up = true;
        D.c.band_rows = rows;
        size_t n = pattern_png(40, 30, 2, 3);
        TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 4096));
        TEST_ASSERT_EQUAL_INT(0, F.order_errors);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(s_px, F.shown, 40 * 30);
        TEST_ASSERT_TRUE(F.image);
        TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_CLEAR, NULL, 0));
        TEST_ASSERT_EQUAL_INT(0, F.order_errors);
        TEST_ASSERT_EACH_EQUAL_UINT8(3, F.shown, 40 * 30);
    }
}

/* A refresh the driver calls done sooner than a panel can refresh: said in the answer and the log. */
static void test_fast_refresh_warns(void)
{
    device(40, 30, 3, false);
    D.c.fast_ms = 300;
    size_t n = pattern_png(40, 30, 1, 0), rn;
    F.refresh_us = 4200000;
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 4096));
    TEST_ASSERT_EQUAL_INT(4200, map_int(F.out + 6, F.out_len - 8, "ms", -1));
    TEST_ASSERT_FALSE(cbor_map_get(F.out + 6, F.out_len - 8, "warn", 4, &(const uint8_t *){0}, &rn));
    F.refresh_us = 12000;
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 4096));
    TEST_ASSERT_TRUE(cbor_map_get(F.out + 6, F.out_len - 8, "warn", 4, &(const uint8_t *){0}, &rn));
    TEST_ASSERT_NOT_NULL(strstr(F.log, "too fast"));
    TEST_ASSERT_EQUAL_CHAR('W', F.log[0]);
    request(USBDISP_CMD_CLEAR, NULL, 0, &rn);
    TEST_ASSERT_TRUE(cbor_map_get(F.out + 6, F.out_len - 8, "warn", 4, &(const uint8_t *){0}, &rn));
}

static void test_png_bound(void)
{
    TEST_ASSERT_EQUAL_UINT32((100 + 1) * 480 + 4096, usbdisp_png_bound(800, 480, 1));
    TEST_ASSERT_EQUAL_UINT32((936 + 1) * 1404 + 4096, usbdisp_png_bound(1872, 1404, 4));
    TEST_ASSERT_EQUAL_UINT32(4096, usbdisp_png_bound(0, 0, 1));
}

/* Hostile PNGs through the whole entry (ASan and UBSan are on): mutated, cut and stretched files, and headers that
 * lie about what follows. Each ends in an answer, shown or refused, with nothing leaked and no transfer left. */
static void test_hostile_png(void)
{
    static uint8_t m[sizeof s_png + 64];
    int shown = 0, refused = 0;
    /* a header that says a larger picture, with the rows to match: would overrun the frame if it were decoded */
    static const uint16_t lie[][2] = {{48, 30}, {40, 31}, {64, 64}, {41, 30}, {40, 60}, {8, 8}};
    for (size_t i = 0; i < sizeof lie / sizeof lie[0]; i++)
        for (int bwry = 0; bwry < 2; bwry++) {
            device(40, 30, 7, bwry);
            size_t n = pattern_png(lie[i][0], lie[i][1], 2, bwry ? 3 : 0);
            TEST_ASSERT_EQUAL_INT(USBDISP_E_WRONG_SIZE, show(s_png, n, 4096));
            TEST_ASSERT_EQUAL_INT(0, F.live);
            TEST_ASSERT_EQUAL_INT(0, F.refreshes);
        }
    for (int i = 0; i < 4000; i++) {
        bool bwry = i % 3 == 2;
        device(40, 30, 7, bwry);
        uint8_t bits = (uint8_t)(1u << rnd() % 3);
        size_t n = pattern_png(40, 30, bwry ? 2 : bits, bwry ? 3 : 0);
        memcpy(m, s_png, n);
        switch (rnd() % 6) {
        case 0: n -= rnd() % (n - USBDISP_PNG_MIN); break;                      /* cut short */
        case 1: m[16 + rnd() % 13] = (uint8_t)rnd(); break;                     /* the header's fields */
        case 2: put32(m + 33 + (bwry ? 24 : 0), rnd()); break;                  /* the IDAT length */
        case 3: memset(m + n, (int)rnd(), 64), n += rnd() % 64; break;          /* bytes after the end */
        default:
            for (unsigned k = 0, c = 1 + rnd() % 4; k < c; k++)
                m[rnd() % n] ^= (uint8_t)(1u << rnd() % 8);
        }
        int e = show(m, n, 1 + rnd() % 300 + 29);
        e ? refused++ : shown++;
        TEST_ASSERT_TRUE(e == 0 || e == USBDISP_E_NOT_PNG || e == USBDISP_E_WRONG_SIZE || e == USBDISP_E_DEPTH ||
                         e == USBDISP_E_BAD_PNG);
        TEST_ASSERT_FALSE(usbdisp_transfer(&D));
        TEST_ASSERT_EQUAL_INT(0, F.live);
        TEST_ASSERT_EQUAL_INT(e ? 0 : 1, F.refreshes);
    }
    TEST_ASSERT_TRUE(shown > 100 && refused > 1000);
}

/* ------------------------------------------------------------------ the simulator's HAL */

#if TINY_HAVE_SIM_HAL

/* The device's own binding (usbdisp_hal.c) on the simulator: PNGs of the sim board's 800 x 480 over its link, and
 * the panel shows them pixel for pixel. */
static void test_sim_shows_pixel_for_pixel(void)
{
    static usbdisp_t d;
    static uint8_t px[800 * 480], png[800 * 480 / 4 + 8192], fr[LINK_FRAME_MAX], p[LINK_MAX_PAYLOAD], out[4096];
    usbdisp_cfg_t c;
    usbdisp_hal_cfg(&c, "0.0.0+sim");
    TEST_ASSERT_EQUAL_UINT16(800, c.w);
    TEST_ASSERT_EQUAL_UINT16(480, c.h);
    usbdisp_init(&d, &c);
    sim_link_take(out, sizeof out);
    for (uint8_t bits = 1; bits <= 2; bits++) {
        for (size_t i = 0; i < sizeof px; i++)
            px[i] = (uint8_t)(rnd() % (1u << bits));
        size_t n = make_png(png, 800, 480, bits, 0, px), on = 0;
        TEST_ASSERT_TRUE(n <= d.c.png_max);
        uint8_t seq = 1;
        for (size_t off = 0; off <= n; off += USBDISP_CHUNK) {
            cbor_w_t w;
            cbor_w_init(&w, p, sizeof p);
            if (!off) {
                cbor_put_map(&w, 1);
                cbor_put_str(&w, "size");
                cbor_put_uint(&w, n);
                sim_link_inject(fr, link_frame_encode(fr, sizeof fr, seq++, USBDISP_CMD_SHOW_BEGIN, p, w.len));
                usbdisp_poll(&d, 0);
                cbor_w_init(&w, p, sizeof p);
            }
            if (off == n)
                break;
            cbor_put_map(&w, 2);
            cbor_put_str(&w, "off");
            cbor_put_uint(&w, off);
            cbor_put_str(&w, "data");
            cbor_put_bstr(&w, png + off, n - off < USBDISP_CHUNK ? n - off : USBDISP_CHUNK);
            sim_link_inject(fr, link_frame_encode(fr, sizeof fr, seq++, USBDISP_CMD_SHOW_CHUNK, p, w.len));
            usbdisp_poll(&d, 0);
        }
        sim_link_take(out, sizeof out);
        sim_link_inject(fr, link_frame_encode(fr, sizeof fr, seq, USBDISP_CMD_SHOW_END, NULL, 0));
        usbdisp_poll(&d, 0);
        on = sim_link_take(out, sizeof out);
        TEST_ASSERT_TRUE(on > 8);
        TEST_ASSERT_EQUAL_HEX8(USBDISP_CMD_SHOW_END | 0x80, out[5]);
        TEST_ASSERT_EQUAL_INT(0, map_int(out + 6, on - 8, "err", 0));
        TEST_ASSERT_EQUAL_INT(bits, map_int(out + 6, on - 8, "depth", 0));
        uint16_t w, h;
        const uint8_t *shown = sim_panel_shown(&w, &h);
        TEST_ASSERT_NOT_NULL(shown);
        TEST_ASSERT_EQUAL_UINT16(800, w);
        for (size_t i = 0; i < sizeof px; i++) /* the sim keeps gray 0..15: a level of `bits` scaled */
            if (shown[i] != px[i] * 15 / ((1 << bits) - 1)) {
                char msg[64];
                snprintf(msg, sizeof msg, "bits %u pixel %u: %u, sent %u", (unsigned)bits, (unsigned)i, shown[i], px[i]);
                TEST_FAIL_MESSAGE(msg);
            }
    }
    TEST_ASSERT_FALSE(usbdisp_transfer(&d));
}
#endif

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_vectors);
    RUN_TEST(test_alloc_fails_at_each_point);
    RUN_TEST(test_quiet_transfer_is_dropped);
    RUN_TEST(test_half_frame_is_forgotten);
    RUN_TEST(test_panel_failures);
    RUN_TEST(test_bottom_up_bands);
    RUN_TEST(test_fast_refresh_warns);
    RUN_TEST(test_png_bound);
    RUN_TEST(test_hostile_png);
#if TINY_HAVE_SIM_HAL
    RUN_TEST(test_sim_shows_pixel_for_pixel);
#endif
    return UNITY_END();
}
