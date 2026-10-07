/* usbdisp on tiny/hal.h: the device's (and the simulator's) link, heap, clock and panel.
 * SPDX-License-Identifier: GPL-3.0-or-later */
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

static int h_begin(void *ctx, uint8_t depth, bool bwry, bool image)
{
    (void)ctx;
    hal_panel_palette(bwry ? GFX_PAL_BWRY : GFX_PAL_GRAY);
    hal_panel_new_image(image);
    return hal_panel_begin(depth, HAL_REFRESH_FULL);
}

static int h_write_rows(void *ctx, const tiny_rect_t *r, const uint8_t *rows, size_t stride)
{
    (void)ctx;
    return hal_panel_write(r, NULL, rows, stride);
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
    hal_console_write("# display ", 10);
    hal_console_write(line, strlen(line));
    hal_console_write("\n", 1);
}

void usbdisp_hal_cfg(usbdisp_cfg_t *cfg, const char *fw)
{
    const hal_board_t *b = hal_board();
    memset(cfg, 0, sizeof *cfg);
    cfg->fw = fw;
    cfg->board = b->id;
    cfg->w = b->panel_w;
    cfg->h = b->panel_h;
    cfg->bwry = b->colors == TINY_COLORS_BWRY;
    cfg->depths = cfg->bwry ? 1 : b->depths & 7;
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
    cfg->log = h_log;
}
