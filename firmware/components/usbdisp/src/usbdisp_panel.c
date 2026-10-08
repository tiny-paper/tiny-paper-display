/* usbdisp: one panel update, full (bands of rows) or partial (one rectangle with its old and new rows).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "usbdisp_priv.h"

/* The rows of `frame` (the whole picture), or with frame NULL `band` again for every band (clear). area: the refresh
 * clears and drives this rectangle alone (cfg panel_area), the picture is written whole all the same. */
int usbdisp_panel_full(usbdisp_t *d, uint8_t depth, bool image, const uint8_t *frame, const uint8_t *band, size_t stride,
                       const tiny_rect_t *area, uint32_t *ms, const char **why)
{
    uint16_t h = d->c.h, rows = d->c.band_rows && d->c.band_rows < h ? d->c.band_rows : h;
    uint64_t t0 = d->c.now_us(d->c.ctx);
    d->in_panel = true;
    int rc = d->c.panel_begin(d->c.ctx, depth, d->c.bwry, image, USBDISP_DRIVE_FULL);
    *why = "the panel did not start";
    if (!rc && area) {
        rc = d->c.panel_area ? d->c.panel_area(d->c.ctx, area) : -1;
        *why = "the panel did not take the rectangle for a full refresh";
    }
    /* bands top first; on a rows_bottom_up board the bottom band first, each next one ending where the last began */
    for (uint32_t done = 0; !rc && done < h;) {
        uint16_t n = (uint16_t)(h - done < rows ? h - done : rows);
        uint16_t y = (uint16_t)(d->c.rows_bottom_up ? h - done - n : done);
        tiny_rect_t r = {0, (int16_t)y, d->c.w, n};
        rc = d->c.panel_write(d->c.ctx, &r, NULL, frame ? frame + (size_t)y * stride : band, stride);
        *why = "the panel did not take the picture";
        done += n;
    }
    if (!rc) {
        rc = d->c.panel_refresh(d->c.ctx);
        *why = "the panel refresh failed";
    }
    d->c.panel_sleep(d->c.ctx);
    d->in_panel = false;
    *ms += (uint32_t)((d->c.now_us(d->c.ctx) - t0) / 1000);
    return rc;
}

int usbdisp_panel_area(usbdisp_t *d, uint8_t depth, const tiny_rect_t *r, const uint8_t *old, const uint8_t *rows,
                       size_t stride, bool all, uint32_t *ms, const char **why)
{
    uint64_t t0 = d->c.now_us(d->c.ctx);
    d->in_panel = true;
    int rc = d->c.panel_begin(d->c.ctx, depth, d->c.bwry, false, all ? USBDISP_DRIVE_PARTIAL_ALL : USBDISP_DRIVE_PARTIAL);
    *why = "the panel did not start the partial refresh";
    if (!rc) {
        rc = d->c.panel_write(d->c.ctx, r, old, rows, stride);
        *why = "the panel did not take the rectangle";
    }
    if (!rc) {
        rc = d->c.panel_refresh(d->c.ctx);
        *why = "the panel refresh failed";
    }
    d->c.panel_sleep(d->c.ctx);
    d->in_panel = false;
    *ms += (uint32_t)((d->c.now_us(d->c.ctx) - t0) / 1000);
    return rc;
}
