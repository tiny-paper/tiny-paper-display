/* usbdisp: a PNG of the panel's size into a packed frame, through imgdec (docs/spec/usb-display.md "The picture").
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>

#include "imgdec.h"
#include "usbdisp_priv.h"

static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

int usbdisp_png_check(const usbdisp_cfg_t *c, const uint8_t *png, size_t n, bool area, usbdisp_pnginfo_t *out,
                      const char **why)
{
    static char msg[LINK_MSG_CAP]; /* one request at a time */
    imgdec_info_t in;
    memset(&in, 0, sizeof in);
    *why = "not a PNG";
    if (n < USBDISP_PNG_HEAD || memcmp(png, SIG, sizeof SIG))
        return USBDISP_E_NOT_PNG;
    int rc = imgdec_probe(png, USBDISP_PNG_HEAD, &in);
    if (rc == IMGDEC_E_SIZE) {
        *why = "the PNG is larger than any panel";
        return USBDISP_E_WRONG_SIZE;
    }
    if (rc == IMGDEC_E_CORRUPT || (rc == IMGDEC_OK && in.format != IMG_PNG)) {
        *why = "the PNG header is damaged";
        return USBDISP_E_BAD_PNG;
    }
    if (rc != IMGDEC_OK) { /* a PNG signature with a header imgdec does not take: no IHDR first, interlaced */
        *why = "an interlaced PNG, or one without a header, is not taken";
        return USBDISP_E_DEPTH;
    }
    out->w = (uint16_t)in.w;
    out->h = (uint16_t)in.h;
    if (!area && (in.w != c->w || in.h != c->h)) {
        snprintf(msg, sizeof msg, "the PNG is %ux%u, the panel is %ux%u: send exactly the panel's size", (unsigned)in.w,
                 (unsigned)in.h, (unsigned)c->w, (unsigned)c->h);
        *why = msg;
        return USBDISP_E_WRONG_SIZE;
    }
    /* gray of 1, 2 or 4 bits (color type 0; in.bits is depth x channels, so gray with alpha is 16 or more) */
    bool gray = !in.color && (in.bits == 1 || in.bits == 2 || in.bits == 4), indexed = in.color && in.bits <= 8;
    if (c->bwry) {
        if (indexed || (gray && in.bits == 1)) {
            out->depth = 2;
            out->indexed = indexed;
            return 0;
        }
        *why = "this panel takes a 1-bit gray PNG or an indexed PNG of black, white, red and yellow";
        return USBDISP_E_DEPTH;
    }
    if (gray && (c->depths & in.bits)) { /* bits 1, 2, 4 are the mask bits of depths */
        out->depth = in.bits;
        out->indexed = false;
        return 0;
    }
    snprintf(msg, sizeof msg, "this panel takes a gray PNG of %s%s%s bits a pixel, no color, alpha or 8 bit",
             c->depths & 1 ? "1" : "", c->depths & 2 ? ", 2" : "", c->depths & 4 ? ", 4" : "");
    *why = msg;
    return USBDISP_E_DEPTH;
}

typedef struct {
    const usbdisp_cfg_t *c;
    const usbdisp_pnginfo_t *pi;
    const uint8_t *png;
    size_t n, pos;
    uint8_t *frame;
    size_t stride;
    uint32_t rows;
    int err;
    const char *why;
} dec_t;

static long dec_read(void *ctx, uint8_t *buf, size_t cap)
{
    dec_t *d = ctx;
    size_t k = d->n - d->pos < cap ? d->n - d->pos : cap;
    memcpy(buf, d->png + d->pos, k);
    d->pos += k;
    return (long)k;
}

static void *dec_alloc(void *ctx, size_t n) { return ((dec_t *)ctx)->c->alloc(((dec_t *)ctx)->c->ctx, n); }
static void dec_free(void *ctx, void *p) { ((dec_t *)ctx)->c->free(((dec_t *)ctx)->c->ctx, p); }

/* The header again, as the decoder reads it: the checks of usbdisp_png_check hold for the bytes decoded. */
static int dec_info(void *ctx, const imgdec_info_t *in)
{
    dec_t *d = ctx;
    return in->format != IMG_PNG || in->w != d->pi->w || in->h != d->pi->h;
}

/* Palette code of an exact panel color (assets.md "Palette frames": 0 black, 1 red, 2 yellow, 3 white), -1 other. */
static int bwry_code(const uint8_t *p)
{
    if (p[2] != 0 && !(p[0] == 255 && p[1] == 255 && p[2] == 255))
        return -1;
    if (p[0] == 0 && p[1] == 0)
        return 0;
    if (p[0] == 255 && p[1] == 0)
        return 1;
    if (p[0] == 255 && p[1] == 255)
        return p[2] ? 3 : 2;
    return -1;
}

static int dec_row8(void *ctx, uint16_t y, const uint8_t *px, uint16_t w, uint8_t ch)
{
    dec_t *d = ctx;
    uint8_t depth = d->pi->depth;
    if (y >= d->pi->h || w != d->pi->w || (ch == 3) != d->pi->indexed)
        return 1;
    uint8_t *row = d->frame + (size_t)y * d->stride;
    memset(row, 0, ((size_t)w * depth + 7) / 8);
    for (uint16_t x = 0; x < w; x++) {
        int v;
        if (ch == 3) {
            v = bwry_code(px + 3u * x);
            if (v < 0) {
                d->err = USBDISP_E_DEPTH;
                d->why = "a color that is not black, white, red (ff0000) or yellow (ffff00)";
                return 1;
            }
        } else if (d->c->bwry) {
            v = px[x] ? 3 : 0; /* 1-bit gray on the color panel */
        } else {
            v = px[x] * ((1 << depth) - 1) / 255; /* the PNG's own level: its sample scaled to 0..255, exactly */
        }
        unsigned bit = (unsigned)x * depth;
        row[bit / 8] |= (uint8_t)(v << (8 - depth - bit % 8));
    }
    d->rows++;
    return 0;
}

int usbdisp_png_frame(const usbdisp_cfg_t *c, const uint8_t *png, size_t n, const usbdisp_pnginfo_t *pi, uint8_t *frame,
                      size_t stride, const char **why)
{
    dec_t d = {.c = c, .pi = pi, .png = png, .n = n, .frame = frame, .stride = stride};
    imgdec_io_t io = {.read = dec_read, .info = dec_info, .row8 = dec_row8, .alloc = dec_alloc, .free = dec_free,
                      .ctx = &d, .want_rgb = true};
    imgdec_info_t in;
    int rc = imgdec_decode(&io, &in);
    if (d.err) {
        *why = d.why;
        return d.err;
    }
    if (rc == IMGDEC_E_NOMEM) {
        *why = "no memory to decode the PNG";
        return USBDISP_E_NOMEM;
    }
    if (rc != IMGDEC_OK || d.rows != pi->h) {
        *why = "the PNG data is damaged or cut short";
        return USBDISP_E_BAD_PNG;
    }
    return 0;
}
