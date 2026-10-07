/*
 * imgdec: BMP (1/4/8/16/24/32 bpp, uncompressed) to gray or RGB rows.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "imgdec_priv.h"

int img_bmp_decode(src_t *s, const imgdec_io_t *io, imgdec_info_t *info)
{
    uint8_t h[54];
    h[0] = 'B';
    h[1] = 'M';
    if (!img_src_read(s, h + 2, 16))
        return s->err ? s->err : IMGDEC_E_CORRUPT;
    uint32_t dib = le32(h + 14);
    size_t got = 18;
    if (dib >= 40) {
        if (!img_src_read(s, h + 18, 36))
            return s->err ? s->err : IMGDEC_E_CORRUPT;
        got = 54;
    } else if (dib == 12) {
        if (!img_src_read(s, h + 18, 8))
            return s->err ? s->err : IMGDEC_E_CORRUPT;
        got = 26;
    }
    else
        return IMGDEC_E_FORMAT;
    bmp_hdr_t hd;
    int rc = img_bmp_parse(h, got, &hd, info);
    if (rc)
        return rc;
    if (io->info && io->info(io->ctx, info))
        return IMGDEC_E_ABORT;
    uint32_t pos = (uint32_t)got;
    /* the rest of the DIB header (V4/V5, bit masks) */
    if (dib > 40) {
        if (!img_src_skip(s, dib - 40))
            return IMGDEC_E_CORRUPT;
        pos += dib - 40;
    }
    uint32_t pal_n = 0;
    uint8_t pal[256];
    bool rgb = io->row8 && io->want_rgb; /* RGB rows (info.color is set for every BMP) */
    uint8_t *prgb = rgb && hd.bpp <= 8 ? al(io, 768) : NULL;
    if (rgb && hd.bpp <= 8 && !prgb)
        return IMGDEC_E_NOMEM;
    if (hd.bpp <= 8) {
        pal_n = hd.colors ? hd.colors : 1u << hd.bpp;
        if (pal_n > 256)
            return IMGDEC_E_CORRUPT;
        uint32_t esz = dib == 12 ? 3 : 4;
        for (uint32_t i = 0; i < pal_n; i++) {
            uint8_t e[4];
            if (!img_src_read(s, e, esz)) {
                fr(io, prgb);
                return s->err ? s->err : IMGDEC_E_CORRUPT;
            }
            pal[i] = imgdec_gray_rgb(e[2], e[1], e[0]);
            if (prgb) {
                prgb[3 * i] = e[2];
                prgb[3 * i + 1] = e[1];
                prgb[3 * i + 2] = e[0];
            }
        }
        pos += pal_n * esz;
    } else if (hd.comp == 3 && dib == 40) {
        if (!img_src_skip(s, 12)) /* the three masks after a 40-byte header */
            return IMGDEC_E_CORRUPT;
        pos += 12;
    }
    if (hd.off_bits < pos || !img_src_skip(s, hd.off_bits - pos)) {
        fr(io, prgb);
        return IMGDEC_E_CORRUPT;
    }
    uint32_t row = ((hd.w * hd.bpp + 31) / 32) * 4;
    uint8_t *rb = al(io, row), *gray = al(io, rgb ? 3u * hd.w : hd.w);
    if (!rb || !gray) {
        fr(io, rb);
        fr(io, gray);
        fr(io, prgb);
        return IMGDEC_E_NOMEM;
    }
    rc = IMGDEC_OK;
    bool up = hd.h > 0, back = up && io->seek; /* bottom-up: read the rows last first, emit top first */
    for (uint32_t r = 0; r < info->h && !rc; r++) {
        if (back) {
            if (io->seek(io->ctx, hd.off_bits + (info->h - 1 - r) * row)) {
                rc = IMGDEC_E_IO;
                break;
            }
            s->in_n = s->in_p = 0;
            s->eof = false;
        }
        if (!img_src_read(s, rb, row)) {
            rc = s->err ? s->err : IMGDEC_E_CORRUPT;
            break;
        }
        for (uint32_t x = 0; x < hd.w; x++) {
            uint8_t c3[3];
            if (hd.bpp <= 8) {
                uint32_t bit = x * hd.bpp, idx = (uint32_t)(rb[bit >> 3] >> (8 - hd.bpp - (bit & 7))) & ((1u << hd.bpp) - 1);
                if (!rgb) {
                    gray[x] = idx < pal_n ? pal[idx] : 0;
                    continue;
                }
                if (idx < pal_n)
                    memcpy(c3, prgb + 3 * idx, 3);
                else
                    memset(c3, 0, 3);
            } else if (hd.bpp == 16) { /* 5-5-5 */
                uint16_t p = le16(rb + 2 * x);
                uint8_t r5 = (p >> 10) & 31, g5 = (p >> 5) & 31, b5 = p & 31;
                c3[0] = (uint8_t)(r5 << 3 | r5 >> 2);
                c3[1] = (uint8_t)(g5 << 3 | g5 >> 2);
                c3[2] = (uint8_t)(b5 << 3 | b5 >> 2);
            } else {
                const uint8_t *p = rb + x * (hd.bpp / 8);
                c3[0] = p[2];
                c3[1] = p[1];
                c3[2] = p[0];
            }
            if (rgb)
                memcpy(gray + 3 * x, c3, 3);
            else
                gray[x] = imgdec_gray_rgb(c3[0], c3[1], c3[2]);
        }
        uint16_t y = (uint16_t)(up && !back ? info->h - 1 - r : r);
        if (rgb ? io->row8(io->ctx, y, gray, (uint16_t)hd.w, 3) : img_emit_row(io, y, gray, (uint16_t)hd.w))
            rc = IMGDEC_E_ABORT;
    }
    fr(io, rb);
    fr(io, gray);
    fr(io, prgb);
    return rc;
}
