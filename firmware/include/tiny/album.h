/*
 * album.h - the photo album on the device (docs/spec/album.md): index and raster formats, limits per board,
 * thumbnails. Shared by the album component (flash, link commands) and plugin_host (album mode of a steps task).
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_ALBUM_H
#define TINY_ALBUM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* TINY_ALBUM: 1 = the core has the photo album (ESP: hal_esp Kconfig CONFIG_TINY_ALBUM; host: always). 0: no album
 * commands (UNKNOWN_CMD), no hello.album, the manifest key `album` ignored, as firmware without it. */
#ifndef TINY_ALBUM
#define TINY_ALBUM 1
#endif

#define TALB_VERSION 1       /* hello.album, the index format */
#define TALB_HDR 24          /* index header bytes */
#define TALB_ENTRY 128       /* index entry bytes */
#define TALB_CAPTION_MAX 88  /* caption bytes (UTF-8) */
#define TALB_DATE_LEN 10     /* YYYY-MM-DD */
#define TALB_SKIP_MAX 32     /* keys of deleted link photos */
#define TALB_GONE_MAX 8      /* ids whose raster is deleted after the index */
#define TALB_MAX 512         /* entries, boards with 2 MiB of filesystem or more (the index <= 64 KiB) */
#define TALB_MAX_SMALL 16    /* entries below that */
#define TALB_THUMB_BYTES 4096
#define TALB_LIST_PAGE 16
#define TALB_RASTER_HDR 20   /* store.md "Images" TIMG header */
#define TALB_SRC_UPLOAD 1
#define TALB_SRC_LINK 2
#define TALB_BLOCK 4096u     /* a filesystem block: photos are counted in whole blocks */
#define TALB_META_MAX 512    /* a photo's kept info (sidecar /album/<id>.m, album.md "Info"), CBOR bytes */
#define TALB_NAME_MAX 64     /* the file name in album_list items */
#define TALB_LIST_MAX 1024   /* the album link's list (album.md "List"): a cached list's items at most */

/* A photo file (album.md "Photo file"): `TAPH` header, the info (CBOR, <= TALB_META_MAX), the image. */
#define TALB_PHOTO_HDR 16
#define TALB_KIND_JPEG 1     /* a JPEG, decoded and dithered for the screen when shown */
#define TALB_KIND_RASTER 2   /* a TIMG raster of the screen (the fallback) */
#define TALB_DEPTH_JPEG 8    /* index entry `depth` of a kind jpeg photo (older firmware never shows it) */
#define TALB_WALL_FILE 0xFF  /* index entry `wall` of a photo file (older firmware never shows it) */
typedef struct {
    uint8_t kind;            /* TALB_KIND_* */
    uint16_t info;           /* info bytes (after the header) */
    uint32_t n;              /* image bytes (after the info) */
    uint16_t w, h;           /* the image's pixels */
} talb_photo_t;
void talb_photo_put(uint8_t out[TALB_PHOTO_HDR], const talb_photo_t *ph);
/* false: not a photo file (a raster of older firmware starts with TIMG), or a bad header. */
bool talb_photo_get(const uint8_t in[TALB_PHOTO_HDR], talb_photo_t *ph);
/* The largest photo file of kind jpeg for a screen of w x h (album.md "Photo file"): 1.5 times a 2-bit raster. */
uint32_t talb_jpeg_max(uint16_t w, uint16_t h);

/* Raster flags (store.md "Images" byte 10). */
#define TALB_RF_FITTED 0x01
#define TALB_RF_FILL 0x02
#define TALB_RF_BWRY 0x04
#define TALB_RF_COLOR 0x08
#define TALB_RF_TONE 0x10
#define TALB_RF_BAD 0x80     /* kind jpeg: a pick could not decode it (an upload, kept; never fits) */

/* Link commands (link-protocol.md, album.md "Link commands"). */
enum {
    TALB_CMD_INFO = 0x28,
    TALB_CMD_LIST = 0x29,
    TALB_CMD_PUT_BEGIN = 0x2A,
    TALB_CMD_PUT_CHUNK = 0x2B,
    TALB_CMD_PUT_END = 0x2C,
    TALB_CMD_EDIT = 0x2D,
    TALB_CMD_THUMB = 0x2E,
    TALB_CMD_META = 0x2F,
};

typedef struct {
    uint32_t id;
    uint32_t key;                     /* a link photo's image key, 0 for uploads */
    uint8_t src;                      /* TALB_SRC_* */
    uint8_t depth, flags, wall;       /* the raster's bytes 5, 10, 11 */
    uint16_t w, h;
    uint32_t size, crc, added;
    char date[TALB_DATE_LEN + 1];     /* "" or YYYY-MM-DD */
    uint8_t caption_len;
    char caption[TALB_CAPTION_MAX + 1]; /* NUL-terminated copy */
    uint8_t meta;                     /* 1: the sidecar /album/<id>.m of older firmware exists (one more block) */
    const uint8_t *meta_data;         /* adding a photo only: its kept info (talb_meta_pack), NULL none */
    uint16_t meta_len;
} talb_entry_t;

/* A photo's kept info (album.md "Info"): the fields the plugin's info card shows, besides caption and date. */
enum { TALB_M_NAME, TALB_M_ALBUM, TALB_M_CAMERA, TALB_M_EXPOSURE, TALB_M_FOCAL, TALB_M_PLACE, TALB_M_TEXTS,
       TALB_M_W = TALB_M_TEXTS, TALB_M_H, TALB_M_BYTES, TALB_M_N };
typedef struct {
    uint16_t has;                     /* bit k: field k is set */
    uint8_t len[TALB_M_TEXTS];        /* text lengths */
    char text[TALB_M_TEXTS][64];      /* not NUL-terminated */
    uint32_t num[3];                  /* w, h, bytes */
} talb_meta_t;
const char *talb_meta_key(int field);
/* Bytes of the sidecar map: the set fields in field order, texts cut at a character boundary to their limit (64, 48
 * for exposure, 16 for focal); 0 when nothing is set. out is TALB_META_MAX bytes. */
size_t talb_meta_pack(const talb_meta_t *m, uint8_t *out);
/* A CBOR map read into m (unknown keys and values of the wrong type dropped, texts cut as above); false: not a map. */
bool talb_meta_parse(const uint8_t *in, size_t n, talb_meta_t *m);
/* The entry has info: a photo file (in the file) or an older sidecar. */
static inline bool talb_has_info(const talb_entry_t *e) { return e->meta || e->wall == TALB_WALL_FILE; }
/* The entry's bytes on flash in whole blocks: the raster and the sidecar. */
uint32_t talb_entry_blocks(const talb_entry_t *e);

/* The screen an album photo must match to be shown (album.md "Pick"). */
typedef struct {
    uint16_t w, h;                    /* logical (rotation applied) */
    uint8_t depth;
    bool color;                       /* a color panel at depth 2: flags bit 3 */
    uint8_t wall;                     /* the device's wall tag, 0 none */
    uint8_t palette;                  /* 0 gray, 1 bw, 2 bwry (album_info `palette`) */
    uint16_t levels;                  /* the gray levels photos use (album_info `levels`, image-fetch.md "Output"), 0 all */
} talb_screen_t;

/* Limits (album.md "Budget"). */
uint16_t talb_max(uint32_t fs_total);
/* The photo budget: max(0, free + album - fs_total / 4); free: the filesystem's free bytes less the image area's unused
 * part, album: the album's rasters in whole blocks. */
uint32_t talb_budget(uint32_t fs_total, uint32_t free, uint32_t album);
uint32_t talb_blocks(uint32_t bytes); /* rounded up to whole TALB_BLOCKs */
uint32_t talb_raster_size(uint16_t w, uint16_t h, uint8_t depth);
/* An entry shown on this screen. */
bool talb_fits(const talb_entry_t *e, const talb_screen_t *s);

/* Entry bytes (album.md "Files"): encode, decode (false: id 0 or a bad length). */
void talb_entry_put(uint8_t out[TALB_ENTRY], const talb_entry_t *e);
bool talb_entry_get(const uint8_t in[TALB_ENTRY], talb_entry_t *e);

/* album_thumb geometry: step s, thumbnail size. */
void talb_thumb_geo(uint16_t W, uint16_t H, uint8_t depth, uint16_t max, uint16_t *s, uint16_t *tw, uint16_t *th);

/* "YYYY-MM-DD" (digits and dashes, 10 bytes). */
bool talb_date_ok(const char *s, size_t n);
/* A date as unix s: "YYYY-MM-DD", optionally "THH:MM" and ":SS" after it (UTC); 0: not a date (or before 1970). */
uint32_t talb_date_value(const char *s, size_t n);

/* ------------------------------------------------------------------ play order, rolling window (album.md "Play") */

/* An item of the album link's list: its key (CRC-32 of the item's list record, 0 becomes 1), its date (unix s, 0 none). */
typedef struct {
    uint32_t key, date;
} talb_item_t;

enum { TALB_ORD_NEXT, TALB_ORD_SHUFFLE, TALB_ORD_NEWEST, TALB_ORD_OLDEST };

#define TALB_PF_LAST 0x01    /* `last` holds the order value of the photo shown last in this round */
#define TALB_PF_FILLING 0x02 /* the last fetch run stopped before the window was full */
#define TALB_PF_WANT 0x04    /* a pick found its photo missing: the next fetch run downloads it first, its pick shows it */

/* The play state (/album/play). */
typedef struct {
    uint8_t mode, flags;     /* TALB_ORD_* it was made for, TALB_PF_* */
    uint32_t seed;           /* this round's seed (shuffle), not 0 */
    uint64_t last;           /* the order value of the photo shown last this round */
    uint32_t rot;            /* the id an offline rotation showed last, 0 none */
    uint32_t fetch_at;       /* no fetch run before this (unix s): after a failed one; 0 none */
    uint32_t shown_at;       /* the last pick (unix s), 0 none */
    uint32_t shown;          /* the id it showed: never deleted for another photo while on the screen */
} talb_play_t;

/* What the play order reads: the index (entries in album order), the skip list, the list. */
typedef struct {
    int (*count)(void *ctx);
    bool (*get)(void *ctx, int pos, talb_entry_t *e);
    bool (*skipped)(void *ctx, uint32_t key);
    int (*list)(void *ctx, talb_item_t *out, int cap); /* items read (<= cap), 0 none */
    void *ctx;
} talb_src_t;

typedef struct {
    uint32_t id, key, date, blocks;
    uint8_t src;
    bool fits;
    int16_t item;            /* a link photo: its list item, -1 none (not in the list: deleted first) */
} talb_vent_t;

/* The album in RAM for one decision (20 bytes an entry, 10 a list item). */
typedef struct {
    talb_vent_t *e;
    int ne;
    talb_item_t *l;
    int16_t *held;           /* per list item: the entry holding it (fits), -1 none, -2 skipped (not a member) */
    int nl;
    int nup;                 /* uploads that fit (members) */
    uint32_t pinned, links;  /* bytes in whole blocks: uploads, link photos */
    int nlinks;
} talb_view_t;

/* 0, TINY_E_LIMIT (no memory). */
int talb_view_load(talb_view_t *v, const talb_src_t *src, const talb_screen_t *s);
void talb_view_free(talb_view_t *v);

typedef struct {
    int pos;                 /* the index entry to show, -1 none */
    uint32_t id;
    bool missed;             /* the photo whose turn it is is not on the device: a stored one instead (rotation) */
    uint32_t index, count;   /* its place in the round (1-based), the photos of the album */
} talb_pick_t;

/* The state for `mode`: kept when made for it, else a new round with seed `seed0` (0: 1). */
void talb_play_use(talb_play_t *st, uint8_t mode, uint32_t seed0);
/* The next photo (album.md "Pick"); st updated (a new state for another mode, `seed0` its seed). false: none fit. */
bool talb_view_pick(const talb_view_t *v, talb_play_t *st, uint8_t mode, uint32_t seed0, talb_pick_t *out);
/* A press of "previous" (manifest.md "Actions" role): the state rewound so that the next pick is the held photo shown
 * before the current one in this round's order (the first of the round: the round's last held one). false: none. */
bool talb_view_back(const talb_view_t *v, talb_play_t *st);

enum { TALB_PLAN_DONE, TALB_PLAN_GET, TALB_PLAN_EVICT };
typedef struct {
    int act;                 /* TALB_PLAN_* */
    int item;                /* GET: the list item */
    uint32_t key;            /* GET: its key */
    uint32_t id;             /* EVICT: the link photo */
} talb_plan_t;

/* Link photos the budget holds: min(keep, nlinks + (budget - pinned - links) / photo, list members): the held ones
 * at their sizes, `photo` for each more (over the budget: fewer, rounded down). Same-size photos: (budget - pinned) / photo. */
uint32_t talb_view_want(const talb_view_t *v, uint32_t budget, uint32_t photo, uint32_t keep);
/* One step of a fetch run (album.md "Window"): budget and photo in bytes (whole blocks), want as talb_view_want. */
void talb_view_plan(const talb_view_t *v, const talb_play_t *st, uint32_t budget, uint32_t photo, uint32_t want,
                    talb_plan_t *out);
/* Link photos ready in play order before the first one missing; *due: a fetch run is due (album.md "Window"). */
uint32_t talb_view_ahead(const talb_view_t *v, const talb_play_t *st, uint32_t want, bool *due);
/* The link photo to delete first for an upload: not in the list, else the one shown furthest ahead; 0 none. */
uint32_t talb_view_victim(const talb_view_t *v, const talb_play_t *st);

#endif
