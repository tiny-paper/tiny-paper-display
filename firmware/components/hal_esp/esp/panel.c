/* hal_esp: panel through the epd component (UC8179: 1-bit, and 2-bit as a 4-gray full refresh), and the
 * "!tp" text commands.
 * The panel variant and waveforms are runtime settings (NVS key "panel"), switchable over
 * USB without reflashing:
 *   !tp panel <t7|gen2> <otp|lut|gx> <win|nowin> [full|fast]   save and redraw fully
 *   !tp panel                                                show the current setting
 *   !tp lut reps <1..8>          register LUTs (lut, gx): frame count multiplier (NVS "lut")
 *   !tp lut vcom <auto|0x08..0x40>  register LUTs: VCOM DC register 0x82 (-0.1 V - 0.05 V/step)
 *   !tp gray <auto|otp|lut>      4-gray full refresh waveform (NVS "gray"): auto = lut
 *   !tp gray temp <auto|0..127>  otp: forced temperature (TSSET) that selects the OTP gray waveform (auto 0x5f)
 *   !tp fast <1..6>              test mode: the clock runs k times faster (NVS "fast", 1 = off)
 *   !tp reboot
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "hal_esp_priv.h"

#if !BOARD_PANEL_BWRY && !BOARD_PANEL_FASTEPD && !BOARD_X4P /* whole file: UC8179 boards (TRMNL OG); BWRY: panel_bwry.c, FastEPD:
                                                             * panel_par.c, X4 Pro: panel_x4p.c */

#if CONFIG_TINY_PANEL_STUB
#define PANEL_STUB 1
#else
#define PANEL_STUB 0
#include "board.h"
#endif
#include "epd.h"
#include "epd_gray.h"
#include "epd_lut.h"
#include "epd_rev.h"
#include "esp_attr.h"

typedef struct {
    uint8_t variant;   /* epd_variant_t */
    uint8_t part_wave; /* epd_part_wave_t */
    uint8_t window;    /* windowed partial refresh */
    uint8_t full_fast; /* full refresh with EPD_FULL_FAST */
} panel_cfg_t;

/* NVS "lut" (separate key: old "panel" blobs stay valid) */
typedef struct {
    uint8_t reps; /* 1..EPD_LUT_REPS_MAX */
    uint8_t vcom; /* EPD_VCOM_AUTO or a VDCS value */
} lut_cfg_t;

/* Defaults tuned by eye on a real TRMNL OG (2026-09-24): GxEPD2 partial LUTs on the GEN2 profile,
 * windowed, reps 1, VCOM DC 0x20. Solid black segments, no flash, least white-level mismatch.
 * "!tp panel ..." / "!tp lut ..." override them (saved in NVS). */
static const panel_cfg_t PANEL_DEFAULT = {EPD_GEN2, EPD_PART_GX, 1, 0};
static const lut_cfg_t LUT_DEFAULT = {1, 0x20};

/* NVS "gray": 4-gray full refresh */
typedef struct {
    uint8_t wave; /* epd_gray_wave_t */
    uint8_t temp; /* 0 = EPD_GRAY_TEMP_DEFAULT */
} gray_cfg_t;
static gray_cfg_t s_gray;

static panel_cfg_t s_cfg;
static lut_cfg_t s_lut = LUT_DEFAULT;
#if !CONFIG_TINY_PANEL_STUB
static int64_t s_t0;
#endif
static bool s_begun;

static void load_cfg(void)
{
    s_cfg = PANEL_DEFAULT;
    s_lut = LUT_DEFAULT;
    panel_cfg_t c;
    if (hal_kv_get("panel", &c, sizeof c) == (int)sizeof c)
        s_cfg = c;
    if (s_cfg.part_wave > EPD_PART_GX)
        s_cfg.part_wave = 0;
    lut_cfg_t l;
    if (hal_kv_get("lut", &l, sizeof l) == (int)sizeof l)
        s_lut = l;
    s_lut.reps = (uint8_t)epd_lut_reps_clamp(s_lut.reps);
    gray_cfg_t g;
    s_gray = (gray_cfg_t){EPD_GRAY_AUTO, 0};
    if (hal_kv_get("gray", &g, sizeof g) == (int)sizeof g && g.wave <= EPD_GRAY_LUT && g.temp <= 127)
        s_gray = g;
    if (s_lut.vcom != EPD_VCOM_AUTO)
        s_lut.vcom = (uint8_t)epd_lut_vcom(EPD_LUTSET_BB, s_lut.vcom);
}

/* The panel ID (UC8179 REV, as stock v1.8.17's Panel-Rev) is read once per power-up and kept across deep sleep. */
static RTC_DATA_ATTR uint32_t s_rev;
#if !CONFIG_TINY_PANEL_STUB
static RTC_DATA_ATTR bool s_rev_read;
#endif

uint32_t hal_panel_rev(void) { return s_rev; }

void hal_panel_init(void)
{
    load_cfg();
#if !CONFIG_TINY_PANEL_STUB
    if (s_rev_read || epd_is_open())
        return;
    s_rev_read = true;
    epd_pins_t p = {BOARD_EPD_SCK, BOARD_EPD_MOSI, BOARD_EPD_CS, BOARD_EPD_DC, BOARD_EPD_RST, BOARD_EPD_BUSY,
                    BOARD_EPD_SPI_HZ};
    uint8_t b[7] = {0};
    if (epd_read_reg(&p, 0x70, b, sizeof b) == EPD_OK)
        s_rev = epd_rev_parse(b);
    char str[9];
    epd_rev_str(s_rev, str);
    hal_logf("I panel rev=%s", s_rev ? str : "unknown");
#endif
}

bool hal_esp_panel_bottom_up(void) { return false; } /* UC8179: rows top first */

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

#if CONFIG_TINY_PANEL_STUB

/* Awake on USB: quick link requests are answered while the panel refreshes. */
void epd_wait_hook(void)
{
    hal_esp_wdt_feed();
    if (hal_esp_stay_awake())
        hal_busy_hook();
}

int hal_panel_begin(uint8_t depth, hal_refresh_t mode)
{
    if (depth != 1 && !(depth == 2 && mode == HAL_REFRESH_FULL))
        return HAL_E_FAIL;
    hal_stats.refresh_kind = mode == HAL_REFRESH_FULL ? 1 : 2;
    hal_stats.rects = 0;
    s_begun = true;
    return HAL_OK;
}

int hal_panel_write(const tiny_rect_t *r, const uint8_t *prev, const uint8_t *next, size_t stride)
{
    (void)prev;
    (void)next;
    (void)stride;
    if (!s_begun)
        return HAL_E_FAIL;
    stats_rect(r);
    return HAL_OK;
}

int hal_panel_refresh(void)
{
    hal_esp_heap_mark(HAL_ESP_PH_RENDER);
    hal_logf("panel stub refresh %s rects=%u box=%d,%d,%d,%d", hal_stats.refresh_kind == 1 ? "full" : "partial",
             hal_stats.rects, hal_stats.bx0, hal_stats.by0, hal_stats.bx1, hal_stats.by1);
    return HAL_OK;
}

void hal_panel_sleep(void) { s_begun = false; }

#else

/* Awake on USB: quick link requests are answered while the panel refreshes (hal.h hal_busy_hook). */
void epd_wait_hook(void)
{
    hal_esp_wdt_feed();
    if (hal_esp_stay_awake())
        hal_busy_hook();
}

/* On battery the chip light-sleeps through BUSY (PON, the ~1.7 s partial, POF) and wakes on its rising edge,
 * instead of polling at 160 MHz (plan 3.1 "light-sleep on BUSY pin"). */
bool epd_wait_sleep(int busy_pin, uint32_t max_ms)
{
    return hal_esp_light_sleep_pin(busy_pin, 1, (uint64_t)max_ms * 1000u);
}

/* Reset and chip select keep their levels through a light sleep while the controller is up. */
void hal_esp_light_sleep_pins(bool hold)
{
    if (!epd_is_open())
        return;
    if (hold) {
        gpio_hold_en(BOARD_EPD_RST);
        gpio_hold_en(BOARD_EPD_CS);
    } else {
        gpio_hold_dis(BOARD_EPD_RST);
        gpio_hold_dis(BOARD_EPD_CS);
    }
}

static int open_panel(void)
{
    if (epd_is_open())
        return HAL_OK;
    epd_pins_t p = {BOARD_EPD_SCK, BOARD_EPD_MOSI, BOARD_EPD_CS, BOARD_EPD_DC, BOARD_EPD_RST, BOARD_EPD_BUSY,
                    BOARD_EPD_SPI_HZ};
    epd_cfg_t c = {(epd_variant_t)s_cfg.variant, (epd_part_wave_t)s_cfg.part_wave, s_cfg.window != 0, s_lut.reps,
                   s_lut.vcom, (epd_gray_wave_t)s_gray.wave, s_gray.temp};
    int rc = epd_open(&p, &c);
    if (rc)
        hal_logf("E panel open rc=%d", rc);
    return rc ? HAL_E_IO : HAL_OK;
}

int hal_panel_begin(uint8_t depth, hal_refresh_t mode)
{
    /* 2-bit: 4-gray full refresh only (the app's hybrid mode sends its partials as 1-bit) */
    if (depth != 1 && !(depth == 2 && mode == HAL_REFRESH_FULL))
        return HAL_E_FAIL;
    s_t0 = esp_timer_get_time();
    if (open_panel())
        return HAL_E_IO;
    epd_mode_t m = depth == 2 ? EPD_GRAY4 : mode == HAL_REFRESH_PARTIAL ? EPD_PARTIAL
                 : s_cfg.full_fast ? EPD_FULL_FAST : EPD_FULL;
    int rc = epd_begin(m);
    if (rc) {
        hal_logf("E panel begin rc=%d", rc);
        epd_close();
        return rc == EPD_E_TIMEOUT ? HAL_E_TIMEOUT : HAL_E_IO;
    }
    hal_stats.refresh_kind = mode == HAL_REFRESH_FULL ? 1 : 2;
    hal_stats.rects = 0;
    s_begun = true;
    return HAL_OK;
}

int hal_panel_write(const tiny_rect_t *r, const uint8_t *prev, const uint8_t *next, size_t stride)
{
    if (!s_begun)
        return HAL_E_FAIL;
    stats_rect(r);
    int rc = epd_write(r, prev, next, stride);
    return rc ? HAL_E_IO : HAL_OK;
}

int hal_panel_refresh(void)
{
    hal_esp_heap_mark(HAL_ESP_PH_RENDER);
    if (!s_begun)
        return HAL_E_FAIL;
    int64_t t = esp_timer_get_time();
    int rc = epd_refresh();
    hal_stats.refresh_ms = (uint32_t)((esp_timer_get_time() - t) / 1000);
    if (rc)
        hal_logf("E panel refresh rc=%d", rc);
    return rc == EPD_OK ? HAL_OK : rc == EPD_E_TIMEOUT ? HAL_E_TIMEOUT : HAL_E_IO;
}

void hal_panel_sleep(void)
{
    if (epd_is_open())
        epd_close(); /* controller deep sleep, pumps off, SPI released */
    s_begun = false;
    (void)s_t0;
}

#endif

/* ------------------------------------------------------------------ text commands */

static void show_cfg(void)
{
    static const char *const var[] = {"t7", "gen2"};
    static const char *const wave[] = {"otp", "lut", "gx"};
    char vc[8];
    if (s_lut.vcom == EPD_VCOM_AUTO)
        snprintf(vc, sizeof vc, "auto");
    else
        snprintf(vc, sizeof vc, "0x%02x", s_lut.vcom);
    static const char *const gw[] = {"auto", "otp", "lut"};
    char gt[8];
    if (s_gray.temp)
        snprintf(gt, sizeof gt, "0x%02x", s_gray.temp);
    else
        snprintf(gt, sizeof gt, "auto");
    epd_gray_wave_t g = epd_gray_wave((epd_variant_t)s_cfg.variant, (epd_gray_wave_t)s_gray.wave);
    hal_logf("panel variant=%s part=%s window=%d full=%s reps=%u vcom=%s gray=%s(%s) gray_temp=%s fast=%u%s",
             var[s_cfg.variant & 1], wave[s_cfg.part_wave <= EPD_PART_GX ? s_cfg.part_wave : 0], s_cfg.window,
             s_cfg.full_fast ? "fast" : "full", s_lut.reps, vc, gw[s_gray.wave <= 2 ? s_gray.wave : 0],
             g == EPD_GRAY_OTP ? "otp" : "lut", gt, hal_esp_fast(), PANEL_STUB ? " (stub)" : "");
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
    char *argv[6];
    int argc = 0;
    for (char *t = strtok(line, " "); t && argc < 6; t = strtok(NULL, " "))
        argv[argc++] = t;
    if (argc >= 1 && !strcmp(argv[0], "panel")) {
        if (argc == 1) {
            show_cfg();
            return true;
        }
        panel_cfg_t c = s_cfg;
        for (int i = 1; i < argc; i++) {
            const char *a = argv[i];
            if (!strcmp(a, "t7")) c.variant = 0;
            else if (!strcmp(a, "gen2")) c.variant = 1;
            else if (!strcmp(a, "otp")) c.part_wave = 0;
            else if (!strcmp(a, "lut")) c.part_wave = 1;
            else if (!strcmp(a, "gx")) c.part_wave = 2;
            else if (!strcmp(a, "win")) c.window = 1;
            else if (!strcmp(a, "nowin")) c.window = 0;
            else if (!strcmp(a, "full")) c.full_fast = 0;
            else if (!strcmp(a, "fast")) c.full_fast = 1;
            else {
                hal_logf("E panel: unknown option %s", a);
                return true;
            }
        }
        hal_kv_set("panel", &c, sizeof c);
        s_cfg = c;
        save_restart("panel");
    }
    if (argc >= 1 && !strcmp(argv[0], "lut")) {
        if (argc == 1) {
            show_cfg();
            return true;
        }
        lut_cfg_t l = s_lut;
        char *end = NULL;
        long v = argc == 3 ? strtol(argv[2], &end, 0) : -1;
        bool num = end && end != argv[2] && !*end;
        if (argc == 3 && !strcmp(argv[1], "reps") && num && v >= EPD_LUT_REPS_MIN && v <= EPD_LUT_REPS_MAX)
            l.reps = (uint8_t)v;
        else if (argc == 3 && !strcmp(argv[1], "vcom") && !strcmp(argv[2], "auto"))
            l.vcom = EPD_VCOM_AUTO;
        else if (argc == 3 && !strcmp(argv[1], "vcom") && num && v >= EPD_VCOM_MIN && v <= EPD_VCOM_MAX)
            l.vcom = (uint8_t)v;
        else {
            hal_logf("E usage: lut reps <%d..%d> | lut vcom <auto|0x%02x..0x%02x>", EPD_LUT_REPS_MIN, EPD_LUT_REPS_MAX,
                     EPD_VCOM_MIN, EPD_VCOM_MAX);
            return true;
        }
        hal_kv_set("lut", &l, sizeof l);
        s_lut = l;
        save_restart("lut");
    }
    if (argc >= 1 && !strcmp(argv[0], "gray")) {
        if (argc == 1) {
            show_cfg();
            return true;
        }
        gray_cfg_t g = s_gray;
        char *end = NULL;
        long v = argc == 3 ? strtol(argv[2], &end, 0) : -1;
        bool num = end && end != argv[2] && !*end;
        if (argc == 2 && !strcmp(argv[1], "auto"))
            g.wave = EPD_GRAY_AUTO;
        else if (argc == 2 && !strcmp(argv[1], "otp"))
            g.wave = EPD_GRAY_OTP;
        else if (argc == 2 && !strcmp(argv[1], "lut"))
            g.wave = EPD_GRAY_LUT;
        else if (argc == 3 && !strcmp(argv[1], "temp") && !strcmp(argv[2], "auto"))
            g.temp = 0;
        else if (argc == 3 && !strcmp(argv[1], "temp") && num && v >= 1 && v <= 127)
            g.temp = (uint8_t)v;
        else {
            hal_logf("E usage: gray <auto|otp|lut> | gray temp <auto|1..127>");
            return true;
        }
        hal_kv_set("gray", &g, sizeof g);
        s_gray = g;
        save_restart("gray");
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
    hal_logf("E unknown command (panel, lut, gray, fast, reboot)");
    return true;
}

#endif /* UC8179 */
