/* usbdisp: the USB display protocol against a fake link, heap, clock and panel, replaying docs/spec/vectors/
 * usb-display.json; the failure paths (allocation, a quiet transfer, half a frame, the panel driver), a mutation fuzz of
 * the PNG entry, and pictures (full and partial) pixel for pixel through the simulator's HAL. Areas and modes:
 * test_usbdisp_area.c. SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if TINY_HAVE_SIM_HAL /* set by hal/sim/host.cmake: a tree with only this component's fakes has no simulator */
#include "sim.h"
#endif
#include "sim_json.h"
#include "usbdisp_fake.h"

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
        const sj_t *pd = sj_get(panel, "partial_depths");
        uint8_t partial = 0;
        for (size_t i = 0; i < sj_len(pd); i++)
            partial |= (uint8_t)sj_num(sj_at(pd, i), 0);
        C.clean_every = (uint8_t)sj_num(sj_get(panel, "clean_every"), 0);
        device_more(partial, sj_bool(sj_get(panel, "partial_all"), false), (uint8_t)sj_num(sj_get(panel, "full_every"), 0));
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
                TEST_ASSERT_EQUAL_INT_MESSAGE(refreshes + (int)sj_num(sj_get(st, "refreshes"), 1), F.refreshes, name);
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
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, F.stale, "a partial was given an old frame the glass did not show");
        TEST_ASSERT_EQUAL_INT(F.begins, F.sleeps); /* the panel is put to sleep after every use */
        usbdisp_reset(&D);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, F.live, "memory left after the session");
    }
    TEST_ASSERT_TRUE(steps_run > 120 && shown >= 26 && refused >= 40);
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
    TEST_ASSERT_EQUAL_INT(0, show_at(s_png, n, 4096, -1, 0, "full", 0));
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

/* Issue #214: the display firmware's binding must switch the redrive bitmap file off (the app never calls this) */
static int fresh_ram = -1;
void hal_panel_fresh_ram(bool ram_only) { fresh_ram = ram_only; }

static void test_binding_keeps_fresh_bitmap_in_ram(void)
{
    usbdisp_cfg_t c;
    fresh_ram = -1;
    usbdisp_hal_cfg(&c, "0.0.0+sim");
    TEST_ASSERT_EQUAL_INT(1, fresh_ram);
}

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
    TEST_ASSERT_EQUAL_HEX8(1, c.partial_depths); /* the OG: partial at 1 bit */
    /* a 1 bit picture (full, the first), a block of it changed (partial, through the HAL's partial write), 2 bit */
    for (int k = 0; k < 3; k++) {
        uint8_t bits = k == 2 ? 2 : 1;
        if (k == 1) {
            for (size_t y = 100; y < 200; y++)
                for (size_t x = 260; x < 500; x++)
                    px[y * 800 + x] ^= 1;
        } else {
            for (size_t i = 0; i < sizeof px; i++)
                px[i] = (uint8_t)(rnd() % (1u << bits));
        }
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
        s_ans = out + 6, s_ans_len = on - 8;
        TEST_ASSERT_EQUAL_STRING(k == 1 ? "partial" : "full", ans_str("mode"));
        TEST_ASSERT_EQUAL_STRING(k == 0 ? "first" : k == 1 ? "changed" : "depth", ans_str("why"));
        if (k == 1) {
            TEST_ASSERT_EQUAL_INT(256, map_int(out + 6, on - 8, "x", -1));
            TEST_ASSERT_EQUAL_INT(100, map_int(out + 6, on - 8, "y", -1));
            TEST_ASSERT_EQUAL_INT(248, map_int(out + 6, on - 8, "w", -1));
            TEST_ASSERT_EQUAL_INT(100, map_int(out + 6, on - 8, "h", -1));
        }
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
    usbdisp_reset(&d);
}
#endif

void setUp(void) {}
void tearDown(void) {}

/* Shared fingerprints and the upload fallback: every board/depth, independent of partial capability. */
static void test_fingerprint_and_same(void)
{
    sj_t *v = load_vectors();
    const sj_t *items = sj_get(v, "fingerprints");
    for (size_t i = 0; i < sj_len(items); i++) {
        const sj_t *item = sj_at(items, i);
        bool color = sj_bool(sj_get(item, "color"), false);
        uint8_t bits = (uint8_t)sj_num(sj_get(item, "bits"), 1);
        device(16, 8, 7, color);
        size_t n = unhex(sj_str(sj_get(item, "png"), ""), s_png, sizeof s_png);
        TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 4096));
        size_t rn, hn;
        const uint8_t *r = request(USBDISP_CMD_HELLO, NULL, 0, &rn), *hp;
        TEST_ASSERT_TRUE(cbor_map_get(r, rn, "fingerprint", 11, &hp, &hn));
        const char *want = sj_str(sj_get(item, "fingerprint"), "");
        TEST_ASSERT_EQUAL_UINT(17, hn);
        TEST_ASSERT_EQUAL_MEMORY(want, hp + 1, 16);
        TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 4096));
        TEST_ASSERT_EQUAL_INT(1, F.refreshes);
        TEST_ASSERT_EQUAL_UINT(0, D.since_full);
        /* The vector uses compressed IDAT; the harness emits stored blocks for the same pixels. */
        size_t alt = make_png(s_png, 16, 8, bits, color ? 3 : 0, F.shown);
        TEST_ASSERT_NOT_EQUAL(n, alt);
        TEST_ASSERT_EQUAL_INT(0, show(s_png, alt, 4096));
        TEST_ASSERT_EQUAL_INT(1, F.refreshes);
        n = alt;
        TEST_ASSERT_EQUAL_INT(0, show_at(s_png, n, 4096, -1, 0, "full", 0));
        TEST_ASSERT_EQUAL_INT(2, F.refreshes);
        TEST_ASSERT_EQUAL_STRING("asked", ans_str("why"));
        TEST_ASSERT_EQUAL_STRING("full", ans_str("mode"));
        TEST_ASSERT_EQUAL_UINT8(bits, F.depth);
        memcpy(s_px, F.shown, 16 * 8);
        s_px[0] ^= 1;
        n = make_png(s_png, 16, 8, bits, color ? 3 : 0, s_px);
        TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 4096));
        TEST_ASSERT_EQUAL_INT(3, F.refreshes);
        TEST_ASSERT_NOT_EQUAL(0, strcmp(want, D.fingerprint));
        usbdisp_reset(&D);
        TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HELLO, NULL, 0));
        TEST_ASSERT_EQUAL_STRING("", ans_str("fingerprint"));
    }
    sj_free(v);
}

static void test_same_before_cleanup_and_cut(void)
{
    device_partial(16, 8, 7, 1, false, 1);
    size_t n = pattern_png(16, 8, 1, 0);
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 4096));
    s_px[0] ^= 1;
    n = make_png(s_png, 16, 8, 1, 0, s_px);
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 4096));
    TEST_ASSERT_EQUAL_UINT(1, D.since_full);
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 4096));
    TEST_ASSERT_EQUAL_INT(2, F.refreshes);
    TEST_ASSERT_EQUAL_STRING("none", ans_str("mode"));
    TEST_ASSERT_EQUAL_UINT(1, D.since_full);
    uint8_t payload[64];
    cbor_w_t w;
    cbor_w_init(&w, payload, sizeof payload);
    cbor_put_map(&w, 1);
    cbor_put_str(&w, "fingerprint");
    cbor_put_str(&w, D.fingerprint);
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HAVE, payload, w.len));
    const uint8_t *v;
    size_t vn;
    TEST_ASSERT_TRUE(cbor_map_get(s_ans, s_ans_len, "same", 4, &v, &vn));
    TEST_ASSERT_EQUAL_HEX8(0xF5, v[0]);
    uint8_t begin[] = {0xA1, 0x64, 's', 'i', 'z', 'e', 0x18, 100};
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_SHOW_BEGIN, begin, sizeof begin));
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HAVE, payload, w.len));
    TEST_ASSERT_TRUE(cbor_map_get(s_ans, s_ans_len, "send", 4, &v, &vn));
    F.now += USBDISP_IDLE_US + 1;
    usbdisp_poll(&D, 0);
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HELLO, NULL, 0));
    TEST_ASSERT_EQUAL_STRING("", ans_str("fingerprint"));
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HAVE, payload, w.len));
    TEST_ASSERT_TRUE(cbor_map_get(s_ans, s_ans_len, "send", 4, &v, &vn));
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 4096));
    TEST_ASSERT_EQUAL_INT(3, F.refreshes);
    TEST_ASSERT_EQUAL_STRING("cut", ans_str("why"));
    TEST_ASSERT_EQUAL_INT(USBDISP_E_BAD_REQUEST, req_err(USBDISP_CMD_HAVE, NULL, 0));
    usbdisp_reset(&D);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fingerprint_and_same);
    RUN_TEST(test_same_before_cleanup_and_cut);
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
    RUN_TEST(test_binding_keeps_fresh_bitmap_in_ram);
#endif
    return UNITY_END();
}
