/* usbdisp: requests and the transfer of the USB display protocol (docs/spec/usb-display.md); the PNG is read in
 * usbdisp_png.c, the panel driven in usbdisp_area.c. SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>

#include "tiny/cbor.h"
#include "tiny/util.h"
#include "usbdisp.h"
#include "usbdisp_priv.h"

void usbdisp_log(usbdisp_t *d, const char *s)
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
    d->no_prev = "first";
    d->full_every = d->c.full_every;
    uint8_t v;
    if (d->c.kv_get && d->c.kv_get(d->c.ctx, USBDISP_KV_FULL_EVERY, &v, 1) == 1 && v <= USBDISP_FULL_EVERY_MAX)
        d->full_every = v, d->full_every_set = true;
    link_parser_init(&d->usb.parser);
    usbdisp_bt_init(d);
}

bool usbdisp_transfer(const usbdisp_t *d) { return d->png != NULL; }

void usbdisp_xfer_drop(usbdisp_t *d)
{
    if (d->png)
        d->c.free(d->c.ctx, d->png);
    d->png = NULL;
    d->size = d->got = 0;
}

void usbdisp_prev_drop(usbdisp_t *d, const char *why)
{
    if (d->prev)
        d->c.free(d->c.ctx, d->prev);
    d->prev = NULL;
    d->fingerprint[0] = 0;
    d->no_prev = why;
}

void usbdisp_reset(usbdisp_t *d)
{
    usbdisp_xfer_drop(d);
    usbdisp_prev_drop(d, "first");
}

const uint8_t *usbdisp_prev(const usbdisp_t *d, uint8_t *depth)
{
    if (d->prev && depth)
        *depth = d->prev_depth;
    return d->prev;
}

size_t usbdisp_stride(const usbdisp_t *d, uint8_t depth) { return ((size_t)d->c.w * depth + 7) / 8; }

/* Canonical panel pixels, not PNG bytes. Mask padding bits in the last byte of each row. */
void usbdisp_fingerprint(const usbdisp_t *d, uint8_t depth, const uint8_t *rows, char out[17])
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    uint8_t head[] = {(uint8_t)(d->c.w >> 8), (uint8_t)d->c.w, (uint8_t)(d->c.h >> 8), (uint8_t)d->c.h,
                      depth, d->c.bwry};
    for (size_t i = 0; i < sizeof head; i++)
        hash = (hash ^ head[i]) * UINT64_C(0x100000001b3);
    size_t stride = usbdisp_stride(d, depth);
    unsigned padding = (8 - (d->c.w * (unsigned)depth) % 8) % 8;
    for (uint16_t y = 0; y < d->c.h; y++)
        for (size_t x = 0; x < stride; x++) {
            uint8_t b = rows ? rows[(size_t)y * stride + x] : 0xFF;
            if (x + 1 == stride)
                b &= (uint8_t)(0xFFu << padding);
            hash = (hash ^ b) * UINT64_C(0x100000001b3);
        }
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; i++)
        out[i] = hex[(hash >> (60 - 4 * i)) & 15];
    out[16] = 0;
}

/* ------------------------------------------------------------------ answers */

void usbdisp_send(usbdisp_t *d, const link_frame_t *f, const cbor_w_t *w)
{
    uint8_t out[sizeof d->tx + LINK_FRAME_OVERHEAD];
    if (w->err)
        return; /* cannot happen: every answer fits tx (the host test builds each) */
    size_t n = link_frame_encode(out, sizeof out, f->seq, (uint8_t)(f->cmd | 0x80), d->tx, w->len);
    if (n && d->via_bt)
        d->c.bt_write(d->c.ctx, out, n);
    else if (n)
        usbdisp_write_all(d, out, n);
}

/* The whole frame, however little the link takes per call (the device's USB write takes 512 bytes at most: an answer
 * longer than that was cut there, and the host read the log lines after it as the rest of the frame). A link that
 * takes nothing or fails ends it: the host is gone. */
void usbdisp_write_all(usbdisp_t *d, const uint8_t *buf, size_t len)
{
    while (len) {
        long k = d->c.write(d->c.ctx, buf, len);
        if (k <= 0)
            return;
        if ((size_t)k > len)
            k = (long)len;
        buf += k;
        len -= (size_t)k;
    }
}

void usbdisp_refuse(usbdisp_t *d, const link_frame_t *f, int err, const char *msg, const char *dkey, uint32_t dval)
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
    /* a refused step of a show: the next update is a full one (BUSY is the answer to a request during a refresh;
     * AUTH: a host that is not bonded changes nothing) */
    if (f->cmd >= USBDISP_CMD_SHOW_BEGIN && f->cmd <= USBDISP_CMD_SHOW_END && err != USBDISP_E_BUSY &&
        err != USBDISP_E_AUTH)
        usbdisp_prev_drop(d, err == USBDISP_E_IO ? "failed" : "refused");
    usbdisp_send(d, f, &w);
}

/* The depths a partial refresh takes now: the board's, less what the panel does not take at the moment (partial_now). */
uint8_t usbdisp_pdepths(const usbdisp_t *d)
{
    return d->c.partial_now ? (uint8_t)(d->c.partial_depths & d->c.partial_now(d->c.ctx, d->c.partial_depths)) : d->c.partial_depths;
}

uint8_t usbdisp_fadepths(const usbdisp_t *d)
{
    return d->c.full_area_now && d->c.panel_area && !d->c.bwry ? (uint8_t)(d->c.depths & d->c.full_area_now(d->c.ctx, d->c.depths)) : 0;
}

/* partial_max: { "2": px, "4": px } the most pixels a partial update takes in its rectangle at each gray depth that has a limit */
static size_t pmax_n(const usbdisp_t *d)
{
    return d->c.gray_partial_max_px ? (size_t)((usbdisp_pdepths(d) >> 1 & 1) + (usbdisp_pdepths(d) >> 2 & 1)) : 0;
}

static void put_pmax(const usbdisp_t *d, cbor_w_t *w)
{
    cbor_put_str(w, "partial_max");
    cbor_put_map(w, pmax_n(d));
    static const char *const key[3] = {"1", "2", "4"};
    for (unsigned b = 1; b < 3; b++)
        if (usbdisp_pdepths(d) >> b & 1) {
            cbor_put_str(w, key[b]);
            cbor_put_uint(w, d->c.gray_partial_max_px);
        }
}

static size_t tune_n(const usbdisp_t *d)
{
    return d->c.tune_count && d->c.tune_info && d->c.tune_get && d->c.tune_set && d->c.tune_defaults ? d->c.tune_count(d->c.ctx) : 0;
}

#define USBDISP_KEY_GRAY_ROW "gray_row"
#define USBDISP_GRAY_ROWS_TEXT 256 /* 16 rows of "15:" + 9 passes + a space */

/* tuning: { name: number | word, ... } the knobs now (docs/spec/usb-display.md "Tuning") */
static void put_tuning(const usbdisp_t *d, cbor_w_t *w)
{
    size_t n = tune_n(d);
    const bool rows = d->c.gray_rows_set && d->c.gray_rows_get;
    cbor_put_str(w, "tuning");
    cbor_put_map(w, n + (rows ? 1 : 0));
    for (size_t i = 0; i < n; i++) {
        const hal_tune_t *t = d->c.tune_info(d->c.ctx, i);
        int v = d->c.tune_get(d->c.ctx, i);
        cbor_put_str(w, t->name);
        if (t->words)
            cbor_put_str(w, t->words[v - t->min]);
        else
            cbor_put_uint(w, (uint64_t)v);
    }
    if (rows) { /* the gray rows that differ from the table in use, as the text the key takes */
        static char txt[USBDISP_GRAY_ROWS_TEXT];
        txt[0] = 0;
        d->c.gray_rows_get(d->c.ctx, txt, sizeof txt);
        cbor_put_str(w, USBDISP_KEY_GRAY_ROW);
        cbor_put_str(w, txt);
    }
}

static void hello(usbdisp_t *d, const link_frame_t *f)
{
    static const char *const bwry[] = {"black", "white", "red", "yellow"};
    cbor_w_t w;
    cbor_w_init(&w, d->tx, sizeof d->tx);
    cbor_put_map(&w, (size_t)(13 + (d->c.bwry ? 1 : 0) + (d->c.bt_open ? 1 : 0)) + 3 + (d->fingerprint[0] != 0) + (tune_n(d) > 0) + (pmax_n(d) > 0) + (usbdisp_fadepths(d) != 0));
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
    uint8_t pd = usbdisp_pdepths(d);
    cbor_put_str(&w, "partial_depths");
    cbor_put_array(&w, (size_t)((pd & 1) + (pd >> 1 & 1) + (pd >> 2 & 1)));
    for (unsigned b = 0; b < 3; b++)
        if (pd >> b & 1)
            cbor_put_uint(&w, 1u << b);
    /* the modes the board does at some depth */
    const char *why;
    size_t modes = 0;
    for (int pass = 0; pass < 2; pass++) {
        if (pass) {
            cbor_put_str(&w, "modes");
            cbor_put_array(&w, modes);
        }
        for (usbdisp_mode_t m = USBDISP_MODE_FULL; m < USBDISP_MODE_NONE; m++)
            if (!usbdisp_mode_check(d, m, false, 0, &why)) {
                if (pass)
                    cbor_put_str(&w, usbdisp_mode_name(m));
                else
                    modes++;
            }
    }
    /* depth_modes: per depth, the modes usbdisp_mode_check lets through at that depth (the check show itself makes) */
    cbor_put_str(&w, "depth_modes");
    cbor_put_map(&w, (size_t)((d->c.depths & 1) + (d->c.depths >> 1 & 1) + (d->c.depths >> 2 & 1)));
    for (unsigned b = 0; b < 3; b++) {
        if (!(d->c.depths >> b & 1))
            continue;
        size_t cnt = 0;
        for (usbdisp_mode_t m = USBDISP_MODE_FULL; m < USBDISP_MODE_NONE; m++)
            cnt += !usbdisp_mode_check(d, m, false, (uint8_t)(1u << b), &why);
        cbor_put_str(&w, b == 0 ? "1" : b == 1 ? "2" : "4"); /* text keys: the profile has no other */
        cbor_put_array(&w, cnt);
        for (usbdisp_mode_t m = USBDISP_MODE_FULL; m < USBDISP_MODE_NONE; m++)
            if (!usbdisp_mode_check(d, m, false, (uint8_t)(1u << b), &why))
                cbor_put_str(&w, usbdisp_mode_name(m));
    }
    cbor_put_str(&w, "full_every");
    cbor_put_uint(&w, d->full_every);
    cbor_put_str(&w, "full_every_set");
    cbor_put_bool(&w, d->full_every_set);
    cbor_put_str(&w, "fingerprint_hash");
    cbor_put_str(&w, USBDISP_FINGERPRINT_HASH);
    if (d->fingerprint[0]) {
        cbor_put_str(&w, "fingerprint");
        cbor_put_str(&w, d->fingerprint);
    }
    if (d->c.bt_open) {
        cbor_put_str(&w, "bt");
        cbor_put_str(&w, d->bt_on ? "on" : "off");
    }
    if (tune_n(d))
        put_tuning(d, &w);
    if (pmax_n(d))
        put_pmax(d, &w);
    const uint8_t fa = usbdisp_fadepths(d);
    if (fa) { /* full_area: [depths] a rectangle's full refresh is on offer at */
        cbor_put_str(&w, "full_area");
        cbor_put_array(&w, (size_t)((fa & 1) + (fa >> 1 & 1) + (fa >> 2 & 1)));
        for (unsigned b = 0; b < 3; b++)
            if (fa >> b & 1)
                cbor_put_uint(&w, 1u << b);
    }
    usbdisp_send(d, f, &w);
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
    usbdisp_send(d, f, &w);
}

/* ------------------------------------------------------------------ requests */

bool usbdisp_get_uint(const link_frame_t *f, const char *key, uint64_t *out)
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
    const char *why = "";
    if (d->png) { /* a new transfer replaces an open one, which was cut */
        usbdisp_xfer_drop(d);
        usbdisp_prev_drop(d, "cut");
    }
    if (!usbdisp_get_uint(f, "size", &size)) {
        usbdisp_refuse(d, f, USBDISP_E_BAD_REQUEST, "show_begin needs size", NULL, 0);
        return;
    }
    int err = usbdisp_target(d, f, &why);
    if (err) {
        usbdisp_refuse(d, f, err, why, NULL, 0);
        return;
    }
    if (size > d->c.png_max) {
        snprintf(msg, sizeof msg, "the PNG has %llu bytes, at most %u are taken", (unsigned long long)size,
                 (unsigned)d->c.png_max);
        usbdisp_refuse(d, f, USBDISP_E_TOO_LARGE, msg, NULL, 0);
        return;
    }
    if (size < USBDISP_PNG_MIN) {
        usbdisp_refuse(d, f, USBDISP_E_NOT_PNG, "too short for a PNG", NULL, 0);
        return;
    }
    d->png = d->c.alloc(d->c.ctx, (size_t)size);
    if (!d->png && d->prev && !d->area && d->mode <= USBDISP_MODE_FULL) { /* a whole picture: rather a full refresh */
        usbdisp_prev_drop(d, "memory");
        d->png = d->c.alloc(d->c.ctx, (size_t)size);
    }
    if (!d->png) {
        usbdisp_refuse(d, f, USBDISP_E_NOMEM, "no memory for the PNG", NULL, 0);
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
        usbdisp_refuse(d, f, USBDISP_E_SEQUENCE, "no transfer: send show_begin", "next", 0);
        return;
    }
    if (!usbdisp_get_uint(f, "off", &off) || !cbor_map_get(f->payload, f->len, "data", 4, &v, &n) ||
        (cbor_r_init(&r, v, n), !cbor_read(&r, &it)) || it.type != CBOR_BSTR || !it.len) {
        usbdisp_refuse(d, f, USBDISP_E_BAD_REQUEST, "show_chunk needs off and data", NULL, 0);
        return;
    }
    if (off != d->got) {
        usbdisp_refuse(d, f, USBDISP_E_SEQUENCE, "not the next offset", "next", d->got);
        return;
    }
    if (it.len > d->size - d->got) {
        usbdisp_xfer_drop(d);
        usbdisp_refuse(d, f, USBDISP_E_TOO_LARGE, "more bytes than show_begin announced", NULL, 0);
        return;
    }
    if (!d->got) { /* the header at once: a wrong picture is refused before the rest is sent */
        usbdisp_pnginfo_t pi;
        const char *why = "the first chunk must hold the PNG header (29 bytes)";
        int err = it.len < USBDISP_PNG_HEAD ? USBDISP_E_BAD_REQUEST : usbdisp_head_check(d, it.ptr, it.len, &pi, &why);
        if (err) {
            usbdisp_xfer_drop(d);
            usbdisp_refuse(d, f, err, why, NULL, 0);
            return;
        }
    }
    memcpy(d->png + d->got, it.ptr, it.len);
    d->got += (uint32_t)it.len;
    answer_next(d, f, false);
}

static void show_end(usbdisp_t *d, const link_frame_t *f)
{
    if (!d->png)
        usbdisp_refuse(d, f, USBDISP_E_SEQUENCE, "no transfer: send show_begin", "next", 0);
    else if (d->got != d->size)
        usbdisp_refuse(d, f, USBDISP_E_SEQUENCE, "the PNG is not complete", "next", d->got);
    else
        usbdisp_show_end(d, f);
}

/* Why a tuning value is refused, and what is allowed, in words (<= 120 bytes). */
static void tune_range(char *out, size_t cap, const hal_tune_t *t)
{
    if (!t->words) {
        snprintf(out, cap, "allowed %d to %d", t->min, t->max);
        return;
    }
    size_t o = (size_t)snprintf(out, cap, "allowed");
    for (int v = t->min; v <= t->max && o < cap; v++)
        o += (size_t)snprintf(out + o, cap - o, "%s %s", v == t->min ? "" : ",", t->words[v - t->min]);
}

#define TUNE_REFUSED_MAX 8

/* set { tuning: { key: value, ... } | "defaults" }: every key is checked on its own; the good ones are applied (from the
 * next refresh, not stored), each refused one is answered under `refused` with why and the allowed range. */
static void set_tuning(usbdisp_t *d, const link_frame_t *f, char refused[][LINK_MSG_CAP + 24], char names[][24], int *nref)
{
    const uint8_t *p;
    size_t n;
    *nref = 0;
    if (!cbor_map_get(f->payload, f->len, "tuning", 6, &p, &n))
        return;
    cbor_r_t r;
    cbor_item_t it;
    cbor_r_init(&r, p, n);
    if (!cbor_read(&r, &it))
        return;
    const size_t count = tune_n(d);
    if (it.type != CBOR_MAP) {
        if (it.type == CBOR_TSTR && it.len == 8 && !memcmp(it.ptr, "defaults", 8) && count) {
            d->c.tune_defaults(d->c.ctx);
            return;
        }
        snprintf(names[0], sizeof names[0], "tuning");
        snprintf(refused[0], sizeof refused[0], count ? "tuning is a map of keys, or \"defaults\"" : "this board has no panel tuning");
        *nref = 1;
        return;
    }
    /* gray_row is applied after the other keys, wherever it stands in the map: a gray_table in the same request
     * switches the table first (a switch puts the rows back) */
    cbor_item_t rows = {0};
    bool have_rows = false;
    for (uint64_t k = 0; k < it.u; k++) {
        cbor_item_t key, val;
        if (!cbor_read(&r, &key) || key.type != CBOR_TSTR || !cbor_read(&r, &val))
            break;
        if (count && d->c.gray_rows_set && d->c.gray_rows_get && val.type == CBOR_TSTR && cbor_str_eq(&key, USBDISP_KEY_GRAY_ROW)) {
            rows = val;
            have_rows = true;
            continue;
        }
        if (count && d->c.gray_rows_set && val.type != CBOR_MAP && val.type != CBOR_ARRAY && cbor_str_eq(&key, USBDISP_KEY_GRAY_ROW)) {
            if (*nref < TUNE_REFUSED_MAX) {
                snprintf(names[*nref], sizeof names[0], USBDISP_KEY_GRAY_ROW);
                snprintf(refused[(*nref)++], sizeof refused[0], "the value is text, level:passes, for example 5:..DDLLDDL");
            }
            continue;
        }
        if (val.type == CBOR_MAP || val.type == CBOR_ARRAY) { /* the children are not read: the request ends here */
            if (*nref < TUNE_REFUSED_MAX) {
                snprintf(names[*nref], sizeof names[0], "%.*s", (int)(key.len < 23 ? key.len : 23), (const char *)key.ptr);
                snprintf(refused[(*nref)++], sizeof refused[0], "the value is a number or a word");
            }
            break;
        }
        char name[24];
        snprintf(name, sizeof name, "%.*s", (int)(key.len < 23 ? key.len : 23), (const char *)key.ptr);
        char why[LINK_MSG_CAP + 24], range[96];
        why[0] = 0;
        size_t idx = count;
        for (size_t i = 0; i < count; i++)
            if (!strcmp(d->c.tune_info(d->c.ctx, i)->name, name) && key.len < 24)
                idx = i;
        if (!count) {
            snprintf(why, sizeof why, "this board has no panel tuning");
        } else if (idx == count) {
            size_t o = (size_t)snprintf(why, sizeof why, "no such key; known:");
            for (size_t i = 0; i < count && o < 100; i++)
                o += (size_t)snprintf(why + o, sizeof why - o, " %s", d->c.tune_info(d->c.ctx, i)->name);
        } else {
            const hal_tune_t *t = d->c.tune_info(d->c.ctx, idx);
            int64_t v = -1;
            bool have = false;
            if (val.type == CBOR_UINT && val.u <= 1000000) { /* above every key's range: "out of range" */
                v = (int64_t)val.u, have = true;
            } else if (val.type == CBOR_TRUE || val.type == CBOR_FALSE) {
                v = val.type == CBOR_TRUE, have = t->words && t->max == 1;
            } else if (val.type == CBOR_TSTR && t->words) {
                for (int w = t->min; w <= t->max; w++)
                    if (strlen(t->words[w - t->min]) == val.len && !memcmp(t->words[w - t->min], val.ptr, val.len))
                        v = w, have = true;
            }
            tune_range(range, sizeof range, t);
            if (!have || v < t->min || v > t->max)
                snprintf(why, sizeof why, "%s; %s", have ? "out of range" : t->words ? "not one of the words" : "not a whole number", range);
            else
                d->c.tune_set(d->c.ctx, idx, (int)v);
        }
        if (why[0] && *nref < TUNE_REFUSED_MAX) {
            snprintf(names[*nref], sizeof names[0], "%s", name);
            snprintf(refused[(*nref)++], sizeof refused[0], "%s", why);
        }
    }
    if (have_rows) {
        char why[LINK_MSG_CAP];
        why[0] = 0;
        if (!d->c.gray_rows_set(d->c.ctx, (const char *)rows.ptr, rows.len, why, sizeof why) && *nref < TUNE_REFUSED_MAX) {
            snprintf(names[*nref], sizeof names[0], USBDISP_KEY_GRAY_ROW);
            snprintf(refused[(*nref)++], sizeof refused[0], "%s", why[0] ? why : "refused");
        }
    }
}

/* { full_every?: 0..250 or null (clear the stored value), tuning?: { ... } | "defaults" }: full_every is stored and answered, without the key it is only
 * answered; tuning (TRMNL X) applies from the next refresh and is not stored. The answer carries `tuning` (the knobs now)
 * where the board has them, and `refused` { key: why } for the keys not applied. */
static void set(usbdisp_t *d, const link_frame_t *f)
{
    uint64_t v;
    const uint8_t *p;
    size_t n;
    if (cbor_map_get(f->payload, f->len, "full_every", 10, &p, &n) && n == 1 && p[0] == 0xF6) {
        /* null: forget the stored value, the board's default applies again */
        if (d->c.kv_erase && d->c.kv_erase(d->c.ctx, USBDISP_KV_FULL_EVERY) < 0) {
            usbdisp_refuse(d, f, USBDISP_E_IO, "the setting could not be cleared", NULL, 0);
            return;
        }
        d->full_every = d->c.full_every;
        d->full_every_set = false;
    } else if (cbor_map_get(f->payload, f->len, "full_every", 10, &p, &n)) {
        if (!usbdisp_get_uint(f, "full_every", &v) || v > USBDISP_FULL_EVERY_MAX) {
            usbdisp_refuse(d, f, USBDISP_E_BAD_REQUEST, "full_every is 0 (never) to 250", NULL, 0);
            return;
        }
        uint8_t b = (uint8_t)v;
        if (d->c.kv_set && d->c.kv_set(d->c.ctx, USBDISP_KV_FULL_EVERY, &b, 1) < 0) {
            usbdisp_refuse(d, f, USBDISP_E_IO, "the setting could not be stored", NULL, 0);
            return;
        }
        d->full_every = b;
        d->full_every_set = true;
    }
    static char refused[TUNE_REFUSED_MAX][LINK_MSG_CAP + 24], names[TUNE_REFUSED_MAX][24];
    int nref;
    set_tuning(d, f, refused, names, &nref);
    cbor_w_t w;
    cbor_w_init(&w, d->tx, sizeof d->tx);
    cbor_put_map(&w, 2 + (tune_n(d) > 0) + (nref > 0));
    cbor_put_str(&w, "full_every");
    cbor_put_uint(&w, d->full_every);
    cbor_put_str(&w, "full_every_set");
    cbor_put_bool(&w, d->full_every_set);
    if (tune_n(d))
        put_tuning(d, &w);
    if (nref) {
        cbor_put_str(&w, "refused");
        cbor_put_map(&w, (size_t)nref);
        for (int i = 0; i < nref; i++) {
            cbor_put_str(&w, names[i]);
            cbor_put_str(&w, refused[i]);
        }
    }
    usbdisp_send(d, f, &w);
}

static void have(usbdisp_t *d, const link_frame_t *f)
{
    const uint8_t *p;
    size_t n;
    cbor_item_t value;
    cbor_r_t reader;
    bool found = cbor_map_get(f->payload, f->len, "fingerprint", 11, &p, &n);
    cbor_r_init(&reader, found ? p : f->payload, found ? n : 0);
    if (!found || !cbor_read(&reader, &value) || value.type != CBOR_TSTR || value.len != 16) {
        usbdisp_refuse(d, f, USBDISP_E_BAD_REQUEST, "fingerprint is 16 lowercase hex digits", NULL, 0);
        return;
    }
    for (size_t i = 0; i < value.len; i++)
        if (!((value.ptr[i] >= '0' && value.ptr[i] <= '9') || (value.ptr[i] >= 'a' && value.ptr[i] <= 'f'))) {
            usbdisp_refuse(d, f, USBDISP_E_BAD_REQUEST, "fingerprint is 16 lowercase hex digits", NULL, 0);
            return;
        }
    bool same = !d->png && d->fingerprint[0] && !memcmp(value.ptr, d->fingerprint, 16);
    cbor_w_t w;
    cbor_w_init(&w, d->tx, sizeof d->tx);
    cbor_put_map(&w, 1);
    cbor_put_str(&w, same ? "same" : "send");
    cbor_put_bool(&w, true);
    usbdisp_send(d, f, &w);
}

static void handle(usbdisp_t *d, const link_frame_t *in, bool busy)
{
    link_frame_t f = *in; /* seq and cmd outlive the parser's buffer (show_end feeds it again) */
    if (f.cmd < 0x01 || f.cmd > 0x3F || !f.seq)
        return; /* not a request */
    if (f.len && (f.payload[0] >> 5 != 5 || !cbor_valid(f.payload, f.len))) {
        usbdisp_refuse(d, &f, USBDISP_E_BAD_REQUEST, "the payload is not one CBOR map", NULL, 0);
        return;
    }
    if (d->via_bt && !usbdisp_bt_allowed(d, &f))
        return;
    if (f.cmd == USBDISP_CMD_HELLO) {
        hello(d, &f);
        return;
    }
    if (busy) {
        usbdisp_refuse(d, &f, USBDISP_E_BUSY, "refreshing", "retry_ms", USBDISP_RETRY_MS);
        return;
    }
    d->last_us = d->c.now_us(d->c.ctx);
    switch (f.cmd) {
    case USBDISP_CMD_SHOW_BEGIN: show_begin(d, &f); break;
    case USBDISP_CMD_SHOW_CHUNK: show_chunk(d, &f); break;
    case USBDISP_CMD_SHOW_END: show_end(d, &f); break;
    case USBDISP_CMD_CLEAR: usbdisp_clear(d, &f); break;
    case USBDISP_CMD_SET: set(d, &f); break;
    case USBDISP_CMD_HAVE: have(d, &f); break;
    case USBDISP_CMD_BT:
        if (d->c.bt_open) {
            usbdisp_bt_cmd(d, &f);
            break;
        }
        /* fallthrough */
    default: usbdisp_refuse(d, &f, USBDISP_E_UNKNOWN_CMD, "this is the display firmware: hello, show, clear, set", NULL, 0); break;
    }
}

/* Bytes still in rx first (the hook runs inside a request of the outer pump), then the link's. */
static int pump(usbdisp_t *d, usbdisp_rx_t *l, bool bt, uint32_t timeout_ms, bool busy)
{
    int handled = 0;
    for (;;) {
        if (l->rx_pos >= l->rx_len) {
            long n = bt ? d->c.bt_read(d->c.ctx, l->rx, sizeof l->rx, timeout_ms)
                        : d->c.read(d->c.ctx, l->rx, sizeof l->rx, timeout_ms);
            if (n <= 0)
                return handled;
            l->rx_len = (size_t)n;
            l->rx_pos = 0;
            l->rx_us = d->c.now_us(d->c.ctx);
            timeout_ms = 0;
        }
        size_t used = 0;
        link_frame_t f;
        int got = link_parser_feed(&l->parser, l->rx + l->rx_pos, l->rx_len - l->rx_pos, &used, &f);
        l->rx_pos += used;
        if (got) {
            bool outer = d->via_bt; /* the hook's requests are answered on their own link, the outer one on its */
            d->via_bt = bt;
            handle(d, &f, busy);
            d->via_bt = outer;
            handled++;
            if (bt && !d->bt)
                return handled; /* the request stopped the stack: its link is gone */
        }
    }
}

/* The start of a frame whose rest never came (a host program that stopped): the next request must not be read as its
 * payload */
static void forget_half(usbdisp_rx_t *l, uint64_t now)
{
    if (l->parser.len > l->parser.consume && now - l->rx_us >= USBDISP_FRAME_IDLE_US)
        link_parser_init(&l->parser);
}

int usbdisp_poll(usbdisp_t *d, uint32_t timeout_ms)
{
    usbdisp_bt_poll(d);
    /* with a host connected over Bluetooth neither link waits long for the other */
    bool conn = d->bt && d->c.bt_connected(d->c.ctx);
    uint32_t bt_ms = timeout_ms < USBDISP_BT_RX_MS ? timeout_ms : USBDISP_BT_RX_MS;
    int n = pump(d, &d->usb, false, conn ? 0 : timeout_ms, false);
    if (d->bt)
        n += pump(d, d->bt, true, conn ? bt_ms : 0, false);
    uint64_t now = d->c.now_us(d->c.ctx);
    forget_half(&d->usb, now);
    if (d->bt)
        forget_half(d->bt, now);
    if (d->png && now - d->last_us >= USBDISP_IDLE_US) {
        usbdisp_log(d, "W show: the transfer went quiet, dropped");
        usbdisp_xfer_drop(d);
        usbdisp_prev_drop(d, "cut");
    }
    return n;
}

void usbdisp_busy(usbdisp_t *d)
{
    if (!d->in_panel)
        return;
    pump(d, &d->usb, false, 0, true);
    if (d->bt)
        pump(d, d->bt, true, 0, true);
}
