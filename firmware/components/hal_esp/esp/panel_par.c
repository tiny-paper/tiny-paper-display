/* hal_esp: parallel panel through FastEPD (BOARD_PANEL_FASTEPD: TRMNL X, 1872x1404) and the "!tp" text commands.
 * FastEPD keeps its frame buffers in PSRAM (components/epd epd_par.h); the app renders whole frames (no bands,
 * firmware/app/CMakeLists.txt) and every hal_panel_write copies into them:
 *   full, depth 4    16-gray full refresh, rows as they come (4 bpp, 0 black .. 15 white, high nibble first:
 *                    the same packing as gfx)
 *   full, depth 2    the same, each 2-bit gray v as the 4-bit gray 5 * v
 *   full, depth 1    1-bit full refresh (FastEPD 1 bpp)
 *   partial          1 bpp only (the app sends hybrid partials as 1-bit): both frames start white, the window
 *                    gets the old pixels in FastEPD's previous frame and the new ones in its current frame,
 *                    so only changed pixels are driven and everything else (grays too) stays as it is
 *                    (plus, with a redrive budget, blacks made by the last few partials: epd_parwave.h; that
 *                    bitmap, two planes of counts, is kept across deep sleep in LittleFS /xfresh.bin, run-length
 *                    coded, written after a partial that changed it, removed at every full refresh).
 * Every update powers the panel rails off when done (epd_par.h).
 * Text commands (they apply at the next refresh, no reboot, and last until the next reset; only `gpart` is
 * stored, NVS "dgpart", see load_cfg):
 *   !tp panel                         show the settings
 *   !tp passes <partial> <full>       FastEPD passes, 1..14 (stock 3 3). More partial passes: darker digits, less
 *                                     ghosting of the old ones; fewer: faster, more ghosting
 *   !tp black <kick> <extra> [r|on|off] 1 bpp partial waveform around the passes (NVS "xwave", epd_parwave.h):
 *                                     kick 0..14 frames pushing new blacks white first, extra 0..14 black-only
 *                                     frames after the passes, r 0..3 (on = the board's, off = 0): each black
 *                                     made by a partial is pushed black again in the kick frames of the next r
 *                                     partials (unbounded in 1134: lines, epd_parwave.h). Each frame ~53 ms,
 *                                     each nonzero step one more; the log line after a partial shows frames and
 *                                     ms per step and redrive=<px>@<rows>
 *   !tp every <n>                     a full refresh after n partials, whatever the plugin's cadence (safety net
 *                                     against lines and ghosting, NVS "xevery", 0 = off, 1..255)
 *   !tp rows <settle> <hold> [drive]  partial row step delays, 0..20 us each (NVS "xrow", epd_rowtime.h): CKV off to
 *                                     LE, and LE to the next row's CKV. 0 0 = FastEPD as is. Every log line after
 *                                     an update shows rowmax=<us>@<row> and slow=<n>[rows] (stalls inside a frame).
 *                                     drive 0..200 us (NVS "xdrive", default 32): each partial row's CKV is an RMT
 *                                     pulse of exactly this length (a stall cannot overdrive the row), 0 = CKV on
 *                                     until the next row (FastEPD). A "panel rows" line follows every update:
 *                                     drive=<set>us high=<min>/<avg>/<max> (measured CKV high per row), waited,
 *                                     absorbed (stalls that went to CKV low), over (rows driven > 150 us)
 *   !tp bus <0|stock|mhz>             LCD bus from the next boot (NVS "xbus"): 0 = FastEPD 8f76e90's TRMNL X setting
 *                                     (26.67 MHz, hardware chip select; default), 10..40 = that clock with hardware
 *                                     chip select, stock = 8dc8c74 as stock pins it (20 MHz, SPH by software)
 *   !tp gpart <on|off> [dark] [light] gray partial (experimental, NVS "dgpart", default on): when a partial
 *                                     changes grays (depth 2/4, gray mode hybrid), the gray table runs on the
 *                                     changed pixels only (epd_parwave.h EPD_GP_*), no clearing flash, instead
 *                                     of a full refresh. dark/light 0..14 (default 0 0: light frames do nothing for pixels that start white and cost about 0.7 s): frames pushing the
 *                                     changed pixels dark, then light, first. Log: panel gpart ...
 *   !tp clear <auto|none|fast|slow>   clear passes before a full refresh; auto = slow every 8th full, else fast
 *                                     (stock display.cpp)
 *   !tp gray <fastepd|stock>          16-gray waveform: FastEPD's table, or stock trmnl-firmware's u8_graytable (default)
 *   !tp ramp                          test card at 4 bit, full refresh (to photograph for the tone curve): top 16 steps
 *                                     0 (black) .. 15; bottom: 1-bit checker 50 %, Bayer 4x4 50 %, solid 8, photo levels 0 5 10 15
 *   !tp seconds [on|off]              live seconds (experimental, NVS "xsec", default off): awake on USB the app
 *                                     refreshes the plugin every second between its minute ticks (hal_live_seconds)
 *   !tp box <x> <y> <w> <h> [inv|diff] a 1 bpp partial from the console (issue #203): the window white with a black
 *                                     rectangle inset 16 px; inv (default) drives every pixel of it (the previous
 *                                     frame inverted, as the press mark), diff only the black ones (as a layout
 *                                     tick). The app's next refresh is full. Log: panel win, panel partial, panel given
 *   !tp win <all|diff>                the app's 1 bpp partials drive the changed pixels (diff, default) or every pixel
 *                                     of each window (all, the press mark's shape; NVS "xwinall", issue #203)
 *   !tp grid [inv|diff] [r0 [r1]]     8 x 6 labelled cells over the panel, each its own partial window (rows r0..r1
 *                                     of them, default all): one photo tells which places reach the glass and
 *                                     whether x or y are mirrored (issue #203). The app is not told (a gray_mode
 *                                     full tick wipes the grid, a hybrid tick leaves it). Log: grid cell, grid
 *   !tp touch                         touchbar (IQS323) status, registers and counters
 *   !tp touch watch [s]               for s seconds (default 10, max 60) every touch window and a forced read
 *                                     each second: the raw status bytes (touch and hold the bar meanwhile)
 *   !tp touch init                    run the stock init again
 *   !tp touch <on|off>                touch bar on or off (NVS "xtouch", at the next restart)
 *   !tp fast <1..6>                   test mode: the clock runs k times faster (NVS "fast", 1 = off)
 *   !tp reboot
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fresh_save.h"
#include "panel_cfg.h"
#include "hal_esp_priv.h"

#if BOARD_PANEL_FASTEPD /* whole file */

#include "epd_clear.h"
#include "epd_par.h"
#include "epd_parwave.h"
#include "epd_parwin.h"
#include "epd_scan.h"

enum { CLEAR_AUTO = 0, CLEAR_NONE_ = 1, CLEAR_FAST_ = 2, CLEAR_SLOW_ = 3 };
enum { GRAY_FASTEPD = 0, GRAY_STOCK = 1 };

typedef struct {
    uint8_t partial, full; /* FastEPD passes */
} pass_cfg_t;

static pass_cfg_t s_pass = {BOARD_EPD_PARTIAL_PASSES, BOARD_EPD_FULL_PASSES};

typedef struct {
    uint8_t kick, extra, redrive; /* epdp_set_wave; redrive: budget 0..EPD_WAVE_REDRIVE_MAX */
} wave_cfg_t;

static wave_cfg_t s_wave = {BOARD_EPD_WAVE_KICK, BOARD_EPD_WAVE_EXTRA, BOARD_EPD_WAVE_REDRIVE};
static epd_row_delay_t s_rows = {BOARD_EPD_ROW_SETTLE, BOARD_EPD_ROW_HOLD};
static uint8_t s_drive = EPD_ROW_DRIVE_DEFAULT; /* NVS "xdrive": fixed partial row drive, us, 0 = free */
static uint8_t s_bus_mhz;                       /* NVS "xbus": 0 = 26.67 MHz hardware CS, 1 = stock, 10..40 MHz */
static epd_scan_cfg_t s_scan = EPD_SCAN_CFG_DEFAULT; /* the scans of a partial (epd_scan.h); tuning keys only */

static gpart_cfg_t s_gp = {1, 0, 0}; /* on by default (display firmware): hello lists the gray partial depths; "!tp gpart off" */
/* windows of a gray partial (depth 2/4): 4 bpp copies of old and new pixels, freed after the refresh */
#define GP_MAX 16
static epdp_gwin_t s_gw[GP_MAX];
static int s_ngw;
static bool s_gw_bad; /* a window did not fit: the refresh fails (the app then does a full one) */
static uint8_t s_clear = CLEAR_AUTO;
static uint8_t s_gray = GRAY_STOCK; /* stock trmnl-firmware's table by default (20260930): photos use all 16 grays */
static uint8_t s_live; /* live seconds on (NVS "xsec") */
static uint8_t s_every = BOARD_EPD_FULL_EVERY; /* NVS "xevery": a full refresh after this many partials, 0 off */
static RTC_DATA_ATTR uint16_t s_since_full;   /* partials since the last full refresh (0 after a reset) */
static RTC_DATA_ATTR uint8_t s_fulls; /* full refreshes since power-on, for CLEAR_AUTO */
/* Fresh blacks across deep sleep (epd_parwave.h redrive): FRESH_PATH holds the bitmap, run-length coded,
 * tagged with s_fresh_tag. The tag is new at every full refresh and 0 after power-on or a reset (RTC data
 * is only kept over deep sleep), so a file from another panel state is never loaded. */
static RTC_DATA_ATTR uint32_t s_fresh_tag;
static bool s_fresh_ready; /* loaded (or known empty) this boot */
static long s_fresh_saved;  /* bytes written after the last partial, -1 failed, 0 none */

static uint8_t s_depth;
static hal_refresh_t s_mode;
static bool s_begun;
static int s_y0, s_y1; /* partial: rows written */
static const char *s_clear_used = "";

/* FastEPD 8dc8c74 u8GrayMatrix (Apache-2.0), 16 grays x 8 passes: FastEPD's default for BB_PANEL_TRMNL_X.
 * Kept here because FastEPD has no call to go back to it after setCustomMatrix. */
static const uint8_t GRAY_FASTEPD_TAB[] = {
    2, 2, 1, 1, 1, 1, 1, 1, /**/ 2, 2, 2, 2, 1, 1, 2, 1, /**/ 2, 2, 2, 1, 1, 1, 1, 2, /**/ 2, 2, 2, 1, 1, 1, 1, 2,
    2, 2, 2, 2, 1, 1, 1, 2, /**/ 2, 2, 2, 2, 1, 2, 2, 1, /**/ 2, 2, 1, 1, 1, 2, 1, 2, /**/ 2, 2, 2, 1, 1, 2, 1, 2,
    1, 1, 1, 1, 1, 1, 2, 2, /**/ 2, 1, 1, 1, 1, 1, 2, 2, /**/ 2, 2, 1, 1, 1, 1, 2, 2, /**/ 2, 2, 2, 1, 1, 1, 2, 2,
    2, 1, 1, 2, 1, 1, 2, 2, /**/ 2, 2, 2, 2, 1, 1, 2, 2, /**/ 2, 2, 2, 2, 2, 1, 2, 2, /**/ 2, 2, 2, 2, 2, 2, 2, 2,
};
/* Stock trmnl-firmware f5f87b7 src/display.cpp u8_graytable (GPL-3.0), 16 grays x 9 passes. */
static const uint8_t GRAY_STOCK_TAB[] = {
    0, 0, 0, 0, 0, 0, 1, 1, 1, /**/ 0, 0, 1, 1, 1, 2, 2, 1, 1, /**/ 0, 0, 0, 0, 1, 2, 2, 1, 1,
    1, 1, 2, 2, 1, 1, 1, 1, 2, /**/ 0, 0, 0, 1, 2, 1, 1, 1, 2, /**/ 1, 2, 2, 2, 2, 1, 1, 1, 2,
    0, 0, 1, 1, 2, 2, 1, 1, 2, /**/ 0, 1, 1, 2, 1, 1, 2, 1, 2, /**/ 0, 1, 1, 1, 2, 1, 2, 1, 2,
    0, 1, 1, 1, 1, 2, 2, 1, 2, /**/ 1, 1, 1, 2, 1, 1, 1, 2, 2, /**/ 0, 0, 1, 2, 1, 1, 1, 2, 2,
    0, 0, 0, 1, 2, 1, 1, 2, 2, /**/ 0, 0, 0, 0, 1, 2, 1, 2, 2, /**/ 0, 1, 1, 1, 2, 2, 2, 2, 2,
    0, 0, 0, 0, 0, 0, 0, 0, 2,
};
_Static_assert(sizeof GRAY_FASTEPD_TAB == 16 * 8 && sizeof GRAY_STOCK_TAB == 16 * 9, "gray tables");

/* The display firmware reads one stored setting, the gray partial switch (src/panel_cfg.h); every other key of the
 * firmware this driver came from is left alone, so a board behaves the same on every device. Why, key by key:
 *   xpass xwave xrow xdrive xbus   waveform and bus tuning of experiments: the board's own defaults are the product
 *   xclear xgray2 xevery xsec xwinall   clear and gray table, refresh cadence, live seconds, window drive: same
 * The console commands still change them, for the running session only. */
static void load_cfg(void)
{
    s_gp = panel_gpart_load(s_gp);
    epdp_set_wave(s_wave.kick, s_wave.extra, s_wave.redrive);
    epdp_set_rows(s_rows.settle, s_rows.hold);
    epdp_set_drive(s_drive);
    epdp_set_bus_hz(s_bus_mhz == 0 || s_bus_mhz == 26 || s_bus_mhz == 27 ? EPDP_BUS_HW_CS
                    : s_bus_mhz == 1                                    ? 0
                                                                        : s_bus_mhz * 1000000u);
}

bool hal_live_seconds(void) { return s_live && hal_esp_stay_awake(); }

bool hal_panel_gray_partial(void) { return s_gp.on; }

/* Tuning for experiments (usbdisp `set { tuning }`, docs/spec/usb-display.md "Tuning"): the knobs of the console commands
 * above under one name each, with the bounds those commands check (EPD_* limits, 1..14 passes, 0..14 frames). Applied from
 * the next refresh, never stored. The 1 bit black step before a gray one does not exist in a gray partial (the 4 bit
 * rectangle goes straight to epdp_gray_partial), so there is no knob to skip it. */
static const char *const W_GRAY[] = {"fastepd", "stock"};
static const char *const W_ONOFF[] = {"off", "on"};
static const char *const W_CLEAR[] = {"auto", "none", "fast", "slow"};
static const struct {
    hal_tune_t t;
    uint8_t *v;
} KNOBS[] = {
    {{"gray_table", GRAY_FASTEPD, GRAY_STOCK, GRAY_STOCK, W_GRAY}, &s_gray},
    {{"gray_partial", 0, 1, 1, W_ONOFF}, &s_gp.on},
    {{"gray_dark", 0, 14, 0, NULL}, &s_gp.dark},
    {{"gray_light", 0, 14, 0, NULL}, &s_gp.light},
    {{"passes_partial", 1, 14, BOARD_EPD_PARTIAL_PASSES, NULL}, &s_pass.partial},
    {{"passes_full", 1, 14, BOARD_EPD_FULL_PASSES, NULL}, &s_pass.full},
    {{"wave_kick", 0, 14, BOARD_EPD_WAVE_KICK, NULL}, &s_wave.kick},
    {{"wave_extra", 0, 14, BOARD_EPD_WAVE_EXTRA, NULL}, &s_wave.extra},
    {{"wave_redrive", 0, EPD_WAVE_REDRIVE_MAX, BOARD_EPD_WAVE_REDRIVE, NULL}, &s_wave.redrive},
    {{"row_settle", 0, EPD_ROW_DELAY_MAX, BOARD_EPD_ROW_SETTLE, NULL}, &s_rows.settle},
    {{"row_hold", 0, EPD_ROW_DELAY_MAX, BOARD_EPD_ROW_HOLD, NULL}, &s_rows.hold},
    {{"row_drive", 0, EPD_ROW_DRIVE_MAX, EPD_ROW_DRIVE_DEFAULT, NULL}, &s_drive},
    {{"clear", CLEAR_AUTO, CLEAR_SLOW_, CLEAR_AUTO, W_CLEAR}, &s_clear},
};
#define NKNOBS (sizeof KNOBS / sizeof KNOBS[0])

static void tune_apply(void)
{
    epdp_set_passes(s_pass.partial, s_pass.full);
    epdp_set_wave(s_wave.kick, s_wave.extra, s_wave.redrive);
    epdp_set_rows(s_rows.settle, s_rows.hold);
    epdp_set_drive(s_drive);
    epdp_set_scan(&s_scan);
}

/* After KNOBS come the keys of the scans of a partial, defined and bounded in epd_scan.h (host-tested there and
 * through usbdisp's tuning test); their defaults are the scans as they were before those keys. */
size_t hal_panel_tune_count(void) { return NKNOBS + EPD_SCAN_KEYS; }

const hal_tune_t *hal_panel_tune_info(size_t i)
{
    static hal_tune_t scan[EPD_SCAN_KEYS];
    if (i < NKNOBS)
        return &KNOBS[i].t;
    const epd_scan_key_t *k = epd_scan_key(i - NKNOBS);
    if (!k)
        return NULL;
    scan[i - NKNOBS] = (hal_tune_t){k->name, k->min, k->max, k->def, k->words};
    return &scan[i - NKNOBS];
}

int hal_panel_tune_get(size_t i) { return i < NKNOBS ? *KNOBS[i].v : epd_scan_key_get(&s_scan, i - NKNOBS); }

bool hal_panel_tune_set(size_t i, int v)
{
    if (i >= NKNOBS) {
        if (!epd_scan_key_set(&s_scan, i - NKNOBS, v))
            return false;
    } else if (v < KNOBS[i].t.min || v > KNOBS[i].t.max) {
        return false;
    } else {
        *KNOBS[i].v = (uint8_t)v;
    }
    tune_apply();
    return true;
}

void hal_panel_tune_defaults(void)
{
    for (size_t i = 0; i < NKNOBS; i++)
        *KNOBS[i].v = (uint8_t)KNOBS[i].t.def;
    epd_scan_key_defaults(&s_scan);
    tune_apply();
}

/* "!tp ramp" keeps its test card up for RAMP_HOLD_S (across deep sleep): the app's refreshes are skipped until then,
 * and the first one after it is full (the app's previous frame is not what the panel shows) */
#define RAMP_HOLD_S 120
static RTC_DATA_ATTR time_t s_hold_until;
static RTC_DATA_ATTR bool s_after_hold;
static bool s_held; /* this refresh is skipped */

static bool held(void)
{
    if (!s_hold_until)
        return false;
    if (time(NULL) < s_hold_until)
        return true;
    s_hold_until = 0;
    return false;
}

bool hal_esp_par_partial(void) { return !s_after_hold && !held() && !(s_every && s_since_full >= s_every); }

void hal_esp_par_force_full(void) { s_after_hold = true; (void)hal_board(); }

/* A console partial (panel_par_box.c) is not the app's frame: it runs while the app's next refresh must still be
 * full (s_after_hold stays set for the app). */
static bool s_console;
void hal_esp_par_console(bool on) { s_console = on; }

void hal_esp_par_state_str(char *b, size_t n)
{
    snprintf(b, n, "after_hold=%d held=%d begun=%d open=%d mode=%d depth=%u", (int)s_after_hold, (int)held(),
             (int)s_begun, (int)epdp_is_open(), (int)s_mode, (unsigned)s_depth);
}

static void gw_free(void)
{
    for (int i = 0; i < s_ngw; i++) {
        free((void *)s_gw[i].old4);
        free((void *)s_gw[i].new4);
    }
    s_ngw = 0;
    s_gw_bad = false;
}

void hal_panel_init(void) { load_cfg(); }

bool hal_esp_panel_bottom_up(void) { return false; }

static void apply_gray(void)
{
    int rc = s_gray == GRAY_STOCK ? epdp_set_matrix(GRAY_STOCK_TAB, (int)sizeof GRAY_STOCK_TAB)
                                  : epdp_set_matrix(GRAY_FASTEPD_TAB, (int)sizeof GRAY_FASTEPD_TAB);
    if (rc)
        hal_logf("W panel gray table rc=%d", rc);
}

static int open_panel(void)
{
    if (epdp_is_open())
        return HAL_OK;
    if (hal_x_i2c_init())
        return HAL_E_IO;
    int64_t t = esp_timer_get_time();
    int rc = epdp_open_trmnl_x(s_pass.partial, s_pass.full);
    if (rc) {
        hal_logf("E panel open rc=%d", rc);
        return HAL_E_IO;
    }
    apply_gray();
    bool soft = false;
    uint32_t hz = epdp_bus_hz(&soft);
    hal_logf("I panel open ms=%u %dx%d bus_khz=%u cs=%s drive=%s", (unsigned)((esp_timer_get_time() - t) / 1000),
             epdp_width(), epdp_height(), (unsigned)(hz / 1000), soft ? "soft" : "hw", epdp_drive_ok() ? "fixed" : "free");
    return HAL_OK;
}

static void fresh_load(void)
{
    if (s_fresh_ready)
        return;
    s_fresh_ready = true;
    size_t n;
    uint8_t *f = epdp_fresh(&n);
    long sz = hal_fs_size(FRESH_PATH);
    if (!f || sz <= 12)
        return;
    if (!s_fresh_tag) { /* power-on or reset: the panel may show anything */
        hal_fs_remove(FRESH_PATH);
        return;
    }
    int64_t t = esp_timer_get_time();
    uint8_t *buf = malloc((size_t)sz);
    hal_file_t *fp = buf ? hal_fs_open(FRESH_PATH, "r") : NULL;
    long got = fp ? hal_fs_read(fp, buf, (size_t)sz) : -1;
    if (fp)
        hal_fs_close(fp);
    uint32_t hdr[3] = {0};
    if (got == sz)
        memcpy(hdr, buf, sizeof hdr);
    int rc = got == sz && hdr[0] == FRESH_MAGIC && hdr[1] == s_fresh_tag && hdr[2] == n
                 ? epd_rle_dec(buf + 12, (size_t)sz - 12, f, n)
                 : -1;
    free(buf);
    if (rc)
        memset(f, 0, n);
    hal_logf("%c panel fresh %s bytes=%ld ms=%u", rc ? 'W' : 'I', rc ? "not loaded" : "loaded", sz,
             (unsigned)((esp_timer_get_time() - t) / 1000));
}

static bool s_fresh_ram; /* hal_panel_fresh_ram: the display firmware keeps the bitmap in RAM only */

void hal_panel_fresh_ram(bool ram_only) { s_fresh_ram = ram_only; }

static void fresh_save(void)
{
    s_fresh_saved = 0;
    if (!s_fresh_ram && !epdp_fresh_take_dirty()) /* RAM only: dirty stays set, nothing to write */
        return;
    size_t n;
    const uint8_t *f = epdp_fresh(&n);
    if (!s_fresh_tag)
        s_fresh_tag = esp_random() | 1u;
    s_fresh_saved = fresh_save_file(f, n, s_fresh_tag, s_fresh_ram);
    if (s_fresh_saved < 0)
        hal_logf("W panel fresh not saved");
}

static void stats_rect(const tiny_rect_t *r)
{
    hal_wake_stats_t *s = &hal_stats;
    int x1 = r->x + r->w, y1 = r->y + r->h;
    if (!s->rects) {
        s->bx0 = r->x;
        s->by0 = r->y;
        s->bx1 = (int16_t)x1;
        s->by1 = (int16_t)y1;
    } else {
        if (r->x < s->bx0) s->bx0 = r->x;
        if (r->y < s->by0) s->by0 = r->y;
        if (x1 > s->bx1) s->bx1 = (int16_t)x1;
        if (y1 > s->by1) s->by1 = (int16_t)y1;
    }
    s->rects++;
}

int hal_panel_begin(uint8_t depth, hal_refresh_t mode)
{
    if ((depth != 1 && depth != 2 && depth != 4) || (mode == HAL_REFRESH_PARTIAL && depth != 1 && !s_gp.on))
        return HAL_E_FAIL;
    s_held = held();
    if (!s_held && s_after_hold && !s_console && mode == HAL_REFRESH_PARTIAL)
        return HAL_E_FAIL; /* the app then refreshes fully */
    if (s_held) {
        s_begun = true;
        return HAL_OK;
    }
    gw_free();
    if (open_panel())
        return HAL_E_IO;
    if (mode == HAL_REFRESH_PARTIAL)
        fresh_load();
    int rc = mode == HAL_REFRESH_PARTIAL ? epdp_partial_begin() : epdp_set_depth(depth == 1 ? 1 : 4);
    if (rc) {
        hal_logf("E panel begin rc=%d", rc);
        return HAL_E_IO;
    }
    s_depth = depth;
    s_mode = mode;
    s_y0 = INT_MAX;
    s_y1 = -1;
    hal_stats.refresh_kind = mode == HAL_REFRESH_FULL ? 1 : 2;
    hal_stats.rects = 0;
    s_begun = true;
    return HAL_OK;
}

/* 4 pixels of 2-bit gray (one byte) -> 4 pixels of 4-bit gray (two bytes), v -> 5 * v */
static uint16_t gray2to4(uint8_t b)
{
    uint16_t o = 0;
    for (int k = 6; k >= 0; k -= 2)
        o = (uint16_t)(o << 4 | 5u * ((b >> k) & 3u));
    return o;
}

int hal_panel_write(const tiny_rect_t *r, const uint8_t *prev, const uint8_t *next, size_t stride)
{
    if (!s_begun || !next)
        return HAL_E_FAIL;
    if (s_held)
        return HAL_OK;
    const int W = epdp_width(), H = epdp_height();
    if (r->x < 0 || r->y < 0 || r->x % 8 || r->w % 8 || r->x + r->w > W || r->y + r->h > H)
        return HAL_E_FAIL;
    uint8_t *cur = epdp_buffer();
    const size_t ds = (size_t)epdp_stride();
    stats_rect(r);
    if (s_mode == HAL_REFRESH_PARTIAL && s_depth > 1) { /* gray partial: keep 4 bpp copies */
        if (!prev || s_ngw >= GP_MAX) {
            s_gw_bad = true;
            return HAL_E_FAIL;
        }
        size_t st4 = (size_t)r->w / 2;
        uint8_t *o = malloc(st4 * r->h), *nx = malloc(st4 * r->h);
        if (!o || !nx) {
            free(o);
            free(nx);
            s_gw_bad = true;
            return HAL_E_FAIL;
        }
        for (int j = 0; j < r->h; j++) {
            const uint8_t *sp = prev + (size_t)j * stride, *sn = next + (size_t)j * stride;
            uint8_t *dp = o + (size_t)j * st4, *dn = nx + (size_t)j * st4;
            if (s_depth == 4) {
                memcpy(dp, sp, st4);
                memcpy(dn, sn, st4);
            } else {
                for (int i = 0; i < r->w / 4; i++) {
                    uint16_t a = gray2to4(sp[i]), b = gray2to4(sn[i]);
                    dp[2 * i] = (uint8_t)(a >> 8);
                    dp[2 * i + 1] = (uint8_t)a;
                    dn[2 * i] = (uint8_t)(b >> 8);
                    dn[2 * i + 1] = (uint8_t)b;
                }
            }
        }
        s_gw[s_ngw++] = (epdp_gwin_t){r->x, r->y, r->w, r->h, o, nx, st4};
        if (r->y < s_y0)
            s_y0 = r->y;
        if (r->y + r->h - 1 > s_y1)
            s_y1 = r->y + r->h - 1;
        return HAL_OK;
    }
    if (s_mode == HAL_REFRESH_PARTIAL) {
        epd_win_t win = {r->x, r->y, r->w, r->h};
        const bool all = hal_esp_par_win_all(); /* "!tp win all": every pixel of the window driven (panel_par_box.c) */
        if (!prev || epd_par_copy_1bpp(cur, epdp_prev_buffer(), ds, W, H, &win, next, prev, stride, all))
            return HAL_E_FAIL;
        /* issue #203: what the caller handed over, to compare with what FastEPD gets ("panel given") */
        hal_logf("panel win x=%d y=%d w=%u h=%u diff=%lu drive=%s", r->x, r->y, r->w, r->h,
                 (unsigned long)hal_esp_par_win_diff(prev, next, stride, r->w, r->h), all ? "all" : "diff");
        if (r->y < s_y0)
            s_y0 = r->y;
        if (r->y + r->h - 1 > s_y1)
            s_y1 = r->y + r->h - 1;
        return HAL_OK;
    }
    for (int j = 0; j < r->h; j++) {
        const uint8_t *src = next + (size_t)j * stride;
        uint8_t *dst = cur + (size_t)(r->y + j) * ds;
        if (s_depth == 1) {
            memcpy(dst + r->x / 8, src, r->w / 8u);
        } else if (s_depth == 4) {
            memcpy(dst + r->x / 2, src, r->w / 2u);
        } else {
            uint8_t *d = dst + r->x / 2;
            for (int i = 0; i < r->w / 4; i++) {
                uint16_t v = gray2to4(src[i]);
                d[2 * i] = (uint8_t)(v >> 8);
                d[2 * i + 1] = (uint8_t)v;
            }
        }
    }
    return HAL_OK;
}

static bool s_new_image; /* hal_panel_new_image: the frame being written is new image content */
static char s_clear_buf[24];

void hal_panel_new_image(bool image) { s_new_image = image; }

static int clear_mode(void)
{
    const char *why = "";
    switch (s_clear) {
    case CLEAR_NONE_: s_clear_used = "none"; return EPDP_CLEAR_NONE;
    case CLEAR_FAST_: s_clear_used = "fast"; return EPDP_CLEAR_FAST;
    case CLEAR_SLOW_: s_clear_used = "slow"; return EPDP_CLEAR_SLOW;
    default:
        break;
    }
    /* slow for new image content (hal_panel_new_image) and on every 8th full (and the first after power-on) */
    int m = epd_clear_auto(s_fulls, s_new_image, &why);
    snprintf(s_clear_buf, sizeof s_clear_buf, m == EPDP_CLEAR_SLOW ? "slow why=%s" : "fast", why);
    s_clear_used = s_clear_buf;
    return m;
}

int hal_panel_refresh(void)
{
    hal_esp_heap_mark(HAL_ESP_PH_RENDER);
    if (!s_begun)
        return HAL_E_FAIL;
    if (s_held) {
        s_held = s_begun = false;
        s_after_hold = true;
        hal_logf("I panel: test card shown, refresh skipped for %d s more", (int)(s_hold_until - time(NULL)));
        return HAL_OK;
    }
    if (s_mode == HAL_REFRESH_FULL)
        s_after_hold = false;
    int64_t t = esp_timer_get_time();
    int rc;
    if (s_mode == HAL_REFRESH_PARTIAL && s_depth > 1) {
        const uint8_t *tab = s_gray == GRAY_STOCK ? GRAY_STOCK_TAB : GRAY_FASTEPD_TAB;
        int np = s_gray == GRAY_STOCK ? (int)sizeof GRAY_STOCK_TAB / 16 : (int)sizeof GRAY_FASTEPD_TAB / 16;
        rc = s_gw_bad ? EPDP_E_ARG : s_ngw ? epdp_gray_partial(s_gw, s_ngw, tab, np, s_gp.dark, s_gp.light) : EPDP_OK;
        gw_free();
        hal_stats.refresh_ms = (uint32_t)((esp_timer_get_time() - t) / 1000);
        fresh_save();
        const epdp_gpart_info_t *g = epdp_last_gpart();
        char rb[200];
        hal_logf("panel gpart depth=%u rows=%d..%d ms=%u dark=%u light=%u gray=%s steps=%u frames=%u chg=%lu drv=%lu %s "
                 "rc=%d", s_depth, s_y0, s_y1, (unsigned)hal_stats.refresh_ms, s_gp.dark, s_gp.light,
                 s_gray == GRAY_STOCK ? "stock" : "fastepd", g->steps, g->frames, (unsigned long)g->changed_px,
                 (unsigned long)g->driven_px, hal_esp_par_rows_str(rb, sizeof rb), rc);
        hal_esp_par_log_drive(s_mode == HAL_REFRESH_FULL);
        hal_esp_par_log_scan();
    } else if (s_mode == HAL_REFRESH_PARTIAL) {
        rc = s_y1 >= s_y0 ? epdp_partial(s_y0, s_y1) : EPDP_OK;
        if (rc == EPDP_OK && s_since_full < UINT16_MAX)
            s_since_full++;
        hal_stats.refresh_ms = (uint32_t)((esp_timer_get_time() - t) / 1000);
        int64_t tf = esp_timer_get_time();
        fresh_save();
        unsigned fms = (unsigned)((esp_timer_get_time() - tf) / 1000);
        static int64_t last_log;
        if (rc || !s_live || esp_timer_get_time() - last_log > 60000000) { /* live seconds: once a minute */
            last_log = esp_timer_get_time();
            const epdp_wave_ms_t *w = epdp_last_wave();
            const epd_fresh_info_t *rd = epdp_last_redrive();
            const epdp_given_t *g = epdp_last_given();
            char rb[200];
            hal_logf("panel partial rows=%d..%d ms=%u passes=%u kick=%u/%ums main=%u/%ums extra=%u/%ums redrive=%lu@%d..%d "
                     "rowdly=%u,%u %s fresh=%ld/%ums since_full=%u rc=%d",
                     s_y0, s_y1, (unsigned)hal_stats.refresh_ms, s_pass.partial, w->frames[EPD_WAVE_KICK],
                     w->ms[EPD_WAVE_KICK], w->frames[EPD_WAVE_MAIN], w->ms[EPD_WAVE_MAIN], w->frames[EPD_WAVE_EXTRA],
                     w->ms[EPD_WAVE_EXTRA], (unsigned long)rd->px, rd->y0, rd->y1, s_rows.settle, s_rows.hold,
                     hal_esp_par_rows_str(rb, sizeof rb), s_fresh_saved, fms, (unsigned)s_since_full, rc);
            /* issue #203: the frames FastEPD was given at the drive call and its own state then */
            hal_esp_par_log_given(g);
            hal_esp_par_log_drive(s_mode == HAL_REFRESH_FULL);
            hal_esp_par_log_scan();
        }
    } else {
        rc = epdp_full(clear_mode());
        s_new_image = false;
        s_fulls++;
        if (rc == EPDP_OK)
            s_since_full = 0;
        /* fresh is empty now: a new tag, the old file gone */
        s_fresh_tag = esp_random() | 1u;
        s_fresh_ready = true;
        hal_fs_remove(FRESH_PATH);
        hal_stats.refresh_ms = (uint32_t)((esp_timer_get_time() - t) / 1000);
        char rb[200];
        hal_logf("panel full depth=%u ms=%u clear=%s gray=%s %s rc=%d", s_depth, (unsigned)hal_stats.refresh_ms,
                 s_clear_used, s_gray == GRAY_STOCK ? "stock" : "fastepd", hal_esp_par_rows_str(rb, sizeof rb), rc);
        hal_esp_par_log_drive(s_mode == HAL_REFRESH_FULL);
    }
    return rc == EPDP_OK ? HAL_OK : HAL_E_IO;
}

void hal_panel_sleep(void)
{
    if (epdp_is_open())
        epdp_close(); /* rails off; FastEPD and its buffers stay for the next wake in place */
    s_begun = false;
}

/* ------------------------------------------------------------------ text commands */

static const char *const CLEAR_NAMES[] = {"auto", "none", "fast", "slow"};

static void show_cfg(void)
{
    hal_logf("panel fastepd passes=%u,%u black=%u,%u,%u rows=%u,%u,%u bus=%u gpart=%s,%u,%u clear=%s gray=%s every=%u "
             "since_full=%u seconds=%s clock_fast=%u",
             s_pass.partial, s_pass.full, s_wave.kick, s_wave.extra, s_wave.redrive, s_rows.settle, s_rows.hold, s_drive,
             s_bus_mhz,
             s_gp.on ? "on" : "off", s_gp.dark, s_gp.light, CLEAR_NAMES[s_clear & 3],
             s_gray == GRAY_STOCK ? "stock" : "fastepd", s_every, (unsigned)s_since_full, s_live ? "on" : "off",
             hal_esp_fast());
}

/* "!tp ramp": the gray test card, drawn straight into the 4 bpp frame (0 black .. 15 white, high nibble first) */
static const uint8_t BAYER4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

static uint8_t ramp_level(int x, int y, int W, int H)
{
    if (y < H * 3 / 5)
        return (uint8_t)(x * 16 / W);
    int q = x * 4 / W;
    if (q == 0)
        return ((x ^ y) & 1) ? 15 : 0;
    if (q == 1)
        return BAYER4[y & 3][x & 3] < 8 ? 15 : 0;
    if (q == 2)
        return 8;
    static const uint8_t PL[4] = {0, 5, 10, 15};
    return PL[(x - 3 * W / 4) * 4 / (W - 3 * W / 4)];
}

static void ramp(void)
{
    s_hold_until = 0;
    int rc = hal_panel_begin(4, HAL_REFRESH_FULL);
    if (rc) {
        hal_logf("E ramp: panel begin rc=%d", rc);
        return;
    }
    const int W = epdp_width(), H = epdp_height();
    uint8_t *buf = epdp_buffer();
    const size_t ds = (size_t)epdp_stride();
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x += 2)
            buf[(size_t)y * ds + (size_t)x / 2] = (uint8_t)(ramp_level(x, y, W, H) << 4 | ramp_level(x + 1, y, W, H));
    s_y0 = 0;
    s_y1 = H - 1;
    rc = hal_panel_refresh();
    if (!rc) {
        s_hold_until = time(NULL) + RAMP_HOLD_S;
        s_after_hold = true;
        (void)hal_board(); /* its partial_refresh now false: the app's next real refresh is full */
    }
    hal_logf("%s ramp: 16 steps, checker, Bayer, solid 8, photo levels; kept %d s (rc=%d)", rc ? "E" : "I", RAMP_HOLD_S, rc);
}

static bool parse_int(const char *s, long lo, long hi, long *out)
{
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!end || end == s || *end || v < lo || v > hi)
        return false;
    *out = v;
    return true;
}

bool hal_textcmd(const uint8_t *buf, size_t len)
{
    if (len < 4 || memcmp(buf, "!tp ", 4))
        return false;
    char line[96];
    size_t n = len - 4 < sizeof line - 1 ? len - 4 : sizeof line - 1;
    memcpy(line, buf + 4, n);
    line[n] = 0;
    char *nl = strpbrk(line, "\r\n");
    if (nl)
        *nl = 0;
    char *argv[6];
    int argc = 0;
    for (char *t = strtok(line, " "); t && argc < 6; t = strtok(NULL, " "))
        argv[argc++] = t;
    if (argc == 0 || !strcmp(argv[0], "panel")) {
        show_cfg();
        return true;
    }
    if (!strcmp(argv[0], "passes")) {
        long p, f;
        if (argc != 3 || !parse_int(argv[1], 1, 14, &p) || !parse_int(argv[2], 1, 14, &f)) {
            hal_logf("E usage: passes <partial 1..14> <full 1..14> (now %u %u)", s_pass.partial, s_pass.full);
            return true;
        }
        s_pass = (pass_cfg_t){(uint8_t)p, (uint8_t)f};
        epdp_set_passes(s_pass.partial, s_pass.full); /* live if the panel is open, else at its open */
        show_cfg();
        return true;
    }
    if (!strcmp(argv[0], "black")) {
        long k, x, r = s_wave.redrive;
        bool ok = argc == 3 || argc == 4;
        if (argc == 4) {
            if (!strcmp(argv[3], "on"))
                r = BOARD_EPD_WAVE_REDRIVE ? BOARD_EPD_WAVE_REDRIVE : 2;
            else if (!strcmp(argv[3], "off"))
                r = 0;
            else
                ok = parse_int(argv[3], 0, EPD_WAVE_REDRIVE_MAX, &r);
        }
        if (!ok || !parse_int(argv[1], 0, 14, &k) || !parse_int(argv[2], 0, 14, &x)) {
            hal_logf("E usage: black <kick 0..14> <extra 0..14> [redrive 0..%d|on|off] (now %u %u %u)",
                     EPD_WAVE_REDRIVE_MAX, s_wave.kick, s_wave.extra, s_wave.redrive);
            return true;
        }
        s_wave = (wave_cfg_t){(uint8_t)k, (uint8_t)x, (uint8_t)r};
        epdp_set_wave(s_wave.kick, s_wave.extra, s_wave.redrive); /* from the next partial */
        show_cfg();
        return true;
    }
    if (!strcmp(argv[0], "every")) {
        long e;
        if (argc != 2 || !parse_int(argv[1], 0, 255, &e)) {
            hal_logf("E usage: every <0..255> (a full refresh after n partials, 0 = off; now %u)", s_every);
            return true;
        }
        s_every = (uint8_t)e;
        show_cfg();
        return true;
    }
    if (!strcmp(argv[0], "rows")) {
        long a, b, d = s_drive;
        if ((argc != 3 && argc != 4) || !parse_int(argv[1], 0, EPD_ROW_DELAY_MAX, &a) ||
            !parse_int(argv[2], 0, EPD_ROW_DELAY_MAX, &b) || (argc == 4 && !parse_int(argv[3], 0, EPD_ROW_DRIVE_MAX, &d))) {
            hal_logf("E usage: rows <settle 0..%d> <hold 0..%d> [drive 0..%d] us (now %u %u %u)", EPD_ROW_DELAY_MAX,
                     EPD_ROW_DELAY_MAX, EPD_ROW_DRIVE_MAX, s_rows.settle, s_rows.hold, s_drive);
            return true;
        }
        s_rows = (epd_row_delay_t){(uint8_t)a, (uint8_t)b};
        epdp_set_rows(s_rows.settle, s_rows.hold); /* from the next partial */
        s_drive = (uint8_t)d;
        epdp_set_drive(s_drive);
        show_cfg();
        return true;
    }
    if (!strcmp(argv[0], "bus")) {
        long m = 1;
        if (argc != 2 || (strcmp(argv[1], "stock") && (!parse_int(argv[1], 0, 40, &m) || m == 1 || (m && m < 10)))) {
            hal_logf("E usage: bus <0|stock|10..40> MHz, 0 = 26.67 with hardware CS (now %u; 1 = stock)", s_bus_mhz);
            return true;
        }
        s_bus_mhz = (uint8_t)m;
        show_cfg();
        hal_logf("I bus clock from the next boot ('!tp reboot')");
        return true;
    }
    if (!strcmp(argv[0], "gpart")) {
        long d = s_gp.dark, l = s_gp.light;
        int on = argc >= 2 ? (!strcmp(argv[1], "on") ? 1 : !strcmp(argv[1], "off") ? 0 : -1) : -1;
        if (on < 0 || argc > 4 || (argc >= 3 && !parse_int(argv[2], 0, 14, &d)) ||
            (argc == 4 && !parse_int(argv[3], 0, 14, &l))) {
            hal_logf("E usage: gpart <on|off> [dark 0..14] [light 0..14] (now %s %u %u)", s_gp.on ? "on" : "off",
                     s_gp.dark, s_gp.light);
            return true;
        }
        s_gp = (gpart_cfg_t){(uint8_t)on, (uint8_t)d, (uint8_t)l};
        panel_gpart_save(&s_gp);
        show_cfg();
        if (on)
            hal_logf("I gpart needs gray mode hybrid and a 2 or 4 bit face; from the next tick");
        return true;
    }
    if (!strcmp(argv[0], "clear")) {
        uint8_t v = 0xff;
        for (uint8_t i = 0; argc == 2 && i < 4; i++)
            if (!strcmp(argv[1], CLEAR_NAMES[i]))
                v = i;
        if (v == 0xff) {
            hal_logf("E usage: clear <auto|none|fast|slow>");
            return true;
        }
        s_clear = v;
        show_cfg();
        return true;
    }
    if (!strcmp(argv[0], "ramp")) {
        ramp();
        return true;
    }
    if (!strcmp(argv[0], "gray")) {
        if (argc != 2 || (strcmp(argv[1], "fastepd") && strcmp(argv[1], "stock"))) {
            hal_logf("E usage: gray <fastepd|stock>");
            return true;
        }
        s_gray = !strcmp(argv[1], "stock") ? GRAY_STOCK : GRAY_FASTEPD;
        if (epdp_is_open())
            apply_gray();
        show_cfg();
        return true;
    }
    if (!strcmp(argv[0], "seconds")) {
        if (argc == 2 && (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))) {
            s_live = argv[1][1] == 'n';
        } else if (argc != 1) {
            hal_logf("E usage: seconds [on|off]");
            return true;
        }
        show_cfg();
        if (s_live && !hal_esp_stay_awake())
            hal_logf("I live seconds run only while awake on USB");
        return true;
    }
    if (!strcmp(argv[0], "touch")) {
        hal_x_touch_cmd(argc - 1, argv + 1);
        return true;
    }
    if (!strcmp(argv[0], "fast")) {
        long k;
        if (argc != 2 || !parse_int(argv[1], 1, HAL_ESP_FAST_MAX, &k)) {
            hal_logf("E usage: fast <1..%d> (1 = off)", HAL_ESP_FAST_MAX);
            return true;
        }
        uint8_t b = (uint8_t)k;
        hal_kv_set("fast", &b, 1);
        hal_esp_fast_set((unsigned)k);
        if (k > 1)
            hal_logf("W FAST MODE x%ld: the clock runs %ld times faster; set network Off while testing", k, k);
        show_cfg();
        hal_logf("I fast saved, restarting");
        hal_reboot();
    }
    if (hal_esp_par_box_cmd(argc, argv)) /* "!tp box", "!tp win", "!tp grid" (panel_par_box.c) */
        return true;
    if (!strcmp(argv[0], "reboot")) {
        hal_link_flush();
        esp_restart();
    }
    hal_logf("E unknown command (panel, passes, black, every, rows, bus, gpart, clear, gray, seconds, touch, fast, box, win, grid, reboot)");
    return true;
}

#endif /* BOARD_PANEL_FASTEPD */
