/* Early timer wakes (wakecal.h). C11. SPDX-License-Identifier: GPL-3.0-or-later */
#include "wakecal.h"

#include <string.h>

void wakecal_reset(wakecal_t *c)
{
    memset(c, 0, sizeof *c);
    c->magic = WAKECAL_MAGIC;
}

bool wakecal_ok(const wakecal_t *c) { return c->magic == WAKECAL_MAGIC; }

uint32_t wakecal_margin_ppm(const wakecal_t *c)
{
    uint64_t m = c->n ? (uint64_t)(c->mean_ppm > 0 ? c->mean_ppm : 0) + 3ull * c->dev_ppm : WAKECAL_INIT_PPM;
    m += WAKECAL_FLOOR_PPM;
    return m > WAKECAL_MAX_PPM ? WAKECAL_MAX_PPM : (uint32_t)m;
}

uint64_t wakecal_timer(wakecal_t *c, uint64_t now_us, uint64_t target_us)
{
    uint64_t left = target_us > now_us ? target_us - now_us : 0;
    uint64_t m = left / 1000000 * wakecal_margin_ppm(c) + left % 1000000 * wakecal_margin_ppm(c) / 1000000 +
                 WAKECAL_MIN_US;
    if (m > left / 2) /* short sleeps: at most half of it */
        m = left / 2;
    uint64_t ask = left - m;
    c->start_us = now_us;
    c->asked_us = ask;
    c->plan_us = target_us;
    return ask;
}

bool wakecal_woke(wakecal_t *c, uint64_t wake_us, int32_t *err_ppm)
{
    if (err_ppm)
        *err_ppm = 0;
    if (!c->plan_us || c->asked_us < 1000000 || wake_us < c->start_us)
        return false; /* nothing pending, too short to measure, or not this timer */
    int64_t err = (int64_t)(wake_us - c->start_us) - (int64_t)c->asked_us;
    int64_t ppm = err * 1000000 / (int64_t)c->asked_us;
    if (err_ppm)
        *err_ppm = (int32_t)(ppm > INT32_MAX ? INT32_MAX : ppm < INT32_MIN ? INT32_MIN : ppm);
    if (ppm > WAKECAL_BAD_PPM || ppm < -WAKECAL_BAD_PPM)
        return false;
    int32_t s = (int32_t)ppm;
    if (!c->n) {
        c->mean_ppm = s;
        c->dev_ppm = WAKECAL_INIT_PPM / 2;
    } else {
        int32_t d = s - c->mean_ppm;
        uint32_t ad = (uint32_t)(d < 0 ? -d : d);
        c->mean_ppm += d / 4;
        c->dev_ppm = ad > c->dev_ppm ? (c->dev_ppm + ad + 1) / 2 : (7 * c->dev_ppm + ad) / 8;
    }
    if (c->n < UINT32_MAX)
        c->n++;
    return true;
}

uint64_t wakecal_plan(const wakecal_t *c) { return c->plan_us; }

void wakecal_clear(wakecal_t *c) { c->start_us = c->asked_us = c->plan_us = 0; }
