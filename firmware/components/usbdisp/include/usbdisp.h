/*
 * usbdisp.h - the USB display protocol (docs/spec/usb-display.md): a board as a plain display driven over the USB
 * link. hello, have (a fingerprint), show (a PNG of the panel's size, or of a rectangle at x, y), clear and set; every refusal has a code
 * and a text. Frames as the link protocol (link.h link_parser_feed, link_frame_encode).
 *
 * The device keeps the frame it last showed (at the depths the board can refresh partially), so a show updates only
 * what changed, without a flash. A full refresh comes by itself when that frame is not known (first picture, after a
 * refused or cut transfer), every full_every updates, and when the host asks. A mode the board or depth cannot do is
 * refused, never replaced silently; the answer says which refresh ran and why.
 *
 * The transfer is bounded (png_max, a quiet transfer is dropped after USBDISP_IDLE_US) and the panel is touched only
 * once the whole PNG has decoded into a frame: any failure before that leaves the old picture.
 * Everything outside (link bytes, memory, clock, panel) goes through usbdisp_cfg_t: usbdisp_hal_cfg binds it to
 * tiny/hal.h, tests plug their own.
 *
 * Bluetooth (spec "Bluetooth", usbdisp_bt.c): the same frames over a second link, behind the stored setting `bt`
 * (off by default, changed over USB only). Off, no bt_* call of the configuration is ever made. On, every request
 * of that link needs bt_secure() (a bonded host), and pairing is taken only in a window opened by the button or
 * over the cable, with the panel saying so.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_USBDISP_H
#define TINY_USBDISP_H

#include "tiny/hal.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "link_frame.h"
#include "tiny/types.h"

#define USBDISP_PROJECT "display"
#define USBDISP_FINGERPRINT_HASH "fnv1a64"
#define USBDISP_FINGERPRINT_BITS 64
#define USBDISP_CHUNK 4096               /* bytes of PNG per show_chunk the device asks for */
#define USBDISP_PNG_MIN 57               /* signature, IHDR, an empty IDAT, IEND */
#define USBDISP_PNG_HEAD 29              /* signature and IHDR data: what the first chunk must hold */
#define USBDISP_IDLE_US 10000000ull      /* a transfer without a request for this long is dropped */
#define USBDISP_FRAME_IDLE_US 2000000ull /* half a frame without a further byte for this long is forgotten */
#define USBDISP_RETRY_MS 2000            /* BUSY detail retry_ms */
#define USBDISP_ALIGN 8                  /* x and the width of a rectangle are multiples of this (hal_panel_write) */
#define USBDISP_FULL_EVERY 20            /* default of `full_every`: a full refresh after this many partial ones */
#define USBDISP_FULL_EVERY_MAX 250
#define USBDISP_PASSES_MAX 4             /* mode passes: drives of the rectangle */
#define USBDISP_KV_FULL_EVERY "dfull"    /* the setting's key (one byte) */
#define USBDISP_KV_BT "dbt"              /* the Bluetooth setting's key (one byte: 1 on, anything else off) */
#define USBDISP_BT_PAIR_S 120            /* the pairing window */
#define USBDISP_BT_QUIET_US 60000000ull  /* a connection without a request for this long keeps nobody awake */
#define USBDISP_BT_RX_MS 20              /* wait on the Bluetooth link per poll while a host is connected */

/* Commands (0x08..0x0B are free in the app's link protocol: the app answers them UNKNOWN_CMD). */
enum {
    USBDISP_CMD_HELLO = 0x01,
    USBDISP_CMD_SHOW_BEGIN = 0x08,
    USBDISP_CMD_SHOW_CHUNK = 0x09,
    USBDISP_CMD_SHOW_END = 0x0A,
    USBDISP_CMD_CLEAR = 0x0B,
    USBDISP_CMD_SET = 0x0C,
    USBDISP_CMD_BT = 0x0D,
    USBDISP_CMD_HAVE = 0x0E,
};

/* Reason codes (`err`): 1..14 as the link protocol's, 32.. this protocol's own. */
enum {
    USBDISP_E_UNKNOWN_CMD = 1,
    USBDISP_E_BAD_REQUEST = 2,
    USBDISP_E_BUSY = 3,         /* the panel is refreshing: detail { retry_ms } */
    USBDISP_E_IO = 10,          /* the panel driver failed */
    USBDISP_E_SEQUENCE = 11,    /* detail { next }: the offset the device expects */
    USBDISP_E_AUTH = 12,        /* a request over Bluetooth from a host that is not bonded */
    USBDISP_E_TOO_LARGE = 13,   /* more bytes than hello.max_png, or than show_begin announced */
    USBDISP_E_NOMEM = 14,
    USBDISP_E_NOT_PNG = 32,
    USBDISP_E_WRONG_SIZE = 33,  /* not exactly the panel's width and height */
    USBDISP_E_DEPTH = 34,       /* a color type or bit depth the panel does not show, interlaced, a foreign color */
    USBDISP_E_BAD_PNG = 35,     /* damaged or truncated PNG data */
    USBDISP_E_MODE = 36,        /* a refresh mode, or an area, this board or depth cannot do; an unknown mode */
    USBDISP_E_RECT = 37,        /* x, y or the PNG's size: not aligned, outside the panel, one of x, y missing */
    USBDISP_E_CABLE = 38,       /* bt over Bluetooth: only over the USB cable */
};

/* Refresh modes of show (`mode`). Without one (AUTO) the device takes partial when it can, else full. */
typedef enum {
    USBDISP_MODE_AUTO = 0,
    USBDISP_MODE_FULL,
    USBDISP_MODE_PARTIAL,       /* only the changed pixels */
    USBDISP_MODE_PARTIAL_ALL,   /* every pixel of the rectangle */
    USBDISP_MODE_INVERSE,       /* the rectangle driven to the negative of the new picture, then to the picture */
    USBDISP_MODE_PASSES,        /* every pixel of the rectangle, `passes` times */
    USBDISP_MODE_NONE,          /* answer only: nothing changed, the panel was not driven */
    USBDISP_MODE_COUNT,
} usbdisp_mode_t;

/* How one panel update is driven (panel_begin) */
typedef enum { USBDISP_DRIVE_FULL = 0, USBDISP_DRIVE_PARTIAL, USBDISP_DRIVE_PARTIAL_ALL } usbdisp_drive_t;

/* The gate clock count of the last update (hal.h hal_gate_clocks_t) */
typedef struct {
    uint32_t edges, frames;
    uint16_t rows, per;
    bool ok;
} usbdisp_gates_t;

typedef struct {
    const char *fw, *board;
    uint16_t w, h;
    uint8_t depths;             /* gray PNG bit depths taken: bit0 = 1, bit1 = 2, bit2 = 4 */
    uint8_t partial_depths;     /* of those, the depths a partial refresh takes (0: none, always a full refresh) */
    bool partial_all;           /* the panel can drive every pixel of a window: partial_all, passes */
    uint8_t full_every;         /* a full refresh after this many partial ones (0: never); the setting replaces it */
    bool bwry;                  /* black, white, red, yellow panel: gray 1-bit, or an indexed PNG of these colors */
    bool rows_bottom_up;        /* hal_board_t rows_bottom_up: bands are written bottom first */
    uint16_t band_rows;         /* rows per panel write (0: the whole frame at once) */
    uint32_t png_max;           /* largest PNG taken (hello max_png); 0: usbdisp_init sets usbdisp_png_bound() */
    uint32_t fast_ms;           /* a refresh that returns sooner gets `warn` in its answer (0: no check) */
    /* link bytes: as hal_link_read / hal_link_write */
    long (*read)(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms);
    long (*write)(void *ctx, const uint8_t *buf, size_t len);
    void *(*alloc)(void *ctx, size_t n);
    void (*free)(void *ctx, void *p);
    uint64_t (*now_us)(void *ctx);
    /* the panel: a refresh at `depth` (bwry: 2, palette codes of assets.md "Palette frames"); 0 = ok.
     * image: new picture content (hal_panel_new_image). A full one is written in bands of whole rows (prev NULL), a
     * partial one as one rectangle with its old (prev) and new rows: both point at the rectangle's first byte in
     * frames of `stride` bytes a row (hal_panel_write). */
    int (*panel_begin)(void *ctx, uint8_t depth, bool bwry, bool image, usbdisp_drive_t drive);
    int (*panel_write)(void *ctx, const tiny_rect_t *r, const uint8_t *prev, const uint8_t *rows, size_t stride);
    int (*panel_refresh)(void *ctx);
    void (*panel_sleep)(void *ctx);
    bool (*panel_gates)(void *ctx, usbdisp_gates_t *g); /* optional: the gate clocks of the last update */
    /* optional, all or none: the panel's tuning knobs (TRMNL X), set from a host with `set { tuning }` and reported in hello
     * and every `set` answer as `tuning`: count, info (name, range, default, words), get, set (false: out of range), defaults.
     * Nothing is stored. tune_count NULL or 0: the board has none and every key is refused. */
    size_t (*tune_count)(void *ctx);
    const hal_tune_t *(*tune_info)(void *ctx, size_t i);
    int (*tune_get)(void *ctx, size_t i);
    bool (*tune_set)(void *ctx, size_t i, int v);
    void (*tune_defaults)(void *ctx);
    /* optional: the device's settings store, as hal_kv_get / hal_kv_set */
    int (*kv_get)(void *ctx, const char *key, void *buf, size_t cap);
    int (*kv_set)(void *ctx, const char *key, const void *buf, size_t len);
    void (*log)(void *ctx, const char *line); /* optional */
    /* optional, all or none: Bluetooth (bt_open NULL: a build without it). Called only while the setting is on,
     * bt_forget also when off (the bonds in the store).
     * bt_open: start the stack and advertise as bt_name; 0 = ok. bt_close: stop it, the radio off.
     * bt_read / bt_write: as read / write. bt_connected: a host is connected. bt_secure: and its link is encrypted
     * with a bonded, passkey-checked key. bt_pairing: open (with the passkey the panel shows) or close the pairing
     * window; closed, the link layer refuses to pair. bt_forget: delete all bonds. rand32: a random number. */
    const char *bt_name;
    int (*bt_open)(void *ctx, const char *name);
    void (*bt_close)(void *ctx);
    long (*bt_read)(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms);
    long (*bt_write)(void *ctx, const uint8_t *buf, size_t len);
    bool (*bt_connected)(void *ctx);
    bool (*bt_secure)(void *ctx);
    void (*bt_pairing)(void *ctx, bool open, uint32_t passkey);
    void (*bt_forget)(void *ctx);
    uint32_t (*rand32)(void *ctx);
    void *ctx;
} usbdisp_cfg_t;

/* The receiving side of one link */
typedef struct {
    link_parser_t parser;
    uint8_t rx[256];
    size_t rx_len, rx_pos;
    uint64_t rx_us;             /* the last link bytes */
} usbdisp_rx_t;

typedef struct {
    usbdisp_cfg_t c;
    usbdisp_rx_t usb;
    uint8_t tx[2048]; /* the largest answer: hello with the tuning, a set that refuses keys */
    /* Bluetooth */
    usbdisp_rx_t *bt;           /* the Bluetooth link's receiving side: allocated while the stack runs, else NULL */
    bool via_bt;                /* the request being answered came over Bluetooth */
    bool bt_on;                 /* the setting */
    bool bt_failed;             /* the stack did not start: not tried again before the next bt { on: true } */
    bool bt_was_connected;
    uint64_t bt_pair_until;     /* the pairing window's end; 0: closed */
    uint64_t bt_last_us;        /* the connection's start or its last request */
    /* the transfer */
    uint8_t *png;
    uint32_t size, got;
    uint64_t last_us;           /* the last request of the transfer */
    /* of the transfer (show_begin) */
    bool area;                  /* x, y given: the PNG is a rectangle there */
    uint16_t x, y;
    usbdisp_mode_t mode;
    uint8_t passes;
    /* what the panel shows */
    uint8_t *prev;              /* the whole frame at prev_depth; NULL: not known, the next update is a full one */
    uint8_t prev_depth;
    char fingerprint[17];       /* FNV-1a 64 bits, lowercase hex; empty when the panel frame is unknown */
    const char *no_prev;        /* why prev is NULL: the `why` of that full refresh */
    uint8_t full_every, since_full;
    bool in_panel;              /* a panel call is running: requests fed from usbdisp_busy get BUSY */
    uint32_t shown, refused;    /* pictures shown (clear counts), requests refused */
} usbdisp_t;

/* Fill cfg from tiny/hal.h (board, panel, link, heap, clock); fw is kept by pointer. */
void usbdisp_hal_cfg(usbdisp_cfg_t *cfg, const char *fw);
/* The largest PNG a panel of this size can need at `depth` bits a pixel: the raw rows, a filter byte each and room
 * for chunk headers. 0 when the size overflows. */
uint32_t usbdisp_png_bound(uint16_t w, uint16_t h, uint8_t depth);

void usbdisp_init(usbdisp_t *d, const usbdisp_cfg_t *cfg);
/* Read link bytes for up to timeout_ms and answer every complete request. Returns the requests handled. Also drops a
 * transfer that went quiet. */
int usbdisp_poll(usbdisp_t *d, uint32_t timeout_ms);
/* From the HAL's wait hook (hal_busy_hook) while the panel refreshes: hello is answered, anything else gets BUSY. */
void usbdisp_busy(usbdisp_t *d);
/* A transfer is open. */
bool usbdisp_transfer(const usbdisp_t *d);
/* Drop the transfer and the frame kept, free their memory (before sleep). */
void usbdisp_reset(usbdisp_t *d);
/* The frame the device believes the panel shows (h rows of ceil(w * depth / 8) bytes), NULL when not known. */
const uint8_t *usbdisp_prev(const usbdisp_t *d, uint8_t *depth);

/* Bluetooth. The setting is on (stored; hello `bt`). */
bool usbdisp_bt_on(const usbdisp_t *d);
/* A press of the device's button: opens the pairing window when the setting is on (and draws the pairing screen).
 * true when it opened. */
bool usbdisp_bt_button(usbdisp_t *d);
/* Bluetooth needs the device awake: the pairing window is open, or a host is connected and was not quiet for
 * USBDISP_BT_QUIET_US. */
bool usbdisp_bt_awake(const usbdisp_t *d);
/* Stop the stack (before sleep): the connection ends, the window closes without a screen. The setting stays; the
 * next usbdisp_poll starts it again. */
void usbdisp_bt_stop(usbdisp_t *d);
/* The seconds the pairing window stays open (0: closed). */
uint32_t usbdisp_bt_pairing(const usbdisp_t *d);

#endif
