/* usbdisp: the pairing screens (docs/spec/usb-display.md "Bluetooth", "The panel"): three centered lines of capitals,
 * digits and '-' in a 5 x 7 dot font drawn in whole-pixel multiples, as one full refresh. The display firmware has
 * no font files; this is all the text it ever draws.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "tiny/util.h"
#include "usbdisp_priv.h"

#define GW 5 /* glyph columns; one more of space */
#define GH 7

/* Columns left to right, bit 0 the top row: '0'..'9', 'A'..'Z', '-' */
static const uint8_t FONT[37][GW] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00}, {0x42, 0x61, 0x51, 0x49, 0x46},
    {0x21, 0x41, 0x45, 0x4B, 0x31}, {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03}, {0x36, 0x49, 0x49, 0x49, 0x36},
    {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36},
    {0x3E, 0x41, 0x41, 0x41, 0x22}, {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41},
    {0x7F, 0x09, 0x09, 0x09, 0x01}, {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F},
    {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41},
    {0x7F, 0x40, 0x40, 0x40, 0x40}, {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F},
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E},
    {0x7F, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01},
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},
    {0x08, 0x08, 0x08, 0x08, 0x08},
};

/* The glyph of a character (small letters as capitals); NULL: a space. */
static const uint8_t *glyph(char ch)
{
    if (ch >= 'a' && ch <= 'z')
        ch = (char)(ch - 'a' + 'A');
    if (ch >= '0' && ch <= '9')
        return FONT[ch - '0'];
    if (ch >= 'A' && ch <= 'Z')
        return FONT[10 + ch - 'A'];
    return ch == '-' ? FONT[36] : NULL;
}

typedef struct {
    uint8_t *px;
    size_t stride;
    uint16_t w, h;
    uint8_t depth;
} canvas_t;

/* Black: every bit of the pixel cleared (0 is black at depth 1 and the palette code of black at 2). */
static void block(const canvas_t *c, long x, long y, long s)
{
    for (long j = y < 0 ? 0 : y; j < y + s && j < c->h; j++)
        for (long i = x < 0 ? 0 : x; i < x + s && i < c->w; i++)
            for (unsigned b = 0; b < c->depth; b++) {
                size_t bit = (size_t)i * c->depth + b;
                c->px[(size_t)j * c->stride + bit / 8] &= (uint8_t)~(0x80u >> (bit % 8));
            }
}

/* One centered line with dots of `s` pixels, shrunk until it fits the width. Returns its height. */
static long line(const canvas_t *c, const char *t, long y, long s)
{
    long n = (long)strlen(t);
    if (!n)
        return 0;
    while (s > 1 && (n * (GW + 1) - 1) * s > c->w)
        s--;
    long x = ((long)c->w - (n * (GW + 1) - 1) * s) / 2;
    for (long k = 0; k < n; k++, x += (GW + 1) * s) {
        const uint8_t *g = glyph(t[k]);
        for (int col = 0; g && col < GW; col++)
            for (int row = 0; row < GH; row++)
                if (g[col] >> row & 1)
                    block(c, x + col * s, y + row * s, s);
    }
    return GH * s;
}

int usbdisp_bt_screen(usbdisp_t *d, const char *title, const char *big, const char *small)
{
    uint8_t depth = d->c.bwry ? 2 : 1;
    size_t stride = usbdisp_stride(d, depth), bytes = 0;
    uint8_t *px = tiny_size_mul(stride, d->c.h, &bytes) ? d->c.alloc(d->c.ctx, bytes) : NULL;
    if (!px && d->prev) { /* rather the frame kept than no screen */
        usbdisp_prev_drop(d, "memory");
        px = bytes ? d->c.alloc(d->c.ctx, bytes) : NULL;
    }
    if (!px)
        return USBDISP_E_NOMEM;
    memset(px, 0xFF, bytes);
    canvas_t c = {px, stride, d->c.w, d->c.h, depth};
    /* dot sizes from the width: the title fills about two thirds, the big line about half */
    long st = d->c.w / 150 ? d->c.w / 150 : 1, sb = d->c.w / 80 ? d->c.w / 80 : 1, ss = st > 1 ? st / 2 + st % 2 : 1;
    long gap = 3 * st, total = GH * (st + sb + ss) + 2 * gap;
    long y = ((long)d->c.h - total) / 2;
    if (y < 0)
        y = 0;
    y += line(&c, title, y, st) + gap;
    y += line(&c, big, y, sb) + gap;
    line(&c, small ? small : "", y, ss);
    uint32_t ms = 0;
    const char *why = "";
    usbdisp_prev_drop(d, "first"); /* the picture is gone: the next show is a full refresh */
    int rc = usbdisp_panel_full(d, depth, true, px, NULL, stride, NULL, &ms, &why);
    d->c.free(d->c.ctx, px);
    d->since_full = 0;
    if (rc)
        usbdisp_log(d, why);
    return rc ? USBDISP_E_IO : 0;
}
