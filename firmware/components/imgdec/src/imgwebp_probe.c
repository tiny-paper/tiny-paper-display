/* WebP header sizes for native decode and the photo relay, including boards without the decoder.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "imgdec_priv.h"
#include "imgwebp.h"


int imgwebp_probe(const uint8_t *b, size_t n, imgdec_info_t *out)
{
    memset(out, 0, sizeof *out);
    static const uint8_t SIG[4] = {'R', 'I', 'F', 'F'};
    if (memcmp(b, SIG, n < 4 ? n : 4) || (n >= 12 && memcmp(b + 8, "WEBP", 4)))
        return IMGDEC_E_FORMAT;
    if (n < 30)
        return IMGDEC_E_CORRUPT; /* more bytes */
    out->format = IMG_WEBP;
    out->orient = 1;
    out->gray_bits = 8;
    out->color = 1;
    out->bits = 24;
    uint32_t w, h;
    if (!memcmp(b + 12, "VP8 ", 4)) {
        if (b[23] != 0x9d || b[24] != 0x01 || b[25] != 0x2a)
            return IMGDEC_E_CORRUPT;
        w = (b[26] | (uint32_t)b[27] << 8) & 0x3fff;
        h = (b[28] | (uint32_t)b[29] << 8) & 0x3fff;
    } else if (!memcmp(b + 12, "VP8L", 4)) {
        if (b[20] != 0x2f)
            return IMGDEC_E_CORRUPT;
        uint32_t v = le32(b + 21);
        w = (v & 0x3fff) + 1;
        h = ((v >> 14) & 0x3fff) + 1;
        out->bits = v >> 28 & 1 ? 32 : 24;
    } else if (!memcmp(b + 12, "VP8X", 4)) {
        w = imgwebp_le24(b + 24) + 1;
        h = imgwebp_le24(b + 27) + 1;
        out->bits = b[20] & 0x10 ? 32 : 24;
    } else {
        return IMGDEC_E_FORMAT;
    }
    if (!w || !h)
        return IMGDEC_E_CORRUPT;
    if (w > IMGDEC_MAX_W || h > IMGDEC_MAX_H)
        return IMGDEC_E_SIZE;
    out->w = out->rw = (uint16_t)w;
    out->h = out->rh = (uint16_t)h;
    return IMGDEC_OK;
}
