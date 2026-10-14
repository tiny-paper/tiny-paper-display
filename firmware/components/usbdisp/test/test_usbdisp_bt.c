/* usbdisp: Bluetooth behind the setting (docs/spec/usb-display.md "Bluetooth") against a fake stack that counts every
 * call: off by default and no call while off, the setting kept over a restart and changed over USB only, the frames
 * over the second link (20-byte pieces, a lost answer, both links at once), a host that is not bonded, the pairing
 * window and its screens. Vectors: usb-display.json "bluetooth". SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_json.h"
#include "usbdisp_fake.h"

static struct {
    int calls;                 /* every bt_* call and rand32 */
    int opens, closes, forgets, pair_opens, pair_closes;
    bool open, window, connected, secure, fail_open;
    uint32_t pass;
    int kv;                    /* the stored setting, -1 none */
    int kv_sets;
    bool fail_kv;
    uint8_t in[1 << 15], out[1 << 15];
    size_t in_len, in_pos, out_len;
    size_t piece;              /* bytes a read gives at most (MTU - 3) */
    int drop_writes;           /* answers lost on the way to the host */
} B;

static int b_open(void *ctx, const char *name)
{
    (void)ctx;
    B.calls++;
    TEST_ASSERT_EQUAL_STRING("tp-display-VEC0", name);
    TEST_ASSERT_FALSE_MESSAGE(B.open, "opened twice");
    if (B.fail_open)
        return -1;
    B.open = true;
    B.opens++;
    return 0;
}

static void b_close(void *ctx)
{
    (void)ctx;
    B.calls++;
    TEST_ASSERT_TRUE(B.open);
    B.open = B.connected = B.secure = false;
    B.in_len = B.in_pos = 0;
    B.closes++;
}

static long b_read(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    (void)ctx, (void)timeout_ms;
    B.calls++;
    TEST_ASSERT_TRUE(B.open);
    size_t n = B.in_len - B.in_pos;
    n = n < cap ? n : cap;
    n = B.piece && n > B.piece ? B.piece : n;
    memcpy(buf, B.in + B.in_pos, n);
    B.in_pos += n;
    return (long)n;
}

static long b_write(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    B.calls++;
    TEST_ASSERT_TRUE(B.open);
    if (B.drop_writes) {
        B.drop_writes--;
        return (long)len;
    }
    TEST_ASSERT_TRUE(B.out_len + len <= sizeof B.out);
    memcpy(B.out + B.out_len, buf, len);
    B.out_len += len;
    return (long)len;
}

static bool b_connected(void *ctx)
{
    (void)ctx;
    B.calls++;
    return B.connected;
}

static bool b_secure(void *ctx)
{
    (void)ctx;
    B.calls++;
    return B.connected && B.secure;
}

static void b_pairing(void *ctx, bool open, uint32_t passkey)
{
    (void)ctx;
    B.calls++;
    TEST_ASSERT_TRUE(B.open);
    B.window = open;
    if (open) {
        TEST_ASSERT_TRUE(passkey < 1000000);
        B.pass = passkey;
        B.pair_opens++;
    } else {
        B.pair_closes++;
    }
}

static void b_forget(void *ctx)
{
    (void)ctx;
    B.calls++;
    TEST_ASSERT_FALSE_MESSAGE(B.open, "the bonds go with the stack stopped: the connection ends");
    B.forgets++;
}

static uint32_t b_rand(void *ctx)
{
    (void)ctx;
    B.calls++;
    return 987654321u;
}

static int b_kv_get(void *ctx, const char *key, void *buf, size_t cap)
{
    if (strcmp(key, USBDISP_KV_BT))
        return f_kv_get(ctx, key, buf, cap);
    if (B.kv < 0 || !cap)
        return -1;
    *(uint8_t *)buf = (uint8_t)B.kv;
    return 1;
}

static int b_kv_set(void *ctx, const char *key, const void *buf, size_t len)
{
    if (strcmp(key, USBDISP_KV_BT))
        return f_kv_set(ctx, key, buf, len);
    if (B.fail_kv || len != 1)
        return -1;
    B.kv = *(const uint8_t *)buf;
    B.kv_sets++;
    return 0;
}

static void bt_cfg(void)
{
    C.bt_name = "tp-display-VEC0";
    C.bt_open = b_open;
    C.bt_close = b_close;
    C.bt_read = b_read;
    C.bt_write = b_write;
    C.bt_connected = b_connected;
    C.bt_secure = b_secure;
    C.bt_pairing = b_pairing;
    C.bt_forget = b_forget;
    C.rand32 = b_rand;
    C.kv_get = b_kv_get;
    C.kv_set = b_kv_set;
}

/* The vectors' gray panel with Bluetooth; kv: the stored setting (-1: a new device). */
static void bt_device(int kv)
{
    usbdisp_bt_stop(&D);
    memset(&B, 0, sizeof B);
    B.kv = kv;
    device_partial(16, 8, 3, 1, true, 20);
    bt_cfg();
    usbdisp_init(&D, &C);
}

/* Power off and on: the store stays, everything else starts again. */
static void restart(void)
{
    if (D.bt) {
        free(D.bt);
        F.live--;
    }
    int kv = B.kv;
    usbdisp_reset(&D);
    memset(&B, 0, sizeof B);
    B.kv = kv;
    usbdisp_init(&D, &C);
}

static int bt_cmd(int on, int pair, int forget) /* -1: key absent */
{
    uint8_t p[32];
    cbor_w_t w;
    cbor_w_init(&w, p, sizeof p);
    cbor_put_map(&w, (size_t)((on >= 0) + (pair >= 0) + (forget >= 0)));
    if (on >= 0) {
        cbor_put_str(&w, "on");
        cbor_put_bool(&w, on);
    }
    if (pair >= 0) {
        cbor_put_str(&w, "pair");
        cbor_put_bool(&w, pair);
    }
    if (forget >= 0) {
        cbor_put_str(&w, "forget");
        cbor_put_bool(&w, forget);
    }
    return req_err(USBDISP_CMD_BT, p, w.len);
}

/* One request over Bluetooth, in reads of B.piece bytes with a poll for each; the err of its answer (0: a success
 * map, -1: no answer). The answer's payload stays in s_ans. */
static uint8_t s_bseq = 100;
static int ble_send(uint8_t cmd, const uint8_t *payload, size_t len)
{
    static uint8_t fr[LINK_FRAME_MAX];
    s_bseq = (uint8_t)(s_bseq % 255 + 1);
    size_t n = link_frame_encode(fr, sizeof fr, s_bseq, cmd, payload, len);
    TEST_ASSERT_TRUE(n > 0 && n <= sizeof B.in);
    memcpy(B.in, fr, n);
    B.in_len = n;
    B.in_pos = 0;
    B.out_len = 0;
    size_t usb_before = F.out_len;
    for (int i = 0; i < 4000 && D.bt && B.in_pos < B.in_len; i++)
        usbdisp_poll(&D, 0);
    usbdisp_poll(&D, 0);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(usb_before, F.out_len, "an answer to a Bluetooth request went to USB");
    if (B.out_len < LINK_FRAME_OVERHEAD)
        return -1;
    size_t plen = (size_t)(B.out[2] | B.out[3] << 8);
    TEST_ASSERT_EQUAL_UINT(plen + LINK_FRAME_OVERHEAD, B.out_len);
    TEST_ASSERT_EQUAL_HEX8(s_bseq, B.out[4]);
    TEST_ASSERT_EQUAL_HEX8(cmd | 0x80, B.out[5]);
    TEST_ASSERT_TRUE(cbor_valid(B.out + 6, plen));
    s_ans = B.out + 6;
    s_ans_len = plen;
    return (int)map_int(s_ans, s_ans_len, "err", 0);
}

static int ble_chunk(const uint8_t *png, size_t off, size_t n)
{
    static uint8_t p[LINK_MAX_PAYLOAD];
    cbor_w_t w;
    cbor_w_init(&w, p, sizeof p);
    cbor_put_map(&w, 2);
    cbor_put_str(&w, "off");
    cbor_put_uint(&w, off);
    cbor_put_str(&w, "data");
    cbor_put_bstr(&w, png + off, n);
    return ble_send(USBDISP_CMD_SHOW_CHUNK, p, w.len);
}

static int ble_begin(size_t size)
{
    uint8_t p[16];
    cbor_w_t w;
    cbor_w_init(&w, p, sizeof p);
    cbor_put_map(&w, 1);
    cbor_put_str(&w, "size");
    cbor_put_uint(&w, size);
    return ble_send(USBDISP_CMD_SHOW_BEGIN, p, w.len);
}

/* A device with the setting on and a bonded host connected. */
static void bonded(void)
{
    bt_device(1);
    usbdisp_poll(&D, 0);
    TEST_ASSERT_TRUE(B.open);
    B.connected = B.secure = true;
    usbdisp_poll(&D, 0);
}

/* ------------------------------------------------------------------ the setting */

/* A new device: off, and nothing of the stack is ever called, whatever is asked over USB or done with the button. */
static void test_off_by_default_and_no_bluetooth_call(void)
{
    bt_device(-1);
    TEST_ASSERT_FALSE(usbdisp_bt_on(&D));
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HELLO, NULL, 0));
    TEST_ASSERT_EQUAL_STRING("off", ans_str("bt"));
    size_t n = pattern_png(16, 8, 1, 0);
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 64));
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_CLEAR, NULL, 0));
    TEST_ASSERT_FALSE(usbdisp_bt_button(&D));
    TEST_ASSERT_EQUAL_INT(0, bt_cmd(-1, -1, -1));
    TEST_ASSERT_EQUAL_INT(0, map_int(s_ans, s_ans_len, "pairing", -1));
    TEST_ASSERT_EQUAL_INT(USBDISP_E_MODE, bt_cmd(-1, 1, -1));
    TEST_ASSERT_EQUAL_INT(0, bt_cmd(0, 0, -1));
    for (int i = 0; i < 400; i++) { /* more than every timer of the component */
        F.now += 1000000;
        usbdisp_poll(&D, 100);
    }
    TEST_ASSERT_FALSE(usbdisp_bt_awake(&D));
    usbdisp_bt_stop(&D);
    usbdisp_busy(&D);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, B.calls, "a Bluetooth function was reached with the setting off");
    TEST_ASSERT_NULL(D.bt);
    TEST_ASSERT_EQUAL_INT(0, B.kv_sets);
    /* a stored value that is not 1 is off too */
    bt_device(7);
    usbdisp_poll(&D, 0);
    TEST_ASSERT_FALSE(usbdisp_bt_on(&D));
    TEST_ASSERT_EQUAL_INT(0, B.calls);
}

static void test_setting_survives_restart(void)
{
    bt_device(-1);
    TEST_ASSERT_EQUAL_INT(0, bt_cmd(1, -1, -1));
    TEST_ASSERT_EQUAL_INT(1, map_int(s_ans, s_ans_len, "on", -1) != 0);
    TEST_ASSERT_EQUAL_INT(1, B.kv);
    TEST_ASSERT_EQUAL_INT(1, B.opens);
    restart();
    TEST_ASSERT_TRUE(usbdisp_bt_on(&D));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, B.opens, "started before the first poll");
    usbdisp_poll(&D, 0);
    usbdisp_poll(&D, 0);
    TEST_ASSERT_EQUAL_INT(1, B.opens);
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HELLO, NULL, 0));
    TEST_ASSERT_EQUAL_STRING("on", ans_str("bt"));
    /* off: stored, the stack stopped; and it stays off over the next restart */
    TEST_ASSERT_EQUAL_INT(0, bt_cmd(0, -1, -1));
    TEST_ASSERT_EQUAL_INT(0, B.kv);
    TEST_ASSERT_EQUAL_INT(1, B.closes);
    TEST_ASSERT_FALSE(B.open);
    restart();
    usbdisp_poll(&D, 0);
    TEST_ASSERT_FALSE(usbdisp_bt_on(&D));
    TEST_ASSERT_EQUAL_INT(0, B.calls);
    /* a store that fails: refused, and still off */
    B.fail_kv = true;
    TEST_ASSERT_EQUAL_INT(USBDISP_E_IO, bt_cmd(1, -1, -1));
    TEST_ASSERT_FALSE(usbdisp_bt_on(&D));
    TEST_ASSERT_EQUAL_INT(0, B.calls);
    /* not a bool */
    uint8_t bad[] = {0xA1, 0x62, 'o', 'n', 0x01};
    TEST_ASSERT_EQUAL_INT(USBDISP_E_BAD_REQUEST, req_err(USBDISP_CMD_BT, bad, sizeof bad));
    TEST_ASSERT_EQUAL_INT(0, B.calls);
}

/* The stack does not start (no memory): refused with the reason, not tried at every poll, tried again when asked. */
static void test_start_failure(void)
{
    bt_device(-1);
    B.fail_open = true;
    TEST_ASSERT_EQUAL_INT(USBDISP_E_IO, bt_cmd(1, -1, -1));
    TEST_ASSERT_TRUE(usbdisp_bt_on(&D));
    int calls = B.calls;
    for (int i = 0; i < 10; i++)
        usbdisp_poll(&D, 0);
    TEST_ASSERT_EQUAL_INT(calls, B.calls);
    TEST_ASSERT_FALSE(usbdisp_bt_button(&D));
    TEST_ASSERT_EQUAL_INT(0, F.refreshes);
    B.fail_open = false;
    TEST_ASSERT_EQUAL_INT(0, bt_cmd(1, -1, -1));
    TEST_ASSERT_TRUE(B.open);
    /* no memory for the link's buffer */
    usbdisp_bt_stop(&D);
    restart();
    F.fail_at = 0;
    F.allocs = 0;
    usbdisp_poll(&D, 0);
    F.fail_at = -1;
    TEST_ASSERT_EQUAL_INT(0, B.opens);
    TEST_ASSERT_NULL(D.bt);
    usbdisp_bt_stop(&D);
    TEST_ASSERT_EQUAL_INT(0, F.live);
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

static void test_vectors(void)
{
    static char text[1 << 18], err[128];
    static uint8_t tx[1024], want[1024];
    FILE *f = fopen(TINY_ROOT "/docs/spec/vectors/usb-display.json", "r");
    TEST_ASSERT_NOT_NULL(f);
    text[fread(text, 1, sizeof text - 1, f)] = 0;
    fclose(f);
    sj_t *v = sj_parse(text, err, sizeof err);
    TEST_ASSERT_NOT_NULL_MESSAGE(v, err);
    const sj_t *steps = sj_get(sj_get(v, "bluetooth"), "steps");
    TEST_ASSERT_EQUAL_UINT(12, sj_len(steps));
    bt_device(-1);
    int over_ble = 0;
    for (size_t i = 0; i < sj_len(steps); i++) {
        const sj_t *st = sj_at(steps, i);
        const char *name = sj_str(sj_get(st, "name"), "?");
        bool ble = !strcmp(sj_str(sj_get(st, "link"), ""), "ble");
        size_t tn = unhex(sj_str(sj_get(st, "tx"), ""), tx, sizeof tx);
        size_t wn = unhex(sj_str(sj_get(st, "rx"), ""), want, sizeof want);
        F.out_len = B.out_len = 0;
        if (ble) {
            TEST_ASSERT_TRUE_MESSAGE(B.open, name);
            B.connected = true;
            B.secure = sj_bool(sj_get(st, "secure"), false);
            memcpy(B.in, tx, tn);
            B.in_len = tn;
            B.in_pos = 0;
            over_ble++;
        } else {
            to_device(tx, tn);
        }
        usbdisp_poll(&D, 0);
        usbdisp_poll(&D, 0);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(wn, ble ? B.out_len : F.out_len, name);
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(want, ble ? B.out : F.out, wn, name);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0, ble ? F.out_len : B.out_len, name);
    }
    TEST_ASSERT_EQUAL_INT(4, over_ble);
    TEST_ASSERT_EQUAL_INT(1, B.forgets);
    TEST_ASSERT_FALSE(B.open);
    sj_free(v);
    TEST_ASSERT_EQUAL_INT(0, F.order_errors);
}

/* ------------------------------------------------------------------ the frames over Bluetooth */

/* MTU 23: every write brings 20 bytes (and less), one read a poll; a whole picture arrives and is shown, the answers
 * leave over Bluetooth, and a USB request in the middle of a Bluetooth frame is answered on USB. */
static void test_frames_in_small_pieces(void)
{
    static const size_t pieces[] = {20, 1, 7, 244};
    for (size_t k = 0; k < sizeof pieces / sizeof pieces[0]; k++) {
        bonded();
        B.piece = pieces[k];
        TEST_ASSERT_EQUAL_INT(0, ble_send(USBDISP_CMD_HELLO, NULL, 0));
        TEST_ASSERT_EQUAL_STRING("display", ans_str("project"));
        size_t n = pattern_png(16, 8, 1, 0);
        TEST_ASSERT_EQUAL_INT(0, ble_begin(n));
        /* half of the chunk's frame, then a whole USB request, then the rest */
        static uint8_t p[512], fr[600];
        cbor_w_t w;
        cbor_w_init(&w, p, sizeof p);
        cbor_put_map(&w, 2);
        cbor_put_str(&w, "off");
        cbor_put_uint(&w, 0);
        cbor_put_str(&w, "data");
        cbor_put_bstr(&w, s_png, n);
        size_t fn = link_frame_encode(fr, sizeof fr, 9, USBDISP_CMD_SHOW_CHUNK, p, w.len);
        memcpy(B.in, fr, fn);
        B.in_len = fn / 2;
        B.in_pos = 0;
        B.out_len = 0;
        while (B.in_pos < B.in_len)
            usbdisp_poll(&D, 0);
        TEST_ASSERT_EQUAL_UINT(0, B.out_len);
        TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HELLO, NULL, 0));
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0, B.out_len, "the USB answer went to Bluetooth");
        B.in_len = fn;
        while (B.in_pos < B.in_len)
            usbdisp_poll(&D, 0);
        usbdisp_poll(&D, 0);
        TEST_ASSERT_TRUE(B.out_len > LINK_FRAME_OVERHEAD);
        TEST_ASSERT_EQUAL_HEX8(USBDISP_CMD_SHOW_CHUNK | 0x80, B.out[5]);
        TEST_ASSERT_EQUAL_INT((int)n, (int)map_int(B.out + 6, B.out_len - 8, "next", -1));
        TEST_ASSERT_EQUAL_INT(0, ble_send(USBDISP_CMD_SHOW_END, NULL, 0));
        TEST_ASSERT_EQUAL_STRING("full", ans_str("mode"));
        TEST_ASSERT_EQUAL_HEX8_ARRAY(s_px, F.shown, 16 * 8);
        TEST_ASSERT_EQUAL_INT(0, F.order_errors);
    }
    usbdisp_bt_stop(&D);
}

/* The answer to a chunk is lost: the host sends the chunk again and is told where to go on; nothing is shown twice
 * or damaged. A lost hello answer: asked again, answered again. */
static void test_lost_answer(void)
{
    bonded();
    B.piece = 20;
    size_t n = pattern_png(16, 8, 1, 0), half = 40;
    TEST_ASSERT_TRUE(n > half);
    B.drop_writes = 1;
    TEST_ASSERT_EQUAL_INT(-1, ble_send(USBDISP_CMD_HELLO, NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, ble_send(USBDISP_CMD_HELLO, NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, ble_begin(n));
    B.drop_writes = 1;
    TEST_ASSERT_EQUAL_INT(-1, ble_chunk(s_png, 0, half));
    TEST_ASSERT_EQUAL_INT(USBDISP_E_SEQUENCE, ble_chunk(s_png, 0, half));
    const uint8_t *dv;
    size_t dn;
    TEST_ASSERT_TRUE(cbor_map_get(s_ans, s_ans_len, "detail", 6, &dv, &dn));
    TEST_ASSERT_EQUAL_INT((int)half, (int)map_int(dv, dn, "next", -1));
    TEST_ASSERT_EQUAL_INT(0, ble_chunk(s_png, half, n - half));
    TEST_ASSERT_EQUAL_INT(0, ble_send(USBDISP_CMD_SHOW_END, NULL, 0));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_px, F.shown, 16 * 8);
    /* a lost show_end answer: the picture is shown; the repeat finds no transfer and the host starts again */
    TEST_ASSERT_EQUAL_INT(0, ble_begin(n));
    TEST_ASSERT_EQUAL_INT(0, ble_chunk(s_png, 0, n));
    B.drop_writes = 1;
    TEST_ASSERT_EQUAL_INT(-1, ble_send(USBDISP_CMD_SHOW_END, NULL, 0));
    TEST_ASSERT_EQUAL_INT(USBDISP_E_SEQUENCE, ble_send(USBDISP_CMD_SHOW_END, NULL, 0));
    TEST_ASSERT_FALSE(usbdisp_transfer(&D));
    usbdisp_bt_stop(&D);
    TEST_ASSERT_EQUAL_INT(0, (usbdisp_reset(&D), F.live));
}

/* A host that left in the middle of a frame: the next host's first request is not read as the rest of it, and half
 * a frame of a host that stays is forgotten after 2 s as on USB. */
static void test_cut_frame(void)
{
    bonded();
    static uint8_t big[400], fr[512];
    size_t fn = link_frame_encode(fr, sizeof fr, 5, USBDISP_CMD_SHOW_CHUNK, big, sizeof big); /* 400 bytes announced */
    memcpy(B.in, fr, fn);
    B.in_len = fn / 2;
    usbdisp_poll(&D, 0);
    B.connected = false;
    usbdisp_poll(&D, 0);
    B.connected = true;
    TEST_ASSERT_EQUAL_INT(0, ble_send(USBDISP_CMD_HELLO, NULL, 0));
    memcpy(B.in, fr, fn);
    B.in_len = fn / 2;
    B.in_pos = 0;
    usbdisp_poll(&D, 0);
    F.now += USBDISP_FRAME_IDLE_US;
    usbdisp_poll(&D, 0);
    TEST_ASSERT_EQUAL_INT(0, ble_send(USBDISP_CMD_HELLO, NULL, 0));
    usbdisp_bt_stop(&D);
}

/* ------------------------------------------------------------------ safety */

/* Not bonded: every request gets AUTH and nothing else, the panel and the device's state stay as they were. */
static void test_unbonded_host_is_refused(void)
{
    bonded();
    B.secure = false;
    B.piece = 20;
    size_t n = pattern_png(16, 8, 1, 0);
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 64)); /* over USB: the picture the stranger must not change */
    static uint8_t before[MAXPIX];
    memcpy(before, F.shown, sizeof before);
    int refreshes = F.refreshes;
    /* a transfer of the USB host is open */
    uint8_t sz[] = {0xA1, 0x64, 's', 'i', 'z', 'e', 0x18, (uint8_t)n};
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_SHOW_BEGIN, sz, sizeof sz));
    uint64_t last = D.bt_last_us;
    F.now += 5000000;
    static const uint8_t cmds[] = {USBDISP_CMD_HELLO, USBDISP_CMD_SHOW_BEGIN, USBDISP_CMD_SHOW_CHUNK, USBDISP_CMD_SHOW_END,
                                   USBDISP_CMD_CLEAR, USBDISP_CMD_SET, USBDISP_CMD_BT, 0x20};
    for (size_t i = 0; i < sizeof cmds; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(USBDISP_E_AUTH, ble_send(cmds[i], sz, sizeof sz), "not AUTH");
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xA2, s_ans[0], "more than err and msg"); /* a map of two */
        TEST_ASSERT_EQUAL_STRING("", ans_str("fw"));
    }
    TEST_ASSERT_EQUAL_INT(refreshes, F.refreshes);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(before, F.shown, sizeof before);
    TEST_ASSERT_TRUE_MESSAGE(usbdisp_transfer(&D), "the stranger dropped the USB host's transfer");
    TEST_ASSERT_NOT_NULL_MESSAGE(usbdisp_prev(&D, NULL), "the stranger forced a full refresh");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(last, D.bt_last_us, "the stranger keeps the device awake");
    TEST_ASSERT_TRUE(usbdisp_bt_on(&D));
    /* the setting cannot be changed over Bluetooth by a bonded host either */
    B.secure = true;
    uint8_t off[] = {0xA1, 0x62, 'o', 'n', 0xF4};
    TEST_ASSERT_EQUAL_INT(USBDISP_E_CABLE, ble_send(USBDISP_CMD_BT, off, sizeof off));
    uint8_t forget[] = {0xA1, 0x66, 'f', 'o', 'r', 'g', 'e', 't', 0xF5};
    TEST_ASSERT_EQUAL_INT(USBDISP_E_CABLE, ble_send(USBDISP_CMD_BT, forget, sizeof forget));
    TEST_ASSERT_TRUE(usbdisp_bt_on(&D));
    TEST_ASSERT_EQUAL_INT(1, B.kv);
    TEST_ASSERT_EQUAL_INT(0, B.forgets);
    TEST_ASSERT_EQUAL_INT(0, B.pair_opens);
    usbdisp_bt_stop(&D);
}

/* How many pixels of the glass are black */
static int black(void)
{
    int n = 0;
    for (size_t i = 0; i < (size_t)C.w * C.h; i++)
        n += F.shown[i] == 0;
    return n;
}

static void test_pairing_window(void)
{
    bt_device(1);
    usbdisp_poll(&D, 0);
    TEST_ASSERT_FALSE(B.window);
    TEST_ASSERT_EQUAL_UINT32(0, usbdisp_bt_pairing(&D));
    TEST_ASSERT_FALSE(usbdisp_bt_awake(&D));
    /* the button opens it: a passkey for the stack, the screen on the panel */
    TEST_ASSERT_TRUE(usbdisp_bt_button(&D));
    TEST_ASSERT_TRUE(B.window);
    TEST_ASSERT_EQUAL_UINT32(987654321u % 1000000u, B.pass);
    TEST_ASSERT_EQUAL_INT(1, F.fulls);
    TEST_ASSERT_TRUE_MESSAGE(black() > 0, "nothing drawn");
    TEST_ASSERT_NULL(usbdisp_prev(&D, NULL));
    TEST_ASSERT_EQUAL_UINT32(USBDISP_BT_PAIR_S, usbdisp_bt_pairing(&D));
    TEST_ASSERT_TRUE(usbdisp_bt_awake(&D));
    /* a second press: the same window, the same passkey, no new screen */
    TEST_ASSERT_TRUE(usbdisp_bt_button(&D));
    TEST_ASSERT_EQUAL_INT(1, B.pair_opens);
    TEST_ASSERT_EQUAL_INT(1, F.fulls);
    /* it closes by itself, and the panel says so */
    F.now += (USBDISP_BT_PAIR_S - 1) * 1000000ull;
    usbdisp_poll(&D, 0);
    TEST_ASSERT_TRUE(B.window);
    F.now += 1000000;
    usbdisp_poll(&D, 0);
    TEST_ASSERT_FALSE(B.window);
    TEST_ASSERT_EQUAL_INT(2, F.fulls);
    TEST_ASSERT_FALSE(usbdisp_bt_awake(&D));
    /* over the cable; a host pairs: closed at once */
    TEST_ASSERT_EQUAL_INT(0, bt_cmd(-1, 1, -1));
    TEST_ASSERT_EQUAL_INT(USBDISP_BT_PAIR_S, (int)map_int(s_ans, s_ans_len, "pairing", -1));
    TEST_ASSERT_TRUE(B.window);
    TEST_ASSERT_EQUAL_INT(3, F.fulls);
    B.connected = true;
    usbdisp_poll(&D, 0);
    TEST_ASSERT_TRUE_MESSAGE(B.window, "closed by a host that has not paired");
    B.secure = true;
    usbdisp_poll(&D, 0);
    TEST_ASSERT_FALSE(B.window);
    TEST_ASSERT_EQUAL_INT(4, F.fulls);
    /* opened while a host is connected: that host leaves (the stack starts again) */
    int closes = B.closes;
    TEST_ASSERT_TRUE(usbdisp_bt_button(&D));
    TEST_ASSERT_EQUAL_INT(closes + 1, B.closes);
    TEST_ASSERT_TRUE(B.open && B.window && !B.connected);
    /* pair: false closes it */
    TEST_ASSERT_EQUAL_INT(0, bt_cmd(-1, 0, -1));
    TEST_ASSERT_FALSE(B.window);
    /* forget: the bonds go, a window open closes, the stack is back without the old connection */
    TEST_ASSERT_TRUE(usbdisp_bt_button(&D));
    int fulls = F.fulls;
    TEST_ASSERT_EQUAL_INT(0, bt_cmd(-1, -1, 1));
    TEST_ASSERT_EQUAL_INT(1, B.forgets);
    TEST_ASSERT_TRUE(B.open);
    TEST_ASSERT_FALSE(B.window);
    TEST_ASSERT_EQUAL_UINT32(0, usbdisp_bt_pairing(&D));
    TEST_ASSERT_EQUAL_INT(fulls + 1, F.fulls);
    /* the panel fails: no window without the screen */
    F.fail_refresh = 1;
    TEST_ASSERT_FALSE(usbdisp_bt_button(&D));
    TEST_ASSERT_FALSE(B.window);
    TEST_ASSERT_EQUAL_UINT32(0, usbdisp_bt_pairing(&D));
    F.fail_refresh = 0;
    /* no memory for the screen */
    F.fail_at = 0;
    F.allocs = 0;
    TEST_ASSERT_FALSE(usbdisp_bt_button(&D));
    F.fail_at = -1;
    TEST_ASSERT_FALSE(B.window);
    /* off closes an open window and stops the stack */
    TEST_ASSERT_TRUE(usbdisp_bt_button(&D));
    TEST_ASSERT_EQUAL_INT(0, bt_cmd(0, -1, -1));
    TEST_ASSERT_FALSE(B.open);
    TEST_ASSERT_FALSE(usbdisp_bt_button(&D));
    TEST_ASSERT_EQUAL_INT(0, F.order_errors);
    TEST_ASSERT_EQUAL_INT(F.begins, F.sleeps);
    TEST_ASSERT_EQUAL_INT(0, (usbdisp_reset(&D), F.live));
}

/* A connected host keeps the device awake only while it asks; a stopped stack is started by the next poll. */
static void test_awake_and_stop(void)
{
    bonded();
    TEST_ASSERT_TRUE(usbdisp_bt_awake(&D));
    F.now += USBDISP_BT_QUIET_US - 1;
    TEST_ASSERT_TRUE(usbdisp_bt_awake(&D));
    TEST_ASSERT_EQUAL_INT(0, ble_send(USBDISP_CMD_HELLO, NULL, 0));
    F.now += USBDISP_BT_QUIET_US - 1;
    TEST_ASSERT_TRUE(usbdisp_bt_awake(&D));
    F.now += 1;
    TEST_ASSERT_FALSE(usbdisp_bt_awake(&D));
    usbdisp_bt_stop(&D);
    TEST_ASSERT_FALSE(B.open);
    TEST_ASSERT_FALSE(usbdisp_bt_awake(&D));
    usbdisp_poll(&D, 0);
    TEST_ASSERT_TRUE(B.open);
    usbdisp_bt_stop(&D);
}

/* A build without Bluetooth: no bt in hello, the command is unknown. */
static void test_build_without_bluetooth(void)
{
    usbdisp_bt_stop(&D);
    device_partial(16, 8, 3, 1, true, 20);
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HELLO, NULL, 0));
    TEST_ASSERT_EQUAL_HEX8(0xB0, s_ans[0]); /* 16 keys: fingerprint_hash, depth_modes and full_every_set are three */
    TEST_ASSERT_EQUAL_INT(USBDISP_E_UNKNOWN_CMD, req_err(USBDISP_CMD_BT, NULL, 0));
    TEST_ASSERT_FALSE(usbdisp_bt_button(&D));
    TEST_ASSERT_FALSE(usbdisp_bt_on(&D));
}

/* The pairing screens on an 800 x 480 panel, for a person to look at: TINY_BT_SCREEN_OUT=<file.pbm>. */
static uint8_t s_big[480][100];
static int big_write(void *ctx, const tiny_rect_t *r, const uint8_t *prev, const uint8_t *rows, size_t stride)
{
    (void)ctx, (void)prev;
    for (int j = 0; j < r->h; j++)
        memcpy(s_big[r->y + j], rows + (size_t)j * stride, 100);
    return 0;
}

static void test_screen_on_a_real_size(void)
{
    bt_device(1);
    C.w = 800;
    C.h = 480;
    C.band_rows = 80;
    C.panel_write = big_write;
    usbdisp_init(&D, &C);
    TEST_ASSERT_TRUE(usbdisp_bt_button(&D));
    int dark = 0, edge = 0;
    for (int j = 0; j < 480; j++)
        for (int i = 0; i < 800; i++) {
            bool on = !(s_big[j][i / 8] >> (7 - i % 8) & 1);
            dark += on;
            edge += on && (i < 8 || i >= 792 || j < 8 || j >= 472);
        }
    TEST_ASSERT_TRUE_MESSAGE(dark > 800 * 480 / 100 && dark < 800 * 480 / 4, "the text is missing or the panel black");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, edge, "the text runs off the panel");
    const char *out = getenv("TINY_BT_SCREEN_OUT");
    if (out) {
        FILE *f = fopen(out, "wb");
        TEST_ASSERT_NOT_NULL(f);
        fprintf(f, "P4\n800 480\n");
        for (int j = 0; j < 480; j++)
            for (int i = 0; i < 100; i++)
                fputc(~s_big[j][i] & 0xFF, f);
        fclose(f);
    }
    usbdisp_bt_stop(&D);
    usbdisp_reset(&D);
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_off_by_default_and_no_bluetooth_call);
    RUN_TEST(test_setting_survives_restart);
    RUN_TEST(test_start_failure);
    RUN_TEST(test_vectors);
    RUN_TEST(test_frames_in_small_pieces);
    RUN_TEST(test_lost_answer);
    RUN_TEST(test_cut_frame);
    RUN_TEST(test_unbonded_host_is_refused);
    RUN_TEST(test_pairing_window);
    RUN_TEST(test_awake_and_stop);
    RUN_TEST(test_build_without_bluetooth);
    RUN_TEST(test_screen_on_a_real_size);
    return UNITY_END();
}
