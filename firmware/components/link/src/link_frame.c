/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "link_frame.h"
#include "tiny/util.h"

void link_parser_init(link_parser_t *p)
{
    memset(p, 0, sizeof *p);
}

static void drop(link_parser_t *p, size_t n)
{
    if (n > p->len)
        n = p->len;
    memmove(p->buf, p->buf + n, p->len - n);
    p->len -= n;
}

/* Look for a complete frame at the start of buf, resyncing as needed. */
static int scan(link_parser_t *p, link_frame_t *f)
{
    for (;;) {
        if (p->len == 0)
            return 0;
        if (p->buf[0] != 0xA5) {
            /* console or garbage bytes: skip up to the next candidate sync */
            size_t i = 1;
            while (i < p->len && p->buf[i] != 0xA5)
                i++;
            p->dropped += (uint32_t)i;
            drop(p, i);
            continue;
        }
        if (p->len < 2)
            return 0;
        if (p->buf[1] != 0x5A) {
            p->dropped++;
            drop(p, 1);
            continue;
        }
        if (p->len < 4)
            return 0;
        size_t n = (size_t)p->buf[2] | (size_t)p->buf[3] << 8;
        if (n > LINK_MAX_PAYLOAD) {
            p->dropped++;
            drop(p, 1);
            continue;
        }
        if (p->len < n + LINK_FRAME_OVERHEAD)
            return 0;
        uint16_t crc = tiny_crc16(0xFFFF, p->buf + 2, n + 4);
        uint16_t got = (uint16_t)(p->buf[6 + n] | p->buf[7 + n] << 8);
        if (crc != got) {
            p->bad_crc++;
            p->dropped++;
            drop(p, 1);
            continue;
        }
        f->seq = p->buf[4];
        f->cmd = p->buf[5];
        f->payload = p->buf + 6;
        f->len = n;
        p->consume = n + LINK_FRAME_OVERHEAD;
        return 1;
    }
}

int link_parser_feed(link_parser_t *p, const uint8_t *data, size_t len, size_t *used, link_frame_t *f)
{
    if (p->consume) {
        drop(p, p->consume);
        p->consume = 0;
    }
    /* a frame may already be complete in the buffer (several frames in one read) */
    if (scan(p, f)) {
        *used = 0;
        return 1;
    }
    for (size_t i = 0; i < len; i++) {
        /* scan() keeps the buffer at most one frame long, so there is always room */
        p->buf[p->len++] = data[i];
        if (scan(p, f)) {
            *used = i + 1;
            return 1;
        }
    }
    *used = len;
    return 0;
}

size_t link_frame_encode(uint8_t *out, size_t cap, uint8_t seq, uint8_t cmd, const uint8_t *payload, size_t len)
{
    if (len > LINK_MAX_PAYLOAD || cap < len + LINK_FRAME_OVERHEAD)
        return 0;
    out[0] = 0xA5;
    out[1] = 0x5A;
    out[2] = (uint8_t)len;
    out[3] = (uint8_t)(len >> 8);
    out[4] = seq;
    out[5] = cmd;
    if (len)
        memmove(out + 6, payload, len);
    uint16_t crc = tiny_crc16(0xFFFF, out + 2, len + 4);
    out[6 + len] = (uint8_t)crc;
    out[7 + len] = (uint8_t)(crc >> 8);
    return len + LINK_FRAME_OVERHEAD;
}
