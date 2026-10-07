/* hal_esp: the experimental fast black/white partial of the PicPak (panel_bwfast.h; epd_bwfast.h holds the LUT builder,
 * its hard limits and the policy, host-tested). The partial path of panel_bwry.c asks for the LUT before
 * bwry_part_begin (the init of that partial sends it) and puts the full LUT back after it; every window row passes
 * the colour gate; a yellow or red code drops the partial and forces a full one next. Log lines: `panel bwfast lut
 * pulses= records= skel= b= w= temp=` when one goes out, `panel bwfast no why=` when refused, `E panel bwfast refused:
 * colour in the window`. Text command (saved in NVS "bwfast", restarts):
 *   !tp bwfast                                   show the setting
 *   !tp bwfast on|off [pulses 1..8] [records 1..2] [skel plain|full] [min <C>]   (defaults 4, 1, plain, 15)
 * Needs `!tp part on` (the app only sends partials then), a register LUT for the full refreshes (refresh_speed auto,
 * fast or fastest with LUTs built in, REV allowed) and a panel at or above `min`. Issue #190 and
 * docs/hardware/picpak-refresh.md "Fast black/white partial" (the device check list).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "panel_bwfast.h"

#include <stdlib.h>
#include <string.h>

#include "hal_esp_priv.h"

#if BOARD_PANEL_BWRY /* whole file */

#include "epd_bwfast.h"
#include "epd_bwspeed.h"

static bwf_set_t s_set; /* NVS "bwfast" */
static bool s_loaded;
static uint8_t s_lut[BWF_LUT_BYTES];
static bool s_active, s_colour;

static void load(void)
{
    s_loaded = true;
    s_set = (bwf_set_t){0, BWF_PULSES_DEFAULT, BWF_RECORDS_DEFAULT, BWF_SKEL_PLAIN, BWF_MIN_C_DEFAULT};
    bwf_set_t v;
    if (hal_kv_get("bwfast", &v, sizeof v) == (int)sizeof v && bwf_set_valid(&v))
        s_set = v;
}

bool bwfast_on(void)
{
    if (!s_loaded)
        load();
    return s_set.on != 0;
}

static const char *skel_name(uint8_t s) { return s == BWF_SKEL_FULL ? "full" : "plain"; }

const uint8_t *bwfast_lut(uint8_t every, const bwp_state_t *ps, int16_t temp_c, const uint8_t *full)
{
    s_active = s_colour = false;
    if (!bwfast_on())
        return NULL;
    int why = bwf_why(&s_set, every, ps, temp_c, full != NULL);
    if (why != BWF_WHY_OK) {
        hal_logf("panel bwfast no why=%s temp=%d n=%u/%u need_full=%d", bwf_why_name(why), temp_c,
                 (unsigned)ps->since_full, (unsigned)every, ps->need_full);
        return NULL;
    }
    bwf_cfg_t c = {s_set.pulses, s_set.records, s_set.skeleton};
    int rc = bwf_build(&c, full, s_lut);
    if (rc != BWF_OK) {
        hal_logf("E panel bwfast lut refused: %s", bwf_err_name(rc));
        return NULL;
    }
    bwf_stat_t st;
    bwf_stat(s_lut, &st);
    hal_logf("panel bwfast lut pulses=%u records=%u skel=%s b=%u w=%u temp=%d", (unsigned)s_set.pulses,
             (unsigned)s_set.records, skel_name(s_set.skeleton), (unsigned)st.t[BWF_T_BLACK].b,
             (unsigned)st.t[BWF_T_WHITE].w, temp_c);
    s_active = true;
    return s_lut;
}

bool bwfast_active(void) { return s_active; }

void bwfast_row(const uint8_t *row, int bytes)
{
    if (s_active && !s_colour && bwf_row_has_colour(row, bytes))
        s_colour = true;
}

bool bwfast_colour(void) { return s_active && s_colour; }

void bwfast_done(void) { s_active = false; }

const char *bwfast_name(void) { return s_active ? "bwfast" : "full"; }

void bwfast_show(void)
{
    if (!s_loaded)
        load();
    hal_logf("panel bwfast=%s pulses=%u records=%u skel=%s min_c=%d", s_set.on ? "on" : "off", (unsigned)s_set.pulses,
             (unsigned)s_set.records, skel_name(s_set.skeleton), s_set.min_c);
}

static bool num(const char *v, long lo, long hi, long *out)
{
    char *end = NULL;
    long k = strtol(v, &end, 10);
    if (!end || end == v || *end || k < lo || k > hi)
        return false;
    *out = k;
    return true;
}

int bwfast_cmd(int argc, char **argv)
{
    if (!s_loaded)
        load();
    if (argc == 1) {
        bwfast_show();
        return 0;
    }
    bwf_set_t s = s_set;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
        long k;
        if (!strcmp(a, "on") || !strcmp(a, "off")) {
            s.on = !strcmp(a, "on");
        } else if (!strcmp(a, "pulses") && num(v, 1, BWF_MAX_PULSES, &k)) {
            s.pulses = (uint8_t)k;
            i++;
        } else if (!strcmp(a, "records") && num(v, 1, BWF_MAX_RECORDS, &k)) {
            s.records = (uint8_t)k;
            i++;
        } else if (!strcmp(a, "min") && num(v, BWS_TEMP_MIN, BWS_TEMP_MAX, &k)) {
            s.min_c = (int8_t)k;
            i++;
        } else if (!strcmp(a, "skel") && (!strcmp(v, "plain") || !strcmp(v, "full"))) {
            s.skeleton = !strcmp(v, "full") ? BWF_SKEL_FULL : BWF_SKEL_PLAIN;
            i++;
        } else {
            hal_logf("E usage: bwfast [on|off] [pulses 1..%d] [records 1..%d] [skel plain|full] [min %d..%d]",
                     BWF_MAX_PULSES, BWF_MAX_RECORDS, BWS_TEMP_MIN, BWS_TEMP_MAX);
            return 0;
        }
    }
    if (!bwf_set_valid(&s)) {
        hal_logf("E bwfast: pulses x records at most %d", BWF_MAX_DRIVE);
        return 0;
    }
    hal_kv_set("bwfast", &s, sizeof s);
    s_set = s;
    if (s.on)
        hal_logf("W EXPERIMENTAL fast black/white partial (issue #190): needs `!tp part on`, a register LUT and a warm "
                 "panel; a full refresh after `part every` partials; watch the window for ghosting and colour");
    return 1;
}

#endif /* BOARD_PANEL_BWRY */
