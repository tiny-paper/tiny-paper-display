/* usbdisp private: the PNG side. SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "usbdisp.h"

typedef struct {
    uint8_t depth;  /* of the frame: the gray PNG's bit depth; 2 on the color panel */
    bool indexed;   /* an indexed PNG of the panel's colors */
} usbdisp_pnginfo_t;

/* The header (the first USBDISP_PNG_HEAD bytes or more) against the panel: 0, or USBDISP_E_NOT_PNG, _WRONG_SIZE,
 * _DEPTH with the reason in *why (a static string or c's own text buffer: valid until the next call). */
int usbdisp_png_check(const usbdisp_cfg_t *c, const uint8_t *png, size_t n, usbdisp_pnginfo_t *out, const char **why);
/* Decode the whole PNG into `frame` (c->h rows of `stride` bytes, packed per assets.md: MSB first, 0 = black; the
 * color panel: palette codes). Nothing is scaled or dithered: a pixel is its level or it is refused.
 * 0, or USBDISP_E_BAD_PNG, _DEPTH (a color that is not the panel's), _NOMEM. */
int usbdisp_png_frame(const usbdisp_cfg_t *c, const uint8_t *png, size_t n, const usbdisp_pnginfo_t *pi, uint8_t *frame,
                      size_t stride, const char **why);
