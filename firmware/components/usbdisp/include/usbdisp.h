/*
 * usbdisp.h - the USB display protocol (docs/spec/usb-display.md): a board as a plain display driven over the USB
 * link. hello, show (a PNG of exactly the panel's size, shown as given with a full refresh) and clear; every refusal
 * has a code and a text. Frames as the link protocol (link.h link_parser_feed, link_frame_encode).
 *
 * The transfer is bounded (png_max, a quiet transfer is dropped after USBDISP_IDLE_US) and the panel is touched only
 * once the whole PNG has decoded into a frame: any failure before that leaves the old picture.
 * Everything outside (link bytes, memory, clock, panel) goes through usbdisp_cfg_t: usbdisp_hal_cfg binds it to
 * tiny/hal.h, tests plug their own.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_USBDISP_H
#define TINY_USBDISP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "link_frame.h"
#include "tiny/types.h"

#define USBDISP_PROJECT "display"
#define USBDISP_CHUNK 4096               /* bytes of PNG per show_chunk the device asks for */
#define USBDISP_PNG_MIN 57               /* signature, IHDR, an empty IDAT, IEND */
#define USBDISP_PNG_HEAD 29              /* signature and IHDR data: what the first chunk must hold */
#define USBDISP_IDLE_US 10000000ull      /* a transfer without a request for this long is dropped */
#define USBDISP_FRAME_IDLE_US 2000000ull /* half a frame without a further byte for this long is forgotten */
#define USBDISP_RETRY_MS 2000            /* BUSY detail retry_ms */

/* Commands (0x08..0x0B are free in the app's link protocol: the app answers them UNKNOWN_CMD). */
enum {
    USBDISP_CMD_HELLO = 0x01,
    USBDISP_CMD_SHOW_BEGIN = 0x08,
    USBDISP_CMD_SHOW_CHUNK = 0x09,
    USBDISP_CMD_SHOW_END = 0x0A,
    USBDISP_CMD_CLEAR = 0x0B,
};

/* Reason codes (`err`): 1..14 as the link protocol's, 32.. this protocol's own. */
enum {
    USBDISP_E_UNKNOWN_CMD = 1,
    USBDISP_E_BAD_REQUEST = 2,
    USBDISP_E_BUSY = 3,         /* the panel is refreshing: detail { retry_ms } */
    USBDISP_E_IO = 10,          /* the panel driver failed */
    USBDISP_E_SEQUENCE = 11,    /* detail { next }: the offset the device expects */
    USBDISP_E_TOO_LARGE = 13,   /* more bytes than hello.max_png, or than show_begin announced */
    USBDISP_E_NOMEM = 14,
    USBDISP_E_NOT_PNG = 32,
    USBDISP_E_WRONG_SIZE = 33,  /* not exactly the panel's width and height */
    USBDISP_E_DEPTH = 34,       /* a color type or bit depth the panel does not show, interlaced, a foreign color */
    USBDISP_E_BAD_PNG = 35,     /* damaged or truncated PNG data */
};

typedef struct {
    const char *fw, *board;
    uint16_t w, h;
    uint8_t depths;             /* gray PNG bit depths taken: bit0 = 1, bit1 = 2, bit2 = 4 */
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
    /* the panel: a full refresh at `depth` (bwry: 2, palette codes of assets.md "Palette frames"); 0 = ok.
     * image: new picture content (hal_panel_new_image) */
    int (*panel_begin)(void *ctx, uint8_t depth, bool bwry, bool image);
    int (*panel_write)(void *ctx, const tiny_rect_t *r, const uint8_t *rows, size_t stride);
    int (*panel_refresh)(void *ctx);
    void (*panel_sleep)(void *ctx);
    void (*log)(void *ctx, const char *line); /* optional */
    void *ctx;
} usbdisp_cfg_t;

typedef struct {
    usbdisp_cfg_t c;
    link_parser_t parser;
    uint8_t rx[256];
    size_t rx_len, rx_pos;
    uint8_t tx[384];
    /* the transfer */
    uint8_t *png;
    uint32_t size, got;
    uint64_t last_us;           /* the last request of the transfer */
    uint64_t rx_us;             /* the last link bytes */
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
/* Drop the transfer and free its memory (before sleep). */
void usbdisp_reset(usbdisp_t *d);

#endif
