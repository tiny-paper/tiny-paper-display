/* epd_bwfast.c - experimental fast black/white partial LUT for the BWRY panel (epd_bwfast.h). C11.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_bwfast.h"

#include <string.h>

#include "epd_bwspeed.h"

#define AT(t, k, c) (((t) * BWF_RECORDS + (k)) + 48 * (c))

static uint8_t sym(uint8_t b, int field) { return (uint8_t)((b >> (6 - 2 * field)) & 3); }

static uint8_t pack(const uint8_t s[4]) { return (uint8_t)((s[0] << 6) | (s[1] << 4) | (s[2] << 2) | s[3]); }

/* 01 <-> 10, the rest as is */
static uint8_t mirror(uint8_t b)
{
    uint8_t s[4];
    for (int f = 0; f < 4; f++) {
        uint8_t v = sym(b, f);
        s[f] = v == BWF_SYM_WHITE ? BWF_SYM_BLACK : v == BWF_SYM_BLACK ? BWF_SYM_WHITE : v;
    }
    return pack(s);
}

static bool cfg_ok(const bwf_cfg_t *c)
{
    return c && c->pulses >= 1 && c->pulses <= BWF_MAX_PULSES && c->records >= 1 && c->records <= BWF_MAX_RECORDS &&
           c->skeleton <= BWF_SKEL_FULL && (unsigned)c->pulses * c->records <= BWF_MAX_DRIVE;
}

int bwf_build(const bwf_cfg_t *c, const uint8_t *full, uint8_t out[BWF_LUT_BYTES])
{
    if (!cfg_ok(c) || !full || !out)
        return BWF_E_CFG;
    memset(out, 0xff, BWF_WAVE);
    memcpy(out + BWF_WAVE, full + BWF_WAVE, BWF_ANALOG);
    uint8_t drive[2][2]; /* [black, white][byte 1, byte 2] */
    for (int by = 0; by < 2; by++) {
        uint8_t sb[4], sw[4];
        for (int f = 0; f < 4; f++) {
            bool on = by * 4 + f < c->pulses;
            sb[f] = on ? BWF_SYM_BLACK : BWF_SYM_IDLE;
            sw[f] = on ? BWF_SYM_WHITE : BWF_SYM_IDLE;
        }
        drive[0][by] = pack(sb);
        drive[1][by] = pack(sw);
    }
    for (int t = 0; t < BWF_TABLES; t++) {
        for (int k = 0; k < c->records; k++) {
            /* the skeleton: the carrier and idle fields, or bytes 0, 3..10 of record 1 of the same table of the full LUT
             * (the record all four colour tables share, issue #190) */
            for (int b = 0; b < BWF_RECORD; b++) {
                if (b == 1 || b == 2)
                    continue;
                out[AT(t, k, b)] = c->skeleton == BWF_SKEL_FULL ? full[AT(t, 1, b)] : b == 0 ? BWF_CARRIER : 0xff;
            }
            if (t == BWF_T_BLACK || t == BWF_T_WHITE) {
                out[AT(t, k, 1)] = drive[t == BWF_T_WHITE][0];
                out[AT(t, k, 2)] = drive[t == BWF_T_WHITE][1];
            }
        }
    }
    return bwf_check(out);
}

void bwf_stat(const uint8_t *lut, bwf_stat_t *st)
{
    memset(st, 0, sizeof *st);
    for (int t = 0; t < BWF_TABLES; t++) {
        bwf_tstat_t *s = &st->t[t];
        for (int k = 0; k < BWF_RECORDS; k++) {
            bool active = false;
            for (int b = 0; b < BWF_RECORD; b++)
                active |= lut[AT(t, k, b)] != 0xff;
            s->active = (uint8_t)(s->active + active);
            for (int b = 1; b <= 2; b++)
                for (int f = 0; f < 4; f++) {
                    uint8_t v = sym(lut[AT(t, k, b)], f);
                    if (v == BWF_SYM_WHITE) s->w++;
                    else if (v == BWF_SYM_BLACK) s->b++;
                    else if (v == BWF_SYM_THIRD) s->third++;
                }
        }
    }
}

int bwf_check(const uint8_t *lut)
{
    bwf_stat_t st;
    bwf_stat(lut, &st);
    for (int t = 0; t < BWF_TABLES; t++) {
        const bwf_tstat_t *s = &st.t[t];
        if (s->active > BWF_MAX_RECORDS || s->w + s->b + s->third > BWF_MAX_DRIVE)
            return BWF_E_LONG;
        if (t != BWF_T_BLACK && t != BWF_T_WHITE && (s->w || s->b || s->third))
            return BWF_E_HOLD;
    }
    if (st.t[BWF_T_BLACK].w || st.t[BWF_T_BLACK].third || st.t[BWF_T_WHITE].b || st.t[BWF_T_WHITE].third)
        return BWF_E_UNBALANCED;
    for (int k = 0; k < BWF_RECORDS; k++) {
        for (int b = 1; b <= 2; b++)
            if (mirror(lut[AT(BWF_T_BLACK, k, b)]) != lut[AT(BWF_T_WHITE, k, b)])
                return BWF_E_UNBALANCED;
        for (int b = 0; b < BWF_RECORD; b++) {
            if (b == 1 || b == 2)
                continue;
            for (int t = 1; t < BWF_TABLES; t++)
                if (lut[AT(t, k, b)] != lut[AT(0, k, b)])
                    return BWF_E_SKEL;
        }
        /* a record with drive needs its carrier: the all-0xFF record ends a table */
        if ((lut[AT(BWF_T_BLACK, k, 1)] != 0xff || lut[AT(BWF_T_BLACK, k, 2)] != 0xff) && lut[AT(0, k, 0)] == 0xff)
            return BWF_E_SKEL;
    }
    return BWF_OK;
}

const char *bwf_err_name(int rc)
{
    switch (rc) {
    case BWF_OK: return "ok";
    case BWF_E_CFG: return "cfg";
    case BWF_E_LONG: return "long";
    case BWF_E_UNBALANCED: return "unbalanced";
    case BWF_E_HOLD: return "hold";
    case BWF_E_SKEL: return "skeleton";
    default: return "?";
    }
}

bool bwf_row_has_colour(const uint8_t *row, int bytes)
{
    for (int i = 0; i < bytes; i++)
        if (row[i] & 0xaa) /* bit 1 of a code: 2 yellow, 3 red */
            return true;
    return false;
}

bool bwf_set_valid(const bwf_set_t *s)
{
    bwf_cfg_t c = {s->pulses, s->records, s->skeleton};
    return s->on <= 1 && cfg_ok(&c) && s->min_c >= BWS_TEMP_MIN && s->min_c <= BWS_TEMP_MAX;
}

int bwf_why(const bwf_set_t *s, uint8_t every, const bwp_state_t *ps, int16_t temp_c, bool full_lut)
{
    if (!s || !s->on || !bwf_set_valid(s))
        return BWF_WHY_OFF;
    if (!full_lut)
        return BWF_WHY_NO_LUT;
    if (ps->need_full)
        return BWF_WHY_NEED_FULL;
    if (every && ps->since_full >= every)
        return BWF_WHY_EVERY;
    if (!bws_temp_ok(temp_c) || temp_c < s->min_c)
        return BWF_WHY_TEMP;
    return BWF_WHY_OK;
}

const char *bwf_why_name(int why)
{
    static const char *const n[] = {"ok", "off", "need_full", "every", "temp", "no_lut"};
    return why >= 0 && why < (int)(sizeof n / sizeof n[0]) ? n[why] : "?";
}
