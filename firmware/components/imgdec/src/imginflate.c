/*
 * imgdec: inflate (RFC 1951), pull model; the PNG decoder feeds it IDAT bytes.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "imginflate.h"

int img_idat_byte(infl_t *z)
{
    while (!z->chunk_left) {
        if (z->idat_end)
            return -1;
        uint8_t h[12];
        if (!img_src_read(z->s, h, 4) || !img_src_read(z->s, h + 4, 8)) /* CRC of the chunk before, then the next header */
            return -1;
        uint32_t len = be32(h + 4);
        if (memcmp(h + 8, "IDAT", 4)) {
            z->idat_end = true;
            return -1;
        }
        z->chunk_left = len;
    }
    z->chunk_left--;
    return img_src_byte(z->s);
}

static int getbit(infl_t *z)
{
    if (!z->nbits) {
        int c = img_idat_byte(z);
        if (c < 0) {
            z->err = IMGDEC_E_CORRUPT;
            return 0;
        }
        z->bits = (uint32_t)c;
        z->nbits = 8;
    }
    int b = (int)(z->bits & 1);
    z->bits >>= 1;
    z->nbits--;
    return b;
}

static uint32_t getbits(infl_t *z, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        v |= (uint32_t)getbit(z) << i;
    return v;
}

static bool build(huff_t *t, const uint8_t *lens, int n)
{
    uint16_t offs[16];
    memset(t->counts, 0, sizeof t->counts);
    for (int i = 0; i < n; i++)
        t->counts[lens[i]]++;
    t->counts[0] = 0;
    int left = 1;
    for (int i = 1; i < 16; i++) {
        left <<= 1;
        left -= t->counts[i];
        if (left < 0)
            return false; /* over-subscribed */
    }
    offs[1] = 0;
    for (int i = 1; i < 15; i++)
        offs[i + 1] = (uint16_t)(offs[i] + t->counts[i]);
    for (int i = 0; i < n; i++)
        if (lens[i])
            t->symbols[offs[lens[i]]++] = (uint16_t)i;
    return true;
}

static int decode_sym(infl_t *z, const huff_t *t)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= getbit(z);
        int count = t->counts[len];
        if (code - count < first)
            return t->symbols[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (z->err)
            return -1;
    }
    z->err = IMGDEC_E_CORRUPT;
    return -1;
}


static int put(infl_t *z, uint8_t b, emit_fn emit, void *ectx)
{
    z->win[z->wpos] = b;
    z->wpos = (z->wpos + 1) & (z->wsize - 1);
    z->total++;
    return emit(ectx, b);
}

static int inflate_block(infl_t *z, emit_fn emit, void *ectx)
{
    static const uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                       35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                       257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        int sym = decode_sym(z, &z->lit);
        if (z->err || sym < 0)
            return z->err ? z->err : IMGDEC_E_CORRUPT;
        if (sym < 256) {
            int rc = put(z, (uint8_t)sym, emit, ectx);
            if (rc)
                return rc;
            continue;
        }
        if (sym == 256)
            return IMGDEC_OK;
        sym -= 257;
        if (sym >= 29)
            return IMGDEC_E_CORRUPT;
        uint32_t len = lbase[sym] + getbits(z, lext[sym]);
        int ds = decode_sym(z, &z->dist);
        if (z->err || ds < 0 || ds >= 30)
            return z->err ? z->err : IMGDEC_E_CORRUPT;
        uint32_t d = dbase[ds] + getbits(z, dext[ds]);
        if (z->err)
            return z->err;
        if (d > z->total)
            return IMGDEC_E_CORRUPT;
        if (d > z->wsize)
            return IMGDEC_E_NOMEM; /* further back than the window we could allocate */
        for (uint32_t i = 0; i < len; i++) {
            int rc = put(z, z->win[(z->wpos - d) & (z->wsize - 1)], emit, ectx);
            if (rc)
                return rc;
        }
    }
}

static int inflate_stored(infl_t *z, emit_fn emit, void *ectx)
{
    z->nbits = 0; /* to a byte boundary */
    uint8_t h[4];
    for (int i = 0; i < 4; i++) {
        int c = img_idat_byte(z);
        if (c < 0)
            return IMGDEC_E_CORRUPT;
        h[i] = (uint8_t)c;
    }
    uint16_t len = le16(h), nlen = le16(h + 2);
    uint16_t inv = (uint16_t)~nlen;
    if (inv != len)
        return IMGDEC_E_CORRUPT;
    for (uint32_t i = 0; i < len; i++) {
        int c = img_idat_byte(z);
        if (c < 0)
            return IMGDEC_E_CORRUPT;
        int rc = put(z, (uint8_t)c, emit, ectx);
        if (rc)
            return rc;
    }
    return IMGDEC_OK;
}

static void fixed_tables(infl_t *z)
{
    uint8_t l[288];
    memset(l, 8, 144);
    memset(l + 144, 9, 112);
    memset(l + 256, 7, 24);
    memset(l + 280, 8, 8);
    build(&z->lit, l, 288);
    memset(l, 5, 30);
    build(&z->dist, l, 30);
}

static int dynamic_tables(infl_t *z)
{
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t lens[288 + 32];
    uint32_t hlit = getbits(z, 5) + 257, hdist = getbits(z, 5) + 1, hclen = getbits(z, 4) + 4;
    if (z->err || hlit > 286 || hdist > 30)
        return IMGDEC_E_CORRUPT;
    memset(lens, 0, 19);
    for (uint32_t i = 0; i < hclen; i++)
        lens[order[i]] = (uint8_t)getbits(z, 3);
    huff_t *cl = &z->dist; /* the code-length code, in the dist table until the real one is built */
    if (!build(cl, lens, 19))
        return IMGDEC_E_CORRUPT;
    for (uint32_t n = 0; n < hlit + hdist;) {
        int sym = decode_sym(z, cl);
        if (z->err || sym < 0)
            return IMGDEC_E_CORRUPT;
        uint32_t rep = 1;
        uint8_t v = 0;
        if (sym < 16) {
            v = (uint8_t)sym;
        } else if (sym == 16) {
            if (!n)
                return IMGDEC_E_CORRUPT;
            v = lens[n - 1];
            rep = 3 + getbits(z, 2);
        } else if (sym == 17) {
            rep = 3 + getbits(z, 3);
        } else {
            rep = 11 + getbits(z, 7);
        }
        if (n + rep > hlit + hdist)
            return IMGDEC_E_CORRUPT;
        while (rep--)
            lens[n++] = v;
    }
    if (z->err || !lens[256])
        return IMGDEC_E_CORRUPT;
    if (!build(&z->lit, lens, (int)hlit) || !build(&z->dist, lens + hlit, (int)hdist))
        return IMGDEC_E_CORRUPT;
    return IMGDEC_OK;
}

int img_inflate_all(infl_t *z, emit_fn emit, void *ectx)
{
    for (;;) {
        int final = getbit(z);
        uint32_t type = getbits(z, 2);
        if (z->err)
            return z->err;
        int rc;
        if (type == 0) {
            rc = inflate_stored(z, emit, ectx);
        } else if (type == 1) {
            fixed_tables(z);
            rc = inflate_block(z, emit, ectx);
        } else if (type == 2) {
            rc = dynamic_tables(z);
            if (!rc)
                rc = inflate_block(z, emit, ectx);
        } else {
            rc = IMGDEC_E_CORRUPT;
        }
        if (rc)
            return rc;
        if (final)
            return IMGDEC_OK;
    }
}
