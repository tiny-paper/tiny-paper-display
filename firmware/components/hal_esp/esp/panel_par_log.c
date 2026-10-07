/* hal_esp: the row timing lines of a TRMNL X panel update (panel_par.c): "rowmax=..." inside the update's own
 * line, and the "panel rows" line after it with the fixed-drive statistics and the hardware CKV edge count
 * (epd_rowtime.h; issue #203: a frame that does not clock the gate driver once per row gets a W line).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>

#include "hal_esp_priv.h"

#if BOARD_PANEL_FASTEPD /* whole file */

#include "epd_par.h"
#include "epd_rowtime.h"

const char *hal_esp_par_rows_str(char *b, size_t n)
{
    const epd_row_stats_t *r = epdp_last_rows();
    int k = snprintf(b, n, "rowmax=%u@%d ", (unsigned)r->max_us, r->max_row);
    if (k > 0 && (size_t)k < n)
        epd_row_stats_str(r, b + k, n - (size_t)k);
    return b;
}

void hal_esp_par_log_drive(bool full)
{
    uint32_t d;
    const epd_row_drive_stats_t *st = epdp_last_drive(&d);
    char b[112];
    hal_logf("panel rows %s", epd_row_drive_str(st, full ? 0 : d, b, sizeof b));
    unsigned per;
    if (st->frames && !epd_row_edges_ok(st->edges, st->frames, epdp_height(), &per))
        hal_logf("W panel rows: %u CKV edges per frame for %d rows (gate clocks %s; issue #203)", per, epdp_height(),
                 per > (unsigned)epdp_height() + EPD_ROW_START_EDGES ? "doubled" : "missing");
}

#endif /* BOARD_PANEL_FASTEPD */
