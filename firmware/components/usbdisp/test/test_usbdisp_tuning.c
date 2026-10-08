/* usbdisp: the panel tuning of `set { tuning }` (docs/spec/usb-display.md "Tuning") against fake knobs: a good key applies,
 * a bad one is refused with why and its range while the rest still apply, an unknown key, a board without knobs,
 * "defaults", and hello and every answer reflect the knobs. SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_graytab.h"
#include "epd_scan.h"
#include "usbdisp_fake.h"

static const char *const W_TABLE[] = {"fastepd", "stock"};
static hal_tune_t K[] = {
    {"gray_table", 0, 1, 1, W_TABLE},
    {"gray_dark", 0, 14, 0, NULL},
    {"gray_light", 0, 14, 3, NULL},
    {"passes_partial", 1, 14, 4, NULL},
};
#define NK (sizeof K / sizeof K[0])
static int val[NK];
static int sets;

/* after the fake knobs, the X's scan keys as the driver defines them (epd_scan.h), wired as hal_esp panel_par.c does */
static epd_scan_cfg_t scan = EPD_SCAN_CFG_DEFAULT;

/* the gray table and its rows as the driver keeps them (epd_graytab.h), wired as hal_esp panel_par.c does: knob 0
 * (gray_table) selects the table, and a change of it puts the rows back */
static epd_graytab_t tab;
static bool g_rows_set(void *ctx, const char *s, size_t len, char *why, size_t cap)
{
    (void)ctx;
    return epd_graytab_set_rows(&tab, s, len, why, cap);
}
static void g_rows_get(void *ctx, char *out, size_t cap) { (void)ctx; epd_graytab_rows_str(&tab, out, cap); }

static size_t t_count(void *ctx) { (void)ctx; return NK + EPD_SCAN_KEYS; }
static const hal_tune_t *t_info(void *ctx, size_t i)
{
    static hal_tune_t s[EPD_SCAN_KEYS];
    (void)ctx;
    if (i < NK)
        return &K[i];
    const epd_scan_key_t *k = epd_scan_key(i - NK);
    s[i - NK] = (hal_tune_t){k->name, k->min, k->max, k->def, k->words};
    return &s[i - NK];
}
static int t_get(void *ctx, size_t i) { (void)ctx; return i < NK ? val[i] : epd_scan_key_get(&scan, i - NK); }
static bool t_set(void *ctx, size_t i, int v)
{
    (void)ctx;
    if (i >= NK) {
        if (!epd_scan_key_set(&scan, i - NK, v))
            return false;
        sets++;
        return true;
    }
    if (v < K[i].min || v > K[i].max)
        return false;
    val[i] = v, sets++;
    if (i == 0 && tab.table != v)
        epd_graytab_select(&tab, v);
    return true;
}
static void t_defaults(void *ctx)
{
    (void)ctx;
    for (size_t i = 0; i < NK; i++)
        val[i] = K[i].def;
    epd_scan_key_defaults(&scan);
    epd_graytab_select(&tab, K[0].def);
}

static void knobs(bool on)
{
    device_partial(16, 8, 7, 7, true, 20);
    t_defaults(NULL);
    sets = 0;
    if (on) {
        C.tune_count = t_count, C.tune_info = t_info, C.tune_get = t_get, C.tune_set = t_set, C.tune_defaults = t_defaults;
        C.gray_rows_set = g_rows_set, C.gray_rows_get = g_rows_get;
    } else {
        C.tune_count = NULL, C.tune_info = NULL, C.tune_get = NULL, C.tune_set = NULL, C.tune_defaults = NULL;
        C.gray_rows_set = NULL, C.gray_rows_get = NULL;
    }
    usbdisp_init(&D, &C);
}

/* set { tuning: { key: value ... } }: values 'u' uint, 's' text, 't' true */
typedef struct { const char *key; char kind; uint64_t u; const char *s; } kv_t;
static int set_tuning(const kv_t *kv, size_t n)
{
    uint8_t p[512];
    cbor_w_t w;
    cbor_w_init(&w, p, sizeof p);
    cbor_put_map(&w, 1);
    cbor_put_str(&w, "tuning");
    cbor_put_map(&w, n);
    for (size_t i = 0; i < n; i++) {
        cbor_put_str(&w, kv[i].key);
        if (kv[i].kind == 'u') cbor_put_uint(&w, kv[i].u);
        else if (kv[i].kind == 's') cbor_put_str(&w, kv[i].s);
        else cbor_put_array(&w, 0); /* not a scalar */
    }
    return req_err(USBDISP_CMD_SET, p, w.len);
}

static const char *sub_str(const char *map, const char *key)
{
    static char out[4][160];
    static int k;
    const uint8_t *v, *m;
    size_t vn, mn;
    cbor_r_t r;
    cbor_item_t it;
    char *o = out[k = (k + 1) % 4];
    o[0] = 0;
    if (cbor_map_get(s_ans, s_ans_len, map, strlen(map), &m, &mn) && cbor_map_get(m, mn, key, strlen(key), &v, &vn)) {
        cbor_r_init(&r, v, vn);
        if (cbor_read(&r, &it) && it.type == CBOR_TSTR && it.len < sizeof out[0]) {
            memcpy(o, it.ptr, it.len);
            o[it.len] = 0;
        } else if (it.type == CBOR_UINT) {
            snprintf(o, sizeof out[0], "%u", (unsigned)it.u);
        }
    }
    return o;
}

static bool has(const char *map)
{
    const uint8_t *v;
    size_t vn;
    return cbor_map_get(s_ans, s_ans_len, map, strlen(map), &v, &vn);
}

void setUp(void) {}
void tearDown(void) {}

static void test_hello_and_get_show_the_knobs(void)
{
    knobs(true);
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HELLO, NULL, 0));
    TEST_ASSERT_EQUAL_STRING("stock", sub_str("tuning", "gray_table"));
    TEST_ASSERT_EQUAL_STRING("3", sub_str("tuning", "gray_light"));
    TEST_ASSERT_EQUAL_STRING("4", sub_str("tuning", "passes_partial"));
    /* a set without tuning is a get */
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_SET, NULL, 0));
    TEST_ASSERT_EQUAL_STRING("0", sub_str("tuning", "gray_dark"));
    TEST_ASSERT_FALSE(has("refused"));
}

static void test_accepted_keys_apply_and_the_answer_reflects_them(void)
{
    knobs(true);
    const kv_t kv[] = {{"gray_table", 's', 0, "fastepd"}, {"gray_dark", 'u', 2, NULL}, {"gray_light", 'u', 0, NULL}};
    TEST_ASSERT_EQUAL_INT(0, set_tuning(kv, 3));
    TEST_ASSERT_EQUAL_INT(3, sets);
    TEST_ASSERT_EQUAL_STRING("fastepd", sub_str("tuning", "gray_table"));
    TEST_ASSERT_EQUAL_STRING("2", sub_str("tuning", "gray_dark"));
    TEST_ASSERT_EQUAL_STRING("0", sub_str("tuning", "gray_light"));
    TEST_ASSERT_FALSE(has("refused"));
    TEST_ASSERT_EQUAL_INT(20, map_int(s_ans, s_ans_len, "full_every", -1));
    TEST_ASSERT_EQUAL_INT(0, F.kv_sets); /* nothing is stored */
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HELLO, NULL, 0));
    TEST_ASSERT_EQUAL_STRING("fastepd", sub_str("tuning", "gray_table"));
    /* a restart of the device (the driver's state) is outside this fake: the values live in the driver, not in usbdisp */
}

static void test_bounds_and_kinds_are_refused_one_by_one(void)
{
    knobs(true);
    const kv_t kv[] = {{"gray_dark", 'u', 15, NULL}, {"gray_light", 'u', 9, NULL}, {"gray_table", 's', 0, "linear"},
                       {"passes_partial", 'u', 0, NULL}, {"gray_dark", 0, 0, NULL}};
    /* a duplicate key is no valid CBOR: use distinct ones */
    const kv_t good[] = {{"gray_dark", 'u', 15, NULL}, {"gray_light", 'u', 9, NULL}, {"gray_table", 's', 0, "linear"},
                         {"passes_partial", 'u', 0, NULL}};
    (void)kv;
    TEST_ASSERT_EQUAL_INT(0, set_tuning(good, 4));
    TEST_ASSERT_EQUAL_STRING("out of range; allowed 0 to 14", sub_str("refused", "gray_dark"));
    TEST_ASSERT_EQUAL_STRING("not one of the words; allowed fastepd, stock", sub_str("refused", "gray_table"));
    TEST_ASSERT_EQUAL_STRING("out of range; allowed 1 to 14", sub_str("refused", "passes_partial"));
    TEST_ASSERT_EQUAL_STRING("", sub_str("refused", "gray_light")); /* the good one is not listed ... */
    TEST_ASSERT_EQUAL_STRING("9", sub_str("tuning", "gray_light")); /* ... and applied */
    TEST_ASSERT_EQUAL_STRING("0", sub_str("tuning", "gray_dark")); /* the refused ones stay */
    TEST_ASSERT_EQUAL_STRING("stock", sub_str("tuning", "gray_table"));
    const kv_t bad_kind[] = {{"gray_dark", 0, 0, NULL}};
    TEST_ASSERT_EQUAL_INT(0, set_tuning(bad_kind, 1));
    TEST_ASSERT_EQUAL_STRING("the value is a number or a word", sub_str("refused", "gray_dark"));
    const kv_t word_for_number[] = {{"gray_dark", 's', 0, "two"}};
    TEST_ASSERT_EQUAL_INT(0, set_tuning(word_for_number, 1));
    TEST_ASSERT_EQUAL_STRING("not a whole number; allowed 0 to 14", sub_str("refused", "gray_dark"));
}

static void test_unknown_key_is_refused_with_the_known_ones(void)
{
    knobs(true);
    const kv_t kv[] = {{"gray_tabel", 'u', 1, NULL}, {"gray_dark", 'u', 5, NULL}};
    TEST_ASSERT_EQUAL_INT(0, set_tuning(kv, 2));
    const char *why = sub_str("refused", "gray_tabel");
    TEST_ASSERT_EQUAL_INT(0, strncmp(why, "no such key; known: gray_table gray_dark", 40));
    TEST_ASSERT_EQUAL_STRING("5", sub_str("tuning", "gray_dark"));
}

static void test_defaults_restore_every_knob(void)
{
    knobs(true);
    const kv_t kv[] = {{"gray_dark", 'u', 9, NULL}, {"passes_partial", 'u', 9, NULL}};
    TEST_ASSERT_EQUAL_INT(0, set_tuning(kv, 2));
    uint8_t p[32];
    cbor_w_t w;
    cbor_w_init(&w, p, sizeof p);
    cbor_put_map(&w, 1);
    cbor_put_str(&w, "tuning");
    cbor_put_str(&w, "defaults");
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_SET, p, w.len));
    TEST_ASSERT_EQUAL_STRING("0", sub_str("tuning", "gray_dark"));
    TEST_ASSERT_EQUAL_STRING("4", sub_str("tuning", "passes_partial"));
    TEST_ASSERT_FALSE(has("refused"));
    cbor_w_init(&w, p, sizeof p);
    cbor_put_map(&w, 1);
    cbor_put_str(&w, "tuning");
    cbor_put_str(&w, "everything");
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_SET, p, w.len));
    TEST_ASSERT_EQUAL_STRING("tuning is a map of keys, or \"defaults\"", sub_str("refused", "tuning"));
}

static void test_a_board_without_knobs_refuses_every_key(void)
{
    knobs(false);
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HELLO, NULL, 0));
    TEST_ASSERT_FALSE(has("tuning"));
    const kv_t kv[] = {{"gray_dark", 'u', 1, NULL}, {"clear", 's', 0, "fast"}};
    TEST_ASSERT_EQUAL_INT(0, set_tuning(kv, 2));
    TEST_ASSERT_EQUAL_STRING("this board has no panel tuning", sub_str("refused", "gray_dark"));
    TEST_ASSERT_EQUAL_STRING("this board has no panel tuning", sub_str("refused", "clear"));
    TEST_ASSERT_FALSE(has("tuning"));
    TEST_ASSERT_EQUAL_INT(0, sets);
    uint8_t p[32];
    cbor_w_t w;
    cbor_w_init(&w, p, sizeof p);
    cbor_put_map(&w, 1);
    cbor_put_str(&w, "tuning");
    cbor_put_str(&w, "defaults");
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_SET, p, w.len));
    TEST_ASSERT_EQUAL_STRING("this board has no panel tuning", sub_str("refused", "tuning"));
}

/* The scan keys of the X (step_neutral, row_skip, row_skip_us, scan_min_ms): hello shows defaults that leave the
 * scans as before, words and numbers are taken, a value outside is refused in words with its range and nothing
 * changes, "defaults" puts them back, and nothing is stored. */
static void test_scan_keys(void)
{
    knobs(true);
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HELLO, NULL, 0));
    TEST_ASSERT_EQUAL_STRING("each", sub_str("tuning", "step_neutral"));
    TEST_ASSERT_EQUAL_STRING("off", sub_str("tuning", "row_skip"));
    TEST_ASSERT_EQUAL_STRING("10", sub_str("tuning", "row_skip_us"));
    TEST_ASSERT_EQUAL_STRING("0", sub_str("tuning", "scan_min_ms"));
    TEST_ASSERT_TRUE(epd_scan_cfg_stock(&scan));
    const kv_t on[] = {{"step_neutral", 's', 0, "end"}, {"row_skip", 's', 0, "on"}, {"row_skip_us", 'u', 5, NULL},
                       {"scan_min_ms", 'u', 60, NULL}};
    TEST_ASSERT_EQUAL_INT(0, set_tuning(on, 4));
    TEST_ASSERT_FALSE(has("refused"));
    TEST_ASSERT_EQUAL_STRING("end", sub_str("tuning", "step_neutral"));
    TEST_ASSERT_EQUAL_STRING("on", sub_str("tuning", "row_skip"));
    TEST_ASSERT_EQUAL_STRING("5", sub_str("tuning", "row_skip_us"));
    TEST_ASSERT_EQUAL_STRING("60", sub_str("tuning", "scan_min_ms"));
    const epd_scan_cfg_t want = {EPD_SCAN_NEUTRAL_END, 1, 5, 60};
    TEST_ASSERT_EQUAL_MEMORY(&want, &scan, sizeof scan);
    TEST_ASSERT_EQUAL_INT(0, F.kv_sets); /* not stored */
    const kv_t bad[] = {{"step_neutral", 's', 0, "never"}, {"row_skip", 'u', 2, NULL}, {"row_skip_us", 'u', 1, NULL},
                        {"scan_min_ms", 'u', 151, NULL}};
    TEST_ASSERT_EQUAL_INT(0, set_tuning(bad, 4));
    TEST_ASSERT_EQUAL_STRING("not one of the words; allowed each, end", sub_str("refused", "step_neutral"));
    TEST_ASSERT_EQUAL_STRING("out of range; allowed off, on", sub_str("refused", "row_skip"));
    TEST_ASSERT_EQUAL_STRING("out of range; allowed 2 to 50", sub_str("refused", "row_skip_us"));
    TEST_ASSERT_EQUAL_STRING("out of range; allowed 0 to 150", sub_str("refused", "scan_min_ms"));
    TEST_ASSERT_EQUAL_MEMORY(&want, &scan, sizeof scan);
    const kv_t top[] = {{"row_skip_us", 'u', 51, NULL}};
    TEST_ASSERT_EQUAL_INT(0, set_tuning(top, 1));
    TEST_ASSERT_EQUAL_STRING("out of range; allowed 2 to 50", sub_str("refused", "row_skip_us"));
    uint8_t p[32];
    cbor_w_t w;
    cbor_w_init(&w, p, sizeof p);
    cbor_put_map(&w, 1);
    cbor_put_str(&w, "tuning");
    cbor_put_str(&w, "defaults");
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_SET, p, w.len));
    TEST_ASSERT_EQUAL_STRING("each", sub_str("tuning", "step_neutral"));
    TEST_ASSERT_EQUAL_STRING("off", sub_str("tuning", "row_skip"));
    TEST_ASSERT_TRUE(epd_scan_cfg_stock(&scan));
    /* another board: each key refused in words */
    knobs(false);
    TEST_ASSERT_EQUAL_INT(0, set_tuning(on, 4));
    TEST_ASSERT_EQUAL_STRING("this board has no panel tuning", sub_str("refused", "step_neutral"));
    TEST_ASSERT_EQUAL_STRING("this board has no panel tuning", sub_str("refused", "row_skip"));
    TEST_ASSERT_EQUAL_STRING("this board has no panel tuning", sub_str("refused", "row_skip_us"));
    TEST_ASSERT_EQUAL_STRING("this board has no panel tuning", sub_str("refused", "scan_min_ms"));
    TEST_ASSERT_TRUE(epd_scan_cfg_stock(&scan));
}

/* gray_row: rows of the gray table in use as text. hello shows the changed rows; a row is taken; a wrong length names
 * the number the table has; a bad character and a bad level are named; defaults and a table switch put the rows back;
 * nothing is stored. */
static void test_gray_row(void)
{
    knobs(true);
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HELLO, NULL, 0));
    TEST_ASSERT_TRUE(has("tuning"));
    TEST_ASSERT_EQUAL_STRING("", sub_str("tuning", "gray_row"));
    TEST_ASSERT_EQUAL_STRING("stock", sub_str("tuning", "gray_table"));
    const kv_t one[] = {{"gray_row", 's', 0, "5:..DDLLDDL"}};
    TEST_ASSERT_EQUAL_INT(0, set_tuning(one, 1));
    TEST_ASSERT_FALSE(has("refused"));
    TEST_ASSERT_EQUAL_STRING("5:..DDLLDDL", sub_str("tuning", "gray_row"));
    const kv_t more[] = {{"gray_row", 's', 0, "6:.DDDLLDDL, 14:.DDDLLLLD"}};
    TEST_ASSERT_EQUAL_INT(0, set_tuning(more, 1)); /* sent again: the rows add up */
    TEST_ASSERT_EQUAL_STRING("5:..DDLLDDL 6:.DDDLLDDL 14:.DDDLLLLD", sub_str("tuning", "gray_row"));
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HELLO, NULL, 0));
    TEST_ASSERT_EQUAL_STRING("5:..DDLLDDL 6:.DDDLLDDL 14:.DDDLLLLD", sub_str("tuning", "gray_row"));
    const uint8_t codes[9] = {0, 1, 1, 1, 2, 2, 1, 1, 2};
    TEST_ASSERT_EQUAL_MEMORY(codes, tab.m + 6 * 9, 9);
    TEST_ASSERT_EQUAL_INT(0, F.kv_sets); /* not stored */
    /* refusals in words; the rows stay as they were */
    const kv_t len[] = {{"gray_row", 's', 0, "5:..DDLLDD"}};
    TEST_ASSERT_EQUAL_INT(0, set_tuning(len, 1));
    TEST_ASSERT_EQUAL_STRING("level 5: 8 passes given, the stock table has 9", sub_str("refused", "gray_row"));
    const kv_t chr[] = {{"gray_row", 's', 0, "5:..DDLLDDX"}};
    TEST_ASSERT_EQUAL_INT(0, set_tuning(chr, 1));
    TEST_ASSERT_EQUAL_STRING("level 5: character 9 is not one of . (no drive) D (dark) L (light)", sub_str("refused", "gray_row"));
    const kv_t lvl[] = {{"gray_row", 's', 0, "16:..DDLLDDL"}};
    TEST_ASSERT_EQUAL_INT(0, set_tuning(lvl, 1));
    TEST_ASSERT_EQUAL_STRING("level 16: the levels are 0 (black) to 15 (white)", sub_str("refused", "gray_row"));
    const kv_t num[] = {{"gray_row", 'u', 5, NULL}, {"gray_dark", 'u', 3, NULL}};
    TEST_ASSERT_EQUAL_INT(0, set_tuning(num, 2));
    TEST_ASSERT_EQUAL_STRING("the value is text, level:passes, for example 5:..DDLLDDL", sub_str("refused", "gray_row"));
    TEST_ASSERT_EQUAL_STRING("3", sub_str("tuning", "gray_dark")); /* the key after it still applies */
    TEST_ASSERT_EQUAL_STRING("5:..DDLLDDL 6:.DDDLLDDL 14:.DDDLLLLD", sub_str("tuning", "gray_row"));
    /* a table switch puts the rows back; rows sent with it are for the new table, wherever the key stands */
    const kv_t sw[] = {{"gray_row", 's', 0, "5:LLLLDLDL"}, {"gray_table", 's', 0, "fastepd"}};
    TEST_ASSERT_EQUAL_INT(0, set_tuning(sw, 2));
    TEST_ASSERT_FALSE(has("refused"));
    TEST_ASSERT_EQUAL_STRING("fastepd", sub_str("tuning", "gray_table"));
    TEST_ASSERT_EQUAL_STRING("5:LLLLDLDL", sub_str("tuning", "gray_row"));
    const kv_t nine[] = {{"gray_row", 's', 0, "5:..DDLLDDL"}};
    TEST_ASSERT_EQUAL_INT(0, set_tuning(nine, 1));
    TEST_ASSERT_EQUAL_STRING("level 5: 9 passes given, the fastepd table has 8", sub_str("refused", "gray_row"));
    const kv_t back[] = {{"gray_table", 's', 0, "stock"}};
    TEST_ASSERT_EQUAL_INT(0, set_tuning(back, 1));
    TEST_ASSERT_EQUAL_STRING("", sub_str("tuning", "gray_row"));
    int np;
    TEST_ASSERT_EQUAL_MEMORY(epd_graytab_own(EPD_GRAYTAB_STOCK, &np), tab.m, 16 * 9);
    /* the same table named again keeps the rows */
    TEST_ASSERT_EQUAL_INT(0, set_tuning(one, 1));
    TEST_ASSERT_EQUAL_INT(0, set_tuning(back, 1));
    TEST_ASSERT_EQUAL_STRING("5:..DDLLDDL", sub_str("tuning", "gray_row"));
    /* an empty text, and "defaults", put them back */
    const kv_t none[] = {{"gray_row", 's', 0, ""}};
    TEST_ASSERT_EQUAL_INT(0, set_tuning(none, 1));
    TEST_ASSERT_EQUAL_STRING("", sub_str("tuning", "gray_row"));
    TEST_ASSERT_EQUAL_INT(0, set_tuning(one, 1));
    uint8_t p[32];
    cbor_w_t w;
    cbor_w_init(&w, p, sizeof p);
    cbor_put_map(&w, 1);
    cbor_put_str(&w, "tuning");
    cbor_put_str(&w, "defaults");
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_SET, p, w.len));
    TEST_ASSERT_EQUAL_STRING("", sub_str("tuning", "gray_row"));
    TEST_ASSERT_EQUAL_MEMORY(epd_graytab_own(EPD_GRAYTAB_STOCK, &np), tab.m, 16 * 9);
    /* every row changed still fits hello */
    char all[EPD_GRAYTAB_TEXT_MAX];
    size_t o = 0;
    for (int l = 0; l < 16; l++)
        o += (size_t)snprintf(all + o, sizeof all - o, "%s%d:LDLDLDLDL", l ? " " : "", l);
    const kv_t full[] = {{"gray_row", 's', 0, all}};
    TEST_ASSERT_EQUAL_INT(0, set_tuning(full, 1));
    TEST_ASSERT_EQUAL_UINT16(0xffff, tab.changed);
    TEST_ASSERT_EQUAL_INT(0, req_err(USBDISP_CMD_HELLO, NULL, 0));
    const uint8_t *v;
    size_t vn;
    TEST_ASSERT_TRUE(cbor_map_get(s_ans, s_ans_len, "tuning", 6, &v, &vn));
    TEST_ASSERT_TRUE(vn > strlen(all));
    /* another board: refused in words */
    knobs(false);
    TEST_ASSERT_EQUAL_INT(0, set_tuning(one, 1));
    TEST_ASSERT_EQUAL_STRING("this board has no panel tuning", sub_str("refused", "gray_row"));
}

static void test_more_refusals_than_room_are_cut_not_overrun(void)
{
    knobs(true);
    kv_t kv[12];
    static char names[12][8];
    for (int i = 0; i < 12; i++) {
        snprintf(names[i], sizeof names[i], "k%d", i);
        kv[i] = (kv_t){names[i], 'u', 1, NULL};
    }
    TEST_ASSERT_EQUAL_INT(0, set_tuning(kv, 12));
    TEST_ASSERT_NOT_EQUAL(0, strlen(sub_str("refused", "k0")));
    TEST_ASSERT_EQUAL_STRING("", sub_str("refused", "k11"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hello_and_get_show_the_knobs);
    RUN_TEST(test_accepted_keys_apply_and_the_answer_reflects_them);
    RUN_TEST(test_bounds_and_kinds_are_refused_one_by_one);
    RUN_TEST(test_unknown_key_is_refused_with_the_known_ones);
    RUN_TEST(test_defaults_restore_every_knob);
    RUN_TEST(test_a_board_without_knobs_refuses_every_key);
    RUN_TEST(test_scan_keys);
    RUN_TEST(test_gray_row);
    RUN_TEST(test_more_refusals_than_room_are_cut_not_overrun);
    return UNITY_END();
}
