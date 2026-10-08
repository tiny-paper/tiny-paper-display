/*
 * fresh_save.h - the TRMNL X redrive bitmap file (/xfresh.bin, panel_par.c): written after a partial refresh so the
 * redrive survives deep sleep, or not written at all while the display firmware keeps it in RAM (issue #214).
 * Hardware-free, host-tested. C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_FRESH_SAVE_H
#define TINY_FRESH_SAVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FRESH_PATH  "/xfresh.bin"
#define FRESH_TMP   "/xfresh.tmp"
#define FRESH_MAGIC 0x32524658u /* "XFR2": two planes of redrive counts (XFR1, one plane, is dropped) */

/* Keep the bitmap `f` (n bytes) in /xfresh.bin tagged `tag`: run-length coded into FRESH_TMP, then renamed. ram_only
 * (hal_panel_fresh_ram): touch no file at all. Returns the bytes written, 0 when nothing was written (ram_only), -1
 * failed (the old file is removed, so a stale bitmap is never loaded). */
long fresh_save_file(const uint8_t *f, size_t n, uint32_t tag, bool ram_only);

#endif
