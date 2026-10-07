/* hal_esp: board description from boards/<TINY_BOARD>/board.h. Limits: docs/spec/manifest.md "Limits per
 * board" (trmnl_og and picpak share them; both are ESP32-C3 without PSRAM).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "hal_esp_priv.h"
#include "tiny_caps.h"
#include <stdio.h>
#if BOARD_BUTTON_TOUCHBAR
#include "btn_queue.h"
#include "freertos/FreeRTOS.h"
#endif

/* Task phase on the OG (ESP32-C3, no PSRAM), LCD Watch 3459 (19.9 KB wasm) fetching Open-Meteo:
 * boot heap 193 KB; tph_open 23.4 KB; WiFi + lwIP ~45-53 KB (estimate, not measured yet); task
 * instance 86.1 KB (64 KiB linear memory + 8 KiB stack + 14 KB WAMR); HTTPS GET peak 30.9 KB
 * (mbedTLS handshake + esp_http_client + DNS, QEMU); request buffers 1.7 KB. The body (6.5 KB)
 * arrives after the handshake and stays under its peak. That leaves ~0-7 KB, not 20. The install
 * check counts 24 KiB + 2 x code for the module (now ~1.2 x code: the code runs from flash) and a
 * full 16 KiB body; in those terms 184 KiB accepts one network task for plugins up to ~25 KB of
 * code, about where the estimate runs out. Provisional: the "task_*" heap probes (app_session.c)
 * and the "http" log line (net.c) give the real numbers on the device. */
#ifndef BOARD_BIG
#define BOARD_BIG 0
#endif
#ifndef BOARD_BATT_CELL
#define BOARD_BATT_CELL HAL_BATT_LIPO
#endif
/* album mode (docs/spec/album.md "Budget"): photos kept in `storage`, only in a build with the album (hal_esp Kconfig
 * TINY_ALBUM; off on the TRMNL OG) */
#if defined(BOARD_PHOTO_STORE) && BOARD_PHOTO_STORE && defined(CONFIG_TINY_ALBUM)
#define PHOTO_STORE true
#else
#define PHOTO_STORE false
#endif

#if BOARD_BIG
/* ESP32-S3 with 8 MB PSRAM (TRMNL X, Xteink X4 Pro): the manifest.md "Limits per board" PSRAM row and host-abi.md
 * "HTTP (TASK)" (2 x 32 KiB). Two 4-bit frames (2 x 1.3 MB), FastEPD's buffers (~2 MB) and a plugin with
 * 1 MiB of linear memory fit in PSRAM next to each other, so the budgets are the spec's, not the heap's. */
static hal_board_t board = {
    .id = BOARD_ID,
    .panel_w = BOARD_EPD_W,
    .panel_h = BOARD_EPD_H,
    .depths = BOARD_DEPTHS,
    .partial_refresh = TINY_CAP_PARTIAL_REFRESH != 0,
    .battery_full_refresh = TINY_CAP_BATTERY && !TINY_CAP_PARTIAL_REFRESH,
    .memory_kb_max = 1024,
    .task_memory_kb_max = 256,
    .store_kb_max = 256,
    .package_max = 4u * 1024 * 1024,
    .heap_render = 4u * 1024 * 1024,
    .heap_task = 1024u * 1024,
    .layer_budget = 4u * 1024 * 1024,
    .http_handles = 2,
    .http_body_max = 32768,
    .code_partition = BOARD_CODE_PARTITION,
    /* TRMNL X: holds as the IQS323 reports them (src/touch_btn.h: continuous reads, the chip's hold gesture), no
     * factory reset from the touch bar. X4 Pro: real keys, holds count */
    .button_short_only = false,
    .button_no_reset = BOARD_BUTTON_TOUCHBAR != 0,
    .frontlight = BOARD_X4P != 0, /* hal_frontlight (x4p_board.c) */
    .touch_screen = BOARD_X4P != 0, /* the GT911 (x4p_board.c), on only in touch windows (firmware.md "Touch") */
    .batt_cell = BOARD_BATT_CELL,
};
#else
static hal_board_t board = {
    .id = BOARD_ID,
    .panel_w = BOARD_EPD_W,
    .panel_h = BOARD_EPD_H,
    /* OG: 1-bit, and 2-bit as a 4-gray full refresh (plan 4.1, 4.4; config `gray_mode`) */
    .depths = BOARD_DEPTHS,
    .partial_refresh = TINY_CAP_PARTIAL_REFRESH != 0,
    .battery_full_refresh = TINY_CAP_BATTERY && !TINY_CAP_PARTIAL_REFRESH,
    .memory_kb_max = 64,
    .task_memory_kb_max = 64,
    .store_kb_max = 64,
    .package_max = 256 * 1024,
    /* plan 3.9 phase budgets, in install-check terms (tph_pkg.c budgets) */
    .heap_render = 200 * 1024,
    .heap_task = 184 * 1024,
    .layer_budget = 96 * 1024,
    /* one open response of <= 16 KiB (Open-Meteo, 8 days hourly x 4 fields: 6.5 KB) */
    .http_handles = 1,
    .http_body_max = 16384,
    /* partitions.csv `code` (OG 56 KiB): the module runs from its mapping, the load copy is freed */
    .code_partition = BOARD_CODE_PARTITION,
    .photo_store = PHOTO_STORE,
    .batt_cell = BOARD_BATT_CELL,
};
#endif

/* hal_button_since: every read of a GPIO button (io.c) notes it; boards reading their inputs apart (X touch bar, X4
 * Pro keys) do not, and report 0 (the app then counts from when it saw the press). */
static uint64_t s_btn_since;

void hal_esp_button_note_at(bool down, uint64_t at)
{
    if (!down)
        s_btn_since = 0;
    else if (!s_btn_since)
        s_btn_since = at ? at : 1;
}

void hal_esp_button_note(bool down) { hal_esp_button_note_at(down, hal_rtc_us()); }

uint64_t hal_button_since(void) { return s_btn_since; }

/* hal_button_take: the first press that ended unseen by the app (a panel wait's sampling, the wake's interrupt) */
static uint64_t s_latch_down, s_latch_up;
static uint8_t s_latch_more; /* presses that ended after it, before it was taken (hal_button_more) */

void hal_esp_button_latch(uint64_t up_rtc)
{
    if (!s_btn_since)
        return;
    if (!s_latch_up) {
        s_latch_down = s_btn_since;
        s_latch_up = up_rtc > s_btn_since ? up_rtc : s_btn_since + 1;
        s_latch_more = 0;
    } else if (s_btn_since > s_latch_up && up_rtc >= s_btn_since + 40000 && s_latch_more < 255) {
        s_latch_more++; /* another press, not noise (the app's IN_LATCH_MIN_MS) */
    }
}

void hal_esp_button_latch_more(uint8_t n)
{
    if (s_latch_up)
        s_latch_more = (uint8_t)(s_latch_more + n < 255 ? s_latch_more + n : 255);
}

#if BOARD_BUTTON_TOUCHBAR
/* Every press of the TRMNL X touch bar (src/btn_queue.h): its task queues each one at its release, also the one the
 * app watched (x_board.c); taken in order after the latch */
static bq_t s_bq;
static portMUX_TYPE s_bq_mux = portMUX_INITIALIZER_UNLOCKED;
static int8_t s_take_swipe;
static bool s_take_swipe_set;
uint8_t hal_esp_button_input = TINY_INPUT_TOUCH;
bool hal_esp_button_brush;

bool hal_esp_button_queue(uint64_t down_rtc, uint64_t up_rtc, uint8_t input, int8_t swipe, bool brush)
{
    portENTER_CRITICAL(&s_bq_mux);
    bool ok = bq_push(&s_bq, down_rtc, up_rtc, input, swipe, brush);
    portEXIT_CRITICAL(&s_bq_mux);
    return ok;
}

bool hal_esp_button_taken_swipe(int8_t *d)
{
    if (!s_take_swipe_set)
        return false;
    s_take_swipe_set = false;
    *d = s_take_swipe;
    return true;
}
#endif

uint8_t hal_button_more(void)
{
    uint8_t n = s_latch_more;
    s_latch_more = 0;
    return n;
}

bool hal_button_take(uint64_t *down_rtc, uint64_t *up_rtc)
{
#if BOARD_BUTTON_TOUCHBAR
    if (!s_latch_up) {
        bq_press_t p;
        portENTER_CRITICAL(&s_bq_mux);
        bool got = bq_take(&s_bq, &p);
        portEXIT_CRITICAL(&s_bq_mux);
        if (!got)
            return false;
        *down_rtc = p.down;
        *up_rtc = p.up;
        s_latch_more = 0; /* never merged: the app counts */
        s_take_swipe = p.swipe; /* hal_button_swipe: this press's */
        s_take_swipe_set = true;
        hal_esp_button_input = p.input; /* hal_inputs_woke: this press's */
        hal_esp_button_brush = p.brush; /* hal_button_brush: this press's */
        return true;
    }
#endif
    if (!s_latch_up)
        return false;
    *down_rtc = s_latch_down;
    *up_rtc = s_latch_up;
    s_latch_up = 0;
    return true;
}

/* Button inputs (docs/spec/firmware.md "Buttons"): the X4 Pro reads its keys apart, else the one button */
uint8_t hal_inputs_woke(void)
{
#if BOARD_X4P
    return hal_x4p_inputs_woke();
#elif BOARD_BUTTON_TOUCHBAR
    return (uint8_t)(1u << hal_esp_button_input); /* the bar's zone of the press taken or watched last */
#else
    return 0; /* one input */
#endif
}

void hal_input_diag(char *out, size_t cap)
{
#if BOARD_BUTTON_TOUCHBAR && !BOARD_X4P
    hal_x_diag(out, cap);
#else
    snprintf(out, cap, "-");
#endif
}

uint8_t hal_inputs_down(void)
{
#if BOARD_X4P
    return hal_x4p_inputs_down();
#elif BOARD_BUTTON_TOUCHBAR
    return hal_x_inputs_down(); /* the bar's zone (x_board.c) */
#else
    return hal_button_down() ? (uint8_t)(1u << TINY_INPUT_MAIN) : 0;
#endif
}

#if BOARD_BUTTON_TOUCHBAR
/* the press taken last (hal_button_take), or one that ended by a rule, not queued (x_board.c press_end) */
bool hal_button_brush(void) { return hal_esp_button_brush; }
#endif

/* Buttons without a direction (x_board.c: the touch bar has its own). */
__attribute__((weak)) int8_t hal_button_swipe(void) { return 0; }

/* Panels without faster waveforms ignore the refresh speed (panel_bwry.c has its own). */
__attribute__((weak)) void hal_panel_speed(uint8_t speed) { (void)speed; }

/* Only the X panel has a fast and a slow clear (panel_par.c). */
__attribute__((weak)) void hal_panel_new_image(bool image) { (void)image; }

const hal_board_t *hal_board(void)
{
    board.rows_bottom_up = hal_esp_panel_bottom_up(); /* BWRY: depends on the saved init profile */
#if BOARD_PANEL_BWRY
    board.colors = TINY_COLORS_BWRY; /* depth-2 frames may hold palette codes (panel_bwry.c) */
    /* "!tp part on" (experimental): partial while panel_bwry.c allows it */
    board.partial_refresh = hal_esp_bwry_partial();
    board.refresh_auto = hal_esp_bwry_luts(); /* built with register LUTs (panel_bwry.c) */
#endif
#if BOARD_PANEL_FASTEPD
    board.photo_levels = 0; /* photos at depth 4 on all 16 grays with the stock gray table (0x8421 with FastEPD's, hal.h) */
    board.partial_refresh = TINY_CAP_PARTIAL_REFRESH != 0 && hal_esp_par_partial(); /* "!tp every n" */
#endif
    return &board;
}
