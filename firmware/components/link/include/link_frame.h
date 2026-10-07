/*
 * link_frame.h - the frame of the link protocol (docs/spec/link-protocol.md "Frame"): sizes, parser, encoder.
 * On its own so that a firmware that only frames (the display firmware, components/usbdisp) needs nothing of the
 * app's link (link.h includes this).
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_LINK_FRAME_H
#define TINY_LINK_FRAME_H

#include <stddef.h>
#include <stdint.h>

#define LINK_PROTO 1
#define LINK_MAX_PAYLOAD 6144
#define LINK_FRAME_OVERHEAD 8
#define LINK_FRAME_MAX (LINK_MAX_PAYLOAD + LINK_FRAME_OVERHEAD)
#define LINK_MSG_CAP 121          /* error msg, <= 120 bytes + NUL */

typedef struct {
    uint8_t seq, cmd;
    const uint8_t *payload;   /* valid until the next link_parser_feed */
    size_t len;
} link_frame_t;

typedef struct {
    uint8_t buf[LINK_FRAME_MAX];
    size_t len;
    size_t consume;           /* bytes of the frame returned last, dropped on the next feed */
    uint32_t bad_crc, dropped;
} link_parser_t;

void link_parser_init(link_parser_t *p);
/* Feed bytes; stops at the first complete frame. Returns 1 and fills *f when a frame is
 * ready (*used = bytes of `data` consumed; call again with the rest), else 0 (all consumed). */
int link_parser_feed(link_parser_t *p, const uint8_t *data, size_t len, size_t *used, link_frame_t *f);
/* Encode a frame. Returns the wire length, or 0 if it does not fit / payload too long. */
size_t link_frame_encode(uint8_t *out, size_t cap, uint8_t seq, uint8_t cmd, const uint8_t *payload, size_t len);

#endif
