/* usbdisp: where a picture goes and how the panel is driven for it (docs/spec/usb-display.md "Area and refresh
 * mode"): the rectangle and mode of show_begin, what each mode needs of the board and the depth, the choice between a
 * partial and a full refresh, and the frame kept as what the panel shows. SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>

#include "tiny/util.h"
#include "usbdisp_priv.h"

static const char *const MODES[USBDISP_MODE_COUNT] = {"auto", "full", "partial", "partial_all", "inverse", "passes", "none"};

const char *usbdisp_mode_name(usbdisp_mode_t m) { return MODES[m < USBDISP_MODE_COUNT ? m : 0]; }

int usbdisp_mode_check(const usbdisp_t *d, usbdisp_mode_t m, bool area, uint8_t depth, const char **why)
{
    static char msg[LINK_MSG_CAP]; /* one request at a time */
    const usbdisp_cfg_t *c = &d->c;
    bool every = m == USBDISP_MODE_PARTIAL_ALL || m == USBDISP_MODE_PASSES, one_bit = every || m == USBDISP_MODE_INVERSE;
    if (!area && (m == USBDISP_MODE_AUTO || m == USBDISP_MODE_FULL))
        return 0; /* a whole picture: every board shows it */
    const uint8_t pd = usbdisp_pdepths(d);
    /* a rectangle at a depth with a full refresh of a rectangle alone: shown without a mode, or with full */
    if (area && (m == USBDISP_MODE_AUTO || m == USBDISP_MODE_FULL) && depth && (usbdisp_fadepths(d) & depth))
        return 0;
    *why = msg;
    if (!pd) {
        *why = "this board has no partial refresh: send the whole picture, without a mode or with full";
        return USBDISP_E_MODE;
    }
    if (every && !c->partial_all) {
        snprintf(msg, sizeof msg, "this board cannot drive every pixel of an area: no %s", MODES[m]);
        return USBDISP_E_MODE;
    }
    if (one_bit && (!(pd & 1) || depth > 1)) {
        snprintf(msg, sizeof msg, "%s needs a 1 bit picture%s", MODES[m], pd & 1 ? "" : ": not on this board");
        return USBDISP_E_MODE;
    }
    if (depth && !(pd & depth)) {
        static const char *const BITS[8] = {"", "1", "2", "1, 2", "4", "1, 4", "2, 4", "1, 2, 4"};
        snprintf(msg, sizeof msg, "%s at %u bit: this board refreshes partially at %s bit only",
                 m == USBDISP_MODE_PARTIAL ? "partial" : "an area", (unsigned)depth, BITS[pd & 7]);
        return USBDISP_E_MODE;
    }
    return 0;
}

static bool has(const link_frame_t *f, const char *key)
{
    const uint8_t *v;
    size_t n;
    return cbor_map_get(f->payload, f->len, key, strlen(key), &v, &n);
}

int usbdisp_target(usbdisp_t *d, const link_frame_t *f, const char **why)
{
    static char msg[LINK_MSG_CAP];
    uint64_t x = 0, y = 0, passes = 0;
    const uint8_t *v;
    size_t n;
    d->area = false;
    d->mode = USBDISP_MODE_AUTO;
    d->passes = 0;
    d->x = d->y = 0;
    if (has(f, "x") != has(f, "y")) {
        *why = "x and y come together";
        return USBDISP_E_RECT;
    }
    if (has(f, "x")) {
        if (!usbdisp_get_uint(f, "x", &x) || !usbdisp_get_uint(f, "y", &y)) {
            *why = "x and y are numbers";
            return USBDISP_E_BAD_REQUEST;
        }
        if (x % USBDISP_ALIGN || x >= d->c.w || y >= d->c.h) {
            snprintf(msg, sizeof msg, "x %llu, y %llu: x is a multiple of %d, both inside the panel (%ux%u)",
                     (unsigned long long)x, (unsigned long long)y, USBDISP_ALIGN, (unsigned)d->c.w, (unsigned)d->c.h);
            *why = msg;
            return USBDISP_E_RECT;
        }
        d->area = true;
        d->x = (uint16_t)x;
        d->y = (uint16_t)y;
    }
    if (cbor_map_get(f->payload, f->len, "mode", 4, &v, &n)) {
        cbor_r_t r;
        cbor_item_t it;
        cbor_r_init(&r, v, n);
        usbdisp_mode_t m = USBDISP_MODE_FULL;
        bool ok = cbor_read(&r, &it) && it.type == CBOR_TSTR;
        while (ok && m < USBDISP_MODE_NONE && !cbor_str_eq(&it, MODES[m]))
            m++;
        if (!ok || m == USBDISP_MODE_NONE) {
            *why = "mode is one of full, partial, partial_all, inverse, passes";
            return USBDISP_E_MODE;
        }
        d->mode = m;
    }
    if (has(f, "passes") != (d->mode == USBDISP_MODE_PASSES) ||
        (d->mode == USBDISP_MODE_PASSES &&
         (!usbdisp_get_uint(f, "passes", &passes) || !passes || passes > USBDISP_PASSES_MAX))) {
        snprintf(msg, sizeof msg, "passes (1..%d) comes with the mode passes, and only with it", USBDISP_PASSES_MAX);
        *why = msg;
        return USBDISP_E_BAD_REQUEST;
    }
    d->passes = (uint8_t)passes;
    return usbdisp_mode_check(d, d->mode, d->area, 0, why);
}

int usbdisp_head_check(const usbdisp_t *d, const uint8_t *png, size_t n, usbdisp_pnginfo_t *pi, const char **why)
{
    static char msg[LINK_MSG_CAP];
    int err = usbdisp_png_check(&d->c, png, n, d->area, pi, why);
    if (err)
        return err;
    if (d->area && (!pi->w || !pi->h || pi->w % USBDISP_ALIGN || (uint32_t)d->x + pi->w > d->c.w ||
                    (uint32_t)d->y + pi->h > d->c.h)) {
        snprintf(msg, sizeof msg, "a PNG of %ux%u at %u,%u: the width is a multiple of %d, all of it inside the panel (%ux%u)",
                 (unsigned)pi->w, (unsigned)pi->h, (unsigned)d->x, (unsigned)d->y, USBDISP_ALIGN, (unsigned)d->c.w,
                 (unsigned)d->c.h);
        *why = msg;
        return USBDISP_E_RECT;
    }
    return usbdisp_mode_check(d, d->mode, d->area, pi->depth, why);
}

/* ------------------------------------------------------------------ answers */

/* The milliseconds of each device step of a show_end (the answer's `steps`; panel is the answer's ms). */
typedef struct {
    uint32_t recv, prep, inflate, convert, diff, fingerprint, clean, total;
} usbdisp_steps_t;

/* A clean of the area the partial updates touched (`clean` of the answer). */
typedef struct {
    tiny_rect_t r;
    usbdisp_mode_t mode;
    uint32_t ms;
} usbdisp_clean_t;

static uint32_t ms_between(uint64_t from, uint64_t to) { return to > from ? (uint32_t)((to - from) / 1000) : 0; }

/* pre_ms: what the update took before the panel was touched (the frame, the picture into it, its fingerprint, the
 * changed rectangle); in the log line only, next to ms, the panel's part. */
static void answer_shown(usbdisp_t *d, const link_frame_t *f, const char *what, uint8_t depth, uint32_t ms,
                         usbdisp_mode_t ran, const char *because, const tiny_rect_t *r, uint8_t passes, uint32_t pre_ms,
                         const usbdisp_steps_t *st, const usbdisp_clean_t *cl)
{
    /* a full refresh that came back sooner than a panel can refresh: the driver said ok but the glass was not driven */
    bool fast = ran == USBDISP_MODE_FULL && d->c.fast_ms && ms < d->c.fast_ms;
    usbdisp_gates_t g = {0};
    bool gates = ran != USBDISP_MODE_NONE && d->c.panel_gates && d->c.panel_gates(d->c.ctx, &g);
    char line[260], ck[48] = "", sl[96] = "";
    d->shown++;
    if (gates)
        snprintf(ck, sizeof ck, " ckv=%u/%u per=%u rows=%u%s", (unsigned)g.edges, (unsigned)g.frames, (unsigned)g.per,
                 (unsigned)g.rows, g.ok ? "" : " WRONG");
    if (st)
        snprintf(sl, sizeof sl, " steps=%u/%u/%u/%u/%u/%u/%u/%u", (unsigned)st->recv, (unsigned)st->prep, (unsigned)st->inflate,
                 (unsigned)st->convert, (unsigned)st->diff, (unsigned)st->fingerprint, (unsigned)ms, (unsigned)st->total);
    snprintf(line, sizeof line, "%s%s mode=%s why=%s area=%d,%d,%u,%u depth=%u ms=%u pre=%u n=%u%s%s%s",
             fast || (gates && !g.ok) ? "W " : "", what, MODES[ran], because, r->x, r->y, (unsigned)r->w, (unsigned)r->h,
             (unsigned)depth, (unsigned)ms, (unsigned)pre_ms, (unsigned)d->shown, ck, sl,
             fast ? " too fast for a refresh: look at the panel" : "");
    usbdisp_log(d, line);
    cbor_w_t w;
    cbor_w_init(&w, d->tx, sizeof d->tx);
    cbor_put_map(&w, 8u + (passes != 0) + gates + fast + (ran == USBDISP_MODE_NONE) + (st != NULL) + (cl != NULL));
    cbor_put_str(&w, "depth");
    cbor_put_uint(&w, depth);
    cbor_put_str(&w, "ms");
    cbor_put_uint(&w, ms);
    cbor_put_str(&w, "mode");
    cbor_put_str(&w, MODES[ran]);
    cbor_put_str(&w, "why");
    cbor_put_str(&w, because);
    cbor_put_str(&w, "x");
    cbor_put_uint(&w, (uint64_t)r->x);
    cbor_put_str(&w, "y");
    cbor_put_uint(&w, (uint64_t)r->y);
    cbor_put_str(&w, "w");
    cbor_put_uint(&w, r->w);
    cbor_put_str(&w, "h");
    cbor_put_uint(&w, r->h);
    if (passes) {
        cbor_put_str(&w, "passes");
        cbor_put_uint(&w, passes);
    }
    if (gates) {
        cbor_put_str(&w, "ckv");
        cbor_put_map(&w, 5);
        cbor_put_str(&w, "edges");
        cbor_put_uint(&w, g.edges);
        cbor_put_str(&w, "frames");
        cbor_put_uint(&w, g.frames);
        cbor_put_str(&w, "per");
        cbor_put_uint(&w, g.per);
        cbor_put_str(&w, "rows");
        cbor_put_uint(&w, g.rows);
        cbor_put_str(&w, "ok");
        cbor_put_bool(&w, g.ok);
    }
    if (fast) {
        cbor_put_str(&w, "warn");
        cbor_put_str(&w, "the refresh returned too fast: look at the panel");
    }
    if (ran == USBDISP_MODE_NONE) {
        cbor_put_str(&w, "same");
        cbor_put_bool(&w, true);
    }
    if (cl) {
        cbor_put_str(&w, "clean");
        cbor_put_map(&w, 6);
        cbor_put_str(&w, "x");
        cbor_put_uint(&w, (uint64_t)cl->r.x);
        cbor_put_str(&w, "y");
        cbor_put_uint(&w, (uint64_t)cl->r.y);
        cbor_put_str(&w, "w");
        cbor_put_uint(&w, cl->r.w);
        cbor_put_str(&w, "h");
        cbor_put_uint(&w, cl->r.h);
        cbor_put_str(&w, "mode");
        cbor_put_str(&w, MODES[cl->mode]);
        cbor_put_str(&w, "ms");
        cbor_put_uint(&w, cl->ms);
    }
    if (st) {
        cbor_put_str(&w, "steps");
        cbor_put_map(&w, 9);
        cbor_put_str(&w, "recv");
        cbor_put_uint(&w, st->recv);
        cbor_put_str(&w, "prep");
        cbor_put_uint(&w, st->prep);
        cbor_put_str(&w, "inflate");
        cbor_put_uint(&w, st->inflate);
        cbor_put_str(&w, "convert");
        cbor_put_uint(&w, st->convert);
        cbor_put_str(&w, "diff");
        cbor_put_uint(&w, st->diff);
        cbor_put_str(&w, "fingerprint");
        cbor_put_uint(&w, st->fingerprint);
        cbor_put_str(&w, "panel");
        cbor_put_uint(&w, ms);
        cbor_put_str(&w, "clean");
        cbor_put_uint(&w, st->clean);
        cbor_put_str(&w, "total");
        cbor_put_uint(&w, st->total);
    }
    usbdisp_send(d, f, &w);
}

/* ------------------------------------------------------------------ the update */

/* The rectangle that holds every byte that differs between two frames, x and w in whole groups of 8 pixels. false:
 * the frames are the same. */
static bool changed_rect(const usbdisp_t *d, const uint8_t *a, const uint8_t *b, size_t stride, uint8_t depth,
                         tiny_rect_t *out)
{
    size_t x0 = stride, x1 = 0;
    int y0 = -1, y1 = -1;
    for (uint16_t y = 0; y < d->c.h; y++) {
        const uint8_t *pa = a + (size_t)y * stride, *pb = b + (size_t)y * stride;
        if (!memcmp(pa, pb, stride))
            continue;
        size_t i = 0, j = stride - 1;
        while (pa[i] == pb[i])
            i++;
        while (pa[j] == pb[j])
            j--;
        x0 = i < x0 ? i : x0;
        x1 = j > x1 ? j : x1;
        y0 = y0 < 0 ? y : y0;
        y1 = y;
    }
    if (y0 < 0)
        return false;
    /* `depth` bytes of a row hold 8 pixels */
    uint32_t px0 = (uint32_t)(x0 / depth) * 8, px1 = (uint32_t)(x1 / depth + 1) * 8;
    if (px1 > d->c.w)
        px1 = d->c.w;
    *out = (tiny_rect_t){(int16_t)px0, (int16_t)y0, (uint16_t)(px1 - px0), (uint16_t)(y1 - y0 + 1)};
    return true;
}

static size_t rect_at(const tiny_rect_t *r, size_t stride, uint8_t depth)
{
    return (size_t)r->y * stride + (size_t)r->x * depth / 8; /* x is a multiple of 8: a whole byte */
}

/* The rectangle to the negative of the new picture, then to the picture: every pixel of it changes at least once.
 * 0, USBDISP_E_NOMEM (nothing driven) or USBDISP_E_IO. */
static int inverse(usbdisp_t *d, const tiny_rect_t *r, const uint8_t *next, size_t stride, uint32_t *ms, const char **why)
{
    size_t at = rect_at(r, stride, 1), row = r->w / 8u, bytes = (size_t)(r->h - 1) * stride + row;
    uint8_t *neg = d->c.alloc(d->c.ctx, bytes);
    if (!neg) {
        *why = "no memory for the negative";
        return USBDISP_E_NOMEM;
    }
    for (uint16_t j = 0; j < r->h; j++)
        for (size_t i = 0; i < row; i++)
            neg[(size_t)j * stride + i] = (uint8_t)~next[at + (size_t)j * stride + i];
    int rc = usbdisp_panel_area(d, 1, r, d->prev + at, neg, stride, false, ms, why);
    if (!rc)
        rc = usbdisp_panel_area(d, 1, r, neg, next + at, stride, false, ms, why);
    d->c.free(d->c.ctx, neg);
    return rc ? USBDISP_E_IO : 0;
}

static void rect_union(tiny_rect_t *a, const tiny_rect_t *b)
{
    if (!a->w || !a->h) {
        *a = *b;
        return;
    }
    int x1 = a->x + a->w > b->x + b->w ? a->x + a->w : b->x + b->w, y1 = a->y + a->h > b->y + b->h ? a->y + a->h : b->y + b->h;
    a->x = a->x < b->x ? a->x : b->x;
    a->y = a->y < b->y ? a->y : b->y;
    a->w = (uint16_t)(x1 - a->x);
    a->h = (uint16_t)(y1 - a->y);
}

/* The strongest area mode the depth takes: inverse drives every pixel of the area through the opposite value and back (both
 * directions, so black turns white fully), then passes, then partial_all. NONE: the depth offers none of them. */
static usbdisp_mode_t clean_mode(const usbdisp_t *d, uint8_t depth)
{
    static const usbdisp_mode_t ORDER[] = {USBDISP_MODE_INVERSE, USBDISP_MODE_PASSES, USBDISP_MODE_PARTIAL_ALL};
    const char *why;
    for (size_t i = 0; i < sizeof ORDER / sizeof *ORDER; i++)
        if (!usbdisp_mode_check(d, ORDER[i], false, depth, &why))
            return ORDER[i];
    return USBDISP_MODE_NONE;
}

/* After an update that left the frame kept in d->prev: the counters of the clean. A full refresh of the whole panel starts
 * them again; a partial update adds its rectangle, and `clean_every` of them clean the union once, redrawing the frame
 * there. When a full refresh is due as well, it does the work (the next update). 0, or USBDISP_E_IO (the panel may show
 * either). A clean that cannot run (no memory, no mode at this depth) is skipped and tried again by the next update. */
static int clean_after(usbdisp_t *d, uint8_t depth, size_t stride, bool whole_full, const tiny_rect_t *drv, usbdisp_clean_t *cl,
                       bool *done, const char **why)
{
    *done = false;
    if (whole_full) {
        d->since_clean = 0, d->clean_area = (tiny_rect_t){0, 0, 0, 0};
        return 0;
    }
    if (drv->w && drv->h) {
        rect_union(&d->clean_area, drv);
        if (d->since_clean < UINT8_MAX)
            d->since_clean++;
    }
    if (!d->clean_every || d->since_clean < d->clean_every || !d->clean_area.w || (d->full_every && d->since_full >= d->full_every))
        return 0;
    const usbdisp_mode_t m = clean_mode(d, depth);
    if (m == USBDISP_MODE_NONE) { /* left to full_every */
        d->since_clean = 0, d->clean_area = (tiny_rect_t){0, 0, 0, 0};
        return 0;
    }
    const tiny_rect_t u = d->clean_area;
    const size_t at = rect_at(&u, stride, depth);
    uint32_t ms = 0;
    int rc = 0;
    if (m == USBDISP_MODE_INVERSE) {
        rc = inverse(d, &u, d->prev, stride, &ms, why);
        if (rc == USBDISP_E_NOMEM)
            return 0;
    } else {
        for (uint8_t k = 0; !rc && k < (m == USBDISP_MODE_PASSES ? 2 : 1); k++)
            rc = usbdisp_panel_area(d, depth, &u, d->prev + at, d->prev + at, stride, true, &ms, why) ? USBDISP_E_IO : 0;
    }
    if (rc)
        return rc;
    d->since_clean = 0, d->clean_area = (tiny_rect_t){0, 0, 0, 0};
    *cl = (usbdisp_clean_t){u, m, ms};
    *done = true;
    return 0;
}

void usbdisp_show_end(usbdisp_t *d, const link_frame_t *f)
{
    static char msg[LINK_MSG_CAP];
    const uint64_t t_pre = d->c.now_us(d->c.ctx);
    usbdisp_steps_t st = {0};
    usbdisp_png_time_t pt = {0};
    uint64_t t_png = t_pre;
    st.recv = ms_between(d->t_first, d->t_last);
    usbdisp_pnginfo_t pi = {0};
    const char *why = "";
    const usbdisp_mode_t mode = d->mode;
    const bool area = d->area;
    const uint8_t passes = d->passes;
    int err = usbdisp_head_check(d, d->png, d->size, &pi, &why);
    const uint8_t depth = pi.depth;
    /* the frame is kept, and compared with, only at a depth the panel refreshes partially */
    const uint8_t fa = usbdisp_fadepths(d);
    const bool part = usbdisp_pdepths(d) & depth; /* a partial refresh at this depth */
    const bool keep = !err && !d->c.bwry && ((usbdisp_pdepths(d) | fa) & depth);
    bool have = keep && d->prev && d->prev_depth == depth;
    tiny_rect_t whole = {0, 0, d->c.w, d->c.h}, r = whole;
    if (area)
        r = (tiny_rect_t){(int16_t)d->x, (int16_t)d->y, pi.w, pi.h};
    if (!err && area && d->prev && !have) { /* the rest of the panel cannot be told at another depth */
        snprintf(msg, sizeof msg, "the picture shown has %u bit and the rectangle %u bit: an area must have the same depth",
                 (unsigned)d->prev_depth, (unsigned)depth);
        err = USBDISP_E_DEPTH, why = msg;
    }
    size_t stride = err ? 0 : usbdisp_stride(d, depth), bytes = 0;
    uint8_t *next = NULL;
    if (!err && !tiny_size_mul(stride, d->c.h, &bytes))
        err = USBDISP_E_NOMEM, why = "the frame is too large";
    if (!err && !(next = d->c.alloc(d->c.ctx, bytes))) {
        if (d->prev && !area && mode <= USBDISP_MODE_FULL) { /* a whole picture can do without the frame kept */
            usbdisp_prev_drop(d, "memory");
            have = false;
            next = d->c.alloc(d->c.ctx, bytes);
        }
        if (!next)
            err = USBDISP_E_NOMEM, why = "no memory for the frame";
    }
    if (!err && area) { /* the rest of the panel: as shown, or white when that is not known */
        if (have)
            memcpy(next, d->prev, bytes);
        else
            memset(next, 0xFF, bytes);
    }
    if (!err) {
        t_png = d->c.now_us(d->c.ctx);
        err = usbdisp_png_frame(&d->c, d->png, d->size, &pi, next + rect_at(&r, stride, depth), stride, &why, &pt);
    }
    usbdisp_xfer_drop(d); /* the PNG is not needed during the refresh */

    char fingerprint[17] = "";
    const uint64_t t_fp = d->c.now_us(d->c.ctx);
    if (!err)
        usbdisp_fingerprint(d, depth, next, fingerprint);
    st.fingerprint = ms_between(t_fp, d->c.now_us(d->c.ctx));
    bool same = !err && d->fingerprint[0] && !strcmp(fingerprint, d->fingerprint);
    usbdisp_mode_t run = mode;
    const char *because = "asked";
    if (same && (mode == USBDISP_MODE_AUTO || mode == USBDISP_MODE_PARTIAL)) {
        run = USBDISP_MODE_NONE, because = "unchanged";
    } else if (err || mode == USBDISP_MODE_FULL) {
        /* as asked */
    } else if (!keep) {
        run = USBDISP_MODE_FULL, because = usbdisp_pdepths(d) ? "depth" : "board";
    } else if (!have) {
        run = USBDISP_MODE_FULL, because = d->prev ? "depth" : d->no_prev;
    } else if (mode == USBDISP_MODE_AUTO) {
        if (d->full_every && d->since_full >= d->full_every)
            run = USBDISP_MODE_FULL, because = "cleanup";
        else
            run = USBDISP_MODE_PARTIAL, because = "changed";
    }
    /* a partial update: the changed rectangle is found before the panel is touched, and a gray one that is too large for the
     * copies of its pixels (usbdisp.h USBDISP_GRAY_PARTIAL_MAX_PX) is a full refresh, or with `partial` named a refusal */
    tiny_rect_t chg = {0, 0, 0, 0};
    bool chg_found = false;
    /* a full refresh of one rectangle alone (cfg panel_area), where the rest of the panel is known: the changed
     * rectangle at a depth without a partial refresh, or the rectangle sent with `full` */
    bool rect_full = false;
    tiny_rect_t fr = whole;
    if (!err && run == USBDISP_MODE_PARTIAL) {
        const uint64_t t_diff = d->c.now_us(d->c.ctx);
        chg_found = changed_rect(d, d->prev, next, stride, depth, &chg);
        st.diff = ms_between(t_diff, d->c.now_us(d->c.ctx));
        if (!chg_found) {
            run = USBDISP_MODE_NONE, because = "unchanged";
        } else if (!part) {
            run = USBDISP_MODE_FULL, because = "area", rect_full = true, fr = chg;
        } else if (depth > 1 && d->c.gray_partial_max_px && (uint32_t)chg.w * chg.h > d->c.gray_partial_max_px) {
            if (mode == USBDISP_MODE_PARTIAL) {
                snprintf(msg, sizeof msg, "a partial update at %u bit takes at most %lu changed pixels in its rectangle, this one has %lu: use full",
                         (unsigned)depth, (unsigned long)d->c.gray_partial_max_px, (unsigned long)((uint32_t)chg.w * chg.h));
                err = USBDISP_E_MODE, why = msg;
            } else {
                run = USBDISP_MODE_FULL, because = "large";
            }
        }
    }
    else if (!err && run == USBDISP_MODE_FULL && mode == USBDISP_MODE_FULL && area && have && (fa & depth)) {
        because = "area", rect_full = true, fr = r;
    }
    if (rect_full && fr.h < 2) { /* a panel takes no rectangle one row high: the row below it too, or the one above */
        if (fr.y + 2 > d->c.h)
            fr.y--;
        fr.h = 2;
    }
    uint32_t ms = 0;
    const uint32_t pre_ms = (uint32_t)((d->c.now_us(d->c.ctx) - t_pre) / 1000);
    tiny_rect_t drv = whole;
    if (!err && run == USBDISP_MODE_NONE) {
        drv = (tiny_rect_t){0, 0, 0, 0};
    } else if (!err && run == USBDISP_MODE_FULL) {
        if (rect_full)
            drv = fr;
        if (usbdisp_panel_full(d, depth, true, next, NULL, stride, rect_full ? &fr : NULL, &ms, &why))
            err = USBDISP_E_IO;
    } else if (!err && run == USBDISP_MODE_PARTIAL) {
        drv = chg;
        if (usbdisp_panel_area(d, depth, &drv, d->prev + rect_at(&drv, stride, depth), next + rect_at(&drv, stride, depth),
                               stride, false, &ms, &why))
            err = USBDISP_E_IO;
    } else if (!err && run == USBDISP_MODE_INVERSE) {
        drv = r;
        err = inverse(d, &r, next, stride, &ms, &why);
    } else if (!err) { /* partial_all, passes: every pixel, once or `passes` times */
        drv = r;
        size_t at = rect_at(&r, stride, depth);
        for (uint8_t k = 0; !err && k < (run == USBDISP_MODE_PASSES ? passes : 1); k++)
            if (usbdisp_panel_area(d, depth, &r, (k ? next : d->prev) + at, next + at, stride, true, &ms, &why))
                err = USBDISP_E_IO;
    }
    /* the request's payload is gone by now (requests were fed during the refresh): seq and cmd were copied by value */
    usbdisp_clean_t cl = {{0, 0, 0, 0}, USBDISP_MODE_NONE, 0};
    bool cleaned = false;
    if (err) {
        if (next)
            d->c.free(d->c.ctx, next);
        usbdisp_refuse(d, f, err, why, NULL, 0); /* and the frame kept is forgotten: the panel may show either */
        return;
    }
    if (run == USBDISP_MODE_NONE) {
        d->c.free(d->c.ctx, next);
    } else {
        d->since_full = run == USBDISP_MODE_FULL && !rect_full ? 0 : (uint8_t)(d->since_full < UINT8_MAX ? d->since_full + 1 : UINT8_MAX);
        if (keep) {
            if (d->prev)
                d->c.free(d->c.ctx, d->prev);
            d->prev = next;
            d->prev_depth = depth;
            d->no_prev = NULL;
        } else {
            d->c.free(d->c.ctx, next);
            usbdisp_prev_drop(d, "depth");
        }
    }
    memcpy(d->fingerprint, fingerprint, sizeof fingerprint);
    if (run != USBDISP_MODE_NONE && d->prev && d->prev_depth == depth) {
        const uint64_t t_clean = d->c.now_us(d->c.ctx);
        int rc = clean_after(d, depth, stride, run == USBDISP_MODE_FULL && !rect_full, &drv, &cl, &cleaned, &why);
        st.clean = ms_between(t_clean, d->c.now_us(d->c.ctx));
        if (rc) { /* the panel may show either picture */
            usbdisp_refuse(d, f, rc, why, NULL, 0);
            return;
        }
    }
    st.inflate = (uint32_t)(pt.inflate_us / 1000);
    st.convert = (uint32_t)(pt.convert_us / 1000);
    st.prep = ms_between(t_pre, t_png); /* head check, the frame's allocation, the copy of the frame kept */
    st.total = ms_between(d->t_first, d->c.now_us(d->c.ctx));
    answer_shown(d, f, "show", depth, ms, run, because, &drv, run == USBDISP_MODE_PASSES ? passes : 0, pre_ms, &st,
                 cleaned ? &cl : NULL);
}

void usbdisp_clear(usbdisp_t *d, const link_frame_t *f)
{
    usbdisp_xfer_drop(d);
    usbdisp_prev_drop(d, "failed"); /* until the panel is white */
    uint8_t depth = d->c.bwry ? 2 : 1; /* white: every bit set at depth 1, palette code 3 on the color panel */
    uint16_t rows = d->c.band_rows && d->c.band_rows < d->c.h ? d->c.band_rows : d->c.h;
    size_t stride = usbdisp_stride(d, depth), bytes = 0;
    uint8_t *band = tiny_size_mul(stride, rows, &bytes) ? d->c.alloc(d->c.ctx, bytes) : NULL;
    if (!band) {
        usbdisp_refuse(d, f, USBDISP_E_NOMEM, "no memory for the frame", NULL, 0);
        return;
    }
    memset(band, 0xFF, bytes);
    uint32_t ms = 0;
    const char *why = "";
    int rc = usbdisp_panel_full(d, depth, false, NULL, band, stride, NULL, &ms, &why);
    d->c.free(d->c.ctx, band);
    if (rc) {
        usbdisp_refuse(d, f, USBDISP_E_IO, why, NULL, 0);
        return;
    }
    d->since_full = 0;
    d->since_clean = 0, d->clean_area = (tiny_rect_t){0, 0, 0, 0};
    /* a white frame is what the panel shows now: the next 1 bit picture needs no full refresh */
    if (!d->c.bwry && (usbdisp_pdepths(d) & 1)) {
        if (tiny_size_mul(stride, d->c.h, &bytes) && (d->prev = d->c.alloc(d->c.ctx, bytes))) {
            memset(d->prev, 0xFF, bytes);
            d->prev_depth = 1;
            d->no_prev = NULL;
        } else {
            d->no_prev = "memory";
        }
    } else {
        d->no_prev = "depth";
    }
    tiny_rect_t whole = {0, 0, d->c.w, d->c.h};
    usbdisp_fingerprint(d, depth, NULL, d->fingerprint);
    answer_shown(d, f, "clear", depth, ms, USBDISP_MODE_FULL, "asked", &whole, 0, 0, NULL, NULL);
}
