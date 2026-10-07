/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tiny/cbor.h"

#include <math.h>
#include <string.h>

void cbor_r_init(cbor_r_t *r, const uint8_t *buf, size_t len)
{
    r->p = buf;
    r->end = buf + len;
    r->err = false;
}

bool cbor_at_end(const cbor_r_t *r) { return r->p == r->end; }

static bool fail(cbor_r_t *r)
{
    r->err = true;
    return false;
}

static double half_to_double(uint16_t h)
{
    int e = (h >> 10) & 0x1f;
    int m = h & 0x3ff;
    double v;
    if (e == 0)
        v = ldexp(m, -24);
    else if (e != 31)
        v = ldexp(m + 1024, e - 25);
    else
        v = m == 0 ? INFINITY : NAN;
    return (h & 0x8000) ? -v : v;
}

bool cbor_read(cbor_r_t *r, cbor_item_t *it)
{
    if (r->err || r->p >= r->end)
        return fail(r);
    uint8_t ib = *r->p++;
    uint8_t major = ib >> 5, ai = ib & 0x1f;
    uint64_t v = 0;
    memset(it, 0, sizeof *it);

    if (major == 7) {
        switch (ai) {
        case 20: it->type = CBOR_FALSE; return true;
        case 21: it->type = CBOR_TRUE; return true;
        case 22: it->type = CBOR_NULL; return true;
        case 25: case 26: case 27: {
            size_t n = (size_t)1 << (ai - 24);
            if ((size_t)(r->end - r->p) < n)
                return fail(r);
            uint64_t bits = 0;
            for (size_t i = 0; i < n; i++)
                bits = (bits << 8) | *r->p++;
            it->type = CBOR_FLOAT;
            if (ai == 25) {
                it->f = half_to_double((uint16_t)bits);
            } else if (ai == 26) {
                uint32_t b32 = (uint32_t)bits;
                float f;
                memcpy(&f, &b32, 4);
                it->f = f;
            } else {
                memcpy(&it->f, &bits, 8);
            }
            return true;
        }
        default: return fail(r); /* undefined, simple values, break */
        }
    }
    if (major == 6)
        return fail(r); /* tags are not in the profile */
    if (ai < 24) {
        v = ai;
    } else if (ai <= 27) {
        size_t n = (size_t)1 << (ai - 24);
        if ((size_t)(r->end - r->p) < n)
            return fail(r);
        for (size_t i = 0; i < n; i++)
            v = (v << 8) | *r->p++;
        /* shortest form required */
        if ((ai == 24 && v < 24) || (ai == 25 && v <= 0xff) || (ai == 26 && v <= 0xffff) ||
            (ai == 27 && v <= 0xffffffffu))
            return fail(r);
    } else {
        return fail(r); /* reserved or indefinite length */
    }
    it->u = v;
    switch (major) {
    case 0: it->type = CBOR_UINT; break;
    case 1: it->type = CBOR_NINT; break;
    case 2:
    case 3:
        if ((uint64_t)(r->end - r->p) < v)
            return fail(r);
        it->type = major == 2 ? CBOR_BSTR : CBOR_TSTR;
        it->ptr = r->p;
        it->len = (size_t)v;
        r->p += v;
        break;
    case 4: it->type = CBOR_ARRAY; break;
    case 5: it->type = CBOR_MAP; break;
    }
    return true;
}

/* One item with everything in it, without recursion: the stack use does not grow with the nesting (the C3's task
 * stack has nothing to spare; OG 0459 overflowed it validating a package inside a check-in). Per open container the
 * items left (a map counts keys and values) and, for check, its pair count and first key. check: maps need text
 * keys without duplicates. Depth as before: the top item is depth 1, at most CBOR_MAX_DEPTH.
 * work (check): the reads of the duplicate scan, counted; over CBOR_KEY_WORK_MAX the item is refused, so the scan
 * (quadratic in the pairs of a map) is bounded for any input. Without check and with work: every read counts. */
static bool walk(cbor_r_t *r, bool check, uint32_t *work)
{
    uint64_t left[CBOR_MAX_DEPTH + 1], pairs[CBOR_MAX_DEPTH + 1];
    const uint8_t *keys[CBOR_MAX_DEPTH + 1];
    int d = 0;
    left[0] = 1;
    pairs[0] = 0;
    for (;;) {
        while (!left[d])
            if (!d--)
                return true;
        if (d >= CBOR_MAX_DEPTH)
            return fail(r);
        left[d]--;
        cbor_item_t it;
        if (!check && work && ++*work > CBOR_KEY_WORK_MAX)
            return fail(r);
        if (!cbor_read(r, &it))
            return false;
        if (check && pairs[d] && (left[d] & 1)) { /* a key: text, not one of the earlier keys of this map */
            if (it.type != CBOR_TSTR)
                return fail(r);
            cbor_r_t scan;
            cbor_r_init(&scan, keys[d], (size_t)(r->end - keys[d]));
            for (uint64_t j = pairs[d] - (left[d] + 1) / 2; j; j--) {
                cbor_item_t pk;
                if (++*work > CBOR_KEY_WORK_MAX)
                    return fail(r);
                cbor_read(&scan, &pk);
                if (pk.len == it.len && memcmp(pk.ptr, it.ptr, it.len) == 0)
                    return fail(r);
                if (!walk(&scan, false, work))
                    return fail(r);
            }
            continue;
        }
        if ((it.type == CBOR_ARRAY || it.type == CBOR_MAP) && it.u) {
            /* each child needs at least one byte; compared before doubling (a map count >= 2^63 would wrap) */
            uint64_t room = (uint64_t)(r->end - r->p);
            if (it.u > (it.type == CBOR_MAP ? room / 2 : room))
                return fail(r);
            d++;
            left[d] = it.type == CBOR_MAP ? it.u * 2 : it.u;
            pairs[d] = it.type == CBOR_MAP ? it.u : 0;
            keys[d] = r->p;
        }
    }
}

bool cbor_skip(cbor_r_t *r) { return walk(r, false, NULL); }

bool cbor_as_int(const cbor_item_t *it, int64_t *out)
{
    if (it->type == CBOR_UINT && it->u <= INT64_MAX) {
        *out = (int64_t)it->u;
        return true;
    }
    if (it->type == CBOR_NINT && it->u <= INT64_MAX) {
        *out = -1 - (int64_t)it->u;
        return true;
    }
    return false;
}

bool cbor_as_num(const cbor_item_t *it, double *out)
{
    int64_t i;
    if (it->type == CBOR_FLOAT) {
        *out = it->f;
        return true;
    }
    if (cbor_as_int(it, &i)) {
        *out = (double)i;
        return true;
    }
    return false;
}

bool cbor_str_eq(const cbor_item_t *it, const char *s)
{
    size_t n = strlen(s);
    return it->type == CBOR_TSTR && it->len == n && memcmp(it->ptr, s, n) == 0;
}

bool cbor_valid_work(const uint8_t *buf, size_t len, uint32_t *work)
{
    cbor_r_t r;
    cbor_r_init(&r, buf, len);
    *work = 0;
    return walk(&r, true, work) && cbor_at_end(&r);
}

bool cbor_valid(const uint8_t *buf, size_t len)
{
    uint32_t work;
    return cbor_valid_work(buf, len, &work);
}

bool cbor_map_get(const uint8_t *buf, size_t len, const char *key, size_t klen,
                  const uint8_t **val, size_t *val_len)
{
    cbor_r_t r;
    cbor_item_t m, k;
    cbor_r_init(&r, buf, len);
    if (!cbor_read(&r, &m) || m.type != CBOR_MAP)
        return false;
    for (uint64_t i = 0; i < m.u; i++) {
        if (!cbor_read(&r, &k))
            return false;
        const uint8_t *start = r.p;
        if (!cbor_skip(&r))
            return false;
        if (k.type == CBOR_TSTR && k.len == klen && memcmp(k.ptr, key, klen) == 0) {
            *val = start;
            *val_len = (size_t)(r.p - start);
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ writer */

void cbor_w_init(cbor_w_t *w, uint8_t *buf, size_t cap)
{
    w->p = buf;
    w->cap = cap;
    w->len = 0;
    w->err = false;
}

void cbor_put_raw(cbor_w_t *w, const void *b, size_t len)
{
    if (w->len + len > w->cap || (len && !w->p))
        w->err = true;
    else if (len)
        memcpy(w->p + w->len, b, len);
    w->len += len;
}

static void put_head(cbor_w_t *w, uint8_t major, uint64_t v)
{
    uint8_t b[9];
    size_t n;
    if (v < 24) {
        b[0] = (uint8_t)(major << 5 | v);
        n = 1;
    } else {
        int bytes = v <= 0xff ? 1 : v <= 0xffff ? 2 : v <= 0xffffffffu ? 4 : 8;
        b[0] = (uint8_t)(major << 5 | (bytes == 1 ? 24 : bytes == 2 ? 25 : bytes == 4 ? 26 : 27));
        for (int i = 0; i < bytes; i++)
            b[1 + i] = (uint8_t)(v >> (8 * (bytes - 1 - i)));
        n = 1 + (size_t)bytes;
    }
    cbor_put_raw(w, b, n);
}

void cbor_put_uint(cbor_w_t *w, uint64_t v) { put_head(w, 0, v); }

void cbor_put_int(cbor_w_t *w, int64_t v)
{
    if (v >= 0)
        put_head(w, 0, (uint64_t)v);
    else
        put_head(w, 1, (uint64_t)(-1 - v));
}

void cbor_put_bstr(cbor_w_t *w, const void *b, size_t len)
{
    put_head(w, 2, len);
    cbor_put_raw(w, b, len);
}

void cbor_put_tstr(cbor_w_t *w, const char *s, size_t len)
{
    put_head(w, 3, len);
    cbor_put_raw(w, s, len);
}

void cbor_put_str(cbor_w_t *w, const char *s) { cbor_put_tstr(w, s, strlen(s)); }
void cbor_put_array(cbor_w_t *w, size_t n) { put_head(w, 4, n); }
void cbor_put_map(cbor_w_t *w, size_t n) { put_head(w, 5, n); }

void cbor_put_bool(cbor_w_t *w, bool v)
{
    uint8_t b = v ? 0xf5 : 0xf4;
    cbor_put_raw(w, &b, 1);
}

void cbor_put_null(cbor_w_t *w)
{
    uint8_t b = 0xf6;
    cbor_put_raw(w, &b, 1);
}

void cbor_put_f64(cbor_w_t *w, double v)
{
    uint8_t b[9];
    uint64_t bits;
    memcpy(&bits, &v, 8);
    b[0] = 0xfb;
    for (int i = 0; i < 8; i++)
        b[1 + i] = (uint8_t)(bits >> (56 - 8 * i));
    cbor_put_raw(w, b, 9);
}
