/* The tuning key `driver` (epd_driver.h): its words and default, and that under `plain` nothing of our own panel
 * driving is installed: no part of it, the library's own row control, the library's own bus clock, no gray partial.
 * The glue (esp/epd_fastepd.cpp) installs what these functions return and nothing else; that it asks them is read
 * from its source. SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>

#include "epd_driver.h"
#include "epd_par.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void ours_rows(void *s, int t) { (void)s, (void)t; }
static void lib_rows(void *s, int t) { (void)s, (void)t; }

static void test_words_and_default(void)
{
    TEST_ASSERT_EQUAL_STRING("driver", EPD_DRIVER_KEY);
    TEST_ASSERT_EQUAL_INT(2, EPD_DRIVER_COUNT);
    TEST_ASSERT_EQUAL_STRING("ours", EPD_DRIVER_WORDS[EPD_DRIVER_OURS]);
    TEST_ASSERT_EQUAL_STRING("plain", EPD_DRIVER_WORDS[EPD_DRIVER_PLAIN]);
    TEST_ASSERT_EQUAL_INT(EPD_DRIVER_OURS, EPD_DRIVER_DEFAULT);
    TEST_ASSERT_TRUE(epd_driver_valid(EPD_DRIVER_OURS));
    TEST_ASSERT_TRUE(epd_driver_valid(EPD_DRIVER_PLAIN));
    TEST_ASSERT_FALSE(epd_driver_valid(-1));
    TEST_ASSERT_FALSE(epd_driver_valid(EPD_DRIVER_COUNT));
    TEST_ASSERT_EQUAL_STRING("plain", epd_driver_name(EPD_DRIVER_PLAIN));
    TEST_ASSERT_EQUAL_STRING("?", epd_driver_name(7));
}

/* every part, whatever is added to the struct later: all of its bytes are zero under plain */
static void test_plain_installs_nothing_of_ours(void)
{
    static const epd_driver_parts_t none;
    const int not_ours[] = {EPD_DRIVER_PLAIN, -1, EPD_DRIVER_COUNT, 99};
    for (size_t i = 0; i < sizeof not_ours / sizeof not_ours[0]; i++) {
        const int d = not_ours[i];
        epd_driver_parts_t p = epd_driver_parts(d);
        TEST_ASSERT_FALSE(p.row_hook);
        TEST_ASSERT_FALSE(p.bus);
        TEST_ASSERT_FALSE(p.sequencer);
        TEST_ASSERT_FALSE(p.wave);
        TEST_ASSERT_FALSE(p.redrive);
        TEST_ASSERT_FALSE(p.rails);
        TEST_ASSERT_FALSE(p.gray_partial);
        TEST_ASSERT_EQUAL_MEMORY(&none, &p, sizeof p);
        TEST_ASSERT_TRUE(epd_driver_rows(d, ours_rows, lib_rows) == lib_rows);
        TEST_ASSERT_EQUAL_UINT32(20000000u, epd_driver_bus_hz(d, EPDP_BUS_HW_CS, 20000000u));
        TEST_ASSERT_EQUAL_HEX8(1, epd_driver_partial_depths(d, 7));
        TEST_ASSERT_EQUAL_HEX8(0, epd_driver_partial_depths(d, 6));
    }
}

static void test_ours_is_as_before(void)
{
    epd_driver_parts_t p = epd_driver_parts(EPD_DRIVER_OURS);
    TEST_ASSERT_TRUE(p.row_hook && p.bus && p.sequencer && p.wave && p.redrive && p.rails && p.gray_partial);
    TEST_ASSERT_TRUE(epd_driver_rows(EPD_DRIVER_OURS, ours_rows, lib_rows) == ours_rows);
    TEST_ASSERT_EQUAL_UINT32(EPDP_BUS_HW_CS, epd_driver_bus_hz(EPD_DRIVER_OURS, EPDP_BUS_HW_CS, 20000000u));
    /* "!tp bus stock": no speed of ours, the library's bus */
    TEST_ASSERT_EQUAL_UINT32(20000000u, epd_driver_bus_hz(EPD_DRIVER_OURS, 0, 20000000u));
    TEST_ASSERT_EQUAL_HEX8(7, epd_driver_partial_depths(EPD_DRIVER_OURS, 7));
}

static void test_scans_of_a_library_call(void)
{
    TEST_ASSERT_EQUAL_UINT32(5, epd_driver_lib_scans(true, 0, 4));  /* 4 passes and the neutral scan */
    TEST_ASSERT_EQUAL_UINT32(4, epd_driver_lib_scans(true, 9, 3));  /* clear is not looked at */
    TEST_ASSERT_EQUAL_UINT32(5, epd_driver_lib_scans(false, EPDP_CLEAR_NONE, 3));
    TEST_ASSERT_EQUAL_UINT32(21, epd_driver_lib_scans(false, EPDP_CLEAR_FAST, 3));
    TEST_ASSERT_EQUAL_UINT32(43, epd_driver_lib_scans(false, EPDP_CLEAR_SLOW, 9)); /* PLAN: 43 scans seen in a gray full */
    TEST_ASSERT_EQUAL_UINT32(13, epd_driver_lib_scans(false, EPDP_CLEAR_WHITE, 3));
    TEST_ASSERT_EQUAL_UINT32(0, epd_driver_lib_scans(false, 4, 3));
    TEST_ASSERT_EQUAL_UINT32(0, epd_driver_lib_scans(false, -1, 3));
    TEST_ASSERT_EQUAL_UINT32(0, epd_driver_lib_scans(true, 0, 0));
}

/* The glue takes every choice from epd_driver.h: read from its source (it is not built on the host). Under plain it
 * may reach our row control, our bus speed, the sequencer and the wave steps only through these calls, and its
 * plain paths call the library's two public updates. */
static char *slurp(const char *path)
{
    static char buf[200000];
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    TEST_ASSERT_TRUE_MESSAGE(n < sizeof buf - 1, "source larger than the buffer");
    return buf;
}

static int count(const char *s, const char *needle)
{
    int n = 0;
    for (const char *p = s; (p = strstr(p, needle)); p += strlen(needle))
        n++;
    return n;
}

static void test_the_glue_asks_before_it_installs(void)
{
    const char *s = slurp(TINY_ROOT "/firmware/components/epd/esp/epd_fastepd.cpp");
    /* our row control reaches the library through one place, and that place asks */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count(s, "s_epd.hook_rows(epd_driver_rows(d, row_ctl, FastEpdX::s_lib_rows))"), "row hook");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count(s, "hook_rows(") - count(s, "void hook_rows("), "a second place installs a row control");
    /* the bus: one place decides its clock, and that place asks */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count(s, "epd_driver_bus_hz(d, s_bus_hz, 0)"), "bus clock");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count(s, "s_epd.rebus("), "a second place sets the bus up again");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(s, "int rc = want ? s_epd.rebus(want) : s_epd.relib();"), "the bus follows the answer");
    /* each update leaves for the plain path before any of our driving */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count(s, "if (!s_parts.wave) return plain_partial(y0, y1);"), "1 bit partial");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count(s, "if (!s_parts.gray_partial) return EPDP_E_STATE;"), "gray partial");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, count(s, "if (!s_parts.row_hook) return plain_full(modes[clear_mode], clear_mode);"), "full");
    /* and the plain paths are one public call each */
    const char *a = strstr(s, "/* plain: begin */"), *b = strstr(s, "/* plain: end */");
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_TRUE(a < b);
    static char plain[20000];
    TEST_ASSERT_TRUE((size_t)(b - a) < sizeof plain);
    memcpy(plain, a, (size_t)(b - a));
    plain[b - a] = 0;
    TEST_ASSERT_EQUAL_INT(1, count(plain, "s_epd.fullUpdate("));
    TEST_ASSERT_EQUAL_INT(1, count(plain, "s_epd.partialUpdate("));
    TEST_ASSERT_EQUAL_INT(1, count(plain, "s_epd.partialUpdate(keep, y0, y1)")); /* the changed rows, the library's keep-on */
    static const char *const ours[] = {"scan_partial", "scan_step", "scan_run", "scan_begin", "rows_begin",
                                       "epd_parjob_begin", "epd_parjob_step", "setPasses", "rebus",
                                       "s_seq_on = true", "s_rows_on = true", "s_drive_us ="};
    for (size_t i = 0; i < sizeof ours / sizeof ours[0]; i++)
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, count(plain, ours[i]), ours[i]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_words_and_default);
    RUN_TEST(test_plain_installs_nothing_of_ours);
    RUN_TEST(test_ours_is_as_before);
    RUN_TEST(test_scans_of_a_library_call);
    RUN_TEST(test_the_glue_asks_before_it_installs);
    return UNITY_END();
}
