/* BWRY refresh speed (epd_bwspeed.h). C11. SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_bwspeed.h"

#include "epd_bwry.h"

__attribute__((noinline)) bws_pick_t bws_pick(uint8_t speed, uint8_t native_fast, bool lut10, bool lut5)
{
    if (speed == BWS_FASTEST && lut5)
        return (bws_pick_t){BWRY_FAST_OFF, BWS_LUT_5S, BWS_LUT5_MS, BWS_WHY_SET};
    if ((speed == BWS_FAST || speed == BWS_FASTEST) && lut10)
        return (bws_pick_t){BWRY_FAST_OFF, BWS_LUT_10S, BWS_LUT10_MS, BWS_WHY_SET};
    if (speed == BWS_FAST || speed == BWS_FASTEST)
        return (bws_pick_t){BWRY_FAST_E6P, BWS_LUT_NONE, BWS_E6P_MS, BWS_WHY_SET};
    return (bws_pick_t){native_fast, BWS_LUT_NONE, native_fast == BWRY_FAST_E6P ? BWS_E6P_MS : 0, BWS_WHY_SET};
}

const char *bws_name(uint8_t speed)
{
    return speed == BWS_AUTO ? "auto" : speed == BWS_FASTEST ? "fastest" : speed == BWS_FAST ? "fast" : "native";
}

const char *bws_pick_name(const bws_pick_t *p)
{
    if (p->lut == BWS_LUT_5S)
        return "lut5";
    if (p->lut == BWS_LUT_10S)
        return "lut10";
    return p->fast == BWRY_FAST_E6P ? "e6p" : p->fast == BWRY_FAST_E6 ? "e6" : p->fast == BWRY_FAST_TSE ? "tse" : "otp";
}

bool bws_lut_allowed(uint8_t mode, bool rev_read, const uint8_t rev[3])
{
    if (mode == BWS_REV_FORCE)
        return true;
    if (mode != BWS_REV_AUTO || !rev_read)
        return false;
    return rev[0] == 0x06 && rev[1] == 0x01 && rev[2] == 0x01;
}

const char *bws_lut_name(const bws_pick_t *p)
{
    return p->lut == BWS_LUT_5S ? "5s" : p->lut == BWS_LUT_10S ? "10s" : "otp";
}

const char *bws_why_name(uint8_t why)
{
    return why == BWS_WHY_TEMP ? "temp" : why == BWS_WHY_REV ? "rev" : why == BWS_WHY_OFF ? "off" : "set";
}

static const bws_band_t BWS_BANDS[] = {{BWS_BAND_WARM_C, BWS_LUT_5S}, {BWS_BAND_COOL_C, BWS_LUT_10S}};

bool bws_temp_ok(int16_t c) { return c >= BWS_TEMP_MIN && c <= BWS_TEMP_MAX; }

int16_t bws_temp_use(int16_t read_c, int16_t cached_c, uint32_t cached_age_s)
{
    if (bws_temp_ok(read_c))
        return read_c;
    return bws_temp_ok(cached_c) && cached_age_s <= BWS_TEMP_CACHE_S ? cached_c : BWS_TEMP_NONE;
}

static bool any_lut(const bws_in_t *in) { return in->lut5 || in->lut10; }

bool bws_wants_temp(const bws_in_t *in)
{
    return any_lut(in) && in->speed != BWS_NATIVE && bws_lut_allowed(in->lut_mode, in->rev_read, in->rev);
}

static bws_pick_t native(const bws_in_t *in, uint8_t why)
{
    bws_pick_t p = bws_pick(BWS_NATIVE, in->native_fast, false, false);
    p.why = why;
    return p;
}

bws_pick_t bws_choose(const bws_in_t *in)
{
    bool ok = bws_lut_allowed(in->lut_mode, in->rev_read, in->rev);
    uint8_t no = !any_lut(in) || in->lut_mode == BWS_REV_OFF ? BWS_WHY_OFF : BWS_WHY_REV;
    if (in->speed != BWS_AUTO) {
        bws_pick_t p = bws_pick(in->speed <= BWS_FASTEST ? in->speed : BWS_NATIVE, in->native_fast, ok && in->lut10,
                                ok && in->lut5);
        p.why = in->speed != BWS_NATIVE && !ok ? no : BWS_WHY_SET;
        return p;
    }
    if (!any_lut(in) || !ok)
        return native(in, no);
    if (in->lut_mode == BWS_REV_FORCE) /* testing: neither the REV nor the temperature */
        return bws_pick(BWS_FASTEST, in->native_fast, in->lut10, in->lut5);
    if (in->temp_c < BWS_TEMP_MIN || in->temp_c > BWS_TEMP_MAX)
        return native(in, BWS_WHY_TEMP);
    for (unsigned i = 0; i < sizeof BWS_BANDS / sizeof BWS_BANDS[0]; i++) {
        if (in->temp_c < BWS_BANDS[i].min_c)
            continue;
        bool five = BWS_BANDS[i].lut == BWS_LUT_5S;
        bws_pick_t p = bws_pick(five ? BWS_FASTEST : BWS_FAST, in->native_fast, in->lut10, five && in->lut5);
        if (p.lut == BWS_LUT_NONE)
            break; /* the band's LUT is not built in */
        p.why = BWS_WHY_TEMP;
        return p;
    }
    return native(in, BWS_WHY_TEMP);
}
