/* BWRY command sequences (epd_bwseq.h). SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "epd_bwfast.h"
#include "epd_bwry.h"
#include "epd_bwseq.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* Flatten ops to a byte trace: CMD c n d..., PON 0x04 -> 'P', WAIT k -> 'W' k, DELAY ms -> 'D' ms, LUT -> 'L'. */
static int trace(const bwq_op_t *o, int n, uint8_t *t)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        switch (o[i].op) {
        case BWQ_CMD:
            t[k++] = o[i].cmd;
            t[k++] = o[i].n;
            memcpy(t + k, o[i].d, o[i].n);
            k += o[i].n;
            break;
        case BWQ_PON: t[k++] = 'P'; break;
        case BWQ_WAIT: t[k++] = 'W'; t[k++] = o[i].cmd; break;
        case BWQ_DELAY: t[k++] = 'D'; t[k++] = o[i].cmd; break;
        case BWQ_LUT: t[k++] = 'L'; break;
        default: t[k++] = '?';
        }
    }
    return k;
}

#define CHECK(ops, n, ...) do { \
    static const uint8_t want_[] = { __VA_ARGS__ }; uint8_t got_[BWQ_MAX * 12]; \
    TEST_ASSERT_TRUE((n) < BWQ_MAX); \
    int k_ = trace((ops), (n), got_); \
    TEST_ASSERT_EQUAL_INT((int)sizeof want_, k_); \
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want_, got_, sizeof want_); } while (0)

/* PicPak stock V1.1.13 (owner's backup, ota_1), panel 0x060401: 0x420207d4..0x420208e4, byte for byte. */
static void test_stock_init_is_v1_1_13(void)
{
    bwq_cfg_t c = {BWRY_INIT_STOCK, BWRY_FAST_OFF, NULL, 0};
    bwq_op_t o[BWQ_MAX];
    int n = bwq_init(&c, o);
    CHECK(o, n, 0x00, 2, 0x07, 0x29, 0x01, 2, 0x07, 0x00, 0x06, 4, 0x0f, 0x8b, 0x9c, 0x96, 0x30, 1, 0x08,
          0x50, 1, 0x37, 0x61, 4, 0x01, 0x90, 0x01, 0x2c, 0x65, 4, 0, 0, 0, 0, 0xe7, 1, 0x96, 0xe9, 1, 0x01,
          0xff, 1, 0xa5);
    TEST_ASSERT_FALSE(bwq_early_pon(&c));
}

/* Stock V1.1.13 0x42020be4..0x42020f6c and tesserae epd_display(): data, PON, wait, DRF 0x00, wait. */
static void test_power_on_after_the_data(void)
{
    bwq_cfg_t c = {BWRY_INIT_STOCK, BWRY_FAST_OFF, NULL, 0};
    bwq_op_t o[BWQ_MAX];
    int n = bwq_stream_head(&c, o);
    CHECK(o, n, 0x83, 9, 0x00, 0x00, 0x01, 0x8f, 0x00, 0x00, 0x01, 0x2b, 0x00, 0x10, 0);
    n = bwq_refresh(&c, false, o);
    CHECK(o, n, 'P', 'W', BWQ_W_PON, 0x50, 1, 0x37, 0x12, 1, 0x00, 'D', 2, 'W', BWQ_W_REFRESH);
    n = bwq_refresh(&c, true, o);
    CHECK(o, n, 'P', 'W', BWQ_W_PON, 0x50, 1, 0x97, 0x12, 1, 0x00, 'D', 2, 'W', BWQ_W_REFRESH);
    c.init = BWRY_INIT_GD;
    n = bwq_stream_head(&c, o);
    CHECK(o, n, 0x10, 0); /* no AREA */
}

static void test_e6p_keeps_power_on_before_the_data(void)
{
    bwq_cfg_t c = {BWRY_INIT_STOCK, BWRY_FAST_E6P, NULL, 0};
    bwq_op_t o[BWQ_MAX];
    TEST_ASSERT_TRUE(bwq_early_pon(&c));
    int n = bwq_init(&c, o);
    uint8_t t[BWQ_MAX * 12];
    int k = trace(o, n, t);
    static const uint8_t tail[] = {'P', 'W', BWQ_W_PON, 0xe0, 1, 0x02, 0xe6, 1, 0x5a, 0xa5, 1, 0x00, 'W', BWQ_W_INIT};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tail, t + k - (int)sizeof tail, sizeof tail);
    n = bwq_refresh(&c, false, o);
    CHECK(o, n, 0x50, 1, 0x37, 0x12, 1, 0x00, 'D', 2, 'W', BWQ_W_REFRESH);
}

static void test_fast_variants_before_power_on(void)
{
    bwq_cfg_t c = {BWRY_INIT_GD, BWRY_FAST_TSE, NULL, 0};
    bwq_op_t o[BWQ_MAX];
    int n = bwq_init(&c, o);
    uint8_t t[BWQ_MAX * 12];
    int k = trace(o, n, t);
    static const uint8_t tse[] = {0x41, 1, 0x07, 0x40, 0, 'W', BWQ_W_INIT};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tse, t + k - (int)sizeof tse, sizeof tse);
    TEST_ASSERT_EQUAL_HEX8(0x00, t[0]); /* GD PSR 0x0f 0x09 */
    TEST_ASSERT_EQUAL_HEX8(0x0f, t[2]);
    c.fast = BWRY_FAST_E6;
    n = bwq_init(&c, o);
    k = trace(o, n, t);
    static const uint8_t e6[] = {0xe0, 1, 0x02, 0xe6, 1, 0x5c, 0xa5, 1, 0x00, 'W', BWQ_W_INIT};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(e6, t + k - (int)sizeof e6, sizeof e6);
    for (int i = 0; i < n; i++)
        TEST_ASSERT_NOT_EQUAL(BWQ_PON, o[i].op);
}

/* Register LUT: tesserae epd_init_vendor_lut order; power levels, VCOM and PLL from the timing bytes. */
static void test_lut_init(void)
{
    static uint8_t L[BWRY_LUT_BYTES];
    for (int i = 0; i < 528; i++)
        L[i] = (uint8_t)i;
    static const uint8_t tr[7] = {0x00, 0x1b, 0x78, 0x18, 0x1b, 0x94, 0x06};
    memcpy(L + 528, tr, 7);
    bwq_cfg_t c = {BWRY_INIT_STOCK, BWRY_FAST_E6P, L, 0}; /* a LUT wins over the OTP variants */
    bwq_op_t o[BWQ_MAX];
    TEST_ASSERT_FALSE(bwq_early_pon(&c));
    int n = bwq_init(&c, o);
    CHECK(o, n, 0x06, 4, 0x0f, 0x8b, 0x9c, 0x96, 0x50, 1, 0x37, 0x00, 2, 0x07, 0xa9, 'W', BWQ_W_INIT,
          0x01, 6, 0x07, 0x00, 0x1b, 0x18, 0x78, 0x1b, 0x82, 1, 0x14, 0x30, 1, 0x06,
          0x61, 4, 0x01, 0x90, 0x01, 0x2c, 'W', BWQ_W_INIT, 0x62, 2, 0x62, 0x51, 0x65, 4, 0, 0, 0, 0, 'W', BWQ_W_INIT,
          0xe7, 1, 0x96, 0xe9, 1, 0x01, 'L');
    TEST_ASSERT_EQUAL_HEX8(0x20, o[n - 1].cmd);
    n = bwq_refresh(&c, false, o);
    TEST_ASSERT_EQUAL_UINT8(BWQ_PON, o[0].op);
    c.init = BWRY_INIT_GD; /* LUTs scan as the stock profile only */
    n = bwq_init(&c, o);
    TEST_ASSERT_EQUAL_HEX8(0x0f, o[0].d[0]);
}

/* PLL experiment: the value replaces the profile's 0x30 in place (nothing else moves), and the LUT's timing byte. */
static void test_pll_override(void)
{
    bwq_cfg_t c = {BWRY_INIT_STOCK, BWRY_FAST_OFF, NULL, 0x0c};
    bwq_op_t o[BWQ_MAX];
    int n = bwq_init(&c, o);
    CHECK(o, n, 0x00, 2, 0x07, 0x29, 0x01, 2, 0x07, 0x00, 0x06, 4, 0x0f, 0x8b, 0x9c, 0x96, 0x30, 1, 0x0c,
          0x50, 1, 0x37, 0x61, 4, 0x01, 0x90, 0x01, 0x2c, 0x65, 4, 0, 0, 0, 0, 0xe7, 1, 0x96, 0xe9, 1, 0x01,
          0xff, 1, 0xa5);
    c.init = BWRY_INIT_GD;
    n = bwq_init(&c, o);
    int pll = 0;
    for (int i = 0; i < n; i++)
        if (o[i].op == BWQ_CMD && o[i].cmd == 0x30) {
            TEST_ASSERT_EQUAL_HEX8(0x0c, o[i].d[0]);
            pll++;
        }
    TEST_ASSERT_EQUAL_INT(1, pll);
    static uint8_t L[BWRY_LUT_BYTES];
    L[534] = 0x06;
    c = (bwq_cfg_t){BWRY_INIT_STOCK, BWRY_FAST_OFF, L, 0x04};
    n = bwq_init(&c, o);
    for (int i = 0; i < n; i++)
        if (o[i].op == BWQ_CMD && o[i].cmd == 0x30)
            TEST_ASSERT_EQUAL_HEX8(0x04, o[i].d[0]);
}

static void test_lut_payload_transposes(void)
{
    static uint8_t L[BWRY_LUT_BYTES], out[BWQ_LUT_OUT];
    for (int i = 0; i < BWRY_LUT_BYTES; i++)
        L[i] = (uint8_t)(i * 7 + 3);
    bwq_lut_payload(L, out);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(L + 528, out, 7);
    for (int r = 0; r < 48; r++)
        for (int col = 0; col < 11; col++)
            TEST_ASSERT_EQUAL_HEX8(L[r + 48 * col], out[7 + r * 11 + col]);
    TEST_ASSERT_EQUAL_INT(BWRY_LUT_BYTES, BWQ_LUT_OUT);
}

/* PicPak 2049: "!tp pll" from the refresh wait's hook closed the driver; the refresh's POF then hit a freed device */
/* The fast black/white partial (epd_bwfast.h, issue #190) changes only the 0x20 payload: the init goes out with the
 * same commands as with the full LUT, and the payload starts with the full LUT's analog bytes. */
static void test_fast_partial_lut_changes_only_the_payload(void)
{
    static uint8_t full[BWRY_LUT_BYTES], fast[BWRY_LUT_BYTES];
    memset(full, 0xef, 528);
    static const uint8_t analog[7] = {0x00, 0x1b, 0x78, 0x18, 0x1b, 0x14, 0x06};
    memcpy(full + 528, analog, 7);
    bwf_cfg_t fc = {BWF_PULSES_DEFAULT, BWF_RECORDS_DEFAULT, BWF_SKEL_PLAIN};
    TEST_ASSERT_EQUAL_INT(BWF_OK, bwf_build(&fc, full, fast));
    bwq_cfg_t a = {BWRY_INIT_STOCK, BWRY_FAST_OFF, full, 0}, b = {BWRY_INIT_STOCK, BWRY_FAST_OFF, fast, 0};
    bwq_op_t oa[BWQ_MAX], ob[BWQ_MAX];
    uint8_t ta[BWQ_MAX * 12], tb[BWQ_MAX * 12];
    int na = bwq_init(&a, oa), nb = bwq_init(&b, ob);
    TEST_ASSERT_EQUAL_INT(na, nb);
    int ka = trace(oa, na, ta), kb = trace(ob, nb, tb);
    TEST_ASSERT_EQUAL_INT(ka, kb);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ta, tb, (size_t)ka);
    na = bwq_refresh(&a, true, oa);
    nb = bwq_refresh(&b, true, ob);
    ka = trace(oa, na, ta);
    kb = trace(ob, nb, tb);
    TEST_ASSERT_EQUAL_INT(ka, kb);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ta, tb, (size_t)ka);
    uint8_t out[BWQ_LUT_OUT];
    bwq_lut_payload(fast, out);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(analog, out, 7);
    TEST_ASSERT_EQUAL_HEX8(BWF_CARRIER, out[7]);                 /* VCOM record 0 byte 0 */
    TEST_ASSERT_EQUAL_HEX8(0xaa, out[7 + 8 * 11 + 1]);           /* black table record 0 byte 1 */
    TEST_ASSERT_EQUAL_HEX8(0x55, out[7 + 16 * 11 + 1]);          /* white table record 0 byte 1 */
    TEST_ASSERT_EQUAL_HEX8(0xff, out[7 + 24 * 11 + 1]);          /* yellow holds */
}

static void test_gate_refuses_reentry_from_a_wait(void)
{
    bwq_gate_t g = {true, true};
    for (int e = BWQ_E_OPEN; e <= BWQ_E_SPI; e++)
        TEST_ASSERT_FALSE(bwq_allowed(&g, (bwq_entry_t)e));
    TEST_ASSERT_FALSE(bwq_fail_recover(&g));
    g.waiting = false;
    TEST_ASSERT_FALSE(bwq_allowed(&g, BWQ_E_OPEN));
    TEST_ASSERT_TRUE(bwq_allowed(&g, BWQ_E_CLOSE));
    TEST_ASSERT_TRUE(bwq_allowed(&g, BWQ_E_SPI));
    TEST_ASSERT_TRUE(bwq_fail_recover(&g));
}

static void test_gate_closed_gets_no_spi(void)
{
    bwq_gate_t g = {false, false};
    TEST_ASSERT_TRUE(bwq_allowed(&g, BWQ_E_OPEN));
    TEST_ASSERT_FALSE(bwq_allowed(&g, BWQ_E_SPI));
    TEST_ASSERT_FALSE(bwq_allowed(&g, BWQ_E_END));
    TEST_ASSERT_FALSE(bwq_fail_recover(&g)); /* a BUSY timeout after a close: no POF, no reset */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_stock_init_is_v1_1_13);
    RUN_TEST(test_power_on_after_the_data);
    RUN_TEST(test_e6p_keeps_power_on_before_the_data);
    RUN_TEST(test_fast_variants_before_power_on);
    RUN_TEST(test_lut_init);
    RUN_TEST(test_pll_override);
    RUN_TEST(test_lut_payload_transposes);
    RUN_TEST(test_fast_partial_lut_changes_only_the_payload);
    RUN_TEST(test_gate_refuses_reentry_from_a_wait);
    RUN_TEST(test_gate_closed_gets_no_spi);
    return UNITY_END();
}
