/*
 * tiny/gfx.h - framebuffers at depth 1/2/4, drawing, sprites, text, diff.
 * Pixel format, colors, RLE, fonts and blit modes: docs/spec/assets.md.
 * Pure, hardware-free, deterministic integer code: the same sources are built
 * for the device, the sim and (Emscripten) the browser preview, and must give
 * byte-identical buffers (plan 13.6). No allocation.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_GFX_H
#define TINY_GFX_H

#include "tiny/types.h"

/*
 * A framebuffer holds a band of panel-native rows [band_y, band_y + band_h) of a
 * panel_w x panel_h panel (the whole panel when band_y = 0, band_h = panel_h).
 * Drawing calls take logical coordinates (after rotation, as the plugin sees
 * the screen) and are clipped to the band and to `clip`.
 */
typedef struct {
    uint8_t *buf;
    uint32_t stride;            /* bytes per panel row: ceil(panel_w * depth / 8) */
    uint16_t panel_w, panel_h;  /* native */
    uint16_t band_y, band_h;    /* native rows held in buf */
    uint8_t depth;              /* 1, 2, 4 */
    uint8_t rotation;           /* 0..3, clockwise quarter turns */
    uint8_t palette;            /* GFX_PAL_*: 0 gray levels (gfx_fb_init), else palette codes (gfx_fb_set_palette) */
    tiny_rect_t clip;           /* logical clip, set by gfx_fb_init to the band */
    /* A wall (gfx_fb_set_view, firmware.md "Walls"): logical coordinates are those of a canvas_w x canvas_h canvas
     * and the panel shows its part at (view_x, view_y). canvas_w 0 (gfx_fb_init): none, the panel's logical frame. */
    uint16_t canvas_w, canvas_h;
    int16_t view_x, view_y;
    bool box; /* a mashup box (gfx_fb_set_box): the canvas is the box, view = -origin; gfx_clear fills the box only */
} gfx_fb_t;

/* Palette frames (docs/spec/assets.md "Palette frames"): on a 4-color panel (hal_board_t colors) a depth-2 frame may
 * hold palette codes instead of gray levels. Codes in luminance order, so a gray panel shows them as grays. */
enum { GFX_PAL_GRAY = 0, GFX_PAL_BWRY = 1 };
enum { GFX_BWRY_BLACK = 0, GFX_BWRY_RED = 1, GFX_BWRY_YELLOW = 2, GFX_BWRY_WHITE = 3 };
/* Inks: drawing calls take them where they take a gray (after tiny_color_level). On a BWRY frame the palette color;
 * elsewhere gray 5 (red) or 10 (yellow), and black at depth 1. */
#define GFX_INK_RED (16 + GFX_BWRY_RED)
#define GFX_INK_YELLOW (16 + GFX_BWRY_YELLOW)
#define GFX_IS_INK(g) ((g) == GFX_INK_RED || (g) == GFX_INK_YELLOW)

/* Bytes needed for `rows` native rows. */
size_t gfx_buf_size(uint16_t panel_w, uint16_t rows, uint8_t depth);

/* Returns TINY_OK, or TINY_E_INVAL (bad depth/rotation/band) / TINY_E_LIMIT (cap too small).
 * Does not clear the buffer. */
int gfx_fb_init(gfx_fb_t *fb, uint8_t *buf, size_t cap, uint16_t panel_w, uint16_t panel_h,
                uint8_t depth, uint8_t rotation, uint16_t band_y, uint16_t band_h);

/* A depth-2 framebuffer holds palette codes from now on: grays are stored dithered to black and white (the 4x4
 * ordered dither of assets.md "Palette frames", on panel coordinates), inks as their code. TINY_E_INVAL: depth. */
int gfx_fb_set_palette(gfx_fb_t *fb, uint8_t palette);

/* Clip (vg_clip_rect, API 1.1): fb->clip = (x, y, w, h) intersected with the band;
 * w or h <= 0 gives an empty clip (nothing is drawn). reset: clip = the band. */
void gfx_clip_set(gfx_fb_t *fb, int x, int y, int w, int h);
void gfx_clip_reset(gfx_fb_t *fb);

/* A wall view: the plugin draws on a canvas_w x canvas_h canvas, the panel (logical size after rotation) shows the
 * part at (view_x, view_y), which must lie inside the canvas (canvas <= GFX_CANVAS_MAX). canvas_w 0: no view.
 * Resets the clip. TINY_OK or TINY_E_INVAL. */
#define GFX_CANVAS_MAX 32767 /* view_x, view_y and tiny_rect_t x, y are int16_t */
int gfx_fb_set_view(gfx_fb_t *fb, uint16_t canvas_w, uint16_t canvas_h, uint16_t view_x, uint16_t view_y);

/* A mashup box (docs/spec/scene.md "Mashups"): logical coordinates are those of a w x h screen at the box's origin (x, y) of the
 * panel's logical frame, drawing and clip stay inside the box, dither phases stay in panel coordinates. The box must lie
 * inside the logical frame. gfx_fb_set_view or gfx_fb_set_box(fb, 0, 0, 0, 0) ends it. Resets the clip. */
int gfx_fb_set_box(gfx_fb_t *fb, uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/* Logical size (w/h swapped for rotation 1 and 3; the canvas with a view). */
void gfx_logical_size(const gfx_fb_t *fb, uint16_t *w, uint16_t *h);

/* The band as a logical rectangle (tiny_screen_t clip_*; canvas coordinates with a view). */
tiny_rect_t gfx_band_logical(const gfx_fb_t *fb);

/* ------------------------------------------------------------------ drawing (gray 0..15, clamped, or an ink) */

void gfx_clear(gfx_fb_t *fb, int gray);                 /* whole band (a box: the box), ignores clip */
void gfx_fill_rect(gfx_fb_t *fb, int x, int y, int w, int h, int gray);
int gfx_get_pixel(const gfx_fb_t *fb, int x, int y);    /* stored value at fb depth, -1 outside band */

/* Random-access byte source for sprite/font data that may live in flash. */
typedef struct {
    const uint8_t *mem;         /* if non-NULL, data is in memory at mem[0..len) */
    int (*read)(void *ctx, uint32_t off, void *buf, size_t len); /* else this; 0 = ok */
    void *ctx;
    uint32_t len;
} gfx_src_t;

typedef struct {
    uint16_t w, h;
    uint8_t depth;              /* 1, 2, 4 */
    uint8_t encoding;           /* 0 raw, 1 RLE */
    gfx_src_t src;              /* the blob (assets.md) */
    uint8_t palette;            /* GFX_PAL_*: codes (a BWRY photo raster) copied as they are into a frame of that
                                 * palette; into a gray frame they are grays (luminance order) */
} gfx_sprite_t;

/* Returns TINY_OK, TINY_E_INVAL (mode), TINY_E_FORMAT (RLE does not decode to
 * stride*h bytes, or a read failed). Converts depth per assets.md. */
int gfx_blit(gfx_fb_t *fb, const gfx_sprite_t *s, int x, int y, tiny_blit_mode_t mode);

typedef struct {
    gfx_src_t src;              /* font blob (assets.md "Font blobs") */
} gfx_font_t;

/* Validate a font blob header and glyph table. TINY_OK or TINY_E_FORMAT. */
int gfx_font_check(const gfx_font_t *font);

/* Draw UTF-8 text, line top at y. Returns the advance width (>= 0) or TINY_E_FORMAT. */
int gfx_text(gfx_fb_t *fb, const gfx_font_t *font, int x, int y, const char *str, size_t len, int gray);
int gfx_text_width(const gfx_font_t *font, const char *str, size_t len);

/* Built-in fonts for OS screens (setup, safe-mode clock, No WiFi):
 * 0 = small text (about 16 px, ASCII), 1 = large digits and ':' (about 120 px). NULL if absent. */
const gfx_font_t *gfx_builtin_font(int which);

/* ------------------------------------------------------------------ vector drawing (API 1.1) */
/* Integer-only scanline rasterizer: 4x4 samples per pixel, nonzero or even-odd, strokes
 * with caps and joins, ordered dither. Normative rules: docs/spec/host-abi.md. */

typedef struct {
    int32_t a, b, c, d, e, f;   /* 16.16: x' = a*x + c*y + e, y' = b*x + d*y + f (path units -> px) */
} gfx_vg_xform_t;

void gfx_vg_identity(gfx_vg_xform_t *t);

/* Caller-provided scratch (no allocation inside gfx). `fuel` is a work budget: every
 * call subtracts its work (segments x rows, pixels) and stops with TINY_E_LIMIT and
 * fuel <= 0 when it runs out. Use INT32_MAX for no limit. */
typedef struct {
    void *mem;                  /* 8-byte aligned */
    size_t size;                /* >= GFX_VG_WORK_SIZE(logical width) */
    int32_t fuel;
} gfx_vg_work_t;

#define GFX_VG_WORK_SIZE(max_w) ((size_t)TINY_VG_MAX_SEGMENTS * 28u + (size_t)(max_w) + 16u)

/* Path syntax check (host-abi.md path records). TINY_OK, TINY_E_INVAL, TINY_E_LIMIT (> 65535 bytes). */
int gfx_vg_path_check(const gfx_src_t *path);

/* Return TINY_OK, TINY_E_INVAL (path, flags), TINY_E_LIMIT (more than
 * TINY_VG_MAX_SEGMENTS segments: nothing drawn; or fuel ran out: w->fuel <= 0). */
int gfx_vg_fill(gfx_fb_t *fb, const gfx_vg_xform_t *t, const gfx_src_t *path, int gray,
                uint32_t flags, gfx_vg_work_t *w);
int gfx_vg_stroke(gfx_fb_t *fb, const gfx_vg_xform_t *t, const gfx_src_t *path, int gray,
                  int32_t width, uint32_t flags, gfx_vg_work_t *w);

/* Vector asset blob (assets.md kind 3). check: TINY_OK or TINY_E_FORMAT. */
int gfx_vg_asset_check(const gfx_src_t *blob, uint16_t w, uint16_t h);
int gfx_vg_draw_asset(gfx_fb_t *fb, const gfx_vg_xform_t *t, const gfx_src_t *blob, gfx_vg_work_t *w);
/* The same with every item in `gray` (0..15) and the dither bit from `flags` (host-abi.md vg_draw_asset_color,
 * after tiny_color_gray); gray < 0 keeps the stored grays and flags. */
int gfx_vg_draw_asset_color(gfx_fb_t *fb, const gfx_vg_xform_t *t, const gfx_src_t *blob, int gray, uint32_t flags,
                           gfx_vg_work_t *w);

/* Copy the band from a full-panel buffer with the same geometry (layers), within
 * fb->clip. `full` holds gfx_buf_size(panel_w, panel_h, depth) bytes. TINY_OK or TINY_E_FORMAT. */
int gfx_copy_band(gfx_fb_t *fb, const gfx_src_t *full);

/* ------------------------------------------------------------------ diff */

typedef struct {
    uint16_t align_x;           /* x and w multiple of this (8 for UC8179) */
    uint16_t merge_gap;         /* merge boxes closer than this many pixels */
    uint16_t max_rects;         /* above this, return the union as one rect */
    uint8_t full_pct;           /* rect area above this % of the band: return the whole band
                                   (one rect, the caller does a full refresh); 0 = off */
} gfx_diff_opts_t;

/* Compare two framebuffers with identical geometry and band. Writes up to `max`
 * panel-native rects (absolute panel rows, aligned per opts) covering every changed
 * pixel; returns the count (0 = identical). *dirty_px receives the number of changed
 * pixels (may be NULL). */
size_t gfx_diff(const gfx_fb_t *a, const gfx_fb_t *b, tiny_rect_t *out, size_t max,
                const gfx_diff_opts_t *opts, uint32_t *dirty_px);

/* ------------------------------------------------------------------ gray base, 1-bit partial over it (plan 4.4) */

/* Hybrid 2-bit refresh (link-protocol.md `gray_mode`): the pixels that differ between the 2-bit buffers `a` and `b`
 * (`len` bytes, 4 pixels per byte) and are gray (not black 0 or white 3) in either. bw_only: none. */
uint32_t gfx_2bit_gray_changes(const uint8_t *a, const uint8_t *b, size_t len);
bool gfx_2bit_bw_only(const uint8_t *a, const uint8_t *b, size_t len);

/* One row of `px` 2-bit pixels (px multiple of 8) to 1 bit, the nearest of black and white: 0, 1 -> 0; 2, 3 -> 1.
 * Used for both frames of a hybrid partial: a pixel on the same side in both is old = new, a transition the
 * partial LUTs (WW, KK) do not drive, so the gray of the base stays; a changed gray pixel shows as its nearest
 * black or white until the next gray full refresh. dst may equal src (in place). */
void gfx_2bit_row_to_1bit(const uint8_t *src, size_t px, uint8_t *dst);

/* The same at 4 bit (TRMNL X: 16-gray base, 1-bit partials over it): 2 pixels per byte, black 0, white 15,
 * 0..7 -> 0, 8..15 -> 1. */
uint32_t gfx_4bit_gray_changes(const uint8_t *a, const uint8_t *b, size_t len);
bool gfx_4bit_bw_only(const uint8_t *a, const uint8_t *b, size_t len);
void gfx_4bit_row_to_1bit(const uint8_t *src, size_t px, uint8_t *dst);

/* Both frames of a hybrid partial, one row of `px` pixels (multiple of 8) at `depth` 2 or 4, to 1 bit as above,
 * except an old gray pixel that changed but stays on the same side: its old bit is the other one, so the partial
 * drives it to the new pixel's nearest black or white. Without that it was never driven and kept the old frame's
 * gray (light icons, card lines, text edges of the previous layout: TRMNL X 2026-09-27). old1/new1 may equal
 * old/new (in place), and old may equal new. Returns the pixels given the other old bit. */
uint32_t gfx_pair_to_1bit(const uint8_t *old, const uint8_t *nw, size_t px, uint8_t depth, uint8_t *old1, uint8_t *new1);

#endif
