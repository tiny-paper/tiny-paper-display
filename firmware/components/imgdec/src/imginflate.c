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

/* At least n (<= 24) bits in the buffer; past the end of the data zero bytes, counted in `over`. */
static inline void need(infl_t *z, uint32_t n)
{
    while (z->nbits < n) {
        int c = img_idat_byte(z);
        if (c < 0) {
            c = 0;
            z->over++;
        }
        z->bits |= (uint32_t)c << z->nbits;
        z->nbits += 8;
    }
}

static inline void consume(infl_t *z, uint32_t n)
{
    z->bits >>= n;
    z->nbits -= n;
    if (z->nbits < z->over * 8)
        z->err = IMGDEC_E_CORRUPT; /* bits from beyond the end of the data */
}

static uint32_t getbits(infl_t *z, uint32_t n)
{
    need(z, n);
    uint32_t v = z->bits & ((1u << n) - 1);
    consume(z, n);
    return v;
}

static bool build(huff_t *t, const uint8_t *lens, int n)
{
    uint16_t offs[16], next[16];
    memset(t->counts, 0, sizeof t->counts);
    memset(t->fast, 0, sizeof t->fast);
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
    uint32_t code = 0;
    next[0] = 0;
    for (int i = 1; i < 16; i++) {
        code = (code + t->counts[i - 1]) << 1;
        next[i] = (uint16_t)code;
    }
    for (int i = 0; i < n; i++) {
        uint32_t len = lens[i];
        if (!len)
            continue;
        t->symbols[offs[len]++] = (uint16_t)i;
        uint32_t c = next[len]++;
        if (len > INFL_FAST)
            continue;
        uint32_t r = 0; /* the code is sent MSB first, the bits arrive LSB first */
        for (uint32_t k = 0; k < len; k++)
            r |= ((c >> k) & 1u) << (len - 1 - k);
        for (; r < (1u << INFL_FAST); r += 1u << len)
            t->fast[r] = (uint16_t)(len << 9 | (uint32_t)i);
    }
    return true;
}

static int decode_sym(infl_t *z, const huff_t *t)
{
    need(z, 15);
    uint32_t e = t->fast[z->bits & ((1u << INFL_FAST) - 1)];
    if (e) {
        consume(z, e >> 9);
        return z->err ? -1 : (int)(e & 511);
    }
    int code = 0, first = 0, index = 0;
    uint32_t b = z->bits;
    for (uint32_t len = 1; len < 16; len++) {
        code |= (int)(b & 1);
        b >>= 1;
        int count = t->counts[len];
        if (code - count < first) {
            consume(z, len);
            return z->err ? -1 : t->symbols[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    z->err = IMGDEC_E_CORRUPT;
    return -1;
}

/* The bytes written since the last flush to emit; at the end of the ring, back to its start. */
static int flush(infl_t *z, emit_fn emit, void *ectx)
{
    int rc = z->wpos > z->fpos ? emit(ectx, z->win + z->fpos, z->wpos - z->fpos) : 0;
    z->fpos = z->wpos;
    if (z->wpos == z->wsize)
        z->wpos = z->fpos = 0;
    return rc;
}

static inline int put(infl_t *z, uint8_t b, emit_fn emit, void *ectx)
{
    z->win[z->wpos++] = b;
    z->total++;
    return z->wpos == z->wsize ? flush(z, emit, ectx) : 0;
}

static int inflate_block(infl_t *z, emit_fn emit, void *ectx)
{
    static const uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                       35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const uint16_t dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                       257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    const uint32_t mask = z->wsize - 1;
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
        z->total += len;
        uint32_t src = (z->wpos - d) & mask;
        while (len) {
            /* a piece that neither ring end cuts and, when the source is behind, no longer than the distance: the
             * bytes it reads are written before it (a repeat of a short run goes piece by piece) */
            uint32_t k = len;
            if (k > d)
                k = d;
            if (k > z->wsize - z->wpos)
                k = z->wsize - z->wpos;
            if (k > z->wsize - src)
                k = z->wsize - src;
            memmove(z->win + z->wpos, z->win + src, k); /* a source ahead (wrapped) may overlap: memmove copies it forward */
            z->wpos += k;
            src = (src + k) & mask;
            len -= k;
            if (z->wpos == z->wsize) {
                int rc = flush(z, emit, ectx);
                if (rc)
                    return rc;
            }
        }
    }
}

static int inflate_stored(infl_t *z, emit_fn emit, void *ectx)
{
    consume(z, z->nbits & 7); /* to a byte boundary */
    uint16_t len = (uint16_t)getbits(z, 16), nlen = (uint16_t)getbits(z, 16);
    uint16_t inv = (uint16_t)~nlen;
    if (z->err || inv != len)
        return IMGDEC_E_CORRUPT;
    for (uint32_t i = 0; i < len; i++) {
        int c;
        if (z->nbits >= 8) { /* bytes already in the bit buffer first */
            c = (int)getbits(z, 8);
            if (z->err)
                return IMGDEC_E_CORRUPT;
        } else if ((c = img_idat_byte(z)) < 0) {
            return IMGDEC_E_CORRUPT;
        }
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
        uint32_t final = getbits(z, 1), type = getbits(z, 2);
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
            return flush(z, emit, ectx);
    }
}
