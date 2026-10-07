/* BWRY partial window math and policy (epd_bwpart.h). SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "epd_bwpart.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define W 400
#define H 300

static void test_map_top_down(void)
{
    bwp_win_t w;
    TEST_ASSERT_EQUAL(BWP_OK, bwp_map(16, 40, 64, 20, W, H, false, 8, &w));
    TEST_ASSERT_EQUAL(16, w.x0);
    TEST_ASSERT_EQUAL(79, w.x1);
    TEST_ASSERT_EQUAL(40, w.y0);
    TEST_ASSERT_EQUAL(59, w.y1);
}

/* stock profile: image row y is controller row H-1-y, so the window's last image row comes first */
static void test_map_bottom_up(void)
{
    bwp_win_t w;
    TEST_ASSERT_EQUAL(BWP_OK, bwp_map(16, 40, 64, 20, W, H, true, 8, &w));
    TEST_ASSERT_EQUAL(16, w.x0);
    TEST_ASSERT_EQUAL(79, w.x1);
    TEST_ASSERT_EQUAL(H - 60, w.y0);
    TEST_ASSERT_EQUAL(H - 41, w.y1);
    /* the full panel maps onto itself either way */
    TEST_ASSERT_EQUAL(BWP_OK, bwp_map(0, 0, W, H, W, H, true, 8, &w));
    TEST_ASSERT_EQUAL(0, w.y0);
    TEST_ASSERT_EQUAL(H - 1, w.y1);
    TEST_ASSERT_EQUAL(W - 1, w.x1);
    /* a single top row is the controller's last one */
    TEST_ASSERT_EQUAL(BWP_OK, bwp_map(0, 0, 8, 1, W, H, true, 8, &w));
    TEST_ASSERT_EQUAL(H - 1, w.y0);
    TEST_ASSERT_EQUAL(H - 1, w.y1);
}

static void test_map_rejects_unaligned_and_outside(void)
{
    bwp_win_t w;
    TEST_ASSERT_EQUAL(BWP_E_ARG, bwp_map(4, 0, 64, 10, W, H, false, 8, &w));   /* x not on 8 */
    TEST_ASSERT_EQUAL(BWP_E_ARG, bwp_map(0, 0, 60, 10, W, H, false, 8, &w));   /* w not on 8 */
    TEST_ASSERT_EQUAL(BWP_E_ARG, bwp_map(392, 0, 16, 10, W, H, false, 8, &w)); /* past the right edge */
    TEST_ASSERT_EQUAL(BWP_E_ARG, bwp_map(0, 295, 8, 10, W, H, true, 8, &w));   /* past the bottom */
    TEST_ASSERT_EQUAL(BWP_E_ARG, bwp_map(0, 0, 0, 10, W, H, false, 8, &w));
    TEST_ASSERT_EQUAL(BWP_E_ARG, bwp_map(0, 0, 8, 0, W, H, false, 8, &w));
    TEST_ASSERT_EQUAL(BWP_E_ARG, bwp_map(-8, 0, 8, 1, W, H, false, 8, &w));
    TEST_ASSERT_EQUAL(BWP_OK, bwp_map(4, 0, 12, 10, W, H, false, 4, &w));      /* 4-px alignment when asked */
}

static void test_command_bytes(void)
{
    bwp_win_t w = {264, 399, 10, 299};
    uint8_t b[9];
    bwp_ptlw_bytes(&w, 0x00, b);
    const uint8_t want[9] = {0x01, 0x08, 0x01, 0x8f, 0x00, 0x0a, 0x01, 0x2b, 0x00};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, b, 9);
    /* the stock full-panel AREA command (epd_bwry.c) is the same layout */
    bwp_win_t f = {0, W - 1, 0, H - 1};
    bwp_ptlw_bytes(&f, 0x00, b);
    const uint8_t full[9] = {0x00, 0x00, 0x01, 0x8f, 0x00, 0x00, 0x01, 0x2b, 0x00};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(full, b, 9);
    bwp_ptl_bytes(&w, b);
    TEST_ASSERT_EQUAL_HEX8(0x01, b[8]);
}

/* the app's bands: one window, same x range */
static void test_acc_one_window(void)
{
    bwp_acc_t a;
    bwp_acc_reset(&a);
    TEST_ASSERT_FALSE(bwp_acc_solid(&a)); /* nothing written: no refresh */
    TEST_ASSERT_EQUAL(BWP_OK, bwp_acc_add(&a, 0, 37, W, 13));
    TEST_ASSERT_EQUAL(BWP_OK, bwp_acc_add(&a, 0, 50, W, 50));
    TEST_ASSERT_EQUAL(BWP_OK, bwp_acc_add(&a, 0, 100, W, 50));
    TEST_ASSERT_TRUE(bwp_acc_solid(&a));
    TEST_ASSERT_EQUAL(0, a.x);
    TEST_ASSERT_EQUAL(37, a.y0);
    TEST_ASSERT_EQUAL(150, a.y1);
    TEST_ASSERT_EQUAL(W, a.w);
}

/* Regression (sim picpak-partial, first run): the band-hash pass writes changed band 0, changed band 2, then the
 * unchanged band 1 between them. Each band is its own RAM window, so that order is fine. */
static void test_acc_any_order(void)
{
    bwp_acc_t a;
    bwp_acc_reset(&a);
    TEST_ASSERT_EQUAL(BWP_OK, bwp_acc_add(&a, 0, 120, W, 30));
    TEST_ASSERT_EQUAL(BWP_OK, bwp_acc_add(&a, 0, 200, W, 50));
    TEST_ASSERT_FALSE(bwp_acc_solid(&a)); /* rows 150..199 not written yet */
    TEST_ASSERT_EQUAL(BWP_OK, bwp_acc_add(&a, 0, 150, W, 50));
    TEST_ASSERT_TRUE(bwp_acc_solid(&a));
    TEST_ASSERT_EQUAL(120, a.y0);
    TEST_ASSERT_EQUAL(250, a.y1);
}

static void test_acc_rejects_gaps_overlap_and_x(void)
{
    bwp_acc_t a;
    bwp_acc_reset(&a);
    TEST_ASSERT_EQUAL(BWP_OK, bwp_acc_add(&a, 80, 50, 64, 50));
    TEST_ASSERT_EQUAL(BWP_OK, bwp_acc_add(&a, 80, 110, 64, 10));
    TEST_ASSERT_FALSE(bwp_acc_solid(&a));                             /* gap: rows 100..109 unwritten */
    TEST_ASSERT_EQUAL(BWP_E_ORDER, bwp_acc_add(&a, 80, 95, 64, 10));  /* rows 95..99 twice */
    TEST_ASSERT_EQUAL(BWP_E_ORDER, bwp_acc_add(&a, 72, 100, 72, 10)); /* other x range */
    TEST_ASSERT_EQUAL(BWP_E_ARG, bwp_acc_add(&a, 80, 100, 64, 0));
    TEST_ASSERT_EQUAL(BWP_E_ARG, bwp_acc_add(&a, 80, BWP_MAX_ROWS - 4, 64, 8));
    TEST_ASSERT_EQUAL(BWP_OK, bwp_acc_add(&a, 80, 100, 64, 10));
    TEST_ASSERT_TRUE(bwp_acc_solid(&a));
    TEST_ASSERT_EQUAL(70, a.rows);
}

static void test_policy_off_and_cold(void)
{
    bwp_cfg_t off = {0, 10}, on = {1, 10};
    bwp_state_t s = {0, false};
    TEST_ASSERT_FALSE(bwp_allowed(&off, &s));
    TEST_ASSERT_TRUE(bwp_allowed(&on, &s));
    s.need_full = true; /* cold boot: panel unknown */
    TEST_ASSERT_FALSE(bwp_allowed(&on, &s));
    bwp_note(&s, true, true);
    TEST_ASSERT_TRUE(bwp_allowed(&on, &s));
}

/* periodic full refresh against ghosting: `every` partials, then partial is withheld until a full one */
static void test_policy_every(void)
{
    bwp_cfg_t c = {1, 3};
    bwp_state_t s = {0, true};
    bwp_note(&s, true, true);
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_TRUE(bwp_allowed(&c, &s));
        bwp_note(&s, false, true);
    }
    TEST_ASSERT_FALSE(bwp_allowed(&c, &s));
    bwp_note(&s, true, true);
    TEST_ASSERT_TRUE(bwp_allowed(&c, &s));
    TEST_ASSERT_EQUAL(0, s.since_full);
    c.every = 0; /* never forced by the HAL (the app's full_every still applies) */
    for (int i = 0; i < 300; i++)
        bwp_note(&s, false, true);
    TEST_ASSERT_TRUE(bwp_allowed(&c, &s));
}

/* a failed refresh (BUSY timeout, bad window): full until a full one succeeds */
static void test_policy_failure_falls_back(void)
{
    bwp_cfg_t c = {1, 0};
    bwp_state_t s = {0, false};
    bwp_note(&s, false, false);
    TEST_ASSERT_FALSE(bwp_allowed(&c, &s));
    bwp_note(&s, false, true); /* a partial that somehow went through does not clear it */
    TEST_ASSERT_FALSE(bwp_allowed(&c, &s));
    bwp_note(&s, true, false); /* the full failed too */
    TEST_ASSERT_FALSE(bwp_allowed(&c, &s));
    bwp_note(&s, true, true);
    TEST_ASSERT_TRUE(bwp_allowed(&c, &s));
}

/* ------------------------------------------------------------------ box partial (the press overlay) */

static void test_boxes_one_box_and_touching(void)
{
    bwp_rect_t win, one[1] = {{8, 104, 216, 88}};
    TEST_ASSERT_EQUAL(BWP_OK, bwp_boxes_window(one, 1, W, H, &win));
    TEST_ASSERT_EQUAL(8, win.x);
    TEST_ASSERT_EQUAL(104, win.y);
    TEST_ASSERT_EQUAL(216, win.w);
    TEST_ASSERT_EQUAL(88, win.h);
    /* side by side, touching: the union is covered (the sweep takes them in any order) */
    bwp_rect_t two[2] = {{176, 252, 224, 40}, {8, 252, 168, 40}};
    TEST_ASSERT_EQUAL(BWP_OK, bwp_boxes_window(two, 2, W, H, &win));
    TEST_ASSERT_EQUAL(8, win.x);
    TEST_ASSERT_EQUAL(392, win.w);
    /* stacked, touching, same x range */
    bwp_rect_t st[2] = {{80, 40, 64, 20}, {80, 60, 64, 30}};
    TEST_ASSERT_EQUAL(BWP_OK, bwp_boxes_window(st, 2, W, H, &win));
    TEST_ASSERT_EQUAL(50, win.h);
}

/* the frame shows between boxes: the union needs it */
static void test_boxes_gaps(void)
{
    bwp_rect_t win;
    bwp_rect_t gap_x[2] = {{8, 252, 160, 40}, {176, 252, 224, 40}};
    TEST_ASSERT_EQUAL(BWP_E_GAP, bwp_boxes_window(gap_x, 2, W, H, &win));
    TEST_ASSERT_EQUAL(8, win.x); /* the union all the same */
    TEST_ASSERT_EQUAL(392, win.w);
    bwp_rect_t gap_y[2] = {{80, 40, 64, 20}, {80, 68, 64, 30}};
    TEST_ASSERT_EQUAL(BWP_E_GAP, bwp_boxes_window(gap_y, 2, W, H, &win));
    bwp_rect_t other_x[2] = {{80, 40, 64, 20}, {88, 60, 56, 30}};
    TEST_ASSERT_EQUAL(BWP_E_GAP, bwp_boxes_window(other_x, 2, W, H, &win));
}

static void test_boxes_bad(void)
{
    bwp_rect_t win, b[BWP_MAX_BOXES + 1] = {{0}};
    TEST_ASSERT_EQUAL(BWP_E_ARG, bwp_boxes_window(b, 0, W, H, &win));
    TEST_ASSERT_EQUAL(BWP_E_ARG, bwp_boxes_window(b, BWP_MAX_BOXES + 1, W, H, &win));
    bwp_rect_t odd[1] = {{4, 0, 64, 8}}, wide[1] = {{0, 0, 64 + 4, 8}}, out[1] = {{392, 0, 16, 8}};
    TEST_ASSERT_EQUAL(BWP_E_ARG, bwp_boxes_window(odd, 1, W, H, &win));
    TEST_ASSERT_EQUAL(BWP_E_ARG, bwp_boxes_window(wide, 1, W, H, &win));
    TEST_ASSERT_EQUAL(BWP_E_ARG, bwp_boxes_window(out, 1, W, H, &win));
}

/* policy without "!tp part": cold boot and failures full, `every` counts every partial */
static void test_boxes_policy(void)
{
    bwp_state_t s = {0, true};
    TEST_ASSERT_FALSE(bwp_boxes_allowed(5, &s));
    bwp_note(&s, true, true);
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_TRUE(bwp_boxes_allowed(5, &s));
        bwp_note(&s, false, true);
    }
    TEST_ASSERT_FALSE(bwp_boxes_allowed(5, &s));
    TEST_ASSERT_TRUE(bwp_boxes_allowed(0, &s));
    bwp_note(&s, false, false);
    TEST_ASSERT_FALSE(bwp_boxes_allowed(0, &s));
}

/* The press mark on the PicPak (one label, bottom right, `in_mark_place` rect on the native panel): one box drives
 * from the box alone at every rotation. */
static void test_boxes_mark(void)
{
    const int PW = 400, PH = 300;
    for (int rot = 0; rot < 4; rot++) {
        int lw = rot & 1 ? PH : PW, lh = rot & 1 ? PW : PH;
        int x = (lw - 8 - 104) & ~7, y = lh - 8 - 26, w = 104, h = 26;
        bwp_rect_t r = rot == 0   ? (bwp_rect_t){x, y, w, h}
                       : rot == 1 ? (bwp_rect_t){PW - y - h, x, h, w}
                       : rot == 2 ? (bwp_rect_t){PW - x - w, PH - y - h, w, h}
                                  : (bwp_rect_t){y, PH - x - w, h, w};
        r.x &= ~7; /* whole bytes on the native x axis */
        r.w = (r.w + 7) & ~7;
        bwp_rect_t win;
        TEST_ASSERT_EQUAL(BWP_OK, bwp_boxes_window(&r, 1, PW, PH, &win));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_map_top_down);
    RUN_TEST(test_map_bottom_up);
    RUN_TEST(test_map_rejects_unaligned_and_outside);
    RUN_TEST(test_command_bytes);
    RUN_TEST(test_acc_one_window);
    RUN_TEST(test_acc_any_order);
    RUN_TEST(test_acc_rejects_gaps_overlap_and_x);
    RUN_TEST(test_policy_off_and_cold);
    RUN_TEST(test_policy_every);
    RUN_TEST(test_policy_failure_falls_back);
    RUN_TEST(test_boxes_one_box_and_touching);
    RUN_TEST(test_boxes_gaps);
    RUN_TEST(test_boxes_bad);
    RUN_TEST(test_boxes_policy);
    RUN_TEST(test_boxes_mark);
    return UNITY_END();
}
