/*
 * tiny/store.h - flash layout, per-plugin double-buffered store, battery history,
 * RTC state. Spec: docs/spec/store.md. Implemented by the store component on top
 * of hal_fs_* / hal_kv_* / hal_rtc_mem; no dynamic allocation.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_STORE_H
#define TINY_STORE_H

#include "tiny/types.h"
#include "tiny/plugin_host.h"

#define TSTORE_ACTIVE_PATH "/plugin/active"
#define TSTORE_STAGE_DIR   "/stage" /* removed whenever it is empty */
#define TSTORE_STAGE_PKG   "/stage/pkg.tiny"
#define TSTORE_STAGE_META  "/stage/meta.cbor"
#define TSTORE_STAGE_INSTALL "/stage/install.cbor" /* an install deferred to the next boot (fresh heap) */
/* in the root directory since 2026-09-28: a directory costs a LittleFS metadata pair (8 KiB) on its own (store.md
 * "LittleFS paths"); a cold boot moves /hist/batt.bin here and drops /cache/poster.* (tstore_boot_cleanup) */
#define TSTORE_HIST_PATH   "/batt.bin"
#define TSTORE_ERRORS_PATH "/log/errors.cbor"

/* NVS battery checkpoint (store.md): version, cell, mV u16 LE, CRC-32 of bytes 0..3. */
#define TSTORE_BATT_KEY "battlast"
#define TSTORE_BATT_SIZE 8u
#define TSTORE_BATT_VERSION 1u

/* Extra slots for the desired state's `plugins` (remote.md "Schedule"): 'c'..'e'. */
#define TSTORE_EXTRA_FIRST 'c'
#define TSTORE_EXTRA_MAX 3
#define TSTORE_IS_EXTRA(s) ((s) >= TSTORE_EXTRA_FIRST && (s) < TSTORE_EXTRA_FIRST + TSTORE_EXTRA_MAX)

/* Path of a file in a slot: slot 'a', 'b' or an extra slot, name e.g. "pkg.tiny". Returns length or -1. */
int tstore_slot_path(char *out, size_t cap, char slot, const char *name);

/* Active slot ('a', 'b') or 0 when no plugin is installed. */
char tstore_active_slot(void);
int tstore_set_active(char slot);   /* atomic; 0 removes */

/* Whole small file into buf (length, copies only if <= cap), and atomic replace. */
int tstore_read_file(const char *path, uint8_t *buf, size_t cap);
int tstore_write_file(const char *path, const uint8_t *buf, size_t len);

/* Remove files left by an interrupted session or transfer (store.new). Call at boot. */
void tstore_boot_cleanup(void);

/* ------------------------------------------------------------------ store: read views */

#define TSTORE_LATEST UINT32_MAX

typedef struct {
    hal_file_t *f;
    char slot;
    uint32_t gen;               /* 0 = empty store */
    uint16_t store_version;
    /* small key index built at open, implementation-defined */
    uint32_t index[128];
    uint16_t index_count;
} tstore_view_t;

/* Open version `gen` of a slot's store (TSTORE_LATEST for the newest valid one).
 * TINY_E_NOTFOUND if that gen is not on flash (caller then uses latest + full refresh). */
int tstore_open(tstore_view_t *v, char slot, uint32_t gen);
int tstore_get(tstore_view_t *v, const char *key, size_t klen, uint8_t *out, size_t cap);
void tstore_close(tstore_view_t *v);

/* Iterate live entries in ascending key order (link store_get). `after` may be NULL.
 * Returns 1 and fills key/value, 0 at end, < 0 on error. */
int tstore_next(tstore_view_t *v, const char *after, size_t after_len,
                char *key, size_t key_cap, size_t *klen, uint8_t *val, size_t val_cap, size_t *vlen);

/* Adapter for tph_frame_t.store (get only). */
void tstore_view_adapter(tstore_view_t *v, tph_store_t *out);

/* ------------------------------------------------------------------ store: task sessions */

typedef struct {
    hal_file_t *f;              /* store.new */
    char slot;
    uint32_t gen, shown_gen;
    uint16_t store_version;
    uint32_t quota;             /* bytes */
    uint32_t live_bytes;
    long run_start;             /* file offset where the current task run began */
    bool dirty;
    uint32_t index[128];
    uint16_t index_count;
} tstore_session_t;

/* Start store.new from the latest version (compacted). store_version != the latest
 * file's version: the caller runs migrate_store (writes go to the new version). */
int tstore_session_begin(tstore_session_t *s, char slot, uint32_t shown_gen,
                         uint16_t store_version, uint32_t quota_bytes);
int tstore_run_begin(tstore_session_t *s);
int tstore_run_end(tstore_session_t *s, bool commit);   /* false: truncate the run */
int tstore_session_get(tstore_session_t *s, const char *key, size_t klen, uint8_t *out, size_t cap);
int tstore_session_put(tstore_session_t *s, const char *key, size_t klen, const uint8_t *val, size_t vlen);
int tstore_session_del(tstore_session_t *s, const char *key, size_t klen);
/* COMMIT + rename when dirty. *new_gen = the committed gen, or the unchanged latest. */
int tstore_session_end(tstore_session_t *s, uint32_t *new_gen);
void tstore_session_abort(tstore_session_t *s);

/* Adapter for tph_frame_t.store (get/put/del) during a task session. */
void tstore_session_adapter(tstore_session_t *s, tph_store_t *out);

/* Delete a slot's store (different plugin, failed migration). */
int tstore_clear(char slot);

/* ------------------------------------------------------------------ poster cache (store.md "Poster cache") */

#define TPCACHE_IDX "/poster.idx"
#define TPCACHE_DAT "/poster.dat"
#define TPCACHE_BUCKETS 2048u
#define TPCACHE_PROBES 8u
#define TPCACHE_DAT_MAX (96u * 1024u) /* poster.dat at most; a record past it starts a new generation */
#define TPCACHE_REC_MAX 294u          /* SCN_CACHE_REC_MAX */
#define TPCACHE_PENDING 2             /* records kept in RAM during a render phase (a tick lays out at most 2) */

/* Open for one render phase (outside plugin calls: the files are opened here). `salt`: the firmware version's
 * CRC-32; another salt, a bad header or unequal generations start the cache empty. */
typedef struct {
    hal_file_t *idx, *dat;
    uint32_t salt, gen, dat_len;
    uint8_t npend;
    uint16_t pend_len[TPCACHE_PENDING];
    uint64_t pend_key[TPCACHE_PENDING];
    uint8_t pend[TPCACHE_PENDING][TPCACHE_REC_MAX];
    uint32_t hits, misses, puts;
} tpcache_t;

int tpcache_open(tpcache_t *c, uint32_t salt);
/* Record of `key` into out: its length, or < 0 (absent, or a record that fails its CRC). Reads only. */
int tpcache_get(tpcache_t *c, uint64_t key, uint8_t *out, size_t cap);
/* Keeps a record in RAM (at most TPCACHE_PENDING a phase); written by tpcache_close. */
void tpcache_put(tpcache_t *c, uint64_t key, const uint8_t *rec, size_t len);
/* Appends the kept records, updates the index, closes. */
int tpcache_close(tpcache_t *c);
/* Adapter for tph_env_t.cache. */
void tpcache_adapter(tpcache_t *c, tph_cache_t *out);
/* Removes both files (factory reset, tests). */
void tpcache_clear(void);

/* ------------------------------------------------------------------ battery history */

typedef struct {
    uint32_t hour;
    uint16_t mv;
    uint8_t pct;
    uint8_t flags;
    uint16_t awake_s;
    uint8_t refreshes;
    uint8_t sessions;
    uint16_t radio_ds;
    uint8_t heap_min_kb;
    uint8_t energy_dmah;
} tiny_hist_rec_t;
_Static_assert(sizeof(tiny_hist_rec_t) == 16, "history record layout");

#define TINY_HIST_CHARGING 0x01
#define TINY_HIST_USB      0x02
#define TINY_HIST_SAFE     0x04
#define TINY_HIST_DEV      0x08

int thist_append(const tiny_hist_rec_t *r);
uint32_t thist_next_seq(void);
/* Copy records with seq >= from (oldest available if older); returns count, *first = seq of out[0]. */
int thist_read(uint32_t from, tiny_hist_rec_t *out, size_t max, uint32_t *first);

/* ------------------------------------------------------------------ RTC state */

#define TINY_RTC_MAGIC 0x53525054u /* "TPRS" */
#define TINY_BAND_CRC_MAX 16  /* render bands with a hash in RTC memory (OG: 6 at 1-bit, 12 at 2-bit) */
#define TINY_RTC_VERSION 1

enum {
    TINY_RTC_TIME_VALID = 1u << 0,
    TINY_RTC_OFFLINE = 1u << 1,
    TINY_RTC_SAFE_MODE = 1u << 2,
    TINY_RTC_PANEL_1BIT_VALID = 1u << 3,
    TINY_RTC_DEVELOPER = 1u << 4,
    TINY_RTC_RENDER_NOW = 1u << 5,  /* render (full refresh) at the next wake */
    TINY_RTC_PANEL_2BIT_VALID = 1u << 6, /* the panel shows the last frame, drawn at a gray depth (2 or 4: tiny_rtc_state_t depth), the hybrid base */
    TINY_RTC_SNTP_OK = 1u << 7,     /* the last SNTP attempt succeeded (host key sys.ok.time) */
    TINY_RTC_FW_APPLY = 1u << 8,   /* a firmware update waits in NVS `fwnext` (firmware.md) */
    TINY_RTC_CONFIG_MODE = 1u << 9, /* enter Bluetooth config mode at the next boot (BLE did not fit the used heap) */
    TINY_RTC_PORTAL = 1u << 10,     /* start the setup network at the next boot (switched from Bluetooth config mode) */
    TINY_RTC_UPDATE_NOW = 1u << 11, /* run an update session at the next cycle (firmware.md "Update now") */
    TINY_RTC_HEAP_RESTART = 1u << 12, /* restarted for a fresh heap after a plugin open ran out of memory; no
                                       * second such restart until an open succeeds */
    TINY_RTC_PLAYLIST_NEXT = 1u << 13, /* run a playlist switch at the next cycle (firmware.md "Playlist") */
    TINY_RTC_AUTO_TICK = 1u << 14,  /* an automatic switch ran just before a tick: the next cycle is that tick (also
                                     * after a restart for the install), the new plugin's frame its one refresh */
    TINY_RTC_GRAY_APPROX = 1u << 15, /* a hybrid partial showed changed gray pixels as their nearest black or white;
                                      * cleared by the next full refresh (link-protocol.md `gray_mode`) */
    TINY_RTC_PANEL_PALETTE = 1u << 16, /* the panel shows a palette frame (assets.md "Palette frames"): a frame of the
                                        * other kind is drawn fully */
    TINY_RTC_SYNC_SCREEN = 1u << 17, /* a remote hold 5 s: the next cycle syncs as for the press, then the sync screen */
    TINY_RTC_WALL_SYNC = 1u << 18,  /* a sync press in a wall: the next check-in says `wall_sync` (remote.md "Walls"),
                                     * cleared by its 200 */
    TINY_RTC_CMDS = 1u << 20,       /* remote commands queued, running or not reported (NVS `cmds`, remote.md "Commands") */
    /* the rules arbiter (firmware.md "Rules", NVS `rules`): the record has rules; a result is true or a rule holds
     * the screen (decide every cycle); a rule holds the screen; a check-in answer came (the winner may take it); the
     * playlist switch queued for the next cycle is a rule's (its `pl_next why=rule`) */
    TINY_RTC_RULES = 1u << 21,
    TINY_RTC_RULE_LIVE = 1u << 22,
    TINY_RTC_RULE_HOLD = 1u << 23,
    TINY_RTC_RULE_FRESH = 1u << 24,
    TINY_RTC_RULE_SWITCH = 1u << 25,
    TINY_RTC_RULE_ANSWER = 1u << 26, /* the held entry's press task was accepted: the check-in asked for it decides
                                      * ("Rules" Answer), without a verified answer the hand-back is local */
};

/* tiny_rtc_state_t.screen: what the panel shows */
enum {
    TINY_SCREEN_NONE = 0,     /* unknown (cold boot) */
    TINY_SCREEN_PLUGIN = 1,
    TINY_SCREEN_NOWIFI = 2,
    TINY_SCREEN_NOBATT = 3,
    TINY_SCREEN_SETUP = 4,
    TINY_SCREEN_SAFE = 5,     /* built-in safe-mode clock */
    TINY_SCREEN_BLE = 6,      /* Bluetooth config mode: name and passkey */
    TINY_SCREEN_SLEEP = 7,    /* remote schedule sleep period: "sleeping" (remote.md "Schedule") */
    TINY_SCREEN_BLANK = 8,    /* ... sleep period, screen "blank" */
    TINY_SCREEN_REVOKED = 9,  /* the plugin was revoked and removed: clock + "plugin revoked" (remote.md "Revocation") */
    TINY_SCREEN_PORTAL = 10,  /* setup network: join QR, portal QR, network name and password (docs/spec/setup.md) */
    TINY_SCREEN_CLAIM = 11,   /* remote management: claim QR until the server sends a state (remote.md) */
    TINY_SCREEN_NOPLUGIN = 12, /* no plugin installed: clock, device name, "open the web app, pick a plugin" */
    TINY_SCREEN_UPDATE = 13,  /* update now: "updating", then the result (firmware.md "Update now") */
    TINY_SCREEN_NEEDS_UPDATE = 14, /* the plugin needs a renderer this firmware lacks: clock + "plugin needs an update" (firmware.md "Renderers") */
    TINY_SCREEN_LOADING = 15, /* playlist switch: "Loading <name>...", then a failure (firmware.md "Playlist") */
    TINY_SCREEN_STORAGE = 16, /* the storage did not mount: "storage problem", nothing erased (firmware.md "Storage recovery") */
    TINY_SCREEN_NOTIME = 17,  /* no valid time over a plugin that depends on it: "No time yet", waits for WiFi (firmware.md "WiFi" No time) */
    TINY_SCREEN_LAYOUT = 18,  /* the installed layout, or the firmware's screen without one (widgets.md "Screens"; TRMNL X) */
    TINY_SCREEN_NOTIFY = 20,  /* a notification until answered or expired (remote.md "Notifications") */
    TINY_SCREEN_MENU = 21,    /* retired 2026-09-28 (the menu) */
    TINY_SCREEN_HELP = 22,    /* Button help: what each gesture does now (firmware.md "Button help") */
    TINY_SCREEN_INFO = 23,    /* the sync screen: device info, the result, the shortcuts (firmware.md "Button help") */
    TINY_SCREEN_RESET = 24,   /* retired 2026-09-28 (the factory reset screen) */
};

typedef struct {
    uint32_t next_due_min;      /* unix minutes, 0 = run at next session */
    uint8_t backoff;            /* 0 none, 1..3 = 5/15/60 min step */
    uint8_t ok;                 /* 1: the last run returned ok (host key sys.ok.<task>) */
    uint8_t retry_left;         /* task `retry` (manifest.md "Tasks" Retry): grid points left, 0 = no window; <= 97 */
    uint8_t retry_flags;        /* TINY_RTC_RETRY_STALE: the last ok run was stale */
} tiny_rtc_task_t;
#define TINY_RTC_RETRY_STALE 0x01

/* A tap target in RTC (tiny_rtc_state_t tap): logical px, v its action or intent */
#define TINY_TAP_MAX 8
typedef struct {
    uint16_t x, y, w, h;
    uint8_t v;
    uint8_t reserved;
} tiny_rtc_tap_t;

/* A press mark rectangle in RTC (tiny_rtc_state_t ovl_rect): native px, x and w whole bytes; w 0 = none */
#define TINY_OVL_RECTS 4
typedef struct {
    uint16_t x, y, w, h;
} tiny_rtc_rect_t;

/* The timekeeper's fields (tiny_rtc_state_t) once more with their own CRC, written by trtc_commit: a reset in the
 * middle of a wake cycle (USB, watchdog) leaves the state's CRC stale, trtc_get then keeps the time and the drift
 * from here (store.md "RTC state"). */
typedef struct {
    int64_t sync_unix_ms;
    uint64_t sync_rtc_us;
    int64_t cal_unix_ms;
    uint64_t cal_rtc_us;
    int32_t drift_ppb;
    uint32_t err_bound_ms;
    uint32_t drift_unc_ppb;
    uint32_t cal_err_ms;
    uint16_t sync_count;
    uint16_t drift_acc;
    uint32_t time_valid;        /* TINY_RTC_TIME_VALID of flags */
    uint32_t crc32;             /* over the bytes before this field */
    uint32_t reserved;
} tiny_rtc_time_t;

/* The WiFi join hints (tiny_rtc_state_t bssid..dns) once more with their own CRC, written by trtc_commit: a reset
 * between two commits keeps them like the time, so the next join needs no scan and no DHCP (store.md "RTC state"). */
typedef struct {
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi_last;
    uint32_t ip, gw, mask, dns;
    uint32_t crc32;             /* over the bytes before this field */
} tiny_rtc_net_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t crc32;             /* over the bytes after this field */
    /* time */
    int64_t sync_unix_ms;
    uint64_t sync_rtc_us;
    int32_t drift_ppb;          /* + = RTC runs fast */
    uint32_t err_bound_ms;      /* error bound at the last sync */
    uint32_t drift_unc_ppb;     /* uncertainty of drift_ppb: growth rate of the bound */
    uint16_t sync_count;        /* syncs since cold boot (saturating) */
    uint16_t drift_acc;         /* observed drift change rate, 10 ppb per hour (quadratic bound term) */
    uint64_t next_wake_rtc_us;
    uint32_t flags;             /* TINY_RTC_* */
    /* display: inputs of the displayed frame */
    int64_t shown_unix;
    uint32_t shown_store_gen;
    uint32_t shown_settings_crc;
    int8_t shown_batt_pct;
    uint8_t shown_charging;
    int8_t shown_rssi;
    uint8_t depth;
    uint16_t partials_since_full;
    uint8_t screen;             /* TINY_SCREEN_* */
    uint8_t rotation;
    /* plugin */
    char plugin_slot;           /* 'a', 'b' or 0 */
    uint8_t fail_count;
    uint32_t plugin_hash32;
    uint16_t renders_ok;
    uint16_t wifi_fails;        /* consecutive failed WiFi connects (retry backoff) */
    /* WiFi fast connect */
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi_last;           /* dBm of the last session, 0 unknown (frame input) */
    uint32_t ip, gw, mask, dns;
    /* scheduler */
    tiny_rtc_task_t task[TPH_MAX_TASKS];
    uint32_t checkin_next_min;
    uint32_t sntp_next_min;
    uint32_t session_pending;   /* bit i: task i due at the next session; bit 16 + i: task i by a press (`run: action`);
                                 * bit 31: check-in */
    uint32_t render_next_min;   /* update.custom: unix minute of the next render, 0 = now */
    /* energy, current hour */
    uint32_t hour;
    uint32_t awake_ms, radio_ms, bytes_fetched, dirty_px, energy_uah;
    uint16_t refreshes;
    uint8_t sessions;           /* saturating (the record holds a u8) */
    uint8_t hist_flags;         /* TINY_HIST_* seen during the hour (ORed into its record) */
    uint16_t batt_mv;
    uint16_t shown_sync;        /* TPH_SYNC_* of the displayed frame (host keys) */
    uint32_t heap_min_render, heap_min_task;
    /* remote schedule (remote.md "Schedule") */
    uint32_t sched_key;         /* CRC-32 of the schedule, TZ and failed marks; 0 = recompute */
    uint32_t sched_from_min, sched_until_min;  /* the cached answer holds for [from, until) */
    uint32_t sched_peek_until_min;             /* button during a sleep period: the face until then */
    uint8_t sched_rule;         /* rule index + 1, 0 = the default plugin */
    uint8_t sched_fails;        /* extra plugin failures in a row */
    char sched_task_slot;       /* slot whose task state is in task[] (0 = unknown) */
    uint8_t sched_reserved;
    uint32_t full_min;          /* unix minute of the frame of the last full refresh (manifest.md "Full refresh") */
    uint32_t claim_since_min;   /* unix minute claim polling (re)started: registration or a press (0 = none, remote.md) */
    /* band hashes of the displayed frame (store.md): CRC-32 of each render band as drawn; a tick draws the previous
     * frame only for bands whose new CRC differs. Valid while band_n > 0 and the geometry matches. */
    uint32_t band_crc[TINY_BAND_CRC_MAX];
    uint16_t band_rows;
    uint8_t band_n;
    uint8_t band_depth;
    uint16_t wake_lead_ms;      /* wake this long before a tick: the last ticks' boot + render time (0 = default) */
    uint16_t dlog_retry_min16;  /* debug log: no WiFi session only for it before this unix minute (low 16 bits, 0 = none) */
    /* last time sync, for the report's `sync` and link status `sync_src` (remote.md "Time") */
    int32_t sync_off_ms;        /* source time - our estimate at that sync (0: no previous estimate) */
    uint16_t sync_rtt_ms;       /* request round trip (server, SNTP; 0: link, wallclock) */
    uint8_t sync_src;           /* TINY_SYNC_* */
    uint8_t sync_reserved;
    uint16_t late_max_ms;       /* worst tick lateness since the last report (saturating) */
    uint16_t act_until16;       /* a timed action's window end (TINY_ACT_TIMED): unix seconds, low 16 bits */
    /* button actions (firmware.md "Buttons"): the displayed plugin's offered actions as the last open saw them
     * (TINY_ACT_INFO), and a pressed one (TINY_ACT_*: its number for the next frame, TINY_ACT_RUN until counted after
     * the open) */
    uint16_t act_info;
    uint8_t act_pending;
    uint8_t act_window;         /* the action (1..6) whose timed window is on (TINY_ACT_TIMED), until act_until16 */
    /* config schedule (firmware.md "Schedule"); NVS plsch holds the edge for a cold boot */
    uint32_t sched_rot_min;     /* unix minute of the last rotation (0 = none) */
    uint32_t sched_edge_until;  /* the period acted on: its `until` (0 = unknown: read NVS plsch) */
    int8_t sched_edge_rule;     /* ... and its rule index (-1 = none) */
    uint8_t sched_owed;         /* 1: a rotation is owed (its target was not ready): it runs once it is */
    uint16_t sched_edge_key16;  /* ... and the low 16 bits of its schedule key (rules, entries, TZ) */
    uint32_t pl_prefetch_retry_min; /* no prefetch session before this unix minute (after a failed one; 0 = none) */
    /* a plugin notification (manifest.md "Notifications"): on screen until ntf_until (unix s, 0 = none), for the
     * plugin in slot ntf_slot; its texts are in that plugin's store */
    uint32_t ntf_until;
    char ntf_slot;
    uint8_t ntf_reserved[3];
    /* Button help and the sync screen (firmware.md "Button help"): menu = TINY_SCREEN_HELP/INFO on screen until
     * menu_until_rtc (0 = none), over the screen menu_base */
    uint8_t menu, menu_base;
    uint8_t help_n;             /* the plugin under it as its open saw it (the panel is checked against a render */
    uint8_t help_show_s[3];     /* without the plugin open): its offered actions (help_n), their show_s, bit k */
    uint8_t help_toggle;        /* toggle, name (cut), resolved labels */
    uint64_t menu_until_rtc;
    char help_name[33];
    char help_label[3][25];
    /* walls (firmware.md "Walls"): a prefetched photo shows at the tick of this unix minute (0 = none); the refresh
     * starts early by the board's last full [0] or partial [1] refresh (ms), so that it ends on the minute */
    uint32_t wall_show_min;
    uint16_t refresh_ms[2];
    /* WiFi state (firmware.md "WiFi"): the last session that connected, the first failed one of the failures in a row
     * (wifi_fails), the next attempt (unix minutes, 0 = unknown / none) and the backoff step (a manual sync resets it) */
    uint32_t wifi_ok_min, wifi_fail_min, wifi_next_min;
    uint8_t wifi_step;
    /* the plugin in slot pl_nu_slot and the network: meta `nu` (store_install.h tstore_pl_meta_t), kept at its park */
    char pl_nu_slot;
    uint8_t task_back; /* tasks whose next run steps back: an action with role `previous` (manifest.md "Actions") */
    uint8_t task_renew; /* tasks whose next run makes its cached list again: a sync (manifest.md "Cached lists" Sync) */
    uint32_t pl_nu;
    /* timekeeper (firmware.md "Clock: drift calibration"): the start of the drift measurement in progress, a sync
     * that measured it (0 = the last sync, older states); syncs too imprecise to tell the learned drift anything
     * extend it rather than restart it */
    int64_t cal_unix_ms;
    uint64_t cal_rtc_us;
    uint32_t cal_err_ms;
    uint32_t ring_fill_min;     /* unix minute of the shown frame a photo ring fill settled for (app_ring_fill; 0 none) */
    /* touch (firmware.md "Touch"): the tap targets on the panel (tap_screen TINY_SCREEN_PLUGIN: the frame's scene `tap`
     * targets, v = the action number; TINY_SCREEN_HELP: Button help's rows, v = the intent), logical px; the offered
     * role actions and the actions a target may run (app_act_note) */
    uint8_t tap_n, tap_screen;
    uint8_t act_roles;          /* boards with roles: bits 0..2 the offered `next` action's number, bits 4..6 `previous` */
    uint8_t act_avail;          /* bit n-1: a tap target may run action n now (label and `when` hold) */
    tiny_rtc_tap_t tap[TINY_TAP_MAX];
    char help_role[2][25];      /* the resolved labels of the offered role actions (act_roles), for Button help */
    uint64_t touch_until_rtc;   /* the touch window is open until then (hal_rtc_us; 0 none): cycles in between keep it */
    /* image relay (image-fetch.md "Relay" Backoff): failed relay answers in a row; no relay before this unix minute */
    uint32_t relay_off_min;
    uint8_t relay_fails;
    uint8_t relay_reserved[3];
    tiny_rtc_time_t time_copy;  /* the time again (trtc_commit), for a reset before the next commit */
    /* press mark (firmware.md "Press mark"), after time_copy: an older image's state (app_fwup.c rtc_restore) leaves
     * these 0. The mark as drawn (ovl_rect[0], native px: the result frame's partial drives it away); a press taken
     * while busy (ovl_queued: its intent, in_action_t, 0 = none, and its press count); ovl_badge_w: the width in logical px of
     * the black pill at the bottom left of the shown frame (0 none), which the mark covers. ovl_until_rtc: the options list's end (the
     * layout stays: the press overlay is removed; ovl_badge_w takes the bytes of the old ovl_status and ovl_flags) */
    uint64_t ovl_until_rtc;
    tiny_rtc_rect_t ovl_rect[TINY_OVL_RECTS];
    struct {
        uint8_t intent, count;
    } ovl_queued;
    uint16_t ovl_badge_w;
    tiny_rtc_net_t net_copy;    /* the WiFi hints again (trtc_commit), after the mark: an older image leaves it 0 */
    /* crash backoff (firmware.md "Safe mode" Quiet crash), after net_copy: no WiFi step (session, playlist switch,
     * install) before this unix minute after a reset in one (0 none; an older image leaves it 0) */
    uint32_t crash_net_min;
    uint32_t crash_render_min; /* no tick render before this unix minute after a reset in a render (0 none) */
    /* at the end (the state is append-only: store/test/test_rtc_layout.c; an older image leaves them 0): the
     * options list (firmware.md "Options list", options.h opt_t): entries shown (0 none; bit 7: pinned by a rule),
     * the highlighted one, the plugin slot it was opened on; its 30 s end in ovl_until_rtc (0: from the next sleep) */
    uint8_t opt_n, opt_sel;
    char opt_slot;
    /* resets in WiFi steps since the last session that ended without one (the crash_net_min hold grows with it;
     * the crash-loop count clears on every completed cycle, this does not: issues #2, #128) */
    uint8_t crash_net_n;
} tiny_rtc_state_t;
/* act_info: bit 15 known; the offered actions k = 0..2 (the press count - 1): bits 3k..3k+2 its number (1..6, 0 =
 * none), bits 9+2k..10+2k its kind (TINY_ACT_K_*) */
#define TINY_ACT_INFO_KNOWN 0x8000u
enum { TINY_ACT_K_PLAIN, TINY_ACT_K_FAM_NEXT, TINY_ACT_K_FAM_PREV }; /* plain: also a task */
#define TINY_ACT_OFFER_N(info, k) ((int)(((info) >> (3 * (k))) & 7u))
#define TINY_ACT_OFFER_KIND(info, k) ((int)(((info) >> (9 + 2 * (k))) & 3u))
#define TINY_ACT_OFFER(k, n, kind) ((uint16_t)(((unsigned)(n) << (3 * (k))) | ((unsigned)(kind) << (9 + 2 * (k)))))
/* act_pending: bits 0..2 the frame's action number (1..6): the press's, then the timed window's (act_window) */
#define TINY_ACT_N_MASK 0x07u
#define TINY_ACT_RUN 0x08u
#define TINY_ACT_TIMED 0x10u /* a timed action (manifest show_s) is on until act_until16 */
#define TINY_ACT_WAS_ON 0x20u /* with TINY_ACT_RUN: pressed while its own timed window was on (manifest toggle) */

/* tiny_rtc_state_t.sync_src */
enum { TINY_SYNC_NONE = 0, TINY_SYNC_SNTP = 1, TINY_SYNC_SERVER = 2, TINY_SYNC_LINK = 3, TINY_SYNC_WALLCLOCK = 4 };

_Static_assert(sizeof(tiny_rtc_state_t) <= 1024, "RTC state must fit 1024 bytes");

/* Pointer into RTC memory; validated on first call (bad magic/version/size/CRC: zeroed,
 * header set, flags = 0; the time and drift from time_copy when its CRC holds and the RTC counter did not go back). */
tiny_rtc_state_t *trtc_get(void);
/* What the first trtc_get found: the state, nothing, or only the time (a reset between commits). */
enum { TRTC_KEPT = 0, TRTC_NEW = 1, TRTC_TIME_KEPT = 2 };
int trtc_origin(void);
/* Recompute the CRC. Call before any sleep or reboot. */
void trtc_commit(void);

#endif
