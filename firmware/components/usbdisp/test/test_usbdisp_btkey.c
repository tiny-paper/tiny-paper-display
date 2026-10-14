/* usbdisp: the Bluetooth key (docs/spec/usb-display.md "Bluetooth key"). The device replays docs/spec/vectors/
 * bt-key.json byte for byte (bt_key over the cable, the handshake, sealed frames both ways) and refuses each of its
 * bad frames by dropping the link without an answer; the key survives a restart, forget erases it, a new key ends the
 * old one, a new connection gets no session it did not make, and the key is made only while the radio runs.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "../../btseal/test/btkey_vec.h"
#include "usbdisp_fake_bt.h"

static sj_t *V;
static uint8_t s_tx[LINK_FRAME_MAX], s_want[LINK_FRAME_MAX];

/* Over USB: the vector's tx in, its rx out byte for byte */
static void usb_step(const sj_t *st, const char *name)
{
    size_t tn = bv_get(st, "tx", s_tx, sizeof s_tx), wn = bv_get(st, "rx", s_want, sizeof s_want);
    F.out_len = 0;
    to_device(s_tx, tn);
    usbdisp_poll(&D, 0);
    usbdisp_poll(&D, 0);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(wn, F.out_len, name);
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(s_want, F.out, wn, name);
}

/* Over Bluetooth: `tx` in; the device's bytes must be `rx` (wn 0: nothing) */
static void ble_step(const char *tx, const char *rx, const char *name)
{
    size_t tn = bv_hex(tx, s_tx, sizeof s_tx), wn = rx ? bv_hex(rx, s_want, sizeof s_want) : 0;
    ble_bytes(s_tx, tn);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(wn, B.out_len, name);
    if (wn)
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(s_want, B.out, wn, name);
}

/* A new device whose RNG gives the vector's key: bt_key over the cable as in the file. */
static void vector_device(void)
{
    bt_device(-1);
    B.rnd_len = bv_get(V, "key", B.rnd, sizeof B.rnd);
    usb_step(sj_get(V, "bt_key"), "bt_key");
    TEST_ASSERT_EQUAL_INT(BTSEAL_KEY, B.key_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(B.rnd, B.key, BTSEAL_KEY);
    TEST_ASSERT_EQUAL_INT(1, B.kv);
}

/* A host connects and does the file's handshake (the device's nonce from the RNG), then the first `upto` frames. */
static void vector_session(size_t upto)
{
    static uint32_t conn = 50;
    const sj_t *auth = sj_get(V, "auth"), *frames = sj_get(V, "frames");
    B.conn = ++conn;
    usbdisp_poll(&D, 0);
    B.rnd_pos = 0;
    B.rnd_len = bv_get(V, "device_nonce", B.rnd, sizeof B.rnd);
    ble_step(sj_str(sj_get(auth, "tx"), ""), sj_str(sj_get(auth, "rx"), ""), "auth");
    for (size_t i = 0; i < upto; i += 2) {
        const sj_t *q = sj_at(frames, i), *a = sj_at(frames, i + 1);
        ble_step(sj_str(sj_get(q, "wire"), ""), sj_str(sj_get(a, "wire"), ""), sj_str(sj_get(q, "name"), "?"));
    }
}

static void test_vectors(void)
{
    vector_device();
    vector_session(4);
    TEST_ASSERT_EQUAL_INT(0, B.drops);
    /* each refused frame where frames[2] is due: no answer, the link dropped; the good one then opens nothing either */
    const sj_t *refused = sj_get(V, "refused");
    TEST_ASSERT_EQUAL_UINT(5, sj_len(refused));
    const char *good = sj_str(sj_get(sj_at(sj_get(V, "frames"), 2), "wire"), "");
    for (size_t i = 0; i < sj_len(refused); i++) {
        const char *name = sj_str(sj_get(sj_at(refused, i), "name"), "?");
        vector_session(2);
        int drops = B.drops;
        ble_step(sj_str(sj_get(sj_at(refused, i), "wire"), ""), NULL, name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(drops + 1, B.drops, name);
        TEST_ASSERT_FALSE_MESSAGE(D.bt->seal.up, name);
        B.conn = 900 + (uint32_t)i; /* the host connects again but skips the handshake: a plain AUTH only */
        size_t gn = bv_hex(good, s_tx, sizeof s_tx);
        ble_bytes(s_tx, gn);
        btseal_end(&B.host);
        TEST_ASSERT_EQUAL_INT_MESSAGE(USBDISP_E_AUTH, ble_answer(s_tx[4], s_tx[5]), name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(drops + 2, B.drops, name);
    }
    TEST_ASSERT_EQUAL_INT(0, F.refreshes);
    usbdisp_bt_stop(&D);
}

/* The key is in the store: a restart (the settings read again) keeps it, the same host authenticates again. */
static void test_key_survives_restart(void)
{
    vector_device();
    restart();
    usbdisp_poll(&D, 0);
    TEST_ASSERT_TRUE(B.open);
    vector_session(4);
    TEST_ASSERT_EQUAL_INT(0, B.drops);
    /* bt {} says which key is there (the id, never the key) */
    TEST_ASSERT_EQUAL_INT(0, bt_cmd(-1, -1));
    uint8_t kid[BTSEAL_KID];
    bv_get(V, "kid", kid, sizeof kid);
    const uint8_t *v;
    size_t n;
    TEST_ASSERT_TRUE(cbor_map_get(s_ans, s_ans_len, "kid", 3, &v, &n));
    TEST_ASSERT_EQUAL_UINT(1 + BTSEAL_KID, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(kid, v + 1, BTSEAL_KID);
    TEST_ASSERT_FALSE(cbor_map_get(s_ans, s_ans_len, "key", 3, &v, &n));
    usbdisp_bt_stop(&D);
}

/* forget: the key is erased, the connected host leaves, and with the same key nobody gets in. */
static void test_forget_erases_the_key(void)
{
    authed();
    uint8_t key[BTSEAL_KEY];
    memcpy(key, B.key, sizeof key);
    TEST_ASSERT_EQUAL_INT(0, bt_cmd(-1, 1));
    TEST_ASSERT_EQUAL_INT(-1, B.key_len);
    TEST_ASSERT_EQUAL_INT(1, B.key_erases);
    TEST_ASSERT_EQUAL_INT(1, B.closes);
    TEST_ASSERT_TRUE(B.open); /* the setting stays on */
    TEST_ASSERT_EQUAL_INT(-1, map_int(s_ans, s_ans_len, "kid", -1));
    TEST_ASSERT_FALSE(cbor_map_get(s_ans, s_ans_len, "kid", 3, &(const uint8_t *){0}, &(size_t){0}));
    TEST_ASSERT_EQUAL_INT(USBDISP_E_AUTH, host_auth(key));
    restart();
    usbdisp_poll(&D, 0);
    TEST_ASSERT_EQUAL_INT(USBDISP_E_AUTH, host_auth(key));
    /* an erase that fails is refused, not passed over */
    authed();
    B.fail_kv = true;
    TEST_ASSERT_EQUAL_INT(USBDISP_E_IO, bt_cmd(-1, 1));
    TEST_ASSERT_EQUAL_INT(BTSEAL_KEY, B.key_len);
    usbdisp_bt_stop(&D);
}

/* A second bt_key: a new key, the host with the old one leaves and cannot come back with it. */
static void test_new_key_replaces_the_old(void)
{
    authed();
    uint8_t old[BTSEAL_KEY];
    memcpy(old, B.key, sizeof old);
    int drops = B.drops;
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_BT_KEY, NULL, 0));
    TEST_ASSERT_EQUAL_INT(drops + 1, B.drops);
    TEST_ASSERT_FALSE(D.bt->seal.up);
    TEST_ASSERT_FALSE(memcmp(old, B.key, sizeof old) == 0);
    const uint8_t *v;
    size_t n;
    TEST_ASSERT_TRUE(cbor_map_get(s_ans, s_ans_len, "key", 3, &v, &n));
    TEST_ASSERT_EQUAL_UINT(2 + BTSEAL_KEY, n); /* bstr of 32: 0x58 0x20 */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(B.key, v + 2, BTSEAL_KEY);
    TEST_ASSERT_EQUAL_INT(USBDISP_E_AUTH, host_auth(old));
    TEST_ASSERT_EQUAL_INT(0, host_auth(B.key));
    TEST_ASSERT_EQUAL_INT(0, ble_send(USBDISP_CMD_HELLO, NULL, 0));
    /* the store fails: refused, the old key stays */
    memcpy(old, B.key, sizeof old);
    B.fail_kv = true;
    TEST_ASSERT_EQUAL_INT(USBDISP_E_IO, req_err(USBDISP_CMD_BT_KEY, NULL, 0));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(old, B.key, sizeof old);
    usbdisp_bt_stop(&D);
}

/* bt_key on a device that is off: switches it on first, and the RNG is read only with the radio running. */
static void test_key_made_with_the_radio_on(void)
{
    bt_device(-1);
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_BT_KEY, NULL, 0));
    TEST_ASSERT_EQUAL_INT(1, B.kv);
    TEST_ASSERT_EQUAL_INT(1, B.opens);
    TEST_ASSERT_EQUAL_INT(1, B.randoms); /* b_random fails the test when the stack is not open */
    TEST_ASSERT_EQUAL_INT(BTSEAL_KEY, B.key_len);
    usbdisp_bt_stop(&D);
}

/* A sealed hello from the host's old session; the device's answer read as a plain one (the host's session ended) */
static int sealed_after_end(void)
{
    ble_frame(USBDISP_CMD_HELLO, NULL, 0);
    ble_bytes(s_last, s_last_len);
    btseal_end(&B.host);
    return ble_answer(s_bseq, USBDISP_CMD_HELLO);
}

/* A session belongs to its connection: a new one (another host, or the same after a drop) must authenticate. */
static void test_new_connection_has_no_session(void)
{
    authed();
    TEST_ASSERT_EQUAL_INT(0, ble_send(USBDISP_CMD_HELLO, NULL, 0));
    B.conn += 1000; /* another connection, between two polls: the old session is not its */
    int drops = B.drops;
    TEST_ASSERT_EQUAL_INT(USBDISP_E_AUTH, sealed_after_end());
    TEST_ASSERT_EQUAL_INT(drops + 1, B.drops);
    /* the host left and came back on a new connection without a handshake: plain or sealed, nothing */
    TEST_ASSERT_EQUAL_INT(0, host_auth(B.key));
    TEST_ASSERT_EQUAL_INT(0, ble_send(USBDISP_CMD_HELLO, NULL, 0));
    B.conn = 0;
    usbdisp_poll(&D, 0);
    TEST_ASSERT_FALSE(D.bt->seal.up);
    B.conn = 5000;
    TEST_ASSERT_EQUAL_INT(USBDISP_E_AUTH, sealed_after_end());
    TEST_ASSERT_EQUAL_INT(drops + 2, B.drops);
    usbdisp_bt_stop(&D);
}

/* Handshakes that fail: another key id, a short nonce, no key stored; each a short AUTH and the link dropped. */
static void test_bad_handshakes(void)
{
    authed();
    uint8_t other[BTSEAL_KEY];
    memcpy(other, B.key, sizeof other);
    other[31] ^= 1;
    TEST_ASSERT_EQUAL_INT(USBDISP_E_AUTH, host_auth(other));
    TEST_ASSERT_EQUAL_HEX8(0xA2, s_ans[0]);
    uint8_t short_nonce[] = {0xA2, 0x63, 'k', 'i', 'd', 0x48, 0, 0, 0, 0, 0, 0, 0, 0, 0x65, 'n', 'o', 'n', 'c', 'e', 0x41, 0};
    btseal_kid(B.key, short_nonce + 6);
    B.conn++;
    usbdisp_poll(&D, 0);
    TEST_ASSERT_EQUAL_INT(USBDISP_E_AUTH, ble_send(USBDISP_CMD_BT_AUTH, short_nonce, sizeof short_nonce));
    uint8_t junk[] = {0x01, 0x02};
    B.conn++;
    usbdisp_poll(&D, 0);
    TEST_ASSERT_EQUAL_INT(USBDISP_E_AUTH, ble_send(USBDISP_CMD_BT_AUTH, junk, sizeof junk));
    B.key_len = 16; /* a stored value of another length is no key */
    TEST_ASSERT_EQUAL_INT(USBDISP_E_AUTH, host_auth(B.key));
    TEST_ASSERT_EQUAL_INT(4, B.drops);
    usbdisp_bt_stop(&D);
}

void setUp(void) { V = bv_load(); }
void tearDown(void) { sj_free(V); }

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_vectors);
    RUN_TEST(test_key_survives_restart);
    RUN_TEST(test_forget_erases_the_key);
    RUN_TEST(test_new_key_replaces_the_old);
    RUN_TEST(test_key_made_with_the_radio_on);
    RUN_TEST(test_new_connection_has_no_session);
    RUN_TEST(test_bad_handshakes);
    return UNITY_END();
}
