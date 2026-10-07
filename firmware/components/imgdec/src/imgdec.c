/*
 * imgdec: PNG (zlib inflate, filters, all non-interlaced color types), BMP (1/4/8/16/24/32 bpp, uncompressed) and
 * GIF (the first frame: LZW, global or local palette, transparency, interlaced) to gray rows; baseline JPEG in imgjpg.c. Pull model over io.read; see imgdec.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "tiny/busy.h"
#include <string.h>

#include "imgdec_priv.h"
#include "imgjpg.h"
#include "imgwebp.h"

int img_src_byte(src_t *s)
{
    if (s->in_p >= s->in_n) {
        if (s->eof)
            return -1;
        long n = s->io->read(s->io->ctx, s->in, sizeof s->in);
        if (n < 0) {
            s->err = IMGDEC_E_IO;
            return -1;
        }
        if (n == 0) {
            s->eof = true;
            return -1;
        }
        s->in_n = (size_t)n;
        s->in_p = 0;
        s->total += (uint32_t)n;
    }
    return s->in[s->in_p++];
}

bool img_src_read(src_t *s, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int c = img_src_byte(s);
        if (c < 0)
            return false;
        out[i] = (uint8_t)c;
    }
    return true;
}

bool img_src_skip(src_t *s, uint32_t n)
{
    while (n--)
        if (img_src_byte(s) < 0)
            return false;
    return true;
}

/* A gray row (levels 0..15) to io.row, or x 17 to io.row8 (the buffer is the decoder's own, rewritten per row). */
int img_emit_row(const imgdec_io_t *io, uint16_t y, uint8_t *gray, uint16_t w)
{
    if (!io->row8)
        return io->row(io->ctx, y, gray, w);
    for (uint16_t i = 0; i < w; i++)
        gray[i] = (uint8_t)(gray[i] * 17u);
    return io->row8(io->ctx, y, gray, w, 1);
}

/* ------------------------------------------------------------------ probe */

static int png_header(const uint8_t *ih, imgdec_info_t *out)
{
    uint32_t w = be32(ih), h = be32(ih + 4);
    uint8_t bd = ih[8], ct = ih[9];
    if (ih[10] != 0 || ih[11] != 0)
        return IMGDEC_E_FORMAT;
    if (ih[12] != 0)
        return IMGDEC_E_FORMAT; /* interlaced (Adam7): not taken */
    static const uint8_t chans[7] = {1, 0, 3, 1, 2, 0, 4};
    bool ok = ct <= 6 && chans[ct] &&
              (ct == 0   ? (bd == 1 || bd == 2 || bd == 4 || bd == 8 || bd == 16)
               : ct == 3 ? (bd == 1 || bd == 2 || bd == 4 || bd == 8)
                         : (bd == 8 || bd == 16));
    if (!ok)
        return IMGDEC_E_FORMAT;
    if (w == 0 || h == 0)
        return IMGDEC_E_CORRUPT;
    if (w > IMGDEC_MAX_W || h > IMGDEC_MAX_H)
        return IMGDEC_E_SIZE;
    out->format = IMG_PNG;
    out->w = (uint16_t)w;
    out->h = (uint16_t)h;
    out->bits = (uint8_t)(bd * chans[ct]);
    out->gray_bits = ct == 0 && bd <= 4 ? bd : 8;
    out->color = ct == 2 || ct == 3 || ct == 6;
    out->orient = 1;
    out->rw = out->w;
    out->rh = out->h;
    return IMGDEC_OK;
}

int img_bmp_parse(const uint8_t *b, size_t n, bmp_hdr_t *hd, imgdec_info_t *out)
{
    if (n < 26)
        return IMGDEC_E_CORRUPT;
    if (b[0] != 'B' || b[1] != 'M')
        return IMGDEC_E_FORMAT;
    hd->off_bits = le32(b + 10);
    hd->dib = le32(b + 14);
    if (hd->dib == 12) { /* OS/2 BITMAPCOREHEADER */
        hd->w = le16(b + 18);
        hd->h = (int16_t)le16(b + 20);
        hd->bpp = le16(b + 24);
        hd->comp = 0;
        hd->colors = 0;
    } else if (hd->dib >= 40) {
        if (n < 50)
            return IMGDEC_E_CORRUPT;
        hd->w = le32(b + 18);
        hd->h = (int32_t)le32(b + 22);
        hd->bpp = le16(b + 28);
        hd->comp = le32(b + 30);
        hd->colors = le32(b + 46);
    } else {
        return IMGDEC_E_FORMAT;
    }
    uint32_t ah = hd->h < 0 ? (uint32_t)(-(int64_t)hd->h) : (uint32_t)hd->h;
    bool bits_ok = hd->bpp == 1 || hd->bpp == 4 || hd->bpp == 8 || hd->bpp == 16 || hd->bpp == 24 || hd->bpp == 32;
    if (!bits_ok || !(hd->comp == 0 || (hd->comp == 3 && (hd->bpp == 16 || hd->bpp == 32))))
        return IMGDEC_E_FORMAT; /* RLE and JPEG/PNG-in-BMP: not taken */
    if (hd->w == 0 || ah == 0)
        return IMGDEC_E_CORRUPT;
    if (hd->w > IMGDEC_MAX_W || ah > IMGDEC_MAX_H)
        return IMGDEC_E_SIZE;
    out->format = IMG_BMP;
    out->w = (uint16_t)hd->w;
    out->h = (uint16_t)ah;
    out->bits = (uint8_t)hd->bpp;
    out->gray_bits = 8;
    out->color = 1; /* palette or 16..32 bit */
    out->orient = 1;
    out->rw = out->w;
    out->rh = out->h;
    return IMGDEC_OK;
}

/* GIF: signature and logical screen (the first 10 bytes). */
int img_gif_header(const uint8_t *b, imgdec_info_t *out)
{
    if (memcmp(b, "GIF87a", 6) && memcmp(b, "GIF89a", 6))
        return IMGDEC_E_FORMAT;
    uint16_t w = le16(b + 6), h = le16(b + 8);
    if (!w || !h)
        return IMGDEC_E_CORRUPT;
    if (w > IMGDEC_MAX_W || h > IMGDEC_MAX_H)
        return IMGDEC_E_SIZE;
    out->format = IMG_GIF;
    out->w = out->rw = w;
    out->h = out->rh = h;
    out->bits = 8;
    out->gray_bits = 8;
    out->color = 1; /* a palette */
    out->orient = 1;
    return IMGDEC_OK;
}

int imgdec_probe(const uint8_t *b, size_t n, imgdec_info_t *out)
{
    memset(out, 0, sizeof *out);
    if (n && b[0] == 'G') {
        if (n < 10)
            return memcmp(b, "GIF8", n < 4 ? n : 4) ? IMGDEC_E_FORMAT : IMGDEC_E_CORRUPT;
        return img_gif_header(b, out);
    }
    if (n && b[0] == 0xFF) {
        if (n < 3)
            return n < 2 || b[1] == 0xD8 ? IMGDEC_E_CORRUPT : IMGDEC_E_FORMAT;
        return b[1] == 0xD8 && b[2] == 0xFF ? imgjpg_probe(b, n, out) : IMGDEC_E_FORMAT;
    }
    if (n >= 12 && !memcmp(b + 4, "ftyp", 4)) {
        out->format = IMG_HEIF;
        return IMGDEC_E_FORMAT;
    }
    if (n && b[0] == 'R') {
        int rc = imgwebp_probe(b, n, out);
#if !IMGDEC_WEBP
        if (rc == IMGDEC_OK)
            return IMGDEC_E_FORMAT; /* header sizes remain available to the relay */
#endif
        return rc;
    }
    if (n >= 2 && b[0] == 'B' && b[1] == 'M') {
        bmp_hdr_t hd;
        int rc = img_bmp_parse(b, n, &hd, out);
        if (rc == IMGDEC_E_CORRUPT && n < 50)
            return IMGDEC_E_CORRUPT;
        return rc;
    }
    if (n < 8)
        return n && memcmp(b, PNG_SIG, n) ? IMGDEC_E_FORMAT : IMGDEC_E_CORRUPT;
    if (memcmp(b, PNG_SIG, 8))
        return IMGDEC_E_FORMAT;
    if (n < 29)
        return IMGDEC_E_CORRUPT;
    if (be32(b + 8) != 13 || memcmp(b + 12, "IHDR", 4))
        return IMGDEC_E_FORMAT;
    return png_header(b + 16, out);
}

size_t imgdec_need(const imgdec_info_t *info, uint32_t window)
{
    if (info->format == IMG_JPEG)
        return imgjpg_need(info);
    if (info->format == IMG_WEBP) {
#if IMGDEC_WEBP
        return imgwebp_need(info);
#else
        return 0;
#endif
    }
    if (info->format == IMG_GIF) /* the state, a row of indices and one out (RGB), the frame when interlaced */
        return img_gif_need(info);
    if (info->format == IMG_BMP) {
        uint32_t row = (((uint32_t)info->w * info->bits + 31) / 32) * 4;
        return row + info->w + sizeof(src_t) + 1024;
    }
    return img_png_need(info, window);
}




__attribute__((weak)) void sim_cpu_ms(double ms);
__attribute__((weak)) double sim_prof_ms(const char *name);

TINY_BUSY_WRAP(int, imgdec_decode, (const imgdec_io_t *io, imgdec_info_t *info), (io, info), "decode")
{
    if (sim_cpu_ms && sim_prof_ms)
        sim_cpu_ms(sim_prof_ms("decode_ms")); /* the simulator's decode time (0 on a device and in host tests) */
    imgdec_info_t tmp;
    if (!info)
        info = &tmp;
    memset(info, 0, sizeof *info);
    src_t *s = io->alloc(io->ctx, sizeof *s);
    if (!s)
        return IMGDEC_E_NOMEM;
    memset(s, 0, sizeof *s);
    s->io = io;
    uint8_t sig[8];
    int rc;
    if (!img_src_read(s, sig, 2)) {
        rc = s->err ? s->err : IMGDEC_E_FORMAT;
    } else if (sig[0] == 0xFF && sig[1] == 0xD8) { /* the bytes read ahead handed over: the JPEG decoder is all */
        uint8_t pre[IN_BUF];
        size_t pn = s->in_n - s->in_p;
        uint32_t at = s->total - (uint32_t)pn;
        memcpy(pre, s->in + s->in_p, pn);
        io->free(io->ctx, s);
        return imgjpg_decode(io, info, pre, pn, at);
    } else if (sig[0] == 'B' && sig[1] == 'M') {
        rc = img_bmp_decode(s, io, info);
    } else if (sig[0] == 'R' && sig[1] == 'I') { /* WebP: the bytes read ahead handed over, as for JPEG */
#if IMGDEC_WEBP
        uint8_t pre[IN_BUF];
        size_t pn = s->in_n - s->in_p;
        uint32_t at = s->total - (uint32_t)pn;
        memcpy(pre, s->in + s->in_p, pn);
        io->free(io->ctx, s);
        return imgwebp_decode(io, info, pre, pn, at);
#else
        info->format = IMG_WEBP;
        rc = IMGDEC_E_FORMAT;
#endif
    } else if (sig[0] == 'G' && sig[1] == 'I') {
        rc = img_gif_decode(s, io, info);
    } else if (!img_src_read(s, sig + 2, 6) || memcmp(sig, PNG_SIG, 8)) {
        rc = s->err ? s->err : IMGDEC_E_FORMAT;
    } else {
        rc = img_png_decode(s, io, info);
    }
    io->free(io->ctx, s);
    return rc;
}
