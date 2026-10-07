/* usbdisp: requests and the transfer of the USB display protocol (docs/spec/usb-display.md); the PNG is read in
 * usbdisp_png.c. SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>

#include "tiny/cbor.h"
#include "tiny/util.h"
#include "usbdisp.h"
#include "usbdisp_priv.h"

static void logline(usbdisp_t *d, const char *s)
{
    if (d->c.log)
        d->c.log(d->c.ctx, s);
}

uint32_t usbdisp_png_bound(uint16_t w, uint16_t h, uint8_t depth)
{
    size_t bits, n;
    if (!tiny_size_mul(w, depth, &bits) || !tiny_size_add(bits, 7, &bits))
        return 0;
    /* a filter byte a row; 4096 for the signature, IHDR, PLTE, IDAT headers and the zlib framing of stored blocks */
    if (!tiny_size_add(bits / 8, 1, &n) || !tiny_size_mul(n, h, &n) || !tiny_size_add(n, 4096, &n) || n > UINT32_MAX)
        return 0;
    return (uint32_t)n;
}

void usbdisp_init(usbdisp_t *d, const usbdisp_cfg_t *cfg)
{
    memset(d, 0, sizeof *d);
    d->c = *cfg;
    if (!d->c.png_max) {
        uint8_t deep = d->c.bwry ? 8 : d->c.depths & 4 ? 4 : d->c.depths & 2 ? 2 : 1; /* bwry: indexed, up to 8 bit */
        d->c.png_max = usbdisp_png_bound(d->c.w, d->c.h, deep);
    }
    link_parser_init(&d->parser);
}

bool usbdisp_transfer(const usbdisp_t *d) { return d->png != NULL; }

void usbdisp_reset(usbdisp_t *d)
{
    if (d->png)
        d->c.free(d->c.ctx, d->png);
    d->png = NULL;
    d->size = d->got = 0;
}

/* ------------------------------------------------------------------ answers */

static void send(usbdisp_t *d, const link_frame_t *f, const cbor_w_t *w, const uint8_t *payload)
{
    uint8_t out[sizeof d->tx + LINK_FRAME_OVERHEAD];
    if (w->err)
        return; /* cannot happen: every answer fits tx (the host test builds each) */
    size_t n = link_frame_encode(out, sizeof out, f->seq, (uint8_t)(f->cmd | 0x80), payload, w->len);
    if (n)
        d->c.write(d->c.ctx, out, n);
}

/* { err, msg, detail?: { <dkey>: dval } } */
static void refuse(usbdisp_t *d, const link_frame_t *f, int err, const char *msg, const char *dkey, uint32_t dval)
{
    cbor_w_t w;
    cbor_w_init(&w, d->tx, sizeof d->tx);
    cbor_put_map(&w, dkey ? 3 : 2);
    cbor_put_str(&w, "err");
    cbor_put_uint(&w, (uint64_t)err);
    cbor_put_str(&w, "msg");
    size_t mn = strlen(msg);
    cbor_put_tstr(&w, msg, mn < LINK_MSG_CAP ? mn : LINK_MSG_CAP - 1);
    if (dkey) {
        cbor_put_str(&w, "detail");
        cbor_put_map(&w, 1);
        cbor_put_str(&w, dkey);
        cbor_put_uint(&w, dval);
    }
    d->refused++;
    send(d, f, &w, d->tx);
}

static void hello(usbdisp_t *d, const link_frame_t *f)
{
    static const char *const bwry[] = {"black", "white", "red", "yellow"};
    cbor_w_t w;
    cbor_w_init(&w, d->tx, sizeof d->tx);
    cbor_put_map(&w, d->c.bwry ? 11 : 10);
    cbor_put_str(&w, "proto");
    cbor_put_uint(&w, LINK_PROTO);
    cbor_put_str(&w, "project");
    cbor_put_str(&w, USBDISP_PROJECT);
    cbor_put_str(&w, "fw");
    cbor_put_str(&w, d->c.fw);
    cbor_put_str(&w, "board");
    cbor_put_str(&w, d->c.board);
    cbor_put_str(&w, "w");
    cbor_put_uint(&w, d->c.w);
    cbor_put_str(&w, "h");
    cbor_put_uint(&w, d->c.h);
    cbor_put_str(&w, "depths");
    cbor_put_array(&w, (size_t)((d->c.depths & 1) + (d->c.depths >> 1 & 1) + (d->c.depths >> 2 & 1)));
    for (unsigned b = 0; b < 3; b++)
        if (d->c.depths >> b & 1)
            cbor_put_uint(&w, 1u << b);
    if (d->c.bwry) {
        cbor_put_str(&w, "colors");
        cbor_put_array(&w, 4);
        for (int i = 0; i < 4; i++)
            cbor_put_str(&w, bwry[i]);
    }
    cbor_put_str(&w, "max_payload");
    cbor_put_uint(&w, LINK_MAX_PAYLOAD);
    cbor_put_str(&w, "max_png");
    cbor_put_uint(&w, d->c.png_max);
    cbor_put_str(&w, "chunk");
    cbor_put_uint(&w, USBDISP_CHUNK);
    send(d, f, &w, d->tx);
}

static void answer_next(usbdisp_t *d, const link_frame_t *f, bool chunk)
{
    cbor_w_t w;
    cbor_w_init(&w, d->tx, sizeof d->tx);
    cbor_put_map(&w, chunk ? 2 : 1);
    if (chunk) {
        cbor_put_str(&w, "chunk");
        cbor_put_uint(&w, USBDISP_CHUNK);
    }
    cbor_put_str(&w, "next");
    cbor_put_uint(&w, d->got);
    send(d, f, &w, d->tx);
}

/* ------------------------------------------------------------------ the panel */

/* The rows of `frame` (the whole picture), or with frame NULL `band` again for every band (clear). 0 = ok, else the
 * step that failed in *why. */
static int panel_show(usbdisp_t *d, uint8_t depth, bool image, const uint8_t *frame, const uint8_t *band, size_t stride,
                      uint32_t *ms, const char **why)
{
    uint16_t h = d->c.h, rows = d->c.band_rows && d->c.band_rows < h ? d->c.band_rows : h;
    uint64_t t0 = d->c.now_us(d->c.ctx);
    d->in_panel = true;
    int rc = d->c.panel_begin(d->c.ctx, depth, d->c.bwry, image);
    *why = "the panel did not start";
    /* bands top first; on a rows_bottom_up board the bottom band first, each next one ending where the last began */
    for (uint32_t done = 0; !rc && done < h;) {
        uint16_t n = (uint16_t)(h - done < rows ? h - done : rows);
        uint16_t y = (uint16_t)(d->c.rows_bottom_up ? h - done - n : done);
        tiny_rect_t r = {0, (int16_t)y, d->c.w, n};
        rc = d->c.panel_write(d->c.ctx, &r, frame ? frame + (size_t)y * stride : band, stride);
        *why = "the panel did not take the picture";
        done += n;
    }
    if (!rc) {
        rc = d->c.panel_refresh(d->c.ctx);
        *why = "the panel refresh failed";
    }
    d->c.panel_sleep(d->c.ctx);
    d->in_panel = false;
    *ms = (uint32_t)((d->c.now_us(d->c.ctx) - t0) / 1000);
    return rc;
}

static void answer_shown(usbdisp_t *d, const link_frame_t *f, const char *what, uint8_t depth, uint32_t ms)
{
    /* a refresh that came back sooner than a panel can refresh: the driver said ok but the glass was not driven */
    bool fast = d->c.fast_ms && ms < d->c.fast_ms;
    char line[120];
    d->shown++;
    snprintf(line, sizeof line, "%s%s depth=%u ms=%u n=%u%s", fast ? "W " : "", what, (unsigned)depth, (unsigned)ms,
             (unsigned)d->shown, fast ? " too fast for a refresh: look at the panel" : "");
    logline(d, line);
    cbor_w_t w;
    cbor_w_init(&w, d->tx, sizeof d->tx);
    cbor_put_map(&w, fast ? 3 : 2);
    cbor_put_str(&w, "depth");
    cbor_put_uint(&w, depth);
    cbor_put_str(&w, "ms");
    cbor_put_uint(&w, ms);
    if (fast) {
        cbor_put_str(&w, "warn");
        cbor_put_str(&w, "the refresh returned too fast: look at the panel");
    }
    send(d, f, &w, d->tx);
}

static size_t frame_stride(const usbdisp_t *d, uint8_t depth) { return ((size_t)d->c.w * depth + 7) / 8; }

static void show_end(usbdisp_t *d, const link_frame_t *f)
{
    if (!d->png) {
        refuse(d, f, USBDISP_E_SEQUENCE, "no transfer: send show_begin", "next", 0);
        return;
    }
    if (d->got != d->size) {
        refuse(d, f, USBDISP_E_SEQUENCE, "the PNG is not complete", "next", d->got);
        return;
    }
    usbdisp_pnginfo_t pi = {0};
    const char *why = "";
    int err = usbdisp_png_check(&d->c, d->png, d->size, &pi, &why);
    size_t stride = err ? 0 : frame_stride(d, pi.depth), bytes = 0;
    uint8_t *frame = NULL;
    if (!err && !tiny_size_mul(stride, d->c.h, &bytes))
        err = USBDISP_E_NOMEM, why = "the frame is too large";
    if (!err && !(frame = d->c.alloc(d->c.ctx, bytes)))
        err = USBDISP_E_NOMEM, why = "no memory for the frame";
    if (!err)
        err = usbdisp_png_frame(&d->c, d->png, d->size, &pi, frame, stride, &why);
    usbdisp_reset(d); /* the PNG is not needed during the refresh */
    uint32_t ms = 0;
    if (!err && panel_show(d, pi.depth, true, frame, NULL, stride, &ms, &why))
        err = USBDISP_E_IO;
    if (frame)
        d->c.free(d->c.ctx, frame);
    /* the request's payload is gone by now (requests were fed during the refresh): seq and cmd were copied by value */
    if (err)
        refuse(d, f, err, why, NULL, 0);
    else
        answer_shown(d, f, "show", pi.depth, ms);
}

static void clear(usbdisp_t *d, const link_frame_t *f)
{
    usbdisp_reset(d);
    uint8_t depth = d->c.bwry ? 2 : 1; /* white: every bit set at depth 1, palette code 3 on the color panel */
    uint16_t rows = d->c.band_rows && d->c.band_rows < d->c.h ? d->c.band_rows : d->c.h;
    size_t stride = frame_stride(d, depth), bytes = 0;
    uint8_t *band = tiny_size_mul(stride, rows, &bytes) ? d->c.alloc(d->c.ctx, bytes) : NULL;
    if (!band) {
        refuse(d, f, USBDISP_E_NOMEM, "no memory for the frame", NULL, 0);
        return;
    }
    memset(band, 0xFF, bytes);
    uint32_t ms = 0;
    const char *why = "";
    int rc = panel_show(d, depth, false, NULL, band, stride, &ms, &why);
    d->c.free(d->c.ctx, band);
    if (rc)
        refuse(d, f, USBDISP_E_IO, why, NULL, 0);
    else
        answer_shown(d, f, "clear", depth, ms);
}

/* ------------------------------------------------------------------ requests */

static bool get_uint(const link_frame_t *f, const char *key, uint64_t *out)
{
    const uint8_t *v;
    size_t n;
    cbor_r_t r;
    cbor_item_t it;
    if (!cbor_map_get(f->payload, f->len, key, strlen(key), &v, &n))
        return false;
    cbor_r_init(&r, v, n);
    if (!cbor_read(&r, &it) || it.type != CBOR_UINT)
        return false;
    *out = it.u;
    return true;
}

static void show_begin(usbdisp_t *d, const link_frame_t *f)
{
    uint64_t size;
    char msg[LINK_MSG_CAP];
    usbdisp_reset(d); /* a new transfer replaces an open one */
    if (!get_uint(f, "size", &size)) {
        refuse(d, f, USBDISP_E_BAD_REQUEST, "show_begin needs size", NULL, 0);
        return;
    }
    if (size > d->c.png_max) {
        snprintf(msg, sizeof msg, "the PNG has %llu bytes, at most %u are taken", (unsigned long long)size,
                 (unsigned)d->c.png_max);
        refuse(d, f, USBDISP_E_TOO_LARGE, msg, NULL, 0);
        return;
    }
    if (size < USBDISP_PNG_MIN) {
        refuse(d, f, USBDISP_E_NOT_PNG, "too short for a PNG", NULL, 0);
        return;
    }
    d->png = d->c.alloc(d->c.ctx, (size_t)size);
    if (!d->png) {
        refuse(d, f, USBDISP_E_NOMEM, "no memory for the PNG", NULL, 0);
        return;
    }
    d->size = (uint32_t)size;
    d->got = 0;
    answer_next(d, f, true);
}

static void show_chunk(usbdisp_t *d, const link_frame_t *f)
{
    uint64_t off;
    const uint8_t *v;
    size_t n;
    cbor_r_t r;
    cbor_item_t it;
    if (!d->png) {
        refuse(d, f, USBDISP_E_SEQUENCE, "no transfer: send show_begin", "next", 0);
        return;
    }
    if (!get_uint(f, "off", &off) || !cbor_map_get(f->payload, f->len, "data", 4, &v, &n) ||
        (cbor_r_init(&r, v, n), !cbor_read(&r, &it)) || it.type != CBOR_BSTR || !it.len) {
        refuse(d, f, USBDISP_E_BAD_REQUEST, "show_chunk needs off and data", NULL, 0);
        return;
    }
    if (off != d->got) {
        refuse(d, f, USBDISP_E_SEQUENCE, "not the next offset", "next", d->got);
        return;
    }
    if (it.len > d->size - d->got) {
        usbdisp_reset(d);
        refuse(d, f, USBDISP_E_TOO_LARGE, "more bytes than show_begin announced", NULL, 0);
        return;
    }
    if (!d->got) { /* the header at once: a wrong picture is refused before the rest is sent */
        usbdisp_pnginfo_t pi;
        const char *why = "the first chunk must hold the PNG header (29 bytes)";
        int err = it.len < USBDISP_PNG_HEAD ? USBDISP_E_BAD_REQUEST : usbdisp_png_check(&d->c, it.ptr, it.len, &pi, &why);
        if (err) {
            usbdisp_reset(d);
            refuse(d, f, err, why, NULL, 0);
            return;
        }
    }
    memcpy(d->png + d->got, it.ptr, it.len);
    d->got += (uint32_t)it.len;
    answer_next(d, f, false);
}

static void handle(usbdisp_t *d, const link_frame_t *in, bool busy)
{
    link_frame_t f = *in; /* seq and cmd outlive the parser's buffer (show_end feeds it again) */
    if (f.cmd < 0x01 || f.cmd > 0x3F || !f.seq)
        return; /* not a request */
    if (f.len && (f.payload[0] >> 5 != 5 || !cbor_valid(f.payload, f.len))) {
        refuse(d, &f, USBDISP_E_BAD_REQUEST, "the payload is not one CBOR map", NULL, 0);
        return;
    }
    if (f.cmd == USBDISP_CMD_HELLO) {
        hello(d, &f);
        return;
    }
    if (busy) {
        refuse(d, &f, USBDISP_E_BUSY, "refreshing", "retry_ms", USBDISP_RETRY_MS);
        return;
    }
    d->last_us = d->c.now_us(d->c.ctx);
    switch (f.cmd) {
    case USBDISP_CMD_SHOW_BEGIN: show_begin(d, &f); break;
    case USBDISP_CMD_SHOW_CHUNK: show_chunk(d, &f); break;
    case USBDISP_CMD_SHOW_END: show_end(d, &f); break;
    case USBDISP_CMD_CLEAR: clear(d, &f); break;
    default: refuse(d, &f, USBDISP_E_UNKNOWN_CMD, "this is the display firmware: hello, show, clear", NULL, 0); break;
    }
}

/* Bytes still in rx first (the hook runs inside a request of the outer pump), then the link's. */
static int pump(usbdisp_t *d, uint32_t timeout_ms, bool busy)
{
    int handled = 0;
    for (;;) {
        if (d->rx_pos >= d->rx_len) {
            long n = d->c.read(d->c.ctx, d->rx, sizeof d->rx, timeout_ms);
            if (n <= 0)
                return handled;
            d->rx_len = (size_t)n;
            d->rx_pos = 0;
            d->rx_us = d->c.now_us(d->c.ctx);
            timeout_ms = 0;
        }
        size_t used = 0;
        link_frame_t f;
        int got = link_parser_feed(&d->parser, d->rx + d->rx_pos, d->rx_len - d->rx_pos, &used, &f);
        d->rx_pos += used;
        if (got) {
            handle(d, &f, busy);
            handled++;
        }
    }
}

int usbdisp_poll(usbdisp_t *d, uint32_t timeout_ms)
{
    int n = pump(d, timeout_ms, false);
    uint64_t now = d->c.now_us(d->c.ctx);
    /* the start of a frame whose rest never came (a host program that stopped): the next request must not be read as
     * its payload */
    if (d->parser.len > d->parser.consume && now - d->rx_us >= USBDISP_FRAME_IDLE_US)
        link_parser_init(&d->parser);
    if (d->png && now - d->last_us >= USBDISP_IDLE_US) {
        logline(d, "W show: the transfer went quiet, dropped");
        usbdisp_reset(d);
    }
    return n;
}

void usbdisp_busy(usbdisp_t *d)
{
    if (d->in_panel)
        pump(d, 0, true);
}
