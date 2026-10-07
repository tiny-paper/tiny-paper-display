/* hal_esp internals. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HAL_ESP_PRIV_H
#define HAL_ESP_PRIV_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_esp.h"
#include "sdkconfig.h"
#include "tiny/hal.h"

#include "board.h"        /* boards/<TINY_BOARD>/board.h */

#define HAL_PIN_BUTTON BOARD_BUTTON          /* wake button, active low */
#ifndef BOARD_BATT_GAUGE
#define BOARD_BATT_GAUGE 0
#endif
#ifndef BOARD_BUTTON_EXT1
#define BOARD_BUTTON_EXT1 0
#endif
#ifndef BOARD_BUTTON_TOUCHBAR
#define BOARD_BUTTON_TOUCHBAR 0
#endif
#ifndef BOARD_PANEL_FASTEPD
#define BOARD_PANEL_FASTEPD 0
#endif
#ifndef BOARD_PANEL_BWRY
#define BOARD_PANEL_BWRY 0
#endif
#ifndef BOARD_VBUS_SENSE
#define BOARD_VBUS_SENSE 0 /* board.h: 1 when VBUS is sensed (TRMNL X, hal_x_vbus); link.c hal_esp_vbus_state */
#endif
#ifndef BOARD_X4P
#define BOARD_X4P 0
#endif
#if !BOARD_BATT_GAUGE
#define HAL_PIN_BATT_ADC BOARD_BATT_ADC_GPIO /* C3: ADC1 channel n on GPIO n (0..4) */
_Static_assert(BOARD_BATT_ADC_GPIO <= 4, "battery ADC must be on ADC1 (GPIO0..4)");
#endif

/* Per-wake numbers for the one summary line printed before sleep. */
typedef struct {
    uint64_t boot_rtc_us;
    uint64_t light_at_boot_us; /* hal_light_us() at boot_rtc_us */
    int64_t tick_late_us;      /* hal_light_sleep_until: wake-up minus target, last call */
    uint8_t refresh_kind;      /* 0 none, 1 full, 2 partial */
    uint16_t rects;
    int16_t bx0, by0, bx1, by1; /* bounding box of the windows written */
    uint32_t refresh_ms;
    int batt_mv;
    uint32_t adc_bad; /* PicPak: ADC readings of a pressed level the digital input contradicted (io.c) */
    uint32_t heap_free[HAL_ESP_PH_COUNT];   /* minimum free heap at the end of each phase */
    uint32_t wifi_ms;
} hal_wake_stats_t;

extern hal_wake_stats_t hal_stats;

/* net.c: esp_netif, the default event loop and the STA netif, once at boot (long-lived: kept out of the
 * heap's largest block) */
void hal_esp_net_init(void);

/* sys.c: a breadcrumb in RTC memory for the radio steps that ran into resets on the PicPak (TG1WDT_SYS_RST
 * right after portal_before, 2026-09-25): set before a step, HAL_ESP_ST_NONE after it. A watchdog, panic or
 * brownout reset during one is logged on the next boot: "W reset=<why> during <step>". */
typedef enum {
    HAL_ESP_ST_NONE,
    HAL_ESP_ST_WIFI_INIT,
    HAL_ESP_ST_WIFI_START,
    HAL_ESP_ST_WIFI_SCAN,
    HAL_ESP_ST_AP_START,
    HAL_ESP_ST_WIFI_CONNECT,
    HAL_ESP_ST_HTTP,            /* hal_http: DNS, TLS, request, body (PicPak 2026-09-26: a hang in a check-in) */
    HAL_ESP_ST_SNTP,
    HAL_ESP_ST_PANEL,           /* a panel driver call (hal_panel_*, "!tp" panel commands): an OS reset, not counted */
    HAL_ESP_ST_COUNT
} hal_esp_stage_t;
void hal_esp_stage(hal_esp_stage_t st);
hal_esp_stage_t hal_esp_stage_now(void);

/* sys.c: the core task's watchdog (docs/spec/firmware.md "Watchdog"). The core's thread is subscribed to the task
 * watchdog (CONFIG_ESP_TASK_WDT_TIMEOUT_S, panic on timeout); this feeds it, at most once a second, and only from
 * that thread (a no-op elsewhere and in ISRs). hal_rtc_us feeds it: every wait loop of the core reads the clock.
 * Blocking HAL steps that do not (HTTP body, WiFi connect, SNTP, panel BUSY) call it themselves. */
void hal_esp_wdt_feed(void);

/* io.c: the button's GPIO wake may be armed (PicPak: false when the released pin already reads low at a low
 * battery, src/btn_level.h); true on other boards. */
#if BOARD_BUTTON_SHARED_ADC
bool hal_esp_button_wake_ok(void);
#else
static inline bool hal_esp_button_wake_ok(void) { return true; }
#endif

/* part_stats.c: the flash operations since boot (esp_partition_read, _write, _erase_range, wrapped at link time), their
 * count and time; fs.c books the difference over a file's life to that file ("fs_file", issues #114 #122). */
typedef struct {
    uint32_t reads, progs, erases;
    uint64_t read_us, prog_us, erase_us;
} hal_esp_flash_stats_t;
extern hal_esp_flash_stats_t hal_esp_flash_stats;

/* sys.c: "!tp heap" (the heap map) */
bool hal_esp_sys_textcmd(const uint8_t *buf, size_t len);
/* part.c (CONFIG_TINY_PHOTO_PART): "!tp flash ...", "!tp photos ..." */
bool hal_esp_part_textcmd(const uint8_t *buf, size_t len);
/* flash_chip.c: reads above 16 MB through ESP-IDF's generic DIO 4-byte path instead of READ4B (stress `dio` only) */
extern bool hal_esp_mxic32_dio4b;
/* flash_chip.c: one read-only command (SFDP, status/configuration registers) for esp_flash_read_unique_chip_id to run
 * under the flash lock (esp/part.c flash_raw); NULL otherwise */
typedef struct {
    uint8_t cmd, addr_bits, dummy, len; /* len <= 64 */
    uint32_t addr;
    uint8_t *buf;
} hal_esp_flash_raw_t;
extern hal_esp_flash_raw_t *hal_esp_flash_raw;
/* flash_chip.c: READ4B transactions checked against the SPI1 registers, mismatches, the first one's registers */
typedef struct {
    uint32_t reads, bad;
    uint32_t want, addr, user, user1, user2, ctrl;
} hal_esp_mxic32_diag_t;
extern hal_esp_mxic32_diag_t hal_esp_mxic32_diag;

/* link diag (hal_diag, esp/diag.c): sys.c's heap map into d->blocks; this boot's reset and the radio step it hit
 * ("" none); diag.c's boot hook (after sys.c's wake_init) for the resets ring and the core step; a board's extra
 * line (x_board.c: the touch bar; weak "" elsewhere) */
void hal_esp_heap_blocks(hal_diag_t *d);
const char *hal_esp_reset_step(void);
void hal_esp_diag_boot(void);
/* Boot faults belong to one cycle; reset history and the panic record survive cycle completion. */
void hal_esp_diag_boot_logged(void);
const char *hal_esp_diag_wake_reset(void);
void hal_esp_diag_cycle_done(void);
void hal_esp_log_stacks(void); /* stacks.c: `stack task=<name> free=<bytes>` per task; nothing without CONFIG_TINY_STACK_REPORT */
void hal_esp_board_diag(char *out, size_t cap);

/* link.c */
void hal_link_init(void);
bool hal_link_host_present(void);
int hal_esp_vbus_state(void); /* VBUS as the board senses it: 1 present, 0 absent, -1 no sense (OG, PicPak) */
/* Wait up to timeout_ms for link bytes; keep them in the stash for hal_link_read. */
bool hal_link_stash_wait(uint32_t timeout_ms);
void hal_link_flush(void);
void hal_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ble_link.c: the BLE transport behind hal_link_*(HAL_LINK_BLE) (config mode), and "!tp ble ..." */
int hal_ble_open(void);
void hal_ble_close(void);
long hal_ble_read(uint8_t *buf, size_t cap, uint32_t timeout_ms);
long hal_ble_write(const uint8_t *buf, size_t len);
bool hal_ble_textcmd(const uint8_t *buf, size_t len);

/* sys.c: light sleep until GPIO `pin` reads `level` or `max_us` (real time) passed; returns false without
 * sleeping while awake on USB (the USB Serial/JTAG drops in light sleep: the caller polls then) or for waits
 * under a few ms. Counted in hal_light_us. Panel waits (BUSY) use it on battery. */
bool hal_esp_light_sleep_pin(int pin, int level, uint64_t max_us);
/* Light sleep until `pin` (a BUSY line, low = busy) goes high, the button goes down (button: its GPIO wake armed) or
 * max_us; false when it did not sleep (awake on USB, BUSY already high, max_us < 5 ms). Counted in hal_light_us. */
bool hal_esp_light_sleep_busy(int pin, uint64_t max_us, bool button);
bool hal_esp_wifi_on(void); /* WiFi started (STA or AP) */
/* board.c: a read of the button (io.c), for hal_button_since */
void hal_esp_button_note(bool down);
void hal_esp_button_note_at(bool down, uint64_t at_rtc); /* the same, the press began at at_rtc */
/* board.c: the press noted down ended at up_rtc where the app could not look: kept for hal_button_take (the first
 * one until taken). Call before the note of the release. */
void hal_esp_button_latch(uint64_t up_rtc);
void hal_esp_button_latch_more(uint8_t n); /* n more presses ended after the latched one (hal_button_more) */
/* board.c (BOARD_BUTTON_TOUCHBAR): a press that ended by a release, from any task (src/btn_queue.h; false: the queue
 * was full); hal_button_take returns them in order after the latch. The swipe of the press hal_button_take returned last
 * (once; false: that take was not a queued press, or it was read). hal_esp_button_input: the input (the bar's zone)
 * of the press taken last or, after it, of the last press the app watched (x_board.c): hal_inputs_woke;
 * hal_esp_button_brush the same for its brush (touch_btn.h): hal_button_brush. */
bool hal_esp_button_queue(uint64_t down_rtc, uint64_t up_rtc, uint8_t input, int8_t swipe, bool brush);
bool hal_esp_button_taken_swipe(int8_t *d);
extern uint8_t hal_esp_button_input;
extern bool hal_esp_button_brush;
/* io.c: a read of the button during a panel wait (PicPak BUSY): a release there is latched (hal_button_take) */
void hal_esp_button_poll(void);
/* io.c (GPIO button boards; weak no-op elsewhere): a button wake, first thing at boot: the button read at once and,
 * when still down, its press dated to the wake (OG 2026-09-26: measured from the app start, ~1.5 s later, every tap
 * released by then was taken as hold1) */
void hal_esp_button_wake(void);
/* Around every light sleep: the panel HAL holds its output pins (reset, chip select) at their levels, so the
 * controller sees no edge while the chip sleeps (weak no-op elsewhere). */
void hal_esp_light_sleep_pins(bool hold);

/* sys.c: stash for link bytes that arrived during a stay-awake wait */
uint8_t *hal_sys_stash(uint16_t **len, uint16_t *cap);

/* sys.c: fast test mode (!tp fast k), hal_rtc_us runs k times faster than real time */
#define HAL_ESP_FAST_MAX 6
unsigned hal_esp_fast(void);           /* current k, 1 = off */
void hal_esp_fast_set(unsigned k);     /* re-base the scaled clock (continuous) and switch to k */

/* fs.c / kv.c */
void hal_kv_init(void);
void hal_fs_init(void);

/* sys.c: the storage is being formatted (first boot). Until this wake cycle ends (hal_sleep_until),
 * the task watchdog does not watch the idle task, and a watchdog or panic reset is not counted as
 * a crash (safe mode); earlier crashes are forgotten (fresh storage). */
void hal_esp_first_boot_begin(void);

/* io.c (ADC boards) / x_board.c (BOARD_BATT_GAUGE: TRMNL X) */
void hal_io_init(void);
/* Before deep sleep: board pins and chips to their low-power state. Returns false when the button
 * wake must not be armed (its line is low and would wake the chip at once). */
bool hal_io_sleep_prep(bool button_wake);

/* x4p_board.c (BOARD_X4P: Xteink X4 Pro): the ext1 deep-sleep wake pins (released keys, set by hal_io_sleep_prep);
 * true when the X4 Classic's IMU answered (other pins: the panel, light and touch rail are never driven);
 * the factory NVS value hw_calib/screenType (< 0 = none). */
uint64_t hal_x4p_wake_mask(void);
bool hal_x4p_is_classic(void);
uint8_t hal_x4p_inputs_down(void); /* TINY_INPUT_POWER, TINY_INPUT_RIGHT, TINY_INPUT_LEFT bits */
uint8_t hal_x4p_inputs_woke(void); /* ... of the key whose ext1 wake started this boot */
int hal_x4p_screen_type(void);
void hal_x4p_show(void); /* "!tp x4p": keys, gauge, clock chip, light, touch */
/* A finished touch stroke is waiting for hal_touch_take (polls the controller while it is on; false on other boards):
 * the USB wait (sys.c stay_awake_until) returns HAL_WAKE_BUTTON for it. */
bool hal_esp_touch_ready(void);

/* x_board.c: the internal I2C bus 0 (created once; FastEPD reuses it). HAL_OK or HAL_E_IO. */
int hal_x_i2c_init(void);
/* x_board.c: "!tp touch [on|off|init|watch [s]|log on|off|swipe on|off]" (argv after "touch"); charger power good (VBUS present) */
void hal_x_touch_cmd(int argc, char **argv);
bool hal_x_vbus(void);
/* x_board.c: ESP32-C5 modem EN (off, or a normal boot after 100 ms off); HAL_OK or HAL_E_IO */
int hal_x_c5_power(bool on);
/* x_board.c: the input bit of the bar's zone held now (TINY_INPUT_LEFT, TINY_INPUT_TOUCH the middle, TINY_INPUT_RIGHT;
 * firmware.md "TRMNL X touch bar"), 0: up */
uint8_t hal_x_inputs_down(void);
/* x_board.c: the touch bar task's state, one line (hal_input_diag) */
void hal_x_diag(char *out, size_t cap);

/* panel.c / panel_bwry.c */
void hal_panel_init(void);
/* hal_board()->rows_bottom_up: the panel takes its rows last first (BWRY stock profile) */
bool hal_esp_panel_bottom_up(void);
/* panel_bwry.c: "!tp part on" saved; and a partial may go out now (hal_board()->partial_refresh) */
bool hal_esp_bwry_part_on(void);
bool hal_esp_bwry_partial(void);
bool hal_esp_bwry_luts(void); /* register LUTs built in (config refresh_speed auto) */
/* panel_par.c: a partial may go out now ("!tp every n": not after n partials; hal_board()->partial_refresh) */
bool hal_esp_par_partial(void);
/* The app's next refresh a full one (the panel no longer shows its frame; "!tp box", panel_par_box.c). */
void hal_esp_par_force_full(void);
/* panel_par_log.c: "rowmax=..." of the last update into b; the "panel rows" line (and its W line) after an update */
const char *hal_esp_par_rows_str(char *b, size_t n);
void hal_esp_par_log_drive(bool full);
/* Console partials (panel_par_box.c): not refused by a pending full of the app; the state for their error line. */
void hal_esp_par_console(bool on);
void hal_esp_par_state_str(char *b, size_t n);
/* "!tp box x y w h [inv|diff]" (panel_par_box.c): a 1 bpp partial of a known shape from the console; true if handled. */
bool hal_esp_par_box_cmd(int argc, char **argv);
bool hal_esp_par_win_all(void); /* "!tp win all": the app's partials drive every pixel of their windows */
/* Pixels that differ between two 1 bpp windows (w px wide, h rows of `stride` bytes), for the "panel win" line. */
uint32_t hal_esp_par_win_diff(const uint8_t *prev, const uint8_t *next, size_t stride, int w, int h);
/* "!tp ..." text command at the start of a USB read: handled and consumed. */
bool hal_textcmd(const uint8_t *buf, size_t len);

#endif
