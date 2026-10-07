/* See usb_detect.h. C11. SPDX-License-Identifier: GPL-3.0-or-later */
#include "usb_detect.h"

void usbdet_init(usbdet_t *d)
{
    d->waited = d->last = d->had = false;
    d->at = d->seen = d->still = 0;
}

static bool moving(usbdet_t *d)
{
    uint32_t a = d->frame(d->ctx) & USBDET_FRAME_MASK;
    d->delay_us(d->ctx, USBDET_SAMPLE_US);
    return (d->frame(d->ctx) & USBDET_FRAME_MASK) != a;
}

bool usbdet_present(usbdet_t *d, bool quick)
{
    int64_t now = d->now_us(d->ctx);
    if (!d->waited) {
        d->waited = true;
        int64_t until = quick ? USBDET_QUICK_US : USBDET_ENUM_US;
        while (d->now_us(d->ctx) < until && !moving(d))
            d->sleep_ms(d->ctx, USBDET_POLL_MS);
        if (d->now_us(d->ctx) < until) {
            d->seen = d->now_us(d->ctx);
            d->had = true;
        }
        d->at = -1000000;
    }
    if (now - d->at >= USBDET_RECHECK_US) {
        bool m = moving(d);
        d->at = d->now_us(d->ctx);
        if (m) {
            d->seen = d->at;
            d->had = true;
            d->still = 0;
        } else if (!d->still) {
            /* the grace runs from the first still sample, not from the last SOF: a long step without a look (a 40 s
             * download, a WiFi session) is not 40 s of silence (TRMNL X 2026-10-07: deep sleep after one still
             * sample with the browser's port open; issue #133) */
            d->still = d->at;
        }
        /* one still frame counter is not an unplug: leaving USB drops light sleep / deep sleep on the device, and the
         * web app's port is lost (PicPak 2026-10-04, "port closed" every few minutes) */
        /* the grace only for a host that was there: a zeroed `seen` is not a SOF at boot (issue #118) */
        d->last = m || (d->had && d->at - d->still < USBDET_LOST_US);
    }
    return d->last;
}
