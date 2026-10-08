/* See epd_parwave.h. C11. SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_parwave.h"

#include <string.h>

bool epd_wave_valid(const epd_wave_t *w)
{
    return w && w->kick <= EPD_WAVE_MAX_FRAMES && w->main >= 1 && w->main <= EPD_WAVE_MAX_FRAMES &&
           w->extra <= EPD_WAVE_MAX_FRAMES && w->redrive <= EPD_WAVE_REDRIVE_MAX;
}

int epd_wave_plan(const epd_wave_t *w, bool any_wb, bool any_fresh, epd_wave_step_t out[3])
{
    int n = 0;
    if (!epd_wave_valid(w))
        return 0;
    if (w->kick && (any_wb || (w->redrive && any_fresh)))
        out[n++] = (epd_wave_step_t){EPD_WAVE_KICK, w->kick};
    out[n++] = (epd_wave_step_t){EPD_WAVE_MAIN, w->main};
    if (w->extra && any_wb)
        out[n++] = (epd_wave_step_t){EPD_WAVE_EXTRA, w->extra};
    return n;
}

int epd_wave_frames(const epd_wave_step_t *steps, int n)
{
    int f = 0;
    for (int i = 0; i < n; i++)
        f += steps[i].frames + 1;
    return f;
}

void epd_wave_scan(const uint8_t *P, const uint8_t *C, const uint8_t *fresh, size_t n, bool *any_wb, bool *any_fresh)
{
    epd_wave_scan_at(P, C, fresh, n, 0, n, any_wb, any_fresh);
}

void epd_wave_scan_at(const uint8_t *P, const uint8_t *C, const uint8_t *fresh, size_t n, size_t off, size_t len,
                      bool *any_wb, bool *any_fresh)
{
    uint8_t wb = 0, fr = 0;
    for (size_t i = off; i < off + len; i++) {
        wb |= (uint8_t)(P[i] & ~C[i]);
        if (fresh)
            fr |= (uint8_t)(fresh[i] | fresh[n + i]);
    }
    *any_wb = wb != 0;
    *any_fresh = fr != 0;
}

void epd_wave_masks(int phase, const uint8_t *P, const uint8_t *C, const uint8_t *fresh, bool redrive, uint8_t *prev_out,
                    uint8_t *cur_out, size_t n)
{
    epd_wave_masks_at(phase, P, C, fresh, redrive, prev_out, cur_out, n, 0, n);
}

void epd_wave_masks_at(int phase, const uint8_t *P, const uint8_t *C, const uint8_t *fresh, bool redrive,
                       uint8_t *prev_out, uint8_t *cur_out, size_t n, size_t off, size_t len)
{
    for (size_t i = off; i < off + len; i++) {
        uint8_t p = P[i], c = C[i];
        uint8_t wb = (uint8_t)(p & ~c), bw = (uint8_t)(~p & c);
        switch (phase) {
        case EPD_WAVE_KICK: {
            /* new blacks white (P'=0, C'=1); earlier partial blacks not turning white now: black (P'=1, C'=0).
             * Outside the caller's windows P = C = white, so "still black" comes from fresh, not C. */
            uint8_t fo = (redrive && fresh) ? (uint8_t)((fresh[i] | fresh[n + i]) & ~bw & ~wb) : 0;
            prev_out[i] = (uint8_t)~wb;
            cur_out[i] = (uint8_t)~fo;
            break;
        }
        case EPD_WAVE_EXTRA:
            prev_out[i] = 0xff;
            cur_out[i] = (uint8_t)~wb;
            break;
        default:
            prev_out[i] = p;
            cur_out[i] = c;
            break;
        }
    }
}

void epd_wave_drive(const uint8_t *P, const uint8_t *C, const uint8_t *prev_out, const uint8_t *cur_out, size_t n,
                    epd_wave_drive_t *out)
{
    out->driven = out->same = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t d = (uint8_t)(prev_out[i] ^ cur_out[i]);
        if (!d)
            continue;
        out->driven += (uint32_t)__builtin_popcount(d);
        out->same += (uint32_t)__builtin_popcount((uint8_t)(d & ~(P[i] ^ C[i])));
    }
}

bool epd_wave_track(uint8_t *fresh, const uint8_t *P, const uint8_t *C, size_t n, bool redriven, uint8_t budget)
{
    return epd_wave_track_at(fresh, P, C, n, 0, n, redriven, budget);
}

bool epd_wave_track_at(uint8_t *fresh, const uint8_t *P, const uint8_t *C, size_t n, size_t off, size_t len,
                       bool redriven, uint8_t budget)
{
    if (budget > EPD_WAVE_REDRIVE_MAX)
        budget = EPD_WAVE_REDRIVE_MAX;
    const uint8_t bl = budget & 1 ? 0xff : 0, bh = budget & 2 ? 0xff : 0;
    uint8_t ch = 0;
    for (size_t i = off; i < off + len; i++) {
        uint8_t l = fresh[i], h = fresh[n + i];
        if (redriven) { /* count - 1 where > 0: 01 -> 00, 10 -> 01, 11 -> 10 */
            uint8_t nl = (uint8_t)(~l & h), nh = (uint8_t)(h & l);
            l = nl;
            h = nh;
        }
        uint8_t wb = (uint8_t)(P[i] & ~C[i]), bw = (uint8_t)(~P[i] & C[i]);
        l = (uint8_t)(((l & ~wb) | (bl & wb)) & ~bw);
        h = (uint8_t)(((h & ~wb) | (bh & wb)) & ~bw);
        ch |= (uint8_t)((l ^ fresh[i]) | (h ^ fresh[n + i]));
        fresh[i] = l;
        fresh[n + i] = h;
    }
    return ch != 0;
}

void epd_wave_fresh_info(const uint8_t *fresh, size_t n, size_t row_bytes, epd_fresh_info_t *out)
{
    epd_wave_fresh_info_at(fresh, n, row_bytes, 0, n, out);
}

void epd_wave_fresh_info_at(const uint8_t *fresh, size_t n, size_t row_bytes, size_t off, size_t len,
                            epd_fresh_info_t *out)
{
    out->px = 0;
    out->y0 = out->y1 = -1;
    if (!fresh || !row_bytes)
        return;
    for (size_t i = off; i < off + len; i++) {
        uint8_t f = (uint8_t)(fresh[i] | fresh[n + i]);
        if (!f)
            continue;
        out->px += (uint32_t)__builtin_popcount(f);
        int y = (int)(i / row_bytes);
        if (out->y0 < 0)
            out->y0 = y;
        out->y1 = y;
    }
}

size_t epd_rle_bound(size_t n) { return n + n / 64 + 16; }

static size_t put_var(uint8_t *out, size_t o, size_t cap, size_t v)
{
    do {
        if (o >= cap)
            return 0;
        out[o++] = (uint8_t)((v & 0x7f) | (v > 0x7f ? 0x80 : 0));
        v >>= 7;
    } while (v);
    return o;
}

/* one token at o: returns the new offset, 0 if it does not fit */
static size_t put_tok(uint8_t *out, size_t o, size_t cap, bool lit, const uint8_t *src, size_t len)
{
    o = put_var(out, o, cap, len << 1 | (lit ? 1u : 0u));
    size_t body = lit ? len : 1;
    if (!o || o + body > cap)
        return 0;
    memcpy(out + o, src, body);
    return o + body;
}

size_t epd_rle_enc(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    size_t o = 0, i = 0, lit = 0; /* literal run starts at i - lit */
    while (i < n) {
        size_t r = 1;
        while (i + r < n && in[i + r] == in[i])
            r++;
        if (r < 4) { /* short runs stay literal */
            lit += r;
            i += r;
            continue;
        }
        if (lit && !(o = put_tok(out, o, cap, true, in + i - lit, lit)))
            return 0;
        lit = 0;
        if (!(o = put_tok(out, o, cap, false, in + i, r)))
            return 0;
        i += r;
    }
    if (lit && !(o = put_tok(out, o, cap, true, in + i - lit, lit)))
        return 0;
    return o;
}

int epd_rle_dec(const uint8_t *in, size_t len, uint8_t *out, size_t n)
{
    size_t i = 0, o = 0;
    while (i < len) {
        size_t v = 0;
        for (int sh = 0;; sh += 7) {
            if (i >= len || sh > 35)
                return -1;
            uint8_t b = in[i++];
            v |= (size_t)(b & 0x7f) << sh;
            if (!(b & 0x80))
                break;
        }
        size_t l = v >> 1;
        if (!l || l > n - o)
            return -1;
        if (v & 1) {
            if (l > len - i)
                return -1;
            memcpy(out + o, in + i, l);
            i += l;
        } else {
            if (i >= len)
                return -1;
            memset(out + o, in[i++], l);
        }
        o += l;
    }
    return o == n ? 0 : -1;
}

void epd_gpart_row(int kind, int j, const uint8_t *matrix, int passes, const uint8_t *old4, const uint8_t *new4, int w,
                   uint8_t *P, uint8_t *C, uint8_t *changed)
{
    for (int x = 0; x < w / 8; x++) {
        uint8_t p = 0xff, c = 0xff, ch = 0;
        for (int k = 0; k < 8; k++) {
            int px = x * 8 + k;
            int o = old4[px / 2] >> (px & 1 ? 0 : 4) & 15, n = new4[px / 2] >> (px & 1 ? 0 : 4) & 15;
            if (o == n)
                continue;
            uint8_t bit = (uint8_t)(0x80 >> k);
            ch |= bit;
            int code = kind == EPD_GP_DARK ? 1 : kind == EPD_GP_LIGHT ? 2 : matrix[n * passes + j];
            if (code == 1)
                c &= (uint8_t)~bit;
            else if (code == 2)
                p &= (uint8_t)~bit;
        }
        P[x] = p;
        C[x] = c;
        if (changed)
            changed[x] = ch;
    }
}

uint32_t epd_gpart_driven(const uint8_t *P, const uint8_t *C, int w)
{
    uint32_t n = 0;
    for (int x = 0; x < w / 8; x++)
        n += (uint32_t)__builtin_popcount((uint8_t)(P[x] ^ C[x]));
    return n;
}

int epd_gpart_groups(const uint8_t *matrix, int passes, uint8_t *first, uint8_t *len)
{
    int n = 0;
    for (int j = 0; j < passes; j++) {
        bool same = n > 0;
        for (int g = 0; same && g < 16; g++)
            same = matrix[g * passes + j] == matrix[g * passes + j - 1];
        if (same) {
            len[n - 1]++;
        } else {
            first[n] = (uint8_t)j;
            len[n++] = 1;
        }
    }
    return n;
}
