/*
 * epd_rails.h - when the panel's supply rails are up (TRMNL X, epd_par.h, esp/epd_fastepd.cpp). Hardware-free,
 * host-tested: the glue only forwards (power, a one-shot timer) and calls in.
 *
 * Before this file every partial update powered the rails up at its start and down at its end. With a hold time
 * (tuning key rails_hold_ms, 0 = as before) they stay up that long after a partial update, so the next one inside
 * that time skips the power up. The rule that must hold whatever happens: rails that are up and not driven by an
 * update come down
 *   when the hold time passes (the timer),
 *   at once when an update failed, when the hold is 0, when no timer could be armed, when the hold is set to 0,
 *   before sleep, restart and a full refresh (epd_rails_off).
 * A failed power up is followed by a power down (the supply may be half up). The timer may fire while an update
 * runs: it does nothing then, the update's end decides again. A timer left from an earlier update that fires
 * after a later one ended brings the rails down early, never late.
 * Not covered: a crash or a hardware reset while the rails are held (as during any update); the pins of the
 * supply keep their state until the firmware's next panel power down.
 * The caller serialises the calls (the glue: one lock, the timer only tries it).
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_EPD_RAILS_H
#define TINY_EPD_RAILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPD_RAILS_HOLD_MAX_MS 10000
#define EPD_RAILS_KEY         "rails_hold_ms"

typedef struct {
    int (*power)(void *ctx, bool on);    /* rails up (0: good) or down; down works whatever an up left behind */
    bool (*arm)(void *ctx, uint32_t ms); /* epd_rails_timer is called once after ms; false: no timer */
    void (*disarm)(void *ctx);
} epd_rails_io_t;

typedef struct {
    const epd_rails_io_t *io;
    void *ctx;
    uint16_t hold_ms;
    bool up, busy, armed;
    uint32_t ups, kept, downs, timer_downs; /* power ups, updates that found the rails up, power downs, by timer */
} epd_rails_t;

void epd_rails_init(epd_rails_t *r, const epd_rails_io_t *io, void *ctx);
/* false: outside 0..EPD_RAILS_HOLD_MAX_MS, nothing changed. 0 while the rails are held: down now. */
bool epd_rails_set_hold(epd_rails_t *r, int ms);
/* An update starts: rails up unless they are. 0, or the supply's error (the rails are down again then).
 * kept: they were up already. */
int epd_rails_begin(epd_rails_t *r, bool *kept);
/* The update is over. ok false: it failed. */
void epd_rails_end(epd_rails_t *r, bool ok);
/* The timer fired. */
void epd_rails_timer(epd_rails_t *r);
/* Down now, whatever the hold (sleep, restart, before a full refresh). Not during an update. */
void epd_rails_off(epd_rails_t *r);

#ifdef __cplusplus
}
#endif

#endif
