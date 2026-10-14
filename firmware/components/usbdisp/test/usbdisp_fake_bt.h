/* usbdisp tests: a fake Bluetooth stack that counts every call, the stored setting and key, and a host that does the
 * handshake and seals its requests with btseal (the device's own crypto, checked against the vectors in
 * components/btseal). Shared by test_usbdisp_bt.c and test_usbdisp_btkey.c. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "usbdisp_fake.h"

static struct {
    int calls;                 /* every bt_* call and random */
    int opens, closes, forgets, drops, randoms;
    bool open, fail_open;
    uint32_t conn;             /* the connection's number, 0: nobody connected */
    int kv;                    /* the stored setting, -1 none */
    int kv_sets;
    bool fail_kv;
    uint8_t key[BTSEAL_KEY];   /* the stored key */
    int key_len;               /* its length, -1 none */
    int key_sets, key_erases;
    uint8_t rnd[64];           /* what the RNG gives first (rnd_pos..rnd_len), then a counter pattern */
    size_t rnd_len, rnd_pos;
    uint8_t in[1 << 15], out[1 << 15];
    size_t in_len, in_pos, out_len;
    size_t piece;              /* bytes a read gives at most (MTU - 3) */
    int drop_writes;           /* answers lost on the way to the host */
    btseal_t host;             /* the host's session */
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
    B.open = false;
    B.conn = 0;
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

static uint32_t b_connected(void *ctx)
{
    (void)ctx;
    B.calls++;
    return B.open ? B.conn : 0;
}

/* The device ends the connection: the host is gone, unread bytes of it too. */
static void b_drop(void *ctx)
{
    (void)ctx;
    B.calls++;
    TEST_ASSERT_TRUE(B.open);
    B.conn = 0;
    B.in_len = B.in_pos = 0;
    B.drops++;
}

static void b_forget(void *ctx)
{
    (void)ctx;
    B.calls++;
    TEST_ASSERT_FALSE_MESSAGE(B.open, "the old bonds go with the stack stopped: the connection ends");
    B.forgets++;
}

static void b_random(void *ctx, void *buf, size_t n)
{
    (void)ctx;
    B.calls++;
    B.randoms++;
    TEST_ASSERT_TRUE_MESSAGE(B.open, "random bytes with the radio off: no entropy");
    uint8_t *p = buf;
    for (size_t i = 0; i < n; i++)
        p[i] = B.rnd_pos < B.rnd_len ? B.rnd[B.rnd_pos++] : (uint8_t)(B.randoms * 31 + i);
}

static int b_kv_get(void *ctx, const char *key, void *buf, size_t cap)
{
    if (!strcmp(key, USBDISP_KV_BTKEY)) {
        if (B.key_len < 0 || (size_t)B.key_len > cap)
            return -1;
        memcpy(buf, B.key, (size_t)B.key_len);
        return B.key_len;
    }
    if (strcmp(key, USBDISP_KV_BT))
        return f_kv_get(ctx, key, buf, cap);
    if (B.kv < 0 || !cap)
        return -1;
    *(uint8_t *)buf = (uint8_t)B.kv;
    return 1;
}

static int b_kv_set(void *ctx, const char *key, const void *buf, size_t len)
{
    if (B.fail_kv && (!strcmp(key, USBDISP_KV_BTKEY) || !strcmp(key, USBDISP_KV_BT)))
        return -1;
    if (!strcmp(key, USBDISP_KV_BTKEY)) {
        TEST_ASSERT_TRUE(len <= sizeof B.key);
        memcpy(B.key, buf, len);
        B.key_len = (int)len;
        B.key_sets++;
        return 0;
    }
    if (strcmp(key, USBDISP_KV_BT))
        return f_kv_set(ctx, key, buf, len);
    if (len != 1)
        return -1;
    B.kv = *(const uint8_t *)buf;
    B.kv_sets++;
    return 0;
}

static int b_kv_erase(void *ctx, const char *key)
{
    if (strcmp(key, USBDISP_KV_BTKEY))
        return f_kv_erase(ctx, key);
    if (B.fail_kv)
        return -1;
    B.key_len = -1;
    B.key_erases++;
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
    C.bt_drop = b_drop;
    C.bt_forget = b_forget;
    C.random = b_random;
    C.kv_get = b_kv_get;
    C.kv_set = b_kv_set;
    C.kv_erase = b_kv_erase;
}

/* The vectors' gray panel with Bluetooth; kv: the stored setting (-1: a new device), no key. */
static void bt_device(int kv)
{
    usbdisp_bt_stop(&D);
    memset(&B, 0, sizeof B);
    B.kv = kv;
    B.key_len = -1;
    device_partial(16, 8, 3, 1, true, 20);
    bt_cfg();
    usbdisp_init(&D, &C);
}

/* Power off and on: the store stays (setting and key), everything else starts again. */
static void restart(void)
{
    if (D.bt) {
        free(D.bt);
        F.live--;
    }
    int kv = B.kv, key_len = B.key_len;
    uint8_t key[BTSEAL_KEY];
    memcpy(key, B.key, sizeof key);
    usbdisp_reset(&D);
    memset(&B, 0, sizeof B);
    B.kv = kv;
    B.key_len = key_len;
    memcpy(B.key, key, sizeof key);
    usbdisp_init(&D, &C);
}

static int bt_cmd(int on, int forget) /* -1: key absent */
{
    uint8_t p[32];
    cbor_w_t w;
    cbor_w_init(&w, p, sizeof p);
    cbor_put_map(&w, (size_t)((on >= 0) + (forget >= 0)));
    if (on >= 0) {
        cbor_put_str(&w, "on");
        cbor_put_bool(&w, on);
    }
    if (forget >= 0) {
        cbor_put_str(&w, "forget");
        cbor_put_bool(&w, forget);
    }
    return req_err(USBDISP_CMD_BT, p, w.len);
}

/* Bytes from the host over Bluetooth, read in pieces of B.piece with a poll for each, then two polls more. Returns
 * the bytes the device sent back (in B.out). */
static size_t ble_bytes(const uint8_t *bytes, size_t n)
{
    TEST_ASSERT_TRUE(n <= sizeof B.in);
    memcpy(B.in, bytes, n);
    B.in_len = n;
    B.in_pos = 0;
    B.out_len = 0;
    size_t usb_before = F.out_len;
    for (int i = 0; i < 4000 && D.bt && B.in_pos < B.in_len; i++)
        usbdisp_poll(&D, 0);
    usbdisp_poll(&D, 0);
    usbdisp_poll(&D, 0);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(usb_before, F.out_len, "an answer to a Bluetooth request went to USB");
    return B.out_len;
}

/* The last request sent sealed over Bluetooth, to send the same bytes again */
static uint8_t s_last[LINK_FRAME_MAX];
static size_t s_last_len;
static uint8_t s_bseq = 100;
static uint8_t s_plain[LINK_MAX_PAYLOAD];

/* The device's answer in B.out, opened with the host's session when it is up: its err (0: a success map, -1: no
 * answer). The answer's payload stays in s_ans. */
static int ble_answer(uint8_t seq, uint8_t cmd)
{
    if (B.out_len < LINK_FRAME_OVERHEAD)
        return -1;
    size_t plen = (size_t)(B.out[2] | B.out[3] << 8);
    TEST_ASSERT_EQUAL_UINT(plen + LINK_FRAME_OVERHEAD, B.out_len);
    TEST_ASSERT_EQUAL_HEX8(seq, B.out[4]);
    TEST_ASSERT_EQUAL_HEX8(cmd | 0x80, B.out[5]);
    s_ans = B.out + 6;
    s_ans_len = plen;
    if (B.host.up) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, btseal_open(&B.host, seq, cmd | 0x80, B.out + 6, plen, s_plain),
                                      "the answer did not open");
        s_ans = s_plain;
        s_ans_len = plen - BTSEAL_TAG;
    }
    TEST_ASSERT_TRUE(cbor_valid(s_ans, s_ans_len));
    return (int)map_int(s_ans, s_ans_len, "err", 0);
}

/* One request's frame into s_last (the next seq), sealed when the host's session is up; nothing sent. */
static void ble_frame(uint8_t cmd, const uint8_t *payload, size_t len)
{
    static uint8_t sealed[LINK_MAX_PAYLOAD];
    s_bseq = (uint8_t)(s_bseq % 255 + 1);
    if (B.host.up) {
        TEST_ASSERT_EQUAL_INT(0, btseal_seal(&B.host, s_bseq, cmd, payload, len, sealed));
        payload = sealed;
        len += BTSEAL_TAG;
    }
    s_last_len = link_frame_encode(s_last, sizeof s_last, s_bseq, cmd, payload, len);
    TEST_ASSERT_TRUE(s_last_len > 0);
}

/* One request over Bluetooth, sealed when the host's session is up; the err of its answer as ble_answer. */
static int ble_send(uint8_t cmd, const uint8_t *payload, size_t len)
{
    ble_frame(cmd, payload, len);
    ble_bytes(s_last, s_last_len);
    return ble_answer(s_bseq, cmd);
}

/* A host connects (a new connection) and proves the stored key: 0, or the err of the refusal (-1: no answer). */
static int host_auth(const uint8_t key[BTSEAL_KEY])
{
    uint8_t p[64], kid[BTSEAL_KID], hn[BTSEAL_NONCE] = {7, 7, 7}, dn[BTSEAL_NONCE], proof[BTSEAL_TAG];
    static uint32_t next_conn = 1;
    btseal_end(&B.host);
    B.conn = ++next_conn;
    usbdisp_poll(&D, 0);
    TEST_ASSERT_EQUAL_INT(0, btseal_kid(key, kid));
    cbor_w_t w;
    cbor_w_init(&w, p, sizeof p);
    cbor_put_map(&w, 2);
    cbor_put_str(&w, "kid");
    cbor_put_bstr(&w, kid, sizeof kid);
    cbor_put_str(&w, "nonce");
    cbor_put_bstr(&w, hn, sizeof hn);
    int err = ble_send(USBDISP_CMD_BT_AUTH, p, w.len);
    if (err)
        return err;
    const uint8_t *v;
    size_t n;
    cbor_r_t r;
    cbor_item_t it;
    TEST_ASSERT_TRUE(cbor_map_get(s_ans, s_ans_len, "nonce", 5, &v, &n));
    cbor_r_init(&r, v, n);
    TEST_ASSERT_TRUE(cbor_read(&r, &it) && it.type == CBOR_BSTR && it.len == sizeof dn);
    memcpy(dn, it.ptr, sizeof dn);
    TEST_ASSERT_TRUE(cbor_map_get(s_ans, s_ans_len, "proof", 5, &v, &n));
    cbor_r_init(&r, v, n);
    TEST_ASSERT_TRUE(cbor_read(&r, &it) && it.type == CBOR_BSTR && it.len == sizeof proof);
    memcpy(proof, it.ptr, sizeof proof);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, btseal_start(&B.host, false, key, hn, dn, proof), "the device's proof is wrong");
    return 0;
}

/* A device with the setting on, a key stored and an authenticated host connected. */
static void authed(void)
{
    bt_device(1);
    for (size_t i = 0; i < BTSEAL_KEY; i++)
        B.key[i] = (uint8_t)(0x40 + i);
    B.key_len = BTSEAL_KEY;
    usbdisp_poll(&D, 0);
    TEST_ASSERT_TRUE(B.open);
    TEST_ASSERT_EQUAL_INT(0, host_auth(B.key));
}
