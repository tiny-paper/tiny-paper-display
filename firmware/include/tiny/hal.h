/*
 * tiny/hal.h - the only way firmware core code touches hardware.
 * Implemented by firmware/hal/esp (ESP-IDF) and firmware/hal/sim (Linux, virtual
 * time, panel emulator, energy model). Core code never includes ESP-IDF headers.
 *
 * Conventions: functions returning int give HAL_OK (0) or a negative HAL_E_*.
 * Paths are absolute within the flash filesystem ("/plugin/a/pkg.tiny").
 * All calls are blocking; the sim advances virtual time inside them.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_HAL_H
#define TINY_HAL_H

#include "tiny/types.h"

enum {
    HAL_OK = 0,
    HAL_E_FAIL = -1,     /* generic failure */
    HAL_E_TIMEOUT = -2,
    HAL_E_NOTFOUND = -3, /* file, key, network */
    HAL_E_NOSPACE = -4,
    HAL_E_DENIED = -5,   /* e.g. address refused by public_only */
    HAL_E_TOOBIG = -6,   /* response larger than the buffer */
    HAL_E_IO = -7,       /* DNS, connect, TLS, flash I/O */
    HAL_E_AGAIN = -8,    /* HTTP body slice exhausted: check presses, then read again */
};

/* ------------------------------------------------------------------ board */

/* Plugin renderers (manifest.md `renderer`, link hello.renderers). */
#define TINY_RENDERER_SCENE 0x1u
#define TINY_RENDERER_WASM 0x2u

typedef struct {
    const char *id;            /* "trmnl_og", "trmnl_x", "trmnl_diy", "picpak", "xteink_x4pro", "sim" */
    uint16_t panel_w, panel_h; /* native panel size */
    uint8_t depths;            /* bit mask: bit0 = 1-bit, bit1 = 2-bit, bit2 = 4-bit */
    /* caps.json battery && !partial_refresh: default 5-minute rhythm on battery, independent of runtime LUTs. */
    bool battery_full_refresh;
    bool partial_refresh;      /* false on picpak */
    /* The panel takes its rows last first and the HAL streams full refreshes without a frame buffer
     * (picpak, stock init profile): a full refresh is written as full-width bands, each exactly once,
     * the bottom band first and each next one ending where the previous began. Otherwise the bands
     * of a full refresh come top first (partial windows in any order). */
    bool rows_bottom_up;
    /* The button can't report a reliable hold (TRMNL X touch bar, 2026-09-25: read as held forever, 9
     * factory resets in a row): every press is a short press, no long press and no factory reset by button. */
    bool button_short_only;
    /* The button is easily held by accident (TRMNL X touch bar, e.g. carried by its lower edge): no safe mode from
     * a hold at power-on, no factory reset offer on the safe screen. (No hold resets on any board since 2026-09-27.) */
    bool button_no_reset;
    /* plugin limits (manifest.md "Limits per board") */
    uint16_t memory_kb_max, task_memory_kb_max, store_kb_max;
    uint32_t package_max;
    /* heap budget per phase in bytes (plan 3.9), for install-time checks */
    uint32_t heap_render, heap_task;
    /* flash bytes for cached plugin layers (manifest.md "Limits per board") */
    uint32_t layer_budget;
    /* plugin HTTP per task run (host-abi.md "HTTP (TASK)"): open handles (1..2) and response
     * body cap in bytes (<= 32768); 0 = the defaults, 2 and 32768 */
    uint8_t http_handles;
    uint32_t http_body_max;
    /* bytes of the `code` flash partition hal_code_map() runs plugin code from (0 = none: the
     * code stays in RAM); a module that fits counts once in the install budgets (tph_pkg.c) */
    uint32_t code_partition;
    /* plugin renderers the board runs, TINY_RENDERER_* bits; 0 = every renderer the firmware was built with
     * (docs/spec/firmware.md "Renderers": scene only, tph_renderers(); WebAssembly plugins, WAMR, are gone) */
    uint8_t renderers;
    /* the panel's colors (link hello `colors`, docs/spec/assets.md "Palette frames"): TINY_COLORS_GRAY, or
     * TINY_COLORS_BWRY (black, white, red, yellow: depth-2 frames may hold palette codes, hal_panel_palette) */
    uint8_t colors;
    /* photos at depth 4 (docs/spec/assets.md "Photos", image-fetch.md): the gray levels they are dithered to, bit k =
     * level k of 16; 0 = all 16 (every board today; the TRMNL X had 0, 5, 10, 15 with FastEPD's uneven 16-gray
     * table, all 16 since it drives stock trmnl-firmware's table) */
    uint16_t photo_levels;
    /* a flash area of its own for photos (store.md "Image area"): album mode and kept link photos (`keep`) only
     * there. Without it (every board so far) photos use the fixed image area: the shown image and one next */
    bool photo_store;
    /* config `refresh_speed` `auto` ("fast when warm enough", firmware.md "PicPak refresh speed"): the panel picks a
     * register LUT by its temperature (PicPak built with LUTs); report `features` `refresh_auto` */
    bool refresh_auto;
    /* a touch screen over the panel (xteink_x4pro GT911): hal_touch_*, on only in touch windows (firmware.md "Touch") */
    bool touch_screen;
    /* a front light (xteink_x4pro): hal_frontlight; the app's attention for a fresh delivery (firmware.md "Rules") */
    bool frontlight;
    /* the battery cell (power.c curves, firmware.md "Battery"): HAL_BATT_LIPO (4.2 V full, 0 = the default) or
     * HAL_BATT_LIHV (4.35 V full: the TRMNL X) */
    uint8_t batt_cell;
} hal_board_t;

#define HAL_BATT_LIPO 0
#define HAL_BATT_LIHV 1

#define TINY_COLORS_GRAY 0
#define TINY_COLORS_BWRY 1

const hal_board_t *hal_board(void);

/* ------------------------------------------------------------------ plugin code (plugin_host) */

/* A read-only, memory-mapped copy of `len` bytes equal to `bytes`: a scene plugin's SCEN section,
 * so plugin_host can free its RAM copy and run from the mapping. Device: the `code` flash
 * partition (store.md), rewritten only when its content differs (first open after an install or
 * a firmware update). Sim: a malloc'd copy.
 * One mapping at a time; NULL if busy, too big or failed (plugin_host keeps the RAM copy). */
const uint8_t *hal_code_map(const uint8_t *bytes, size_t len);
/* The same, the bytes read in chunks from `read(ctx, off, buf, n)` (0 = ok): a scene section (scene.md)
 * goes from the package file to the mapping without a RAM copy. NULL as above, or when a read fails.
 * write false (an install check, plugin_info): the mapping only when the partition already holds these bytes,
 * never a flash write; NULL otherwise (the caller makes a RAM copy). */
const uint8_t *hal_code_map_read(int (*read)(void *ctx, uint32_t off, void *buf, size_t len), void *ctx,
                                 size_t len, bool write);
void hal_code_unmap(const uint8_t *map);

/* ------------------------------------------------------------------ clock, sleep, reset */

/* Slow-clock (RTC) time in microseconds since cold boot. Monotonic, keeps
 * counting through deep sleep, uncorrected (drift correction is timekeeper's job).
 * The sim applies its configured RTC error here. */
uint64_t hal_rtc_us(void);

typedef enum {
    HAL_WAKE_COLD = 0,   /* power-on or reset */
    HAL_WAKE_TIMER,
    HAL_WAKE_BUTTON,
    HAL_WAKE_USB,        /* USB host: link bytes are waiting */
} hal_wake_t;

hal_wake_t hal_wake_cause(void);

/* Light sleep (RAM kept) until hal_rtc_us() >= rtc_us. Returns early on a button press
 * (returns 1) or 0 when the time was reached. */
int hal_light_sleep_until(uint64_t rtc_us);

/* Time spent in light sleep since this boot, in hal_rtc_us units: hal_light_sleep_until and the HAL's own
 * waits (a panel's BUSY). The core takes differences, to count only the time the CPU runs as awake
 * (store.md battery history `awake_s`). 0 on a HAL that never light-sleeps. */
uint64_t hal_light_us(void);

/* Deep sleep. Only RTC memory survives; the next boot starts from main().
 * rtc_us = 0: no timer (wake only by button/USB). Does not return. */
_Noreturn void hal_deep_sleep_until(uint64_t rtc_us, bool button_wake);

/* End of a wake cycle. On battery: hal_deep_sleep_until (does not return). While the device
 * stays awake (a USB host is attached) it waits in place, RAM kept, until the wake time
 * (rtc_us, 0 = none), a button press (button_wake) or link bytes, and returns the cause:
 * HAL_WAKE_TIMER, HAL_WAKE_BUTTON or HAL_WAKE_USB (read the bytes with hal_link_read).
 * If the host goes away while waiting it deep-sleeps for the rest. */
hal_wake_t hal_sleep_until(uint64_t rtc_us, bool button_wake);

/* Provided by the core (the HALs have a weak no-op): called about every 20 ms from long
 * blocking HAL waits (panel refresh BUSY, awake light sleep) while a USB host is attached,
 * so quick link requests are answered during a wake cycle. Must not touch the panel. */
void hal_busy_hook(void);

_Noreturn void hal_reboot(void);

/* Live seconds (experimental, TRMNL X): the user turned it on and the device stays awake on USB right now.
 * The core then refreshes the plugin every second between its minute ticks (host-abi.md time_local). Boards
 * without it return false. */
bool hal_live_seconds(void);

/* Restart the chip in place of a wait (RAM lost, RTC memory kept): after hours of cycles in place
 * on USB the heap can be too fragmented for a plugin instance, and a restart puts it back in one
 * piece. The next boot's hal_wake_cause() is `as`, so it is not a cold boot. */
_Noreturn void hal_restart_as(hal_wake_t as);

/* Largest free heap block in bytes; SIZE_MAX when the HAL does not track it (host builds). */
size_t hal_heap_largest(void);
/* Free heap in bytes (all blocks); SIZE_MAX when the HAL does not track it. */
size_t hal_heap_free(void);
/* Log the heap map (`heapmap` lines, as the console's "!tp heap"; remote command `diag_upload`). Optional: the core
 * has a weak no-op. */
void hal_heap_map_log(void);

/* Persistent RTC memory (survives deep sleep, not power loss). At least 1024 bytes.
 * Content is garbage after a cold boot; callers validate it (store.h tiny_rtc_state_t). */
void *hal_rtc_mem(size_t *size);

/* Cryptographically secure random bytes. (Not "hal_random": the ESP32 WiFi blob exports that name.) */
void hal_random_bytes(void *buf, size_t len);

/* ------------------------------------------------------------------ panel */

typedef enum { HAL_REFRESH_FULL = 0, HAL_REFRESH_PARTIAL = 1 } hal_refresh_t;

/* Power up / init the controller for this depth and refresh kind. */
int hal_panel_begin(uint8_t depth, hal_refresh_t mode);

/* Send one panel-native area. r->x and r->w are multiples of 8. `prev` and `next`
 * point at the first byte of the area's first row inside buffers packed per
 * assets.md (row-major, MSB-first, 0 = black), `stride` bytes per row.
 * PARTIAL needs both (old and new data); FULL ignores prev (may be NULL).
 * Several calls may precede one refresh (dirty rects, render bands). A full refresh
 * comes as full-width bands, top first or, on a rows_bottom_up board, bottom first. */
int hal_panel_write(const tiny_rect_t *r, const uint8_t *prev, const uint8_t *next, size_t stride);

/* Color panels (hal_board_t colors): the frame of the next hal_panel_begin holds palette codes (GFX_PAL_BWRY, docs/spec/
 * assets.md "Palette frames"), or gray levels (GFX_PAL_GRAY). Called before every hal_panel_begin; other boards
 * ignore it. */
void hal_panel_palette(uint8_t palette);

/* The panel ID read once at boot (UC8179 REV, TRMNL OG; epd_rev.h), 0 = unknown or a panel without that register
 * (report `panel_rev`, remote.md "Report"). */
uint32_t hal_panel_rev(void);

/* The panel takes gray partials (TRMNL X "!tp gpart on", experimental): hal_panel_begin(2 or 4, PARTIAL)
 * with gray windows, only changed pixels driven, no flash. Boards without it return false; the core then
 * sends gray changes as a full refresh. */
bool hal_panel_gray_partial(void);

/* Panel tuning for experiments from a host (TRMNL X; usbdisp `set { tuning }`): the driver's knobs of the `!tp` console
 * commands as named integers, each with its range and default. A value applies from the next refresh and is NOT stored
 * (it lasts until the next restart). `words`: NULL for a number, else the names of min..max (words[v - min]).
 * Boards without tuning: count 0. */
typedef struct {
    const char *name;
    int16_t min, max, def;
    const char *const *words;
} hal_tune_t;
size_t hal_panel_tune_count(void);
const hal_tune_t *hal_panel_tune_info(size_t i);
int hal_panel_tune_get(size_t i);
bool hal_panel_tune_set(size_t i, int v); /* false: outside min..max, nothing changed */
void hal_panel_tune_defaults(void);

/* Box partial (the press mark on a panel without a quick partial: PicPak, firmware.md "Press mark"): while
 * true, hal_panel_begin(depth, PARTIAL) is taken also when partial_refresh is false. The areas written must form one
 * window (the same x range, every row of it once); every pixel of it is driven from `next` alone (prev ignored), at
 * the frame's depth and palette; the rest of the panel is not driven. Same waveform and time as a full refresh at
 * the current speed. False after a cold boot, a failed refresh and N partials (a full one first). Boards without
 * it: false (weak default). */
bool hal_panel_box_partial(void);

/* Partial drive of the following 1 bpp partials (hal_panel_begin(1, PARTIAL)): false drives only the pixels that
 * differ between prev and next, true every pixel of the written windows. Returns whether the board can choose
 * (TRMNL X); the others ignore the call and return false (weak default). Display firmware (usb-display.md). */
bool hal_panel_partial_all(bool all);

/* Keep the redrive bitmap of the following partial refreshes in RAM only (TRMNL X: no /xfresh.bin write per partial,
 * issue #214). For firmware that stays awake and never needs it across deep sleep (the display firmware); the app
 * leaves it off. The other boards write no such file: weak default, no-op. */
void hal_panel_fresh_ram(bool ram_only);

/* The gate clock count the board measured over its last panel update (TRMNL X: CKV rising edges, issue #203):
 * `edges` over `frames` drive frames of `rows` gate rows, `per` edges a frame; ok when per is rows plus the start
 * pulses (a doubled or missing clock is not ok). False when the board does not count (weak default) or no frame ran. */
typedef struct {
    uint32_t edges, frames;
    uint16_t rows, per;
    bool ok;
} hal_gate_clocks_t;
bool hal_panel_gate_clocks(hal_gate_clocks_t *g);

/* Refresh speed of the following refreshes (device config `refresh_speed`): HAL_SPEED_*. Only panels with faster
 * waveforms (PicPak: docs/spec/firmware.md "PicPak refresh speed") act on it; the others ignore it. */
enum { HAL_SPEED_NATIVE = 0, HAL_SPEED_FAST = 1, HAL_SPEED_FASTEST = 2, HAL_SPEED_AUTO = 3 };
void hal_panel_speed(uint8_t speed);

/* The next full refresh shows new image content (a new photo, comic, art; not a clock, tile or text redraw). Called
 * before hal_panel_refresh, cleared by it. TRMNL X: auto clear is slow then (log `clear=slow why=image`), else
 * fast (slow on every 8th full as a backstop); other boards ignore it (weak default). */
void hal_panel_new_image(bool image);

/* Start the refresh of everything written since begin and block (light sleep on BUSY)
 * until the panel is done. Called at the exact minute (plan 4.3). */
int hal_panel_refresh(void);

/* Controller deep sleep; the panel keeps its image. */
void hal_panel_sleep(void);

/* ------------------------------------------------------------------ buttons, power */

bool hal_button_down(void);        /* the wake button (any input) is held right now */
/* hal_rtc_us() when the press held now began, as far as the HAL saw it (it samples the button during long panel
 * waits: PicPak ~18 s BUSY); 0: not down, or not known. The app measures a hold from it (firmware.md "Buttons"). */
uint64_t hal_button_since(void);
/* One line of the input reader's state for a log ("task_alive=1 reads=12 s_stop=0"; "-" where a board has none), for
 * the gesture test's `gtest_wait` line. */
void hal_input_diag(char *out, size_t cap);
/* A press that ended where the app could not look, timed by the HAL: sampled during a panel wait (PicPak ~18 s
 * BUSY), the release of a button wake's press caught by a GPIO interrupt before the app's first look (OG), or every
 * press, kept in order at its release, also one the app watched (TRMNL X touch bar; right after a take,
 * hal_button_swipe and hal_inputs_woke are that press's). true once per press, with its start and release in
 * hal_rtc_us(); false: none. The app counts it as a press made now (firmware.md "Buttons"). */
bool hal_button_take(uint64_t *down_rtc, uint64_t *up_rtc);
/* Presses that ended after the one hal_button_take returned, where the app could not look either (the same panel
 * wait, a button wake before the first look): 0..255, read once right after hal_button_take (then 0). They add to a
 * press count (manifest.md "Actions"); a HAL that keeps one press only: 0. */
uint8_t hal_button_more(void);
/* Button inputs (docs/spec/firmware.md "Buttons"; each board's inputs and table: components/input).
 * hal_inputs_down: bit TINY_INPUT_* of each input held now (TRMNL X: the bar's zone, left, touch or right).
 * hal_inputs_woke: the input whose press woke the device from deep sleep (0: not a button wake, or one input only): a
 * press released before the first look was that one (TRMNL X: the zone of the press taken or watched last). */
enum { TINY_INPUT_MAIN, TINY_INPUT_TOUCH, TINY_INPUT_POWER, TINY_INPUT_LEFT, TINY_INPUT_RIGHT, TINY_INPUT_UP,
       TINY_INPUT_DOWN, TINY_INPUT_SCREEN, TINY_INPUT_COUNT };
uint8_t hal_inputs_down(void);
uint8_t hal_inputs_woke(void);
/* The direction of the press that ended last, where the input reports one (TRMNL X touch bar with NVS xswipe 1, off
 * by default: its slider, swipe gesture and zones; docs/devices/trmnl-x-touch.md): -1 the finger moved left, +1 right,
 * 0 none (a tap, a hold, a board without). Once per press (then 0); a new press clears it. */
int8_t hal_button_swipe(void);
/* The press that ended last was a brush (docs/spec/firmware.md "TRMNL X touch bar" Brush; vectors
 * inputs.brush): it touched 2 or more zones of the TRMNL X bar or its slider moved >= 64 between its first and last
 * read (the finger picking the device up). Like hal_inputs_woke: the press hal_button_take returned last, or the one
 * watched last; not consumed, cleared by the next press. A board without a touch bar: false (weak, common/util.c). */
bool hal_button_brush(void);
/* Touch screen (hal_board_t touch_screen; docs/spec/firmware.md "Touch"). hal_touch_power(true) powers the controller
 * for a touch window (rail, reset, config; HAL_OK, or HAL_E_IO: not answering), false powers it off; it is always off
 * in deep sleep, so a touch never wakes the device. While on, strokes are read (hal_touch_take) and the USB wait
 * (hal_sleep_until) returns HAL_WAKE_BUTTON for one. hal_touch_take: the oldest finished stroke (finger lifted), its
 * first and last point in panel-native px and how long it was down; false: none. */
typedef struct {
    uint16_t x0, y0, x1, y1;
    uint32_t ms;
} hal_touch_t;
int hal_touch_power(bool on);
bool hal_touch_take(hal_touch_t *t);
int hal_battery_mv(void);          /* at rest, averaged; < 0 if unknown */
/* Status LED (board.h BOARD_LED; PicPak GPIO 21, active low): on/off, level kept while awake. A board without an LED,
 * or a HAL that does not implement it, does nothing (weak no-op in common/util.c). The app flashes it as press feedback
 * (firmware.md "Press feedback"); it is off before every sleep. */
void hal_led(bool on);
bool hal_usb_power(void);

/* ------------------------------------------------------------------ board extras */

/* Frontlight (xteink_x4pro: a cool and a warm LED channel). level 0..100 % (0 = off), warm 0..100 = the warm
 * channel's share. Off at every boot and in deep sleep. HAL_E_NOTFOUND on boards without one. */
int hal_frontlight(uint8_t level, uint8_t warm);

/* Battery-backed clock chip (xteink_x4pro: BM8563) keeping UTC while the device sleeps, restarts or loses its RTC
 * memory. get: the time now (unix ms) and its error bound in ms; may wait up to ~1 s for the chip's seconds edge.
 * HAL_E_NOTFOUND: no chip. HAL_E_FAIL: its time is not usable (the chip lost power, or hal_wallclock_set never
 * wrote it). set: the time now is unix_ms (from SNTP or the link); the HAL may skip the write when the chip was set
 * recently. The sim has one when the board JSON says "wallclock": true. */
int hal_wallclock_get(int64_t *unix_ms, uint32_t *err_ms);
int hal_wallclock_set(int64_t unix_ms);

/* ------------------------------------------------------------------ key/value (NVS) */

/* Returns the value length (copies only if <= cap), HAL_E_NOTFOUND. Keys <= 15 chars. */
int hal_kv_get(const char *key, void *buf, size_t cap);
int hal_kv_set(const char *key, const void *buf, size_t len);
int hal_kv_erase(const char *key);
int hal_kv_erase_all(void);

/* The device identity record (remote.md "Device identity", store.md "Identity"): kept apart from the key/value
 * namespace (ESP: NVS namespace `tpid`), so hal_kv_erase_all (factory reset) never touches it. <= 128 bytes.
 * get: the length (copies only if <= cap), HAL_E_NOTFOUND. */
int hal_id_get(void *buf, size_t cap);
int hal_id_set(const void *buf, size_t len);
/* The chip's factory-programmed id (ESP32: the base MAC in eFuse, read only, nothing burned): binds the identity
 * record to the chip, so a flash image copied onto another board makes a new identity there. Not a secret. */
void hal_chip_id(uint8_t out[6]);

/* ------------------------------------------------------------------ flash filesystem (LittleFS) */

typedef struct hal_file hal_file_t;

/* The longest name (one part of a path between '/') and path the firmware may use: every HAL refuses more, so a
 * host test or scenario fails where a device could (issue #183: the simulator took any name). The name limit is
 * esp_littlefs's "maximum object name length" with its NUL (CONFIG_LITTLEFS_OBJ_NAME_LEN=64, asserted in
 * hal_esp/esp/fs.c); the path limit is hal_esp's path buffer. Too long a name: open gives NULL, mkdir and rename
 * to it HAL_E_IO, remove HAL_E_NOTFOUND (no such file can exist). A path over HAL_FS_PATH_MAX: NULL, HAL_E_FAIL. */
#define HAL_FS_NAME_MAX 63u
#define HAL_FS_PATH_MAX 92u

/* mode: "r", "w" (create/truncate), "r+" (read/write existing), "a". NULL on error. */
hal_file_t *hal_fs_open(const char *path, const char *mode);
long hal_fs_read(hal_file_t *f, void *buf, size_t len);         /* bytes read, < 0 error */
long hal_fs_write(hal_file_t *f, const void *buf, size_t len);  /* bytes written, < 0 error */
int hal_fs_seek(hal_file_t *f, long off);                       /* absolute */
long hal_fs_tell(hal_file_t *f);
int hal_fs_truncate(hal_file_t *f, long len);
int hal_fs_sync(hal_file_t *f);                                 /* durable after return */
int hal_fs_close(hal_file_t *f);                                /* also syncs */
long hal_fs_size(const char *path);                             /* < 0 if missing */
int hal_fs_rename(const char *from, const char *to);            /* atomic, replaces `to`; a failure leaves both */
int hal_fs_remove(const char *path);                            /* file or empty dir */
int hal_fs_mkdir(const char *path);                             /* HAL_OK if it exists */
int hal_fs_usage(uint32_t *total, uint32_t *used);
/* After boot (firmware.md "Storage recovery"). HAL_FS_RECOVERY: the storage did not mount and was not formatted (its
 * data is kept); every hal_fs_* call fails. *err (may be NULL): the platform's mount error, 0 when mounted. */
typedef enum { HAL_FS_OK = 0, HAL_FS_RECOVERY = 1 } hal_fs_state_t;
hal_fs_state_t hal_fs_state(int *err);
/* Each entry of directory `dir` (not recursive): its name (no path), its size (-1: a directory). Diagnostics only
 * (remote `diag_upload`, the sim's file list); the store never lists. HAL_OK, else an error. */
typedef void (*hal_fs_list_fn)(void *ctx, const char *name, long size);
int hal_fs_list(const char *dir, hal_fs_list_fn fn, void *ctx);

/* ------------------------------------------------------------------ raw flash partitions */

/* A data partition read and written by offset, no filesystem (store.md "Photo partition": the PicPak's `photos`, the
 * upper 16 MB of its chip). NOR rules: erase sets whole HAL_PART_SECTOR blocks to 0xFF, write only clears bits (write
 * onto erased bytes). No memory mapping (the ESP32-C3 cannot map above 16 MB). Device: esp_partition; sim: a lazily
 * allocated image per board (sim board `partitions`). */
#define HAL_PART_SECTOR 4096u
typedef struct hal_part hal_part_t;
/* NULL: no such partition (a partition table from before it, other boards). The handle stays valid until reboot. */
hal_part_t *hal_part_open(const char *label);
uint32_t hal_part_size(const hal_part_t *p);
/* HAL_OK, HAL_E_FAIL (off + len beyond the partition; erase: off or len not a multiple of HAL_PART_SECTOR),
 * HAL_E_IO (flash error). len 0 is HAL_OK. */
int hal_part_read(hal_part_t *p, uint32_t off, void *buf, size_t len);
int hal_part_write(hal_part_t *p, uint32_t off, const void *buf, size_t len);
int hal_part_erase(hal_part_t *p, uint32_t off, uint32_t len);
/* The shared bounds rule of the above: true when [off, off + len) lies in a partition of `size` bytes and, with
 * `align` > 0, off and len are multiples of it (hal_part.c in components/common, host-tested). */
bool hal_part_range_ok(uint32_t size, uint32_t off, uint64_t len, uint32_t align);

/* ------------------------------------------------------------------ WiFi, SNTP, HTTP */

typedef struct {
    char ssid[33];
    char psk[64];
    uint8_t bssid[6];          /* fast connect hint, all zero = scan */
    uint8_t channel;           /* 0 = unknown */
    uint32_t ip, gw, mask, dns; /* static IP / cached lease, network byte order, 0 = DHCP */
    int8_t tx_power_dbm;
    uint8_t band;              /* HAL_WIFI_BAND_*: which radio on a board with two (TRMNL X); one-radio boards ignore it */
} hal_wifi_cfg_t;

/* hal_wifi_cfg_t.band (config `wifi_band`). TRMNL X: the S3 (2.4 GHz) or the ESP32-C5 modem (2.4 and 5 GHz); auto
 * takes the S3 unless the network was last joined on 5 GHz (channel >= 36) or the S3 does not find it. Joining through
 * the C5 costs its boot (~1.7 s) on top of timeout_ms, and without hints both radios may be tried. */
enum { HAL_WIFI_BAND_AUTO = 0, HAL_WIFI_BAND_24 = 2, HAL_WIFI_BAND_5 = 5 };

typedef struct {
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;
    uint32_t ip, gw, mask, dns; /* actual lease, for caching */
} hal_wifi_info_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    bool secure;
} hal_ap_t;

int hal_wifi_connect(const hal_wifi_cfg_t *cfg, uint32_t timeout_ms, hal_wifi_info_t *out);
void hal_wifi_off(void);                               /* radio fully off */
int hal_wifi_scan(hal_ap_t *out, size_t max, size_t *count);
int hal_sntp(int64_t *unix_ms, uint32_t timeout_ms);   /* needs a connection */

typedef struct {
    const char *method;         /* one of HAL_HTTP_METHODS (hal_http_method); NULL = GET; others: HAL_E_FAIL */
    const char *url;
    const char *headers;        /* "Name: value\n" lines or NULL; host adds Host etc. */
    const uint8_t *body;
    size_t body_len;
    /* Streaming request body (uploads): when set, the body_len bytes are read through it in pieces of at
     * most `cap` bytes (returns the bytes, <= 0 aborts with HAL_E_IO) and `body` is ignored. */
    long (*body_read)(void *body_ctx, uint8_t *buf, size_t cap);
    void *body_ctx;
    uint32_t timeout_ms;        /* per socket wait (connect, a read), 0 = the HAL default (10 s) */
    /* The whole request: DNS, connect, TLS, sending, headers and a buffered body (hal_http); for a stream the open
     * through the headers. Over it the request ends as HAL_E_TIMEOUT with nothing delivered, never a half answer.
     * 0: no whole-request bound (per-read slices and the caller's stop hook bound a stream's body). The values per
     * request kind: tiny/net_budget.h (responsiveness.md "Network steps and budgets"). */
    uint32_t budget_ms;
    bool public_only;           /* refuse private/loopback/link-local/CGNAT/multicast after DNS */
    uint32_t range_start;       /* > 0: send "Range: bytes=N-" (resumable downloads) */
    /* Response body goes to on_body when set (streaming, e.g. package downloads),
     * otherwise into resp_buf; more than resp_cap bytes fails with HAL_E_TOOBIG. */
    int (*on_body)(void *ctx, const uint8_t *data, size_t len); /* non-zero aborts */
    void *ctx;
    uint8_t *resp_buf;
    size_t resp_cap;
    /* Optional: a 3xx answer's Location header, NUL-terminated ("" when absent or longer than location_cap - 1).
     * Redirects are still not followed: the caller decides (image tasks follow a few, manifest.md). */
    char *location;
    size_t location_cap;
} hal_http_req_t;

typedef struct {
    int status;                 /* HTTP status */
    size_t body_len;            /* bytes delivered */
    char etag[80];              /* ETag response header, "" if none */
    char last_modified[32];     /* Last-Modified response header, "" if none (cut at 31 bytes; manifest.md "Fresh check") */
    char content_type[64];      /* Content-Type response header, "" if none (cut at 63 bytes) */
    char content_encoding[16];  /* Content-Encoding response header, "" if none */
    /* hal_rtc_us once the request was sent (connection and TLS set up, body written) and when the response headers
     * arrived; 0 when unknown. The check-in's server time (remote.md "Time") is taken at their middle. */
    uint64_t sent_us, hdr_us;
    uint32_t length;            /* Content-Length of the body (the whole file for a 200), 0 unknown or chunked */
} hal_http_resp_t;

/* HTTP methods (manifest.md "Formats": declarative `method`): index 0.., in this order. A request body (body or
 * body_read with body_len > 0) may go with any of them; a HEAD answer has no body. */
#define HAL_HTTP_METHOD_COUNT 15
static inline int hal_http_method(const char *m)
{
    static const char *const k[HAL_HTTP_METHOD_COUNT] = {"GET",      "HEAD",      "POST",  "PUT",  "PATCH",
                                                          "DELETE",   "OPTIONS",   "PROPFIND", "PROPPATCH", "MKCOL",
                                                          "COPY",     "MOVE",      "LOCK",  "UNLOCK", "REPORT"};
    if (!m)
        return 0;
    for (int i = 0; i < HAL_HTTP_METHOD_COUNT; i++) {
        const char *a = k[i], *b = m;
        while (*a && *a == *b) {
            a++;
            b++;
        }
        if (!*a && !*b)
            return i;
    }
    return -1;
}
enum { HAL_HTTP_GET, HAL_HTTP_HEAD, HAL_HTTP_POST };

/* One request, keep-alive and TLS session resumption handled inside. Redirects are
 * not followed. req->budget_ms bounds the whole request (HAL_E_TIMEOUT at its end). */
int hal_http(const hal_http_req_t *req, hal_http_resp_t *resp);

/* The same request as a pull stream (docs/spec/image-fetch.md: a photo decoded while it downloads): hal_http_open
 * sends it and reads the response headers into resp (req's on_body and resp_buf are not used); hal_http_read then
 * gives the body in pieces of at most cap bytes (0 at its end, HAL_E_AGAIN to retry after checking presses,
 * other negatives an error). A body read yields within 1 s; an empty slice is not EOF. The request's timeout
 * bounds time without body progress, not the whole download; hal_http_close ends the
 * request, also before the body's end (the connection is dropped: the device read only what it asked for plus the
 * network's receive window). One stream at a time, no other hal_http call while it is open; after a failed open
 * nothing is open. */
int hal_http_open(const hal_http_req_t *req, hal_http_resp_t *resp);
long hal_http_read(uint8_t *buf, size_t cap);
void hal_http_close(void);

/* ------------------------------------------------------------------ setup network (docs/spec/setup.md) */

/* Temporary soft AP for the captive setup portal (plan 9.2). While it runs the device is HAL_AP_IP; its DHCP
 * server hands out addresses with the device as router and DNS server (and the portal URL as DHCP option 114
 * where the stack supports it), a DNS responder answers every A query with HAL_AP_IP, and port 80 takes HTTP
 * requests (one connection at a time, "Connection: close"). hal_wifi_scan and hal_wifi_connect keep working
 * (AP + station; the AP follows the station's channel, so a phone may drop off for a moment). Never together
 * with BLE or a plugin instance (plan 3.9). */
#define HAL_AP_IP "192.168.4.1"
#define HAL_AP_BODY_MAX 1536

typedef struct {
    char ssid[33];
    char psk[64];               /* 8..63 characters: WPA2-PSK; "" = open */
    uint8_t channel;            /* 0 = 1 */
} hal_ap_cfg_t;

typedef struct {
    char method[8];             /* "GET", "POST", ... */
    char path[160];             /* request target as sent (path + ?query), truncated */
    char host[64];              /* Host header without port, "" if none */
    uint8_t body[HAL_AP_BODY_MAX];
    size_t body_len;            /* a longer body is answered 413 by the HAL and never returned */
} hal_ap_req_t;

/* HAL_E_NOTFOUND without WiFi (QEMU), HAL_E_NOSPACE when the heap is too small. */
int hal_ap_start(const hal_ap_cfg_t *cfg);
/* AP, DHCP, DNS and HTTP down; the radio goes off (hal_wifi_off) unless the station is connected. */
void hal_ap_stop(void);
/* Wait up to timeout_ms for the next HTTP request (DNS is answered meanwhile). 1: *req filled, answer it with
 * hal_ap_respond before the next call (an unanswered one gets 503); 0: timeout; < 0: the AP is not running. */
int hal_ap_request(hal_ap_req_t *req, uint32_t timeout_ms);
/* headers: "Name: value\n" lines or NULL. The HAL adds Content-Length, Connection: close and Cache-Control: no-store. */
int hal_ap_respond(int status, const char *content_type, const char *headers, const void *body, size_t len);
/* Stations associated right now. */
int hal_ap_clients(void);

/* ------------------------------------------------------------------ firmware update (docs/spec/firmware.md) */

/* Bytes of the inactive app slot, 0 = no OTA support. */
uint32_t hal_ota_slot_size(void);
/* Start writing an image of `size` bytes to the inactive slot (erased as written). */
int hal_ota_begin(uint32_t size);
int hal_ota_write(const void *data, size_t len);
/* commit: check the image and make it the boot slot for the next restart; false: drop it. */
int hal_ota_end(bool commit);
/* The running image is new and not yet confirmed: the bootloader returns to the previous slot on
 * the next reset. While pending, hal_reboot and hal_restart_as keep it (deep-sleep restart). */
bool hal_ota_pending(void);
int hal_ota_confirm(void);
/* Give up on the pending image: mark it invalid and restart into the previous slot. */
_Noreturn void hal_ota_rollback(void);

/* ------------------------------------------------------------------ diagnostics (link diag) */

/* What only the HAL knows, for link `diag` (link-protocol.md "Diagnostics"). Strings "" and numbers 0 when unknown. */
#define HAL_DIAG_RESETS 8
#define HAL_DIAG_PARTS 10
#define HAL_DIAG_BT 8
#define HAL_DIAG_BLOCKS 64
typedef struct {
    char chip[48];              /* "esp32c3 rev 0.4, 4 MB flash" */
    char sdk[24];               /* SDK version */
    uint32_t uptime_s;          /* since this boot */
    char reset[12];             /* this boot's reset: poweron, sw, panic, task_wdt, int_wdt, wdt, deepsleep, brownout, ... */
    char reset_step[40];        /* the step a panic, watchdog or brownout reset hit ("" none) */
    uint8_t crashes;            /* crash counter (safe mode) */
    uint8_t n_resets;           /* resets[] used, newest first; since power-on, deep-sleep wakes not counted */
    struct {
        char r[12];
        char step[40];
        uint32_t s;             /* seconds since power-on */
    } resets[HAL_DIAG_RESETS];
    bool panic;                 /* the last panic since power-on: */
    char panic_reason[48], panic_task[16];
    uint32_t panic_pc, panic_ra, panic_s;
    uint8_t n_bt;
    uint32_t bt[HAL_DIAG_BT];   /* return addresses (Xtensa); none on RISC-V */
    uint32_t heap_min;          /* minimum free heap since boot */
    uint32_t phase_min[4];      /* boot, render, net, sleep of this cycle */
    bool nvs;                   /* nvs_* valid */
    uint32_t nvs_used, nvs_free, nvs_total, nvs_ns;
    char running[17], boot[17]; /* app partition labels */
    uint32_t slot;              /* hal_ota_slot_size */
    uint8_t n_parts;
    struct {
        char label[17];
        uint8_t type, subtype;
        uint32_t off, size;
    } parts[HAL_DIAG_PARTS];
    char refresh[8];            /* this cycle: "full", "partial", "none" */
    uint32_t refresh_ms;
    char extra[120];            /* board specifics (TRMNL X: the touch controller) */
    uint16_t n_blocks;          /* heap map: blocks >= 1 KiB in address order (as "!tp heap"), at most HAL_DIAG_BLOCKS */
    struct {
        uint32_t at, size;      /* at: offset in its region */
        bool used;
        char owner[16];
    } blocks[HAL_DIAG_BLOCKS];
} hal_diag_t;

/* Fill *d (the caller zeroes it). Allocates nothing that outlives the call. */
void hal_diag(hal_diag_t *d);
/* The core's current step (<= 39 chars, NULL or "" = none), kept in RTC memory: a panic or watchdog reset during it
 * is the next boot's `reset_step` (radio steps of the HAL come first). */
void hal_diag_step(const char *step);

/* ------------------------------------------------------------------ link transport */

typedef enum { HAL_LINK_USB = 0, HAL_LINK_BLE = 1 } hal_link_t;

/* TINY_BLE: 1 = the core has Bluetooth config mode (ESP: hal_esp Kconfig CONFIG_TINY_BLE, off on every board;
 * host: cmake option TINY_BLE, on for the unit tests and the default sim). 0: a 3 s hold opens the setup network,
 * ble_setup answers UNSUPPORTED, status heap_min.ble is 0. The hal_link_ble_* calls below stay (stubs). */
#ifndef TINY_BLE
#define TINY_BLE 0
#endif

/* USB is always open. BLE exists only in config mode (link-protocol.md "Bluetooth"): hal_link_ble_setup
 * first, then open brings the stack up and advertises; close drops the connection, stops the stack and
 * frees all its memory. open: HAL_E_NOSPACE when the heap is too small, HAL_E_NOTFOUND without BLE.
 * The sim maps USB to TCP 127.0.0.1:7701 and BLE to scripted events. */
int hal_link_open(hal_link_t t);
void hal_link_close(hal_link_t t);
/* Byte stream (link-protocol.md). read returns bytes (0 on timeout), < 0 on error. BLE: write blocks
 * until the bytes are queued as notifications of at most MTU - 3 bytes (flow controlled); without a
 * subscribed peer the bytes are dropped (returns len). */
long hal_link_read(hal_link_t t, uint8_t *buf, size_t cap, uint32_t timeout_ms);
long hal_link_write(hal_link_t t, const uint8_t *buf, size_t len);
/* BLE config mode: advertised name (config `name`) and the 6-digit passkey (0..999999) the device answers
 * pairing with (LE Secure Connections, passkey entry, bonding). Before hal_link_open(HAL_LINK_BLE); the
 * passkey may be changed while open (a new one after a failed pairing). */
void hal_link_ble_setup(const char *name, uint32_t passkey);
typedef struct {
    bool open;              /* stack up: advertising or connected */
    bool connected;
    bool secure;            /* hal_link_ble_secure() */
    uint16_t mtu;           /* ATT MTU of the connection (23 until exchanged) */
    uint16_t pair_fails;    /* failed pairings since open */
    uint32_t connects;      /* connections since open */
    uint32_t heap_min;      /* minimum free heap in bytes since open (0 = unknown); kept after close */
} hal_ble_state_t;
void hal_link_ble_state(hal_ble_state_t *out);
/* BLE link encrypted with an authenticated (passkey) LE Secure Connections key and bonded: commands
 * other than hello allowed. */
bool hal_link_ble_secure(void);
/* Delete all BLE bonds (factory reset). */
void hal_link_ble_forget(void);

/* Raw console text on USB (log lines, already formatted with the "# " prefix). */
void hal_console_write(const char *s, size_t len);

/* Provided by the core (the HALs have a weak no-op): every console line the HAL prints itself (its own log
 * lines, e.g. `hal boot`, `http`), without the "# " prefix and the newline, from the thread that runs the core
 * only (the debug log, firmware.md "Debug log upload"). Lines of other tasks are not passed. */
void hal_log_hook(const char *line, size_t len);

#endif
