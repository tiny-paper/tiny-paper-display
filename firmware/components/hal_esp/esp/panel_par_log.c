/* hal_esp: the row timing lines of a TRMNL X panel update (panel_par.c): "rowmax=..." inside the update's own
 * line, and the "panel rows" line after it with the fixed-drive statistics and the hardware CKV edge count
 * (epd_rowtime.h; issue #203: a frame that does not clock the gate driver once per row gets a W line).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>

#include "esp_timer.h"
#include "hal_esp_priv.h"

#if BOARD_PANEL_FASTEPD /* whole file */

#include "epd_par.h"
#include "epd_parwave.h"
#include "epd_rowtime.h"
#include "epd_scan.h"

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

/* After a partial: "panel scan neutral=.. skip=.. frames=.. careful=.. skipped=.. left=.. gap=.. ok per=<edges a
 * scan>" (epd_scan.h): the rows each scan wrote with the timed step and those it clocked through, as counted while
 * they ran, next to the hardware's edge count. A W line when the rows do not add up to the panel's for every scan. */
void hal_esp_par_log_scan(void)
{
    epd_scan_cfg_t c;
    bool skip;
    const epd_scan_stats_t *st = epdp_last_scan(&c, &skip);
    const epd_row_drive_stats_t *dr = epdp_last_drive(NULL);
    unsigned per = 0;
    char b[128];
    if (!st->frames)
        return;
    epd_row_edges_ok(dr->edges, dr->frames, epdp_height(), &per);
    hal_logf("panel scan %s per=%u", epd_scan_str(&c, skip, st, epdp_height(), b, sizeof b), per);
    if (!epd_scan_stats_ok(st, epdp_height()))
        hal_logf("W panel scan: %lu rows written and %lu clocked through in %lu scans of %d rows, %lu stray row steps",
                 (unsigned long)st->careful, (unsigned long)st->skipped, (unsigned long)st->frames, epdp_height(),
                 (unsigned long)st->stray);
}

/* Last of an update's lines: "panel time total=.. begin=.. write=.. up=.. prep=.. masks=.. lib=.. scans=.. down=..
 * after=.. save=.. log=.. other=.. calls=.." in ms (epd_split.h): the driver's parts and the caller's (begin, write,
 * save), the time the lines before this one took to write (log), and what no part covers (other). total runs from the
 * caller's begin (t_begin) to here; t_log is when the first of the update's lines was written. */
void hal_esp_par_log_time(const epd_split_t *caller, int64_t t_begin, int64_t t_log)
{
    epd_split_t s = *epdp_last_split();
    char b[200];
    int64_t now = esp_timer_get_time();
    epd_split_merge(&s, caller);
    epd_split_add(&s, EPD_SPLIT_LOG, (uint32_t)(now - t_log));
    hal_logf("panel time %s", epd_split_str(&s, (uint32_t)(now - t_begin), b, sizeof b));
}

/* issue #203: the frames FastEPD was given at the drive call, its own state then, and per step (kick/main/extra)
 * the pixels driven and those of them that did not change. Unchanged white is never driven: same= counts only
 * the blacks a redrive pushes again in the kick (the partial line's redrive=<px>), 0 in main and extra. */
void hal_esp_par_log_given(const epdp_given_t *g)
{
    const epd_wave_drive_t *d = g->drv;
    hal_logf("panel given prev=%08lx cur=%08lx diff=%lu wb=%lu bw=%lu drv=%lu/%lu/%lu same=%lu/%lu/%lu drows=%d..%d "
             "lib=mode%u/prev%u pwr=%u/%u",
             (unsigned long)g->crc_prev, (unsigned long)g->crc_cur, (unsigned long)g->diff_px, (unsigned long)g->wb_px,
             (unsigned long)g->bw_px, (unsigned long)d[EPD_WAVE_KICK].driven, (unsigned long)d[EPD_WAVE_MAIN].driven,
             (unsigned long)d[EPD_WAVE_EXTRA].driven, (unsigned long)d[EPD_WAVE_KICK].same,
             (unsigned long)d[EPD_WAVE_MAIN].same, (unsigned long)d[EPD_WAVE_EXTRA].same, g->y0, g->y1, g->mode,
             g->prev_mode, g->pwr_before, g->pwr_after);
    if (d[EPD_WAVE_MAIN].driven != g->diff_px || d[EPD_WAVE_MAIN].same || d[EPD_WAVE_EXTRA].same)
        hal_logf("W panel given: main drives %lu px for %lu changed, unchanged driven main=%lu extra=%lu (issue #203)",
                 (unsigned long)d[EPD_WAVE_MAIN].driven, (unsigned long)g->diff_px, (unsigned long)d[EPD_WAVE_MAIN].same,
                 (unsigned long)d[EPD_WAVE_EXTRA].same);
}

bool hal_panel_gate_clocks(hal_gate_clocks_t *g)
{
    uint32_t d;
    const epd_row_drive_stats_t *st = epdp_last_drive(&d);
    unsigned per = 0;
    if (!st->frames)
        return false;
    g->edges = st->edges;
    g->frames = st->frames;
    g->rows = (uint16_t)epdp_height();
    g->ok = epd_row_edges_ok(st->edges, st->frames, epdp_height(), &per);
    g->per = (uint16_t)per;
    return true;
}

#endif /* BOARD_PANEL_FASTEPD */
