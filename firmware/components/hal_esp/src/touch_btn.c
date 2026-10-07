/* See touch_btn.h. C11. SPDX-License-Identifier: GPL-3.0-or-later */
#include "touch_btn.h"

#include <string.h>

void tbtn_reset(tbtn_t *b)
{
    uint16_t sl_min = b->sl_min; /* a setting, not state */
    memset(b, 0, sizeof *b);
    b->sl_min = sl_min;
    b->sl0 = b->sl1 = b->z0 = b->z1 = -1;
}

static void end(tbtn_t *b, const char *why)
{
    if (b->down)
        b->ended = true;
    b->down = false;
    b->need_up = true;
    b->why = why;
}

/* Rules that end a press by time (checked before every read and on every query). */
static void check(tbtn_t *b, int64_t now)
{
    if (!b->down)
        return;
    if (now - b->last_us > TBTN_GAP_US)
        end(b, "gap");
    else if (now - b->since_us > TBTN_REST_US) {
        end(b, "rest"); /* a resting object, never a press: no input, the chip is re-baselined (tbtn_reseed_take) */
        b->rest = b->reseed = true;
    } else if (now - b->since_us > TBTN_MAX_US)
        end(b, "max");
}

uint8_t tbtn_zones(uint8_t st1)
{
    return (uint8_t)((st1 >> 1 & 1u) | (st1 >> 2 & 2u) | (st1 >> 3 & 4u));
}

static int8_t gesture_dir(uint8_t g0)
{
    return (g0 & TBTN_G_SWIPE_NEG) ? -1 : (g0 & TBTN_G_SWIPE_POS) ? 1 : 0;
}

static int iabs(int v) { return v < 0 ? -v : v; }

/* the zone position of a mask: the mean of CH0 0, CH1 1024, CH2 2048 */
static int16_t zone_pos(uint8_t z)
{
    int n = 0, sum = 0;
    for (int i = 0; i < 3; i++)
        if (z >> i & 1u) {
            n++;
            sum += i * 1024;
        }
    return n ? (int16_t)(sum / n) : -1;
}

/* Swipes (touch_btn.h): the direction of a press released at `now`, and what decided it (*by) */
static int8_t classify(const tbtn_t *b, int64_t now, uint8_t *by)
{
    int sl_min = b->sl_min ? b->sl_min : TBTN_SWIPE_SL;
    *by = TBTN_BY_NONE;
    if (now - b->since_us > TBTN_SWIPE_MAX_US)
        return 0;
    if (b->sl0 >= 0 && iabs(b->sl1 - b->sl0) >= sl_min) {
        *by = TBTN_BY_SLIDER;
        return b->sl1 > b->sl0 ? 1 : -1;
    }
    if (b->gswipe) {
        *by = TBTN_BY_GESTURE;
        return b->gswipe;
    }
    if (b->sl0 < 0 && b->z0 >= 0 && iabs(b->z1 - b->z0) >= TBTN_SWIPE_ZONE) {
        *by = TBTN_BY_ZONES;
        return b->z1 > b->z0 ? 1 : -1;
    }
    return 0;
}

bool tbtn_brush_of(uint8_t zones, unsigned slide, unsigned ms)
{
    unsigned z = zones & 7u;
    return ms < TBTN_BRUSH_MAX_MS && ((z & (z - 1u)) != 0 || slide >= TBTN_BRUSH_SL);
}

bool tbtn_brush(const tbtn_t *b)
{
    unsigned slide = b->sl0 >= 0 && b->sl1 >= 0 ? (unsigned)iabs(b->sl1 - b->sl0) : 0;
    int64_t end = b->last_us;
    return b->wide || tbtn_brush_of(b->zones, slide, (unsigned)((end - b->since_us) / 1000));
}

/* a reset or lost config: the press in progress is dropped (never queued), a new one needs a read without touch */
void tbtn_drop(tbtn_t *b)
{
    b->down = b->ended = false;
    b->need_up = true;
    b->why = NULL;
    b->swipe = 0;
    b->hold = false;
}

bool tbtn_reseed_take(tbtn_t *b)
{
    bool r = b->reseed;
    b->reseed = false;
    return r;
}

void tbtn_quiet(tbtn_t *b, int64_t until_us)
{
    b->quiet_us = until_us;
    b->down = false;
    b->need_up = true;
}

const char *tbtn_by_name(uint8_t by)
{
    return by == TBTN_BY_SLIDER ? "slider" : by == TBTN_BY_GESTURE ? "gesture" : by == TBTN_BY_ZONES ? "zones" : "-";
}

/* A finger covers one zone or two next to each other. A sample that shows all three, or that moves to a zone not
 * next to the last kept one between reads close together (the stream: a window every report), is no finger
 * (touch_btn.h "Rejected samples"). Reads farther apart may have missed the way between (windows lost while the
 * bus was busy): a far zone is then a move. */
static bool plausible(uint8_t prev, uint8_t z, int64_t dt)
{
    if (z == 7)
        return false;
    if (!prev || dt >= TBTN_JUMP_US)
        return true;
    for (int i = 0; i < 3; i++) {
        if (!(z >> i & 1u))
            continue;
        bool near = false;
        for (int j = 0; j < 3 && !near; j++)
            near = (prev >> j & 1u) && i - j <= 1 && j - i <= 1;
        if (!near)
            return false;
    }
    return true;
}

/* a touched read of the press: its movement */
static void track(tbtn_t *b, uint8_t st1, uint8_t g0, uint16_t sl)
{
    int8_t d = gesture_dir(g0);
    if (d)
        b->gswipe = d;
    if (sl != TBTN_SL_NONE && sl < 0x8000) {
        if (b->sl0 < 0)
            b->sl0 = (int16_t)sl;
        b->sl1 = (int16_t)sl;
    }
    uint8_t z = tbtn_zones(st1);
    int16_t p = zone_pos(z);
    if (p >= 0) {
        if (b->z0 < 0)
            b->z0 = p;
        b->z1 = p;
    }
    b->zones |= z;
    if (!b->ntrail || b->trail[b->ntrail - 1] != z) {
        if (b->ntrail < TBTN_TRAIL)
            b->ntrail++;
        b->trail[b->ntrail - 1] = z;
    }
}

void tbtn_read(tbtn_t *b, int64_t now, uint8_t st1, uint8_t g0) { tbtn_read_sl(b, now, st1, g0, TBTN_SL_NONE); }

void tbtn_read_sl(tbtn_t *b, int64_t now, uint8_t st1, uint8_t g0, uint16_t sl)
{
    check(b, now);
    bool touched = (st1 & TBTN_TOUCH_BITS) != 0;
    if (now < b->quiet_us) { /* just after a reseed: the chip's reads settle */
        if (touched)
            return;
        b->need_up = false;
        return;
    }
    int64_t prev_us = b->last_us; /* the read before this one: how close the samples are (plausible) */
    b->last_us = now;
    if (!touched) {
        if (b->rest) { /* the resting object left: the baseline moves again */
            b->rest = false;
            b->reseed = true;
        }
        if (b->down) {
            int64_t end = now;
            if (now - b->touched_us > TBTN_LATE_US) { /* a late release: the touch ended with its last touched read */
                b->late_us = now - b->touched_us;
                end = b->last_us = b->touched_us;
            }
            int8_t d = gesture_dir(g0); /* a swipe the chip reports with the release */
            if (d)
                b->gswipe = d;
            b->swipe = classify(b, end, &b->by);
            b->ended = true;
        }
        b->down = false;
        b->need_up = false;
        b->why = NULL;
        return;
    }
    uint8_t z = tbtn_zones(st1);
    if (!b->down) {
        if (b->need_up)
            return;
        b->wide = z == 7; /* three zones at once: no finger; a brush whatever its length (a resting object still rests) */
        b->touched_us = now;
        b->late_us = 0;
        b->jumps = 0;
        b->down = true;
        b->since_us = now;
        b->hold = false;
        b->why = NULL;
        b->gswipe = b->swipe = 0;
        b->by = TBTN_BY_NONE;
        b->sl0 = b->sl1 = b->z0 = b->z1 = -1;
        b->ntrail = b->zones = 0;
    } else if (!plausible(b->trail[b->ntrail - 1], z, now - prev_us)) {
        if (b->jumps < 255)
            b->jumps++;
        return; /* the chip answered (last_us): nothing of the sample is kept */
    }
    b->touched_us = now;
    track(b, st1, g0, sl);
    /* the hold gesture of this press: HOLD with BUSY (a HOLD without BUSY is the end of an earlier one) */
    if ((g0 & (TBTN_G_HOLD | TBTN_G_BUSY)) == (TBTN_G_HOLD | TBTN_G_BUSY))
        b->hold = true;
    else if (b->hold && !(g0 & TBTN_G_HOLD))
        b->hold = false;
    check(b, now);
}

bool tbtn_down(tbtn_t *b, int64_t now)
{
    check(b, now);
    return b->down;
}

bool tbtn_poll_due(const tbtn_t *b, int64_t now)
{
    return b->down && now - b->last_us >= TBTN_POLL_US && now - b->try_us >= TBTN_RETRY_US;
}

void tbtn_tried(tbtn_t *b, int64_t now) { b->try_us = now; }

void tbtn_wake(tbtn_t *b, uint8_t st1, uint8_t g0)
{
    if (!b->down && !(st1 & TBTN_TOUCH_BITS)) {
        b->swipe = gesture_dir(g0);
        b->by = b->swipe ? TBTN_BY_GESTURE : TBTN_BY_NONE;
    }
}

int8_t tbtn_swipe_take(tbtn_t *b)
{
    int8_t d = b->swipe;
    b->swipe = 0;
    return d;
}
