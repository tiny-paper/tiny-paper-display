/*
 * epd_graytab.h - the 16 gray drive table of the TRMNL X (hal_esp panel_par.c) and its correction row by row.
 * Hardware-free, host-tested.
 *
 * A table has one row per gray level (0 black .. 15 white) and one column per pass; a full refresh drives every pixel
 * with its level's row after the clearing passes, a gray partial update the changed pixels only (epd_parwave.h).
 * Codes are FastEPD's: 0 no drive, 1 dark, 2 light; written as text '.', 'D', 'L', one character a pass.
 * Two tables: FastEPD's own (8 passes) and stock trmnl-firmware's (9 passes). Both are tuned by hand, and on the X
 * levels 5 and 6 came out of order (2026-10-08), so a host can replace single rows for the session
 * (docs/spec/usb-display.md "Tuning", key gray_row): "5:..DDLLDDL" or several, "5:..DDLLDDL 6:.DDDLLDDL".
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_EPD_GRAYTAB_H
#define TINY_EPD_GRAYTAB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { EPD_GRAYTAB_FASTEPD = 0, EPD_GRAYTAB_STOCK = 1 };
#define EPD_GRAYTAB_LEVELS     16
#define EPD_GRAYTAB_PASSES_MAX 9
/* room for every row as text: "15:" + passes + a space each, and the NUL */
#define EPD_GRAYTAB_TEXT_MAX (EPD_GRAYTAB_LEVELS * (3 + EPD_GRAYTAB_PASSES_MAX + 1) + 1)

typedef struct {
    uint8_t table, passes;
    uint16_t changed;                                       /* bit per level: its row is not the table's own */
    uint8_t m[EPD_GRAYTAB_LEVELS * EPD_GRAYTAB_PASSES_MAX]; /* the rows in use: level * passes + pass */
} epd_graytab_t;

/* A table as it comes: 16 rows of *passes codes. NULL for an unknown table. */
const uint8_t *epd_graytab_own(int table, int *passes);
const char *epd_graytab_name(int table); /* "fastepd", "stock" */
/* The table's own rows into t: every changed row is back to it. */
void epd_graytab_select(epd_graytab_t *t, int table);
/* Rows from text: entries "<level>:<passes>" separated by spaces or commas, level 0..15, passes exactly t->passes
 * characters of . D L; an empty text puts every row back. All entries are checked first: on false nothing changed
 * and `why` says in words which entry and why (<= 100 bytes). A row equal to the table's own is not a changed one. */
bool epd_graytab_set_rows(epd_graytab_t *t, const char *s, size_t len, char *why, size_t cap);
/* One row as text (passes characters and a NUL into b, room for EPD_GRAYTAB_PASSES_MAX + 1); b. */
char *epd_graytab_row_str(const uint8_t *m, int passes, int level, char *b);
/* The changed rows as the text epd_graytab_set_rows takes ("" when none) into b (EPD_GRAYTAB_TEXT_MAX); b. */
char *epd_graytab_rows_str(const epd_graytab_t *t, char *b, size_t n);
/* For a refresh's log line: "" when no row is changed, else " rows*=5,6" into b; b. */
char *epd_graytab_mark(const epd_graytab_t *t, char *b, size_t n);

#ifdef __cplusplus
}
#endif

#endif
