/*
 * epd_bwspeed.h - refresh speed of the BWRY panel (PicPak, epd_bwry.h): device config `refresh_speed` to a driver
 * setting. Hardware-free, host-tested; the simulator times its refreshes with it.
 *
 *   native   the OTP waveform as the controller picks it (~18 s; `!tp bwry fast` still applies)
 *   fast     a register LUT of ~10 s if one is built in, else BWRY_FAST_E6P (OTP waveform forced hot, Good Display's
 *            "Fast update mode (12s)" for the JD79668 successor panel)
 *   fastest  a register LUT of ~5 s if built in, else as fast
 *   auto     "fast when warm enough": by the panel temperature (BWS_BANDS), a register LUT only (built in, REV allowed);
 *            otherwise the native OTP waveform. Offered only with LUTs built in (report `features` `refresh_auto`)
 * Register LUTs: the vendor's "5s" / "10s" (stock V1.1.21+, docs/hardware/picpak-refresh.md; tesserae ships the same
 * bytes); not in this repository. tools/diag/picpak_fw.py writes them from a stock image into
 * third_party/picpak/bwry_luts.h (AGPL-3.0-or-later from picpak-tesserae-client, see docs/spec/firmware.md "PicPak refresh speed"). LUTs need the
 * stock init profile (its PSR scans bottom-up, as their PSR 0x07 0xA9 does). Colors (yellow, red) may come out
 * weaker in the fast modes and vary per panel.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_EPD_BWSPEED_H
#define TINY_EPD_BWSPEED_H

#include <stdbool.h>
#include <stdint.h>

enum { BWS_NATIVE = 0, BWS_FAST = 1, BWS_FASTEST = 2, BWS_AUTO = 3 };
enum { BWS_LUT_NONE = 0, BWS_LUT_10S = 10, BWS_LUT_5S = 5 };

#define BWS_E6P_MS   12000 /* expected full refresh, OTP forced hot */
#define BWS_LUT10_MS 10000
#define BWS_LUT5_MS  5000

typedef struct {
    uint8_t fast;  /* bwry_fast_t for the OTP path (ignored with a LUT) */
    uint8_t lut;   /* BWS_LUT_* */
    uint32_t ms;   /* expected refresh; 0 = the native waveform (board's figure) */
    uint8_t why;   /* BWS_WHY_*: what decided (bws_choose) */
} bws_pick_t;

/* Why a LUT or not (log `why=`): set = the speed as configured; temp = the panel temperature (auto); rev = the panel
 * REV does not allow LUTs; off = no LUTs (not built in, or `!tp lut off`). */
enum { BWS_WHY_SET = 0, BWS_WHY_TEMP = 1, BWS_WHY_REV = 2, BWS_WHY_OFF = 3 };

/* speed BWS_*; native_fast: the `!tp bwry fast` setting; lut10 / lut5: those LUTs are built in (and usable: stock
 * init profile). */
bws_pick_t bws_pick(uint8_t speed, uint8_t native_fast, bool lut10, bool lut5);
const char *bws_name(uint8_t speed);        /* "native", "fast", "fastest" */

/* Register LUTs only on panels whose REV (0x70, 3 bytes read 3-wire on SDA) is 06 01 01: stock V1.1.21 and later
 * (docs/hardware/picpak-refresh.md) load the vendor LUT for that REV only and fall back to the OTP waveform for any
 * other. mode: BWS_REV_AUTO (that rule), BWS_REV_FORCE (LUT on any REV), BWS_REV_OFF (never a LUT). */
enum { BWS_REV_AUTO = 0, BWS_REV_FORCE = 1, BWS_REV_OFF = 2 };
bool bws_lut_allowed(uint8_t mode, bool rev_read, const uint8_t rev[3]);
const char *bws_pick_name(const bws_pick_t *p); /* "otp", "e6p", "lut10", "lut5" ... */
const char *bws_lut_name(const bws_pick_t *p);  /* "5s", "10s", "otp" (log `lut=`) */
const char *bws_why_name(uint8_t why);          /* "set", "temp", "rev", "off" */

/* Temperature bands of `auto`, warmest first: at or above min_c the LUT (a missing 5 s LUT falls back to the 10 s one);
 * below the last band, or no plausible reading (BWS_TEMP_MIN..BWS_TEMP_MAX), the OTP waveform. The OTP waveform is
 * temperature compensated, a register LUT is not (colder = slower ink: weak yellow/red, ghosting). Tune after the
 * hardware test (docs/hardware/picpak-refresh.md). */
typedef struct {
    int8_t min_c;
    uint8_t lut; /* BWS_LUT_* */
} bws_band_t;
#define BWS_BAND_WARM_C 15
#define BWS_BAND_COOL_C 5
/* the table (epd_bwspeed.c): {BWS_BAND_WARM_C, 5 s}, {BWS_BAND_COOL_C, 10 s} */
#define BWS_TEMP_MIN (-25)
#define BWS_TEMP_MAX 60
#define BWS_TEMP_NONE (-128) /* not read, or the read failed */

typedef struct {
    uint8_t speed;       /* BWS_* (config refresh_speed) */
    uint8_t native_fast; /* `!tp bwry fast` */
    bool lut10, lut5;    /* built in and usable (stock init profile) */
    uint8_t lut_mode;    /* BWS_REV_* (`!tp lut`); force ignores the REV and the temperature */
    bool rev_read;
    uint8_t rev[3];
    int16_t temp_c;      /* panel sensor (TSC), BWS_TEMP_NONE = none */
} bws_in_t;

/* The temperature to decide with: this refresh's reading when plausible, else the last plausible one (hal_esp keeps
 * it from the reads before and after refreshes, over deep sleep) when at most BWS_TEMP_CACHE_S old, else
 * BWS_TEMP_NONE. PicPak 2026-09-28: the read right after a boot failed, the ones after a refresh worked. */
#define BWS_TEMP_CACHE_S 3600
bool bws_temp_ok(int16_t c);
int16_t bws_temp_use(int16_t read_c, int16_t cached_c, uint32_t cached_age_s);

/* A register LUT may be picked: read the panel temperature first (auto uses it; the others log it). */
bool bws_wants_temp(const bws_in_t *in);
bws_pick_t bws_choose(const bws_in_t *in);

#endif
