/* usbdisp: area updates and refresh modes (docs/spec/usb-display.md "Area and refresh mode") against the fake panel,
 * whose glass changes only where the driver is told to drive: the frame the device keeps equals the glass after every
 * mode, a partial without that frame becomes a full one and says so, a refused, cut or failed update is followed by a
 * full one, every allocation fails once, the gate clock count is reported. SPDX-License-Identifier: GPL-3.0-or-later */
#include "usbdisp_fake.h"

#define W 48
#define H 32
static uint8_t s_want[MAXPIX]; /* what the glass must show, one byte a pixel */
static uint8_t s_box[MAXPIX];

static void random_px(uint8_t *px, size_t n, uint8_t bits)
{
    for (size_t i = 0; i < n; i++)
        px[i] = (uint8_t)(rnd() % (1u << bits));
}

/* The frame the device keeps, against s_want: it must be what the glass shows. */
static void expect_shown(uint8_t bits, const char *what)
{
    uint8_t depth = 0;
    const uint8_t *prev = usbdisp_prev(&D, &depth);
    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(s_want, F.shown, W * H, what);
    TEST_ASSERT_NOT_NULL_MESSAGE(prev, what);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(bits, depth, what);
    size_t stride = (W * (size_t)bits + 7) / 8;
    for (uint16_t y = 0; y < H; y++)
        for (uint16_t x = 0; x < W; x++) {
            unsigned bit = (unsigned)x * bits;
            uint8_t v = (uint8_t)(prev[y * stride + bit / 8] >> (8 - bits - bit % 8) & ((1 << bits) - 1));
            if (v != s_want[(size_t)y * W + x]) {
                char msg[96];
                snprintf(msg, sizeof msg, "%s: the frame kept has %u at %u,%u, the glass %u", what, v, x, y,
                         s_want[(size_t)y * W + x]);
                TEST_FAIL_MESSAGE(msg);
            }
        }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, F.stale, what);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, F.order_errors, what);
}

/* A whole picture: s_want itself. */
static int show_want(const char *mode, int passes)
{
    size_t n = make_png(s_png, W, H, 1, 0, s_want);
    return show_at(s_png, n, 512, -1, 0, mode, passes);
}

/* A rectangle of random pixels at x, y; s_want follows when `into`. */
static int show_box(int x, int y, uint16_t w, uint16_t h, const char *mode, int passes, bool into)
{
    random_px(s_box, (size_t)w * h, 1);
    for (uint16_t j = 0; into && j < h; j++)
        memcpy(s_want + (size_t)(y + j) * W + x, s_box + (size_t)j * w, w);
    size_t n = make_png(s_png, w, h, 1, 0, s_box);
    return show_at(s_png, n, 512, x, y, mode, passes);
}

/* Random updates in every mode: after each one the glass shows the picture and the device knows it. */
static void test_frame_kept_is_the_glass(void)
{
    static const char *const MODES[] = {NULL, "partial", "partial_all", "inverse", "passes", "full"};
    static const char *const NAMES[USBDISP_MODE_COUNT] = {"auto", "full", "partial", "partial_all", "inverse", "passes", "none"};
    int ran[USBDISP_MODE_COUNT] = {0}, fulls_no_prev = 0;
    device_partial(W, H, 3, 1, true, 0);
    bool known = false; /* the device has a frame of what the panel shows */
    for (int i = 0; i < 400; i++) {
        unsigned op = rnd() % 16, m = rnd() % 6;
        int passes = m == 4 ? 1 + (int)(rnd() % USBDISP_PASSES_MAX) : 0, refreshes = F.refreshes;
        char what[64];
        snprintf(what, sizeof what, "step %d op %u mode %s", i, op, MODES[m] ? MODES[m] : "none");
        if (op == 0) { /* a refused transfer: the device no longer trusts its frame */
            TEST_ASSERT_EQUAL_INT_MESSAGE(USBDISP_E_MODE, show_want("fast", 0), what);
            TEST_ASSERT_NULL(usbdisp_prev(&D, NULL));
            known = false;
            continue;
        }
        if (op == 1) {
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, req_err(USBDISP_CMD_CLEAR, NULL, 0), what);
            memset(s_want, 1, sizeof s_want);
            expect_shown(1, what);
            known = true;
            continue;
        }
        bool area = op >= 6;
        int e;
        if (area) {
            int x = 8 * (int)(rnd() % (W / 8)), y = (int)(rnd() % H);
            uint16_t w = (uint16_t)(8 * (1 + rnd() % ((W - (unsigned)x) / 8))), h = (uint16_t)(1 + rnd() % (H - (unsigned)y));
            if (!known)
                memset(s_want, 1, sizeof s_want); /* the rest of the panel: white */
            e = show_box(x, y, w, h, MODES[m], passes, true);
        } else {
            if (op == 2) {
                /* the same picture again */
            } else if (op < 5) { /* a few pixels, else a new picture */
                for (int k = 0; k < 5; k++)
                    s_want[rnd() % (W * H)] ^= 1;
            } else
                random_px(s_want, W * H, 1);
            e = show_want(MODES[m], passes);
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, e, what);
        expect_shown(1, what);
        const char *mode = ans_str("mode"), *why = ans_str("why");
        if (!known) { /* without the frame: a full refresh, whatever was asked, and the answer says why */
            TEST_ASSERT_EQUAL_STRING_MESSAGE("full", mode, what);
            TEST_ASSERT_TRUE_MESSAGE(!strcmp(why, m == 5 ? "asked" : "first") || !strcmp(why, m == 5 ? "asked" : "refused"), what);
            fulls_no_prev++;
        } else if (m == 0) {
            TEST_ASSERT_TRUE_MESSAGE(!strcmp(mode, "partial") || !strcmp(mode, "none"), what);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(mode[0] == 'p' ? "changed" : "unchanged", why, what);
        } else if (m == 1) {
            TEST_ASSERT_TRUE_MESSAGE(!strcmp(mode, "partial") || !strcmp(mode, "none"), what);
        } else {
            TEST_ASSERT_EQUAL_STRING_MESSAGE(MODES[m], mode, what);
            TEST_ASSERT_EQUAL_STRING_MESSAGE("asked", why, what);
        }
        int want_refreshes = !strcmp(mode, "none") ? 0 : !strcmp(mode, "inverse") ? 2 : !strcmp(mode, "passes") ? passes : 1;
        TEST_ASSERT_EQUAL_INT_MESSAGE(refreshes + want_refreshes, F.refreshes, what);
        TEST_ASSERT_EQUAL_INT_MESSAGE(passes && known, map_int(s_ans, s_ans_len, "passes", 0) != 0, what);
        for (int k = 0; k < USBDISP_MODE_COUNT; k++)
            ran[k] += !strcmp(mode, NAMES[k]);
        known = true;
    }
    for (int k = USBDISP_MODE_FULL; k < USBDISP_MODE_COUNT; k++)
        TEST_ASSERT_TRUE_MESSAGE(ran[k] >= 5, NAMES[k]);
    TEST_ASSERT_TRUE(fulls_no_prev >= 10);
    TEST_ASSERT_EQUAL_INT(F.begins, F.sleeps);
    usbdisp_reset(&D);
    TEST_ASSERT_EQUAL_INT(0, F.live);
}

/* What each mode asks of the panel: partial drives the changed pixels only, partial_all every pixel of the rectangle,
 * inverse shows the negative first, passes repeats the drive. */
static void test_modes_drive_as_named(void)
{
    static uint8_t before[MAXPIX];
    device_partial(W, H, 1, 1, true, 0);
    random_px(s_want, W * H, 1);
    TEST_ASSERT_EQUAL_INT(0, show_want(NULL, 0));
    TEST_ASSERT_EQUAL_STRING("full", ans_str("mode"));
    TEST_ASSERT_EQUAL_STRING("first", ans_str("why"));
    TEST_ASSERT_EQUAL_INT(1, F.fulls);

    /* one pixel changed: the device finds the rectangle, one pixel is driven */
    s_want[10 * W + 20] ^= 1;
    TEST_ASSERT_EQUAL_INT(0, show_want(NULL, 0));
    TEST_ASSERT_EQUAL_STRING("partial", ans_str("mode"));
    TEST_ASSERT_EQUAL_INT(16, map_int(s_ans, s_ans_len, "x", -1));
    TEST_ASSERT_EQUAL_INT(10, map_int(s_ans, s_ans_len, "y", -1));
    TEST_ASSERT_EQUAL_INT(8, map_int(s_ans, s_ans_len, "w", -1));
    TEST_ASSERT_EQUAL_INT(1, map_int(s_ans, s_ans_len, "h", -1));
    TEST_ASSERT_EQUAL_INT(USBDISP_DRIVE_PARTIAL, F.drive);
    TEST_ASSERT_EQUAL_INT(1, F.driven);
    TEST_ASSERT_EQUAL_INT(1, F.fulls);
    expect_shown(1, "one pixel");

    /* the same picture: nothing is driven at all */
    int begins = F.begins;
    TEST_ASSERT_EQUAL_INT(0, show_want(NULL, 0));
    TEST_ASSERT_EQUAL_STRING("none", ans_str("mode"));
    TEST_ASSERT_EQUAL_STRING("unchanged", ans_str("why"));
    TEST_ASSERT_EQUAL_INT(begins, F.begins);

    /* partial_all: every pixel of the rectangle, changed or not */
    TEST_ASSERT_EQUAL_INT(0, show_box(8, 4, 16, 6, "partial_all", 0, true));
    TEST_ASSERT_EQUAL_INT(USBDISP_DRIVE_PARTIAL_ALL, F.drive);
    TEST_ASSERT_EQUAL_INT(16 * 6, F.driven);
    TEST_ASSERT_EQUAL_INT(8, F.rect.x);
    TEST_ASSERT_EQUAL_INT(4, F.rect.y);
    expect_shown(1, "partial_all");

    /* inverse: after the first refresh the rectangle shows the negative of the new picture, the rest is untouched */
    memcpy(before, F.shown, sizeof before);
    TEST_ASSERT_EQUAL_INT(0, show_box(24, 8, 16, 10, "inverse", 0, true));
    for (uint16_t y = 0; y < H; y++)
        for (uint16_t x = 0; x < W; x++) {
            bool in = x >= 24 && x < 40 && y >= 8 && y < 18;
            uint8_t mid = F.hist[1][(size_t)y * W + x];
            TEST_ASSERT_EQUAL_UINT8(in ? 1 - s_want[(size_t)y * W + x] : before[(size_t)y * W + x], mid);
        }
    TEST_ASSERT_EQUAL_INT(16 * 10, F.driven); /* the second refresh changes every pixel of it */
    expect_shown(1, "inverse");

    /* passes: the drive of every pixel, n times */
    int partials = F.partials;
    TEST_ASSERT_EQUAL_INT(0, show_box(0, 20, 8, 8, "passes", 3, true));
    TEST_ASSERT_EQUAL_INT(partials + 3, F.partials);
    TEST_ASSERT_EQUAL_INT(3, map_int(s_ans, s_ans_len, "passes", 0));
    TEST_ASSERT_EQUAL_INT(64, F.driven);
    expect_shown(1, "passes");
    TEST_ASSERT_EQUAL_INT(1, F.fulls);
}

/* A board whose panel cannot drive every pixel of a window (the OG): partial and inverse run, partial_all and passes
 * are refused with the reason; hello lists what it does. */
static void test_board_without_partial_all(void)
{
    device_partial(W, H, 3, 1, false, 0);
    size_t rn;
    const uint8_t *r = request(USBDISP_CMD_HELLO, NULL, 0, &rn);
    s_ans = r, s_ans_len = rn;
    const uint8_t *v;
    size_t vn;
    TEST_ASSERT_TRUE(cbor_map_get(r, rn, "modes", 5, &v, &vn));
    static const uint8_t modes[] = {0x83, 0x64, 'f', 'u', 'l', 'l', 0x67, 'p', 'a', 'r', 't', 'i', 'a', 'l',
                                    0x67, 'i', 'n', 'v', 'e', 'r', 's', 'e'};
    TEST_ASSERT_EQUAL_UINT(sizeof modes, vn);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(modes, v, vn);
    random_px(s_want, W * H, 1);
    TEST_ASSERT_EQUAL_INT(0, show_want(NULL, 0));
    TEST_ASSERT_EQUAL_INT(USBDISP_E_MODE, show_box(0, 0, 8, 8, "partial_all", 0, false));
    TEST_ASSERT_EQUAL_STRING("this board cannot drive every pixel of an area: no partial_all", ans_str("msg"));
    TEST_ASSERT_EQUAL_INT(USBDISP_E_MODE, show_box(0, 0, 8, 8, "passes", 2, false));
    TEST_ASSERT_EQUAL_STRING("this board cannot drive every pixel of an area: no passes", ans_str("msg"));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_want, F.shown, W * H); /* a refusal never changes the panel */
    memset(s_want, 1, sizeof s_want); /* after a refusal: a full refresh, the rest white */
    TEST_ASSERT_EQUAL_INT(0, show_box(0, 0, 8, 8, "inverse", 0, true));
    TEST_ASSERT_EQUAL_STRING("full", ans_str("mode"));
    TEST_ASSERT_EQUAL_STRING("refused", ans_str("why"));
    TEST_ASSERT_EQUAL_INT(0, show_box(8, 8, 8, 8, "inverse", 0, true));
    TEST_ASSERT_EQUAL_STRING("inverse", ans_str("mode"));
    TEST_ASSERT_EQUAL_INT(0, show_box(16, 8, 8, 8, NULL, 0, true));
    TEST_ASSERT_EQUAL_STRING("partial", ans_str("mode"));
    expect_shown(1, "og");
}

/* Gray partials (a panel that takes them): the changed rectangle comes in whole groups of 8 pixels at 2 bit too, and
 * the modes that need a 1 bit picture are refused. */
static void test_gray_partial(void)
{
    device_partial(W, H, 3, 3, true, 0);
    random_px(s_want, W * H, 2);
    size_t n = make_png(s_png, W, H, 2, 0, s_want);
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 512));
    TEST_ASSERT_EQUAL_STRING("full", ans_str("mode"));
    s_want[5 * W + 13] ^= 3;
    s_want[7 * W + 30] ^= 1;
    n = make_png(s_png, W, H, 2, 0, s_want);
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 512));
    TEST_ASSERT_EQUAL_STRING("partial", ans_str("mode"));
    TEST_ASSERT_EQUAL_INT(8, map_int(s_ans, s_ans_len, "x", -1));
    TEST_ASSERT_EQUAL_INT(24, map_int(s_ans, s_ans_len, "w", -1));
    TEST_ASSERT_EQUAL_INT(5, map_int(s_ans, s_ans_len, "y", -1));
    TEST_ASSERT_EQUAL_INT(3, map_int(s_ans, s_ans_len, "h", -1));
    TEST_ASSERT_EQUAL_INT(2, F.driven);
    expect_shown(2, "gray partial");
    TEST_ASSERT_EQUAL_INT(USBDISP_E_MODE, show_at(s_png, n, 512, -1, 0, "partial_all", 0));
    TEST_ASSERT_EQUAL_STRING("partial_all needs a 1 bit picture", ans_str("msg"));
    /* a 1 bit area on a 2 bit picture: the rest of the panel cannot be told */
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 512));
    TEST_ASSERT_EQUAL_INT(USBDISP_E_DEPTH, show_box(0, 0, 8, 8, NULL, 0, false));
    TEST_ASSERT_EQUAL_STRING("the picture shown has 2 bit and the rectangle 1 bit: an area must have the same depth", ans_str("msg"));
    /* refused before the panel was touched, and the frame is forgotten: the next update is a full one */
    TEST_ASSERT_NULL(usbdisp_prev(&D, NULL));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_want, F.shown, W * H);
}

/* A panel setting takes the gray partial away while it lasts (cfg partial_now; the X's tuning key `driver`: plain): a
 * changed gray picture is then a full refresh and the answer says why, `partial` named is refused in words before the
 * panel is touched, 1 bit partials go on, and with the setting gone the gray partial is back. */
static uint8_t s_now_mask = 7;
static uint8_t now_mask(void *ctx, uint8_t depths) { (void)ctx; return depths & s_now_mask; }

static void test_gray_partial_taken_away(void)
{
    device_partial(W, H, 7, 7, true, 0);
    C.partial_now = now_mask;
    usbdisp_init(&D, &C);
    s_now_mask = 1;
    random_px(s_want, W * H, 4);
    size_t n = make_png(s_png, W, H, 4, 0, s_want);
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 512));
    TEST_ASSERT_EQUAL_STRING("full", ans_str("mode"));
    s_want[5 * W + 13] ^= 5;
    n = make_png(s_png, W, H, 4, 0, s_want);
    int partials = F.partials, fulls = F.fulls;
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 512));
    TEST_ASSERT_EQUAL_STRING("full", ans_str("mode"));
    TEST_ASSERT_EQUAL_STRING("depth", ans_str("why"));
    TEST_ASSERT_EQUAL_INT(partials, F.partials);
    TEST_ASSERT_EQUAL_INT(fulls + 1, F.fulls);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_want, F.shown, W * H);
    TEST_ASSERT_NULL(usbdisp_prev(&D, NULL)); /* no frame kept at a depth without partial updates */
    s_want[6 * W + 13] ^= 5;
    n = make_png(s_png, W, H, 4, 0, s_want);
    int begins = F.begins;
    TEST_ASSERT_EQUAL_INT(USBDISP_E_MODE, show_at(s_png, n, 512, -1, 0, "partial", 0));
    TEST_ASSERT_NOT_NULL(strstr(ans_str("msg"), "4 bit"));
    TEST_ASSERT_EQUAL_INT(begins, F.begins);
    /* 1 bit: a partial update as ever */
    random_px(s_want, W * H, 1);
    n = make_png(s_png, W, H, 1, 0, s_want);
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 512));
    s_want[5 * W + 13] ^= 1;
    n = make_png(s_png, W, H, 1, 0, s_want);
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 512));
    TEST_ASSERT_EQUAL_STRING("partial", ans_str("mode"));
    /* the setting gone: gray partials again */
    s_now_mask = 7;
    random_px(s_want, W * H, 4);
    n = make_png(s_png, W, H, 4, 0, s_want);
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 512));
    s_want[5 * W + 13] ^= 5;
    n = make_png(s_png, W, H, 4, 0, s_want);
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 512));
    TEST_ASSERT_EQUAL_STRING("partial", ans_str("mode"));
    TEST_ASSERT_EQUAL_STRING("changed", ans_str("why"));
}

/* A panel that refreshes one rectangle fully, alone (cfg full_area_now, panel_area; the X under `driver` plain), with
 * partial updates at 1 bit only: hello says so; a changed gray picture is one full refresh of the changed rectangle
 * (`why: area`, the answer's area the rectangle, the rest of the glass untouched, the frame kept is the glass); a
 * rectangle sent at 4 bit, without a mode or with full, is the same; a change one row high takes a second row; 1 bit
 * stays a partial update, and with `full` named on a 1 bit rectangle it is the rectangle's full refresh; the first
 * picture and the periodic full refresh are whole; a panel that refuses the rectangle fails the request; with the
 * offer gone the old answers are back. */
static uint8_t s_fa_mask = 7;
static uint8_t fa_mask(void *ctx, uint8_t depths) { (void)ctx; return depths & s_fa_mask; }

static void area_device(uint8_t every)
{
    device_partial(W, H, 7, 7, true, every);
    s_now_mask = 1, s_fa_mask = 7;
    C.partial_now = now_mask;
    C.full_area_now = fa_mask;
    C.panel_area = f_area;
    usbdisp_init(&D, &C);
}

static void test_full_refresh_of_a_rectangle(void)
{
    area_device(0);
    const uint8_t *v;
    size_t vn;
    cbor_r_t rd;
    cbor_item_t it;
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HELLO, NULL, 0));
    TEST_ASSERT_TRUE(cbor_map_get(s_ans, s_ans_len, "full_area", 9, &v, &vn));
    cbor_r_init(&rd, v, vn);
    TEST_ASSERT_TRUE(cbor_read(&rd, &it) && it.type == CBOR_ARRAY && it.u == 3);
    /* the first picture: whole */
    random_px(s_want, W * H, 4);
    size_t n = make_png(s_png, W, H, 4, 0, s_want);
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 512));
    TEST_ASSERT_EQUAL_STRING("first", ans_str("why"));
    TEST_ASSERT_EQUAL_INT(0, F.area_fulls);
    /* a change: the changed rectangle alone, fully */
    s_want[5 * W + 13] ^= 5;
    s_want[7 * W + 30] ^= 9;
    n = make_png(s_png, W, H, 4, 0, s_want);
    int partials = F.partials;
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 512));
    TEST_ASSERT_EQUAL_STRING("full", ans_str("mode"));
    TEST_ASSERT_EQUAL_STRING("area", ans_str("why"));
    TEST_ASSERT_EQUAL_INT(8, map_int(s_ans, s_ans_len, "x", -1));
    TEST_ASSERT_EQUAL_INT(24, map_int(s_ans, s_ans_len, "w", -1));
    TEST_ASSERT_EQUAL_INT(5, map_int(s_ans, s_ans_len, "y", -1));
    TEST_ASSERT_EQUAL_INT(3, map_int(s_ans, s_ans_len, "h", -1));
    TEST_ASSERT_EQUAL_INT(1, F.area_fulls);
    TEST_ASSERT_EQUAL_INT(partials, F.partials);
    TEST_ASSERT_EQUAL_INT(8, F.area.x);
    TEST_ASSERT_EQUAL_INT(3, F.area.h);
    expect_shown(4, "changed rectangle, full");
    /* one row changed: two rows driven */
    s_want[(H - 1) * W + 3] ^= 5;
    n = make_png(s_png, W, H, 4, 0, s_want);
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 512));
    TEST_ASSERT_EQUAL_STRING("area", ans_str("why"));
    TEST_ASSERT_EQUAL_INT(H - 2, map_int(s_ans, s_ans_len, "y", -1));
    TEST_ASSERT_EQUAL_INT(2, map_int(s_ans, s_ans_len, "h", -1));
    expect_shown(4, "one changed row at the bottom");
    /* a rectangle sent at 4 bit: without a mode its changed part, with full all of it */
    random_px(s_box, 16 * 6, 4);
    for (int j = 0; j < 6; j++)
        memcpy(s_want + (size_t)(4 + j) * W + 16, s_box + j * 16, 16);
    n = make_png(s_png, 16, 6, 4, 0, s_box);
    TEST_ASSERT_EQUAL_INT(0, show_at(s_png, n, 512, 16, 4, NULL, 0));
    TEST_ASSERT_EQUAL_STRING("full", ans_str("mode"));
    TEST_ASSERT_EQUAL_STRING("area", ans_str("why"));
    expect_shown(4, "rectangle, no mode");
    random_px(s_box, 16 * 6, 4);
    for (int j = 0; j < 6; j++)
        memcpy(s_want + (size_t)(4 + j) * W + 16, s_box + j * 16, 16);
    n = make_png(s_png, 16, 6, 4, 0, s_box);
    TEST_ASSERT_EQUAL_INT(0, show_at(s_png, n, 512, 16, 4, "full", 0));
    TEST_ASSERT_EQUAL_STRING("area", ans_str("why"));
    TEST_ASSERT_EQUAL_INT(16, map_int(s_ans, s_ans_len, "x", -1));
    TEST_ASSERT_EQUAL_INT(4, map_int(s_ans, s_ans_len, "y", -1));
    TEST_ASSERT_EQUAL_INT(16, map_int(s_ans, s_ans_len, "w", -1));
    TEST_ASSERT_EQUAL_INT(6, map_int(s_ans, s_ans_len, "h", -1));
    expect_shown(4, "rectangle, full named");
    /* `partial` named at 4 bit is still refused in words */
    s_want[2 * W + 2] ^= 3;
    n = make_png(s_png, W, H, 4, 0, s_want);
    TEST_ASSERT_EQUAL_INT(USBDISP_E_MODE, show_at(s_png, n, 512, -1, 0, "partial", 0));
    TEST_ASSERT_NOT_NULL(strstr(ans_str("msg"), "4 bit"));

    /* 1 bit: changed pixels are a partial update; a rectangle with full named is the rectangle's full refresh */
    area_device(0);
    random_px(s_want, W * H, 1);
    TEST_ASSERT_EQUAL_INT(0, show_want(NULL, 0));
    s_want[5 * W + 13] ^= 1;
    int areas = F.area_fulls;
    TEST_ASSERT_EQUAL_INT(0, show_want(NULL, 0));
    TEST_ASSERT_EQUAL_STRING("partial", ans_str("mode"));
    TEST_ASSERT_EQUAL_INT(areas, F.area_fulls);
    TEST_ASSERT_EQUAL_INT(0, show_box(8, 2, 16, 4, "full", 0, true));
    TEST_ASSERT_EQUAL_STRING("full", ans_str("mode"));
    TEST_ASSERT_EQUAL_STRING("area", ans_str("why"));
    TEST_ASSERT_EQUAL_INT(areas + 1, F.area_fulls);
    expect_shown(1, "1 bit rectangle, full named");
    /* a whole picture with full named is the whole panel, as asked */
    TEST_ASSERT_EQUAL_INT(0, show_want("full", 0));
    TEST_ASSERT_EQUAL_STRING("asked", ans_str("why"));
    TEST_ASSERT_EQUAL_INT(areas + 1, F.area_fulls);

    /* the periodic full refresh is whole, and a rectangle's full refresh does not put it off */
    area_device(2);
    random_px(s_want, W * H, 4);
    n = make_png(s_png, W, H, 4, 0, s_want);
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 512));
    for (int k = 0; k < 2; k++) {
        s_want[5 * W + 13 + k] ^= 5;
        n = make_png(s_png, W, H, 4, 0, s_want);
        TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 512));
        TEST_ASSERT_EQUAL_STRING("area", ans_str("why"));
    }
    s_want[5 * W + 20] ^= 5;
    n = make_png(s_png, W, H, 4, 0, s_want);
    areas = F.area_fulls;
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 512));
    TEST_ASSERT_EQUAL_STRING("cleanup", ans_str("why"));
    TEST_ASSERT_EQUAL_INT(areas, F.area_fulls);

    /* the panel refuses the rectangle: the request fails, the frame is forgotten, the next picture is whole */
    area_device(0);
    random_px(s_want, W * H, 4);
    n = make_png(s_png, W, H, 4, 0, s_want);
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 512));
    s_want[5 * W + 13] ^= 5;
    n = make_png(s_png, W, H, 4, 0, s_want);
    F.fail_begin = 0;
    s_fa_mask = 7;
    F.fail_refresh = 1;
    TEST_ASSERT_EQUAL_INT(USBDISP_E_IO, show(s_png, n, 512));
    TEST_ASSERT_NULL(usbdisp_prev(&D, NULL));
    F.fail_refresh = 0;
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 512));
    TEST_ASSERT_EQUAL_STRING("full", ans_str("mode"));
    TEST_ASSERT_EQUAL_STRING("failed", ans_str("why"));

    /* the offer gone (the key back to ours, gray partial back): a partial update again */
    area_device(0);
    s_now_mask = 7, s_fa_mask = 0;
    random_px(s_want, W * H, 4);
    n = make_png(s_png, W, H, 4, 0, s_want);
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 512));
    s_want[5 * W + 13] ^= 5;
    n = make_png(s_png, W, H, 4, 0, s_want);
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 512));
    TEST_ASSERT_EQUAL_STRING("partial", ans_str("mode"));
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HELLO, NULL, 0));
    TEST_ASSERT_FALSE(cbor_map_get(s_ans, s_ans_len, "full_area", 9, &v, &vn));
}

/* A gray partial keeps copies of its rectangle in RAM: the most pixels it takes is a limit of the board (usbdisp.h). Auto
 * chooses a full refresh for a larger change, and says why; `partial` named is refused before the panel is touched; a
 * single large rectangle is judged the same; a change under the limit is still a partial update. */
static void gray_limit_device(uint32_t max_px)
{
    device_partial(W, H, 7, 7, true, 0);
    C.gray_partial_max_px = max_px;
    usbdisp_init(&D, &C);
    random_px(s_want, W * H, 4);
    size_t n = make_png(s_png, W, H, 4, 0, s_want);
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 512));
    TEST_ASSERT_EQUAL_STRING("full", ans_str("mode")); /* the first picture, at 4 bit */
}

static void test_gray_partial_too_large(void)
{
    /* hello tells the limit per gray depth (a board without a limit, or with 1 bit only, has no key) */
    gray_limit_device(100);
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HELLO, NULL, 0));
    const uint8_t *pm, *pv;
    size_t pmn, pvn;
    TEST_ASSERT_TRUE(cbor_map_get(s_ans, s_ans_len, "partial_max", 11, &pm, &pmn));
    TEST_ASSERT_EQUAL_INT(100, map_int(pm, pmn, "2", -1));
    TEST_ASSERT_EQUAL_INT(100, map_int(pm, pmn, "4", -1));
    TEST_ASSERT_FALSE(cbor_map_get(pm, pmn, "1", 1, &pv, &pvn));
    device_partial(W, H, 1, 1, true, 0);
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HELLO, NULL, 0));
    TEST_ASSERT_FALSE(cbor_map_get(s_ans, s_ans_len, "partial_max", 11, &pm, &pmn));
    /* under the limit: a partial update */
    gray_limit_device(40 * 1000);
    s_want[5 * W + 13] ^= 5;
    size_t n = make_png(s_png, W, H, 4, 0, s_want);
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 512));
    TEST_ASSERT_EQUAL_STRING("partial", ans_str("mode"));
    TEST_ASSERT_EQUAL_STRING("changed", ans_str("why"));

    /* auto, a whole new picture over the limit: a full refresh, "large", the panel's partial path not used */
    gray_limit_device(100);
    random_px(s_want, W * H, 4);
    n = make_png(s_png, W, H, 4, 0, s_want);
    int partials = F.partials, fulls = F.fulls;
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 512));
    TEST_ASSERT_EQUAL_STRING("full", ans_str("mode"));
    TEST_ASSERT_EQUAL_STRING("large", ans_str("why"));
    TEST_ASSERT_EQUAL_INT(partials, F.partials);
    TEST_ASSERT_EQUAL_INT(fulls + 1, F.fulls);
    expect_shown(4, "large change: full");
    /* the frame is known: a small change after it is a partial update again */
    s_want[5 * W + 13] ^= 5;
    n = make_png(s_png, W, H, 4, 0, s_want);
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 512));
    TEST_ASSERT_EQUAL_STRING("partial", ans_str("mode"));

    /* `partial` named, too large: refused before the panel is touched, the limit and the size in the message */
    random_px(s_want, W * H, 4);
    n = make_png(s_png, W, H, 4, 0, s_want);
    int begins = F.begins;
    TEST_ASSERT_EQUAL_INT(USBDISP_E_MODE, show_at(s_png, n, 512, -1, 0, "partial", 0));
    TEST_ASSERT_NOT_NULL(strstr(ans_str("msg"), "a partial update at 4 bit takes at most 100 changed pixels in its rectangle, this one has "));
    TEST_ASSERT_NOT_NULL(strstr(ans_str("msg"), ": use full"));
    TEST_ASSERT_EQUAL_INT(begins, F.begins);
    TEST_ASSERT_EQUAL_INT(0, F.live);
    TEST_ASSERT_NULL(usbdisp_prev(&D, NULL)); /* refused: the next update is a full one */
    TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 512));
    TEST_ASSERT_EQUAL_STRING("full", ans_str("mode"));

    /* a single large rectangle: judged the same */
    uint8_t box[32 * 8];
    random_px(box, sizeof box, 4);
    size_t bn = make_png(s_png, 32, 8, 4, 0, box);
    begins = F.begins;
    TEST_ASSERT_EQUAL_INT(USBDISP_E_MODE, show_at(s_png, bn, 512, 8, 4, "partial", 0));
    TEST_ASSERT_EQUAL_INT(begins, F.begins);
}

/* After a real panel failure at a gray depth the frame kept is unknown: the next update is a full one and says why. */
static void test_gray_panel_failure_forgets_the_frame(void)
{
    for (int step = 0; step < 2; step++) {
        gray_limit_device(40 * 1000);
        s_want[5 * W + 13] ^= 5;
        size_t n = make_png(s_png, W, H, 4, 0, s_want);
        if (step == 0)
            F.fail_write = 1;
        else
            F.fail_refresh = 1;
        TEST_ASSERT_EQUAL_INT(USBDISP_E_IO, show(s_png, n, 512));
        F.fail_write = F.fail_refresh = 0;
        TEST_ASSERT_NULL(usbdisp_prev(&D, NULL));
        TEST_ASSERT_EQUAL_INT(F.begins, F.sleeps);
        TEST_ASSERT_EQUAL_INT(0, show(s_png, n, 512));
        TEST_ASSERT_EQUAL_STRING("full", ans_str("mode"));
        TEST_ASSERT_EQUAL_STRING("failed", ans_str("why"));
    }
}

/* A transfer that went quiet was cut: the device no longer trusts its frame, the next update is a full one. */
static void test_cut_transfer_then_full(void)
{
    device_partial(W, H, 1, 1, true, 0);
    random_px(s_want, W * H, 1);
    TEST_ASSERT_EQUAL_INT(0, show_want(NULL, 0));
    uint8_t p[32];
    cbor_w_t w;
    cbor_w_init(&w, p, sizeof p);
    cbor_put_map(&w, 1);
    cbor_put_str(&w, "size");
    cbor_put_uint(&w, 100);
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_SHOW_BEGIN, p, w.len));
    TEST_ASSERT_NOT_NULL(usbdisp_prev(&D, NULL));
    F.now += USBDISP_IDLE_US;
    usbdisp_poll(&D, 0);
    TEST_ASSERT_FALSE(usbdisp_transfer(&D));
    TEST_ASSERT_NULL(usbdisp_prev(&D, NULL));
    TEST_ASSERT_EQUAL_INT(0, F.live);
    s_want[3] ^= 1;
    TEST_ASSERT_EQUAL_INT(0, show_want("partial", 0));
    TEST_ASSERT_EQUAL_STRING("full", ans_str("mode"));
    TEST_ASSERT_EQUAL_STRING("cut", ans_str("why"));
    TEST_ASSERT_EQUAL_INT(2, F.fulls);
    expect_shown(1, "after the cut");
}

/* Every allocation of an update in every mode fails once: a clean NOMEM (the panel as it was, nothing leaked) or a
 * full refresh that says "memory"; the update after it shows its picture. */
static void test_alloc_fails_in_every_mode(void)
{
    static const char *const MODES[] = {NULL, "partial", "partial_all", "inverse", "passes", "full"};
    static uint8_t before[MAXPIX];
    int nomem = 0, fallback = 0;
    for (int m = 0; m < 6; m++)
        for (int area = 0; area < 2; area++)
            for (int k = 0;; k++) {
                char what[64];
                snprintf(what, sizeof what, "mode %s area %d alloc %d", MODES[m] ? MODES[m] : "none", area, k);
                device_partial(W, H, 1, 1, true, 0);
                random_px(s_want, W * H, 1);
                TEST_ASSERT_EQUAL_INT(0, show_want(NULL, 0));
                memcpy(before, F.shown, sizeof before);
                int refreshes = F.refreshes;
                if (!area)
                    random_px(s_want, W * H, 1);
                F.allocs = 0;
                F.fail_at = k;
                int e = area ? show_box(8, 8, 24, 12, MODES[m], m == 4 ? 2 : 0, true) : show_want(MODES[m], m == 4 ? 2 : 0);
                bool hit = F.allocs > k;
                F.fail_at = -1;
                if (e) {
                    TEST_ASSERT_EQUAL_INT_MESSAGE(USBDISP_E_NOMEM, e, what);
                    TEST_ASSERT_TRUE_MESSAGE(hit, what);
                    TEST_ASSERT_EQUAL_INT_MESSAGE(refreshes, F.refreshes, what);
                    TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(before, F.shown, sizeof before, what);
                    TEST_ASSERT_EQUAL_INT_MESSAGE(0, F.live, what);
                    TEST_ASSERT_FALSE(usbdisp_transfer(&D));
                    nomem++;
                } else {
                    expect_shown(1, what);
                    if (hit) { /* the frame kept was given up for the picture */
                        TEST_ASSERT_EQUAL_STRING_MESSAGE("full", ans_str("mode"), what);
                        if (m == 0)
                            TEST_ASSERT_EQUAL_STRING_MESSAGE("memory", ans_str("why"), what);
                        fallback++;
                    }
                    TEST_ASSERT_EQUAL_INT_MESSAGE(1, F.live, what);
                }
                random_px(s_want, W * H, 1);
                TEST_ASSERT_EQUAL_INT_MESSAGE(0, show_want(NULL, 0), what);
                if (e) {
                    TEST_ASSERT_EQUAL_STRING_MESSAGE("full", ans_str("mode"), what);
                    TEST_ASSERT_EQUAL_STRING_MESSAGE("refused", ans_str("why"), what);
                }
                expect_shown(1, what);
                usbdisp_reset(&D);
                TEST_ASSERT_EQUAL_INT_MESSAGE(0, F.live, what);
                if (!hit)
                    break;
            }
    TEST_ASSERT_TRUE_MESSAGE(nomem >= 40 && fallback >= 4, "fewer allocation points than the code has");
}

/* The panel driver fails in the middle of an update (the second refresh of an inverse): IO, and since the glass may
 * show either picture the next update is a full one. */
static void test_panel_fails_mid_update(void)
{
    for (int step = 0; step < 3; step++) {
        device_partial(W, H, 1, 1, true, 0);
        random_px(s_want, W * H, 1);
        TEST_ASSERT_EQUAL_INT(0, show_want(NULL, 0));
        if (step == 0)
            F.fail_begin = 2;
        else if (step == 1)
            F.fail_refresh = 2;
        else
            F.fail_write = 1;
        TEST_ASSERT_EQUAL_INT(USBDISP_E_IO, show_box(8, 8, 16, 8, "inverse", 0, true));
        F.fail_begin = F.fail_refresh = F.fail_write = 0;
        TEST_ASSERT_NULL(usbdisp_prev(&D, NULL));
        TEST_ASSERT_EQUAL_INT(0, F.live);
        TEST_ASSERT_EQUAL_INT(F.begins, F.sleeps);
        TEST_ASSERT_EQUAL_INT(0, show_want(NULL, 0));
        TEST_ASSERT_EQUAL_STRING("full", ans_str("mode"));
        TEST_ASSERT_EQUAL_STRING("failed", ans_str("why"));
        expect_shown(1, "after the failure");
    }
}

/* The gate clock count the panel measured is in the answer and the log; a wrong count is marked. */
static void test_gate_clocks_reported(void)
{
    device_partial(W, H, 1, 1, true, 0);
    random_px(s_want, W * H, 1);
    TEST_ASSERT_EQUAL_INT(0, show_want(NULL, 0));
    const uint8_t *v;
    size_t vn;
    TEST_ASSERT_FALSE(cbor_map_get(s_ans, s_ans_len, "ckv", 3, &v, &vn)); /* a board that does not count */
    F.gates = true;
    F.gate = (usbdisp_gates_t){11265, 8, 1404, 1408, true};
    s_want[0] ^= 1;
    TEST_ASSERT_EQUAL_INT(0, show_want(NULL, 0));
    TEST_ASSERT_TRUE(cbor_map_get(s_ans, s_ans_len, "ckv", 3, &v, &vn));
    TEST_ASSERT_EQUAL_INT(11265, map_int(v, vn, "edges", 0));
    TEST_ASSERT_EQUAL_INT(8, map_int(v, vn, "frames", 0));
    TEST_ASSERT_EQUAL_INT(1408, map_int(v, vn, "per", 0));
    TEST_ASSERT_EQUAL_INT(1404, map_int(v, vn, "rows", 0));
    TEST_ASSERT_NOT_NULL(strstr(F.log, "show mode=partial why=changed area=0,0,8,1 depth=1"));
    TEST_ASSERT_NOT_NULL(strstr(F.log, "ckv=11265/8 per=1408 rows=1404"));
    TEST_ASSERT_NULL(strstr(F.log, "WRONG"));
    /* the log line splits the update's time: ms on the panel, pre before it (here: its allocations, 7 ms each, and
     * none of the transfer's, which come before the update) */
    TEST_ASSERT_NOT_NULL(strstr(F.log, " pre=0 n="));
    F.alloc_us = 7000;
    F.refresh_us = 123000;
    s_want[0] ^= 1;
    uint64_t t0 = F.now;
    TEST_ASSERT_EQUAL_INT(0, show_want(NULL, 0));
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(F.log, "show mode=partial"), F.log);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(F.log, " ms=123 pre="), F.log);
    int pre = atoi(strstr(F.log, " pre=") + 5);
    TEST_ASSERT_TRUE_MESSAGE(pre >= 7 && pre % 7 == 0 && (uint64_t)pre * 1000 + 123000 < F.now - t0, F.log);
    F.alloc_us = F.refresh_us = 0;
    /* issue #203: two gate clocks a row */
    F.gate = (usbdisp_gates_t){22495, 8, 1404, 2811, false};
    s_want[0] ^= 1;
    TEST_ASSERT_EQUAL_INT(0, show_want(NULL, 0));
    TEST_ASSERT_TRUE(cbor_map_get(s_ans, s_ans_len, "ckv", 3, &v, &vn));
    TEST_ASSERT_EQUAL_INT(2811, map_int(v, vn, "per", 0));
    static const uint8_t ok_false[] = {0x62, 'o', 'k', 0xF4};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ok_false, v + vn - sizeof ok_false, sizeof ok_false);
    TEST_ASSERT_EQUAL_CHAR('W', F.log[0]);
    TEST_ASSERT_NOT_NULL(strstr(F.log, "per=2811 rows=1404 WRONG"));
    /* nothing driven: no count of an older update */
    TEST_ASSERT_EQUAL_INT(0, show_want(NULL, 0));
    TEST_ASSERT_EQUAL_STRING("none", ans_str("mode"));
    TEST_ASSERT_FALSE(cbor_map_get(s_ans, s_ans_len, "ckv", 3, &v, &vn));
}

/* show_end's answer carries `steps`: the milliseconds of each device step and the total, the steps adding up to at most
 * the total (docs/spec/usb-display.md "The answer"). */
static void test_steps_in_the_answer(void)
{
    static const char *const KEYS[] = {"recv", "prep", "inflate", "convert", "diff", "fingerprint", "panel", "clean"};
    device_partial(W, H, 1, 1, true, 0);
    random_px(s_want, W * H, 1);
    TEST_ASSERT_EQUAL_INT(0, show_want(NULL, 0)); /* the first picture: a full refresh */
    F.tick_us = 3000;                              /* each reading of the clock costs 3 ms */
    F.alloc_us = 5000;
    F.refresh_us = 40000;
    s_want[0] ^= 1;
    TEST_ASSERT_EQUAL_INT(0, show_want(NULL, 0));
    const uint8_t *v;
    size_t vn;
    TEST_ASSERT_TRUE_MESSAGE(cbor_map_get(s_ans, s_ans_len, "steps", 5, &v, &vn), "the answer has steps");
    uint32_t sum = 0;
    for (size_t i = 0; i < sizeof KEYS / sizeof *KEYS; i++) {
        int ms = map_int(v, vn, KEYS[i], -1);
        TEST_ASSERT_TRUE_MESSAGE(ms >= 0, KEYS[i]);
        sum += (uint32_t)ms;
    }
    int total = map_int(v, vn, "total", -1);
    TEST_ASSERT_TRUE_MESSAGE(total > 0, "total");
    TEST_ASSERT_TRUE_MESSAGE(sum <= (uint32_t)total, "the steps add up to at most the total");
    TEST_ASSERT_EQUAL_INT(map_int(v, vn, "panel", -1), (int)map_int(s_ans, s_ans_len, "ms", -2)); /* panel is the answer's ms */
    TEST_ASSERT_TRUE_MESSAGE(map_int(v, vn, "panel", 0) >= 40, "panel carries the refresh");
    TEST_ASSERT_TRUE_MESSAGE(map_int(v, vn, "prep", 0) >= 5, "prep carries the frame's allocation");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(F.log, " steps="), F.log);
    F.tick_us = F.alloc_us = F.refresh_us = 0;
    /* nothing driven: the steps are still there, panel 0 */
    TEST_ASSERT_EQUAL_INT(0, show_want(NULL, 0));
    TEST_ASSERT_EQUAL_STRING("none", ans_str("mode"));
    TEST_ASSERT_TRUE(cbor_map_get(s_ans, s_ans_len, "steps", 5, &v, &vn));
    TEST_ASSERT_EQUAL_INT(0, map_int(v, vn, "panel", -1));
}

static int set_full_every(int v)
{
    uint8_t p[32];
    cbor_w_t w;
    cbor_w_init(&w, p, sizeof p);
    cbor_put_map(&w, 1);
    cbor_put_str(&w, "full_every");
    cbor_put_uint(&w, (uint64_t)v);
    return req_err(USBDISP_CMD_SET, p, w.len);
}

/* full_every: the board's default, the stored value over it, a clean-up full refresh after that many partial ones
 * (only when the host leaves the mode to the device), and a store that fails. */
static void test_full_every_setting(void)
{
    device_partial(W, H, 1, 1, true, 3);
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_SET, NULL, 0));
    TEST_ASSERT_EQUAL_INT(3, map_int(s_ans, s_ans_len, "full_every", -1));
    random_px(s_want, W * H, 1);
    static const char *const want[] = {"full", "partial", "partial", "partial", "full", "partial"};
    for (int i = 0; i < 6; i++) {
        s_want[i] ^= 1;
        TEST_ASSERT_EQUAL_INT(0, show_want(NULL, 0));
        TEST_ASSERT_EQUAL_STRING(want[i], ans_str("mode"));
        TEST_ASSERT_EQUAL_STRING(i == 0 ? "first" : i == 4 ? "cleanup" : "changed", ans_str("why"));
        expect_shown(1, "full_every");
    }
    /* a mode the host names is run as named, however many partial ones came before */
    for (int i = 0; i < 6; i++) {
        s_want[i] ^= 1;
        TEST_ASSERT_EQUAL_INT(0, show_want("partial", 0));
        TEST_ASSERT_EQUAL_STRING("partial", ans_str("mode"));
    }
    s_want[0] ^= 1;
    TEST_ASSERT_EQUAL_INT(0, show_want(NULL, 0));
    TEST_ASSERT_EQUAL_STRING("cleanup", ans_str("why"));
    /* stored, and read at the next start */
    TEST_ASSERT_EQUAL_INT(0, set_full_every(0));
    TEST_ASSERT_EQUAL_INT(0, F.kv);
    for (int i = 0; i < 30; i++) {
        s_want[i] ^= 1;
        TEST_ASSERT_EQUAL_INT(0, show_want(NULL, 0));
        TEST_ASSERT_EQUAL_STRING("partial", ans_str("mode"));
    }
    TEST_ASSERT_EQUAL_INT(0, set_full_every(7));
    usbdisp_reset(&D);
    usbdisp_init(&D, &C);
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_SET, NULL, 0));
    TEST_ASSERT_EQUAL_INT(7, map_int(s_ans, s_ans_len, "full_every", -1));
    /* a store that fails: said so, the value in use stays */
    F.fail_kv = 1;
    TEST_ASSERT_EQUAL_INT(USBDISP_E_IO, set_full_every(9));
    F.fail_kv = 0;
    TEST_ASSERT_EQUAL_INT(USBDISP_E_BAD_REQUEST, set_full_every(USBDISP_FULL_EVERY_MAX + 1));
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_SET, NULL, 0));
    TEST_ASSERT_EQUAL_INT(7, map_int(s_ans, s_ans_len, "full_every", -1));
    /* a stored byte out of range is not taken */
    F.kv = 251;
    usbdisp_init(&D, &C);
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_SET, NULL, 0));
    TEST_ASSERT_EQUAL_INT(3, map_int(s_ans, s_ans_len, "full_every", -1));
}

/* A 1 bit partial board with clean_every `ce` and full_every `fe`; a first full picture is shown. */
static void clean_device(uint8_t ce, uint8_t fe)
{
    device_partial(W, H, 1, 1, true, fe);
    C.clean_every = ce;
    usbdisp_init(&D, &C);
    random_px(s_want, W * H, 1);
    TEST_ASSERT_EQUAL_INT(0, show_want(NULL, 0));
}

/* One partial update that flips the pixel at x, y. */
static void flip(int x, int y, const char *mode)
{
    s_want[y * W + x] ^= 1;
    TEST_ASSERT_EQUAL_INT(0, show_want(mode, 0));
}

/* The answer's `clean` as x,y,w,h; false: none. */
static bool clean_of(int *x, int *y, int *w, int *h, const char **mode)
{
    const uint8_t *v;
    size_t vn;
    if (!cbor_map_get(s_ans, s_ans_len, "clean", 5, &v, &vn))
        return false;
    *x = (int)map_int(v, vn, "x", -1), *y = (int)map_int(v, vn, "y", -1);
    *w = (int)map_int(v, vn, "w", -1), *h = (int)map_int(v, vn, "h", -1);
    *mode = NULL;
    const uint8_t *m;
    size_t mn;
    if (cbor_map_get(v, vn, "mode", 4, &m, &mn))
        *mode = (mn == 1 + 7 && !memcmp(m + 1, "inverse", 7)) ? "inverse" : "other";
    TEST_ASSERT_TRUE(map_int(v, vn, "ms", -1) >= 0);
    return true;
}

/* clean_every: after N partial updates the Nth answer carries `clean` with the union of their rectangles, drawn again as
 * inverse; the next union starts empty. */
static void test_clean_union_and_restart(void)
{
    int x, y, w, h;
    const char *mode;
    clean_device(3, 0);
    flip(1, 2, NULL);
    TEST_ASSERT_FALSE(clean_of(&x, &y, &w, &h, &mode));
    flip(20, 10, NULL);
    TEST_ASSERT_FALSE(clean_of(&x, &y, &w, &h, &mode));
    int fulls = F.fulls;
    flip(40, 20, NULL);
    TEST_ASSERT_TRUE(clean_of(&x, &y, &w, &h, &mode));
    TEST_ASSERT_EQUAL_INT(0, x);
    TEST_ASSERT_EQUAL_INT(2, y);
    TEST_ASSERT_EQUAL_INT(48, w);
    TEST_ASSERT_EQUAL_INT(19, h);
    TEST_ASSERT_EQUAL_STRING("inverse", mode);
    TEST_ASSERT_EQUAL_INT(fulls, F.fulls); /* no whole-screen flash */
    expect_shown(1, "after a clean");
    /* the next union starts empty: three flips at one place give exactly that place */
    flip(9, 5, NULL);
    flip(9, 5, NULL);
    TEST_ASSERT_FALSE(clean_of(&x, &y, &w, &h, &mode));
    flip(9, 5, NULL);
    TEST_ASSERT_TRUE(clean_of(&x, &y, &w, &h, &mode));
    TEST_ASSERT_EQUAL_INT(8, x);
    TEST_ASSERT_EQUAL_INT(5, y);
    TEST_ASSERT_EQUAL_INT(8, w);
    TEST_ASSERT_EQUAL_INT(1, h);
    expect_shown(1, "after the second clean");
}

/* A full refresh resets both counters: the clean after it counts from it and holds nothing from before. */
static void test_full_resets_clean(void)
{
    int x, y, w, h;
    const char *mode;
    clean_device(3, 0);
    flip(1, 2, NULL);
    flip(41, 30, NULL);
    s_want[0] ^= 1;
    TEST_ASSERT_EQUAL_INT(0, show_want("full", 0));
    flip(9, 5, NULL);
    flip(9, 5, NULL);
    TEST_ASSERT_FALSE(clean_of(&x, &y, &w, &h, &mode)); /* 2 since the full, not 3 */
    flip(9, 5, NULL);
    TEST_ASSERT_TRUE(clean_of(&x, &y, &w, &h, &mode));
    TEST_ASSERT_EQUAL_INT(8, x);
    TEST_ASSERT_EQUAL_INT(5, y);
    TEST_ASSERT_EQUAL_INT(1, h);
}

/* clean_every 0 never cleans; a full refresh due at the same time wins; a depth with no area mode is left to full_every. */
static void test_clean_off_full_wins_no_mode(void)
{
    int x, y, w, h;
    const char *mode;
    clean_device(0, 0);
    for (int i = 0; i < 40; i++) {
        flip(i, 3, NULL);
        TEST_ASSERT_FALSE(clean_of(&x, &y, &w, &h, &mode));
    }
    clean_device(3, 3);
    for (int i = 0; i < 3; i++) {
        flip(i, 3, NULL);
        TEST_ASSERT_FALSE(clean_of(&x, &y, &w, &h, &mode));
    }
    flip(5, 3, NULL);
    TEST_ASSERT_EQUAL_STRING("cleanup", ans_str("why"));
    TEST_ASSERT_FALSE(clean_of(&x, &y, &w, &h, &mode));
    /* 4 bit, partial only: no area mode at that depth */
    device_partial(W, H, 4, 4, false, 0);
    C.clean_every = 2;
    usbdisp_init(&D, &C);
    random_px(s_want, W * H, 4);
    uint8_t png[4096];
    size_t n = make_png(png, W, H, 4, 0, s_want);
    TEST_ASSERT_EQUAL_INT(0, show(png, n, 512));
    for (int i = 0; i < 6; i++) {
        s_want[i] ^= 5;
        n = make_png(png, W, H, 4, 0, s_want);
        TEST_ASSERT_EQUAL_INT(0, show(png, n, 512));
        TEST_ASSERT_FALSE(clean_of(&x, &y, &w, &h, &mode));
    }
}

static int set_clean(int v, bool null)
{
    uint8_t p[32];
    cbor_w_t w;
    cbor_w_init(&w, p, sizeof p);
    cbor_put_map(&w, 1);
    cbor_put_str(&w, "clean_every");
    if (null)
        cbor_put_raw(&w, (const uint8_t[]){0xF6}, 1);
    else
        cbor_put_uint(&w, (uint64_t)v);
    return req_err(USBDISP_CMD_SET, p, w.len);
}

/* The setting: default (the board's), stored over a restart, null restores the default, out of range and a failing store. */
static void test_clean_every_setting(void)
{
    device_partial(W, H, 1, 1, true, 30);
    C.clean_every = USBDISP_CLEAN_EVERY;
    usbdisp_init(&D, &C);
    TEST_ASSERT_EQUAL_INT(15, USBDISP_CLEAN_EVERY);
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HELLO, NULL, 0));
    TEST_ASSERT_EQUAL_INT(15, map_int(s_ans, s_ans_len, "clean_every", -1));
    TEST_ASSERT_EQUAL_INT(0, set_clean(4, false));
    TEST_ASSERT_EQUAL_INT(4, map_int(s_ans, s_ans_len, "clean_every", -1));
    TEST_ASSERT_EQUAL_INT(4, F.kvc);
    TEST_ASSERT_EQUAL_INT(-1, F.kv); /* its own key: full_every is untouched */
    usbdisp_reset(&D);
    usbdisp_init(&D, &C); /* a config reload */
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HELLO, NULL, 0));
    TEST_ASSERT_EQUAL_INT(4, map_int(s_ans, s_ans_len, "clean_every", -1));
    const uint8_t *v;
    size_t vn;
    TEST_ASSERT_TRUE(cbor_map_get(s_ans, s_ans_len, "clean_every_set", 15, &v, &vn));
    TEST_ASSERT_EQUAL_HEX8(0xF5, v[0]);
    TEST_ASSERT_EQUAL_INT(0, set_clean(0, true));
    TEST_ASSERT_EQUAL_INT(15, map_int(s_ans, s_ans_len, "clean_every", -1));
    TEST_ASSERT_EQUAL_INT(-1, F.kvc);
    TEST_ASSERT_TRUE(cbor_map_get(s_ans, s_ans_len, "clean_every_set", 15, &v, &vn));
    TEST_ASSERT_EQUAL_HEX8(0xF4, v[0]);
    TEST_ASSERT_EQUAL_INT(USBDISP_E_BAD_REQUEST, set_clean(USBDISP_CLEAN_EVERY_MAX + 1, false));
    F.fail_kv = 1;
    TEST_ASSERT_EQUAL_INT(USBDISP_E_IO, set_clean(9, false));
    F.fail_kv = 0;
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_SET, NULL, 0));
    TEST_ASSERT_EQUAL_INT(15, map_int(s_ans, s_ans_len, "clean_every", -1));
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_frame_kept_is_the_glass);
    RUN_TEST(test_modes_drive_as_named);
    RUN_TEST(test_board_without_partial_all);
    RUN_TEST(test_gray_partial);
    RUN_TEST(test_gray_partial_taken_away);
    RUN_TEST(test_full_refresh_of_a_rectangle);
    RUN_TEST(test_gray_partial_too_large);
    RUN_TEST(test_gray_panel_failure_forgets_the_frame);
    RUN_TEST(test_cut_transfer_then_full);
    RUN_TEST(test_alloc_fails_in_every_mode);
    RUN_TEST(test_panel_fails_mid_update);
    RUN_TEST(test_gate_clocks_reported);
    RUN_TEST(test_steps_in_the_answer);
    RUN_TEST(test_full_every_setting);
    RUN_TEST(test_clean_union_and_restart);
    RUN_TEST(test_full_resets_clean);
    RUN_TEST(test_clean_off_full_wins_no_mode);
    RUN_TEST(test_clean_every_setting);
    return UNITY_END();
}
