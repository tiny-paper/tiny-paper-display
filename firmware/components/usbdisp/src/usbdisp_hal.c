/* usbdisp on tiny/hal.h: the device's (and the simulator's) link, heap, clock and panel.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tiny/gfx.h"
#include "tiny/hal.h"
#include "usbdisp.h"

/* Bands of a full refresh, as the app writes them (boards/<board>/caps.json app_bands; host: the OG's) */
#ifndef USBDISP_BANDS
#define USBDISP_BANDS 6
#endif

static long h_read(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    (void)ctx;
    return hal_link_read(HAL_LINK_USB, buf, cap, timeout_ms);
}

static long h_write(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    return hal_link_write(HAL_LINK_USB, buf, len);
}

static void *h_alloc(void *ctx, size_t n)
{
    (void)ctx;
    return malloc(n);
}

static void h_free(void *ctx, void *p)
{
    (void)ctx;
    free(p);
}

static uint64_t h_now(void *ctx)
{
    (void)ctx;
    return hal_rtc_us();
}

static int h_begin(void *ctx, uint8_t depth, bool bwry, bool image, usbdisp_drive_t drive)
{
    (void)ctx;
    hal_panel_palette(bwry ? GFX_PAL_BWRY : GFX_PAL_GRAY);
    hal_panel_new_image(image);
    if (drive != USBDISP_DRIVE_FULL)
        hal_panel_partial_all(drive == USBDISP_DRIVE_PARTIAL_ALL);
    return hal_panel_begin(depth, drive == USBDISP_DRIVE_FULL ? HAL_REFRESH_FULL : HAL_REFRESH_PARTIAL);
}

static int h_write_rows(void *ctx, const tiny_rect_t *r, const uint8_t *prev, const uint8_t *rows, size_t stride)
{
    (void)ctx;
    return hal_panel_write(r, prev, rows, stride);
}

static bool h_gates(void *ctx, usbdisp_gates_t *g)
{
    (void)ctx;
    hal_gate_clocks_t c;
    if (!hal_panel_gate_clocks(&c))
        return false;
    *g = (usbdisp_gates_t){c.edges, c.frames, c.rows, c.per, c.ok};
    return true;
}

static uint8_t h_partial_now(void *ctx, uint8_t depths) { (void)ctx; return hal_panel_partial_now(depths); }
static size_t h_tune_count(void *ctx) { (void)ctx; return hal_panel_tune_count(); }
static const hal_tune_t *h_tune_info(void *ctx, size_t i) { (void)ctx; return hal_panel_tune_info(i); }
static int h_tune_get(void *ctx, size_t i) { (void)ctx; return hal_panel_tune_get(i); }
static bool h_tune_set(void *ctx, size_t i, int v) { (void)ctx; return hal_panel_tune_set(i, v); }
static void h_tune_defaults(void *ctx) { (void)ctx; hal_panel_tune_defaults(); }
static bool h_gray_rows_set(void *ctx, const char *s, size_t len, char *why, size_t cap)
{
    (void)ctx;
    return hal_panel_gray_rows_set(s, len, why, cap);
}
static void h_gray_rows_get(void *ctx, char *out, size_t cap) { (void)ctx; hal_panel_gray_rows_get(out, cap); }

static int h_kv_get(void *ctx, const char *key, void *buf, size_t cap)
{
    (void)ctx;
    return hal_kv_get(key, buf, cap);
}

static int h_kv_set(void *ctx, const char *key, const void *buf, size_t len)
{
    (void)ctx;
    return hal_kv_set(key, buf, len);
}

static int h_refresh(void *ctx)
{
    (void)ctx;
    return hal_panel_refresh();
}

static void h_sleep(void *ctx)
{
    (void)ctx;
    hal_panel_sleep();
}

static void h_log(void *ctx, const char *line)
{
    (void)ctx;
    /* one write: the link's lock is held per write, and a line in three pieces let another task's line in between */
    char b[272];
    int n = snprintf(b, sizeof b, "# display %s", line);
    if (n < 0)
        return;
    if ((size_t)n > sizeof b - 2)
        n = (int)sizeof b - 2;
    b[n++] = '\n';
    hal_console_write(b, (size_t)n);
}

void usbdisp_hal_cfg(usbdisp_cfg_t *cfg, const char *fw)
{
    const hal_board_t *b = hal_board();
    memset(cfg, 0, sizeof *cfg);
    hal_panel_fresh_ram(true); /* awake, and a wake is a full refresh: no redrive bitmap file per partial (issue #214) */
    cfg->fw = fw;
    cfg->board = b->id;
    cfg->w = b->panel_w;
    cfg->h = b->panel_h;
    cfg->bwry = b->colors == TINY_COLORS_BWRY;
    cfg->depths = cfg->bwry ? 1 : b->depths & 7;
    /* partial refresh: 1 bit where the board has one, the gray depths where its panel takes gray partials */
    if (!cfg->bwry && b->partial_refresh)
        cfg->partial_depths = (uint8_t)(1 | (hal_panel_gray_partial() ? cfg->depths & 6 : 0));
    cfg->partial_all = cfg->partial_depths && hal_panel_partial_all(false);
    cfg->gray_partial_max_px = cfg->partial_depths & 6 ? USBDISP_GRAY_PARTIAL_MAX_PX : 0;
    cfg->full_every = USBDISP_FULL_EVERY;
    cfg->rows_bottom_up = b->rows_bottom_up;
    cfg->band_rows = (uint16_t)((b->panel_h + USBDISP_BANDS - 1) / USBDISP_BANDS);
    cfg->read = h_read;
    cfg->write = h_write;
    cfg->alloc = h_alloc;
    cfg->free = h_free;
    cfg->now_us = h_now;
    cfg->panel_begin = h_begin;
    cfg->panel_write = h_write_rows;
    cfg->panel_refresh = h_refresh;
    cfg->panel_sleep = h_sleep;
    cfg->panel_gates = h_gates;
    cfg->partial_now = h_partial_now;
    cfg->tune_count = h_tune_count;
    cfg->tune_info = h_tune_info;
    cfg->tune_get = h_tune_get;
    cfg->tune_set = h_tune_set;
    cfg->tune_defaults = h_tune_defaults;
    cfg->gray_rows_set = h_gray_rows_set;
    cfg->gray_rows_get = h_gray_rows_get;
    cfg->kv_get = h_kv_get;
    cfg->kv_set = h_kv_set;
    cfg->log = h_log;
}
