/*
 * tiny/util.h - checksums and spec parsers shared by all components:
 * CRC-32 (tiny-format.md), CRC-16 (link-protocol.md), durations and the cron
 * dialect (manifest.md), name rules (manifest.md, host-abi.md).
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_UTIL_H
#define TINY_UTIL_H

#include "tiny/types.h"

/* Sizes computed from untrusted data (a package, the network): a * b and a + b, false with *out untouched when the
 * result does not fit size_t (32 bits on the devices). Use these before the result is compared with a buffer; bound
 * the count against the spec's limit first where there is one (issue #56). */
static inline bool tiny_size_mul(size_t a, size_t b, size_t *out)
{
    if (b && a > SIZE_MAX / b)
        return false;
    *out = a * b;
    return true;
}
static inline bool tiny_size_add(size_t a, size_t b, size_t *out)
{
    if (a > SIZE_MAX - b)
        return false;
    *out = a + b;
    return true;
}

/* CRC-32/ISO-HDLC (zlib). Streaming: crc = tiny_crc32(0, a, n); crc = tiny_crc32(crc, b, m). */
uint32_t tiny_crc32(uint32_t crc, const void *data, size_t len);

/* CRC-16/CCITT-FALSE. Streaming: start with 0xFFFF. */
uint16_t tiny_crc16(uint16_t crc, const void *data, size_t len);

/* Duration "15m", "6h", "1d" -> minutes. "0m" only when allow_zero. TINY_OK / TINY_E_INVAL. */
int tiny_duration_parse(const char *s, size_t len, bool allow_zero, uint32_t *minutes);

typedef struct {
    uint64_t minute;     /* bit n = minute n */
    uint32_t hour;       /* bits 0..23 */
    uint32_t dom;        /* bits 1..31 */
    uint16_t month;      /* bits 1..12 */
    uint8_t dow;         /* bits 0..6, Sunday = 0 (7 folded into 0) */
    bool dom_any, dow_any;
} tiny_cron_t;

/* Parse the 5-field dialect of manifest.md. TINY_OK / TINY_E_INVAL. */
int tiny_cron_parse(tiny_cron_t *c, const char *s, size_t len);
/* Does this local wall-clock minute match? (DST gap/repeat rules are the scheduler's.) */
bool tiny_cron_match(const tiny_cron_t *c, const tiny_time_t *local);
/* Minimum circular gap of the minute field in minutes (60 for a single value). */
uint32_t tiny_cron_min_gap(const tiny_cron_t *c);

/* Name rules. */
bool tiny_valid_plugin_id(const char *s, size_t len);   /* manifest id */
bool tiny_valid_host(const char *s, size_t len);        /* manifest hosts */
bool tiny_valid_task_name(const char *s, size_t len);
bool tiny_valid_setting_key(const char *s, size_t len);
bool tiny_valid_store_key(const char *s, size_t len);

#endif
