/*
 * imgdec: GIF (the first frame: LZW, global or local palette, transparency, interlaced) to gray or RGB rows.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "imgdec_priv.h"

/* GIF: the LZW dictionary (4096 entries), the palettes, the frame's row of indices (or all of them, interlaced) */
typedef struct {
    src_t *s;
    uint16_t prefix[4096];
    uint8_t suffix[4096];
    uint8_t stack[4097];
    uint8_t gpal[768], lpal[768], rgbp[768], grey[256];
    uint8_t *out, *idx;
    uint32_t W, left, fw;
    uint32_t bits;
    uint32_t nb;
    uint8_t blk;         /* bytes left in the data sub-block */
    bool blk_end, cut;   /* its terminator read; the file ended in the blocks */
    bool rgb;
} gif_t;

/* The next byte of the image data's sub-blocks, -1 at their end. */
static int gif_data(gif_t *g)
{
    if (!g->blk) {
        if (g->blk_end)
            return -1;
        int n = img_src_byte(g->s);
        if (n <= 0) {
            g->blk_end = true;
            g->cut = n < 0;
            return -1;
        }
        g->blk = (uint8_t)n;
    }
    g->blk--;
    int c = img_src_byte(g->s);
    if (c < 0)
        g->cut = g->blk_end = true;
    return c;
}

/* The rest of a block sequence (an extension's, or the image data's after its pixels) up to its terminator. */
static int gif_skip_blocks(gif_t *g)
{
    if (g->cut)
        return g->s->err ? g->s->err : IMGDEC_E_CORRUPT;
    if (g->blk_end) {
        g->blk_end = false;
        return IMGDEC_OK;
    }
    if (g->blk && !img_src_skip(g->s, g->blk))
        return g->s->err ? g->s->err : IMGDEC_E_CORRUPT;
    g->blk = 0;
    for (;;) {
        int n = img_src_byte(g->s);
        if (n < 0)
            return g->s->err ? g->s->err : IMGDEC_E_CORRUPT;
        if (!n)
            return IMGDEC_OK;
        if (!img_src_skip(g->s, (uint32_t)n))
            return g->s->err ? g->s->err : IMGDEC_E_CORRUPT;
    }
}

/* Screen row y: white, the frame's indices (fw of them from column left) over it when given. */
static int gif_row(gif_t *g, const imgdec_io_t *io, uint32_t y, const uint8_t *idx)
{
    if (g->rgb) {
        memset(g->out, 0xFF, 3u * g->W);
        for (uint32_t x = 0; idx && x < g->fw; x++)
            memcpy(g->out + 3u * (g->left + x), g->rgbp + 3u * idx[x], 3);
        return io->row8(io->ctx, (uint16_t)y, g->out, (uint16_t)g->W, 3) ? IMGDEC_E_ABORT : IMGDEC_OK;
    }
    memset(g->out, 15, g->W);
    for (uint32_t x = 0; idx && x < g->fw; x++)
        g->out[g->left + x] = g->grey[idx[x]];
    return img_emit_row(io, (uint16_t)y, g->out, (uint16_t)g->W) ? IMGDEC_E_ABORT : IMGDEC_OK;
}

/* The frame's LZW data (after its minimum code size) into rows top.. of the screen: total = fw x fh indices. */
static int gif_lzw(gif_t *g, const imgdec_io_t *io, uint32_t top, uint32_t total, bool inter)
{
    static const uint8_t START[4] = {0, 4, 2, 1}, STEP[4] = {8, 8, 4, 2};
    int mcs = img_src_byte(g->s);
    if (mcs < 1 || mcs > 8)
        return g->s->err ? g->s->err : IMGDEC_E_CORRUPT;
    uint32_t clear = 1u << mcs, eoi = clear + 1, next = clear + 2, width = (uint32_t)mcs + 1;
    uint32_t fw = g->fw, fh = total / fw, pix = 0, x = 0, r = 0, pass = 0;
    int prev = -1, rc;
    while (pix < total) {
        while (g->nb < width) {
            int c = gif_data(g);
            if (c < 0)
                return g->s->err ? g->s->err : IMGDEC_E_CORRUPT; /* pixels missing */
            g->bits |= (uint32_t)c << g->nb;
            g->nb += 8;
        }
        uint32_t code = g->bits & ((1u << width) - 1);
        g->bits >>= width;
        g->nb -= width;
        if (code == clear) {
            next = clear + 2;
            width = (uint32_t)mcs + 1;
            prev = -1;
            continue;
        }
        if (code == eoi)
            return IMGDEC_E_CORRUPT; /* pixels missing */
        uint32_t sp = 0, c;
        if (prev < 0) {
            if (code > clear)
                return IMGDEC_E_CORRUPT;
            c = code;
        } else {
            if (code > next)
                return IMGDEC_E_CORRUPT;
            bool kwk = code == next; /* the string of prev and its own first index */
            sp = kwk;
            c = kwk ? (uint32_t)prev : code;
            while (c >= clear) {
                if (sp >= 4096)
                    return IMGDEC_E_CORRUPT;
                g->stack[sp++] = g->suffix[c];
                c = g->prefix[c];
            }
            if (kwk)
                g->stack[0] = (uint8_t)c;
            if (next < 4096) {
                g->prefix[next] = (uint16_t)prev;
                g->suffix[next] = (uint8_t)c;
                if (++next == 1u << width && width < 12)
                    width++;
            }
        }
        g->stack[sp++] = (uint8_t)c;
        prev = (int)code;
        while (sp && pix < total) {
            pix++;
            if (!inter) {
                g->idx[x++] = g->stack[--sp];
                if (x == fw) {
                    if ((rc = gif_row(g, io, top + r++, g->idx)))
                        return rc;
                    x = 0;
                }
                continue;
            }
            g->idx[r * fw + x++] = g->stack[--sp];
            if (x == fw) { /* the next row of the pass, or the next pass */
                x = 0;
                r += STEP[pass];
                while (r >= fh && ++pass < 4)
                    r = START[pass];
            }
        }
    }
    for (uint32_t k = 0; inter && k < fh; k++)
        if ((rc = gif_row(g, io, top + k, g->idx + k * fw)))
            return rc;
    return IMGDEC_OK;
}

size_t img_gif_need(const imgdec_info_t *info)
{
    return sizeof(src_t) + sizeof(gif_t) + 4u * info->w + (info->whole ? (size_t)info->w * info->h : 0);
}

int img_gif_decode(src_t *s, const imgdec_io_t *io, imgdec_info_t *info)
{
    uint8_t h[13] = {'G', 'I'};
    if (!img_src_read(s, h + 2, 4) || (memcmp(h, "GIF87a", 6) && memcmp(h, "GIF89a", 6)))
        return s->err ? s->err : IMGDEC_E_FORMAT; /* the signature first: another file is not a bad GIF */
    if (!img_src_read(s, h + 6, 7))
        return s->err ? s->err : IMGDEC_E_CORRUPT;
    int rc = img_gif_header(h, info);
    if (rc)
        return rc;
    gif_t *g = al(io, sizeof *g);
    if (!g)
        return IMGDEC_E_NOMEM;
    memset(g, 0, sizeof *g);
    g->s = s;
    uint32_t W = info->w, H = info->h;
    int gct = h[10] & 0x80 ? 2 << (h[10] & 7) : 0, trans = -1;
    uint8_t d[9];
    if (gct && !img_src_read(s, g->gpal, 3u * (uint32_t)gct)) {
        rc = s->err ? s->err : IMGDEC_E_CORRUPT;
        goto out;
    }
    for (;;) { /* extensions up to the first image */
        int c = img_src_byte(s);
        if (c == 0x2C)
            break;
        if (c != 0x21) { /* the trailer, garbage or the end: no image */
            rc = s->err ? s->err : IMGDEC_E_CORRUPT;
            goto out;
        }
        int label = img_src_byte(s);
        if (label == 0xF9) { /* graphic control: the transparent index */
            if (img_src_byte(s) != 4 || !img_src_read(s, d, 4)) {
                rc = s->err ? s->err : IMGDEC_E_CORRUPT;
                goto out;
            }
            trans = d[0] & 1 ? d[3] : -1;
        } else if (label < 0) {
            rc = s->err ? s->err : IMGDEC_E_CORRUPT;
            goto out;
        }
        if ((rc = gif_skip_blocks(g)))
            goto out;
    }
    if (!img_src_read(s, d, 9)) {
        rc = s->err ? s->err : IMGDEC_E_CORRUPT;
        goto out;
    }
    uint32_t left = le16(d), top = le16(d + 2), fw = le16(d + 4), fh = le16(d + 6);
    if (!fw || !fh || left + fw > W || top + fh > H) { /* the frame within the logical screen */
        rc = IMGDEC_E_CORRUPT;
        goto out;
    }
    int n = gct;
    const uint8_t *pal = g->gpal;
    if (d[8] & 0x80) { /* a local palette */
        n = 2 << (d[8] & 7);
        if (!img_src_read(s, g->lpal, 3u * (uint32_t)n)) {
            rc = s->err ? s->err : IMGDEC_E_CORRUPT;
            goto out;
        }
        pal = g->lpal;
    }
    info->whole = (d[8] & 0x40) != 0;
    if (io->info && io->info(io->ctx, info)) {
        rc = IMGDEC_E_ABORT;
        goto out;
    }
    g->rgb = io->row8 && io->want_rgb;
    for (int i = 0; i < 256; i++) { /* white past the palette and for the transparent index */
        const uint8_t *e = pal + 3 * i;
        bool white = i >= n || i == trans;
        g->grey[i] = white ? 15 : imgdec_gray_rgb(e[0], e[1], e[2]);
        if (g->rgb)
            memcpy(g->rgbp + 3 * i, white ? (const uint8_t *)"\xFF\xFF\xFF" : e, 3);
    }
    g->W = W;
    g->left = left;
    g->fw = fw;
    g->out = al(io, g->rgb ? 3u * W : W);
    g->idx = al(io, info->whole ? fw * fh : fw);
    if (!g->out || !g->idx) {
        rc = IMGDEC_E_NOMEM;
        goto out;
    }
    uint32_t y = 0;
    for (; y < top && !rc; y++)
        rc = gif_row(g, io, y, NULL);
    if (!rc)
        rc = gif_lzw(g, io, top, fw * fh, info->whole);
    for (y = top + fh; y < H && !rc; y++)
        rc = gif_row(g, io, y, NULL);
    if (!rc) /* the frame's blocks end, and the file goes on (a cut file fails) */
        rc = gif_skip_blocks(g);
    if (!rc && img_src_byte(s) < 0)
        rc = s->err ? s->err : IMGDEC_E_CORRUPT;
out:
    fr(io, g->out);
    fr(io, g->idx);
    fr(io, g);
    return rc;
}
