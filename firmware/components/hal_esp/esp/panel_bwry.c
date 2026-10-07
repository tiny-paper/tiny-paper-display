/* hal_esp: panel on BWRY boards (PicPak, BOARD_PANEL_BWRY) through epd_bwry.h, and the "!tp" text commands.
 * Full refresh ~18 s, the panel flashes. Windowed partial (experimental, "!tp part on", NVS "bwpart", off by
 * default): hal_board()->partial_refresh follows bwp_allowed (epd_bwpart.h: not after a cold boot or a failed
 * refresh, and not after `every` partials: a full one clears ghosting first); the app's bands of a partial go
 * out as RAM windows as they come (each reversed on the stock profile), the refresh runs on their union. Same
 * waveform as a full refresh, only the window flashes. Grays go as the dither (hal_panel_gray_partial: the
 * whole window is driven, so a gray pixel inside it must be sent as it is).
 * Box partial (hal_panel_box_partial, the press mark; BOARD_BOX_PARTIAL, off until the hardware check): the same
 * windowed partial without "!tp part on", one window per refresh (epd_bwpart.h bwp_boxes_window), under the same
 * policy (not after a cold boot or a failure, a full after `every` partials: "!tp part every", else 5).
 * No frame buffer (the plugin instance leaves ~14 KB of heap while the app renders): hal_panel_begin opens
 * the controller's data stream (bwry_stream_begin) and every hal_panel_write band goes out as it comes, in
 * panel row order. The stock profile takes rows last first: hal_board()->rows_bottom_up, the app sends the
 * bottom band first and each band's rows go out reversed; the gd profile takes them top first. A write out
 * of that order fails (the frame is dropped, no refresh).
 * Pixels, per row as it goes out (epd_bwrymap.h): depth 1 as is (0 black, 1 white). Depth 2, a gray frame: 0 black,
 * 3 white, the grays 1 and 2 as a 4x4 ordered dither on panel coordinates (5/16 and 11/16 white). Depth 2, a palette
 * frame (hal_panel_palette, docs/spec/assets.md "Palette frames"): 0 black, 1 red, 2 yellow, 3 white.
 * Refresh speed (device config `refresh_speed`, hal_panel_speed; epd_bwspeed.h): native = the setting below; fast
 * and fastest = a built-in register LUT (third_party/picpak/bwry_luts.h: varanu5's, AGPL-3.0-or-later) or the hot OTP waveform
 * (e6p); auto = a LUT by the panel temperature (read before each refresh that may use one, bws_choose), else OTP.
 * The log line after every full refresh shows speed=<config>/<used> lut= why= temp=.
 * Text commands:
 *   !tp bwry                                  show the setting
 *   !tp bwry <stock|gd> [flip <auto|0|1>] [fast <off|tse|e6|e6p>]   save and redraw (NVS "bwry")
 *   !tp panel testcard                        test card (epd_bwcard.h) at the current speed, labelled with the mode
 *   !tp lut [auto|force|off]                  register LUTs (built in only): auto = only on panel REV 06 01 01 as stock
 *                                             V1.1.21 (read once per cold boot), force = any REV; save and redraw (NVS "bwlut")
 *   !tp part [on|off] [every <0..255>]        windowed partial, a full refresh after `every` partials
 *                                             (0 = only the app's full_every); save and redraw (NVS "bwpart")
 *   !tp pll [<01..3f>|off]                    experiment: PLL (0x30) frame rate in place of the profile's or the
 *                                             LUT's (stock 08, LUT 5s 06, 10s 03), then the test card (NVS "bwpll")
 *   !tp bwfast [on|off] [...]                 experiment: a short black/white LUT for the windowed partial, off by
 *                                             default (panel_bwfast.c, issue #190; NVS "bwfast")
 *   !tp fast <1..6>                           test mode: the clock runs k times faster (NVS "fast", 1 = off)
 *   !tp reboot
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "busy_sleep.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "hal_esp_priv.h"

#if BOARD_PANEL_BWRY /* whole file */

#include "epd_bwcard.h"
#include "epd_bwpart.h"
#include "epd_bwry.h"
#include "epd_bwrymap.h"
#include "epd_bwspeed.h"
#include "esp_attr.h"
#include "panel_bwfast.h"

/* Register LUTs (epd_bwspeed.h): third_party/picpak/bwry_luts.h (from picpak-tesserae-client, AGPL-3.0-or-later) defining
 * `static const uint8_t BWRY_LUT10[BWRY_LUT_BYTES]` and/or `BWRY_LUT5[...]` (and BWRY_HAVE_LUT10 / BWRY_HAVE_LUT5). */
#if __has_include("bwry_luts.h")
#include "bwry_luts.h"
#endif
#ifndef BWRY_HAVE_LUT10
#define BWRY_HAVE_LUT10 0
static const uint8_t *const BWRY_LUT10 = NULL;
#endif
#ifndef BWRY_HAVE_LUT5
#define BWRY_HAVE_LUT5 0
static const uint8_t *const BWRY_LUT5 = NULL;
#endif

#ifndef BOARD_BOX_PARTIAL
#define BOARD_BOX_PARTIAL 0
#endif

#ifndef BOARD_BWRY_FAST
#define BOARD_BWRY_FAST BWRY_FAST_OFF
#endif

_Static_assert(BOARD_EPD_W == BWRY_W && BOARD_EPD_H == BWRY_H, "board.h panel size must match epd_bwry.h");

/* NVS "bwry" */
typedef struct {
    uint8_t init; /* bwry_init_t */
    int8_t flip;  /* -1 auto, 0, 1 */
    uint8_t fast; /* bwry_fast_t */
} bwry_nvs_t;

/* NVS "bwpart": bwp_cfg_t {mode, every} */
#define PART_EVERY_DEFAULT 5 /* Good Display: a full refresh after every 5 fast or partial ones */

static bwry_nvs_t s_cfg;
static uint8_t s_speed; /* BWS_*, from the app (config refresh_speed) */
static bwp_cfg_t s_part;
static bool s_cfg_loaded;
static uint8_t s_depth;
static bool s_palette, s_palette_next; /* this frame, the next hal_panel_begin's (hal_panel_palette) */
static bool s_begun;
static hal_refresh_t s_mode;
static bwp_acc_t s_acc;
static bool s_acc_bad;
/* kept over deep sleep; a cold boot (RTC data loaded again) starts with a full refresh */
static RTC_DATA_ATTR bwp_state_t s_ps = {0, true};
/* Panel REV (0x70) for the register LUT gate (bws_lut_allowed), read once per cold boot, only with LUTs built in */
static RTC_DATA_ATTR uint8_t s_rev[3];
static RTC_DATA_ATTR bool s_rev_read;
static uint8_t s_lut_mode; /* BWS_REV_*, NVS "bwlut" */
static uint8_t s_pll;      /* PLL override (bwry_cfg_t.pll), NVS "bwpll"; 0 = off */
/* panel temperature for this refresh (measure_temp), BWS_TEMP_NONE when not read or the read failed */
static int16_t s_temp = BWS_TEMP_NONE;

static const bwry_pins_t *panel_pins(void)
{
    static const bwry_pins_t p = {BOARD_EPD_SCK, BOARD_EPD_MOSI, BOARD_SPI_MISO, BOARD_EPD_CS, BOARD_EPD_DC,
                                  BOARD_EPD_RST, BOARD_EPD_BUSY, BOARD_IMU_CS, BOARD_EPD_SPI_HZ};
    return &p;
}

/* As stock V1.1.21: 3 bytes of 0x70 on SDA (3-wire), bit-banged while the driver is closed. */
static void read_rev(void)
{
    if (s_rev_read || s_lut_mode != BWS_REV_AUTO || !(BWRY_HAVE_LUT5 || BWRY_HAVE_LUT10) || bwry_is_open())
        return;
    s_rev_read = bwry_read_reg(panel_pins(), 0x70, s_rev, sizeof s_rev, false) == BWRY_OK;
    hal_logf("panel rev=%02x%02x%02x read=%d lut=%s", s_rev[0], s_rev[1], s_rev[2], s_rev_read,
             bws_lut_allowed(s_lut_mode, s_rev_read, s_rev) ? "ok" : "no");
}

static void load_cfg(void)
{
    s_cfg_loaded = true;
    s_cfg = (bwry_nvs_t){BWRY_INIT_STOCK, -1, BOARD_BWRY_FAST};
    bwry_nvs_t c;
    if (hal_kv_get("bwry", &c, sizeof c) == (int)sizeof c && c.init <= BWRY_INIT_GD && c.flip >= -1 && c.flip <= 1 &&
        c.fast <= BWRY_FAST_E6P)
        s_cfg = c;
    s_part = (bwp_cfg_t){0, PART_EVERY_DEFAULT};
    bwp_cfg_t p;
    if (hal_kv_get("bwpart", &p, sizeof p) == (int)sizeof p && p.mode <= 1)
        s_part = p;
    uint8_t m;
    s_lut_mode = hal_kv_get("bwlut", &m, 1) == 1 && m <= BWS_REV_OFF ? m : BWS_REV_AUTO;
    s_pll = hal_kv_get("bwpll", &m, 1) == 1 && m >= 0x01 && m <= 0x3f ? m : 0;
    read_rev();
}

void hal_panel_init(void) { load_cfg(); }

void hal_panel_palette(uint8_t palette) { s_palette_next = palette != 0; }

/* LUTs scan as the stock profile; bws_choose applies the REV gate and, for auto, the temperature bands */
static bws_in_t speed_in(void)
{
    bool stock = s_cfg.init == BWRY_INIT_STOCK;
    return (bws_in_t){s_speed, s_cfg.fast, stock && BWRY_HAVE_LUT10, stock && BWRY_HAVE_LUT5, s_lut_mode, s_rev_read,
                      {s_rev[0], s_rev[1], s_rev[2]}, s_temp};
}

static bws_pick_t speed_pick(void)
{
    bws_in_t in = speed_in();
    return bws_choose(&in);
}

bool hal_esp_bwry_luts(void) { return BWRY_HAVE_LUT5 || BWRY_HAVE_LUT10; }

/* Before a refresh that may use a register LUT: the panel's own sensor (bwry_read_temp, driver closed). Not read
 * otherwise (no extra reset). An implausible value counts as none (bws_choose: OTP in auto). */
/* the last plausible reading and when (hal_rtc_us), kept over deep sleep (bws_temp_use) */
static RTC_DATA_ATTR int16_t s_temp_last = BWS_TEMP_NONE;
static RTC_DATA_ATTR uint64_t s_temp_at;

/* One reading, driver closed, one retry after 20 ms; a plausible one becomes the cached one. `when`: before or after
 * a refresh (PicPak 2026-09-28: the reads right after a boot failed, those after a refresh worked). */
static int16_t read_temp(const char *when)
{
    if (bwry_is_open())
        bwry_close();
    uint8_t raw[2] = {0, 0};
    int16_t c = BWS_TEMP_NONE;
    int rc = BWRY_E_IO;
    for (int i = 0; i < 2; i++) {
        rc = bwry_read_temp(panel_pins(), &c, raw);
        if (!rc && bws_temp_ok(c))
            break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    bool ok = !rc && bws_temp_ok(c);
    if (ok) {
        s_temp_last = c;
        s_temp_at = hal_rtc_us();
    }
    hal_logf("panel temp=%d raw=%02x%02x rc=%d %s%s", c, raw[0], raw[1], rc, when, ok ? "" : " (none)");
    return ok ? c : BWS_TEMP_NONE;
}

static void measure_temp(void)
{
    s_temp = BWS_TEMP_NONE;
    if (!(BWRY_HAVE_LUT5 || BWRY_HAVE_LUT10))
        return; /* no read code in builds without LUTs */
    bws_in_t in = speed_in();
    if (!bws_wants_temp(&in))
        return;
    int16_t c = read_temp("before");
    uint64_t age = s_temp_at ? (hal_rtc_us() - s_temp_at) / 1000000u : UINT32_MAX;
    s_temp = bws_temp_use(c, s_temp_last, age > UINT32_MAX ? UINT32_MAX : (uint32_t)age);
    if (!bws_temp_ok(c) && bws_temp_ok(s_temp))
        hal_logf("panel temp=%d from the reading %us ago", s_temp, (unsigned)age);
}

static const uint8_t *pick_lut(const bws_pick_t *p)
{
    return p->lut == BWS_LUT_5S ? BWRY_LUT5 : p->lut == BWS_LUT_10S ? BWRY_LUT10 : NULL;
}

static bwry_cfg_t drv_cfg(void)
{
    if (!s_cfg_loaded)
        load_cfg();
    bws_pick_t p = speed_pick();
    return (bwry_cfg_t){(bwry_init_t)s_cfg.init, s_cfg.flip, 0, p.fast, pick_lut(&p), s_pll};
}

void hal_panel_speed(uint8_t speed)
{
    if (!s_cfg_loaded)
        load_cfg();
    s_speed = speed <= BWS_AUTO ? speed : BWS_NATIVE;
    bws_pick_t p = speed_pick();
    bwry_set_speed(p.fast, pick_lut(&p)); /* from the next refresh (also when the driver is open) */
}

bool hal_esp_panel_bottom_up(void)
{
    bwry_cfg_t c = drv_cfg();
    return bwry_rows_bottom_up(&c);
}

bool hal_esp_bwry_part_on(void)
{
    if (!s_cfg_loaded)
        load_cfg();
    return s_part.mode != 0;
}

bool hal_esp_bwry_partial(void) { return hal_esp_bwry_part_on() && bwp_allowed(&s_part, &s_ps); }

bool hal_panel_box_partial(void)
{
    if (!BOARD_BOX_PARTIAL)
        return false;
    if (!s_cfg_loaded)
        load_cfg();
    return bwp_boxes_allowed(s_part.mode ? s_part.every : PART_EVERY_DEFAULT, &s_ps);
}

/* A refresh done: the policy state, then hal_board() again (board.c reads hal_esp_bwry_partial): the app reads
 * the board once per boot and keeps the pointer, also through the waits in place on USB (RAM kept). */
static void part_note(bool full, bool ok)
{
    bwp_note(&s_ps, full, ok);
    (void)hal_board();
}

bool hal_panel_gray_partial(void) { return hal_esp_bwry_part_on(); }

/* On battery the chip light-sleeps through the BUSY waits (PON, the 5..19 s refresh, POF) instead of polling at
 * 160 MHz: woken by BUSY going high (GPIO wake, any pin in light sleep on the C3), a press or a cap (src/busy_sleep.h).
 * After each wake epd_wait_hook polls the button (a press latched as before) and feeds the watchdog. RST, CS, DC and
 * the IMU CS are held through the sleep. */
static uint64_t s_light0; /* hal_light_us at the start of the refresh (log light_ms) */

bool bwry_wait_sleep(int busy_pin, uint32_t max_ms)
{
    bool down = hal_button_down();
    bsl_in_t in = {hal_esp_wifi_on(), hal_esp_stay_awake(), down, !down && hal_esp_button_wake_ok(), max_ms};
    bsl_plan_t p = bsl_plan(&in);
    return p.ms && hal_esp_light_sleep_busy(busy_pin, (uint64_t)p.ms * 1000u, p.button_wake);
}

void hal_esp_light_sleep_pins(bool hold)
{
    if (!bwry_is_open())
        return;
    static const uint8_t pins[] = {BOARD_EPD_RST, BOARD_EPD_CS, BOARD_EPD_DC, BOARD_IMU_CS};
    for (unsigned i = 0; i < sizeof pins; i++) {
        if (hold)
            gpio_hold_en(pins[i]);
        else
            gpio_hold_dis(pins[i]);
    }
}

/* USB attached: answer quick link requests during the ~18 s BUSY wait (hal.h hal_busy_hook). */
void epd_wait_hook(void)
{
    static int64_t last;
    int64_t t = esp_timer_get_time();
    hal_esp_wdt_feed();
    /* a press during the wait: its start noted (hal_button_since; PicPak 2026-09-26), one released in it latched
     * (hal_button_take; PicPak 2026-09-27: presses during Minute by Minute's refresh were lost) */
    hal_esp_button_poll();
    if (hal_esp_stay_awake() && t - last >= 20000) {
        last = t;
        hal_busy_hook();
    }
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

/* The fast LUT of a partial (panel_bwfast.c) is for that partial only: the full LUT again for whatever comes next,
 * also after a dropped frame (the driver keeps the last LUT set while it stays open). */
static void speed_restore(void)
{
    if (!bwfast_active())
        return;
    bwfast_done();
    bws_pick_t p = speed_pick();
    bwry_set_speed(p.fast, pick_lut(&p));
}

static void release(void)
{
    speed_restore();
    if (bwry_is_open()) {
        bwry_stream_abort(); /* a frame not refreshed (render failed): power off */
        bwry_close();        /* controller deep sleep, SPI released */
    }
    s_begun = false;
}

int hal_panel_begin(uint8_t depth, hal_refresh_t mode)
{
    if (depth != 1 && depth != 2)
        return HAL_E_FAIL;
    if (mode == HAL_REFRESH_PARTIAL && !hal_esp_bwry_partial() && !hal_panel_box_partial())
        return HAL_E_FAIL; /* hal_board()->partial_refresh and hal_panel_box_partial were false */
    if (s_begun)
        release();
    if (!s_cfg_loaded)
        load_cfg();
    measure_temp(); /* closes the driver when it reads */
    if (!bwry_is_open()) {
        bwry_cfg_t c = drv_cfg();
        int rc = bwry_open(panel_pins(), &c);
        if (rc) {
            hal_logf("E panel open rc=%d", rc);
            release();
            return rc == BWRY_E_TIMEOUT ? HAL_E_TIMEOUT : HAL_E_IO;
        }
    }
    if (mode == HAL_REFRESH_PARTIAL && bwfast_on()) {
        /* experiment (issue #190): this partial's init sends the short black/white LUT instead of the full one */
        bws_pick_t p = speed_pick();
        const uint8_t *fl = bwfast_lut(s_part.mode ? s_part.every : PART_EVERY_DEFAULT, &s_ps, s_temp, pick_lut(&p));
        if (fl)
            bwry_set_speed(p.fast, fl);
    }
    int rc = mode == HAL_REFRESH_PARTIAL ? bwry_part_begin() : bwry_stream_begin();
    if (rc) {
        hal_logf("E panel begin rc=%d", rc);
        release();
        if (mode == HAL_REFRESH_PARTIAL)
            part_note(false, false);
        return rc == BWRY_E_TIMEOUT ? HAL_E_TIMEOUT : HAL_E_IO;
    }
    s_depth = depth;
    s_palette = depth == 2 && s_palette_next;
    s_palette_next = false; /* set again before the next begin */
    s_mode = mode;
    bwp_acc_reset(&s_acc);
    s_acc_bad = false;
    hal_stats.refresh_kind = mode == HAL_REFRESH_FULL ? 1 : 2;
    hal_stats.rects = 0;
    s_begun = true;
    return HAL_OK;
}

typedef struct {
    const uint8_t *src; /* the band's first row */
    size_t stride;
    int y0;             /* the band's first panel row */
} band_t;

/* One band row -> panel codes, 4 pixels per byte (bwry_row_fn, epd_bwrymap.h). */
static void band_row(void *ctx, int y, uint8_t *row)
{
    const band_t *b = ctx;
    bwry_map_row(b->src + (size_t)(y - b->y0) * b->stride, s_depth, s_palette, 0, BWRY_W, y, row);
}

/* One window row -> panel codes: pixels [x, x + w) of the band row, src at pixel x (bwry_wrow_fn). */
static void win_row(void *ctx, int y, uint8_t *row, int x, int w)
{
    const band_t *b = ctx;
    bwry_map_row(b->src + (size_t)(y - b->y0) * b->stride, s_depth, s_palette, x, w, y, row);
    bwfast_row(row, w / 4); /* the colour gate of the fast LUT (no-op unless one is active) */
}

static int part_write(const tiny_rect_t *r, const uint8_t *next, size_t stride)
{
    if (s_acc_bad)
        return HAL_E_FAIL;
    if (bwp_acc_add(&s_acc, r->x, r->y, r->w, r->h) != BWP_OK) {
        hal_logf("E panel partial window x=%d y=%d w=%u h=%u does not fit x=%d w=%d rows %d..%d: full next", r->x,
                 r->y, r->w, r->h, s_acc.x, s_acc.w, s_acc.y0, s_acc.y1 - 1);
        s_acc_bad = true;
        return HAL_E_FAIL;
    }
    stats_rect(r);
    band_t b = {next, stride, r->y};
    int rc = bwry_part_window(r->x, r->y, r->w, r->h, win_row, &b);
    if (rc) {
        hal_logf("E panel partial write x=%d y=%d w=%u h=%u rc=%d", r->x, r->y, r->w, r->h, rc);
        s_acc_bad = true;
        return rc == BWRY_E_TIMEOUT ? HAL_E_TIMEOUT : HAL_E_FAIL;
    }
    if (bwfast_colour()) {
        /* the short LUT holds yellow and red only as long as no such pixel is in the window: this partial is dropped
         * (no refresh) and the next refresh is a full one */
        hal_logf("E panel bwfast refused: colour in the window x=%d y=%d w=%u h=%u: full next", r->x, r->y, r->w, r->h);
        s_acc_bad = true;
        part_note(false, false);
        return HAL_E_FAIL;
    }
    return HAL_OK;
}

int hal_panel_write(const tiny_rect_t *r, const uint8_t *prev, const uint8_t *next, size_t stride)
{
    (void)prev; /* the window is driven from the new data only */
    if (!s_begun || !next)
        return HAL_E_FAIL;
    if (s_mode == HAL_REFRESH_PARTIAL)
        return part_write(r, next, stride);
    /* the next rows of the stream in panel order: full width, the band right above (bottom-up) or
     * below (top-down) the rows already sent */
    int sent = bwry_stream_sent();
    bool up = hal_esp_panel_bottom_up();
    if (r->x != 0 || r->w != BWRY_W || r->y < 0 || r->h == 0 || r->y + r->h > BWRY_H ||
        (up ? r->y + r->h != BWRY_H - sent : r->y != sent)) {
        hal_logf("E panel write y=%d h=%u x=%d w=%u out of stream order (sent %d, %s)", r->y, r->h, r->x, r->w, sent,
                 up ? "bottom-up" : "top-down");
        return HAL_E_FAIL;
    }
    stats_rect(r);
    band_t b = {next, stride, r->y};
    int rc = bwry_stream_rows(band_row, &b, r->h);
    if (rc) {
        hal_logf("E panel write rc=%d", rc);
        return rc == BWRY_E_TIMEOUT ? HAL_E_TIMEOUT : HAL_E_IO;
    }
    return HAL_OK;
}

static int part_refresh(void)
{
    int64_t t = esp_timer_get_time();
    const bwp_acc_t *a = &s_acc;
    if (!s_acc_bad && !bwp_acc_solid(a))
        hal_logf("E panel partial: rows %d..%d not all written (%d)", a->y0, a->y1 - 1, a->rows);
    int rc = s_acc_bad || !bwp_acc_solid(a) ? BWRY_E_ARG : bwry_part_end(a->x, a->y0, a->w, a->y1 - a->y0);
    if (rc == BWRY_E_ARG)
        bwry_stream_abort(); /* no refresh: power off */
    hal_stats.refresh_ms = (uint32_t)((esp_timer_get_time() - t) / 1000);
    part_note(false, rc == BWRY_OK);
    const bwry_stats_t *st = bwry_stats();
    hal_logf("panel partial rows=%d..%d x=%d w=%d ms=%u busy_ms=%u fast=%s n=%u/%u init=%s lut=%s rc=%d", a->y0,
             a->y1 - 1, a->x, a->w, (unsigned)hal_stats.refresh_ms, (unsigned)st->refresh_ms,
             bwry_fast_name(s_cfg.fast), (unsigned)s_ps.since_full, (unsigned)s_part.every,
             s_cfg.init == BWRY_INIT_GD ? "gd" : "stock", bwfast_name(), rc);
    speed_restore();
    return rc == BWRY_OK ? HAL_OK : rc == BWRY_E_TIMEOUT ? HAL_E_TIMEOUT : HAL_E_IO;
}

int hal_panel_refresh(void)
{
    hal_esp_heap_mark(HAL_ESP_PH_RENDER);
    if (!s_begun)
        return HAL_E_FAIL;
    if (s_mode == HAL_REFRESH_PARTIAL)
        return part_refresh();
    int64_t t = esp_timer_get_time();
    s_light0 = hal_light_us();
    int rc = bwry_stream_end();
    hal_stats.refresh_ms = (uint32_t)((esp_timer_get_time() - t) / 1000);
    const bwry_stats_t *st = bwry_stats();
    if (hal_esp_bwry_part_on() || BOARD_BOX_PARTIAL)
        part_note(true, rc == BWRY_OK);
    if (rc)
        hal_logf("E panel refresh rc=%d", rc);
    else {
        bws_pick_t sp = speed_pick();
        unsigned light = (unsigned)((hal_light_us() - s_light0) / 1000);
        hal_logf("panel full ms=%u busy_ms=%u light_ms=%u awake_ms=%u fast=%s speed=%s/%s lut=%s why=%s temp=%d part=%s "
                 "pll=%02x",
                 (unsigned)hal_stats.refresh_ms, (unsigned)st->refresh_ms, light,
                 (unsigned)hal_stats.refresh_ms > light ? (unsigned)hal_stats.refresh_ms - light : 0u,
                 bwry_fast_name(s_cfg.fast), bws_name(s_speed), bws_pick_name(&sp), bws_lut_name(&sp),
                 bws_why_name(sp.why), s_temp, s_part.mode ? "on" : "off", s_pll);
        /* the sensor reads well right after a refresh: the next refresh's fallback, and the reading in native too */
        if (BWRY_HAVE_LUT5 || BWRY_HAVE_LUT10)
            (void)read_temp("after");
    }
    return rc == BWRY_OK ? HAL_OK : rc == BWRY_E_TIMEOUT ? HAL_E_TIMEOUT : HAL_E_IO;
}

static void testcard(void);
static bool s_card_pending; /* "!tp panel testcard" / "!tp pll" during a frame: shown at its hal_panel_sleep */

void hal_panel_sleep(void)
{
    release();
    if (s_card_pending) {
        s_card_pending = false;
        testcard();
    }
}

/* ------------------------------------------------------------------ text commands */

static void show_cfg(void)
{
    hal_logf("panel bwry init=%s flip=%s fast=%s part=%s every=%u since_full=%u need_full=%d clock_fast=%u "
             "lut=%s luts=%d%d rev=%02x%02x%02x/%d pll=%02x",
             s_cfg.init == BWRY_INIT_GD ? "gd" : "stock", s_cfg.flip < 0 ? "auto" : s_cfg.flip ? "1" : "0",
             bwry_fast_name(s_cfg.fast), s_part.mode ? "on" : "off", (unsigned)s_part.every, (unsigned)s_ps.since_full,
             s_ps.need_full, hal_esp_fast(), s_lut_mode == BWS_REV_FORCE ? "force" : s_lut_mode == BWS_REV_OFF ? "off" : "auto",
             BWRY_HAVE_LUT5, BWRY_HAVE_LUT10, s_rev[0], s_rev[1], s_rev[2], s_rev_read, s_pll);
    bwfast_show();
}

/* "!tp panel testcard" (docs/hardware/picpak-refresh.md): the test card (epd_bwcard.h) as a full refresh at the
 * current refresh speed, labelled with the mode it used. The app's image comes back with its next refresh. */
static void card_row(void *ctx, int y, uint8_t *row) { bwc_row(ctx, y, row); }

static void testcard(void)
{
    static bool in_card;
    /* A link command runs from the BUSY wait's hook (epd_wait_hook) and between the app's bands: the app's frame (or a
     * card) is on the panel now. PicPak 2049 (2026-09-28): the card closed the driver under the app's refresh, its POF
     * timed out, the refresh timed out 45 s later and powered off through a removed SPI device: a panic, 4 times, then
     * safe mode. Now the card follows that frame. */
    if (s_begun || in_card || bwry_busy()) {
        s_card_pending = !in_card;
        hal_logf("W panel busy: the test card %s", in_card ? "is running" : "follows this refresh");
        return;
    }
    in_card = true;
    hal_esp_stage_t was = hal_esp_stage_now();
    hal_esp_stage(HAL_ESP_ST_PANEL);
    measure_temp();
    bws_pick_t sp = speed_pick();
    if (!bwry_is_open()) {
        bwry_cfg_t c = drv_cfg();
        int rc = bwry_open(panel_pins(), &c);
        if (rc) {
            hal_logf("E panel open rc=%d", rc);
            release();
            hal_esp_stage(was);
            in_card = false;
            return;
        }
    } else {
        bwry_set_speed(sp.fast, pick_lut(&sp));
        bwry_set_pll(s_pll);
    }
    char label[64];
    snprintf(label, sizeof label, "SPEED %s LUT %s WHY %s TEMP %d PLL %02x", bws_name(s_speed), bws_lut_name(&sp),
             bws_why_name(sp.why), s_temp, s_pll);
    for (char *q = label; *q; q++)
        if (*q >= 'a' && *q <= 'z')
            *q = (char)(*q - 'a' + 'A');
    int64_t t0 = esp_timer_get_time();
    int rc = bwry_show_rows(card_row, label);
    hal_logf("panel testcard rc=%d ms=%u busy_ms=%u speed=%s/%s lut=%s why=%s temp=%d pll=%02x", rc,
             (unsigned)((esp_timer_get_time() - t0) / 1000), (unsigned)bwry_stats()->refresh_ms, bws_name(s_speed),
             bws_pick_name(&sp), bws_lut_name(&sp), bws_why_name(sp.why), s_temp, s_pll);
    release();
    s_ps.need_full = true; /* a partial must not build on the card */
    hal_esp_stage(was);
    in_card = false;
}

_Noreturn static void save_restart(const char *what)
{
    show_cfg();
    hal_logf("I %s saved, restarting for a full refresh", what);
    hal_reboot(); /* cold wake: the app redraws fully with the new setting */
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
    char *argv[8];
    int argc = 0;
    for (char *t = strtok(line, " "); t && argc < 8; t = strtok(NULL, " "))
        argv[argc++] = t;
    if (argc >= 1 && !strcmp(argv[0], "bwry")) {
        if (argc == 1) {
            show_cfg();
            return true;
        }
        bwry_nvs_t c = s_cfg;
        for (int i = 1; i < argc; i++) {
            const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
            if (!strcmp(a, "stock")) c.init = BWRY_INIT_STOCK;
            else if (!strcmp(a, "gd")) c.init = BWRY_INIT_GD;
            else if (!strcmp(a, "flip") && (!strcmp(v, "auto") || !strcmp(v, "0") || !strcmp(v, "1"))) {
                c.flip = (int8_t)(!strcmp(v, "auto") ? -1 : v[0] - '0');
                i++;
            } else if (!strcmp(a, "fast") && (!strcmp(v, "off") || !strcmp(v, "tse") || !strcmp(v, "e6") ||
                                               !strcmp(v, "e6p"))) {
                c.fast = !strcmp(v, "off") ? BWRY_FAST_OFF : !strcmp(v, "tse") ? BWRY_FAST_TSE
                       : !strcmp(v, "e6") ? BWRY_FAST_E6 : BWRY_FAST_E6P;
                i++;
            } else {
                hal_logf("E usage: bwry [stock|gd] [flip auto|0|1] [fast off|tse|e6|e6p]");
                return true;
            }
        }
        hal_kv_set("bwry", &c, sizeof c);
        s_cfg = c;
        save_restart("bwry");
    }
    if (argc == 2 && !strcmp(argv[0], "panel") && !strcmp(argv[1], "testcard")) {
        testcard();
        return true;
    }
    if (argc >= 1 && !strcmp(argv[0], "lut")) {
        uint8_t m = argc == 2 && !strcmp(argv[1], "auto") ? BWS_REV_AUTO : argc == 2 && !strcmp(argv[1], "force")
                  ? BWS_REV_FORCE : argc == 2 && !strcmp(argv[1], "off") ? BWS_REV_OFF : 0xff;
        if (m == 0xff) {
            if (argc == 1) show_cfg();
            else hal_logf("E usage: lut <auto|force|off>");
            return true;
        }
        hal_kv_set("bwlut", &m, 1);
        s_lut_mode = m;
        s_rev_read = false; /* auto: read again at the restart */
        save_restart("bwlut");
    }
    if (argc >= 1 && !strcmp(argv[0], "part")) {
        if (argc == 1) {
            show_cfg();
            return true;
        }
        bwp_cfg_t p = s_part;
        for (int i = 1; i < argc; i++) {
            const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
            char *end = NULL;
            long k = !strcmp(a, "every") ? strtol(v, &end, 10) : -1;
            if (!strcmp(a, "on") || !strcmp(a, "off")) {
                p.mode = !strcmp(a, "on");
            } else if (end && end != v && !*end && k >= 0 && k <= 255) {
                p.every = (uint8_t)k;
                i++;
            } else {
                hal_logf("E usage: part [on|off] [every 0..255]");
                return true;
            }
        }
        hal_kv_set("bwpart", &p, sizeof p);
        s_part = p;
        s_ps.need_full = true; /* the restart redraws fully anyway */
        if (p.mode)
            hal_logf("W EXPERIMENTAL windowed partial: set depth 1 (or 2 with gray_mode hybrid)");
        save_restart("part");
    }
    if (argc >= 1 && !strcmp(argv[0], "pll")) {
        if (argc == 1) {
            show_cfg();
            return true;
        }
        char *end = NULL;
        long k = argc == 2 && strcmp(argv[1], "off") ? strtol(argv[1], &end, 16) : 0;
        if (argc != 2 || (end && (end == argv[1] || *end || k < 0x01 || k > 0x3f))) {
            hal_logf("E usage: pll <01..3f|off> (hex; stock 08, LUT 5s 06, 10s 03)");
            return true;
        }
        uint8_t b = (uint8_t)k;
        hal_kv_set("bwpll", &b, 1);
        s_pll = b;
        if (b)
            hal_logf("W EXPERIMENTAL pll=%02x: compare the test card with `pll off` at the same temperature", b);
        testcard(); /* the app's next refreshes keep the setting */
        return true;
    }
    if (argc >= 1 && !strcmp(argv[0], "bwfast")) {
        if (!bwfast_cmd(argc, argv))
            return true;
        s_ps.need_full = true; /* the restart redraws fully anyway */
        save_restart("bwfast");
    }
    if (argc >= 1 && !strcmp(argv[0], "fast")) {
        if (argc == 1) {
            show_cfg();
            return true;
        }
        char *end = NULL;
        long k = argc == 2 ? strtol(argv[1], &end, 10) : -1;
        if (!end || end == argv[1] || *end || k < 1 || k > HAL_ESP_FAST_MAX) {
            hal_logf("E usage: fast <1..%d> (1 = off)", HAL_ESP_FAST_MAX);
            return true;
        }
        uint8_t b = (uint8_t)k;
        hal_kv_set("fast", &b, 1);
        hal_esp_fast_set((unsigned)k);
        if (k > 1)
            hal_logf("W FAST MODE x%ld: the clock runs %ld times faster; set network Off while testing", k, k);
        save_restart("fast");
    }
    if (argc >= 1 && !strcmp(argv[0], "reboot")) {
        hal_link_flush();
        esp_restart();
    }
    hal_logf("E unknown command (bwry, lut, pll, panel testcard, part, bwfast, fast, reboot)");
    return true;
}

#endif /* BOARD_PANEL_BWRY */
