/* See epd_split.h. C11. SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_split.h"

#include <stdio.h>
#include <string.h>

void epd_split_reset(epd_split_t *s) { memset(s, 0, sizeof *s); }

void epd_split_add(epd_split_t *s, int part, uint32_t us)
{
    if (part < 0 || part >= EPD_SPLIT_PARTS)
        return;
    uint32_t v = s->us[part] + us;
    s->us[part] = v < us ? UINT32_MAX : v;
}

void epd_split_call(epd_split_t *s, uint32_t t0, uint32_t first, uint32_t t1)
{
    if (s->calls < UINT16_MAX)
        s->calls++;
    if (!first || first - t0 > t1 - t0) { /* no scan, or a stamp from outside the call: all of it is the library's */
        epd_split_add(s, EPD_SPLIT_LIB, t1 - t0);
        return;
    }
    epd_split_add(s, EPD_SPLIT_LIB, first - t0);
    epd_split_add(s, EPD_SPLIT_SCANS, t1 - first);
}

void epd_split_merge(epd_split_t *s, const epd_split_t *from)
{
    for (int i = 0; i < EPD_SPLIT_PARTS; i++)
        epd_split_add(s, i, from->us[i]);
    s->calls = (uint16_t)(s->calls + from->calls < s->calls ? UINT16_MAX : s->calls + from->calls);
}

static unsigned ms(uint32_t us) { return (unsigned)(us / 1000u + (us % 1000u >= 500u)); }

char *epd_split_str(const epd_split_t *s, uint32_t total_us, char *b, size_t n)
{
    uint64_t sum = 0;
    for (int i = 0; i < EPD_SPLIT_PARTS; i++)
        sum += s->us[i];
    const uint32_t *u = s->us;
    snprintf(b, n, "total=%u begin=%u write=%u up=%u prep=%u masks=%u lib=%u scans=%u down=%u after=%u save=%u log=%u "
                   "other=%u calls=%u",
             ms(total_us), ms(u[EPD_SPLIT_BEGIN]), ms(u[EPD_SPLIT_WRITE]), ms(u[EPD_SPLIT_UP]), ms(u[EPD_SPLIT_PREP]),
             ms(u[EPD_SPLIT_MASKS]), ms(u[EPD_SPLIT_LIB]), ms(u[EPD_SPLIT_SCANS]), ms(u[EPD_SPLIT_DOWN]),
             ms(u[EPD_SPLIT_AFTER]), ms(u[EPD_SPLIT_SAVE]), ms(u[EPD_SPLIT_LOG]),
             ms(sum < total_us ? (uint32_t)(total_us - sum) : 0),
             (unsigned)s->calls);
    return b;
}
