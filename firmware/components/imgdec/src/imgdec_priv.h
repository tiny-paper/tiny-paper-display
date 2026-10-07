/* imgdec private: the byte source and helpers shared by the format files. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "imgdec.h"

#define IN_BUF 256

typedef struct {
    const imgdec_io_t *io;
    uint8_t in[IN_BUF];
    size_t in_n, in_p;
    uint32_t total; /* bytes read so far */
    bool eof;
    int err;
} src_t;

int img_src_byte(src_t *s);
bool img_src_read(src_t *s, uint8_t *out, size_t n);
bool img_src_skip(src_t *s, uint32_t n);

static inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static const uint8_t PNG_SIG[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

static inline void *al(const imgdec_io_t *io, size_t n) { return io->alloc(io->ctx, n); }
static inline void fr(const imgdec_io_t *io, void *p)
{
    if (p)
        io->free(io->ctx, p);
}


/* A gray row (levels 0..15) to io.row, or x 17 to io.row8 (the buffer is the decoder's own, rewritten per row). */
int img_emit_row(const imgdec_io_t *io, uint16_t y, uint8_t *gray, uint16_t w);

/* The probe's header parsers, shared with the decoders */
typedef struct {
    uint32_t off_bits, w;
    int32_t h;
    uint16_t bpp;
    uint32_t comp, colors, dib;
} bmp_hdr_t;

int img_bmp_parse(const uint8_t *b, size_t n, bmp_hdr_t *hd, imgdec_info_t *out);
int img_gif_header(const uint8_t *b, imgdec_info_t *out);

int img_png_decode(src_t *s, const imgdec_io_t *io, imgdec_info_t *info);
size_t img_png_need(const imgdec_info_t *info, uint32_t window);
int img_bmp_decode(src_t *s, const imgdec_io_t *io, imgdec_info_t *info);
int img_gif_decode(src_t *s, const imgdec_io_t *io, imgdec_info_t *info);
size_t img_gif_need(const imgdec_info_t *info);
