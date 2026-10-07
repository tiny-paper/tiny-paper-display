/* imgdec private: the inflate state shared with the PNG decoder. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "imgdec_priv.h"

typedef struct {
    uint16_t counts[16];
    uint16_t symbols[288];
} huff_t;

typedef struct {
    src_t *s;
    /* the zlib stream's bytes come from IDAT chunks */
    uint32_t chunk_left;
    bool idat_end;
    uint32_t bits, nbits;
    /* window: a ring of wsize bytes (a power of two) */
    uint8_t *win;
    uint32_t wsize, wpos, total;
    huff_t lit, dist;
    int err;
} infl_t;


typedef int (*emit_fn)(void *ctx, uint8_t b);

/* The next byte of IDAT data, over chunk boundaries. -1 at the end or on error. */
int img_idat_byte(infl_t *z);
int img_inflate_all(infl_t *z, emit_fn emit, void *ectx);
