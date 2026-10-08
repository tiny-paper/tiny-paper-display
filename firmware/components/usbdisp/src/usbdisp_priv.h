/* usbdisp private: the PNG side, one panel update, the area and mode rules.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "tiny/cbor.h"
#include "usbdisp.h"

typedef struct {
    uint8_t depth;  /* of the frame: the gray PNG's bit depth; 2 on the color panel */
    bool indexed;   /* an indexed PNG of the panel's colors */
    uint16_t w, h;  /* the PNG's size */
} usbdisp_pnginfo_t;

/* The header (the first USBDISP_PNG_HEAD bytes or more) against the panel: 0, or USBDISP_E_NOT_PNG, _WRONG_SIZE,
 * _DEPTH with the reason in *why (a static string or a static text buffer: valid until the next call). area: any
 * size up to the panel's is taken (the caller checks the rectangle), else exactly the panel's. */
int usbdisp_png_check(const usbdisp_cfg_t *c, const uint8_t *png, size_t n, bool area, usbdisp_pnginfo_t *out,
                      const char **why);
/* Decode the whole PNG into `at` (pi->h rows, `stride` bytes from one row to the next, packed per assets.md: MSB
 * first, 0 = black; the color panel: palette codes); only the bytes of the PNG's own width are written in a row.
 * Nothing is scaled or dithered: a pixel is its level or it is refused.
 * 0, or USBDISP_E_BAD_PNG, _DEPTH (a color that is not the panel's), _NOMEM. */
int usbdisp_png_frame(const usbdisp_cfg_t *c, const uint8_t *png, size_t n, const usbdisp_pnginfo_t *pi, uint8_t *at,
                      size_t stride, const char **why);

/* usbdisp.c */
void usbdisp_send(usbdisp_t *d, const link_frame_t *f, const cbor_w_t *w);
/* { err, msg, detail?: { <dkey>: dval } }. A refused show request also forgets the frame kept. */
void usbdisp_refuse(usbdisp_t *d, const link_frame_t *f, int err, const char *msg, const char *dkey, uint32_t dval);
void usbdisp_log(usbdisp_t *d, const char *s);
/* Every byte to the USB link, in as many writes as it takes. */
void usbdisp_write_all(usbdisp_t *d, const uint8_t *buf, size_t len);
bool usbdisp_get_uint(const link_frame_t *f, const char *key, uint64_t *out);
/* Drop the transfer (the PNG); the frame kept stays. */
void usbdisp_xfer_drop(usbdisp_t *d);
/* Forget the frame kept: the next update is a full one that answers `why`. */
void usbdisp_prev_drop(usbdisp_t *d, const char *why);
void usbdisp_fingerprint(const usbdisp_t *d, uint8_t depth, const uint8_t *rows, char out[17]);
size_t usbdisp_stride(const usbdisp_t *d, uint8_t depth);
uint8_t usbdisp_pdepths(const usbdisp_t *d);
uint8_t usbdisp_fadepths(const usbdisp_t *d); /* the depths a rectangle's full refresh is on offer at now (cfg full_area_now) */ /* partial_depths as they are now (cfg partial_now) */

/* usbdisp_panel.c: one panel update. 0 = ok, else the step that failed in *why; the time of the panel work is added
 * to *ms. full: the rows of `frame` in bands, or with frame NULL `band` again for every band (clear). */
int usbdisp_panel_full(usbdisp_t *d, uint8_t depth, bool image, const uint8_t *frame, const uint8_t *band, size_t stride,
                       const tiny_rect_t *area, uint32_t *ms, const char **why);
/* The rectangle r: its old and new rows, each pointing at the rectangle's first byte, `stride` bytes from one row to
 * the next; all: every pixel is driven, else the changed ones. */
int usbdisp_panel_area(usbdisp_t *d, uint8_t depth, const tiny_rect_t *r, const uint8_t *old, const uint8_t *rows,
                       size_t stride, bool all, uint32_t *ms, const char **why);

/* usbdisp_bt.c: Bluetooth behind the setting */
void usbdisp_bt_init(usbdisp_t *d);
/* Start the stack when the setting is on and it does not run; close the pairing window when a host paired or its
 * time ran out. From usbdisp_poll. */
void usbdisp_bt_poll(usbdisp_t *d);
/* A request that came over Bluetooth may run: false, and it was refused (AUTH: not bonded; CABLE: bt). */
bool usbdisp_bt_allowed(usbdisp_t *d, const link_frame_t *f);
void usbdisp_bt_cmd(usbdisp_t *d, const link_frame_t *f);
/* usbdisp_bt_screen.c: a full refresh with up to three lines of capitals, digits and '-' (the pairing screens);
 * `big` is the large middle line. 0 = shown. The frame kept is forgotten. */
int usbdisp_bt_screen(usbdisp_t *d, const char *title, const char *big, const char *small);

/* usbdisp_area.c */
const char *usbdisp_mode_name(usbdisp_mode_t m);
/* The mode on this board (depth 0: not known yet, the board alone): 0, or USBDISP_E_MODE with *why. */
int usbdisp_mode_check(const usbdisp_t *d, usbdisp_mode_t m, bool area, uint8_t depth, const char **why);
/* show_begin: x, y, mode, passes into d. 0, or a refusal code with *why. */
int usbdisp_target(usbdisp_t *d, const link_frame_t *f, const char **why);
/* The PNG's header against the transfer's target (size, rectangle, mode at this depth). 0, or a refusal code. */
int usbdisp_head_check(const usbdisp_t *d, const uint8_t *png, size_t n, usbdisp_pnginfo_t *pi, const char **why);
void usbdisp_show_end(usbdisp_t *d, const link_frame_t *f);
void usbdisp_clear(usbdisp_t *d, const link_frame_t *f);
