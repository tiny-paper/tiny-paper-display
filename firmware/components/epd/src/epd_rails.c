/* See epd_rails.h. C11. SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_rails.h"

#include <string.h>

void epd_rails_init(epd_rails_t *r, const epd_rails_io_t *io, void *ctx)
{
    memset(r, 0, sizeof *r);
    r->io = io;
    r->ctx = ctx;
}

static void disarm(epd_rails_t *r)
{
    if (r->armed)
        r->io->disarm(r->ctx);
    r->armed = false;
}

static void down(epd_rails_t *r)
{
    disarm(r);
    r->io->power(r->ctx, false);
    r->up = false;
    r->downs++;
}

bool epd_rails_set_hold(epd_rails_t *r, int ms)
{
    if (ms < 0 || ms > EPD_RAILS_HOLD_MAX_MS)
        return false;
    r->hold_ms = (uint16_t)ms;
    if (!ms && r->up && !r->busy)
        down(r);
    return true;
}

int epd_rails_begin(epd_rails_t *r, bool *kept)
{
    disarm(r);
    r->busy = true;
    if (kept)
        *kept = r->up;
    if (r->up) {
        r->kept++;
        return 0;
    }
    int rc = r->io->power(r->ctx, true);
    if (rc) { /* half up is not up: down, and the update does not run */
        down(r);
        r->busy = false;
        return rc;
    }
    r->up = true;
    r->ups++;
    return 0;
}

void epd_rails_end(epd_rails_t *r, bool ok)
{
    r->busy = false;
    if (!r->up)
        return;
    if (!ok || !r->hold_ms) {
        down(r);
        return;
    }
    disarm(r);
    r->armed = r->io->arm(r->ctx, r->hold_ms);
    if (!r->armed)
        down(r);
}

void epd_rails_timer(epd_rails_t *r)
{
    if (r->busy)
        return; /* the update's end arms again or powers down */
    r->armed = false;
    if (r->up) {
        down(r);
        r->timer_downs++;
    }
}

void epd_rails_off(epd_rails_t *r)
{
    if (r->busy)
        return;
    if (r->up)
        down(r);
    else
        disarm(r);
}
