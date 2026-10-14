/* usbdisp: what hello says a board can do (docs/spec/usb-display.md hello): `depth_modes` agrees with what show
 * accepts at each depth, and `full_every` has a default of 30 that `set { full_every: null }` brings back.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "usbdisp_fake.h"

#define W 48
#define H 32

static const char *const NAMES[] = {"full", "partial", "partial_all", "inverse", "passes"};
#define NNAMES (sizeof NAMES / sizeof NAMES[0])

/* the modes hello lists for `depth` as a bit set over NAMES; -1 when the depth has no entry */
static int listed(const uint8_t *hello, size_t hn, unsigned depth)
{
    const uint8_t *v;
    size_t vn;
    if (!cbor_map_get(hello, hn, "depth_modes", 11, &v, &vn))
        return -1;
    cbor_r_t r;
    cbor_item_t it;
    cbor_r_init(&r, v, vn);
    if (!cbor_read(&r, &it) || it.type != CBOR_MAP)
        return -1;
    int found = -1;
    for (uint64_t i = 0; i < it.u; i++) {
        cbor_item_t k, a;
        if (!cbor_read(&r, &k) || k.type != CBOR_TSTR || !cbor_read(&r, &a) || a.type != CBOR_ARRAY)
            return -1;
        int bits = 0;
        for (uint64_t j = 0; j < a.u; j++) {
            cbor_item_t s;
            if (!cbor_read(&r, &s))
                return -1;
            for (size_t m = 0; m < NNAMES; m++)
                if (cbor_str_eq(&s, NAMES[m]))
                    bits |= 1 << m;
        }
        if (cbor_str_eq(&k, depth == 1 ? "1" : depth == 2 ? "2" : "4"))
            found = bits;
    }
    return found;
}

/* Every board shape: each mode listed for a depth is accepted at that depth, every other one is refused. */
static void test_depth_modes_are_what_show_accepts(void)
{
    static const struct { uint8_t depths, partial, all; } SHAPES[] = {
        {7, 7, 1}, {7, 1, 0}, {7, 1, 1}, {3, 3, 1}, {1, 0, 0}, {1, 1, 1}, {7, 6, 1},
    };
    static uint8_t px[W * H];
    for (size_t s = 0; s < sizeof SHAPES / sizeof SHAPES[0]; s++) {
        device_partial(W, H, SHAPES[s].depths, SHAPES[s].partial, SHAPES[s].all, 0);
        static uint8_t hello[LINK_MAX_PAYLOAD]; /* request() reuses its buffer: keep a copy */
        size_t hn;
        const uint8_t *ans = request(USBDISP_CMD_HELLO, NULL, 0, &hn);
        TEST_ASSERT_TRUE(hn <= sizeof hello);
        memcpy(hello, ans, hn);
        for (unsigned depth = 1; depth <= 4; depth <<= 1) {
            if (!(SHAPES[s].depths & depth)) {
                TEST_ASSERT_EQUAL_INT(-1, listed(hello, hn, depth));
                continue;
            }
            int bits = listed(hello, hn, depth);
            TEST_ASSERT_TRUE_MESSAGE(bits >= 0, "a depth of the board has no depth_modes entry");
            TEST_ASSERT_TRUE_MESSAGE(bits & 1, "full is listed at every depth");
            for (size_t m = 0; m < NNAMES; m++) {
                char msg[96];
                snprintf(msg, sizeof msg, "shape %u, %u bit, %s", (unsigned)s, depth, NAMES[m]);
                for (size_t i = 0; i < sizeof px; i++)
                    px[i] = (uint8_t)(rnd() % (1u << (depth == 4 ? 3 : depth == 2 ? 2 : 1)));
                size_t n = make_png(s_png, W, H, depth == 4 ? 4 : (uint8_t)depth, 0, px);
                int err = show_at(s_png, n, 512, -1, 0, NAMES[m], m == 4 ? 2 : 0);
                if (bits >> m & 1)
                    TEST_ASSERT_EQUAL_INT_MESSAGE(0, err, msg);
                else
                    TEST_ASSERT_EQUAL_INT_MESSAGE(USBDISP_E_MODE, err, msg);
            }
        }
    }
}

static int set_every(const uint8_t *v, size_t vn)
{
    uint8_t p[32];
    cbor_w_t w;
    cbor_w_init(&w, p, sizeof p);
    cbor_put_map(&w, 1);
    cbor_put_str(&w, "full_every");
    cbor_put_raw(&w, v, vn);
    return req_err(USBDISP_CMD_SET, p, w.len);
}

static int set_every_n(int n)
{
    uint8_t b[2] = {0x18, (uint8_t)n}; /* uint, one byte follows (for 24 and up) */
    if (n < 24)
        return b[0] = (uint8_t)n, set_every(b, 1);
    return set_every(b, 2);
}

static int set_flag(void)
{
    const uint8_t *v;
    size_t vn;
    TEST_ASSERT_TRUE(cbor_map_get(s_ans, s_ans_len, "full_every_set", 14, &v, &vn));
    TEST_ASSERT_EQUAL_UINT(1, vn);
    return v[0] == 0xF5 ? 1 : v[0] == 0xF4 ? 0 : -1;
}

static void restart(void)
{
    usbdisp_reset(&D);
    usbdisp_init(&D, &C);
}

/* The default is 30, hello says it and that it is the default; set stores, null clears (also over a restart). */
static void test_full_every_default_and_clear(void)
{
    TEST_ASSERT_EQUAL_INT(30, USBDISP_FULL_EVERY);
    device_partial(W, H, 1, 1, true, USBDISP_FULL_EVERY);
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HELLO, NULL, 0));
    TEST_ASSERT_EQUAL_INT(30, map_int(s_ans, s_ans_len, "full_every", -1));
    TEST_ASSERT_EQUAL_INT(0, set_flag());
    TEST_ASSERT_EQUAL_INT(0, set_every_n(7));
    TEST_ASSERT_EQUAL_INT(7, map_int(s_ans, s_ans_len, "full_every", -1));
    TEST_ASSERT_EQUAL_INT(1, set_flag());
    restart();
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HELLO, NULL, 0));
    TEST_ASSERT_EQUAL_INT(7, map_int(s_ans, s_ans_len, "full_every", -1));
    TEST_ASSERT_EQUAL_INT(1, set_flag());
    const uint8_t null = 0xF6;
    TEST_ASSERT_EQUAL_INT(0, set_every(&null, 1));
    TEST_ASSERT_EQUAL_INT(30, map_int(s_ans, s_ans_len, "full_every", -1));
    TEST_ASSERT_EQUAL_INT(0, set_flag());
    TEST_ASSERT_EQUAL_INT(-1, F.kv);
    restart();
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HELLO, NULL, 0));
    TEST_ASSERT_EQUAL_INT(30, map_int(s_ans, s_ans_len, "full_every", -1));
    TEST_ASSERT_EQUAL_INT(0, set_flag());
    /* 0 is never, and is a stored value */
    TEST_ASSERT_EQUAL_INT(0, set_every_n(0));
    TEST_ASSERT_EQUAL_INT(0, map_int(s_ans, s_ans_len, "full_every", -1));
    TEST_ASSERT_EQUAL_INT(1, set_flag());
    /* a store that cannot clear: said so, the value stays */
    F.fail_kv = 1;
    TEST_ASSERT_EQUAL_INT(USBDISP_E_IO, set_every(&null, 1));
    F.fail_kv = 0;
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_SET, NULL, 0));
    TEST_ASSERT_EQUAL_INT(1, set_flag());
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_depth_modes_are_what_show_accepts);
    RUN_TEST(test_full_every_default_and_clear);
    return UNITY_END();
}
