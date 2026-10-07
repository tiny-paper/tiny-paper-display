/* epd_bwseq.c - BWRY panel command sequences (epd_bwseq.h). C11. SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_bwseq.h"

#include <string.h>

#include "epd_bwry.h"

typedef struct {
    bwq_op_t *o;
    int n;
} seq_t;

__attribute__((noinline)) static void put(seq_t *s, uint8_t op, uint8_t cmd, const uint8_t *d, uint8_t n)
{
    if (s->n >= BWQ_MAX || n > sizeof s->o->d)
        return; /* the tests check every sequence fits */
    bwq_op_t *p = &s->o[s->n++];
    memset(p, 0, sizeof *p);
    p->op = op;
    p->cmd = cmd;
    p->n = n;
    if (n)
        memcpy(p->d, d, n);
}

/* Tables: cmd, n, data[n]; n = WT: a BUSY wait of kind `cmd` (flash: one loop instead of a call per register). */
#define WT 0xf0
#define TRES 0x61, 4, BWRY_W >> 8, BWRY_W & 0xff, BWRY_H >> 8, BWRY_H & 0xff
#define GSST 0x65, 4, 0x00, 0x00, 0x00, 0x00

/* pll != 0: sent in place of the table's PLL (0x30) value, in the same place */
static void table(seq_t *s, const uint8_t *t, size_t len, uint8_t pll)
{
    for (size_t i = 0; i + 1 < len; i += t[i + 1] == WT ? 2 : 2u + t[i + 1]) {
        if (t[i + 1] == WT)
            put(s, BWQ_WAIT, t[i], NULL, 0);
        else if (pll && t[i] == 0x30 && t[i + 1] == 1)
            put(s, BWQ_CMD, 0x30, &pll, 1);
        else
            put(s, BWQ_CMD, t[i], t + i + 2, t[i + 1]);
    }
}
#define TABLE(s, t) table((s), (t), sizeof (t), 0)
#define TABLE_PLL(s, t, pll) table((s), (t), sizeof (t), (pll))

/* PicPak stock for panel 0x060401: V1.1.13 0x420207d4..0x420208e4 (also open-PicPak epd_init_panel). */
static const uint8_t STOCK[] = {
    0x00, 2, 0x07, 0x29, 0x01, 2, 0x07, 0x00, 0x06, 4, 0x0f, 0x8b, 0x9c, 0x96, 0x30, 1, 0x08, 0x50, 1, 0x37, TRES, GSST,
    0xe7, 1, 0x96, 0xe9, 1, 0x01, 0xff, 1, 0xa5,
};
/* GxEPD2_420c_GDEY0420F51::_InitDisplay() (Good Display demo values) */
static const uint8_t GD[] = {
    0x00, 2, 0x0f, 0x09, 0x01, 6, 0x07, 0x00, 0x22, 0x78, 0x0a, 0x22, 0x03, 3, 0x10, 0x54, 0x44, 0x06, 3, 0x26, 0x26,
    0x26, 0x30, 1, 0x02, 0x41, 1, 0x00, 0x50, 1, 0x37, 0x60, 2, 0x02, 0x02, TRES, GSST, 0x82, 1, 0xad, 0xe7, 1, 0x1c,
    0xe3, 1, 0x22, 0xe0, 1, 0x00, 0xe9, 1, 0x01,
};
/* Register LUT (vendor "SSD2683 P420E55" order, stock V1.1.21+ and tesserae epd_init_vendor_lut): booster, CDI, PSR
 * with the LUT-from-register bit (0xA9), [power levels, VCOM, PLL from the timing bytes], resolution, 0x62, GSST. */
static const uint8_t LUT_HEAD[] = {0x06, 4, 0x0f, 0x8b, 0x9c, 0x96, 0x50, 1, 0x37, 0x00, 2, 0x07, 0xa9, BWQ_W_INIT, WT};
static const uint8_t LUT_TAIL[] = {
    TRES, BWQ_W_INIT, WT, 0x62, 2, 0x62, 0x51, GSST, BWQ_W_INIT, WT, 0xe7, 1, 0x96, 0xe9, 1, 0x01,
};
static const uint8_t TSE[] = {0x41, 1, 0x07, 0x40, 0, BWQ_W_INIT, WT}; /* sensor offset +7 C, TSC: measure again */
static const uint8_t E6[] = {0xe0, 1, 0x02, 0xe6, 1, 0x5c, 0xa5, 1, 0x00, BWQ_W_INIT, WT};
static const uint8_t E6P[] = {0xe0, 1, 0x02, 0xe6, 1, 0x5a, 0xa5, 1, 0x00, BWQ_W_INIT, WT};

static void init_lut(seq_t *s, const uint8_t *L, uint8_t pll)
{
    const uint8_t *t = L + 528;
    TABLE(s, LUT_HEAD);
    const uint8_t pwr[6] = {0x07, t[0], t[1], t[3], t[2], t[4]};
    put(s, BWQ_CMD, 0x01, pwr, sizeof pwr);
    const uint8_t vd = (uint8_t)(t[5] - 0x80);
    put(s, BWQ_CMD, 0x82, &vd, 1);
    put(s, BWQ_CMD, 0x30, pll ? &pll : &t[6], 1);
    TABLE(s, LUT_TAIL);
    put(s, BWQ_LUT, 0x20, NULL, 0);
}

static bool use_lut(const bwq_cfg_t *c) { return c->lut && c->init == BWRY_INIT_STOCK; }

bool bwq_early_pon(const bwq_cfg_t *c) { return !use_lut(c) && c->fast == BWRY_FAST_E6P; }

int bwq_init(const bwq_cfg_t *c, bwq_op_t *ops)
{
    seq_t s = {ops, 0};
    if (use_lut(c)) {
        init_lut(&s, c->lut, c->pll);
        return s.n;
    }
    if (c->init == BWRY_INIT_GD)
        TABLE_PLL(&s, GD, c->pll);
    else
        TABLE_PLL(&s, STOCK, c->pll);
    if (c->fast == BWRY_FAST_TSE) {
        TABLE(&s, TSE);
    } else if (c->fast == BWRY_FAST_E6) {
        TABLE(&s, E6);
    } else if (bwq_early_pon(c)) {
        put(&s, BWQ_PON, 0x04, NULL, 0);
        put(&s, BWQ_WAIT, BWQ_W_PON, NULL, 0);
        TABLE(&s, E6P);
    }
    return s.n;
}

int bwq_stream_head(const bwq_cfg_t *c, bwq_op_t *ops)
{
    static const uint8_t AREA[] = {0x83, 9, 0x00, 0x00, (BWRY_W - 1) >> 8, (BWRY_W - 1) & 0xff, 0x00, 0x00,
                                   (BWRY_H - 1) >> 8, (BWRY_H - 1) & 0xff, 0x00};
    seq_t s = {ops, 0};
    if (c->init == BWRY_INIT_STOCK)
        TABLE(&s, AREA);
    put(&s, BWQ_CMD, 0x10, NULL, 0);
    return s.n;
}

int bwq_refresh(const bwq_cfg_t *c, bool partial, bwq_op_t *ops)
{
    /* border: white (0x37) full, floating (0x97, GxEPD2) partial; DRF */
    static const uint8_t FULL[] = {0x50, 1, 0x37, 0x12, 1, 0x00}, PART[] = {0x50, 1, 0x97, 0x12, 1, 0x00};
    seq_t s = {ops, 0};
    if (!bwq_early_pon(c)) {
        put(&s, BWQ_PON, 0x04, NULL, 0);
        put(&s, BWQ_WAIT, BWQ_W_PON, NULL, 0);
    }
    if (partial)
        TABLE(&s, PART);
    else
        TABLE(&s, FULL);
    put(&s, BWQ_DELAY, 2, NULL, 0);
    put(&s, BWQ_WAIT, BWQ_W_REFRESH, NULL, 0);
    return s.n;
}

void bwq_lut_payload(const uint8_t *L, uint8_t out[BWQ_LUT_OUT])
{
    memcpy(out, L + 528, 7);
    for (int r = 0; r < 48; r++)
        for (int c = 0; c < 11; c++)
            out[7 + r * 11 + c] = L[r + 48 * c];
}

bool bwq_allowed(const bwq_gate_t *g, bwq_entry_t e)
{
    if (g->waiting)
        return false;
    return e == BWQ_E_OPEN ? !g->open : g->open;
}

bool bwq_fail_recover(const bwq_gate_t *g) { return g->open && !g->waiting; }
