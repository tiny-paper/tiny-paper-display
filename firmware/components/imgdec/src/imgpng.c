/*
 * imgdec: PNG rows: scanline filters, all non-interlaced color types to gray or RGB rows; the zlib stream is in imginflate.c.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "imginflate.h"

/* ------------------------------------------------------------------ PNG rows */

typedef struct {
    const imgdec_io_t *io;
    imgdec_info_t info;
    uint8_t ct, bd, bpp;        /* color type, bit depth, bytes per complete pixel (>= 1) */
    uint32_t stride;            /* bytes per scanline without the filter byte */
    uint8_t *cur, *prev, *gray; /* cur/prev: stride + 1 bytes (filter byte first) */
    uint32_t pos;               /* bytes of the current scanline received */
    uint16_t y;
    uint8_t pal[256];           /* palette as gray, alpha applied over white */
    uint8_t rgb[768];           /* palette colors (over white from the first IDAT) */
    uint16_t pal_n;
    bool done;
    uint8_t *rgbrow;            /* RGB rows (row8 with want_rgb, a color PNG): 3 w bytes, else NULL */
} png_t;

static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c)
{
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

static uint8_t over_white(uint8_t c, uint8_t a) { return (uint8_t)((c * a + 255u * (255u - a) + 127u) / 255u); }

static int png_row(png_t *g)
{
    uint8_t *c = g->cur + 1, *p = g->prev + 1, f = g->cur[0];
    uint32_t bpp = g->bpp;
    for (uint32_t i = 0; i < g->stride; i++) {
        uint8_t a = i >= bpp ? c[i - bpp] : 0, b = p[i], d = i >= bpp ? p[i - bpp] : 0;
        switch (f) {
        case 0: break;
        case 1: c[i] = (uint8_t)(c[i] + a); break;
        case 2: c[i] = (uint8_t)(c[i] + b); break;
        case 3: c[i] = (uint8_t)(c[i] + ((a + b) >> 1)); break;
        case 4: c[i] = (uint8_t)(c[i] + paeth(a, b, d)); break;
        default: return IMGDEC_E_CORRUPT;
        }
    }
    uint16_t w = g->info.w;
    uint8_t bd = g->bd;
    for (uint32_t x = 0; x < w; x++) {
        uint8_t v;
        if (g->rgbrow && g->ct == 3) { /* palette index: its color over white */
            uint32_t bit = x * bd, s = bd < 8 ? (uint32_t)(c[bit >> 3] >> (8 - bd - (bit & 7))) & ((1u << bd) - 1) : c[x];
            if (s < g->pal_n)
                memcpy(g->rgbrow + 3 * x, g->rgb + 3 * s, 3);
            else
                memset(g->rgbrow + 3 * x, 0, 3);
            continue;
        }
        if (g->ct == 0 || g->ct == 3) {
            uint32_t s;
            if (bd < 8) {
                uint32_t bit = x * bd;
                s = (uint32_t)(c[bit >> 3] >> (8 - bd - (bit & 7))) & ((1u << bd) - 1);
            } else {
                s = c[x * (bd / 8)];
            }
            if (g->ct == 3)
                v = s < g->pal_n ? g->pal[s] : 0;
            else
                v = bd < 8 ? (uint8_t)(s * 15 / ((1u << bd) - 1)) : (uint8_t)(s >> 4);
        } else {
            uint32_t k = bd / 8, o = x * g->bpp; /* high byte of 16-bit samples */
            uint8_t r, gg, b, al = 255;
            if (g->ct == 4) {
                r = gg = b = c[o];
                al = c[o + k];
            } else {
                r = c[o];
                gg = c[o + k];
                b = c[o + 2 * k];
                if (g->ct == 6)
                    al = c[o + 3 * k];
            }
            if (al != 255) {
                r = over_white(r, al);
                gg = over_white(gg, al);
                b = over_white(b, al);
            }
            if (g->rgbrow) {
                g->rgbrow[3 * x] = r;
                g->rgbrow[3 * x + 1] = gg;
                g->rgbrow[3 * x + 2] = b;
                continue;
            }
            v = imgdec_gray_rgb(r, gg, b);
        }
        g->gray[x] = v;
    }
    if (g->rgbrow ? g->io->row8(g->io->ctx, g->y, g->rgbrow, w, 3) : img_emit_row(g->io, g->y, g->gray, w))
        return IMGDEC_E_ABORT;
    uint8_t *t = g->prev;
    g->prev = g->cur;
    g->cur = t;
    g->y++;
    if (g->y == g->info.h)
        g->done = true;
    return IMGDEC_OK;
}

static int png_emit(void *ctx, uint8_t b)
{
    png_t *g = ctx;
    if (g->done)
        return IMGDEC_E_CORRUPT; /* more data than the image holds */
    g->cur[g->pos++] = b;
    if (g->pos == g->stride + 1) {
        g->pos = 0;
        return png_row(g);
    }
    return IMGDEC_OK;
}

static uint32_t png_stride(const imgdec_info_t *in) { return ((uint32_t)in->w * in->bits + 7) / 8; }

static uint32_t pow2_at_least(uint32_t n)
{
    uint32_t p = 256;
    while (p < n && p < 32768)
        p <<= 1;
    return p;
}

size_t img_png_need(const imgdec_info_t *info, uint32_t window)
{
    uint32_t stride = png_stride(info);
    uint64_t total = (uint64_t)(stride + 1) * info->h;
    uint32_t w = pow2_at_least(total < window ? (uint32_t)total : window);
    return (size_t)w + 2 * (stride + 1) + info->w + sizeof(infl_t) + sizeof(png_t) + sizeof(src_t);
}

int img_png_decode(src_t *s, const imgdec_io_t *io, imgdec_info_t *info)
{
    uint8_t h[29]; /* signature, IHDR length, type and data (its CRC is read with the next chunk header) */
    memcpy(h, PNG_SIG, 8);
    if (!img_src_read(s, h + 8, 21))
        return s->err ? s->err : IMGDEC_E_CORRUPT;
    int rc = imgdec_probe(h, sizeof h, info);
    if (rc)
        return rc;
    if (io->info && io->info(io->ctx, info))
        return IMGDEC_E_ABORT;
    png_t *g = al(io, sizeof *g);
    infl_t *z = al(io, sizeof *z);
    if (!g || !z) {
        fr(io, g);
        fr(io, z);
        return IMGDEC_E_NOMEM;
    }
    memset(g, 0, sizeof *g);
    memset(z, 0, sizeof *z);
    g->io = io;
    g->info = *info;
    g->ct = h[25];
    g->bd = h[24];
    g->stride = png_stride(info);
    g->bpp = (uint8_t)((info->bits + 7) / 8);
    rc = IMGDEC_OK;
    bool trns = false;
    uint8_t alpha[256];
    memset(alpha, 255, sizeof alpha);
    /* chunks up to the first IDAT: PLTE, tRNS; others skipped */
    for (;;) {
        uint8_t ch[8];
        if (!img_src_read(s, ch, 4) || !img_src_read(s, ch, 8)) { /* CRC of the chunk before, then the next header */
            rc = s->err ? s->err : IMGDEC_E_CORRUPT;
            goto out;
        }
        uint32_t len = be32(ch);
        if (!memcmp(ch + 4, "IDAT", 4)) {
            z->chunk_left = len;
            break;
        }
        if (!memcmp(ch + 4, "IEND", 4)) {
            rc = IMGDEC_E_CORRUPT;
            goto out;
        }
        if (!memcmp(ch + 4, "PLTE", 4) && len % 3 == 0 && len <= 768) {
            g->pal_n = (uint16_t)(len / 3);
            if (!img_src_read(s, g->rgb, len)) {
                rc = IMGDEC_E_CORRUPT;
                goto out;
            }
            continue;
        }
        if (!memcmp(ch + 4, "tRNS", 4) && g->ct == 3 && len <= 256) {
            if (!img_src_read(s, alpha, len)) {
                rc = IMGDEC_E_CORRUPT;
                goto out;
            }
            trns = true;
            continue;
        }
        if (!img_src_skip(s, len)) {
            rc = IMGDEC_E_CORRUPT;
            goto out;
        }
    }
    if (g->ct == 3 && !g->pal_n) {
        rc = IMGDEC_E_CORRUPT;
        goto out;
    }
    for (uint32_t i = 0; i < g->pal_n; i++) { /* palette alpha (tRNS) over white, then the gray */
        uint8_t *c = g->rgb + 3 * i;
        uint8_t a = trns ? alpha[i] : 255;
        for (int k = 0; k < 3; k++)
            c[k] = over_white(c[k], a);
        g->pal[i] = imgdec_gray_rgb(c[0], c[1], c[2]);
    }
    z->s = s;
    /* zlib header */
    int cmf = img_idat_byte(z), flg = img_idat_byte(z);
    if (cmf < 0 || flg < 0 || (cmf & 15) != 8 || ((cmf << 8) | flg) % 31 || (flg & 0x20)) {
        rc = IMGDEC_E_CORRUPT;
        goto out;
    }
    uint32_t wmax = io->window_max ? io->window_max : 32768;
    uint32_t zw = 1u << ((cmf >> 4) + 8);
    uint64_t total = (uint64_t)(g->stride + 1) * info->h;
    uint32_t want = zw < total ? zw : (uint32_t)total;
    if (want > 32768)
        want = 32768;
    z->wsize = pow2_at_least(want < wmax ? want : wmax);
    if (z->wsize > 32768)
        z->wsize = 32768;
    z->win = al(io, z->wsize);
    g->cur = al(io, g->stride + 1);
    g->prev = al(io, g->stride + 1);
    g->gray = al(io, info->w);
    if (io->row8 && io->want_rgb && info->color && g->gray)
        g->rgbrow = al(io, 3u * info->w);
    if (!z->win || !g->cur || !g->prev || !g->gray || (io->row8 && io->want_rgb && info->color && !g->rgbrow)) {
        rc = IMGDEC_E_NOMEM;
        goto out;
    }
    memset(g->prev, 0, g->stride + 1);
    rc = img_inflate_all(z, png_emit, g);
    if (!rc && (!g->done || g->pos))
        rc = IMGDEC_E_CORRUPT; /* truncated */
out:
    if (s->err && rc == IMGDEC_E_CORRUPT)
        rc = s->err;
    fr(io, z->win);
    fr(io, g->cur);
    fr(io, g->prev);
    fr(io, g->gray);
    fr(io, g->rgbrow);
    fr(io, z);
    fr(io, g);
    return rc;
}
