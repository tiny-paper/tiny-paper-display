/*
 * imgdec: WebP (docs/spec/assets.md "Images", "WebP"). The RIFF container (simple and extended), lossy images: the
 * VP8 key frame of RFC 6386 decoded one macroblock row at a time (boolean entropy decoder, intra prediction, the
 * 4x4 transforms, the loop filter) into a row cache that trails the filter by 0, 2 or 8 rows; bit-exact with
 * libwebp's dwebp (its Y plane, and RGB with -nofancy: chroma sampled at the pixel's 2x2 block).
 * Gray rows from Y: clip8((Y * 19077 >> 8) - 1160) >> 6, libwebp's R and B at neutral chroma (limited range).
 * Partition 0 and every token partition are read through their own small buffers (with io.seek), or partition 0 held
 * in RAM and the token partitions read in order (a stream, one token partition; more of them: held in RAM too).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "imgwebp.h"
#include "imgwebp_tab.h"

typedef uint32_t u32;

/* ------------------------------------------------------------------ the file */

#define WIN 256

typedef struct {
    const imgdec_io_t *io;
    const uint8_t *pre;
    size_t pre_n, pre_p;
    u32 fpos; /* file offset of the next byte read */
    bool cut; /* the file ended before a range did */
    int err;
} wfile_t;

/* Up to n bytes from where the file is: the bytes read before the decoder started, then io.read. */
static size_t f_read(wfile_t *f, uint8_t *b, size_t n)
{
    size_t got = 0;
    while (got < n && f->pre_p < f->pre_n)
        b[got++] = f->pre[f->pre_p++];
    while (got < n) {
        long r = f->io->read(f->io->ctx, b + got, n - got);
        if (r < 0)
            f->err = IMGDEC_E_IO;
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    f->fpos += (u32)got;
    return got;
}

/* The next read starts at off: already there, a seek, or (a stream) forward by reading. */
static bool f_seek(wfile_t *f, u32 off)
{
    if (off == f->fpos)
        return true;
    if (f->io->seek) {
        if (f->io->seek(f->io->ctx, off))
            return false;
        f->fpos = off;
        f->pre_p = f->pre_n; /* those bytes are behind */
        return true;
    }
    uint8_t sk[64];
    while (f->fpos < off) {
        size_t k = off - f->fpos < sizeof sk ? off - f->fpos : sizeof sk;
        if (f_read(f, sk, k) != k)
            return false;
    }
    return f->fpos == off;
}

/* A byte range of the file (a partition), read through its own buffer, or held in RAM. */
typedef struct {
    wfile_t *f;
    const uint8_t *mem;
    u32 off, end;
    uint16_t n, p;
    uint8_t buf[WIN];
} wsrc_t;

/* The next byte, -1 past the range (a cut file sets f->cut). */
static int s_byte(wsrc_t *s)
{
    if (s->p < s->n)
        return s->buf[s->p++];
    if (s->off >= s->end)
        return -1;
    if (s->mem)
        return s->mem[s->off++];
    size_t k = s->end - s->off < WIN ? s->end - s->off : WIN;
    if (!f_seek(s->f, s->off)) {
        s->f->cut = true;
        s->off = s->end;
        return -1;
    }
    size_t got = f_read(s->f, s->buf, k);
    s->off += (u32)got;
    if (got < k) {
        s->f->cut = true;
        s->end = s->off;
    }
    s->n = (uint16_t)got;
    s->p = 0;
    return got ? s->buf[s->p++] : -1;
}

/* ------------------------------------------------------------------ boolean decoder (RFC 6386 7.3) */

typedef struct {
    wsrc_t *s;
    u32 value;
    int range, bits;
} bd_t;

static u32 nb(wsrc_t *s)
{
    int c = s_byte(s);
    return c < 0 ? 0u : (u32)c; /* past the end: zeros */
}

static void bd_init(bd_t *d, wsrc_t *s)
{
    d->s = s;
    d->value = nb(s) << 8;
    d->value |= nb(s);
    d->range = 255;
    d->bits = 0;
}

static int bd_bit(bd_t *d, int prob)
{
    u32 split = 1 + (((u32)(d->range - 1) * (u32)prob) >> 8), big = split << 8;
    int b;
    if (d->value >= big) {
        b = 1;
        d->range -= (int)split;
        d->value -= big;
    } else {
        b = 0;
        d->range = (int)split;
    }
    while (d->range < 128) {
        d->value <<= 1;
        d->range <<= 1;
        if (++d->bits == 8) {
            d->bits = 0;
            d->value |= nb(d->s);
        }
    }
    return b;
}

static u32 bd_lit(bd_t *d, int n)
{
    u32 v = 0;
    while (n--)
        v = v << 1 | (u32)bd_bit(d, 128);
    return v;
}

static int bd_signed(bd_t *d, int n)
{
    int v = (int)bd_lit(d, n);
    return bd_bit(d, 128) ? -v : v;
}

/* ------------------------------------------------------------------ VP8 state */

enum { B_DC, B_TM, B_VE, B_HE, B_RD, B_VR, B_LD, B_VL, B_HD, B_HU, DC_NOTOP = 4, DC_NOLEFT = 5, DC_NOTOPLEFT = 6 };

#define BPS 32
#define YOFF (BPS + 8) /* the work block: row -1 and column -1 of Y, U, V */

typedef struct {
    int16_t y1[2], y2[2], uv[2];
} wq_t;

typedef struct {
    uint8_t limit, ilevel, inner, hev;
} wf_t;

typedef struct {
    uint8_t nz, nz_dc;
} wnz_t;

typedef struct {
    wfile_t *f;
    const imgdec_io_t *io;
    u32 w, h, mbw, mbh;
    int ftype, extra;          /* filter: 0 none, 1 simple, 2 normal; rows the output trails by */
    bool seg_on, seg_map, seg_abs, lf_delta;
    int8_t seg_q[4], seg_lf[4], ref_lf0, mode_lf0;
    int level, sharp;
    uint8_t seg_p[3];
    int skip_p;                /* -1 no skip flags */
    uint8_t parts;             /* token partitions */
    uint8_t coef[4 * 8 * 3 * 11];
    wq_t q[4];
    wf_t fs[4][2];             /* filter strengths per segment and i4x4 */
    bd_t b0, bt[8];
    wsrc_t *src;               /* 1 + parts sources */
    uint8_t *intra_t;          /* mbw * 4 */
    uint8_t intra_l[4];
    wnz_t *nz_t, nz_l;
    uint8_t *top;              /* unfiltered bottom row of each MB of the row above: Y 16, U 8, V 8 */
    wf_t *fi;                  /* the row's filter info */
    uint8_t *cy, *cu, *cv;     /* row caches (filtered), with `extra` rows above */
    u32 cys, cuvs;             /* their strides */
    uint8_t work[BPS * 17 + BPS * 9 * 2];
    int16_t coeffs[384];
    uint8_t modes[16], uvmode;
    bool i4, skip;
    uint8_t seg;
    uint32_t nzy, nzuv;
    uint8_t *row;              /* an output row (gray or RGB) */
    uint8_t gray[256];         /* Y to gray */
    const uint8_t *alpha;      /* the ALPH plane (w x h), NULL none */
    bool rgb;
} vp8_t;

static const uint8_t ZIGZAG[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
static const uint8_t BANDS[17] = {0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 0};
static const uint8_t CAT3[] = {173, 148, 140, 0}, CAT4[] = {176, 155, 140, 135, 0},
                     CAT5[] = {180, 157, 141, 134, 130, 0},
                     CAT6[] = {254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129, 0};
static const uint8_t *const CATS[4] = {CAT3, CAT4, CAT5, CAT6};

static const uint8_t *prob(const vp8_t *v, int t, int band, int ctx) { return v->coef + ((t * 8 + band) * 3 + ctx) * 11; }

/* ------------------------------------------------------------------ headers (RFC 6386 9) */

static int clampq(int q, int hi) { return q < 0 ? 0 : q > hi ? hi : q; }

static void headers(vp8_t *v)
{
    bd_t *d = &v->b0;
    bd_bit(d, 128); /* color space */
    bd_bit(d, 128); /* clamping type */
    v->seg_on = bd_bit(d, 128);
    v->seg_abs = true;
    memset(v->seg_p, 255, sizeof v->seg_p);
    if (v->seg_on) {
        v->seg_map = bd_bit(d, 128);
        if (bd_bit(d, 128)) { /* update the segment data */
            v->seg_abs = bd_bit(d, 128);
            for (int s = 0; s < 4; s++)
                v->seg_q[s] = (int8_t)(bd_bit(d, 128) ? bd_signed(d, 7) : 0);
            for (int s = 0; s < 4; s++)
                v->seg_lf[s] = (int8_t)(bd_bit(d, 128) ? bd_signed(d, 6) : 0);
        }
        if (v->seg_map)
            for (int s = 0; s < 3; s++)
                v->seg_p[s] = (uint8_t)(bd_bit(d, 128) ? bd_lit(d, 8) : 255);
    }
    bool simple = bd_bit(d, 128);
    v->level = (int)bd_lit(d, 6);
    v->sharp = (int)bd_lit(d, 3);
    v->lf_delta = bd_bit(d, 128);
    if (v->lf_delta && bd_bit(d, 128)) { /* update the deltas: only the first of each counts for a key frame */
        for (int i = 0; i < 4; i++)
            if (bd_bit(d, 128)) {
                int x = bd_signed(d, 6);
                if (!i)
                    v->ref_lf0 = (int8_t)x;
            }
        for (int i = 0; i < 4; i++)
            if (bd_bit(d, 128)) {
                int x = bd_signed(d, 6);
                if (!i)
                    v->mode_lf0 = (int8_t)x;
            }
    }
    v->ftype = !v->level ? 0 : simple ? 1 : 2;
    v->extra = v->ftype == 2 ? 8 : v->ftype == 1 ? 2 : 0;
    v->parts = (uint8_t)(1u << bd_lit(d, 2));
}

static void quant_probs(vp8_t *v)
{
    bd_t *d = &v->b0;
    int base = (int)bd_lit(d, 7);
    int dy1 = bd_bit(d, 128) ? bd_signed(d, 4) : 0, dy2dc = bd_bit(d, 128) ? bd_signed(d, 4) : 0,
        dy2ac = bd_bit(d, 128) ? bd_signed(d, 4) : 0, duvdc = bd_bit(d, 128) ? bd_signed(d, 4) : 0,
        duvac = bd_bit(d, 128) ? bd_signed(d, 4) : 0;
    for (int s = 0; s < 4; s++) {
        int q = v->seg_on ? v->seg_q[s] + (v->seg_abs ? 0 : base) : base;
        wq_t *m = &v->q[s];
        m->y1[0] = W_DC[clampq(q + dy1, 127)];
        m->y1[1] = (int16_t)W_AC[clampq(q, 127)];
        m->y2[0] = (int16_t)(W_DC[clampq(q + dy2dc, 127)] * 2);
        int ac = (W_AC[clampq(q + dy2ac, 127)] * 101581) >> 16; /* x 155 / 100 */
        m->y2[1] = (int16_t)(ac < 8 ? 8 : ac);
        m->uv[0] = W_DC[clampq(q + duvdc, 117)];
        m->uv[1] = (int16_t)W_AC[clampq(q + duvac, 127)];
    }
    bd_bit(d, 128); /* refresh entropy probs (one frame: no matter) */
    for (int i = 0; i < 4 * 8 * 3 * 11; i++)
        v->coef[i] = bd_bit(d, W_COEF_UPD[i]) ? (uint8_t)bd_lit(d, 8) : W_COEF0[i];
    v->skip_p = bd_bit(d, 128) ? (int)bd_lit(d, 8) : -1;
    for (int s = 0; s < 4; s++) { /* filter strengths (libwebp PrecomputeFilterStrengths) */
        int base_level = v->seg_on ? v->seg_lf[s] + (v->seg_abs ? 0 : v->level) : v->level;
        for (int i4 = 0; i4 < 2; i4++) {
            wf_t *fs = &v->fs[s][i4];
            int level = base_level + (v->lf_delta ? v->ref_lf0 + (i4 ? v->mode_lf0 : 0) : 0);
            level = level < 0 ? 0 : level > 63 ? 63 : level;
            memset(fs, 0, sizeof *fs);
            if (level > 0) {
                int il = level;
                if (v->sharp > 0) {
                    il >>= v->sharp > 4 ? 2 : 1;
                    if (il > 9 - v->sharp)
                        il = 9 - v->sharp;
                }
                if (il < 1)
                    il = 1;
                fs->ilevel = (uint8_t)il;
                fs->limit = (uint8_t)(2 * level + il);
                fs->hev = (uint8_t)(level >= 40 ? 2 : level >= 15 ? 1 : 0);
            }
            fs->inner = (uint8_t)i4;
        }
    }
}

/* ------------------------------------------------------------------ macroblock header and residuals (RFC 6386 13) */

static const int8_t YMODE4_TREE[18] = {-B_DC, 1, -B_TM, 2, -B_VE, 3, 4, 6, -B_HE, 5, -B_RD, -B_VR, -B_LD, 7, -B_VL, 8,
                                       -B_HD, -B_HU};

static void mb_header(vp8_t *v, u32 x)
{
    bd_t *d = &v->b0;
    uint8_t *top = v->intra_t + 4 * x, *left = v->intra_l;
    v->seg = 0;
    if (v->seg_map)
        v->seg = (uint8_t)(!bd_bit(d, v->seg_p[0]) ? bd_bit(d, v->seg_p[1]) : bd_bit(d, v->seg_p[2]) + 2);
    v->skip = v->skip_p >= 0 ? bd_bit(d, v->skip_p) : false;
    v->i4 = !bd_bit(d, 145);
    if (!v->i4) {
        int m = bd_bit(d, 156) ? (bd_bit(d, 128) ? B_TM : B_HE) : (bd_bit(d, 163) ? B_VE : B_DC);
        v->modes[0] = (uint8_t)m;
        memset(top, m, 4);
        memset(left, m, 4);
    } else {
        for (int y = 0; y < 4; y++) {
            int m = left[y];
            for (int k = 0; k < 4; k++) {
                const uint8_t *p = W_BMODE + (top[k] * 10 + m) * 9;
                int i = YMODE4_TREE[bd_bit(d, p[0])];
                while (i > 0)
                    i = YMODE4_TREE[2 * i + bd_bit(d, p[i])];
                m = -i;
                top[k] = (uint8_t)m;
                v->modes[y * 4 + k] = (uint8_t)m;
            }
            left[y] = (uint8_t)m;
        }
    }
    v->uvmode = (uint8_t)(!bd_bit(d, 142) ? B_DC : !bd_bit(d, 114) ? B_VE : bd_bit(d, 183) ? B_TM : B_HE);
}

static int large(bd_t *d, const uint8_t *p)
{
    int v;
    if (!bd_bit(d, p[3])) {
        v = !bd_bit(d, p[4]) ? 2 : 3 + bd_bit(d, p[5]);
    } else if (!bd_bit(d, p[6])) {
        if (!bd_bit(d, p[7])) {
            v = 5 + bd_bit(d, 159);
        } else {
            v = 7 + 2 * bd_bit(d, 165);
            v += bd_bit(d, 145);
        }
    } else {
        int b1 = bd_bit(d, p[8]), b0 = bd_bit(d, p[9 + b1]), cat = 2 * b1 + b0;
        v = 0;
        for (const uint8_t *t = CATS[cat]; *t; t++)
            v += v + bd_bit(d, *t);
        v += 3 + (8 << cat);
    }
    return v;
}

/* One block's coefficients from position n; the index after the last one coded. */
static int coeffs(vp8_t *v, bd_t *d, int t, int ctx, const int16_t dq[2], int n, int16_t *out)
{
    const uint8_t *p = prob(v, t, BANDS[n], ctx);
    for (; n < 16; n++) {
        if (!bd_bit(d, p[0]))
            return n;
        while (!bd_bit(d, p[1])) {
            p = prob(v, t, BANDS[++n], 0);
            if (n == 16)
                return 16;
        }
        int val, band = BANDS[n + 1];
        if (!bd_bit(d, p[2])) {
            val = 1;
            p = prob(v, t, band, 1);
        } else {
            val = large(d, p);
            p = prob(v, t, band, 2);
        }
        out[ZIGZAG[n]] = (int16_t)((bd_bit(d, 128) ? -val : val) * dq[n > 0]);
    }
    return 16;
}

static void wht(const int16_t *in, int16_t *out)
{
    int tmp[16];
    for (int i = 0; i < 4; i++) {
        int a0 = in[i] + in[12 + i], a1 = in[4 + i] + in[8 + i], a2 = in[4 + i] - in[8 + i], a3 = in[i] - in[12 + i];
        tmp[i] = a0 + a1;
        tmp[8 + i] = a0 - a1;
        tmp[4 + i] = a3 + a2;
        tmp[12 + i] = a3 - a2;
    }
    for (int i = 0; i < 4; i++) {
        int dc = tmp[i * 4] + 3, a0 = dc + tmp[3 + i * 4], a1 = tmp[1 + i * 4] + tmp[2 + i * 4],
            a2 = tmp[1 + i * 4] - tmp[2 + i * 4], a3 = dc - tmp[3 + i * 4];
        out[0] = (int16_t)((a0 + a1) >> 3);
        out[16] = (int16_t)((a3 + a2) >> 3);
        out[32] = (int16_t)((a0 - a1) >> 3);
        out[48] = (int16_t)((a3 - a2) >> 3);
        out += 64;
    }
}

static uint32_t nzbits(uint32_t acc, int nz, int dc_nz) { return acc << 2 | (u32)(nz > 3 ? 3 : nz > 1 ? 2 : dc_nz); }

/* libwebp ParseResiduals: the contexts from the blocks above (nz_t) and left (nz_l). */
static void residuals(vp8_t *v, bd_t *d, u32 x)
{
    wnz_t *mb = v->nz_t + x, *lf = &v->nz_l;
    const wq_t *q = &v->q[v->seg];
    int16_t *dst = v->coeffs;
    memset(dst, 0, sizeof v->coeffs);
    int first, t;
    if (!v->i4) {
        int16_t dc[16] = {0};
        int nz = coeffs(v, d, 1, mb->nz_dc + lf->nz_dc, q->y2, 0, dc);
        mb->nz_dc = lf->nz_dc = nz > 0;
        if (nz > 1) {
            wht(dc, dst);
        } else {
            int dc0 = (dc[0] + 3) >> 3;
            for (int i = 0; i < 256; i += 16)
                dst[i] = (int16_t)dc0;
        }
        first = 1;
        t = 0;
    } else {
        first = 0;
        t = 3;
    }
    u32 tnz = mb->nz & 0x0f, lnz = lf->nz & 0x0f, nzy = 0, nzuv = 0;
    for (int y = 0; y < 4; y++) {
        u32 l = lnz & 1, acc = 0;
        for (int k = 0; k < 4; k++) {
            int ctx = (int)(l + (tnz & 1));
            int nz = coeffs(v, d, t, ctx, q->y1, first, dst);
            l = nz > first;
            tnz = (tnz >> 1) | (l << 7);
            acc = nzbits(acc, nz, dst[0] != 0);
            dst += 16;
        }
        tnz >>= 4;
        lnz = (lnz >> 1) | (l << 7);
        nzy = (nzy << 8) | acc;
    }
    u32 out_t = tnz, out_l = lnz >> 4;
    for (int ch = 0; ch < 4; ch += 2) {
        u32 acc = 0;
        tnz = (u32)mb->nz >> (4 + ch);
        lnz = (u32)lf->nz >> (4 + ch);
        for (int y = 0; y < 2; y++) {
            u32 l = lnz & 1;
            for (int k = 0; k < 2; k++) {
                int ctx = (int)(l + (tnz & 1));
                int nz = coeffs(v, d, 2, ctx, q->uv, 0, dst);
                l = nz > 0;
                tnz = (tnz >> 1) | (l << 3);
                acc = nzbits(acc, nz, dst[0] != 0);
                dst += 16;
            }
            tnz >>= 2;
            lnz = (lnz >> 1) | (l << 5);
        }
        nzuv |= acc << (4 * ch);
        out_t |= (tnz << 4) << ch;
        out_l |= (lnz & 0xf0) << ch;
    }
    mb->nz = (uint8_t)out_t;
    lf->nz = (uint8_t)out_l;
    v->nzy = nzy;
    v->nzuv = nzuv;
}

/* ------------------------------------------------------------------ reconstruction (libwebp dsp/dec.c) */

static inline uint8_t clip8(int v) { return !(v & ~0xff) ? (uint8_t)v : v < 0 ? 0 : 255; } /* one test when in range */

#define M1(a) ((((a) * 20091) >> 16) + (a))
#define M2(a) (((a) * 35468) >> 16)

static void idct_add(const int16_t *in, uint8_t *dst)
{
    int c[16], *t = c;
    for (int i = 0; i < 4; i++, in++, t += 4) {
        int a = in[0] + in[8], b = in[0] - in[8], cc = M2(in[4]) - M1(in[12]), dd = M1(in[4]) + M2(in[12]);
        t[0] = a + dd;
        t[1] = b + cc;
        t[2] = b - cc;
        t[3] = a - dd;
    }
    t = c;
    for (int i = 0; i < 4; i++, t++, dst += BPS) {
        int dc = t[0] + 4, a = dc + t[8], b = dc - t[8], cc = M2(t[4]) - M1(t[12]), dd = M1(t[4]) + M2(t[12]);
        dst[0] = clip8(dst[0] + ((a + dd) >> 3));
        dst[1] = clip8(dst[1] + ((b + cc) >> 3));
        dst[2] = clip8(dst[2] + ((b - cc) >> 3));
        dst[3] = clip8(dst[3] + ((a - dd) >> 3));
    }
}

static bool any16(const int16_t *c)
{
    for (int i = 0; i < 16; i++)
        if (c[i])
            return true;
    return false;
}

#define AVG3(a, b, c) ((uint8_t)(((a) + 2 * (b) + (c) + 2) >> 2))
#define AVG2(a, b) ((uint8_t)(((a) + (b) + 1) >> 1))
#define D(x, y) d[(x) + (y) * BPS]

static void fill(uint8_t *d, int n, int val)
{
    for (int j = 0; j < n; j++)
        memset(d + j * BPS, val, (size_t)n);
}

/* 16x16 (n 16) and chroma 8x8 (n 8): DC (and its edge forms), TM, VE, HE. */
static void pred_big(uint8_t *d, int n, int mode)
{
    int sh = n == 16 ? 4 : 3;
    switch (mode) {
    case B_DC: {
        int dc = n;
        for (int j = 0; j < n; j++)
            dc += d[-1 + j * BPS] + d[j - BPS];
        fill(d, n, dc >> (sh + 1));
        break;
    }
    case DC_NOTOP: {
        int dc = n >> 1;
        for (int j = 0; j < n; j++)
            dc += d[-1 + j * BPS];
        fill(d, n, dc >> sh);
        break;
    }
    case DC_NOLEFT: {
        int dc = n >> 1;
        for (int j = 0; j < n; j++)
            dc += d[j - BPS];
        fill(d, n, dc >> sh);
        break;
    }
    case DC_NOTOPLEFT:
        fill(d, n, 0x80);
        break;
    case B_VE:
        for (int j = 0; j < n; j++)
            memcpy(d + j * BPS, d - BPS, (size_t)n);
        break;
    case B_HE:
        for (int j = 0; j < n; j++)
            memset(d + j * BPS, d[j * BPS - 1], (size_t)n);
        break;
    default: /* TM */
        for (int j = 0; j < n; j++)
            for (int i = 0; i < n; i++)
                d[i + j * BPS] = clip8(d[i - BPS] + d[j * BPS - 1] - d[-1 - BPS]);
        break;
    }
}

static void pred4(uint8_t *d, int mode)
{
    const uint8_t *t = d - BPS;
    int I = d[-1], J = d[-1 + BPS], K = d[-1 + 2 * BPS], L = d[-1 + 3 * BPS], X = t[-1], A = t[0], B = t[1], C = t[2],
        Dd = t[3], E = t[4], F = t[5], G = t[6], H = t[7];
    switch (mode) {
    case B_DC: {
        int dc = 4;
        for (int i = 0; i < 4; i++)
            dc += t[i] + d[-1 + i * BPS];
        fill(d, 4, dc >> 3);
        break;
    }
    case B_TM:
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 4; i++)
                D(i, j) = clip8(t[i] + d[j * BPS - 1] - X);
        break;
    case B_VE: {
        uint8_t v4[4] = {AVG3(X, A, B), AVG3(A, B, C), AVG3(B, C, Dd), AVG3(C, Dd, E)};
        for (int j = 0; j < 4; j++)
            memcpy(d + j * BPS, v4, 4);
        break;
    }
    case B_HE: {
        uint8_t r[4] = {AVG3(X, I, J), AVG3(I, J, K), AVG3(J, K, L), AVG3(K, L, L)};
        for (int j = 0; j < 4; j++)
            memset(d + j * BPS, r[j], 4);
        break;
    }
    case B_RD:
        D(0, 3) = AVG3(J, K, L);
        D(1, 3) = D(0, 2) = AVG3(I, J, K);
        D(2, 3) = D(1, 2) = D(0, 1) = AVG3(X, I, J);
        D(3, 3) = D(2, 2) = D(1, 1) = D(0, 0) = AVG3(A, X, I);
        D(3, 2) = D(2, 1) = D(1, 0) = AVG3(B, A, X);
        D(3, 1) = D(2, 0) = AVG3(C, B, A);
        D(3, 0) = AVG3(Dd, C, B);
        break;
    case B_LD:
        D(0, 0) = AVG3(A, B, C);
        D(1, 0) = D(0, 1) = AVG3(B, C, Dd);
        D(2, 0) = D(1, 1) = D(0, 2) = AVG3(C, Dd, E);
        D(3, 0) = D(2, 1) = D(1, 2) = D(0, 3) = AVG3(Dd, E, F);
        D(3, 1) = D(2, 2) = D(1, 3) = AVG3(E, F, G);
        D(3, 2) = D(2, 3) = AVG3(F, G, H);
        D(3, 3) = AVG3(G, H, H);
        break;
    case B_VR:
        D(0, 0) = D(1, 2) = AVG2(X, A);
        D(1, 0) = D(2, 2) = AVG2(A, B);
        D(2, 0) = D(3, 2) = AVG2(B, C);
        D(3, 0) = AVG2(C, Dd);
        D(0, 3) = AVG3(K, J, I);
        D(0, 2) = AVG3(J, I, X);
        D(0, 1) = D(1, 3) = AVG3(I, X, A);
        D(1, 1) = D(2, 3) = AVG3(X, A, B);
        D(2, 1) = D(3, 3) = AVG3(A, B, C);
        D(3, 1) = AVG3(B, C, Dd);
        break;
    case B_VL:
        D(0, 0) = AVG2(A, B);
        D(1, 0) = D(0, 2) = AVG2(B, C);
        D(2, 0) = D(1, 2) = AVG2(C, Dd);
        D(3, 0) = D(2, 2) = AVG2(Dd, E);
        D(0, 1) = AVG3(A, B, C);
        D(1, 1) = D(0, 3) = AVG3(B, C, Dd);
        D(2, 1) = D(1, 3) = AVG3(C, Dd, E);
        D(3, 1) = D(2, 3) = AVG3(Dd, E, F);
        D(3, 2) = AVG3(E, F, G);
        D(3, 3) = AVG3(F, G, H);
        break;
    case B_HU:
        D(0, 0) = AVG2(I, J);
        D(2, 0) = D(0, 1) = AVG2(J, K);
        D(2, 1) = D(0, 2) = AVG2(K, L);
        D(1, 0) = AVG3(I, J, K);
        D(3, 0) = D(1, 1) = AVG3(J, K, L);
        D(3, 1) = D(1, 2) = AVG3(K, L, L);
        D(3, 2) = D(2, 2) = D(0, 3) = D(1, 3) = D(2, 3) = D(3, 3) = (uint8_t)L;
        break;
    default: /* HD */
        D(0, 0) = D(2, 1) = AVG2(I, X);
        D(0, 1) = D(2, 2) = AVG2(J, I);
        D(0, 2) = D(2, 3) = AVG2(K, J);
        D(0, 3) = AVG2(L, K);
        D(3, 0) = AVG3(A, B, C);
        D(2, 0) = AVG3(X, A, B);
        D(1, 0) = D(3, 1) = AVG3(I, X, A);
        D(1, 1) = D(3, 2) = AVG3(J, I, X);
        D(1, 2) = D(3, 3) = AVG3(K, J, I);
        D(1, 3) = AVG3(L, K, J);
        break;
    }
}

static int check_mode(u32 x, u32 y, int mode)
{
    if (mode != B_DC)
        return mode;
    return x == 0 ? (y == 0 ? DC_NOTOPLEFT : DC_NOLEFT) : (y == 0 ? DC_NOTOP : B_DC);
}

/* Macroblock (x, y) into the work block, its bottom row kept for the row below, then into the caches. */
static void reconstruct(vp8_t *v, u32 x, u32 y)
{
    uint8_t *yd = v->work + YOFF, *ud = v->work + BPS * 17 + YOFF, *vd = ud + BPS * 9;
    uint8_t *top = v->top + (size_t)x * 32;
    if (x == 0) {
        for (int j = 0; j < 16; j++)
            yd[j * BPS - 1] = 129;
        for (int j = 0; j < 8; j++)
            ud[j * BPS - 1] = vd[j * BPS - 1] = 129;
        if (y > 0) {
            yd[-1 - BPS] = ud[-1 - BPS] = vd[-1 - BPS] = 129;
        } else {
            memset(yd - BPS - 1, 127, 16 + 4 + 1);
            memset(ud - BPS - 1, 127, 8 + 1);
            memset(vd - BPS - 1, 127, 8 + 1);
        }
    } else { /* the left column (and the top-left) from the macroblock before */
        for (int j = -1; j < 16; j++)
            yd[j * BPS - 1] = yd[j * BPS + 15];
        for (int j = -1; j < 8; j++) {
            ud[j * BPS - 1] = ud[j * BPS + 7];
            vd[j * BPS - 1] = vd[j * BPS + 7];
        }
    }
    if (y > 0) {
        memcpy(yd - BPS, top, 16);
        memcpy(ud - BPS, top + 16, 8);
        memcpy(vd - BPS, top + 24, 8);
    }
    const int16_t *c = v->coeffs;
    if (v->i4) {
        uint8_t *tr = yd - BPS + 16;
        if (y > 0) {
            if (x + 1 >= v->mbw)
                memset(tr, top[15], 4);
            else
                memcpy(tr, top + 32, 4);
        }
        for (int r = 1; r < 4; r++) /* the right column's top-right: the macroblock's */
            memcpy(tr + 4 * r * BPS, tr, 4);
        for (int n = 0; n < 16; n++) {
            uint8_t *d = yd + (n & 3) * 4 + (n >> 2) * 4 * BPS;
            pred4(d, v->modes[n]);
            if (any16(c + n * 16))
                idct_add(c + n * 16, d);
        }
    } else {
        pred_big(yd, 16, check_mode(x, y, v->modes[0]));
        for (int n = 0; n < 16; n++)
            if (any16(c + n * 16))
                idct_add(c + n * 16, yd + (n & 3) * 4 + (n >> 2) * 4 * BPS);
    }
    int um = check_mode(x, y, v->uvmode);
    pred_big(ud, 8, um);
    pred_big(vd, 8, um);
    for (int n = 0; n < 4; n++) {
        int off = (n & 1) * 4 + (n >> 1) * 4 * BPS;
        if (any16(c + 256 + n * 16))
            idct_add(c + 256 + n * 16, ud + off);
        if (any16(c + 320 + n * 16))
            idct_add(c + 320 + n * 16, vd + off);
    }
    if (y + 1 < v->mbh) {
        memcpy(top, yd + 15 * BPS, 16);
        memcpy(top + 16, ud + 7 * BPS, 8);
        memcpy(top + 24, vd + 7 * BPS, 8);
    }
    for (int j = 0; j < 16; j++)
        memcpy(v->cy + j * v->cys + x * 16, yd + j * BPS, 16);
    for (int j = 0; j < 8; j++) {
        memcpy(v->cu + j * v->cuvs + x * 8, ud + j * BPS, 8);
        memcpy(v->cv + j * v->cuvs + x * 8, vd + j * BPS, 8);
    }
}

/* ------------------------------------------------------------------ loop filter (RFC 6386 15, libwebp dsp) */

static inline int sclip1(int v) { return v < -128 ? -128 : v > 127 ? 127 : v; }
static inline int sclip2(int v) { return v < -16 ? -16 : v > 15 ? 15 : v; }
static inline int iabs(int v) { return v < 0 ? -v : v; }
static inline int imax(int a, int b) { return a > b ? a : b; }

/* The common filter (hev, simple): p0 and q0. */
static inline void f2(uint8_t *p, int s, int p1, int p0, int q0, int q1)
{
    int a = 3 * (q0 - p0) + sclip1(p1 - q1), a1 = sclip2((a + 4) >> 3), a2 = sclip2((a + 3) >> 3);
    p[-s] = clip8(p0 + a2);
    p[0] = clip8(q0 - a1);
}

/* An edge of `size` pixels: hs across it, vs along it; mb: the macroblock edge (6 taps), else inner (4). */
static void edge(uint8_t *p, int hs, int vs, int size, int thresh, int it, int hv, bool mb)
{
    int t2 = 2 * thresh + 1;
    for (; size-- > 0; p += vs) {
        int p1 = p[-2 * hs], p0 = p[-hs], q0 = p[0], q1 = p[hs];
        if (4 * iabs(p0 - q0) + iabs(p1 - q1) > t2)
            continue;
        int p3 = p[-4 * hs], p2 = p[-3 * hs], q2 = p[2 * hs], q3 = p[3 * hs];
        int d1 = iabs(p1 - p0), d2 = iabs(q1 - q0);
        if (imax(imax(imax(iabs(p3 - p2), iabs(p2 - p1)), imax(iabs(q3 - q2), iabs(q2 - q1))), imax(d1, d2)) > it)
            continue;
        if (d1 > hv || d2 > hv) {
            f2(p, hs, p1, p0, q0, q1);
        } else if (mb) {
            int a = sclip1(3 * (q0 - p0) + sclip1(p1 - q1));
            int a1 = (27 * a + 63) >> 7, a2 = (18 * a + 63) >> 7, a3 = (9 * a + 63) >> 7;
            p[-3 * hs] = clip8(p2 + a3);
            p[-2 * hs] = clip8(p1 + a2);
            p[-hs] = clip8(p0 + a1);
            p[0] = clip8(q0 - a1);
            p[hs] = clip8(q1 - a2);
            p[2 * hs] = clip8(q2 - a3);
        } else {
            int a = 3 * (q0 - p0), a1 = sclip2((a + 4) >> 3), a2 = sclip2((a + 3) >> 3), a3 = (a1 + 1) >> 1;
            p[-2 * hs] = clip8(p1 + a3);
            p[-hs] = clip8(p0 + a2);
            p[0] = clip8(q0 - a1);
            p[hs] = clip8(q1 - a3);
        }
    }
}

static void simple_edge(uint8_t *p, int hs, int vs, int thresh)
{
    int t2 = 2 * thresh + 1;
    for (int i = 0; i < 16; i++, p += vs) {
        int p1 = p[-2 * hs], p0 = p[-hs], q0 = p[0], q1 = p[hs];
        if (4 * iabs(p0 - q0) + iabs(p1 - q1) <= t2)
            f2(p, hs, p1, p0, q0, q1);
    }
}

static void filter_mb(vp8_t *v, u32 x, u32 y)
{
    const wf_t *fi = v->fi + x;
    int limit = fi->limit, il = fi->ilevel, hv = fi->hev;
    if (!limit)
        return;
    int ys = (int)v->cys, uvs = (int)v->cuvs;
    uint8_t *yd = v->cy + x * 16, *ud = v->cu + x * 8, *vd = v->cv + x * 8;
    if (v->ftype == 1) {
        if (x > 0)
            simple_edge(yd, 1, ys, limit + 4);
        if (fi->inner)
            for (int k = 4; k < 16; k += 4)
                simple_edge(yd + k, 1, ys, limit);
        if (y > 0)
            simple_edge(yd, ys, 1, limit + 4);
        if (fi->inner)
            for (int k = 4; k < 16; k += 4)
                simple_edge(yd + k * ys, ys, 1, limit);
        return;
    }
    if (x > 0) {
        edge(yd, 1, ys, 16, limit + 4, il, hv, true);
        edge(ud, 1, uvs, 8, limit + 4, il, hv, true);
        edge(vd, 1, uvs, 8, limit + 4, il, hv, true);
    }
    if (fi->inner) {
        for (int k = 4; k < 16; k += 4)
            edge(yd + k, 1, ys, 16, limit, il, hv, false);
        edge(ud + 4, 1, uvs, 8, limit, il, hv, false);
        edge(vd + 4, 1, uvs, 8, limit, il, hv, false);
    }
    if (y > 0) {
        edge(yd, ys, 1, 16, limit + 4, il, hv, true);
        edge(ud, uvs, 1, 8, limit + 4, il, hv, true);
        edge(vd, uvs, 1, 8, limit + 4, il, hv, true);
    }
    if (fi->inner) {
        for (int k = 4; k < 16; k += 4)
            edge(yd + k * ys, ys, 1, 16, limit, il, hv, false);
        edge(ud + 4 * uvs, uvs, 1, 8, limit, il, hv, false);
        edge(vd + 4 * uvs, uvs, 1, 8, limit, il, hv, false);
    }
}

/* ------------------------------------------------------------------ output */

/* libwebp yuv.h: 14-bit fixed point, BT.601 limited range. */
static int mhi(int v, int c) { return (v * c) >> 8; }
static uint8_t clip6(int v) { return (v & ~16383) == 0 ? (uint8_t)(v >> 6) : v < 0 ? 0 : 255; }
static uint8_t yuv_gray(int y) { return clip6(mhi(y, 19077) - 1160); }

/* Rows y0 .. y1 - 1 of the image; row y0 at the caches' row r0 (Y; chroma at r0 / 2). */
static inline uint8_t over_white(int c, int a) { return a == 255 ? (uint8_t)c : (uint8_t)((c * a + 255 * (255 - a) + 127) / 255); }

/* A row of w pixels (gray or RGB in v->row) to the callbacks. */
static int put_row(const imgdec_io_t *io, uint8_t *row, u32 y, u32 w, bool rgb)
{
    if (rgb)
        return io->row8(io->ctx, (uint16_t)y, row, (uint16_t)w, 3) ? IMGDEC_E_ABORT : IMGDEC_OK;
    if (io->row8)
        return io->row8(io->ctx, (uint16_t)y, row, (uint16_t)w, 1) ? IMGDEC_E_ABORT : IMGDEC_OK;
    for (u32 x = 0; x < w; x++)
        row[x] >>= 4;
    return io->row(io->ctx, (uint16_t)y, row, (uint16_t)w) ? IMGDEC_E_ABORT : IMGDEC_OK;
}

static int emit(vp8_t *v, u32 y0, u32 y1, const uint8_t *ybase, const uint8_t *ubase, const uint8_t *vbase)
{
    const imgdec_io_t *io = v->io;
    for (u32 y = y0; y < y1; y++) {
        const uint8_t *yr = ybase + (size_t)(y - y0) * v->cys;
        size_t uvr = (size_t)((y >> 1) - (y0 >> 1)) * v->cuvs;
        int stop;
        if (v->rgb) {
            for (u32 x = 0; x < v->w; x++) {
                int Y = yr[x], U = ubase[uvr + (x >> 1)], V = vbase[uvr + (x >> 1)];
                uint8_t *o = v->row + 3 * x;
                o[0] = clip6(mhi(Y, 19077) + mhi(V, 26149) - 14234);
                o[1] = clip6(mhi(Y, 19077) - mhi(U, 6419) - mhi(V, 13320) + 8708);
                o[2] = clip6(mhi(Y, 19077) + mhi(U, 33050) - 17685);
            }
            if (v->alpha)
                for (u32 x = 0; x < 3 * v->w; x++)
                    v->row[x] = over_white(v->row[x], v->alpha[(size_t)y * v->w + x / 3]);
        } else {
            for (u32 x = 0; x < v->w; x++)
                v->row[x] = v->alpha ? over_white(v->gray[yr[x]], v->alpha[(size_t)y * v->w + x]) : v->gray[yr[x]];
        }
        if ((stop = put_row(io, v->row, y, v->w, v->rgb)))
            return stop;
    }
    return IMGDEC_OK;
}

/* ------------------------------------------------------------------ the frame */

static void *al(const imgdec_io_t *io, size_t n) { return io->alloc(io->ctx, n); }
static void fr(const imgdec_io_t *io, void *p)
{
    if (p)
        io->free(io->ctx, p);
}

/* Bytes the lossy decoder allocates for a frame w wide (the caches for the normal filter, RGB rows). */
static size_t vp8_need(u32 w)
{
    u32 mbw = (w + 15) / 16;
    return sizeof(vp8_t) + 9 * sizeof(wsrc_t) + mbw * (4 + sizeof(wnz_t) + 32 + sizeof(wf_t)) +
           (16 + 8) * mbw * 16 + 2 * (8 + 4) * mbw * 8 + 3 * w + 64;
}

/* The VP8 chunk's payload at `off` (size n) of the file (f at off): its frame into rows. info set, io.info called. */
static int vp8_frame(wfile_t *f, imgdec_info_t *info, u32 off, u32 n, uint8_t **held, const uint8_t *alpha)
{
    const imgdec_io_t *io = f->io;
    uint8_t h[10];
    if (n < 10 || f_read(f, h, 10) != 10)
        return f->err ? f->err : IMGDEC_E_CORRUPT;
    u32 tag = h[0] | (u32)h[1] << 8 | (u32)h[2] << 16, p0 = tag >> 5;
    if ((tag & 1) || ((tag >> 1) & 7) > 3 || !((tag >> 4) & 1) || h[3] != 0x9d || h[4] != 0x01 || h[5] != 0x2a)
        return IMGDEC_E_CORRUPT; /* not a shown key frame */
    u32 w = (h[6] | (u32)h[7] << 8) & 0x3fff, hh = (h[8] | (u32)h[9] << 8) & 0x3fff;
    if (!w || !hh || p0 > n - 10)
        return IMGDEC_E_CORRUPT;
    if (w > IMGDEC_MAX_W || hh > IMGDEC_MAX_H)
        return IMGDEC_E_SIZE;
    if (info->w && (info->w != w || info->h != hh))
        return IMGDEC_E_CORRUPT; /* not the canvas */
    info->w = info->rw = (uint16_t)w;
    info->h = info->rh = (uint16_t)hh;
    if (io->info && io->info(io->ctx, info))
        return IMGDEC_E_ABORT;
    vp8_t *v = al(io, sizeof *v);
    if (!v)
        return IMGDEC_E_NOMEM;
    memset(v, 0, sizeof *v);
    v->f = f;
    v->io = io;
    v->w = w;
    v->h = hh;
    v->mbw = (w + 15) / 16;
    v->mbh = (hh + 15) / 16;
    v->rgb = io->row8 && io->want_rgb;
    v->alpha = alpha;
    for (int i = 0; i < 256; i++)
        v->gray[i] = yuv_gray(i);
    int rc = IMGDEC_OK;
    v->src = al(io, 9 * sizeof(wsrc_t));
    if (!v->src) {
        rc = IMGDEC_E_NOMEM;
        goto out;
    }
    memset(v->src, 0, 9 * sizeof(wsrc_t));
    u32 at = off + 10;
    wsrc_t *s0 = &v->src[0];
    s0->f = f;
    s0->off = at;
    s0->end = at + p0;
    if (!io->seek) { /* a stream: partition 0 in RAM */
        *held = al(io, p0 ? p0 : 1);
        if (!*held) {
            rc = IMGDEC_E_NOMEM;
            goto out;
        }
        if (f_read(f, *held, p0) != p0) {
            rc = f->err ? f->err : IMGDEC_E_CORRUPT;
            goto out;
        }
        s0->mem = *held - at;
    }
    bd_init(&v->b0, s0);
    headers(v);
    /* the token partitions: their sizes (3 bytes each but the last) after partition 0 */
    u32 pat = at + p0, np = v->parts, left = n - 10 - p0;
    uint8_t sz[21];
    if (left < 3 * (np - 1)) {
        rc = IMGDEC_E_CORRUPT;
        goto out;
    }
    if (!f_seek(f, pat) || f_read(f, sz, 3 * (np - 1)) != 3 * (np - 1)) {
        rc = f->err ? f->err : IMGDEC_E_CORRUPT;
        goto out;
    }
    left -= 3 * (np - 1);
    u32 p = pat + 3 * (np - 1);
    uint8_t *rest = NULL;
    if (!io->seek && np > 1) { /* a stream with several: all of them in RAM */
        rest = al(io, left ? left : 1);
        if (!rest || f_read(f, rest, left) != left) {
            fr(io, rest);
            rc = rest ? (f->err ? f->err : IMGDEC_E_CORRUPT) : IMGDEC_E_NOMEM;
            goto out;
        }
    }
    for (u32 i = 0; i < np; i++) {
        u32 ps = i + 1 < np ? (sz[3 * i] | (u32)sz[3 * i + 1] << 8 | (u32)sz[3 * i + 2] << 16) : left;
        if (ps > left)
            ps = left;
        wsrc_t *s = &v->src[1 + i];
        s->f = f;
        s->off = p;
        s->end = p + ps;
        if (rest)
            s->mem = rest - (pat + 3 * (np - 1));
        p += ps;
        left -= ps;
    }
    quant_probs(v);
    for (u32 i = 0; i < np; i++)
        bd_init(&v->bt[i], &v->src[1 + i]);
    /* rows */
    v->cys = v->mbw * 16;
    v->cuvs = v->mbw * 8;
    size_t ey = (size_t)v->extra * v->cys, euv = (size_t)(v->extra / 2) * v->cuvs;
    v->intra_t = al(io, v->mbw * 4);
    v->nz_t = al(io, v->mbw * sizeof(wnz_t));
    v->top = al(io, (size_t)v->mbw * 32);
    v->fi = al(io, v->mbw * sizeof(wf_t));
    uint8_t *cyb = al(io, ey + 16 * (size_t)v->cys), *cub = al(io, euv + 8 * (size_t)v->cuvs),
            *cvb = al(io, euv + 8 * (size_t)v->cuvs);
    v->row = al(io, (v->rgb ? 3u : 1u) * w);
    if (!v->intra_t || !v->nz_t || !v->top || !v->fi || !cyb || !cub || !cvb || !v->row) {
        rc = IMGDEC_E_NOMEM;
    } else {
        memset(v->intra_t, B_DC, v->mbw * 4);
        memset(v->nz_t, 0, v->mbw * sizeof(wnz_t));
        memset(v->top, 0, (size_t)v->mbw * 32);
        v->cy = cyb + ey;
        v->cu = cub + euv;
        v->cv = cvb + euv;
    }
    for (u32 y = 0; y < v->mbh && !rc; y++) {
        bd_t *td = &v->bt[y & (np - 1)];
        memset(v->intra_l, B_DC, 4);
        v->nz_l.nz = v->nz_l.nz_dc = 0;
        for (u32 x = 0; x < v->mbw; x++) {
            mb_header(v, x);
            bool skip = v->skip;
            if (!skip) {
                residuals(v, td, x);
                skip = !(v->nzy | v->nzuv);
            } else {
                memset(v->coeffs, 0, sizeof v->coeffs);
                v->nz_l.nz = v->nz_t[x].nz = 0;
                if (!v->i4)
                    v->nz_l.nz_dc = v->nz_t[x].nz_dc = 0;
            }
            if (v->ftype) {
                v->fi[x] = v->fs[v->seg][v->i4];
                v->fi[x].inner |= !skip;
            }
            reconstruct(v, x, y);
            if (v->ftype)
                filter_mb(v, x, y);
        }
        if (f->cut || f->err) {
            rc = f->err ? f->err : IMGDEC_E_CORRUPT;
            break;
        }
        /* the rows the filter is done with: from `extra` rows above this row (kept since the last) */
        bool last = y + 1 == v->mbh;
        u32 y0 = y * 16 - (y ? (u32)v->extra : 0), y1 = (y + 1) * 16 - (last ? 0 : (u32)v->extra);
        if (y1 > hh)
            y1 = hh;
        const uint8_t *yb = y ? v->cy - ey : v->cy, *ub = y ? v->cu - euv : v->cu, *vb = y ? v->cv - euv : v->cv;
        if (y0 < y1)
            rc = emit(v, y0, y1, yb, ub, vb);
        if (!last && v->extra) {
            memcpy(v->cy - ey, v->cy + 16 * (size_t)v->cys - ey, ey);
            memcpy(v->cu - euv, v->cu + 8 * (size_t)v->cuvs - euv, euv);
            memcpy(v->cv - euv, v->cv + 8 * (size_t)v->cuvs - euv, euv);
        }
    }
    fr(io, rest);
    fr(io, cyb);
    fr(io, cub);
    fr(io, cvb);
out:
    if (v->src)
        fr(io, v->src);
    fr(io, v->intra_t);
    fr(io, v->nz_t);
    fr(io, v->top);
    fr(io, v->fi);
    fr(io, v->row);
    fr(io, v);
    return rc;
}

/* ------------------------------------------------------------------ lossless (VP8L, WebP Lossless Bitstream) */

/* A bump allocator over blocks of the io allocator, freed at once. */
typedef struct wblk {
    struct wblk *next;
    size_t used, cap;
} wblk_t;

typedef struct {
    uint16_t fast[256];   /* the next 8 bits (first bit lowest): sym << 4 | len, 0 a longer code */
    uint16_t count[16];   /* codes of each length */
    uint16_t *syms;       /* the symbols in code order */
    int single;           /* >= 0: the only symbol (0 bits) */
} hcode_t;

typedef struct {
    hcode_t c[5];         /* green + lengths + cache, red, blue, alpha, distance */
} hgroup_t;

typedef struct {
    uint8_t type, bits;
    u32 xs;               /* the width it gives back */
    uint32_t *data;
    uint32_t *rows;       /* predictor: two rows (previous, this); others: one */
} wtr_t;

typedef struct {
    wfile_t *f;
    const imgdec_io_t *io;
    wsrc_t *s;
    uint64_t val;
    int nb;
    uint64_t used, avail; /* bits taken, bits in the chunk */
    wblk_t *blk;
    int err;
    wtr_t tr[4];
    int ntr;
    bool seen[4];
} vp8l_t;

static void *l_take(vp8l_t *l, size_t n)
{
    n = (n + 7) & ~(size_t)7;
    if (!l->blk || l->blk->cap - l->blk->used < n) {
        size_t cap = n > 16384 ? n : 16384;
        wblk_t *b = al(l->io, sizeof *b + cap);
        if (!b)
            return NULL;
        b->next = l->blk;
        b->used = 0;
        b->cap = cap;
        l->blk = b;
    }
    void *p = (uint8_t *)(l->blk + 1) + l->blk->used;
    l->blk->used += n;
    return p;
}

static void l_free_all(vp8l_t *l)
{
    while (l->blk) {
        wblk_t *n = l->blk->next;
        fr(l->io, l->blk);
        l->blk = n;
    }
}

static void l_fill(vp8l_t *l)
{
    while (l->nb <= 56) {
        int c = s_byte(l->s);
        l->val |= (uint64_t)(c < 0 ? 0 : c) << l->nb;
        l->nb += 8;
    }
}

static u32 l_bits(vp8l_t *l, int n)
{
    if (!n)
        return 0;
    if (l->nb < n)
        l_fill(l);
    u32 v = (u32)(l->val & ((1u << n) - 1));
    l->val >>= n;
    l->nb -= n;
    l->used += (u32)n;
    return v;
}

static bool l_eos(const vp8l_t *l) { return l->used > l->avail || l->f->cut; }

static u32 rev(u32 c, int n)
{
    u32 r = 0;
    while (n--) {
        r = r << 1 | (c & 1);
        c >>= 1;
    }
    return r;
}

/* A canonical code from its lengths (libwebp's checks: lengths <= 15, not all zero, a complete tree unless one
 * symbol). false: bad. */
static bool h_build(vp8l_t *l, hcode_t *h, const uint8_t *len, int n)
{
    memset(h, 0, sizeof *h);
    h->single = -1;
    int nz = 0, last = 0;
    for (int i = 0; i < n; i++)
        if (len[i]) {
            if (len[i] > 15)
                return false;
            h->count[len[i]]++;
            nz++;
            last = i;
        }
    if (!nz)
        return false;
    if (nz == 1) {
        h->single = last;
        return true;
    }
    int left = 1;
    for (int k = 1; k < 16; k++) {
        left = (left << 1) - h->count[k];
        if (left < 0)
            return false; /* over-subscribed */
    }
    if (left)
        return false; /* incomplete */
    h->syms = l_take(l, (size_t)nz * sizeof *h->syms);
    if (!h->syms)
        return false;
    uint16_t off[16];
    off[1] = 0;
    for (int k = 1; k < 15; k++)
        off[k + 1] = (uint16_t)(off[k] + h->count[k]);
    for (int i = 0; i < n; i++)
        if (len[i])
            h->syms[off[len[i]]++] = (uint16_t)i;
    u32 code = 0;
    int idx = 0;
    for (int k = 1; k < 16; k++) {
        for (int j = 0; j < h->count[k]; j++, code++, idx++)
            if (k <= 8) {
                u32 r = rev(code, k);
                for (u32 f = r; f < 256; f += 1u << k)
                    h->fast[f] = (uint16_t)(h->syms[idx] << 4 | k);
            }
        code <<= 1;
    }
    return true;
}

static int h_read(vp8l_t *l, const hcode_t *h)
{
    if (h->single >= 0)
        return h->single;
    if (l->nb < 16)
        l_fill(l);
    uint16_t e = h->fast[l->val & 255];
    if (e) {
        int k = e & 15;
        l->val >>= k;
        l->nb -= k;
        l->used += (u32)k;
        return e >> 4;
    }
    int code = 0, first = 0, idx = 0;
    for (int k = 1; k < 16; k++) { /* canonical, bit by bit */
        code |= (int)l_bits(l, 1);
        int c = h->count[k];
        if (code - first < c)
            return h->syms[idx + code - first];
        idx += c;
        first = (first + c) << 1;
        code <<= 1;
    }
    return -1;
}

static const uint8_t CL_ORDER[19] = {17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

/* One prefix code of n symbols (libwebp ReadHuffmanCode). */
static bool h_code(vp8l_t *l, hcode_t *h, int n, uint8_t *len)
{
    memset(len, 0, (size_t)n);
    if (l_bits(l, 1)) { /* simple: one or two symbols */
        int two = (int)l_bits(l, 1);
        int s0 = (int)l_bits(l, l_bits(l, 1) ? 8 : 1);
        if (s0 >= n)
            return false;
        len[s0] = 1;
        if (two) {
            int s1 = (int)l_bits(l, 8);
            if (s1 >= n)
                return false;
            len[s1] = 1;
        }
    } else {
        uint8_t cl[19] = {0};
        int k = (int)l_bits(l, 4) + 4;
        for (int i = 0; i < k; i++)
            cl[CL_ORDER[i]] = (uint8_t)l_bits(l, 3);
        hcode_t lc;
        wblk_t *mark = l->blk;
        size_t used = mark ? mark->used : 0;
        if (!h_build(l, &lc, cl, 19))
            return false;
        int max = n;
        if (l_bits(l, 1)) {
            int nbits = 2 + 2 * (int)l_bits(l, 3);
            max = 2 + (int)l_bits(l, nbits);
            if (max > n)
                return false;
        }
        int prev = 8;
        for (int sym = 0; sym < n;) {
            if (max-- == 0)
                break;
            int c = h_read(l, &lc);
            if (c < 0)
                return false;
            if (c < 16) {
                len[sym++] = (uint8_t)c;
                if (c)
                    prev = c;
            } else {
                static const uint8_t EB[3] = {2, 3, 7}, RO[3] = {3, 3, 11};
                int rep = (int)l_bits(l, EB[c - 16]) + RO[c - 16];
                if (sym + rep > n)
                    return false;
                memset(len + sym, c == 16 ? prev : 0, (size_t)rep);
                sym += rep;
            }
        }
        if (l->blk == mark && mark) /* the code-length code's symbols are no longer needed */
            mark->used = used;
    }
    return !l_eos(l) && h_build(l, h, len, n);
}

static u32 sub_size(u32 n, int bits) { return (n + (1u << bits) - 1) >> bits; }

static int l_copy_len(vp8l_t *l, int sym)
{
    if (sym < 4)
        return sym + 1;
    int eb = (sym - 2) >> 1, off = (2 + (sym & 1)) << eb;
    return off + (int)l_bits(l, eb) + 1;
}

static const uint8_t PLANE[120] = {
    0x18, 0x07, 0x17, 0x19, 0x28, 0x06, 0x27, 0x29, 0x16, 0x1a, 0x26, 0x2a, 0x38, 0x05, 0x37, 0x39, 0x15, 0x1b, 0x36, 0x3a,
    0x25, 0x2b, 0x48, 0x04, 0x47, 0x49, 0x14, 0x1c, 0x35, 0x3b, 0x46, 0x4a, 0x24, 0x2c, 0x58, 0x45, 0x4b, 0x34, 0x3c, 0x03,
    0x57, 0x59, 0x13, 0x1d, 0x56, 0x5a, 0x23, 0x2d, 0x44, 0x4c, 0x55, 0x5b, 0x33, 0x3d, 0x68, 0x02, 0x67, 0x69, 0x12, 0x1e,
    0x66, 0x6a, 0x22, 0x2e, 0x54, 0x5c, 0x43, 0x4d, 0x65, 0x6b, 0x32, 0x3e, 0x78, 0x01, 0x77, 0x79, 0x53, 0x5d, 0x11, 0x1f,
    0x64, 0x6c, 0x42, 0x4e, 0x76, 0x7a, 0x21, 0x2f, 0x75, 0x7b, 0x31, 0x3f, 0x63, 0x6d, 0x52, 0x5e, 0x00, 0x74, 0x7c, 0x41,
    0x4f, 0x10, 0x20, 0x62, 0x6e, 0x30, 0x73, 0x7d, 0x51, 0x5f, 0x40, 0x72, 0x7e, 0x61, 0x6f, 0x50, 0x71, 0x7f, 0x60, 0x70};

static int stream(vp8l_t *l, u32 xs, u32 ys, bool top, u32 **out);

/* The transforms (top-level image only), each type once; xs becomes the coded width. */
static int transforms(vp8l_t *l, u32 *xs, u32 ys)
{
    while (l_bits(l, 1)) {
        int t = (int)l_bits(l, 2);
        if (l->seen[t] || l->ntr >= 4)
            return IMGDEC_E_CORRUPT;
        l->seen[t] = true;
        wtr_t *tr = &l->tr[l->ntr++];
        tr->type = (uint8_t)t;
        tr->xs = *xs;
        if (t == 0 || t == 1) { /* predictor, cross color: a sub-image of modes / multipliers */
            tr->bits = (uint8_t)(l_bits(l, 3) + 2);
            int rc = stream(l, sub_size(*xs, tr->bits), sub_size(ys, tr->bits), false, &tr->data);
            if (rc)
                return rc;
        } else if (t == 3) { /* color indexing: a palette; pixels packed 2, 4 or 8 to one */
            u32 n = l_bits(l, 8) + 1;
            tr->bits = (uint8_t)(n > 16 ? 0 : n > 4 ? 1 : n > 2 ? 2 : 3);
            uint32_t *pal;
            int rc = stream(l, n, 1, false, &pal);
            if (rc)
                return rc;
            u32 full = 1u << (8 >> tr->bits);
            tr->data = l_take(l, 256 * sizeof(uint32_t));
            if (!tr->data)
                return IMGDEC_E_NOMEM;
            memset(tr->data, 0, 256 * sizeof(uint32_t));
            uint8_t *d = (uint8_t *)tr->data;
            const uint8_t *sp = (const uint8_t *)pal;
            memcpy(d, sp, 4);
            for (u32 i = 4; i < 4 * n; i++) /* coded as differences */
                d[i] = (uint8_t)(sp[i] + d[i - 4]);
            (void)full; /* past the palette: black (transparent), as libwebp's tail of 0 */
            *xs = sub_size(*xs, tr->bits);
        }
        if (l_eos(l))
            return IMGDEC_E_CORRUPT;
    }
    return IMGDEC_OK;
}

/* An entropy-coded image of xs x ys (top: the main image, with transforms and a meta prefix image). */
static int stream(vp8l_t *l, u32 xs, u32 ys, bool top, u32 **out)
{
    int rc;
    int cbits = 0;
    if (l_bits(l, 1)) {
        cbits = (int)l_bits(l, 4);
        if (cbits < 1 || cbits > 11)
            return IMGDEC_E_CORRUPT;
    }
    uint32_t *meta = NULL;
    int mbits = 0;
    u32 mxs = 0, ngroups = 1;
    uint16_t *map = NULL;
    if (top && l_bits(l, 1)) { /* the meta prefix image: a group per tile */
        mbits = (int)l_bits(l, 3) + 2;
        mxs = sub_size(xs, mbits);
        if ((rc = stream(l, mxs, sub_size(ys, mbits), false, &meta)))
            return rc;
        u32 n = mxs * sub_size(ys, mbits), gmax = 0;
        for (u32 i = 0; i < n; i++) {
            meta[i] = (meta[i] >> 8) & 0xffff;
            if (meta[i] + 1 > gmax)
                gmax = meta[i] + 1;
        }
        map = l_take(l, gmax * sizeof *map); /* groups used, in order; 0xffff: read and dropped */
        if (!map)
            return IMGDEC_E_NOMEM;
        memset(map, 0xff, gmax * sizeof *map);
        ngroups = 0;
        for (u32 i = 0; i < n; i++) {
            if (map[meta[i]] == 0xffff)
                map[meta[i]] = (uint16_t)ngroups++;
            meta[i] = map[meta[i]];
        }
        ngroups = gmax;
    }
    u32 used_groups = map ? 0 : 1;
    for (u32 g = 0; map && g < ngroups; g++)
        used_groups += map[g] != 0xffff;
    hgroup_t *groups = l_take(l, used_groups * sizeof *groups);
    uint8_t *len = l_take(l, 280 + 2048);
    if (!groups || !len)
        return IMGDEC_E_NOMEM;
    static const uint16_t ASZ[5] = {256 + 24, 256, 256, 256, 40};
    for (u32 g = 0; g < ngroups; g++) {
        hgroup_t tmp, *hg = map && map[g] == 0xffff ? &tmp : &groups[map ? map[g] : 0];
        wblk_t *mark = l->blk;
        size_t used = mark ? mark->used : 0;
        for (int k = 0; k < 5; k++)
            if (!h_code(l, &hg->c[k], ASZ[k] + (k == 0 && cbits ? 1 << cbits : 0), len))
                return IMGDEC_E_CORRUPT;
        if (hg == &tmp && l->blk == mark && mark)
            mark->used = used;
    }
    uint32_t *cache = cbits ? l_take(l, (sizeof(uint32_t)) << cbits) : NULL;
    uint64_t np = (uint64_t)xs * ys;
    uint32_t *data = top ? al(l->io, (size_t)np * 4) : l_take(l, (size_t)np * 4);
    if ((cbits && !cache) || !data)
        return IMGDEC_E_NOMEM;
    if (cache)
        memset(cache, 0, (sizeof(uint32_t)) << cbits);
    *out = data;
    u32 x = 0, y = 0, mmask = mbits ? (1u << mbits) - 1 : ~0u;
    uint64_t pos = 0, cached = 0;
    const hgroup_t *hg = &groups[0];
    int cshift = 32 - cbits;
    while (pos < np) {
        if (meta && (x & mmask) == 0)
            hg = &groups[meta[(y >> mbits) * mxs + (x >> mbits)]];
        int c = h_read(l, &hg->c[0]);
        if (c < 0 || l_eos(l))
            return IMGDEC_E_CORRUPT;
        if (c < 256) { /* a literal */
            int r = h_read(l, &hg->c[1]), b = h_read(l, &hg->c[2]), a = h_read(l, &hg->c[3]);
            if (r < 0 || b < 0 || a < 0)
                return IMGDEC_E_CORRUPT;
            data[pos++] = (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)c << 8 | (uint32_t)b;
            if (++x >= xs) {
                x = 0;
                y++;
            }
        } else if (c < 256 + 24) { /* a copy of earlier pixels */
            int n = l_copy_len(l, c - 256), ds = h_read(l, &hg->c[4]);
            if (ds < 0)
                return IMGDEC_E_CORRUPT;
            int code = l_copy_len(l, ds), dist;
            if (code > 120) {
                dist = code - 120;
            } else {
                int dc = PLANE[code - 1];
                dist = (dc >> 4) * (int)xs + 8 - (dc & 15);
                if (dist < 1)
                    dist = 1;
            }
            if ((uint64_t)dist > pos || (uint64_t)n > np - pos || l_eos(l))
                return IMGDEC_E_CORRUPT;
            for (int i = 0; i < n; i++, pos++)
                data[pos] = data[pos - (u32)dist];
            x += (u32)n;
            while (x >= xs) {
                x -= xs;
                y++;
            }
            if (meta && (x & mmask))
                hg = &groups[meta[(y >> mbits) * mxs + (x >> mbits)]];
        } else { /* the color cache */
            u32 k = (u32)c - 280;
            if (!cache || k >= 1u << cbits)
                return IMGDEC_E_CORRUPT;
            while (cached < pos) {
                uint32_t p = data[cached++];
                cache[(p * 0x1e35a7bdu) >> cshift] = p;
            }
            data[pos++] = cache[k];
            if (++x >= xs) {
                x = 0;
                y++;
            }
        }
    }
    return l_eos(l) ? IMGDEC_E_CORRUPT : IMGDEC_OK;
}

static uint32_t addpx(uint32_t a, uint32_t b)
{
    return (((a & 0xff00ff00u) + (b & 0xff00ff00u)) & 0xff00ff00u) | (((a & 0x00ff00ffu) + (b & 0x00ff00ffu)) & 0x00ff00ffu);
}
static uint32_t avg2(uint32_t a, uint32_t b) { return (((a ^ b) & 0xfefefefeu) >> 1) + (a & b); }
static uint32_t clip255(uint32_t a) { return a < 256 ? a : ~a >> 24; }
static uint32_t cas_full(uint32_t c0, uint32_t c1, uint32_t c2)
{
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8)
        r |= clip255((uint32_t)((int)(c0 >> s & 255) + (int)(c1 >> s & 255) - (int)(c2 >> s & 255))) << s;
    return r;
}
static uint32_t cas_half(uint32_t c0, uint32_t c1, uint32_t c2)
{
    uint32_t ave = avg2(c0, c1), r = 0;
    for (int s = 0; s < 32; s += 8) {
        int a = (int)(ave >> s & 255), b = (int)(c2 >> s & 255);
        r |= clip255((uint32_t)(a + (a - b) / 2)) << s;
    }
    return r;
}
#define SUB3(s) (iabs((int)(b >> s & 255) - (int)(c >> s & 255)) - iabs((int)(a >> s & 255) - (int)(c >> s & 255)))
static inline uint32_t sel(uint32_t a, uint32_t b, uint32_t c)
{
    return SUB3(24) + SUB3(16) + SUB3(8) + SUB3(0) <= 0 ? a : b;
}
#undef SUB3


/* A run of pixels of one predictor mode: out[i] = in[i] + prediction (left out[i - 1], top T[i]). */
typedef void (*padd_t)(const uint32_t *in, const uint32_t *T, int n, uint32_t *out);
#define PADD(NAME, EXPR)                                                                                               \
    static void NAME(const uint32_t *in, const uint32_t *T, int n, uint32_t *out)                                      \
    {                                                                                                                  \
        for (int i = 0; i < n; i++) {                                                                                  \
            uint32_t L = out[i - 1];                                                                                   \
            const uint32_t *t = T + i;                                                                                 \
            (void)L;                                                                                                   \
            (void)t;                                                                                                   \
            out[i] = addpx(in[i], (EXPR));                                                                             \
        }                                                                                                              \
    }
PADD(padd0, 0xff000000u)
PADD(padd1, L)
PADD(padd2, t[0])
PADD(padd3, t[1])
PADD(padd4, t[-1])
PADD(padd5, avg2(avg2(L, t[1]), t[0]))
PADD(padd6, avg2(L, t[-1]))
PADD(padd7, avg2(L, t[0]))
PADD(padd8, avg2(t[-1], t[0]))
PADD(padd9, avg2(t[0], t[1]))
PADD(padd10, avg2(avg2(L, t[-1]), avg2(t[0], t[1])))
PADD(padd11, sel(t[0], L, t[-1]))
PADD(padd12, cas_full(L, t[0], t[-1]))
PADD(padd13, cas_half(L, t[0], t[-1]))
static const padd_t PADDS[16] = {padd0, padd1, padd2,  padd3,  padd4,  padd5,  padd6, padd7,
                                 padd8, padd9, padd10, padd11, padd12, padd13, padd0, padd0};

static int ctd(int8_t pred, int8_t c) { return ((int)pred * c) >> 5; }

/* Row y of the main image through the transforms (last read, first undone) into ARGB of the full width. */
static const uint32_t *l_row(vp8l_t *l, const uint32_t *in, u32 y)
{
    for (int i = l->ntr - 1; i >= 0; i--) {
        wtr_t *t = &l->tr[i];
        u32 w = t->xs;
        uint32_t *o = t->type == 0 ? t->rows + w : t->rows;
        switch (t->type) {
        case 0: { /* predictor: this row after the previous (t->rows), so T[1] of the last pixel is this row's first */
            if (y == 0) {
                o[0] = addpx(in[0], 0xff000000u);
                for (u32 x = 1; x < w; x++)
                    o[x] = addpx(in[x], o[x - 1]);
            } else {
                const uint32_t *T = o - w, *modes = t->data + (y >> t->bits) * sub_size(w, t->bits);
                u32 tile = 1u << t->bits;
                o[0] = addpx(in[0], T[0]);
                for (u32 x = 1; x < w;) { /* a tile's run at a time */
                    u32 xe = (x & ~(tile - 1)) + tile;
                    if (xe > w)
                        xe = w;
                    PADDS[(modes[x >> t->bits] >> 8) & 15](in + x, T + x, (int)(xe - x), o + x);
                    x = xe;
                }
            }
            break;
        }
        case 1: { /* cross color */
            const uint32_t *m = t->data + (y >> t->bits) * sub_size(w, t->bits);
            for (u32 x = 0; x < w; x++) {
                uint32_t c = m[x >> t->bits], p = in[x];
                int8_t g = (int8_t)(p >> 8);
                int r = (int)(p >> 16 & 255) + ctd((int8_t)c, g);
                r &= 255;
                int b = (int)(p & 255) + ctd((int8_t)(c >> 8), g) + ctd((int8_t)(c >> 16), (int8_t)r);
                o[x] = (p & 0xff00ff00u) | (uint32_t)r << 16 | (uint32_t)(b & 255);
            }
            break;
        }
        case 2: /* subtract green */
            for (u32 x = 0; x < w; x++) {
                uint32_t p = in[x], g = p >> 8 & 255, rb = ((p & 0x00ff00ffu) + (g << 16 | g)) & 0x00ff00ffu;
                o[x] = (p & 0xff00ff00u) | rb;
            }
            break;
        default: { /* color indexing */
            int bpp = 8 >> t->bits;
            u32 cm = (1u << t->bits) - 1, bm = (1u << bpp) - 1, packed = 0;
            for (u32 x = 0; x < w; x++) {
                if ((x & cm) == 0)
                    packed = *in++ >> 8 & 255;
                o[x] = t->data[packed & bm];
                packed >>= bpp;
            }
            break;
        }
        }
        in = o;
    }
    return in;
}

/* After row y: a predictor's row becomes its previous. */
static void l_row_done(vp8l_t *l)
{
    for (int i = 0; i < l->ntr; i++)
        if (l->tr[i].type == 0)
            memcpy(l->tr[i].rows, l->tr[i].rows + l->tr[i].xs, l->tr[i].xs * 4);
}

/* A lossless image of w x h in the byte range s (no header: an ALPH chunk's; with: a VP8L chunk's). Main image
 * pixels into *img (the coded width *cw), the transforms ready in l. */
static int l_open(vp8l_t *l, wfile_t *f, wsrc_t *s, u32 *w, u32 *h, bool header, uint32_t **img, u32 *cw)
{
    memset(l, 0, sizeof *l);
    l->f = f;
    l->io = f->io;
    l->s = s;
    l->avail = (uint64_t)(s->end - s->off) * 8;
    if (header) {
        if (l_bits(l, 8) != 0x2f)
            return IMGDEC_E_CORRUPT;
        *w = l_bits(l, 14) + 1;
        *h = l_bits(l, 14) + 1;
        l_bits(l, 1); /* alpha used: the pixels say */
        if (l_bits(l, 3) != 0)
            return IMGDEC_E_CORRUPT;
    }
    u32 xs = *w;
    int rc = transforms(l, &xs, *h);
    if (rc)
        return rc;
    for (int i = 0; i < l->ntr; i++) {
        wtr_t *t = &l->tr[i];
        t->rows = l_take(l, (t->type == 0 ? 2u : 1u) * t->xs * 4);
        if (!t->rows)
            return IMGDEC_E_NOMEM;
    }
    *cw = xs;
    return stream(l, xs, *h, true, img);
}

/* ------------------------------------------------------------------ container (RIFF) */

static u32 le32(const uint8_t *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

/* A lossless (VP8L) chunk's image into rows: ARGB over white, gray (54 R + 183 G + 19 B) >> 8 or RGB. */
static int vp8l_frame(wfile_t *f, imgdec_info_t *info, u32 off, u32 n)
{
    const imgdec_io_t *io = f->io;
    vp8l_t *l = al(io, sizeof *l);
    wsrc_t *s = al(io, sizeof *s);
    uint32_t *img = NULL;
    uint8_t *row = NULL;
    int rc = l && s ? IMGDEC_OK : IMGDEC_E_NOMEM;
    if (!rc) {
        memset(l, 0, sizeof *l);
        memset(s, 0, sizeof *s);
        s->f = f;
        s->off = off + 5; /* after the header, read here */
        s->end = off + n;
        u32 w = 0, h = 0, cw;
        /* the header first, to know the size before the rest is decoded */
        uint8_t hd[5];
        if (n < 5 || !f_seek(f, off) || f_read(f, hd, 5) != 5 || hd[0] != 0x2f) {
            rc = f->err ? f->err : IMGDEC_E_CORRUPT;
        } else {
            u32 v = le32(hd + 1);
            w = (v & 0x3fff) + 1;
            h = (v >> 14 & 0x3fff) + 1;
            if ((v >> 29) != 0)
                rc = IMGDEC_E_CORRUPT; /* version */
            else if (w > IMGDEC_MAX_W || h > IMGDEC_MAX_H)
                rc = IMGDEC_E_SIZE;
            else if (info->w && (info->w != w || info->h != h))
                rc = IMGDEC_E_CORRUPT;
        }
        if (!rc) {
            info->w = info->rw = (uint16_t)w;
            info->h = info->rh = (uint16_t)h;
            info->bits = 32;
            info->whole = 1;
            if (io->info && io->info(io->ctx, info))
                rc = IMGDEC_E_ABORT;
        }
        if (!rc)
            rc = l_open(l, f, s, &w, &h, false, &img, &cw);
        bool rgb = io->row8 && io->want_rgb;
        if (!rc && !(row = al(io, (rgb ? 3u : 1u) * w)))
            rc = IMGDEC_E_NOMEM;
        for (u32 y = 0; y < h && !rc; y++) {
            const uint32_t *p = l_row(l, img + (size_t)y * cw, y);
            for (u32 x = 0; x < w; x++) {
                uint32_t c = p[x];
                int a = (int)(c >> 24), r = over_white(c >> 16 & 255, a), g = over_white(c >> 8 & 255, a),
                    b = over_white(c & 255, a);
                if (rgb) {
                    row[3 * x] = (uint8_t)r;
                    row[3 * x + 1] = (uint8_t)g;
                    row[3 * x + 2] = (uint8_t)b;
                } else {
                    row[x] = (uint8_t)((54 * r + 183 * g + 19 * b) >> 8);
                }
            }
            l_row_done(l);
            rc = put_row(io, row, y, w, rgb);
        }
        l_free_all(l);
    }
    fr(io, img);
    fr(io, row);
    fr(io, s);
    fr(io, l);
    return rc;
}

/* An ALPH chunk (after its header byte, off .. off + n) into a w x h plane: raw or lossless (its green), then
 * unfiltered (none, horizontal, vertical, gradient). */
static int alpha_plane(wfile_t *f, u32 off, u32 n, u32 w, u32 h, uint8_t **out)
{
    const imgdec_io_t *io = f->io;
    uint8_t hd;
    if (!n || !f_seek(f, off) || f_read(f, &hd, 1) != 1)
        return f->err ? f->err : IMGDEC_E_CORRUPT;
    int method = hd & 3, filter = hd >> 2 & 3;
    if (method > 1 || (hd >> 4 & 3) > 1 || (hd >> 6))
        return IMGDEC_E_CORRUPT;
    uint8_t *a = al(io, (size_t)w * h);
    if (!a)
        return IMGDEC_E_NOMEM;
    *out = a;
    int rc = IMGDEC_OK;
    if (method == 0) {
        if (n - 1 < w * h || f_read(f, a, (size_t)w * h) != (size_t)w * h)
            rc = f->err ? f->err : IMGDEC_E_CORRUPT;
    } else {
        vp8l_t *l = al(io, sizeof *l);
        wsrc_t *s = al(io, sizeof *s);
        uint32_t *img = NULL;
        rc = l && s ? IMGDEC_OK : IMGDEC_E_NOMEM;
        if (!rc) {
            memset(l, 0, sizeof *l);
            memset(s, 0, sizeof *s);
            s->f = f;
            s->off = off + 1;
            s->end = off + n;
            u32 cw, ww = w, hh = h;
            rc = l_open(l, f, s, &ww, &hh, false, &img, &cw);
            for (u32 y = 0; y < h && !rc; y++) {
                const uint32_t *p = l_row(l, img + (size_t)y * cw, y);
                for (u32 x = 0; x < w; x++)
                    a[(size_t)y * w + x] = (uint8_t)(p[x] >> 8);
                l_row_done(l);
            }
            l_free_all(l);
        }
        fr(io, img);
        fr(io, s);
        fr(io, l);
    }
    for (u32 y = 0; y < h && !rc && filter; y++) { /* libwebp dsp/filters.c, in place */
        uint8_t *o = a + (size_t)y * w;
        const uint8_t *prev = y ? o - w : NULL;
        if (filter == 1 || !prev) {
            uint8_t pred = prev ? prev[0] : 0;
            for (u32 x = 0; x < w; x++)
                pred = o[x] = (uint8_t)(pred + o[x]);
        } else if (filter == 2) {
            for (u32 x = 0; x < w; x++)
                o[x] = (uint8_t)(prev[x] + o[x]);
        } else {
            uint8_t top = prev[0], tl = top, left = top;
            for (u32 x = 0; x < w; x++) {
                top = prev[x];
                int g = left + top - tl;
                left = o[x] = (uint8_t)(o[x] + (!(g & ~0xff) ? g : g < 0 ? 0 : 255));
                tl = top;
            }
        }
    }
    return rc;
}

/* The chunks of one image from pos to end: ALPH (kept), then VP8 or VP8L into rows. first: the first chunk's
 * header when already read (a stream cannot go back to it). */
static int image_chunks(wfile_t *f, imgdec_info_t *info, u32 pos, u32 end, const uint8_t *first)
{
    const imgdec_io_t *io = f->io;
    uint8_t *held = NULL, *alpha = NULL;
    int rc = IMGDEC_E_CORRUPT;
    while (pos + 8 <= end) {
        uint8_t c[8];
        if (first) {
            memcpy(c, first, 8);
            first = NULL;
        } else if (!f_seek(f, pos) || f_read(f, c, 8) != 8) {
            break;
        }
        u32 n = le32(c + 4), body = pos + 8;
        if (n > end - body)
            break;
        if (!memcmp(c, "ALPH", 4)) { /* decoded here: a stream is past it at the image (needs the canvas: VP8X) */
            fr(io, alpha);
            alpha = NULL;
            rc = info->w ? alpha_plane(f, body, n, info->w, info->h, &alpha) : IMGDEC_E_CORRUPT;
            if (rc)
                goto out;
            info->bits = 32;
            info->whole = 1;
        } else if (!memcmp(c, "VP8 ", 4) || !memcmp(c, "VP8L", 4)) {
            if (c[3] == 'L') {
                rc = vp8l_frame(f, info, body, n);
            } else {
                rc = f_seek(f, body) ? vp8_frame(f, info, body, n, &held, alpha) : IMGDEC_E_IO;
            }
            if (!rc && f->fpos < body + n) { /* the file reaches the chunk's end (a cut file fails) */
                uint8_t last;
                if (!f_seek(f, body + n - 1) || f_read(f, &last, 1) != 1)
                    rc = f->err ? f->err : IMGDEC_E_CORRUPT;
            }
            goto out;
        }
        pos = body + n + (n & 1);
    }
    rc = f->err ? f->err : IMGDEC_E_CORRUPT; /* no image */
out:
    fr(io, held);
    fr(io, alpha);
    return rc;
}

/* ------------------------------------------------------------------ animation: the first frame on the canvas */

typedef struct {
    const imgdec_io_t *io; /* the caller's */
    imgdec_io_t pio;       /* the frame decoders' (its callbacks below) */
    u32 W, H, left, top, fw, next; /* canvas, frame place and width, the next canvas row */
    uint8_t *row;
    int err;
} anim_t;

static long a_read(void *c, uint8_t *b, size_t n) { const imgdec_io_t *io = ((anim_t *)c)->io; return io->read(io->ctx, b, n); }
static int a_seek(void *c, uint32_t off) { const imgdec_io_t *io = ((anim_t *)c)->io; return io->seek(io->ctx, off); }
static void *a_alloc(void *c, size_t n) { const imgdec_io_t *io = ((anim_t *)c)->io; return io->alloc(io->ctx, n); }
static void a_free(void *c, void *p) { const imgdec_io_t *io = ((anim_t *)c)->io; io->free(io->ctx, p); }
static int a_info(void *c, const imgdec_info_t *in) { (void)c, (void)in; return 0; } /* the canvas's was given */

/* Canvas row a->next: white, with the frame's row px (w pixels of ch bytes, NULL none) at its place. */
static int a_put(anim_t *a, const uint8_t *px, u32 w, uint8_t ch, bool levels)
{
    const imgdec_io_t *io = a->io;
    memset(a->row, levels ? 15 : 255, (size_t)a->W * ch);
    if (px)
        memcpy(a->row + (size_t)a->left * ch, px, (size_t)w * ch);
    uint16_t y = (uint16_t)a->next++;
    int stop = levels ? io->row(io->ctx, y, a->row, (uint16_t)a->W) : io->row8(io->ctx, y, a->row, (uint16_t)a->W, ch);
    return stop;
}

static int a_row8(void *c, uint16_t y, const uint8_t *px, uint16_t w, uint8_t ch)
{
    anim_t *a = c;
    while (a->next < a->top + y)
        if (a_put(a, NULL, 0, ch, false))
            return 1;
    return a_put(a, px, w, ch, false);
}

static int a_row(void *c, uint16_t y, const uint8_t *g, uint16_t w)
{
    anim_t *a = c;
    while (a->next < a->top + y)
        if (a_put(a, NULL, 0, 1, true))
            return 1;
    return a_put(a, g, w, 1, true);
}

/* The chunks after VP8X of an animation (pos .. end): ANIM, then the first ANMF's frame on the canvas. */
static int animation(wfile_t *f, imgdec_info_t *info, u32 pos, u32 end)
{
    const imgdec_io_t *io = f->io;
    while (pos + 8 <= end) {
        uint8_t c[24];
        if (!f_seek(f, pos) || f_read(f, c, 8) != 8)
            break;
        u32 n = le32(c + 4), body = pos + 8;
        if (n > end - body)
            break;
        if (!memcmp(c, "ANMF", 4)) {
            if (n < 16 || f_read(f, c + 8, 16) != 16)
                break;
            u32 x = 2 * imgwebp_le24(c + 8), y = 2 * imgwebp_le24(c + 11), w = imgwebp_le24(c + 14) + 1, h = imgwebp_le24(c + 17) + 1;
            if (x + w > info->w || y + h > info->h)
                return IMGDEC_E_CORRUPT; /* not within the canvas */
            if (io->info && io->info(io->ctx, info))
                return IMGDEC_E_ABORT;
            anim_t *a = al(io, sizeof *a);
            if (!a)
                return IMGDEC_E_NOMEM;
            memset(a, 0, sizeof *a);
            bool rgb = io->row8 && io->want_rgb;
            a->io = io;
            a->W = info->w;
            a->H = info->h;
            a->left = x;
            a->top = y;
            a->fw = w;
            a->pio = *io;
            a->pio.ctx = a;
            a->pio.read = a_read;
            a->pio.seek = io->seek ? a_seek : NULL;
            a->pio.alloc = a_alloc;
            a->pio.free = a_free;
            a->pio.info = a_info;
            a->pio.row8 = io->row8 ? a_row8 : NULL;
            a->pio.row = io->row ? a_row : NULL;
            a->row = al(io, (rgb ? 3u : 1u) * info->w);
            int rc = a->row ? IMGDEC_OK : IMGDEC_E_NOMEM;
            if (!rc) {
                imgdec_info_t fi = *info;
                fi.w = fi.rw = (uint16_t)w;
                fi.h = fi.rh = (uint16_t)h;
                f->io = &a->pio;
                rc = image_chunks(f, &fi, body + 16, body + n, NULL);
                f->io = io;
                if (rc == IMGDEC_E_ABORT && a->next < a->H && !a->err)
                    rc = IMGDEC_E_ABORT;
                info->bits = fi.bits;
                info->whole = fi.whole;
                uint8_t ch = rgb ? 3 : 1;
                while (!rc && a->next < a->H) /* white below the frame */
                    if (a_put(a, NULL, 0, ch, !io->row8))
                        rc = IMGDEC_E_ABORT;
            }
            fr(io, a->row);
            fr(io, a);
            return rc;
        }
        pos = body + n + (n & 1);
    }
    return f->err ? f->err : IMGDEC_E_CORRUPT; /* no frame */
}

int imgwebp_decode(const imgdec_io_t *io, imgdec_info_t *info, const uint8_t *pre, size_t pre_n, uint32_t pre_off)
{
    wfile_t f = {io, pre, pre_n, 0, pre_off, false, 0};
    uint8_t h[12] = {'R', 'I'};
    if (pre_off != 2 || f_read(&f, h + 2, 10) != 10 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WEBP", 4))
        return f.err ? f.err : IMGDEC_E_FORMAT;
    memset(info, 0, sizeof *info);
    info->format = IMG_WEBP;
    info->orient = 1;
    info->gray_bits = 8;
    info->color = 1;
    info->bits = 24;
    u32 riff_end = 8 + le32(h + 4), pos = 12;
    uint8_t c[18];
    if (f_read(&f, c, 8) != 8)
        return f.err ? f.err : IMGDEC_E_CORRUPT;
    if (memcmp(c, "VP8X", 4))
        return image_chunks(&f, info, pos, riff_end, c);
    if (f_read(&f, c + 8, 10) != 10)
        return f.err ? f.err : IMGDEC_E_CORRUPT;
    { /* extended: the canvas, then its chunks */
        u32 w = imgwebp_le24(c + 12) + 1, hh = imgwebp_le24(c + 15) + 1;
        if (w > IMGDEC_MAX_W || hh > IMGDEC_MAX_H)
            return IMGDEC_E_SIZE;
        info->w = info->rw = (uint16_t)w;
        info->h = info->rh = (uint16_t)hh;
        pos += 8 + le32(c + 4) + (le32(c + 4) & 1);
        if (c[8] & 0x02)
            return animation(&f, info, pos, riff_end);
    }
    return image_chunks(&f, info, pos, riff_end, NULL);
}

/* Lossy rows; held whole (lossless, alpha): the pixels, the alpha plane, transform rows, prefix codes. */
size_t imgwebp_need(const imgdec_info_t *info)
{
    size_t w = info->w, n = sizeof(wfile_t) + vp8_need((u32)w) + sizeof(anim_t) + 3 * w + 64;
    if (info->whole)
        n += w * info->h * 5 + w * 5 * 4 + sizeof(vp8l_t) + sizeof(wsrc_t) + 262144;
    return n;
}
