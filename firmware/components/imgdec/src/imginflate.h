/* imgdec private: the inflate state shared with the PNG decoder. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "imgdec_priv.h"

/* Codes of up to INFL_FAST bits are decoded with one table lookup, longer ones bit by bit from the canonical counts. */
#define INFL_FAST 9

typedef struct {
    uint16_t counts[16];
    uint16_t symbols[288];
    uint16_t fast[1 << INFL_FAST]; /* the next INFL_FAST bits (LSB first) -> length << 9 | symbol; 0: a longer code */
} huff_t;

typedef struct {
    src_t *s;
    /* the zlib stream's bytes come from IDAT chunks */
    uint32_t chunk_left;
    bool idat_end;
    /* bit buffer, LSB first; `over`: zero bytes put in past the end of the data (a peek near the end), an error only
     * when a decode consumes them */
    uint32_t bits, nbits, over;
    /* window: a ring of wsize bytes (a power of two); the output is handed on in spans from fpos up to wpos */
    uint8_t *win;
    uint32_t wsize, wpos, fpos, total;
    huff_t lit, dist;
    int err;
} infl_t;

/* n bytes of output, in order. Non-zero stops the inflate with that code. */
typedef int (*emit_fn)(void *ctx, const uint8_t *p, size_t n);

/* The next byte of IDAT data, over chunk boundaries. -1 at the end or on error. */
int img_idat_byte(infl_t *z);
int img_inflate_all(infl_t *z, emit_fn emit, void *ectx);
