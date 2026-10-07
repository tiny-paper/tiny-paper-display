/*
 * tiny/cbor.h - minimal CBOR reader/writer for the profile in docs/spec/README.md
 * (definite lengths, no tags, text map keys, nesting <= 8). No allocation.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_CBOR_H
#define TINY_CBOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define CBOR_MAX_DEPTH 8
/* Key work of one item (docs/spec/README.md "CBOR"): for every map key the items before it in its map (earlier keys,
 * their values and all inside those), summed over all maps. The duplicate-key check costs exactly this many reads, so
 * a larger item is invalid: validation stays bounded for any input (issue #56). */
#define CBOR_KEY_WORK_MAX 4194304u

typedef enum {
    CBOR_UINT, CBOR_NINT, CBOR_BSTR, CBOR_TSTR, CBOR_ARRAY, CBOR_MAP,
    CBOR_FALSE, CBOR_TRUE, CBOR_NULL, CBOR_FLOAT,
} cbor_type_t;

typedef struct {
    cbor_type_t type;
    uint64_t u;          /* UINT value; NINT: -1 - u; ARRAY/MAP: element/pair count */
    double f;            /* FLOAT (half, single, double decoded) */
    const uint8_t *ptr;  /* BSTR/TSTR bytes */
    size_t len;          /* BSTR/TSTR byte length */
} cbor_item_t;

typedef struct {
    const uint8_t *p, *end;
    bool err;
} cbor_r_t;

void cbor_r_init(cbor_r_t *r, const uint8_t *buf, size_t len);
/* Read one head. Strings are consumed whole; for ARRAY/MAP only the head (then read
 * the children). Returns false on malformed or unsupported input (sets r->err). */
bool cbor_read(cbor_r_t *r, cbor_item_t *it);
/* Skip one complete item including children. */
bool cbor_skip(cbor_r_t *r);
bool cbor_at_end(const cbor_r_t *r);

/* Convenience accessors over a read item. */
bool cbor_as_int(const cbor_item_t *it, int64_t *out);   /* UINT/NINT within int64 */
bool cbor_as_num(const cbor_item_t *it, double *out);    /* UINT/NINT/FLOAT */
bool cbor_str_eq(const cbor_item_t *it, const char *s);  /* TSTR equal to NUL-terminated s */

/* Whole buffer is exactly one well-formed item per the profile (also rejects
 * duplicate map keys). */
bool cbor_valid(const uint8_t *buf, size_t len);
/* As cbor_valid; *work gets the key work done (at most CBOR_KEY_WORK_MAX + 1; tests, tools). */
bool cbor_valid_work(const uint8_t *buf, size_t len, uint32_t *work);

/* Find `key` in the map encoded at buf. On success *val / *val_len span the encoded
 * value item. Returns false if buf is not a map or the key is missing. */
bool cbor_map_get(const uint8_t *buf, size_t len, const char *key, size_t klen,
                  const uint8_t **val, size_t *val_len);

typedef struct {
    uint8_t *p;
    size_t cap, len;
    bool err;            /* set when out of space; len keeps counting (size probing) */
} cbor_w_t;

void cbor_w_init(cbor_w_t *w, uint8_t *buf, size_t cap);
void cbor_put_uint(cbor_w_t *w, uint64_t v);
void cbor_put_int(cbor_w_t *w, int64_t v);
void cbor_put_bstr(cbor_w_t *w, const void *b, size_t len);
void cbor_put_tstr(cbor_w_t *w, const char *s, size_t len);
void cbor_put_str(cbor_w_t *w, const char *s);           /* NUL-terminated tstr */
void cbor_put_array(cbor_w_t *w, size_t n);
void cbor_put_map(cbor_w_t *w, size_t n);
void cbor_put_bool(cbor_w_t *w, bool v);
void cbor_put_null(cbor_w_t *w);
void cbor_put_f64(cbor_w_t *w, double v);                /* always 0xfb + 8 bytes */
void cbor_put_raw(cbor_w_t *w, const void *b, size_t len); /* pre-encoded item */

#endif
