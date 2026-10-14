/* btseal: the Bluetooth key's crypto against docs/spec/vectors/bt-key.json (the Python side checks the same file):
 * key id, handshake proof, sealed frames both ways, and every refusal: a forged tag, a replayed counter, a wrong key,
 * the other direction, a plain frame; after a refusal the session is over. SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "btkey_vec.h"
#include "btseal.h"
#include "link_frame.h"

static sj_t *V;
static uint8_t key[BTSEAL_KEY], hn[BTSEAL_NONCE], dn[BTSEAL_NONCE], proof[BTSEAL_TAG];

void setUp(void)
{
    V = bv_load();
    bv_get(V, "key", key, sizeof key);
    bv_get(V, "host_nonce", hn, sizeof hn);
    bv_get(V, "device_nonce", dn, sizeof dn);
    bv_get(V, "proof", proof, sizeof proof);
}

void tearDown(void) { sj_free(V); }

typedef struct {
    uint8_t wire[LINK_FRAME_MAX];
    size_t n;
} wire_t;

static wire_t frame_of(const sj_t *f)
{
    wire_t w;
    w.n = bv_get(f, "wire", w.wire, sizeof w.wire);
    TEST_ASSERT_TRUE(w.n >= LINK_FRAME_OVERHEAD);
    return w;
}

static int open_wire(btseal_t *s, const wire_t *w, uint8_t *plain)
{
    return btseal_open(s, w->wire[4], w->wire[5], w->wire + 6, w->n - LINK_FRAME_OVERHEAD, plain);
}

static void device(btseal_t *s)
{
    uint8_t p[BTSEAL_TAG];
    TEST_ASSERT_EQUAL_INT(0, btseal_start(s, true, key, hn, dn, p));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(proof, p, sizeof p);
}

static void test_kid(void)
{
    uint8_t kid[BTSEAL_KID], want[BTSEAL_KID];
    bv_get(V, "kid", want, sizeof want);
    TEST_ASSERT_EQUAL_INT(0, btseal_kid(key, kid));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, kid, sizeof kid);
}

/* Both sides from the vectors: the device seals what the file says it sends, the host opens it, and back. */
static void test_frames_both_ways(void)
{
    btseal_t dev, host;
    uint8_t plain[LINK_MAX_PAYLOAD], want[LINK_MAX_PAYLOAD], out[LINK_MAX_PAYLOAD];
    device(&dev);
    TEST_ASSERT_EQUAL_INT(0, btseal_start(&host, false, key, hn, dn, proof));
    const sj_t *frames = sj_get(V, "frames");
    TEST_ASSERT_EQUAL_UINT(4, sj_len(frames));
    for (size_t i = 0; i < sj_len(frames); i++) {
        const sj_t *f = sj_at(frames, i);
        bool h2d = !strcmp(sj_str(sj_get(f, "dir"), ""), "h2d");
        wire_t w = frame_of(f);
        size_t pn = bv_get(f, "plain", want, sizeof want);
        uint8_t seq = w.wire[4], cmd = w.wire[5];
        /* the sender's bytes are the file's */
        TEST_ASSERT_EQUAL_INT(0, btseal_seal(h2d ? &host : &dev, seq, cmd, want, pn, out));
        TEST_ASSERT_EQUAL_UINT(pn + BTSEAL_TAG, w.n - LINK_FRAME_OVERHEAD);
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(w.wire + 6, out, pn + BTSEAL_TAG, sj_str(sj_get(f, "name"), "?"));
        /* the receiver opens them */
        TEST_ASSERT_EQUAL_INT(0, open_wire(h2d ? &dev : &host, &w, plain));
        if (pn)
            TEST_ASSERT_EQUAL_HEX8_ARRAY(want, plain, pn);
    }
}

/* Each refused frame where frames[2] is due: refused, and the session is over (the good frame no longer opens). */
static void test_refused(void)
{
    uint8_t plain[LINK_MAX_PAYLOAD];
    const sj_t *refused = sj_get(V, "refused");
    wire_t first = frame_of(sj_at(sj_get(V, "frames"), 0)), good = frame_of(sj_at(sj_get(V, "frames"), 2));
    TEST_ASSERT_EQUAL_UINT(5, sj_len(refused));
    for (size_t i = 0; i < sj_len(refused); i++) {
        const char *name = sj_str(sj_get(sj_at(refused, i), "name"), "?");
        btseal_t dev;
        device(&dev);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, open_wire(&dev, &first, plain), name);
        wire_t bad = frame_of(sj_at(refused, i));
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, open_wire(&dev, &bad, plain), name);
        TEST_ASSERT_FALSE_MESSAGE(dev.up, name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, open_wire(&dev, &good, plain), name);
    }
    /* the control: the good frame does open in that place */
    btseal_t dev;
    device(&dev);
    TEST_ASSERT_EQUAL_INT(0, open_wire(&dev, &first, plain));
    TEST_ASSERT_EQUAL_INT(0, open_wire(&dev, &good, plain));
}

/* A host with another key: the device's proof does not check, no session. */
static void test_wrong_key_proof(void)
{
    btseal_t host;
    uint8_t other[BTSEAL_KEY];
    memcpy(other, key, sizeof other);
    other[0] ^= 1;
    TEST_ASSERT_EQUAL_INT(-1, btseal_start(&host, false, other, hn, dn, proof));
    TEST_ASSERT_FALSE(host.up);
    uint8_t p[BTSEAL_TAG];
    memcpy(p, proof, sizeof p);
    p[15] ^= 0x80;
    TEST_ASSERT_EQUAL_INT(-1, btseal_start(&host, false, key, hn, dn, p));
    TEST_ASSERT_EQUAL_INT(0, btseal_start(&host, false, key, hn, dn, proof));
    TEST_ASSERT_TRUE(host.up);
}

/* Nothing seals or opens without a session; a payload shorter than a tag is refused. */
static void test_no_session(void)
{
    btseal_t s;
    uint8_t buf[64] = {0}, out[64];
    memset(&s, 0, sizeof s);
    TEST_ASSERT_EQUAL_INT(-1, btseal_seal(&s, 1, 1, buf, 4, out));
    TEST_ASSERT_EQUAL_INT(-1, btseal_open(&s, 1, 1, buf, 20, out));
    device(&s);
    TEST_ASSERT_EQUAL_INT(-1, btseal_open(&s, 1, 1, buf, BTSEAL_TAG - 1, out));
    TEST_ASSERT_FALSE(s.up);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_kid);
    RUN_TEST(test_frames_both_ways);
    RUN_TEST(test_refused);
    RUN_TEST(test_wrong_key_proof);
    RUN_TEST(test_no_session);
    return UNITY_END();
}
