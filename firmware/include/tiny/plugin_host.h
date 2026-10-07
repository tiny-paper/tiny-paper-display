/*
 * tiny/plugin_host.h - loads .tiny packages, runs them in the scene machine (scene plugins, tiny/scene.h),
 * implements the tiny1 host API. WebAssembly plugins (WAMR) are gone, no plugin uses them. Specs:
 * docs/spec/tiny-format.md, manifest.md, assets.md, host-abi.md.
 *
 * Owned by the plugin_host component. It knows nothing about files, schedules or
 * the panel: the caller (core) passes a package reader, the inputs and a store
 * adapter, and gets frames and task results back. HTTP goes through hal_http().
 *
 * Lifecycle per phase (fresh instance each time, plan 3.9):
 *   tph_open -> tph_begin(phase) -> tph_render / tph_next_update / tph_run_task ... -> tph_end -> tph_close
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_PLUGIN_HOST_H
#define TINY_PLUGIN_HOST_H

#include <stddef.h>
#include <string.h>

#include "tiny/album.h"
#include "tiny/types.h"
#include "tiny/hal.h"
#include "tiny/gfx.h"

typedef enum {
    TPH_OK = 0,
    TPH_E_FORMAT = -100,   /* container: header, CRC, TOC (tiny-format.md) */
    TPH_E_MANIFEST = -101, /* MANI or SETS invalid (manifest.md) */
    TPH_E_ASSETS = -102,   /* ASET invalid (assets.md) */
    TPH_E_MODULE = -103,   /* scene invalid (scene.md) */
    TPH_E_API = -104,      /* requires.api / requires.firmware not satisfied */
    TPH_E_DISPLAY = -105,  /* no manifest display matches the screen and depth */
    TPH_E_LIMITS = -106,   /* caps above board limits or phase budget, too big */
    TPH_E_NOMEM = -107,
    TPH_E_TRAP = -108,     /* scene trap (scene.md) */
    TPH_E_BUDGET = -109,   /* instruction or time budget exceeded */
    TPH_E_STATE = -110,    /* call not valid now (wrong phase, no such task) */
    TPH_E_IO = -111,       /* reader failed */
    TPH_E_RENDERER = -112, /* manifest renderer not scene (a code plugin: code plugins are no longer supported) */
} tph_err_t;

/* The renderers this firmware runs on `board` (TINY_RENDERER_*): scene only, WebAssembly plugins (WAMR) are gone
 * (docs/spec/firmware.md "Renderers"). */
uint8_t tph_renderers(const hal_board_t *board);

/* Random-access package bytes (file on flash, or memory in tests). */
typedef struct {
    int (*read)(void *ctx, uint32_t off, void *buf, size_t len); /* 0 = ok */
    void *ctx;
    uint32_t size;
    const uint8_t *mem;   /* optional: whole package mapped in memory */
} tph_reader_t;

/* Parsed manifest, the parts the core needs (scheduler, UI, install). Strings are
 * NUL-terminated copies. */
#define TPH_IMAGE_URL_SCENE_MINOR 18 /* image-fetch.md Fixed endpoints; no version bump */

#define TPH_MAX_TASKS 8
#define TPH_MAX_HOSTS 8
#define TPH_HOST_BUF 512 /* all host names of one task, NULs included */
#define TPH_MANI_MAX 16384 /* whole MANI section (manifest.md, tiny-format.md) */
#define TPH_TASK_MAP_MAX 8192 /* one task's map in MANI before scene 1.14 (then MANI bounds it; a run reads it at its own size) */

enum { TPH_ON_ERROR_KEEP = 0, TPH_ON_ERROR_CLEAR = 1 };

/* manifest `update` (manifest.md "Render schedule"): the time grid. ON_DATA and AT have none (renders on data and at
 * the `at` times only); `at` and `on_data` combine with EVERY and CRON too (tph_info_t update_at, update_on_data). */
typedef enum { TPH_UPDATE_EVERY, TPH_UPDATE_CRON, TPH_UPDATE_ON_DATA, TPH_UPDATE_CUSTOM, TPH_UPDATE_AT } tph_update_kind_t;
#define TPH_UPDATE_AT_MAX 8

typedef struct {
    char name[17];
    uint32_t every_min;        /* 0 when cron */
    char cron[65];             /* "" when every */
    uint16_t flex_min;          /* <= 7 d; uint16 keeps the struct at its size with map_off, map_len (RAM, heap model) */
    bool net;
    bool decl;                 /* declarative (manifest get + store): runs in the host, no instance */
    bool image;                /* declarative with `image` (scene 1.4, manifest.md "Image tasks") */
    bool photo;                /* a steps task with an image step (scene 1.8): decoded as assets.md "Photos" */
    bool ics;                  /* declarative with `ics` (scene 1.8, manifest.md "ICS tasks") */
    uint8_t on_error;          /* declarative: TPH_ON_ERROR_* */
    uint8_t host_count;
    uint8_t format;            /* declarative with `format` (scene 1.8, manifest.md "Formats"): FF_* + 1; 0 = JSON */
    uint8_t steps;             /* declarative with `steps` or `variants` (scene 1.8, manifest.md "Steps") */
    bool optional;             /* manifest `role: optional`: the plugin works without its data (manifest.md "Tasks") */
    bool by_action;            /* manifest `run: action` (scene 1.12): runs only when an action names it, never scheduled */
    uint8_t server_len;        /* `get` starts with {settings.<key>} (manifest.md "Server from a setting"): the key's
                                * length, the key at host_buf + server_off (after the hosts); 0 = none */
    uint16_t server_off;
    uint16_t map_off, map_len; /* the task's CBOR map within MANI (a run reads only that) */
    uint16_t valid_min;        /* its data stays usable this long after the last ok run: `valid`, else 3 x the
                                * interval within 1 h..24 h (manifest.md "Tasks") */
    uint16_t retry_every_min;  /* task `retry` (manifest.md "Tasks" Retry): 0 = none, else 15..360 */
    uint16_t retry_for_min;    /* every..1440 */
    bool retry_on_fail;
    /* Host names, NUL-terminated, packed into host_buf (the whole list <= TPH_HOST_BUF bytes with
     * the NULs); read them with tph_task_host(). Fixed [8][254] slots made tph_info_t 16.6 KB, too
     * big for the C3's task stack (2026-09-24). */
    uint16_t host_off[TPH_MAX_HOSTS];
    char host_buf[TPH_HOST_BUF];
} tph_task_info_t;

static inline const char *tph_task_host(const tph_task_info_t *t, int i) { return t->host_buf + t->host_off[i]; }

/* Append a host (not NUL-terminated, length n) to a task. Returns false when the list is full. */
static inline bool tph_task_host_add(tph_task_info_t *t, const char *h, size_t n)
{
    size_t off = t->host_count ? t->host_off[t->host_count - 1] + strlen(tph_task_host(t, t->host_count - 1)) + 1 : 0;
    if (t->host_count >= TPH_MAX_HOSTS || off + n + 1 > TPH_HOST_BUF)
        return false;
    memcpy(t->host_buf + off, h, n);
    t->host_buf[off + n] = 0;
    t->host_off[t->host_count++] = (uint16_t)off;
    return true;
}

/* The setting key of a task's user server (manifest.md "Server from a setting"), NULL = none. */
static inline const char *tph_task_server(const tph_task_info_t *t)
{
    return t->server_len ? t->host_buf + t->server_off : NULL;
}

/* manifest.md "Tasks" `every` floor: TPH_EVERY_MIN minutes for every task, whatever its host, in the manifest and as
 * a user override (issue #146; a cron's shortest gap is a minute by its grammar). Before scene 1.19 it was 15 for
 * a task without a server from a setting: the validators ask requires.scene >=1.19 of such a package. */
#define TPH_EVERY_MIN 1
/* A `get` starting with `{settings.<key>}`: the key, NUL-terminated (cap >= 33). False for any other `get`. */
bool tph_get_server_key(const char *get, size_t n, char *key, size_t cap);

/* Manifest `actions` (manifest.md "Actions", scene 1.5; more than 2, `when` and labels over settings: scene 1.11):
 * action n is actions[n - 1]. At most TPH_OFFER_MAX are offered at a time (the press count picks one). */
#define TPH_MAX_ACTIONS 6
#define TPH_OFFER_MAX 3
enum { TPH_ACT_PLAIN = 0, TPH_ACT_TASK = 1, TPH_ACT_FAMILY_NEXT = 2, TPH_ACT_FAMILY_PREV = 3 };
/* `when` (manifest.md "Actions"): offered only while the action `when_ref` (an index) has its timed window on (OPEN),
 * off (CLOSED), or its presses mod `when_of` are `when_is` (PAGE) */
enum { TPH_WHEN_NONE, TPH_WHEN_OPEN, TPH_WHEN_CLOSED, TPH_WHEN_PAGE };
/* `role` (manifest.md "Actions"): the plugin's next or previous page; boards with keys or swipes for it run it there */
enum { TPH_ROLE_NONE, TPH_ROLE_NEXT, TPH_ROLE_PREV };
typedef struct {
    char id[17], label[25];    /* label: text, may hold {settings.<key>} (offered only when it is not empty) */
    uint8_t kind;              /* TPH_ACT_* */
    uint8_t task;              /* TPH_ACT_TASK: index into tph_info_t.tasks */
    uint8_t show_s;            /* a timed action: seconds it stays on (5..120), 0 = one frame (scene 1.6) */
    bool toggle;               /* with show_s: a press inside its window ends it (manifest.md "Actions") */
    uint8_t when, when_ref, when_of, when_is; /* TPH_WHEN_* */
    uint8_t role;              /* TPH_ROLE_* */
} tph_action_t;

typedef struct {
    char id[65], name[49], version[33];
    char family[65];           /* manifest family.id, "" = none */
    uint8_t action_count;
    tph_action_t actions[TPH_MAX_ACTIONS];
    uint8_t api_major, api_minor;
    uint8_t scene_minor;       /* requires.scene minor (scene plugins) */
    uint16_t store_version;
    uint16_t store_kb, memory_kb, task_memory_kb;
    tph_update_kind_t update;
    uint32_t update_every_min;
    union { /* by `update` (one buffer: tph_info is on the C3 heap) */
        char update_cron[65];    /* TPH_UPDATE_CRON: the cron string */
        char update_setting[65]; /* TPH_UPDATE_EVERY: manifest update_setting, a settings key ("" none) whose
                                  * duration replaces update_every_min */
    };
    uint8_t display_index;     /* chosen for the screen given to tph_check/tph_open */
    bool has_settings;         /* SETS present */
    bool scene;                /* renderer scene (scene.md): SCEN, no module; memory_kb and task_memory_kb are 0 */
    uint16_t scene_arena;      /* SCEN arena bytes */
    bool scene_poster;         /* SCEN calls tposter: the render phase takes a poster cache (tph_env_t.cache) */
    bool scene_next;           /* SCEN calls next (scene 1.8): tph_next_update answers the time the last frame asked */
    uint8_t wall_mode;         /* manifest `wall` (firmware.md "Walls"): TPH_WALL_* */
    bool has_migrate_store;
    uint8_t layer_count;       /* manifest layers (API 1.1), layer(id) for id < layer_count */
    bool task_entry;           /* manifest task_entry (scene 1.16): SCEN's last entry is task code (manifest.md "Task programs") */
    uint16_t full_refresh;     /* manifest full_refresh: TPH_FULL_* or minutes 1..1440 */
    bool update_on_data;       /* update.on_data: a render after a task session that committed a store change */
    bool offline_never;        /* manifest `offline: never`: paused whenever the device is offline (manifest.md) */
    uint8_t update_at_count;   /* update.at (scene 1.8): local times, minutes of the day */
    uint16_t update_at[TPH_UPDATE_AT_MAX];
    /* manifest notify (manifest.md "Notifications", scene 1.8): the store keys of title and text ("" = no notify) */
    char notify_title[33], notify_text[33];
    bool notify_yes_no;
    uint16_t notify_expires_min;
    /* manifest `strings` (manifest.md "Strings"): the map's bytes in the MANI section, 0 = none */
    uint16_t strings_off, strings_len;
    uint8_t task_count;
    /* Last: tph_info() of an open plugin is allocated only up to tasks[task_count] (RAM on the
     * C3), so never copy that one by value. */
    tph_task_info_t tasks[TPH_MAX_TASKS];
} tph_info_t;

/* Full-refresh cadence (manifest.md "Full refresh"): manifest `full_refresh`, settings `_full_refresh`. */
enum { TPH_FULL_UNSET = 0, TPH_FULL_ALWAYS = 0xfffe, TPH_FULL_NEVER = 0xffff }; /* else minutes 1..1440 */
#define TPH_FULL_REFRESH_KEY "_full_refresh"
/* "always", "never" or a duration 1m..1d -> *out. TPH_OK or TPH_E_MANIFEST. */
int tph_full_refresh_parse(const char *s, size_t n, uint16_t *out);
/* The `_full_refresh` of a settings document (CBOR map), TPH_FULL_UNSET when absent or invalid. */
uint16_t tph_full_refresh_setting(const uint8_t *settings, size_t len);

/* Manifest `update` (the map at m, n bytes) into info->update*, `notify` into info->notify_*. `scene`: custom refused.
 * TPH_OK or TPH_E_MANIFEST. Own file (tph_hints.c) so the simulator's fake plugin host links it too. */
int tph_update_parse(const uint8_t *m, size_t n, bool scene, tph_info_t *info);
int tph_notify_parse(const uint8_t *m, size_t n, tph_info_t *info);
/* Host standard setting `_notify` (bool): false only when the document sets it false (the user turned the plugin's
 * notifications off). */
#define TPH_NOTIFY_KEY "_notify"
bool tph_notify_setting(const uint8_t *settings, size_t len);
/* Host key sys.notify (host-abi.md "Host keys"): the answer to the plugin's last notification, stored by the host
 * (tstr ok, yes, no, expired); sys.ntf: the host's notification state ([crc32, rate limit time]), never readable. */
#define TPH_NOTIFY_ANSWER_KEY "sys.notify"
#define TPH_NOTIFY_STATE_KEY "sys.ntf"

/* Bytes of a tph_info_t that hold its tasks and everything before them. */
#define TPH_INFO_SIZE(task_count) (offsetof(tph_info_t, tasks) + (size_t)(task_count) * sizeof(tph_task_info_t))

/* Manifest `wall` (manifest.md, firmware.md "Walls"). */
enum { TPH_WALL_NONE = 0, TPH_WALL_SPAN = 1, TPH_WALL_TILES = 2 };
#define TPH_WALL_MAX 200 /* wall members (remote.md "Walls") */

/* The screen the plugin will draw on. */
typedef struct {
    uint16_t panel_w, panel_h;
    uint8_t depth, rotation;   /* in a wall: the wall rotation */
    /* A wall (remote.md "Walls"): canvas_w 0 = none. Every plugin draws on the canvas and the panel shows
     * the view at (view_x, view_y); picks follow wall_seed in every mode. */
    uint16_t canvas_w, canvas_h, view_x, view_y;
    uint32_t wall_seed;
    uint8_t wall_n, wall_index; /* members (<= TPH_WALL_MAX) and this one (scene IN 26, 27); rects: tph_env_t wall_rect */
    /* A mashup box (docs/spec/scene.md "Mashups", MASHUP_SCENE_MINOR): box_w 0 = none, the whole screen. The plugin sees the
     * box as its screen and draws at its origin (box_x, box_y), clipped to it; the host clears and decodes only there.
     * Ignored when a wall is set (a mashup is never on a wall). */
    uint16_t box_x, box_y, box_w, box_h;
} tph_screen_t;

/* Full install checks (tiny-format.md reader checks, manifest, SETS, ASET,
 * module validation, board limits and phase budgets). Does not instantiate.
 * `err` receives a short reason for reports and link error messages. */
int tph_check(const tph_reader_t *pkg, const hal_board_t *board, const tph_screen_t *scr,
              const char *fw_version, tph_info_t *info, char *err, size_t err_cap);

/* Settings: validate `in` (CBOR map, may be NULL) against the package's SETS and write
 * the complete document (defaults filled, unknown keys dropped, renamed_from applied, a valid host
 * standard setting such as `_full_refresh` kept, an invalid one dropped and reported) to `out`. Keys that fell back to defaults are written as a CBOR array of tstr to
 * `invalid` (may be NULL). Lengths returned through *out_len / *invalid_len. */
int tph_settings_validate(const tph_reader_t *pkg, const uint8_t *in, size_t in_len,
                          uint8_t *out, size_t out_cap, size_t *out_len,
                          uint8_t *invalid, size_t invalid_cap, size_t *invalid_len);

/* Secret hosts (manifest.md "Secret hosts"). The allowed hosts of secret `key` from a MANI and a SETS map (CBOR):
 * NUL-terminated names one after another, the markers TPH_SECRET_USER (the user's server) and TPH_SECRET_URL (the
 * secret is the URL: its own host and a parent domain of it; "(url)" until issue #86, the address itself only) included; length in *len. TINY_E_NOTFOUND: no such secret field; TINY_E_LIMIT: over cap. */
#define TPH_SECRET_USER "(user)"
#define TPH_SECRET_URL "(url+parent)"
int tph_secret_hosts(const uint8_t *mani, size_t mani_len, const uint8_t *sets, size_t sets_len, const char *key,
                     char *out, size_t cap, size_t *len);
/* The upgrade rule: copy the secrets map `in` to `out`, keeping a key only when its allowed hosts in new_pkg are a
 * subset of those in old_pkg (old_pkg NULL: all kept). Dropped keys, comma-separated, in `dropped` (may be NULL). */
int tph_secrets_carry(const tph_reader_t *old_pkg, const tph_reader_t *new_pkg, const uint8_t *in, size_t in_len,
                      uint8_t *out, size_t out_cap, size_t *out_len, char *dropped, size_t dropped_cap);

/* ------------------------------------------------------------------ running */

typedef struct tph_plugin tph_plugin_t;

typedef enum { TPH_PHASE_RENDER = 0, TPH_PHASE_TASK = 1 } tph_phase_t;

/* Store adapter (store.h provides one over flash; tests use memory).
 * get returns the value length (copies only if <= cap) or TINY_E_NOTFOUND.
 * put/del return TINY_OK, TINY_E_INVAL or TINY_E_LIMIT (host-abi.md store rules are
 * checked by the host before calling). put/del are only called in TASK phase. */
typedef struct {
    int (*get)(void *ctx, const char *key, size_t klen, uint8_t *out, size_t cap);
    int (*put)(void *ctx, const char *key, size_t klen, const uint8_t *val, size_t vlen);
    int (*del)(void *ctx, const char *key, size_t klen);
    void *ctx;
} tph_store_t;

/* Poster cache (scene 1.3, store.md "Poster cache"): tposter records by key. get copies a record into out and
 * returns its length (<= cap), or < 0 when absent; put offers one. Both are called inside plugin calls: no
 * allocation, no flash writes there (put keeps records in RAM until the phase ends). */
typedef struct {
    int (*get)(void *ctx, uint64_t key, uint8_t *out, size_t cap);
    void (*put)(void *ctx, uint64_t key, const uint8_t *rec, size_t len);
    void *ctx;
} tph_cache_t;

/* Image tasks (manifest.md "Image tasks", store.md "Images"): the core keeps their files in the plugin's slot;
 * `task` is the task's index in tph_info_t.tasks, `part` which file. open: NULL when missing or on an error; `write`
 * creates or truncates. Writes come front to back. rename replaces `to` atomically. */
enum { TPH_IMG_RASTER = 0, TPH_IMG_DL = 1, TPH_IMG_NEW = 2, TPH_IMG_LST = 3, TPH_IMG_TMP = 4, TPH_IMG_OLD = 5,
       TPH_IMG_WORK = 6, TPH_IMG_RING = 7 }; /* 3: cached list, 4: photo decode, 5: the photo shown before (kept), 6: a
                            * photo's download outside the image area for its decode (store.md "Image area"), 7: the
                            * photo ring's index (album.md "Photo ring") */
#define TPH_RING_MAX 32  /* photo ring rasters: parts TPH_IMG_RING0 .. +TPH_RING_MAX-1 (`img<i>.r<k>`) */
#define TPH_IMG_RING0 8
#define TPH_IMG_PARTS (TPH_IMG_RING0 + TPH_RING_MAX)
#define TPH_IMG_WORK_MAX (256u * 1024u) /* the largest TPH_IMG_WORK download (a progressive preview on the C3 boards) */
typedef struct {
    void *(*open)(void *ctx, uint8_t task, int part, bool write);
    long (*read)(void *ctx, void *f, uint32_t off, void *buf, size_t len); /* bytes (fewer at the end), < 0 error */
    int (*write)(void *ctx, void *f, uint32_t off, const void *buf, size_t len); /* 0 ok */
    void (*close)(void *ctx, void *f);
    int (*rename)(void *ctx, uint8_t task, int from, int to);             /* 0 ok */
    void (*remove)(void *ctx, uint8_t task, int part);
    void *ctx;
    /* optional (NULL: no limit): 0 when `need` more bytes fit the image area (store.md "Image area"), else the
     * download is not kept (decoded as it comes) */
    int (*room)(void *ctx, uint8_t task, uint32_t need);
    /* optional (NULL: 8): the photo ring's size for rasters of `raster` bytes, 8..TPH_RING_MAX (album.md "Photo ring") */
    unsigned (*ring_n)(void *ctx, uint32_t raster);
} tph_images_t;

/* The photo album (album.md), for a steps task in album mode: its entries in album order, a photo's image by id
 * (album.md "Photo file": the raster or the JPEG, offsets from its start; read and close as the image files), link
 * photos added from the task's photo file and removed. NULL in tph_env_t: no album (the
 * task key `album` is ignored, as on firmware without it). */
typedef struct {
    int (*count)(void *ctx);
    bool (*get)(void *ctx, int pos, talb_entry_t *e);
    void *(*open)(void *ctx, uint32_t id);                                 /* NULL: missing */
    long (*read)(void *ctx, void *f, uint32_t off, void *buf, size_t len);
    void (*close)(void *ctx, void *f);
    bool (*skipped)(void *ctx, uint32_t key);                              /* a link photo the user deleted */
    uint32_t (*budget)(void *ctx);                                         /* album.md "Budget": bytes, whole blocks */
    uint16_t (*max)(void *ctx);                                            /* entries */
    /* The raster of image task `task` becomes a link photo (its file renamed into the album): 0 ok (*id set), else
     * TINY_E_LIMIT (album full, no room) or TINY_E_IO. `e` gives key, caption, date, added; the rest from the raster. */
    int (*adopt)(void *ctx, uint8_t task, const talb_entry_t *e, uint32_t *id);
    int (*del)(void *ctx, uint32_t id);                                    /* a link photo out (not skipped) */
    uint32_t (*synced)(void *ctx);
    void (*set_synced)(void *ctx, uint32_t unix_s);
    void (*skip)(void *ctx, uint32_t key);                                 /* a link photo the device cannot show */
    /* The album link's list (album.md "List"): read (<= cap items, *made its time), write (made: the cached list's) */
    int (*list)(void *ctx, talb_item_t *out, int cap, uint32_t *made);
    int (*set_list)(void *ctx, const talb_item_t *items, int n, uint32_t made);
    bool (*play)(void *ctx, talb_play_t *st);                              /* the play state; false: none */
    void (*set_play)(void *ctx, const talb_play_t *st);
    void *ctx;
    void (*set_want)(void *ctx, uint32_t n); /* the window found by a run (album.md index `want`, report); NULL: none */
    /* A photo's kept info (album.md "Info"): its sidecar's bytes into buf (<= cap), 0 none; NULL: no sidecars. `adopt`
     * keeps `e->meta_data`. */
    size_t (*meta)(void *ctx, uint32_t id, uint8_t *buf, size_t cap);
    /* A kind jpeg upload a pick could not decode (album.md "Pick"): flags bit 7 set; NULL: none. */
    void (*mark_bad)(void *ctx, uint32_t id);
    /* Every link photo out, uploads kept (a new source, album.md "Fill"), a few per index write: the number deleted.
     * NULL: del one by one. */
    int (*del_links)(void *ctx);
} tph_album_t;

/* The image relay (docs/spec/image-fetch.md "Relay", config `image_relay`): image steps ask it for the part of the
 * photo they show. TASK only; NULL in tph_env_t: off. */
typedef struct {
    const char *url;           /* base URL, http(s)://, no query or fragment, <= 200 bytes */
    bool lan;                  /* an http:// relay may carry the step's auth (config image_relay.lan) */
    /* The request signature for GET `path` (the target with its query; remote.md "Device request signing"): the
     * header lines "X-Tiny-Device: ..\nX-Tiny-Time: ..\nX-Tiny-Sig: ..\n" into out. 0 ok; else no relay for this
     * photo (no identity, no valid time). */
    int (*sign)(void *ctx, const char *path, char *out, size_t cap);
    /* false while the relay is off after failures ("Relay" Backoff). NULL: always usable */
    bool (*usable)(void *ctx);
    /* A relay request's outcome: ok, or failed; `source`: the photo's own failure (502 `source ...`), not counted.
     * The host keeps the count (3 in a row: off for 60 min, RTC relay_off_min). NULL: none kept */
    void (*result)(void *ctx, bool ok, bool source);
    void *ctx;
} tph_relay_t;

/* The box a task's picture is for, when a layout's widget shows it and not the plugin's own screen (widgets.md
 * "Sources" Picture size): the relay is asked for this size and the download is the picture, no raster is made.
 * w 0: none, the screen (the plugin's own picture). */
typedef struct {
    uint16_t w, h;
    bool fill; /* fills the box (cropped) instead of fitting inside it */
} tph_image_box_t;

/* Inputs that stay fixed for one instance. Buffers must outlive the instance. */
typedef struct {
    const uint8_t *settings; size_t settings_len;           /* validated CBOR map */
    const uint8_t *device_settings; size_t device_settings_len;
    const uint8_t *secrets; size_t secrets_len;             /* CBOR map key -> tstr; TASK only */
    void (*log)(void *ctx, const char *msg, size_t len);
    void (*time_now)(void *ctx, tiny_time_t *out);          /* TASK: current local time */
    void *ctx;
    const tph_cache_t *cache;                               /* RENDER, scene: tposter results; NULL: none */
    const tph_images_t *images;                             /* TASK and RENDER: image task files; NULL: none */
    const tph_album_t *album;                               /* TASK and RENDER: the photo album; NULL: none */
    bool offline;                                           /* TASK: a run without WiFi (album mode, album.md) */
    bool net_ok;                                            /* TASK: config network `full` (a sync may ask for WiFi) */
    uint32_t wifi_retry; /* TASK: unix s of the next WiFi try after failed sessions, 0 none (album.md "Pick") */
    /* TASK: true until the run ends when the host wants a safe stop (queued press or plugin switch):
     * requests and downloads stop; the run writes no error and its staged store values are discarded. NULL: never */
    bool (*stop)(void *ctx);
    /* RENDER: the plugin's strings file for the device language (manifest.md "Strings"), host key sys.strings;
     * the caller checked it against tph_strings_hash. NULL: none (English, the package's own texts) */
    const uint8_t *strings; size_t strings_len;
    /* TASK: a press waits (album.md "Fill"). `job` true: a stoppable job runs (the album fill), presses made meanwhile
     * are kept for the host to act on after the run, true when one waits (the job ends, its download fails as a
     * network error but the run is ok); false: the job ended. NULL: never */
    bool (*press)(void *ctx, bool job);
    const tph_relay_t *relay;  /* TASK: the image relay (config image_relay); NULL: off */
    const tph_image_box_t *boxes; /* TASK: TPH_MAX_TASKS boxes, by task; NULL: none */
    /* RENDER, walls (scene FN 41 wrect): member i's view [x, y, w, h] on the canvas (i < tph_screen_t wall_n, never
     * this member's own: the host answers that from the view). false: unknown. Read on demand from the stored wall
     * (no member table in RAM). NULL: only the own rect */
    bool (*wall_rect)(void *ctx, uint8_t i, uint16_t out[4]);
    /* TASK: current photo n and total of (1-based) before a download; 0, 0 at run start and end clears the counts.
     * Album fills count held link photos toward the window. NULL: no progress reporting. */
    void (*progress)(void *ctx, uint32_t n, uint32_t of);
} tph_env_t;
/* TASK phase: the allow-list and HTTP limits come from the manifest task being run. */

/* Cached layers (API 1.1). read() copies bytes of layer `id`: packed rows of the whole
 * panel at the active depth, gfx_buf_size(panel_w, panel_h, depth) bytes. 0 = ok,
 * non-zero = not cached (layer_blit then returns TINY_E_NOTFOUND). */
typedef struct {
    int (*read)(void *ctx, uint8_t id, uint32_t off, void *buf, size_t len);
    void *ctx;
} tph_layers_t;

/* tph_frame_t.sync: the host keys sys.* of a frame (host-abi.md "Host keys"). 0 = not known: every
 * sys.* key is TINY_E_NOTFOUND. Network: neither NET bit = full. */
#define TPH_SYNC_KNOWN 0x0001u
#define TPH_SYNC_TIME_OK 0x0002u      /* sys.ok.time */
#define TPH_SYNC_NET_TIME 0x0004u     /* sys.net "time" */
#define TPH_SYNC_NET_OFF 0x0008u      /* sys.net "off" */
/* sys.action n (1..6), scene IN 23 (manifest.md "Actions"): the first frame after a press; bits 4..6 */
#define TPH_SYNC_ACTION_MASK 0x0070u
#define TPH_SYNC_ACTION(n) ((uint16_t)(((unsigned)(n) & 7u) << 4))
#define TPH_SYNC_ACTION_N(sync) ((int)(((sync) >> 4) & 7u))
#define TPH_SYNC_OPTIONS 0x0080u      /* sys.options 1: the host lists the offered actions (firmware.md "Options list") */
#define TPH_ACT_KEY_PREFIX "sys.act." /* store keys the host writes: presses of action n, "sys.act.<n>" (CBOR uint) */
/* the action pressed last and when (host-abi.md "Host keys"): a store key the host writes at a press, CBOR uint
 * unix ms * 8 + the action's number */
#define TPH_ACT_LAST_KEY TPH_ACT_KEY_PREFIX "last"
#define TPH_ACT_KEY_1 TPH_ACT_KEY_PREFIX "1"
#define TPH_ACT_KEY_2 TPH_ACT_KEY_PREFIX "2"
#define TPH_SYNC_TASK_OK(i) (0x0100u << (i)) /* sys.ok.<name of manifest task i> */

/* Inputs of one frame (host-abi.md: fixed per render). */
typedef struct {
    tiny_time_t time;          /* the frame's local time, second = 0 */
    int32_t battery_pct;       /* -1 unknown */
    int32_t charging;
    int32_t rssi_last;         /* 0 unknown */
    const tph_store_t *store;  /* the store version of this frame */
    const tph_layers_t *layers; /* may be NULL (no cache) */
    uint16_t sync;             /* TPH_SYNC_* (RENDER); 0 in tasks */
    bool prev;                 /* the previous frame drawn again (a partial refresh): its `tap` targets do not count */
    bool back;                 /* TASK: a press of a `previous` role action: the pick steps back (manifest.md "Actions") */
    bool peek;                 /* TASK: peek the next pick (album.md "Photo ring" Fill): the list/pick step runs, the image step only
                                * names its photo (peek_key, peek_url), nothing is written (store, shuffle state), nothing downloaded */
    bool ring_add;             /* TASK: fill the ring (album.md "Photo ring" Fill): a photo task downloads and decodes the next pick into
                                * a ring slot (nothing else of the screen touched), writes no store value, keeps its error for
                                * tph_task_ring_error; offline: only asks for WiFi (tph_task_wants_net) when a download is due */
    uint32_t *peek_key;        /* out of a peek run: the photo's key (0 none; NULL: not wanted) */
    char *peek_url;            /* out of a peek run: its URL, peek_cap bytes (NULL: not wanted) */
    size_t peek_cap;
    bool renew;                /* TASK: a sync: a cached list is made again (manifest.md "Cached lists" Sync) */
    int64_t due_s;             /* TASK: the due time (UTC s) a run up to `flex` early serves, 0 none: `{time}` (scene 1.16) */
} tph_frame_t;

/* Scene 1.12 `tap` (scene.md "Builtins", firmware.md "Touch"): the tap targets of the frame rendered since
 * tph_tap_reset (every band of it; the same rect and action once), at most TPH_TAP_MAX (the first ones), in logical px
 * of the screen (a wall view's origin taken off, clipped to the screen). tph_tap_targets returns the count. */
#define TPH_TAP_MAX 8
typedef struct {
    uint16_t x, y, w, h;
    uint8_t n; /* the action number, 1..6 */
} tph_tap_t;
void tph_tap_reset(tph_plugin_t *p);
int tph_tap_targets(const tph_plugin_t *p, tph_tap_t *out, int cap);

/* Value of host key `key` (a "sys." key) for frame bits `sync` and the manifest tasks of `info`:
 * the CBOR length (copied only if <= cap), or TINY_E_NOTFOUND. */
int tph_sys_key(const tph_info_t *info, uint16_t sync, const char *key, size_t klen, uint8_t *out, size_t cap);
/* The same for a frame, sys.act.<n> from its store. tph_presses: that count (0 when absent). */
int tph_sys_value(const tph_info_t *info, const tph_frame_t *f, const char *key, size_t klen, uint8_t *out, size_t cap);
uint32_t tph_presses(const tph_frame_t *f, int n);
static inline bool tph_is_sys_key(const char *key, size_t klen) { return klen >= 4 && !memcmp(key, "sys.", 4); }
/* sys.act.1 .. sys.act.6 (host-abi.md "Host keys"): presses of action n, stored by the host as CBOR uint 0..65535
 * (the frame's store answers them, absent = 0). Returns n, 0 for another key. */
static inline int tph_act_key(const char *key, size_t klen)
{
    return klen == 9 && !memcmp(key, "sys.act.", 8) && key[8] >= '1' && key[8] <= '0' + TPH_MAX_ACTIONS ? key[8] - '0'
                                                                                                       : 0;
}

/* Check and open the package (scene plugins only). `pkg` must stay valid until close. */
int tph_open(tph_plugin_t **out, const tph_reader_t *pkg, const hal_board_t *board,
             const tph_screen_t *scr);
void tph_close(tph_plugin_t *p);
const tph_info_t *tph_info(const tph_plugin_t *p);
/* The sha256 the manifest `strings` names for the device language lang (BCP 47): its own entry, else its primary
 * subtag's ("nl" for "nl-BE"). TPH_OK, or TINY_E_NOTFOUND (no entry, "en", or no `strings`). */
#define TPH_STRINGS_KEY "sys.strings"
#define TPH_STRINGS_MAX 4096 /* bytes of a strings file (a store value's cap) */
int tph_strings_hash(const tph_plugin_t *p, const char *lang, uint8_t sha[32]);
/* Render-phase heap an open scene plugin needs (scene bytes when copied, machine state, arena,
 * vector work buffer and margin; compared with hal_board_t.heap_render). Frame buffers not included.
 * These are separate allocations, not a single contiguous block. */
uint32_t tph_render_need(const tph_plugin_t *p);

/* Start a phase. RENDER: fresh instance (memory cap per phase), runs start + init (INIT budget).
 * TASK: only the HTTP request buffers; the instance comes with the first code task or
 * migrate_store, so a session of declarative tasks never creates one. */
int tph_begin(tph_plugin_t *p, tph_phase_t phase, const tph_env_t *env);
void tph_end(tph_plugin_t *p);

/* RENDER phase. Clears the framebuffer (band) to white, calls render() with
 * screen().clip_* = gfx_band_logical(fb). May be called several times per instance. */
int tph_render(tph_plugin_t *p, const tph_frame_t *frame, gfx_fb_t *fb);

/* RENDER phase begun: the palette of this plugin's frames (docs/spec/assets.md "Palette frames"). GFX_PAL_BWRY on a
 * color panel (hal_board_t colors) at depth 2 when the plugin uses color: a scene program of header version >= 8
 * (inks, scene 1.8), or an image task whose raster holds BWRY codes (store.md "Images"). Else GFX_PAL_GRAY: the
 * frames stay as they were before palette frames. The caller sets it on every band (gfx_fb_set_palette). */
uint8_t tph_frame_palette(tph_plugin_t *p);

/* After tph_render: the frame named an image its raster no longer holds (timg, scene.md "Images": the previous frame of
 * a partial refresh after a new image). The frame cannot be drawn as it was shown: refresh the panel fully. */
bool tph_render_stale(const tph_plugin_t *p);

/* After tph_render: it drew an image task's raster (a photo). A partial refresh that changes grays of such a frame is a
 * full one (docs/spec/firmware.md "Refresh path" `image`). */
bool tph_render_image(const tph_plugin_t *p);

/* RENDER phase: rebuild layer `id` (< info.layer_count). Clears the band to white and
 * calls layer(id) in LAYER context (no frame inputs). The caller stores the band rows
 * (store.md layer<id>.bin); for bands, call once per band. */
int tph_render_layer(tph_plugin_t *p, uint8_t id, gfx_fb_t *fb);

/* RENDER phase, update.custom only: *next = clamped unix seconds of the next render. */
int tph_next_update(tph_plugin_t *p, int64_t now, int64_t *next);

/* TASK phase. `store` is the session writer; the core commits or discards the run's
 * writes according to *result (TINY_TASK_*). A trap returns TPH_E_TRAP/TPH_E_BUDGET
 * (treat as retry + failure). `frame->time` is the time at session start.
 * Code tasks run in an instance created on the first one of the phase (tph_begin(TASK) creates
 * none); declarative tasks (info.tasks[i].decl) run in the host without one (host-abi.md
 * "Declarative tasks"). *result may carry TPH_TASK_COMMIT: commit the run's writes although
 * the result is not ok (a declarative task with on_error: clear deleted its keys). A stopped run
 * returns retry without TPH_TASK_COMMIT: discard its staged writes and keep the old store. */
#define TPH_TASK_COMMIT 0x100
int tph_run_task(tph_plugin_t *p, const char *name, const tph_frame_t *frame, int *result);

/* After tph_run_task: minutes until the task's next run that its answer asked for (image task `every`, clamped to
 * 5..1440), 0 = its own schedule. */
uint32_t tph_task_next_min(const tph_plugin_t *p);
/* After tph_run_task: in a wall, the unix minute of the time slot the run picked for (manifest.md "Steps" Wall; later
 * than the run: a prefetch, firmware.md "Walls"), 0 = not a wall's steps run. */
uint32_t tph_task_wall_min(const tph_plugin_t *p);
/* After tph_run_task: an ok run whose step `stale` was true (manifest.md "Tasks" Retry): its data is old, the task's
 * `retry` keeps trying. */
bool tph_task_stale(const tph_plugin_t *p);
/* After tph_run_task: a run without WiFi of a task in album mode whose link sync is due asks for a WiFi session
 * (album.md "Schedule"): the caller marks the task pending so the next session runs it with the network. */
bool tph_task_wants_net(const tph_plugin_t *p);
/* After tph_run_task of a declarative task that failed: its stable code (manifest.md "Task errors"), also when the
 * store keeps none (a task that loaded before keeps its screen); NULL: none. */
const char *tph_task_error(const tph_plugin_t *p);
/* After a ring_add run: a raster joined the ring / the run failed with this code and message (NULL: no failure). */
bool tph_task_ring_added(const tph_plugin_t *p);
const char *tph_task_ring_error(const tph_plugin_t *p, const char **msg); /* the code, NULL: none */
/* After tph_run_task: an ok run in album mode that changed nothing on the screen (a fetch run between two picks,
 * album.md "Fill"): the caller drops its store writes (the downloads' values), so no new frame and no refresh. */
bool tph_task_unchanged(const tph_plugin_t *p);
/* Task `i` is in album mode with these settings (album.md "Plugin"): it runs without WiFi (the scheduler's net off).
 * False without env->album. Outside a phase. */
bool tph_task_album_on(tph_plugin_t *p, int i, const tph_env_t *env);

/* Image tasks (store.md "Images"), outside a phase: a raster that no longer fits the screen (depth, rotation) or
 * the fit setting is decoded again from the kept download, no network. Returns the image tasks (bit i: task i)
 * whose raster could not be made that way (no download kept, or it failed): run them now. */
uint32_t tph_image_sync(tph_plugin_t *p, const tph_env_t *env);

/* TASK phase: migrate_store(from) if exported. TPH_E_STATE if not exported. */
int tph_migrate_store(tph_plugin_t *p, uint32_t from_version, const tph_frame_t *frame, int *result);

/* A device setting as a program reads it (DEVICE, manifest.md): its encoded value from the settings map `devset`
 * (NULL: none), else the default of that key, into out. The length (above cap: nothing copied), or TINY_E_NOTFOUND
 * for a key that is no device setting. Also for programs that are no plugin (the widgets of a layout). */
int tph_device_value(const uint8_t *devset, size_t len, const char *k, size_t n, uint8_t *out, size_t cap);

/* Peak linear memory and host pool use of the current/last instance (reports, 3.9). A scene plugin: mem_peak is
 * the most arena bytes a call read, instructions the fuel of the last call (instructions and drawing work),
 * scene_ops its instructions alone. */
typedef struct {
    uint32_t mem_peak, pool_peak, instructions;
    uint32_t instances;        /* WebAssembly instances created since tph_begin */
    uint32_t scene_ops;
    /* scene: tposter layouts since tph_begin, computed and from the cache, and the steps computed (C3 time) */
    uint32_t layouts, layout_hits, layout_steps;
    uint32_t img_missed;       /* timg calls that drew nothing although the frame named an image */
} tph_stats_t;
void tph_stats(const tph_plugin_t *p, tph_stats_t *out);

#endif
