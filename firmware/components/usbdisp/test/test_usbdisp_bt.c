/* usbdisp: Bluetooth behind the setting (docs/spec/usb-display.md "Bluetooth") against a fake stack that counts every
 * call: off by default and no call while off, the setting kept over a restart and changed over USB only, the frames
 * over the second link (20-byte pieces, a lost answer, both links at once), a host that has not authenticated. The
 * key and the sealed frames: test_usbdisp_btkey.c. Vectors: usb-display.json "bluetooth".
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_json.h"
#include "usbdisp_fake_bt.h"

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

/* ------------------------------------------------------------------ the setting */

/* A new device: off, and nothing of the stack is ever called, whatever is asked over USB. */
static void test_off_by_default_and_no_bluetooth_call(void)
{
    bt_device(-1);
    TEST_ASSERT_FALSE(usbdisp_bt_on(&D));
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HELLO, NULL, 0));
    TEST_ASSERT_EQUAL_STRING("off", ans_str("bt"));
    size_t n = pattern_png(16, 8, 1, 0);
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 64));
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_CLEAR, NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, bt_cmd(-1, -1));
    TEST_ASSERT_EQUAL_INT(-1, map_int(s_ans, s_ans_len, "kid", -1));
    TEST_ASSERT_EQUAL_INT(0, bt_cmd(0, -1));
    TEST_ASSERT_EQUAL_INT(USBDISP_E_BAD_REQUEST, req_err(USBDISP_CMD_BT_AUTH, NULL, 0));
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
    TEST_ASSERT_EQUAL_INT(0, bt_cmd(1, -1));
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
    TEST_ASSERT_EQUAL_INT(0, bt_cmd(0, -1));
    TEST_ASSERT_EQUAL_INT(0, B.kv);
    TEST_ASSERT_EQUAL_INT(1, B.closes);
    TEST_ASSERT_FALSE(B.open);
    restart();
    usbdisp_poll(&D, 0);
    TEST_ASSERT_FALSE(usbdisp_bt_on(&D));
    TEST_ASSERT_EQUAL_INT(0, B.calls);
    /* a store that fails: refused, and still off */
    B.fail_kv = true;
    TEST_ASSERT_EQUAL_INT(USBDISP_E_IO, bt_cmd(1, -1));
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
    TEST_ASSERT_EQUAL_INT(USBDISP_E_IO, bt_cmd(1, -1));
    TEST_ASSERT_TRUE(usbdisp_bt_on(&D));
    int calls = B.calls;
    for (int i = 0; i < 10; i++)
        usbdisp_poll(&D, 0);
    TEST_ASSERT_EQUAL_INT(calls, B.calls);
    TEST_ASSERT_EQUAL_INT(0, F.refreshes);
    /* bt_key too: no key without the radio (its entropy) */
    TEST_ASSERT_EQUAL_INT(USBDISP_E_IO, req_err(USBDISP_CMD_BT_KEY, NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, B.randoms);
    TEST_ASSERT_EQUAL_INT(-1, B.key_len);
    B.fail_open = false;
    TEST_ASSERT_EQUAL_INT(0, bt_cmd(1, -1));
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
    TEST_ASSERT_EQUAL_UINT(10, sj_len(steps));
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
            B.conn = 10 + (uint32_t)i; /* a new connection: the last one was dropped */
            usbdisp_poll(&D, 0);
            int drops = B.drops;
            ble_bytes(tx, tn);
            TEST_ASSERT_EQUAL_INT_MESSAGE(drops + 1, B.drops, name);
            over_ble++;
        } else {
            to_device(tx, tn);
            usbdisp_poll(&D, 0);
            usbdisp_poll(&D, 0);
        }
        TEST_ASSERT_EQUAL_UINT_MESSAGE(wn, ble ? B.out_len : F.out_len, name);
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(want, ble ? B.out : F.out, wn, name);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0, ble ? F.out_len : B.out_len, name);
    }
    TEST_ASSERT_EQUAL_INT(3, over_ble);
    TEST_ASSERT_EQUAL_INT(1, B.forgets);
    TEST_ASSERT_EQUAL_INT(1, B.key_erases);
    TEST_ASSERT_FALSE(B.open);
    sj_free(v);
    TEST_ASSERT_EQUAL_INT(0, F.order_errors);
}

/* ------------------------------------------------------------------ the frames over Bluetooth */

/* MTU 23: every write brings 20 bytes (and less), one read a poll; a whole picture arrives sealed and is shown, the
 * answers leave over Bluetooth, and a USB request in the middle of a Bluetooth frame is answered on USB. */
static void test_frames_in_small_pieces(void)
{
    static const size_t pieces[] = {20, 1, 7, 244};
    for (size_t k = 0; k < sizeof pieces / sizeof pieces[0]; k++) {
        authed();
        B.piece = pieces[k];
        TEST_ASSERT_EQUAL_INT(0, ble_send(USBDISP_CMD_HELLO, NULL, 0));
        TEST_ASSERT_EQUAL_STRING("display", ans_str("project"));
        size_t n = pattern_png(16, 8, 1, 0);
        TEST_ASSERT_EQUAL_INT(0, ble_begin(n));
        /* half of the chunk's frame, then a whole USB request, then the rest */
        static uint8_t p[512], sealed[512 + BTSEAL_TAG], fr[600];
        cbor_w_t w;
        cbor_w_init(&w, p, sizeof p);
        cbor_put_map(&w, 2);
        cbor_put_str(&w, "off");
        cbor_put_uint(&w, 0);
        cbor_put_str(&w, "data");
        cbor_put_bstr(&w, s_png, n);
        TEST_ASSERT_EQUAL_INT(0, btseal_seal(&B.host, 9, USBDISP_CMD_SHOW_CHUNK, p, w.len, sealed));
        size_t fn = link_frame_encode(fr, sizeof fr, 9, USBDISP_CMD_SHOW_CHUNK, sealed, w.len + BTSEAL_TAG);
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
        TEST_ASSERT_EQUAL_INT(0, ble_answer(9, USBDISP_CMD_SHOW_CHUNK));
        TEST_ASSERT_EQUAL_INT((int)n, (int)map_int(s_ans, s_ans_len, "next", -1));
        TEST_ASSERT_EQUAL_INT(0, ble_send(USBDISP_CMD_SHOW_END, NULL, 0));
        TEST_ASSERT_EQUAL_STRING("full", ans_str("mode"));
        TEST_ASSERT_EQUAL_HEX8_ARRAY(s_px, F.shown, 16 * 8);
        TEST_ASSERT_EQUAL_INT(0, F.order_errors);
        TEST_ASSERT_EQUAL_INT(0, B.drops);
    }
    usbdisp_bt_stop(&D);
}

/* Lost bytes. A chunk whose frame arrives damaged (CRC): no answer, nothing counted, and the same sealed bytes again
 * are taken. Its answer is lost: the same bytes again are a replay, the link drops without an answer; a new connection
 * and handshake, and the transfer goes on where the device says. Nothing is shown twice or damaged. */
static void test_lost_answer(void)
{
    static uint8_t chunk[LINK_FRAME_MAX], bad[LINK_FRAME_MAX], p[256];
    authed();
    B.piece = 20;
    size_t n = pattern_png(16, 8, 1, 0), half = 40;
    TEST_ASSERT_TRUE(n > half && n < 200);
    TEST_ASSERT_EQUAL_INT(0, ble_begin(n));
    cbor_w_t w;
    cbor_w_init(&w, p, sizeof p);
    cbor_put_map(&w, 2);
    cbor_put_str(&w, "off");
    cbor_put_uint(&w, 0);
    cbor_put_str(&w, "data");
    cbor_put_bstr(&w, s_png, half);
    ble_frame(USBDISP_CMD_SHOW_CHUNK, p, w.len);
    size_t cn = s_last_len;
    uint8_t seq = s_bseq;
    memcpy(chunk, s_last, cn);
    memcpy(bad, chunk, cn);
    bad[cn - 1] ^= 1;
    TEST_ASSERT_EQUAL_UINT(0, ble_bytes(bad, cn));
    TEST_ASSERT_EQUAL_INT(0, B.drops);
    ble_bytes(chunk, cn);
    TEST_ASSERT_EQUAL_INT(0, ble_answer(seq, USBDISP_CMD_SHOW_CHUNK));
    TEST_ASSERT_EQUAL_INT((int)half, (int)map_int(s_ans, s_ans_len, "next", -1));
    /* that answer was lost: the copy is a replay */
    TEST_ASSERT_EQUAL_UINT(0, ble_bytes(chunk, cn));
    TEST_ASSERT_EQUAL_INT(1, B.drops);
    TEST_ASSERT_EQUAL_INT(0, host_auth(B.key));
    TEST_ASSERT_EQUAL_INT(USBDISP_E_SEQUENCE, ble_chunk(s_png, 0, half));
    const uint8_t *dv;
    size_t dn;
    TEST_ASSERT_TRUE(cbor_map_get(s_ans, s_ans_len, "detail", 6, &dv, &dn));
    TEST_ASSERT_EQUAL_INT((int)half, (int)map_int(dv, dn, "next", -1));
    TEST_ASSERT_EQUAL_INT(0, ble_chunk(s_png, half, n - half));
    TEST_ASSERT_EQUAL_INT(0, ble_send(USBDISP_CMD_SHOW_END, NULL, 0));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_px, F.shown, 16 * 8);
    TEST_ASSERT_EQUAL_INT(1, F.fulls);
    usbdisp_bt_stop(&D);
    TEST_ASSERT_EQUAL_INT(0, (usbdisp_reset(&D), F.live));
}

/* A host that left in the middle of a frame: the next host's first request is not read as the rest of it, and half
 * a frame of a host that stays is forgotten after 2 s as on USB. */
static void test_cut_frame(void)
{
    authed();
    static uint8_t big[400], fr[512];
    size_t fn = link_frame_encode(fr, sizeof fr, 5, USBDISP_CMD_SHOW_CHUNK, big, sizeof big); /* 400 bytes announced */
    memcpy(B.in, fr, fn);
    B.in_len = fn / 2;
    usbdisp_poll(&D, 0);
    B.conn = 0;
    usbdisp_poll(&D, 0);
    TEST_ASSERT_EQUAL_INT(0, host_auth(B.key));
    TEST_ASSERT_EQUAL_INT(0, ble_send(USBDISP_CMD_HELLO, NULL, 0));
    memcpy(B.in, fr, fn);
    B.in_len = fn / 2;
    B.in_pos = 0;
    usbdisp_poll(&D, 0);
    F.now += USBDISP_FRAME_IDLE_US;
    usbdisp_poll(&D, 0);
    TEST_ASSERT_EQUAL_INT(0, ble_send(USBDISP_CMD_HELLO, NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, B.drops);
    usbdisp_bt_stop(&D);
}

/* ------------------------------------------------------------------ safety */

/* Not authenticated: every request gets a short AUTH and the link drops; the panel and the device's state stay as
 * they were. An authenticated host cannot change the setting or the key either. */
static void test_unauthenticated_host_is_refused(void)
{
    authed();
    btseal_end(&B.host); /* a stranger on a new connection: plain frames */
    B.piece = 20;
    size_t n = pattern_png(16, 8, 1, 0);
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 64)); /* over USB: the picture the stranger must not change */
    static uint8_t before[MAXPIX];
    memcpy(before, F.shown, sizeof before);
    int refreshes = F.refreshes;
    /* a transfer of the USB host is open */
    uint8_t sz[] = {0xA1, 0x64, 's', 'i', 'z', 'e', 0x18, (uint8_t)n};
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_SHOW_BEGIN, sz, sizeof sz));
    F.now += 5000000;
    static const uint8_t cmds[] = {USBDISP_CMD_HELLO, USBDISP_CMD_SHOW_BEGIN, USBDISP_CMD_SHOW_CHUNK, USBDISP_CMD_SHOW_END,
                                   USBDISP_CMD_CLEAR, USBDISP_CMD_SET, USBDISP_CMD_BT, USBDISP_CMD_BT_KEY,
                                   USBDISP_CMD_HAVE, USBDISP_CMD_BT_AUTH, 0x20};
    for (size_t i = 0; i < sizeof cmds; i++) {
        B.conn = 100 + (uint32_t)i;
        usbdisp_poll(&D, 0);
        uint64_t last = D.bt_last_us;
        F.now += 1000;
        int drops = B.drops;
        TEST_ASSERT_EQUAL_INT_MESSAGE(USBDISP_E_AUTH, ble_send(cmds[i], sz, sizeof sz), "not AUTH");
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xA2, s_ans[0], "more than err and msg"); /* a map of two */
        TEST_ASSERT_EQUAL_INT_MESSAGE(drops + 1, B.drops, "the link stayed");
        TEST_ASSERT_EQUAL_UINT64_MESSAGE(last, D.bt_last_us, "the stranger keeps the device awake");
    }
    TEST_ASSERT_EQUAL_INT(refreshes, F.refreshes);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(before, F.shown, sizeof before);
    TEST_ASSERT_TRUE_MESSAGE(usbdisp_transfer(&D), "the stranger dropped the USB host's transfer");
    TEST_ASSERT_NOT_NULL_MESSAGE(usbdisp_prev(&D, NULL), "the stranger forced a full refresh");
    TEST_ASSERT_TRUE(usbdisp_bt_on(&D));
    TEST_ASSERT_EQUAL_INT(0, B.key_sets);
    /* the setting and the key cannot be changed over Bluetooth by an authenticated host either */
    TEST_ASSERT_EQUAL_INT(0, host_auth(B.key));
    uint8_t off[] = {0xA1, 0x62, 'o', 'n', 0xF4};
    TEST_ASSERT_EQUAL_INT(USBDISP_E_CABLE, ble_send(USBDISP_CMD_BT, off, sizeof off));
    uint8_t forget[] = {0xA1, 0x66, 'f', 'o', 'r', 'g', 'e', 't', 0xF5};
    TEST_ASSERT_EQUAL_INT(USBDISP_E_CABLE, ble_send(USBDISP_CMD_BT, forget, sizeof forget));
    TEST_ASSERT_EQUAL_INT(USBDISP_E_CABLE, ble_send(USBDISP_CMD_BT_KEY, NULL, 0));
    TEST_ASSERT_EQUAL_INT(USBDISP_E_BAD_REQUEST, ble_send(USBDISP_CMD_BT_AUTH, NULL, 0));
    TEST_ASSERT_TRUE(usbdisp_bt_on(&D));
    TEST_ASSERT_EQUAL_INT(1, B.kv);
    TEST_ASSERT_EQUAL_INT(0, B.forgets + B.key_sets + B.key_erases);
    usbdisp_bt_stop(&D);
}

/* A connected host keeps the device awake only while it asks; a stopped stack is started by the next poll. */
static void test_awake_and_stop(void)
{
    authed();
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

/* A build without Bluetooth: no bt in hello, the commands are unknown. */
static void test_build_without_bluetooth(void)
{
    usbdisp_bt_stop(&D);
    device_partial(16, 8, 3, 1, true, 20);
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HELLO, NULL, 0));
    TEST_ASSERT_EQUAL_HEX8(0xB0, s_ans[0]); /* 16 keys: fingerprint_hash, depth_modes and full_every_set are three */
    TEST_ASSERT_EQUAL_INT(USBDISP_E_UNKNOWN_CMD, req_err(USBDISP_CMD_BT, NULL, 0));
    TEST_ASSERT_EQUAL_INT(USBDISP_E_UNKNOWN_CMD, req_err(USBDISP_CMD_BT_KEY, NULL, 0));
    TEST_ASSERT_EQUAL_INT(USBDISP_E_UNKNOWN_CMD, req_err(USBDISP_CMD_BT_AUTH, NULL, 0));
    TEST_ASSERT_FALSE(usbdisp_bt_on(&D));
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
    RUN_TEST(test_unauthenticated_host_is_refused);
    RUN_TEST(test_awake_and_stop);
    RUN_TEST(test_build_without_bluetooth);
    return UNITY_END();
}
