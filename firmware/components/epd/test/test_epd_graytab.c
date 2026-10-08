/* The gray tables of the TRMNL X and their correction row by row (epd_graytab.h), and that the spec prints the
 * tables as they are. SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "epd_graytab.h"
#include "unity.h"

static epd_graytab_t T;
static char why[128];

static bool set(const char *s) { return epd_graytab_set_rows(&T, s, strlen(s), why, sizeof why); }

void setUp(void)
{
    epd_graytab_select(&T, EPD_GRAYTAB_STOCK);
    why[0] = 0;
}
void tearDown(void) {}

static void test_the_tables_as_they_come(void)
{
    int np;
    char b[EPD_GRAYTAB_PASSES_MAX + 1];
    const uint8_t *m = epd_graytab_own(EPD_GRAYTAB_STOCK, &np);
    TEST_ASSERT_EQUAL_INT(9, np);
    TEST_ASSERT_EQUAL_STRING("......DDD", epd_graytab_row_str(m, np, 0, b));
    TEST_ASSERT_EQUAL_STRING("DLLLLDDDL", epd_graytab_row_str(m, np, 5, b));
    TEST_ASSERT_EQUAL_STRING("..DDLLDDL", epd_graytab_row_str(m, np, 6, b));
    TEST_ASSERT_EQUAL_STRING("........L", epd_graytab_row_str(m, np, 15, b));
    m = epd_graytab_own(EPD_GRAYTAB_FASTEPD, &np);
    TEST_ASSERT_EQUAL_INT(8, np);
    TEST_ASSERT_EQUAL_STRING("LLLLDLLD", epd_graytab_row_str(m, np, 5, b));
    TEST_ASSERT_EQUAL_STRING("LLLLLLLL", epd_graytab_row_str(m, np, 15, b));
    TEST_ASSERT_NULL(epd_graytab_own(2, &np));
    TEST_ASSERT_EQUAL_STRING("stock", epd_graytab_name(EPD_GRAYTAB_STOCK));
    TEST_ASSERT_EQUAL_STRING("fastepd", epd_graytab_name(EPD_GRAYTAB_FASTEPD));
    TEST_ASSERT_EQUAL_UINT8(9, T.passes);
    TEST_ASSERT_EQUAL_UINT16(0, T.changed);
    TEST_ASSERT_EQUAL_MEMORY(epd_graytab_own(EPD_GRAYTAB_STOCK, &np), T.m, 16 * 9);
}

static void test_a_row_is_replaced(void)
{
    char b[EPD_GRAYTAB_TEXT_MAX], r[EPD_GRAYTAB_PASSES_MAX + 1];
    int np;
    const uint8_t *own = epd_graytab_own(EPD_GRAYTAB_STOCK, &np);
    TEST_ASSERT_TRUE(set("5:..DDLLDDL"));
    TEST_ASSERT_EQUAL_STRING("..DDLLDDL", epd_graytab_row_str(T.m, T.passes, 5, r));
    const uint8_t codes[9] = {0, 0, 1, 1, 2, 2, 1, 1, 2};
    TEST_ASSERT_EQUAL_MEMORY(codes, T.m + 5 * 9, 9); /* . no drive, D dark (1), L light (2) */
    TEST_ASSERT_EQUAL_MEMORY(own, T.m, 5 * 9);       /* the other rows untouched */
    TEST_ASSERT_EQUAL_MEMORY(own + 6 * 9, T.m + 6 * 9, 10 * 9);
    TEST_ASSERT_EQUAL_STRING("5:..DDLLDDL", epd_graytab_rows_str(&T, b, sizeof b));
    TEST_ASSERT_EQUAL_STRING(" rows*=5", epd_graytab_mark(&T, b, sizeof b));
    /* sent again with another row: both stay; a list in one text, spaces or commas */
    TEST_ASSERT_TRUE(set("6:.DDDLLDDL"));
    TEST_ASSERT_EQUAL_STRING("5:..DDLLDDL 6:.DDDLLDDL", epd_graytab_rows_str(&T, b, sizeof b));
    TEST_ASSERT_TRUE(set(" 0:DDDDDDDDD, 15:LLLLLLLLL  "));
    TEST_ASSERT_EQUAL_STRING("0:DDDDDDDDD 5:..DDLLDDL 6:.DDDLLDDL 15:LLLLLLLLL", epd_graytab_rows_str(&T, b, sizeof b));
    TEST_ASSERT_EQUAL_STRING(" rows*=0,5,6,15", epd_graytab_mark(&T, b, sizeof b));
    /* a row sent as the table has it is no changed row */
    TEST_ASSERT_TRUE(set("5:DLLLLDDDL"));
    TEST_ASSERT_EQUAL_STRING("0:DDDDDDDDD 6:.DDDLLDDL 15:LLLLLLLLL", epd_graytab_rows_str(&T, b, sizeof b));
    /* an empty text: the table's own rows */
    TEST_ASSERT_TRUE(set(""));
    TEST_ASSERT_EQUAL_UINT16(0, T.changed);
    TEST_ASSERT_EQUAL_MEMORY(own, T.m, 16 * 9);
    TEST_ASSERT_EQUAL_STRING("", epd_graytab_rows_str(&T, b, sizeof b));
    TEST_ASSERT_EQUAL_STRING("", epd_graytab_mark(&T, b, sizeof b));
}

static void refused(const char *s, const char *words)
{
    epd_graytab_t before = T;
    TEST_ASSERT_FALSE_MESSAGE(set(s), s);
    TEST_ASSERT_EQUAL_STRING(words, why);
    TEST_ASSERT_TRUE(strlen(why) <= 100);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&before, &T, sizeof T, "a refused text changed the table");
}

static void test_refusals_in_words_and_nothing_changes(void)
{
    TEST_ASSERT_TRUE(set("6:.DDDLLDDL"));
    refused("5:..DDLLDD", "level 5: 8 passes given, the stock table has 9");
    refused("5:..DDLLDDLL", "level 5: 10 passes given, the stock table has 9");
    refused("5:", "level 5: 0 passes given, the stock table has 9");
    refused("5:..DDxLDDL", "level 5: character 5 is not one of . (no drive) D (dark) L (light)");
    refused("5:..ddllddl", "level 5: character 3 is not one of . (no drive) D (dark) L (light)");
    refused("16:..DDLLDDL", "level 16: the levels are 0 (black) to 15 (white)");
    refused("99999:..DDLLDDL", "entry 1 is not level:passes, for example 5:..DDLLDDL");
    refused("-1:..DDLLDDL", "entry 1 is not level:passes, for example 5:..DDLLDDL");
    refused("..DDLLDDL", "entry 1 is not level:passes, for example 5:..DDLLDDL");
    refused("5 ..DDLLDDL", "entry 1 is not level:passes, for example 5:..DDLLDDL");
    /* a good entry before a bad one is not applied either */
    refused("4:DDDDDDDDD 5:..DDLLDD", "level 5: 8 passes given, the stock table has 9");
    refused("4:DDDDDDDDD five", "entry 2 is not level:passes, for example 5:..DDLLDDL");
    epd_graytab_select(&T, EPD_GRAYTAB_FASTEPD);
    refused("5:..DDLLDDL", "level 5: 9 passes given, the fastepd table has 8");
    refused("x", "entry 1 is not level:passes, for example 5:..DDLLDD");
    TEST_ASSERT_TRUE(set("5:LLLLDLDL"));
}

static void test_a_table_switch_puts_the_rows_back(void)
{
    int np;
    char b[EPD_GRAYTAB_TEXT_MAX];
    TEST_ASSERT_TRUE(set("5:..DDLLDDL 6:.DDDLLDDL"));
    epd_graytab_select(&T, EPD_GRAYTAB_FASTEPD);
    TEST_ASSERT_EQUAL_UINT8(8, T.passes);
    TEST_ASSERT_EQUAL_UINT16(0, T.changed);
    TEST_ASSERT_EQUAL_MEMORY(epd_graytab_own(EPD_GRAYTAB_FASTEPD, &np), T.m, 16 * 8);
    epd_graytab_select(&T, EPD_GRAYTAB_STOCK); /* and back: the stock table's own rows, not the changed ones */
    TEST_ASSERT_EQUAL_MEMORY(epd_graytab_own(EPD_GRAYTAB_STOCK, &np), T.m, 16 * 9);
    TEST_ASSERT_EQUAL_STRING("", epd_graytab_rows_str(&T, b, sizeof b));
    epd_graytab_select(&T, 7); /* no such table: the stock one */
    TEST_ASSERT_EQUAL_UINT8(EPD_GRAYTAB_STOCK, T.table);
}

static void test_every_row_changed_fits_the_text(void)
{
    char all[EPD_GRAYTAB_TEXT_MAX], b[EPD_GRAYTAB_TEXT_MAX], small[20];
    size_t o = 0;
    for (int l = 0; l < 16; l++)
        o += (size_t)snprintf(all + o, sizeof all - o, "%s%d:LDLDLDLDL", l ? " " : "", l);
    TEST_ASSERT_TRUE(set(all));
    TEST_ASSERT_EQUAL_UINT16(0xffff, T.changed);
    TEST_ASSERT_EQUAL_STRING(all, epd_graytab_rows_str(&T, b, sizeof b));
    /* a short buffer: whole entries only */
    TEST_ASSERT_EQUAL_STRING("0:LDLDLDLDL", epd_graytab_rows_str(&T, small, sizeof small));
    TEST_ASSERT_EQUAL_STRING(" rows*=0,1,2,3,4,5", epd_graytab_mark(&T, small, sizeof small));
}

/* docs/spec/usb-display.md prints both tables: every row there is the row here. */
static void test_the_spec_prints_the_tables(void)
{
    FILE *f = fopen(TINY_ROOT "/docs/spec/usb-display.md", "rb");
    TEST_ASSERT_NOT_NULL(f);
    static char spec[65536];
    size_t n = fread(spec, 1, sizeof spec - 1, f);
    fclose(f);
    spec[n] = 0;
    TEST_ASSERT_TRUE(n < sizeof spec - 1);
    int ns, nf;
    const uint8_t *s = epd_graytab_own(EPD_GRAYTAB_STOCK, &ns), *e = epd_graytab_own(EPD_GRAYTAB_FASTEPD, &nf);
    for (int l = 0; l < 16; l++) {
        char line[64], a[10], b[10];
        snprintf(line, sizeof line, "| %d | `%s` | `%s` |", l, epd_graytab_row_str(s, ns, l, a), epd_graytab_row_str(e, nf, l, b));
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(spec, line), line);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_tables_as_they_come);
    RUN_TEST(test_a_row_is_replaced);
    RUN_TEST(test_refusals_in_words_and_nothing_changes);
    RUN_TEST(test_a_table_switch_puts_the_rows_back);
    RUN_TEST(test_every_row_changed_fits_the_text);
    RUN_TEST(test_the_spec_prints_the_tables);
    return UNITY_END();
}
