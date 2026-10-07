/*
 * imgdec: baseline and progressive JPEG (docs/spec/assets.md "Images", "JPEG"). Huffman, 8-bit, 1 or 3 components,
 * luma sampled 1x1, 2x1, 1x2 or 2x2 and chroma 1x1, restart markers. Baseline: one row of MCUs at a time, decoded
 * into a row buffer of the (scaled) image width, then handed out row by row. Progressive (SOF2, unless built with
 * IMGDEC_NO_PROGRESSIVE): every scan (spectral selection, successive approximation) into a coefficient store of the
 * whole image (fewer coefficients a block when scaled, Y only for gray rows, capped by io.prog_max), then the same
 * rows as baseline from it (io.prog_bands: in bands from a file; io.prog_dc: a stream's first, DC, scan only, at 1/8:
 * assets.md "JPEG" Progressive DC). Integer only: libjpeg's "islow" IDCT with wrapping 32-bit arithmetic (the TS twin
 * packages/tinypack/src/jpeg.ts computes the same bits), a scale of 1/2 or 1/4 as the mean of the full block, 1/8 as
 * the DC value. Chroma is upsampled nearest; without RGB it is not decoded at all (Adobe transform 0: the three
 * components are R, G, B, all decoded, gray = the host-abi.md luminance).
 * EXIF (APP1): orientation and a few tags, read through io.seek beyond the segment's first bytes.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "imgjpg.h"

#ifndef IMGDEC_NO_PROGRESSIVE
#define PROG 1
#else
#define PROG 0
#endif

#define JIN 512
#define J_EOI 1 /* headers(): the end of a progressive image */
#define PROG_SCANS 128 /* scans a progressive image may have */
#define HEAD 384 /* the mcu buffer; also the EXIF segment's first bytes while the headers are read */

typedef uint32_t u32;

typedef struct {
    uint16_t look[256]; /* first 8 bits: length << 8 | value, 0 = a longer code */
    uint8_t val[256];
    int32_t maxcode[18];
    u32 mincode[17];
    uint16_t valptr[17];
    bool ok;
} jhuff_t;

typedef struct {
    uint8_t id, hs, vs, tq, td, ta;
    int32_t pred;
} jcomp_t;

typedef struct {
    const imgdec_io_t *io;
    uint8_t in[JIN];
    size_t n, p;
    u32 off; /* file offset of in[n] */
    bool eof;
    int err;
    u32 bits;
    int nb, fake, marker;
    bool qok[4];
    uint16_t q[4][64]; /* zigzag order, as stored */
    jhuff_t ht[4];     /* DC 0, 1; AC 2, 3 */
    jcomp_t c[3];
    uint8_t nc;
    uint16_t w, h, ri;
    bool sof, sos;
    bool rgbc; /* Adobe APP14 transform 0: the components are R, G, B (no YCbCr) */
    int32_t coef[64];
    u32 ws[64];
    uint8_t blk[64];
    uint8_t mcu[HEAD]; /* Y 16 x 16, Cb 8 x 8 at 256, Cr at 320 */
    /* progressive */
    bool prog;
    uint8_t sn, sc[4], ss, se, ah, al; /* the scan: its components (indexes into c), spectral band, bit positions */
    uint8_t K;                          /* coefficients kept a block: 64 (natural order), 16 (4x4), 4 (2x2), 1 (DC) */
    u32 eobrun;
    int8_t kmap[64];                    /* zigzag index -> index among the kept ones, -1 not kept */
    int16_t *co[3];                     /* per component, NULL: not kept */
    uint8_t *nz[3];                     /* nonzero bitmaps (8 bytes a block by zigzag index) when K is 16 or 4 */
    u32 cbw[3];                         /* the store's blocks a row (whole MCUs) */
    u32 r0[3], r1[3];                   /* the band: block rows r0 .. r1 - 1 of each component kept */
    uint8_t *store;
    size_t store_n;
    u32 ub;                             /* units of rows a band (prog_units: all of them without bands) */
    bool dcs;                           /* prog_dc: decoding a progressive file's first (DC) scan only, at 1/8 */
    bool ymode, bad;                    /* only Y kept for a color file: units are Y block rows; bad until a restart */
    struct jscan *scans;                /* bands: every scan (NULL: the file is read once, in order) */
    uint8_t nscans;
    u32 ht_at[4];                       /* file offsets of the DHT definitions of the tables loaded */
} jpg_t;

static const uint8_t ZZ[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                               41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                               30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/* ------------------------------------------------------------------ bytes */

static int jb(jpg_t *j)
{
    if (j->p >= j->n) {
        if (j->eof)
            return -1;
        long r = j->io->read(j->io->ctx, j->in, JIN);
        if (r <= 0) {
            if (r < 0)
                j->err = IMGDEC_E_IO;
            j->eof = true;
            return -1;
        }
        j->n = (size_t)r;
        j->p = 0;
        j->off += (u32)r;
    }
    return j->in[j->p++];
}

static u32 jpos(const jpg_t *j) { return j->off - (u32)(j->n - j->p); }

static int u16be(jpg_t *j)
{
    int a = jb(j), b = jb(j);
    return a < 0 || b < 0 ? -1 : a << 8 | b;
}

static bool jseek(jpg_t *j, u32 off)
{
    if (!j->io->seek || j->io->seek(j->io->ctx, off))
        return false;
    j->n = j->p = 0;
    j->off = off;
    j->eof = false;
    return true;
}

static bool jskip(jpg_t *j, u32 k)
{
    if (k > j->n - j->p && j->io->seek)
        return jseek(j, jpos(j) + k);
    while (k--)
        if (jb(j) < 0)
            return false;
    return true;
}

/* ------------------------------------------------------------------ EXIF */

typedef struct {
    jpg_t *j;
    u32 base, len, hn; /* file offset of the TIFF header, its bytes, the bytes in hb */
    bool le, moved;
    uint8_t *hb;       /* the segment's first bytes: j->mcu, or without seek (a stream) up to EXIF_STREAM */
} tiff_t;

#define EXIF_STREAM 4096 /* EXIF bytes held while the headers of a stream are read (no seek: IFDs past HEAD) */

static bool tread(tiff_t *t, u32 off, uint8_t *b, u32 n)
{
    if (off > t->len || n > t->len - off)
        return false;
    if (off + n <= t->hn) {
        memcpy(b, t->hb + off, n);
        return true;
    }
    if (!t->j->io->seek)
        return false;
    t->moved = true;
    if (!jseek(t->j, t->base + off))
        return false;
    for (u32 i = 0; i < n; i++) {
        int c = jb(t->j);
        if (c < 0)
            return false;
        b[i] = (uint8_t)c;
    }
    return true;
}

static u32 t16(const tiff_t *t, const uint8_t *b) { return t->le ? (u32)(b[0] | b[1] << 8) : (u32)(b[0] << 8 | b[1]); }
static u32 t32(const tiff_t *t, const uint8_t *b)
{
    return t->le ? (u32)b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 | (u32)b[3] << 24
                 : (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | (u32)b[3];
}

static void tstr(tiff_t *t, const uint8_t *e, char *out, size_t cap)
{
    u32 cnt = t32(t, e + 4);
    u32 k = cnt < cap - 1 ? cnt : (u32)cap - 1;
    out[0] = 0;
    if (t16(t, e + 2) != 2)
        return;
    if (cnt <= 4)
        memcpy(out, e + 8, k);
    else if (!tread(t, t32(t, e + 8), (uint8_t *)out, k))
        k = 0;
    out[k] = 0;
    size_t n = strlen(out);
    for (size_t i = 0; i < n; i++)
        if ((uint8_t)out[i] < 0x20)
            out[i] = ' ';
    while (n && out[n - 1] == ' ')
        out[--n] = 0;
    size_t s = 0;
    while (out[s] == ' ')
        s++;
    memmove(out, out + s, n - s + 1);
}

static bool trat(tiff_t *t, const uint8_t *e, u32 i, u32 *num, u32 *den)
{
    uint8_t b[8];
    u32 ty = t16(t, e + 2);
    if ((ty != 5 && ty != 10) || t32(t, e + 4) <= i || !tread(t, t32(t, e + 8) + 8 * i, b, 8))
        return false;
    *num = t32(t, b);
    *den = t32(t, b + 4);
    return true;
}

static uint16_t x10(tiff_t *t, const uint8_t *e)
{
    u32 n, d;
    if (!trat(t, e, 0, &n, &d) || !d)
        return 0;
    uint64_t v = ((uint64_t)n * 10u + d / 2u) / d;
    return v > 65535u ? 65535u : (uint16_t)v;
}

static int32_t dms(tiff_t *t, const uint8_t *e)
{
    static const u32 DIV[3] = {1, 60, 3600};
    uint64_t acc = 0;
    for (u32 i = 0; i < 3; i++) {
        u32 n, d;
        if (!trat(t, e, i, &n, &d))
            return -1;
        if (d)
            acc += (uint64_t)n * 100000u / ((uint64_t)d * DIV[i]);
    }
    return acc > 1000000000u ? 1000000000 : (int32_t)acc;
}

/* kind 0: IFD0, 1: Exif IFD, 2: GPS IFD */
static void ifd(tiff_t *t, u32 o, int kind, imgdec_exif_t *x, u32 *sub, u32 *gps)
{
    uint8_t e[12];
    if (!tread(t, o, e, 2))
        return;
    u32 n = t16(t, e);
    int32_t lat = -1, lon = -1;
    char rlat = 'N', rlon = 'E';
    for (u32 i = 0; i < n && i < 256; i++) {
        if (!tread(t, o + 2 + 12 * i, e, 12))
            return;
        u32 tag = t16(t, e), ty = t16(t, e + 2);
        if (kind == 0) {
            if (tag == 0x0112 && ty == 3) {
                u32 v = t16(t, e + 8);
                if (v >= 1 && v <= 8)
                    x->orient = (uint8_t)v;
            } else if (tag == 0x010F) {
                tstr(t, e, x->make, sizeof x->make);
            } else if (tag == 0x0110) {
                tstr(t, e, x->model, sizeof x->model);
            } else if (tag == 0x010E) {
                tstr(t, e, x->desc, sizeof x->desc);
            } else if (tag == 0x8769) {
                *sub = t32(t, e + 8);
            } else if (tag == 0x8825) {
                *gps = t32(t, e + 8);
            }
        } else if (kind == 1) {
            if (tag == 0x9003) {
                tstr(t, e, x->date, sizeof x->date);
            } else if (tag == 0x829A) {
                u32 a, b;
                if (trat(t, e, 0, &a, &b)) {
                    x->exp_num = a;
                    x->exp_den = b;
                }
            } else if (tag == 0x829D) {
                x->f_x10 = x10(t, e);
            } else if (tag == 0x8827) {
                u32 v = ty == 3 ? t16(t, e + 8) : ty == 4 ? t32(t, e + 8) : 0;
                x->iso = v > 65535u ? 65535u : (uint16_t)v;
            } else if (tag == 0x920A) {
                x->focal_x10 = x10(t, e);
            }
        } else {
            if (tag == 1 && ty == 2)
                rlat = (char)e[8];
            else if (tag == 3 && ty == 2)
                rlon = (char)e[8];
            else if (tag == 2)
                lat = dms(t, e);
            else if (tag == 4)
                lon = dms(t, e);
        }
    }
    if (kind == 2 && lat >= 0 && lon >= 0) {
        x->gps = true;
        x->lat_e5 = rlat == 'S' ? -lat : lat;
        x->lon_e5 = rlon == 'W' ? -lon : lon;
    }
}

static void exif_parse(tiff_t *t, imgdec_exif_t *x)
{
    uint8_t b[8];
    if (!tread(t, 0, b, 8))
        return;
    if (b[0] == 'I' && b[1] == 'I')
        t->le = true;
    else if (!(b[0] == 'M' && b[1] == 'M'))
        return;
    if (t16(t, b + 2) != 42)
        return;
    u32 sub = 0, gps = 0;
    ifd(t, t32(t, b + 4), 0, x, &sub, &gps);
    if (sub)
        ifd(t, sub, 1, x, NULL, NULL);
    if (gps)
        ifd(t, gps, 2, x, NULL, NULL);
}

/* APP1, after its length: seglen bytes. */
static int app1(jpg_t *j, u32 seglen, imgdec_exif_t *x)
{
    u32 start = jpos(j), used = 0;
    tiff_t t = {j, start + 6, 0, 0, false, false, j->mcu};
    uint8_t id[6];
    if (seglen >= 14 && x) {
        for (int i = 0; i < 6; i++) {
            int c = jb(j);
            if (c < 0)
                return IMGDEC_E_CORRUPT;
            id[i] = (uint8_t)c;
        }
        used = 6;
        if (!memcmp(id, "Exif\0\0", 6)) {
            t.len = seglen - 6;
            u32 cap = HEAD;
            if (!j->io->seek && t.len > HEAD && (t.hb = j->io->alloc(j->io->ctx, EXIF_STREAM)))
                cap = EXIF_STREAM;
            else
                t.hb = j->mcu;
            t.hn = t.len < cap ? t.len : cap;
            int rc = IMGDEC_OK;
            for (u32 i = 0; i < t.hn && !rc; i++) {
                int c = jb(j);
                if (c < 0)
                    rc = IMGDEC_E_CORRUPT;
                t.hb[i] = (uint8_t)c;
            }
            if (!rc)
                exif_parse(&t, x);
            if (t.hb != j->mcu)
                j->io->free(j->io->ctx, t.hb);
            if (rc)
                return rc;
            used += t.hn;
        }
    }
    if (t.moved)
        return jseek(j, start + seglen) ? IMGDEC_OK : IMGDEC_E_IO;
    return jskip(j, seglen - used) ? IMGDEC_OK : IMGDEC_E_CORRUPT;
}

/* ------------------------------------------------------------------ headers */

static bool is_other_sof(int m) { return (m >= 0xC2 && m <= 0xCF && m != 0xC4 && m != 0xC8) ; }

static int huff_build(jhuff_t *t, const uint8_t *cnt, int total)
{
    memset(t->look, 0, sizeof t->look);
    u32 code = 0;
    int k = 0;
    for (int l = 1; l <= 16; l++) {
        t->valptr[l] = (uint16_t)k;
        t->mincode[l] = code;
        for (int i = 0; i < cnt[l - 1]; i++, k++, code++) {
            if (code >= (1u << l))
                return IMGDEC_E_CORRUPT; /* more codes than the length has */
            if (l <= 8)
                for (u32 f = 0; f < (1u << (8 - l)); f++)
                    t->look[(code << (8 - l)) | f] = (uint16_t)(l << 8 | t->val[k]);
        }
        t->maxcode[l] = cnt[l - 1] ? (int32_t)code - 1 : -1;
        if (code > (1u << l))
            return IMGDEC_E_CORRUPT;
        code <<= 1;
    }
    t->maxcode[17] = 0x7FFFFFFF;
    t->ok = k == total && total > 0;
    return t->ok ? IMGDEC_OK : IMGDEC_E_CORRUPT;
}

/* One table of a DHT segment (rest: the segment's bytes left), from its class and slot byte on. */
static int dht_one(jpg_t *j, u32 *rest)
{
    u32 at = jpos(j);
    int tc = jb(j);
    uint8_t cnt[16];
    int total = 0;
    if (tc < 0 || *rest < 17 || (tc >> 4) > 1)
        return IMGDEC_E_CORRUPT;
    if ((tc & 15) > 1)
        return IMGDEC_E_FORMAT; /* baseline: two tables of each class */
    for (int i = 0; i < 16; i++) {
        int v = jb(j);
        if (v < 0)
            return IMGDEC_E_CORRUPT;
        cnt[i] = (uint8_t)v;
        total += v;
    }
    if (total > 256 || *rest < 17u + (u32)total)
        return IMGDEC_E_CORRUPT;
    int k = (tc >> 4) * 2 + (tc & 15);
    jhuff_t *t = &j->ht[k];
    for (int i = 0; i < total; i++) {
        int v = jb(j);
        if (v < 0)
            return IMGDEC_E_CORRUPT;
        t->val[i] = (uint8_t)v;
    }
    j->ht_at[k] = at;
    *rest -= 17u + (u32)total;
    return huff_build(t, cnt, total);
}

/* Segments up to the next SOS (IMGDEC_OK), or a progressive image's EOI after a scan (J_EOI). x: EXIF wanted. */
static int headers(jpg_t *j, imgdec_exif_t *x)
{
    for (;;) {
        int m = j->marker; /* a progressive scan ended at this marker */
        j->marker = 0;
        if (!m) {
            int c = jb(j);
            while (c >= 0 && c != 0xFF)
                c = jb(j); /* stray bytes */
            while (c == 0xFF)
                c = jb(j);
            if (c < 0)
                return j->err ? j->err : IMGDEC_E_CORRUPT;
            m = c;
        }
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01 || m == 0x00)
            continue;
        if (m == 0xD9)
            return j->prog && j->sos ? J_EOI : IMGDEC_E_CORRUPT;
        bool sof2 = PROG && m == 0xC2;
        if (sof2 && !j->io->prog_max && !(j->io->prog_bands && j->io->seek) && !j->io->prog_dc)
            return IMGDEC_E_PROGRESSIVE; /* not on this board */
        if (is_other_sof(m) && !sof2)
            return IMGDEC_E_PROGRESSIVE; /* progressive, lossless, hierarchical, arithmetic (and DAC 0xCC) */
        int len = u16be(j);
        if (len < 2)
            return j->err ? j->err : IMGDEC_E_CORRUPT;
        u32 rest = (u32)len - 2;
        if (m == 0xDB) { /* DQT */
            while (rest > 0) {
                int pt = jb(j);
                if (pt < 0 || (pt & 15) > 3 || rest < 1u + ((pt >> 4) ? 128u : 64u))
                    return IMGDEC_E_CORRUPT;
                int tq = pt & 15;
                for (int k = 0; k < 64; k++) {
                    int v = (pt >> 4) ? u16be(j) : jb(j);
                    if (v < 0)
                        return IMGDEC_E_CORRUPT;
                    j->q[tq][k] = (uint16_t)v;
                }
                j->qok[tq] = true;
                rest -= 1u + ((pt >> 4) ? 128u : 64u);
            }
        } else if (m == 0xC4) { /* DHT */
            while (rest > 0) {
                int rc = dht_one(j, &rest);
                if (rc)
                    return rc;
            }
        } else if (m == 0xC0 || m == 0xC1 || sof2) { /* SOF, baseline, extended or progressive Huffman */
            int p = jb(j), h = u16be(j), w = u16be(j), nc = jb(j);
            if (j->sof)
                return IMGDEC_E_CORRUPT;
            j->prog = sof2;
            if (p < 0 || h < 0 || w < 0 || nc < 0)
                return IMGDEC_E_CORRUPT;
            if (p != 8)
                return IMGDEC_E_PROGRESSIVE; /* 12-bit */
            if (nc != 1 && nc != 3)
                return IMGDEC_E_FORMAT;
            if (h == 0)
                return IMGDEC_E_FORMAT; /* DNL */
            if (w == 0 || rest != 6u + 3u * (u32)nc)
                return IMGDEC_E_CORRUPT;
            if (w > IMGDEC_JPEG_MAX || h > IMGDEC_JPEG_MAX)
                return IMGDEC_E_SIZE;
            j->w = (uint16_t)w;
            j->h = (uint16_t)h;
            j->nc = (uint8_t)nc;
            for (int i = 0; i < nc; i++) {
                int id = jb(j), hv = jb(j), tq = jb(j);
                if (id < 0 || hv < 0 || tq < 0 || tq > 3)
                    return IMGDEC_E_CORRUPT;
                j->c[i] = (jcomp_t){(uint8_t)id, (uint8_t)(hv >> 4), (uint8_t)(hv & 15), (uint8_t)tq, 0, 0, 0};
            }
            j->sof = true;
        } else if (m == 0xDD) { /* DRI */
            int ri = u16be(j);
            if (ri < 0 || rest != 2)
                return IMGDEC_E_CORRUPT;
            j->ri = (uint16_t)ri;
        } else if (m == 0xDA) { /* SOS */
            int ns = jb(j);
            if (!j->sof || ns < 0)
                return IMGDEC_E_CORRUPT;
            if (!j->prog && ns != j->nc)
                return IMGDEC_E_FORMAT; /* one scan per component */
            if (ns < 1 || ns > j->nc)
                return IMGDEC_E_CORRUPT;
            for (int i = 0; i < ns; i++) {
                int cs = jb(j), t = jb(j);
                if (cs < 0 || t < 0)
                    return IMGDEC_E_CORRUPT;
                int ci = -1;
                for (int k = 0; k < j->nc; k++)
                    if (j->c[k].id == cs)
                        ci = k;
                for (int k = 0; k < i && ci >= 0; k++)
                    if (j->sc[k] == ci)
                        ci = -1; /* twice in the scan */
                if (ci < 0 || (t >> 4) > 1 || (t & 15) > 1)
                    return IMGDEC_E_CORRUPT;
                j->sc[i] = (uint8_t)ci;
                j->c[ci].td = (uint8_t)(t >> 4);
                j->c[ci].ta = (uint8_t)(2 + (t & 15));
            }
            j->sn = (uint8_t)ns;
#if PROG
            if (j->prog) { /* Ss, Se, Ah Al (assets.md "JPEG" progressive) */
                int ss = jb(j), se = jb(j), a = jb(j);
                if (ss < 0 || se < 0 || a < 0 || ss > se || se > 63 || (ss == 0 && se != 0) || (ss > 0 && ns != 1) ||
                    (a >> 4) > 13 || (a & 15) > 13 || ((a >> 4) && (a & 15) != (a >> 4) - 1))
                    return IMGDEC_E_CORRUPT;
                j->ss = (uint8_t)ss;
                j->se = (uint8_t)se;
                j->ah = (uint8_t)(a >> 4);
                j->al = (uint8_t)(a & 15);
            } else
#endif
            if (!jskip(j, 3))
                return IMGDEC_E_CORRUPT;
            j->sos = true;
            return IMGDEC_OK;
        } else if (m == 0xE1) {
            int rc = app1(j, rest, x);
            if (rc)
                return rc;
        } else if (m == 0xFE && j->io->com && j->io->com_cap && !j->io->com[0] && !j->sof) { /* COM: its text */
            u32 k = 0, keep = rest < (u32)j->io->com_cap - 1u ? rest : (u32)j->io->com_cap - 1u;
            for (; k < keep; k++) {
                int v = jb(j);
                if (v < 0)
                    return j->err ? j->err : IMGDEC_E_CORRUPT;
                j->io->com[k] = (char)v;
            }
            j->io->com[k] = 0;
            if (!jskip(j, rest - keep))
                return j->err ? j->err : IMGDEC_E_CORRUPT;
        } else if (m == 0xEE && rest >= 12) { /* Adobe: "Adobe", version, flags0, flags1, transform */
            uint8_t a[12];
            for (int i = 0; i < 12; i++) {
                int v = jb(j);
                if (v < 0)
                    return IMGDEC_E_CORRUPT;
                a[i] = (uint8_t)v;
            }
            if (!memcmp(a, "Adobe", 5))
                j->rgbc = a[11] == 0;
            if (!jskip(j, rest - 12))
                return j->err ? j->err : IMGDEC_E_CORRUPT;
        } else if (!jskip(j, rest)) {
            return j->err ? j->err : IMGDEC_E_CORRUPT;
        }
    }
}

/* ------------------------------------------------------------------ entropy */

static void fill(jpg_t *j)
{
    while (j->nb <= 24) {
        int c = 0;
        if (j->marker) {
            j->fake++;
        } else if ((c = jb(j)) < 0) {
            c = 0;
            j->fake++;
        } else if (c == 0xFF) {
            int c2 = jb(j);
            while (c2 == 0xFF)
                c2 = jb(j);
            if (c2 != 0) {
                j->marker = c2 < 0 ? 0xD9 : c2;
                j->fake++;
                c = 0;
            }
        }
        j->bits |= (u32)c << (24 - j->nb);
        j->nb += 8;
    }
}

static int hdec(jpg_t *j, const jhuff_t *t)
{
    if (j->nb < 16)
        fill(j);
    u32 e = t->look[j->bits >> 24];
    if (e) {
        int l = (int)(e >> 8);
        j->bits <<= l;
        j->nb -= l;
        return (int)(e & 0xFF);
    }
    for (int l = 9; l <= 16; l++) {
        u32 code = j->bits >> (32 - l);
        if ((int32_t)code <= t->maxcode[l]) {
            u32 k = t->valptr[l] + code - t->mincode[l];
            j->bits <<= l;
            j->nb -= l;
            return k < 256 ? t->val[k] : -1;
        }
    }
    return -1;
}

static int32_t recv(jpg_t *j, int s)
{
    if (!s)
        return 0;
    if (j->nb < s)
        fill(j);
    u32 v = j->bits >> (32 - s);
    j->bits <<= s;
    j->nb -= s;
    return v < (1u << (s - 1)) ? (int32_t)v - (1 << s) + 1 : (int32_t)v;
}

/* One block; coefficients dequantized into j->coef when keep (only DC when dc_only). false: a bad code. */
static bool dblock(jpg_t *j, jcomp_t *c, bool keep, bool dc_only)
{
    const jhuff_t *dc = &j->ht[c->td], *ac = &j->ht[c->ta];
    const uint16_t *q = j->q[c->tq];
    int t = hdec(j, dc);
    if (t < 0 || t > 11)
        return false;
    c->pred = (int32_t)((u32)c->pred + (u32)recv(j, t));
    if (keep) {
        if (!dc_only)
            memset(j->coef, 0, sizeof j->coef);
        j->coef[0] = (int32_t)(((u32)c->pred << (j->dcs ? j->al : 0)) * q[0]);
    }
    if (j->dcs)
        return true; /* a DC scan: no AC codes */
    for (int k = 1; k < 64; k++) {
        int rs = hdec(j, ac);
        if (rs < 0)
            return false;
        int r = rs >> 4, s = rs & 15;
        if (s) {
            k += r;
            if (k > 63)
                return false;
            int32_t v = recv(j, s);
            if (keep && !dc_only)
                j->coef[ZZ[k]] = (int32_t)((u32)v * q[k]);
        } else {
            if (r != 15)
                break;
            k += 15;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ IDCT (libjpeg jidctint.c "islow", wrapping) */

#define M(a, b) ((u32)(a) * (u32)(int32_t)(b))
#define DS(x, n) ((u32)((int32_t)((x) + (1u << ((n) - 1))) >> (n)))

static uint8_t clamp8(int32_t v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }

static void idct(jpg_t *j)
{
    const int32_t *in = j->coef;
    u32 *ws = j->ws;
    for (int c = 0; c < 8; c++) {
        const int32_t *p = in + c;
        if (!(p[8] | p[16] | p[24] | p[32] | p[40] | p[48] | p[56])) {
            u32 dc = (u32)p[0] << 2;
            for (int r = 0; r < 8; r++)
                ws[r * 8 + c] = dc;
            continue;
        }
        u32 z2 = (u32)p[16], z3 = (u32)p[48];
        u32 z1 = M(z2 + z3, 4433);
        u32 t2 = z1 + M(z3, -15137), t3 = z1 + M(z2, 6270);
        z2 = (u32)p[0];
        z3 = (u32)p[32];
        u32 t0 = (z2 + z3) << 13, t1 = (z2 - z3) << 13;
        u32 t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
        t0 = (u32)p[56];
        t1 = (u32)p[40];
        t2 = (u32)p[24];
        t3 = (u32)p[8];
        z1 = t0 + t3;
        z2 = t1 + t2;
        z3 = t0 + t2;
        u32 z4 = t1 + t3, z5 = M(z3 + z4, 9633);
        t0 = M(t0, 2446);
        t1 = M(t1, 16819);
        t2 = M(t2, 25172);
        t3 = M(t3, 12299);
        z1 = M(z1, -7373);
        z2 = M(z2, -20995);
        z3 = M(z3, -16069) + z5;
        z4 = M(z4, -3196) + z5;
        t0 += z1 + z3;
        t1 += z2 + z4;
        t2 += z2 + z3;
        t3 += z1 + z4;
        ws[c] = DS(t10 + t3, 11);
        ws[56 + c] = DS(t10 - t3, 11);
        ws[8 + c] = DS(t11 + t2, 11);
        ws[48 + c] = DS(t11 - t2, 11);
        ws[16 + c] = DS(t12 + t1, 11);
        ws[40 + c] = DS(t12 - t1, 11);
        ws[24 + c] = DS(t13 + t0, 11);
        ws[32 + c] = DS(t13 - t0, 11);
    }
    for (int r = 0; r < 8; r++) {
        const u32 *w = ws + r * 8;
        uint8_t *o = j->blk + r * 8;
        if (!(w[1] | w[2] | w[3] | w[4] | w[5] | w[6] | w[7])) {
            uint8_t v = clamp8((int32_t)DS(w[0], 5) + 128);
            memset(o, v, 8);
            continue;
        }
        u32 z2 = w[2], z3 = w[6];
        u32 z1 = M(z2 + z3, 4433);
        u32 t2 = z1 + M(z3, -15137), t3 = z1 + M(z2, 6270);
        u32 t0 = (w[0] + w[4]) << 13, t1 = (w[0] - w[4]) << 13;
        u32 t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
        t0 = w[7];
        t1 = w[5];
        t2 = w[3];
        t3 = w[1];
        z1 = t0 + t3;
        z2 = t1 + t2;
        z3 = t0 + t2;
        u32 z4 = t1 + t3, z5 = M(z3 + z4, 9633);
        t0 = M(t0, 2446);
        t1 = M(t1, 16819);
        t2 = M(t2, 25172);
        t3 = M(t3, 12299);
        z1 = M(z1, -7373);
        z2 = M(z2, -20995);
        z3 = M(z3, -16069) + z5;
        z4 = M(z4, -3196) + z5;
        t0 += z1 + z3;
        t1 += z2 + z4;
        t2 += z2 + z3;
        t3 += z1 + z4;
        o[0] = clamp8((int32_t)DS(t10 + t3, 18) + 128);
        o[7] = clamp8((int32_t)DS(t10 - t3, 18) + 128);
        o[1] = clamp8((int32_t)DS(t11 + t2, 18) + 128);
        o[6] = clamp8((int32_t)DS(t11 - t2, 18) + 128);
        o[2] = clamp8((int32_t)DS(t12 + t1, 18) + 128);
        o[5] = clamp8((int32_t)DS(t12 - t1, 18) + 128);
        o[3] = clamp8((int32_t)DS(t13 + t0, 18) + 128);
        o[4] = clamp8((int32_t)DS(t13 - t0, 18) + 128);
    }
}

/* The block at scale s into dst (bw = 8 >> s columns, stride). */
static void put_block(jpg_t *j, int s, uint8_t *dst, int stride)
{
    int bw = 8 >> s;
    if (s == 3) {
        dst[0] = clamp8(((int32_t)((u32)j->coef[0] + 4u) >> 3) + 128);
        return;
    }
    idct(j);
    if (s == 0) {
        for (int y = 0; y < 8; y++)
            memcpy(dst + y * stride, j->blk + y * 8, 8);
        return;
    }
    int f = 1 << s, sh = 2 * s;
    for (int y = 0; y < bw; y++)
        for (int x = 0; x < bw; x++) {
            u32 sum = 0;
            for (int a = 0; a < f; a++)
                for (int b = 0; b < f; b++)
                    sum += j->blk[(y * f + a) * 8 + x * f + b];
            dst[y * stride + x] = (uint8_t)((sum + ((1u << sh) >> 1)) >> sh);
        }
}

/* ------------------------------------------------------------------ probe */

int imgdec_scan(imgdec_scan_t *s, const uint8_t *b, size_t n, imgdec_info_t *out)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        switch (s->st) {
        case 0: /* before a marker (stray bytes skipped, as the decoder does) */
            s->st = c == 0xFF;
            break;
        case 1: /* the marker */
            if (c == 0xFF)
                break;
            s->st = 0;
            if (c == 0xD8 || (c >= 0xD0 && c <= 0xD7) || c <= 0x01)
                break;
            if (c == 0xD9 || c == 0xDA)
                return c == 0xD9 ? IMGDEC_E_CORRUPT : IMGDEC_E_FORMAT; /* no frame header before the scan */
            s->m = c;
            s->st = 2;
            break;
        case 2:
            s->len = (uint16_t)(c << 8);
            s->st = 3;
            break;
        case 3: /* the length: a frame header is read, any other segment skipped (APPn, EXIF: nothing kept) */
            s->len = (uint16_t)(s->len | c);
            if (s->len < 2)
                return IMGDEC_E_CORRUPT;
            s->len -= 2;
            s->k = 0;
            s->st = s->m >= 0xC0 && s->m <= 0xCF && s->m != 0xC4 && s->m != 0xC8 && s->m != 0xCC ? 5 : s->len ? 4 : 0;
            break;
        case 4: {
            size_t k = n - i < s->len ? n - i : s->len;
            i += k - 1;
            s->len = (uint16_t)(s->len - k);
            s->st = s->len ? 4 : 0;
            break;
        }
        default: /* 5: SOF: P, H, W, Nc */
            s->sof[s->k++] = c;
            if (s->k < 6)
                break;
            uint32_t h = (uint32_t)s->sof[1] << 8 | s->sof[2], w = (uint32_t)s->sof[3] << 8 | s->sof[4], nc = s->sof[5];
            bool sof2 = PROG && s->m == 0xC2;
            if ((s->m != 0xC0 && s->m != 0xC1 && !sof2) || s->sof[0] != 8)
                return IMGDEC_E_PROGRESSIVE; /* progressive without the decoder, lossless, arithmetic, 12-bit */
            if ((nc != 1 && nc != 3) || !h)
                return IMGDEC_E_FORMAT; /* CMYK (Adobe), DNL */
            if (!w)
                return IMGDEC_E_CORRUPT;
            if (w > IMGDEC_JPEG_MAX || h > IMGDEC_JPEG_MAX)
                return IMGDEC_E_SIZE;
            memset(out, 0, sizeof *out);
            out->format = IMG_JPEG;
            out->orient = 1;
            out->gray_bits = 8;
            out->w = out->rw = (uint16_t)w;
            out->h = out->rh = (uint16_t)h;
            out->bits = (uint8_t)(8 * nc);
            out->color = nc == 3;
            out->progressive = sof2;
            return 1;
        }
    }
    return 0;
}

int imgjpg_probe(const uint8_t *b, size_t n, imgdec_info_t *out)
{
    memset(out, 0, sizeof *out);
    out->format = IMG_JPEG;
    out->orient = 1;
    out->bits = 24;
    out->gray_bits = 8;
    imgdec_scan_t s = {0};
    int rc = imgdec_scan(&s, b, n, out);
    return rc < 0 ? rc : IMGDEC_OK; /* 0: the size comes later */
}

bool imgdec_progressive(void) { return PROG; }

uint64_t imgdec_prog_store(uint16_t w, uint16_t h, uint8_t hs, uint8_t vs, uint8_t nc, bool rgb, int s)
{
#if !PROG
    (void)w, (void)h, (void)hs, (void)vs, (void)nc, (void)rgb, (void)s;
    return UINT64_MAX;
#else
    static const uint8_t PER[4] = {128, 40, 16, 2};
    if (hs < 1 || vs < 1 || s < 0 || s > 3)
        return UINT64_MAX;
    uint64_t mxn = ((uint64_t)w + 8u * hs - 1) / (8u * hs), myn = ((uint64_t)h + 8u * vs - 1) / (8u * vs);
    uint64_t blocks = mxn * hs * myn * vs + (rgb && nc == 3 ? 2u * mxn * myn : 0);
    return blocks * PER[s];
#endif
}

size_t imgjpg_need(const imgdec_info_t *info)
{
    uint32_t w = info->rw ? info->rw : info->w;
    size_t n = sizeof(jpg_t) + (size_t)w * 16u * 3u;
#if PROG
    if (info->progressive) /* gray rows at the scale taken, 4:2:0 as the MCUs above */
        n += (size_t)imgdec_prog_store(info->w, info->h, 2, 2, 1, false, info->scale);
#endif
    return n;
}

/* ------------------------------------------------------------------ decode */

/* The next restart marker after an interval: false when the data ended instead. */
static bool restart(jpg_t *j)
{
    j->bits = 0;
    j->nb = 0;
    j->fake = 0;
    int m = j->marker;
    while (!m) {
        int c = jb(j);
        if (c < 0)
            return false;
        if (c != 0xFF)
            continue;
        while (c == 0xFF)
            c = jb(j);
        if (c < 0)
            return false;
        if (c)
            m = c;
    }
    if (m < 0xD0 || m > 0xD7) {
        j->marker = m;
        return false;
    }
    j->marker = 0;
    for (int i = 0; i < j->nc; i++)
        j->c[i].pred = 0;
    return true;
}

/* One pixel out of Y and the other two components' samples (Cb, Cr; or G, B with the Adobe transform 0): gray or RGB. */
static void pix(const jpg_t *j, uint8_t *o, int Y, int a, int b, uint8_t ch)
{
    if (ch == 1 && !j->rgbc) {
        o[0] = (uint8_t)Y;
    } else if (j->rgbc) { /* R, G, B */
        if (ch == 1) {
            o[0] = (uint8_t)((54u * (u32)Y + 183u * (u32)a + 19u * (u32)b) >> 8);
        } else {
            o[0] = (uint8_t)Y;
            o[1] = (uint8_t)a;
            o[2] = (uint8_t)b;
        }
    } else {
        int cb = a - 128, cr = b - 128;
        o[0] = clamp8(Y + ((91881 * cr + 32768) >> 16));
        o[1] = clamp8(Y + ((-22554 * cb - 46802 * cr + 32768) >> 16));
        o[2] = clamp8(Y + ((116130 * cb + 32768) >> 16));
    }
}

/* One MCU (j->mcu: Y mw x mh, chroma at 256 and 320) into the row buffer: the window's columns, gray or RGB. */
static void mcu_rows(jpg_t *j, uint8_t *rb, u32 x0, int mw, int mh, int hs, int vs, int bw, u32 wx, u32 wn, uint8_t ch)
{
    for (int yy = 0; yy < mh; yy++) {
        for (int xx = 0; xx < mw; xx++) {
            u32 x = x0 + (u32)xx;
            if (x < wx || x >= wx + wn)
                continue;
            int ci = (yy >> (vs - 1)) * bw + (xx >> (hs - 1));
            pix(j, rb + ((size_t)yy * wn + (x - wx)) * ch, j->mcu[yy * mw + xx], j->mcu[256 + ci], j->mcu[320 + ci], ch);
        }
    }
}

/* Row y (rw pixels of ch bytes at px) to the callbacks; IMGDEC_E_ABORT when one says stop. */
static int emit_row(jpg_t *j, uint8_t *px, u32 y, u32 rw, uint8_t ch)
{
    const imgdec_io_t *io = j->io;
    int stop;
    if (io->row8) {
        stop = io->row8(io->ctx, (uint16_t)y, px, (uint16_t)rw, ch);
    } else {
        for (u32 i = 0; i < rw; i++)
            px[i] >>= 4;
        stop = io->row(io->ctx, (uint16_t)y, px, (uint16_t)rw);
    }
    return stop ? IMGDEC_E_ABORT : IMGDEC_OK;
}

/* The rows of MCU row my to the callbacks. */
static int emit_rows(jpg_t *j, uint8_t *rb, u32 my, int mh, u32 rh, u32 rw, uint8_t ch)
{
    u32 y0 = my * (u32)mh;
    int rc = IMGDEC_OK;
    for (u32 r = 0; r < (u32)mh && y0 + r < rh && !rc; r++)
        rc = emit_row(j, rb + (size_t)r * rw * ch, y0 + r, rw, ch);
    return rc;
}

#if PROG
/* ------------------------------------------------------------------ progressive */

static int bit1(jpg_t *j)
{
    if (j->nb < 1)
        fill(j);
    int v = (int)(j->bits >> 31);
    j->bits <<= 1;
    j->nb--;
    return v;
}

static u32 rbits(jpg_t *j, int n)
{
    if (!n)
        return 0;
    if (j->nb < n)
        fill(j);
    u32 v = j->bits >> (32 - n);
    j->bits <<= n;
    j->nb -= n;
    return v;
}

/* Blocks a row, rows of component i's own area (a non-interleaved scan's). */
static u32 comp_bw(const jpg_t *j, int i)
{
    u32 cw = ((u32)j->w * j->c[i].hs + j->c[0].hs - 1) / j->c[0].hs;
    return (cw + 7) / 8;
}
static u32 comp_bh(const jpg_t *j, int i)
{
    u32 cv = ((u32)j->h * j->c[i].vs + j->c[0].vs - 1) / j->c[0].vs;
    return (cv + 7) / 8;
}

static bool nzk(const int16_t *co, const uint8_t *nz, int k)
{
    return nz ? (nz[k >> 3] >> (k & 7)) & 1 : co[ZZ[k]] != 0;
}

static void setk(const jpg_t *j, int16_t *co, uint8_t *nz, int k, int32_t v)
{
    if (nz)
        nz[k >> 3] |= (uint8_t)(1u << (k & 7));
    int i = j->kmap[k];
    if (i >= 0)
        co[i] = (int16_t)(uint16_t)(u32)v;
}

/* A correction bit for a coefficient already nonzero (AC refinement). */
static void refine(jpg_t *j, int16_t *co, int k, int p1)
{
    if (!bit1(j))
        return;
    int i = j->kmap[k];
    if (i < 0)
        return;
    int v = co[i];
    if (!(v & p1))
        co[i] = (int16_t)(uint16_t)(u32)(v + (v >= 0 ? p1 : -p1));
}

/* One block of the scan (libjpeg jdphuff.c). false: a bad code. */
static bool prog_block(jpg_t *j, int ci, u32 bx, u32 by)
{
    jcomp_t *c = &j->c[ci];
    int16_t *co = NULL;
    uint8_t *nz = NULL;
    if (j->co[ci] && by >= j->r0[ci] && by < j->r1[ci]) { /* in the band: kept */
        size_t b = (size_t)(by - j->r0[ci]) * j->cbw[ci] + bx;
        co = j->co[ci] + b * j->K;
        nz = j->nz[ci] ? j->nz[ci] + b * 8u : NULL;
    }
    if (j->ss == 0) { /* DC */
        if (!j->ah) {
            int t = hdec(j, &j->ht[c->td]);
            if (t < 0 || t > 11)
                return false;
            c->pred = (int32_t)((u32)c->pred + (u32)recv(j, t));
            if (co)
                co[0] = (int16_t)(uint16_t)((u32)c->pred << j->al);
        } else if (bit1(j) && co) {
            co[0] = (int16_t)(uint16_t)((u32)(uint16_t)co[0] | (1u << j->al));
        }
        return true;
    }
    const jhuff_t *ac = &j->ht[c->ta];
    if (!j->ah) { /* AC first */
        if (j->eobrun) {
            j->eobrun--;
            return true;
        }
        for (int k = j->ss; k <= j->se; k++) {
            int rs = hdec(j, ac);
            if (rs < 0)
                return false;
            int r = rs >> 4, s = rs & 15;
            if (s) {
                k += r;
                if (k > j->se)
                    return false;
                setk(j, co, nz, k, (int32_t)((u32)recv(j, s) << j->al));
            } else if (r == 15) {
                k += 15;
            } else {
                j->eobrun = (1u << r) + rbits(j, r) - 1u;
                break;
            }
        }
        return true;
    }
    /* AC refinement */
    int p1 = 1 << j->al, k = j->ss;
    if (!j->eobrun) {
        for (; k <= j->se; k++) {
            int rs = hdec(j, ac);
            if (rs < 0)
                return false;
            int r = rs >> 4, s = rs & 15, v = 0;
            if (s) {
                if (s != 1)
                    return false;
                v = bit1(j) ? p1 : -p1;
            } else if (r != 15) {
                j->eobrun = (1u << r) + rbits(j, r);
                break;
            }
            do {
                if (nzk(co, nz, k))
                    refine(j, co, k, p1);
                else if (--r < 0)
                    break;
                k++;
            } while (k <= j->se);
            if (v) {
                if (k > j->se)
                    return false;
                setk(j, co, nz, k, v);
            }
        }
    }
    if (j->eobrun) {
        for (; k <= j->se; k++)
            if (nzk(co, nz, k))
                refine(j, co, k, p1);
        j->eobrun--;
    }
    return true;
}

/* Past the entropy data to the next marker that is not RSTn (in j->marker; 0: the data ended). */
static void next_marker(jpg_t *j)
{
    int m = j->marker;
    for (;;) {
        while (!m) {
            int c = jb(j);
            if (c < 0)
                break;
            if (c != 0xFF)
                continue;
            while (c == 0xFF)
                c = jb(j);
            if (c < 0)
                break;
            m = c;
        }
        if (m < 0xD0 || m > 0xD7)
            break;
        m = 0;
    }
    j->marker = m;
}

/* Store rows a unit of rows takes of component i: Y block rows for gray rows of a color file (only Y kept), else MCU
 * rows (vs block rows of each component). */
static u32 unit_rows(const jpg_t *j, int i) { return j->ymode ? 1u : j->c[i].vs; }
static u32 prog_units(const jpg_t *j)
{
    u32 myn = ((u32)j->h + 8u * j->c[0].vs - 1) / (8u * j->c[0].vs);
    return j->ymode ? myn * j->c[0].vs : myn;
}

/* The coefficient store for scale s and `ub` units of rows: Y, and chroma for RGB rows (all). */
static size_t prog_bytes(jpg_t *j, bool all, int s, u32 ub, bool set)
{
    static const uint8_t K[4] = {64, 16, 4, 1}, PER[4] = {128, 40, 16, 2};
    u32 mxn = ((u32)j->w + 8u * j->c[0].hs - 1) / (8u * j->c[0].hs);
    int kept = all && j->nc == 3 ? 3 : 1;
    size_t off = 0;
    for (int i = 0; i < kept; i++) {
        size_t n = (size_t)mxn * j->c[i].hs * ub * unit_rows(j, i);
        if (set) {
            j->cbw[i] = mxn * j->c[i].hs;
            j->co[i] = (int16_t *)(void *)(j->store + off);
            j->nz[i] = s == 1 || s == 2 ? j->store + off + n * K[s] * 2u : NULL;
        }
        off += n * PER[s];
    }
    if (set) {
        j->K = K[s];
        for (int k = 0; k < 64; k++) {
            int pos = ZZ[k], r = pos >> 3, c = pos & 7, n = 8 >> s;
            j->kmap[k] = (int8_t)(s == 0 ? pos : s == 3 ? (k == 0 ? 0 : -1) : (r < n && c < n ? r * n + c : -1));
        }
    }
    return off;
}

static int prog_alloc(jpg_t *j, bool all, int s, u32 ub)
{
    size_t n = prog_bytes(j, all, s, ub, false);
    j->store = j->io->alloc(j->io->ctx, n ? n : 1);
    if (!j->store)
        return IMGDEC_E_NOMEM;
    j->store_n = n;
    j->ub = ub;
    prog_bytes(j, all, s, ub, true);
    return IMGDEC_OK;
}

/* A scan of a file decoded in bands (assets.md "JPEG" Progressive bands): its header, the tables in effect (file
 * offsets of their DHT definitions) and where its data was left, at the start of `row` (MCU rows when interleaved, else
 * its component's block rows). */
typedef struct jscan {
    u32 pos, bits, eobrun, ht[4];
    int32_t pred[3];
    uint16_t row, ri, fake;
    int8_t nb;
    uint8_t marker, bad, sn, sc[3], tt[3], ss, se, ah, al;
} jscan_t;

static void st_save(jpg_t *j, jscan_t *s, u32 row)
{
    s->pos = jpos(j);
    s->bits = j->bits;
    s->nb = (int8_t)j->nb;
    s->fake = (uint16_t)(j->fake < 0xFFFF ? j->fake : 0xFFFF);
    s->marker = (uint8_t)j->marker;
    s->bad = j->bad;
    s->eobrun = j->eobrun;
    for (int i = 0; i < 3; i++)
        s->pred[i] = j->c[i].pred;
    s->row = (uint16_t)row;
}

/* The scan whose header was just read, from the start of its data. */
static void scan_rec(jpg_t *j, jscan_t *s)
{
    memset(s, 0, sizeof *s);
    s->pos = jpos(j);
    s->sn = j->sn;
    for (int i = 0; i < j->sn; i++) {
        s->sc[i] = j->sc[i];
        s->tt[i] = (uint8_t)(j->c[j->sc[i]].td << 4 | (j->c[j->sc[i]].ta - 2));
    }
    s->ss = j->ss;
    s->se = j->se;
    s->ah = j->ah;
    s->al = j->al;
    s->ri = j->ri;
    memcpy(s->ht, j->ht_at, sizeof s->ht);
}

/* Every scan of the file (j at the first scan's data): counted, then recorded (j->scans). */
static int prog_index(jpg_t *j)
{
    jscan_t first;
    scan_rec(j, &first);
    int n = 1, rc;
    for (;; n++) {
        next_marker(j);
        if ((rc = headers(j, NULL)) == J_EOI)
            break;
        if (rc || n >= PROG_SCANS)
            return rc ? rc : IMGDEC_E_CORRUPT;
    }
    if (j->err)
        return j->err;
    j->scans = j->io->alloc(j->io->ctx, sizeof(jscan_t) * (size_t)n);
    if (!j->scans)
        return IMGDEC_E_NOMEM;
    j->nscans = (uint8_t)n;
    j->scans[0] = first;
    if (!jseek(j, first.pos))
        return IMGDEC_E_IO;
    j->marker = 0;
    for (int k = 1; k < n; k++) {
        next_marker(j);
        if ((rc = headers(j, NULL)))
            return rc == J_EOI ? IMGDEC_E_CORRUPT : rc;
        scan_rec(j, &j->scans[k]);
    }
    return IMGDEC_OK;
}

/* Whether scale s fits (1; 0: not; < 0 an error): the whole store within prog_max, read once as the file comes; else with io.prog_bands and seek, bands of rows whose store fits mem_max (else prog_max) beside `fixed` bytes
 * and the scan index. */
static int prog_fit(jpg_t *j, bool all, int s, size_t fixed)
{
    const imgdec_io_t *io = j->io;
    j->ymode = !all && j->nc == 3;
    u32 U = prog_units(j);
    size_t per = prog_bytes(j, all, s, 1, false);
    uint64_t whole = (uint64_t)per * U;
    if (!j->scans && whole <= io->prog_max && (!io->mem_max || fixed <= io->mem_max)) /* prog_max: the store's own */
        return prog_alloc(j, all, s, U) == IMGDEC_OK;
    if (!io->prog_bands || !io->seek)
        return 0;
    if (!j->scans) {
        int rc = prog_index(j);
        if (rc)
            return rc;
    }
    fixed += sizeof(jscan_t) * j->nscans;
    uint64_t budget = io->mem_max ? (io->mem_max > fixed ? io->mem_max - fixed : 0) : io->prog_max;
    uint64_t ub = budget / per;
    return ub && prog_alloc(j, all, s, ub < U ? (u32)ub : U) == IMGDEC_OK;
}

/* The scan into the store: after its SOS (sc NULL: the file read once, then past its data), or resumed where the band
 * before left it (sc) for the rows of this band, and left again at the first row the next band needs. */
static int prog_scan(jpg_t *j, jscan_t *sc)
{
    if (sc) { /* its header */
        j->sn = sc->sn;
        for (int i = 0; i < sc->sn; i++) {
            j->sc[i] = sc->sc[i];
            j->c[sc->sc[i]].td = (uint8_t)(sc->tt[i] >> 4);
            j->c[sc->sc[i]].ta = (uint8_t)(2 + (sc->tt[i] & 15));
        }
        j->ss = sc->ss;
        j->se = sc->se;
        j->ah = sc->ah;
        j->al = sc->al;
        j->ri = sc->ri;
    }
    bool dc = j->ss == 0, any = false;
    for (int i = 0; i < j->sn; i++)
        any |= j->co[j->sc[i]] != NULL;
    if (!any || (!dc && j->K == 1)) { /* chroma for gray rows; AC at 1/8 */
        if (!sc)
            next_marker(j);
        return IMGDEC_OK;
    }
    for (int i = 0; i < j->sn; i++) {
        jcomp_t *c = &j->c[j->sc[i]];
        int t = dc ? (j->ah ? -1 : c->td) : c->ta;
        if (sc && t >= 0 && j->ht_at[t] != sc->ht[t]) { /* the table this scan was coded with, again */
            u32 rest = 0xFFFF;
            if (!jseek(j, sc->ht[t]))
                return IMGDEC_E_IO;
            int rc = dht_one(j, &rest);
            if (rc)
                return rc;
        }
        if (t >= 0 && !j->ht[t].ok)
            return IMGDEC_E_CORRUPT;
    }
    if (sc) {
        if (!jseek(j, sc->pos))
            return IMGDEC_E_IO;
        j->bits = sc->bits;
        j->nb = sc->nb;
        j->fake = sc->fake;
        j->marker = sc->marker;
        j->bad = sc->bad;
        j->eobrun = sc->eobrun;
        for (int i = 0; i < 3; i++)
            j->c[i].pred = sc->pred[i];
    } else {
        j->bits = 0;
        j->nb = 0;
        j->fake = 0;
        j->marker = 0;
        j->bad = false;
        j->eobrun = 0;
        for (int i = 0; i < 3; i++)
            j->c[i].pred = 0;
    }
    u32 mxn = ((u32)j->w + 8u * j->c[0].hs - 1) / (8u * j->c[0].hs);
    u32 myn = ((u32)j->h + 8u * j->c[0].vs - 1) / (8u * j->c[0].vs);
    bool il = j->sn > 1;
    int c0 = j->sc[0];
    u32 ubw = il ? mxn : comp_bw(j, c0), rows = il ? myn : comp_bh(j, c0);
    u32 first = 0, last = rows, save = rows;
    if (sc) { /* the rows holding the band's (MCU rows: those with a kept row; left at the first one not wholly used) */
        first = sc->row;
        if (il) {
            last = 0;
            for (int i = 0; i < 3; i++)
                if (j->co[i]) {
                    u32 v = j->c[i].vs, e = (j->r1[i] + v - 1) / v;
                    last = e > last ? e : last;
                    save = j->r1[i] / v < save ? j->r1[i] / v : save;
                }
            last = last < rows ? last : rows;
        } else {
            last = j->r1[c0] < rows ? j->r1[c0] : rows;
            save = last;
        }
    }
    for (u32 u = first * ubw; u < last * ubw; u++) {
        if (sc && u == save * ubw)
            st_save(j, sc, save);
        if (j->ri && u && u % j->ri == 0) {
            if (!restart(j))
                return j->err ? j->err : IMGDEC_E_CORRUPT;
            j->bad = false;
            j->eobrun = 0;
        }
        if (j->bad)
            continue;
        u32 ux = u % ubw, uy = u / ubw;
        if (!il) {
            j->bad = !prog_block(j, c0, ux, uy);
        } else {
            for (int i = 0; i < j->sn && !j->bad; i++) {
                int ci = j->sc[i], hs = j->c[ci].hs, vs = j->c[ci].vs;
                for (int by = 0; by < vs && !j->bad; by++)
                    for (int bx = 0; bx < hs && !j->bad; bx++)
                        j->bad = !prog_block(j, ci, ux * (u32)hs + (u32)bx, uy * (u32)vs + (u32)by);
            }
        }
        if (!j->ri && (j->bad || j->nb < 8 * j->fake))
            return j->err ? j->err : IMGDEC_E_CORRUPT; /* bad data, or past its end: no restart to resync at */
    }
    if (sc && save >= last)
        st_save(j, sc, last);
    /* bits past the end of the data were used, or the file ended in the scan (fill() then made up an EOI: a whole
     * file's last scan meets its EOI before the end) */
    if (last == rows && ((!j->bad && j->nb < 8 * j->fake) || (!sc && j->eof)))
        return j->err ? j->err : IMGDEC_E_CORRUPT;
    if (!sc)
        next_marker(j);
    return j->err;
}

/* The band's blocks (store rows r0 .. r1 of each component kept) into pixels in place: block b's (8 >> s)^2 pixels over
 * the first bytes of its own coefficients (after every block before it), only blocks that reach the window's columns;
 * then its image rows y0 .. y1 - 1, the window's columns, to the callbacks. */
static int band_rows(jpg_t *j, int s, uint8_t *rb, u32 y0, u32 y1, u32 wx, u32 wn, uint8_t ch)
{
    int sh = 3 - s;
    u32 bw = 1u << sh, pb = bw * bw, m = bw - 1;
    int kept = j->co[1] ? 3 : 1, hs = j->c[0].hs, vs = j->c[0].vs;
    for (int i = 0; i < kept; i++) {
        u32 n = (j->r1[i] - j->r0[i]) * j->cbw[i], f = (u32)(hs / j->c[i].hs) << sh;
        const uint16_t *q = j->q[j->c[i].tq];
        for (u32 b = 0; b < n; b++) {
            u32 x0 = b % j->cbw[i] * f;
            if (x0 >= wx + wn || x0 + f <= wx)
                continue;
            const int16_t *co = j->co[i] + (size_t)b * j->K;
            memset(j->coef, 0, sizeof j->coef);
            for (int k = 0; k < 64; k++)
                if (j->kmap[k] >= 0)
                    j->coef[ZZ[k]] = (int32_t)((u32)(int32_t)co[j->kmap[k]] * q[k]);
            put_block(j, s, (uint8_t *)j->co[i] + (size_t)b * pb, (int)bw);
        }
    }
    const uint8_t *P0 = (const uint8_t *)j->co[0], *P1 = (const uint8_t *)j->co[1], *P2 = (const uint8_t *)j->co[2];
    int rc = IMGDEC_OK;
    for (u32 y = y0; y < y1 && !rc; y++) {
        u32 ly = y - (j->r0[0] << sh), cy = kept == 3 ? (y >> (vs - 1)) - (j->r0[1] << sh) : 0;
        const uint8_t *yr = P0 + (size_t)(ly >> sh) * j->cbw[0] * pb + (ly & m) * bw;
        size_t cr = (size_t)(cy >> sh) * j->cbw[1] * pb + (cy & m) * bw;
        uint8_t *o = rb;
        for (u32 x = wx; x < wx + wn; x++, o += ch) {
            size_t at = 0;
            if (kept == 3) {
                u32 cx = x >> (hs - 1);
                at = cr + (size_t)(cx >> sh) * pb + (cx & m);
            }
            pix(j, o, yr[(size_t)(x >> sh) * pb + (x & m)], kept == 3 ? P1[at] : 0, kept == 3 ? P2[at] : 0, ch);
        }
        rc = emit_row(j, rb, y, wn, ch);
    }
    return rc;
}
#endif

int imgjpg_decode(const imgdec_io_t *io, imgdec_info_t *info, const uint8_t *pre, size_t pre_n, uint32_t pre_off)
{
    if (pre_n > JIN)
        return IMGDEC_E_CORRUPT;
    jpg_t *j = io->alloc(io->ctx, sizeof *j);
    if (!j)
        return IMGDEC_E_NOMEM;
    memset(j, 0, sizeof *j);
    j->io = io;
    memcpy(j->in, pre, pre_n);
    j->n = pre_n;
    j->off = pre_off + (u32)pre_n;
    uint8_t *rb = NULL;
    imgdec_exif_t ex;
    memset(&ex, 0, sizeof ex);
    int rc = headers(j, &ex);
    if (rc)
        goto out;
    /* sampling: luma 1 or 2 each way, chroma 1x1 (all equal counts as 1x1) */
    jcomp_t *c = j->c;
    if (j->nc == 1 || (c[0].hs == c[1].hs && c[0].hs == c[2].hs && c[0].vs == c[1].vs && c[0].vs == c[2].vs)) {
        for (int i = 0; i < j->nc; i++)
            c[i].hs = c[i].vs = 1;
    }
    if (c[0].hs < 1 || c[0].hs > 2 || c[0].vs < 1 || c[0].vs > 2 ||
        (j->nc == 3 && (c[1].hs != 1 || c[1].vs != 1 || c[2].hs != 1 || c[2].vs != 1))) {
        rc = IMGDEC_E_FORMAT;
        goto out;
    }
    for (int i = 0; i < j->nc; i++)
        if (!j->qok[c[i].tq] || (!j->prog && (!j->ht[c[i].td].ok || !j->ht[c[i].ta].ok))) {
            rc = IMGDEC_E_CORRUPT; /* progressive: each scan checks the tables it uses */
            goto out;
        }
    bool rgb = io->row8 && io->want_rgb && j->nc == 3;
    uint8_t ch = rgb ? 3 : 1;
    j->rgbc = j->rgbc && j->nc == 3;
    bool all = rgb || j->rgbc; /* every component decoded */
    int hs = c[0].hs, vs = c[0].vs;
    int s = 0;
    uint16_t minw = io->min_w, minh = io->min_h;
    if (io->row8 && io->want)
        io->want(io->ctx, j->w, j->h, ex.orient ? ex.orient : 1, &minw, &minh);
    if (io->row8 && (minw || minh)) {
        s = 3;
        while (s > 0 && (((u32)j->w + (1u << s) - 1) >> s < minw || ((u32)j->h + (1u << s) - 1) >> s < minh))
            s--;
    }
#if PROG
    /* prog_dc (assets.md "JPEG" Progressive DC): no store and no file to resume: the first scan when it is the DC of
     * every component, interleaved, and its 1/8 rows are not much smaller than the wanted ones */
    if (j->prog && io->row8 && io->prog_dc && !io->prog_max && !(io->prog_bands && io->seek)) {
        bool std = j->ss == 0 && j->se == 0 && j->ah == 0 && j->sn == j->nc;
        for (int i = 0; std && i < j->nc; i++)
            std = j->sc[i] == i;
        u32 rw8 = ((u32)j->w + 7) >> 3, rh8 = ((u32)j->h + 7) >> 3;
        if (!std || rw8 * 8 < minw * 5u || rh8 * 8 < minh * 5u) {
            rc = IMGDEC_E_PROGRESSIVE;
            goto out;
        }
        for (int i = 0; i < j->nc; i++)
            std = std && j->ht[c[i].td].ok;
        if (!std) {
            rc = IMGDEC_E_CORRUPT;
            goto out;
        }
        j->dcs = true;
        s = 3;
    }
#endif
    uint16_t wx = 0, wn = 0; /* the column window (io->window) */
    for (;;) {
        u32 rw = ((u32)j->w + (1u << s) - 1) >> s;
        wx = 0;
        wn = (uint16_t)rw;
        if (io->row8 && io->window) {
            io->window(io->ctx, (uint16_t)rw, (uint16_t)(((u32)j->h + (1u << s) - 1) >> s), &wx, &wn);
            if (wx >= rw || !wn)
                wx = 0, wn = 1;
            if ((u32)wx + wn > rw)
                wn = (uint16_t)(rw - wx);
        }
        size_t need = sizeof *j + (size_t)wn * (size_t)((vs * 8) >> s) * ch;
        int fits = !io->mem_max || need <= io->mem_max;
#if PROG
        if (j->prog && !j->dcs) /* the store now: a failed allocation takes a larger scale too (row8) */
            fits = prog_fit(j, all, s, sizeof *j + (size_t)wn * ch);
        if (fits < 0) {
            rc = fits;
            goto out;
        }
#endif
        if (fits)
            break;
        if (!io->row8 || s == 3) {
            rc = IMGDEC_E_NOMEM;
            goto out;
        }
        s++;
    }
    info->format = IMG_JPEG;
    info->w = j->w;
    info->h = j->h;
    info->bits = (uint8_t)(8 * j->nc);
    info->gray_bits = 8;
    info->color = j->nc == 3;
    info->orient = ex.orient ? ex.orient : 1;
    info->scale = (uint8_t)s;
    info->rw = (uint16_t)(((u32)j->w + (1u << s) - 1) >> s);
    info->rh = (uint16_t)(((u32)j->h + (1u << s) - 1) >> s);
    info->progressive = j->prog;
    if (io->exif)
        *io->exif = ex;
    if (io->info && io->info(io->ctx, info)) {
        rc = IMGDEC_E_ABORT;
        goto out;
    }
    int bw = 8 >> s, mw = hs * bw, mh = vs * bw;
    u32 rh = info->rh, rw = wn; /* rb holds the window's columns wx .. wx + wn - 1 */
    rb = io->alloc(io->ctx, (size_t)rw * (size_t)(j->prog && !j->dcs ? 1 : mh) * ch); /* progressive: a row */
    if (!rb) {
        rc = IMGDEC_E_NOMEM;
        goto out;
    }
    u32 mxn = ((u32)j->w + 8u * hs - 1) / (8u * hs), myn = ((u32)j->h + 8u * vs - 1) / (8u * vs);
#if PROG
    if (j->prog && !j->dcs) { /* per band (one: the whole image): every scan into the store, then its rows */
        u32 U = prog_units(j), sh = (u32)(3 - s);
        for (u32 u0 = 0; u0 < U && !rc; u0 += j->ub) {
            u32 u1 = U - u0 < j->ub ? U : u0 + j->ub;
            for (int i = 0; i < 3; i++) {
                j->r0[i] = u0 * unit_rows(j, i);
                j->r1[i] = u1 * unit_rows(j, i);
            }
            memset(j->store, 0, j->store_n);
            for (int k = 0; j->scans && k < j->nscans && !rc; k++)
                rc = prog_scan(j, &j->scans[k]);
            for (int k = 0; !j->scans && !rc; k++) { /* read once, in order */
                if (k >= PROG_SCANS) /* libjpeg's scripts take 10 or so */
                    rc = IMGDEC_E_CORRUPT;
                else if (!(rc = prog_scan(j, NULL)) && (rc = headers(j, NULL)) == J_EOI)
                    break;
            }
            if (rc == J_EOI)
                rc = IMGDEC_OK;
            u32 y1 = j->r1[0] << sh;
            if (!rc)
                rc = band_rows(j, s, rb, j->r0[0] << sh, y1 < rh ? y1 : rh, wx, wn, ch);
        }
        goto out;
    }
#endif
    u32 mcus = 0;
    bool bad = false, lost = false;
    for (u32 my = 0; my < myn && !rc; my++) {
        for (u32 mx = 0; mx < mxn; mx++) {
            if (j->ri && mcus && mcus % j->ri == 0) {
                if (restart(j))
                    bad = false;
                else
                    lost = true;
            }
            mcus++;
            u32 x0 = mx * (u32)mw;
            bool in = x0 + (u32)mw > wx && x0 < (u32)wx + wn; /* outside the window: entropy decoded only */
            if (bad || lost) {
                memset(j->mcu, 128, sizeof j->mcu);
            } else {
                for (int by = 0; by < vs && !bad; by++)
                    for (int bx = 0; bx < hs && !bad; bx++) {
                        if (!dblock(j, &c[0], in, s == 3))
                            bad = true;
                        else if (in)
                            put_block(j, s, j->mcu + by * bw * mw + bx * bw, mw);
                    }
                for (int i = 1; i < j->nc && !bad; i++) {
                    if (!dblock(j, &c[i], all && in, s == 3))
                        bad = true;
                    else if (all && in)
                        put_block(j, s, j->mcu + 256 + (i - 1) * 64, bw);
                }
                if (bad)
                    memset(j->mcu, 128, sizeof j->mcu);
            }
            if (in)
                mcu_rows(j, rb, x0, mw, mh, hs, vs, bw, wx, wn, ch);
        }
        /* truncated (bits past the end of the file were used), or bad data with no restart to resync at */
        if ((j->eof && j->nb < 8 * j->fake) || lost || (!j->ri && (bad || j->nb < 8 * j->fake))) {
            rc = j->err ? j->err : IMGDEC_E_CORRUPT;
            break;
        }
        rc = emit_rows(j, rb, my, mh, rh, rw, ch);
    }
out:
    if (j->store)
        io->free(io->ctx, j->store);
    if (j->scans)
        io->free(io->ctx, j->scans);
    if (rb)
        io->free(io->ctx, rb);
    io->free(io->ctx, j);
    return rc;
}
