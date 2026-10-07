/* imgdec: WebP (private to the component). SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IMGWEBP_H
#define IMGWEBP_H

#include "imgdec.h"

static inline uint32_t imgwebp_le24(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
}

/* The file from its start; `pre` holds bytes already read (file offset pre_off), the rest comes through io.read. */
int imgwebp_decode(const imgdec_io_t *io, imgdec_info_t *info, const uint8_t *pre, size_t pre_n, uint32_t pre_off);
/* The header from the first bytes (RIFF, WEBP and the first chunk's header and start): IMGDEC_OK, or
 * IMGDEC_E_CORRUPT when more bytes are needed, IMGDEC_E_FORMAT, IMGDEC_E_SIZE. */
int imgwebp_probe(const uint8_t *b, size_t n, imgdec_info_t *out);
size_t imgwebp_need(const imgdec_info_t *info);

#endif
