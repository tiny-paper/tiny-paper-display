/*
 * tiny/types.h - types shared by every firmware component and mirrored by
 * the plugin SDK (sdk/c/tiny.h). Layouts follow docs/spec/host-abi.md.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_TYPES_H
#define TINY_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define TINY_API_MAJOR 1
#define TINY_API_MINOR 2
#define TINY_IMPORT_MODULE "tiny1"

/* Result codes shared by the host ABI and internal APIs (host-abi.md). */
enum {
    TINY_OK = 0,
    TINY_E_NOTFOUND = -1,
    TINY_E_INVAL = -2,
    TINY_E_DENIED = -3,
    TINY_E_LIMIT = -4,
    TINY_E_IO = -5,
    TINY_E_FORMAT = -6,
};

/* Task return values (task_<name> export). */
enum { TINY_TASK_OK = 0, TINY_TASK_RETRY = 1, TINY_TASK_FAIL = 2 };

/* Blit modes (assets.md). */
typedef enum {
    TINY_BLIT_COPY = 0,
    TINY_BLIT_OR = 1,
    TINY_BLIT_AND = 2,
    TINY_BLIT_INVERT = 3,
    TINY_BLIT_KEY = 4,
} tiny_blit_mode_t;

/* Vector drawing (API 1.1, host-abi.md "Vector drawing"): path ops and flags. */
enum { TINY_VG_MOVE = 0, TINY_VG_LINE = 1, TINY_VG_QUAD = 2, TINY_VG_CUBIC = 3, TINY_VG_CLOSE = 4 };
#define TINY_VG_EVENODD     0x01u   /* fill rule, else nonzero */
/* Colors of the drawing imports (host-abi.md "Colors"): 0..15 a gray level, 0x01RRGGBB an sRGB color, anything
 * else invalid. Gray panels take RGB by integer luminance. Returns the gray level 0..15, or -1 if invalid. */
static inline int tiny_color_gray(int32_t c)
{
    if (c >= 0 && c <= 15)
        return c;
    if (((uint32_t)c >> 24) != 1)
        return -1;
    uint32_t r = ((uint32_t)c >> 16) & 0xff, g = ((uint32_t)c >> 8) & 0xff, b = (uint32_t)c & 0xff;
    return (int)(((54 * r + 183 * g + 19 * b) >> 8) >> 4);
}
#define TINY_COLOR_RGB(r, g, b) ((int32_t)(0x01000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b)))
/* Inks (scene 1.8, host-abi.md "Colors"): a color panel's red and yellow; gray 5 and 10 on gray panels, black at 1 bit.
 * As levels (tiny_color_level) 17 and 18: gfx.h GFX_INK_RED, GFX_INK_YELLOW. */
#define TINY_INK_RED ((int32_t)0x02000001)
#define TINY_INK_YELLOW ((int32_t)0x02000002)
/* A color as the drawing takes it: a gray 0..15, with `inks` (scene 1.8) an ink as 16 + its palette code (red 17,
 * yellow 18); -1 if invalid. */
static inline int tiny_color_level(int32_t c, bool inks)
{
    if (inks && (c == TINY_INK_RED || c == TINY_INK_YELLOW))
        return 16 + (int)(c & 3);
    return tiny_color_gray(c);
}

#define TINY_VG_DITHER      0x02u   /* ordered 4x4 dither, else quantize */
/* API 1.2: the dither pattern moved right by x and down by y pixels (0..3 each; calls only, not asset items) */
#define TINY_VG_DITHER_PHASE(x, y) ((((uint32_t)(x) & 3u) << 6) | (((uint32_t)(y) & 3u) << 8))
#define TINY_VG_DITHER_PHASE_MASK 0x3C0u
#define TINY_VG_CAP_BUTT    0x00u
#define TINY_VG_CAP_ROUND   0x04u
#define TINY_VG_CAP_SQUARE  0x08u
#define TINY_VG_JOIN_MITER  0x00u   /* miter limit 4, then bevel */
#define TINY_VG_JOIN_ROUND  0x10u
#define TINY_VG_JOIN_BEVEL  0x20u
#define TINY_VG_MAX_SEGMENTS 1024   /* line segments per call after flattening/stroking */
#define TINY_VG_MAX_PATH 65535      /* path bytes per call */
#define TINY_MAX_LAYERS 4

/* Manifest memory_kb / task_memory_kb (manifest.md): below 64 a multiple of 4 (one short page of exactly
 * that size), from 64 a multiple of 64 (WebAssembly pages). */
#define TINY_MEMORY_KB_MIN 4
#define TINY_MEMORY_KB_STEP 4
static inline bool tiny_memory_kb_ok(uint32_t kb)
{
    return kb >= TINY_MEMORY_KB_MIN && (kb < 64 ? kb % TINY_MEMORY_KB_STEP == 0 : kb % 64 == 0);
}

/* Gray levels: 0 black .. 15 white. */
#define TINY_BLACK 0
#define TINY_WHITE 15

/* 24 bytes, little-endian, written by time_local / time_now. */
typedef struct {
    int64_t unix_s;      /* UTC seconds (not "unix": a GNU predefined macro) */
    int32_t utc_offset;  /* seconds, local = unix_s + utc_offset */
    uint16_t year;
    uint8_t month;       /* 1..12 */
    uint8_t day;         /* 1..31 */
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t wday;        /* 0 = Sunday */
    uint16_t yday;       /* 0..365 */
    uint8_t dst;         /* 0 or 1 */
    uint8_t reserved;
} tiny_time_t;

/* 16 bytes, written by screen(). Logical coordinates (after rotation). */
typedef struct {
    uint16_t w, h;
    uint8_t depth;          /* 1, 2 or 4 */
    uint8_t rotation;       /* 0..3 = 0, 90, 180, 270 clockwise */
    uint8_t display_index;  /* into manifest displays */
    uint8_t reserved;
    uint16_t clip_x, clip_y, clip_w, clip_h; /* current render band */
} tiny_screen_t;

/* Rectangle; panel-native or logical depending on the API that uses it. */
typedef struct {
    int16_t x, y;
    uint16_t w, h;
} tiny_rect_t;

_Static_assert(sizeof(tiny_time_t) == 24, "tiny_time_t layout");
_Static_assert(offsetof(tiny_time_t, year) == 12, "tiny_time_t layout");
_Static_assert(offsetof(tiny_time_t, yday) == 20, "tiny_time_t layout");
_Static_assert(sizeof(tiny_screen_t) == 16, "tiny_screen_t layout");
_Static_assert(offsetof(tiny_screen_t, clip_x) == 8, "tiny_screen_t layout");

#endif
