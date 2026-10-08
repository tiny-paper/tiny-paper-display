/* See epd_graytab.h. C11. SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_graytab.h"

#include <stdio.h>
#include <string.h>

/* FastEPD 8dc8c74 u8GrayMatrix (Apache-2.0), 16 grays x 8 passes: FastEPD's default for BB_PANEL_TRMNL_X.
 * Kept here because FastEPD has no call to go back to it after setCustomMatrix. */
static const uint8_t FASTEPD_TAB[] = {
    2, 2, 1, 1, 1, 1, 1, 1, /**/ 2, 2, 2, 2, 1, 1, 2, 1, /**/ 2, 2, 2, 1, 1, 1, 1, 2, /**/ 2, 2, 2, 1, 1, 1, 1, 2,
    2, 2, 2, 2, 1, 1, 1, 2, /**/ 2, 2, 2, 2, 1, 2, 2, 1, /**/ 2, 2, 1, 1, 1, 2, 1, 2, /**/ 2, 2, 2, 1, 1, 2, 1, 2,
    1, 1, 1, 1, 1, 1, 2, 2, /**/ 2, 1, 1, 1, 1, 1, 2, 2, /**/ 2, 2, 1, 1, 1, 1, 2, 2, /**/ 2, 2, 2, 1, 1, 1, 2, 2,
    2, 1, 1, 2, 1, 1, 2, 2, /**/ 2, 2, 2, 2, 1, 1, 2, 2, /**/ 2, 2, 2, 2, 2, 1, 2, 2, /**/ 2, 2, 2, 2, 2, 2, 2, 2,
};
/* Stock trmnl-firmware f5f87b7 src/display.cpp u8_graytable (GPL-3.0), 16 grays x 9 passes. */
static const uint8_t STOCK_TAB[] = {
    0, 0, 0, 0, 0, 0, 1, 1, 1, /**/ 0, 0, 1, 1, 1, 2, 2, 1, 1, /**/ 0, 0, 0, 0, 1, 2, 2, 1, 1,
    1, 1, 2, 2, 1, 1, 1, 1, 2, /**/ 0, 0, 0, 1, 2, 1, 1, 1, 2, /**/ 1, 2, 2, 2, 2, 1, 1, 1, 2,
    0, 0, 1, 1, 2, 2, 1, 1, 2, /**/ 0, 1, 1, 2, 1, 1, 2, 1, 2, /**/ 0, 1, 1, 1, 2, 1, 2, 1, 2,
    0, 1, 1, 1, 1, 2, 2, 1, 2, /**/ 1, 1, 1, 2, 1, 1, 1, 2, 2, /**/ 0, 0, 1, 2, 1, 1, 1, 2, 2,
    0, 0, 0, 1, 2, 1, 1, 2, 2, /**/ 0, 0, 0, 0, 1, 2, 1, 2, 2, /**/ 0, 1, 1, 1, 2, 2, 2, 2, 2,
    0, 0, 0, 0, 0, 0, 0, 0, 2,
};
_Static_assert(sizeof FASTEPD_TAB == 16 * 8 && sizeof STOCK_TAB == 16 * 9, "gray tables");

static const char CODES[] = ".DL";

const uint8_t *epd_graytab_own(int table, int *passes)
{
    if (table == EPD_GRAYTAB_FASTEPD) {
        *passes = 8;
        return FASTEPD_TAB;
    }
    if (table == EPD_GRAYTAB_STOCK) {
        *passes = 9;
        return STOCK_TAB;
    }
    *passes = 0;
    return NULL;
}

const char *epd_graytab_name(int table) { return table == EPD_GRAYTAB_STOCK ? "stock" : "fastepd"; }

void epd_graytab_select(epd_graytab_t *t, int table)
{
    int np;
    const uint8_t *own = epd_graytab_own(table, &np);
    if (!own)
        own = epd_graytab_own(table = EPD_GRAYTAB_STOCK, &np);
    memset(t, 0, sizeof *t);
    t->table = (uint8_t)table;
    t->passes = (uint8_t)np;
    memcpy(t->m, own, (size_t)np * EPD_GRAYTAB_LEVELS);
}

static bool is_sep(char c) { return c == ' ' || c == ',' || c == '\t' || c == '\n' || c == '\r'; }

bool epd_graytab_set_rows(epd_graytab_t *t, const char *s, size_t len, char *why, size_t cap)
{
    epd_graytab_t n = *t;
    int np;
    const uint8_t *own = epd_graytab_own(t->table, &np);
    size_t i = 0;
    int entries = 0;
    if (!own || np != t->passes)
        return false;
    for (;;) {
        while (i < len && is_sep(s[i]))
            i++;
        if (i >= len)
            break;
        int level = 0, digits = 0;
        while (i < len && s[i] >= '0' && s[i] <= '9' && digits < 4) {
            level = level * 10 + (s[i++] - '0');
            digits++;
        }
        if (!digits || i >= len || s[i] != ':') {
            snprintf(why, cap, "entry %d is not level:passes, for example 5:%.*s", entries + 1, np, "..DDLLDDL");
            return false;
        }
        i++;
        if (level >= EPD_GRAYTAB_LEVELS) {
            snprintf(why, cap, "level %d: the levels are 0 (black) to 15 (white)", level);
            return false;
        }
        size_t k = 0;
        uint8_t row[EPD_GRAYTAB_PASSES_MAX];
        while (i < len && !is_sep(s[i])) {
            const char *c = memchr(CODES, s[i], 3);
            if (!c) {
                snprintf(why, cap, "level %d: character %u is not one of . (no drive) D (dark) L (light)", level,
                         (unsigned)k + 1);
                return false;
            }
            if (k < sizeof row)
                row[k] = (uint8_t)(c - CODES);
            k++;
            i++;
        }
        if (k != (size_t)np) {
            snprintf(why, cap, "level %d: %u passes given, the %s table has %d", level, (unsigned)k,
                     epd_graytab_name(t->table), np);
            return false;
        }
        memcpy(n.m + level * np, row, (size_t)np);
        if (memcmp(row, own + level * np, (size_t)np))
            n.changed |= (uint16_t)(1u << level);
        else
            n.changed &= (uint16_t)~(1u << level);
        entries++;
    }
    if (!entries)
        epd_graytab_select(&n, t->table);
    *t = n;
    return true;
}

char *epd_graytab_row_str(const uint8_t *m, int passes, int level, char *b)
{
    for (int j = 0; j < passes; j++)
        b[j] = CODES[m[level * passes + j] <= 2 ? m[level * passes + j] : 0];
    b[passes] = 0;
    return b;
}

char *epd_graytab_rows_str(const epd_graytab_t *t, char *b, size_t n)
{
    size_t o = 0;
    if (!n)
        return b;
    b[0] = 0;
    for (int l = 0; l < EPD_GRAYTAB_LEVELS; l++) {
        char row[EPD_GRAYTAB_PASSES_MAX + 1];
        if (!(t->changed >> l & 1))
            continue;
        int k = snprintf(b + o, n - o, "%s%d:%s", o ? " " : "", l, epd_graytab_row_str(t->m, t->passes, l, row));
        if (k < 0 || (size_t)k >= n - o) {
            b[o] = 0;
            break;
        }
        o += (size_t)k;
    }
    return b;
}

char *epd_graytab_mark(const epd_graytab_t *t, char *b, size_t n)
{
    size_t o = 0;
    if (!n)
        return b;
    b[0] = 0;
    for (int l = 0; l < EPD_GRAYTAB_LEVELS; l++) {
        if (!(t->changed >> l & 1))
            continue;
        int k = snprintf(b + o, n - o, o ? ",%d" : " rows*=%d", l);
        if (k < 0 || (size_t)k >= n - o) {
            b[o] = 0;
            break;
        }
        o += (size_t)k;
    }
    return b;
}
