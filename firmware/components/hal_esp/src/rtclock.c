/* RTC clock from raw ticks (rtclock.h). SPDX-License-Identifier: GPL-3.0-or-later */
#include "rtclock.h"

bool rtclock_ok(const rtclock_t *c, uint64_t ticks)
{
    return c->magic == RTCLOCK_MAGIC && c->cal && ticks >= c->ticks0;
}

void rtclock_init(rtclock_t *c, uint64_t ticks, uint64_t now_us, uint32_t cal)
{
    c->magic = RTCLOCK_MAGIC;
    c->cal = cal ? cal : 1;
    c->base_us = now_us;
    c->ticks0 = ticks;
}

uint64_t rtclock_us(const rtclock_t *c, uint64_t ticks)
{
    uint64_t d = ticks - c->ticks0;
    /* split: no overflow for long sleeps (d * cal exceeds 64 bits after ~3 days at 150 kHz) */
    uint64_t hi = d >> 20, lo = d & 0xFFFFF;
    return c->base_us + ((hi * c->cal) << 1) + ((lo * c->cal) >> RTCLOCK_FRACT);
}

void rtclock_recal(rtclock_t *c, uint64_t ticks, uint32_t cal)
{
    c->base_us = rtclock_us(c, ticks);
    c->ticks0 = ticks;
    if (!cal)
        return;
    uint64_t old = c->cal;
    if ((uint64_t)cal * 5 < old * 4 || (uint64_t)cal * 5 > old * 6)
        c->cal = cal; /* another oscillator (or a stale state): no blending */
    else
        c->cal = (uint32_t)((old * 3 + cal + 2) / 4);
}
