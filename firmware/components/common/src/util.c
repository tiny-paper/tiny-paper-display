/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tiny/util.h"
#include "tiny/hal.h"

#include <string.h>

/* CRC-32 (IEEE, reflected) a nibble at a time: 64 bytes of table, ~4x faster than bit by bit. It runs on every
 * wake over the plugin package and the band hashes, so it matters on the C3. */
static const uint32_t CRC32_NIB[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t tiny_crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ CRC32_NIB[crc & 15u];
        crc = (crc >> 4) ^ CRC32_NIB[crc & 15u];
    }
    return ~crc;
}

uint16_t tiny_crc16(uint16_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int k = 0; k < 8; k++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_lower(char c) { return c >= 'a' && c <= 'z'; }

int tiny_duration_parse(const char *s, size_t len, bool allow_zero, uint32_t *minutes)
{
    if (len == 2 && s[0] == '0' && s[1] == 'm') {
        if (!allow_zero)
            return TINY_E_INVAL;
        *minutes = 0;
        return TINY_OK;
    }
    if (len < 2 || len > 5 || s[0] < '1' || s[0] > '9')
        return TINY_E_INVAL;
    uint32_t n = 0;
    for (size_t i = 0; i + 1 < len; i++) {
        if (!is_digit(s[i]))
            return TINY_E_INVAL;
        n = n * 10 + (uint32_t)(s[i] - '0');
    }
    switch (s[len - 1]) {
    case 'm': *minutes = n; break;
    case 'h': *minutes = n * 60; break;
    case 'd': *minutes = n * 1440; break;
    default: return TINY_E_INVAL;
    }
    return TINY_OK;
}

/* ------------------------------------------------------------------ cron */

static bool parse_num(const char **p, const char *end, uint32_t *out)
{
    uint32_t n = 0;
    int digits = 0;
    while (*p < end && is_digit(**p) && digits < 3) {
        n = n * 10 + (uint32_t)(**p - '0');
        (*p)++;
        digits++;
    }
    if (digits == 0 || (*p < end && is_digit(**p)))
        return false;
    *out = n;
    return true;
}

/* One field: comma list of "*", "N", "A-B", "*" "/S", "A-B/S", "N/S". Sets bits in *bits. */
static bool parse_field(const char *s, const char *end, uint32_t lo, uint32_t hi, uint64_t *bits, bool *any)
{
    const char *p = s;
    *bits = 0;
    *any = (end - s == 1 && *s == '*');
    while (p < end) {
        uint32_t a, b, step = 1;
        bool star = false, range = false;
        if (*p == '*') {
            star = true;
            a = lo;
            b = hi;
            p++;
        } else {
            if (!parse_num(&p, end, &a))
                return false;
            b = a;
            if (p < end && *p == '-') {
                range = true;
                p++;
                if (!parse_num(&p, end, &b))
                    return false;
            }
        }
        if (p < end && *p == '/') {
            p++;
            if (!parse_num(&p, end, &step) || step == 0)
                return false;
            if (!star && !range)
                b = hi; /* N/S = N-max/S */
        }
        if (a < lo || b > hi || a > b)
            return false;
        for (uint32_t v = a; v <= b; v += step)
            *bits |= (uint64_t)1 << v;
        if (p < end) {
            if (*p != ',' || p + 1 == end)
                return false;
            p++;
        }
    }
    return p == end && s != end;
}

int tiny_cron_parse(tiny_cron_t *c, const char *s, size_t len)
{
    static const uint8_t lo[5] = {0, 0, 1, 1, 0}, hi[5] = {59, 23, 31, 12, 7};
    const char *p = s, *end = s + len;
    uint64_t bits[5];
    bool any[5];
    if (len == 0 || len > 64)
        return TINY_E_INVAL;
    for (int f = 0; f < 5; f++) {
        if (p >= end || *p == ' ')
            return TINY_E_INVAL;
        const char *fe = p;
        while (fe < end && *fe != ' ')
            fe++;
        if (!parse_field(p, fe, lo[f], hi[f], &bits[f], &any[f]))
            return TINY_E_INVAL;
        p = fe;
        if (f < 4) {
            if (p >= end)
                return TINY_E_INVAL;
            p++; /* exactly one space */
        }
    }
    if (p != end)
        return TINY_E_INVAL;
    c->minute = bits[0];
    c->hour = (uint32_t)bits[1];
    c->dom = (uint32_t)bits[2];
    c->month = (uint16_t)bits[3];
    c->dow = (uint8_t)((bits[4] | (bits[4] >> 7)) & 0x7f); /* 7 -> Sunday */
    c->dom_any = any[2];
    c->dow_any = any[4];
    return TINY_OK;
}

bool tiny_cron_match(const tiny_cron_t *c, const tiny_time_t *t)
{
    if (!(c->minute >> t->minute & 1) || !(c->hour >> t->hour & 1) || !(c->month >> t->month & 1))
        return false;
    bool dom = c->dom >> t->day & 1, dow = c->dow >> t->wday & 1;
    if (c->dom_any || c->dow_any)
        return dom && dow; /* the '*' side matches everything */
    return dom || dow;
}

uint32_t tiny_cron_min_gap(const tiny_cron_t *c)
{
    int first = -1, prev = -1;
    uint32_t gap = 60;
    for (int m = 0; m < 60; m++) {
        if (!(c->minute >> m & 1))
            continue;
        if (prev >= 0 && (uint32_t)(m - prev) < gap)
            gap = (uint32_t)(m - prev);
        if (first < 0)
            first = m;
        prev = m;
    }
    if (first >= 0 && prev != first && (uint32_t)(first + 60 - prev) < gap)
        gap = (uint32_t)(first + 60 - prev);
    return gap;
}

/* ------------------------------------------------------------------ names */

static bool label_ok(const char *s, size_t n)
{
    if (n < 1 || n > 63 || s[0] == '-' || s[n - 1] == '-')
        return false;
    for (size_t i = 0; i < n; i++)
        if (!is_lower(s[i]) && !is_digit(s[i]) && s[i] != '-')
            return false;
    return true;
}

/* Splits on '.', checks every label; returns the label count or 0 on failure. */
static int labels_ok(const char *s, size_t len, const char **last, size_t *last_len)
{
    int count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || s[i] == '.') {
            if (!label_ok(s + start, i - start))
                return 0;
            *last = s + start;
            *last_len = i - start;
            count++;
            start = i + 1;
        }
    }
    return count;
}

bool tiny_valid_plugin_id(const char *s, size_t len)
{
    const char *last;
    size_t ll;
    return len >= 3 && len <= 64 && labels_ok(s, len, &last, &ll) >= 2;
}

static bool ends_with(const char *s, size_t len, const char *suffix)
{
    size_t n = strlen(suffix);
    return len >= n && memcmp(s + len - n, suffix, n) == 0;
}

bool tiny_valid_host(const char *s, size_t len)
{
    static const char *const bad[] = {".local", ".localhost", ".internal", ".lan", ".home.arpa", ".arpa"};
    const char *last;
    size_t ll;
    if (len < 4 || len > 253 || labels_ok(s, len, &last, &ll) < 2)
        return false;
    for (size_t i = 0; i < ll; i++)
        if (!is_lower(last[i]))
            return false; /* last label alphabetic: also rules out IPv4 literals */
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        if (ends_with(s, len, bad[i]))
            return false;
    return true;
}

static bool ident_ok(const char *s, size_t len, size_t max)
{
    if (len < 1 || len > max || !is_lower(s[0]))
        return false;
    for (size_t i = 1; i < len; i++)
        if (!is_lower(s[i]) && !is_digit(s[i]) && s[i] != '_')
            return false;
    return true;
}

bool tiny_valid_task_name(const char *s, size_t len) { return ident_ok(s, len, 16); }
bool tiny_valid_setting_key(const char *s, size_t len) { return ident_ok(s, len, 32); }

bool tiny_valid_store_key(const char *s, size_t len)
{
    if (len < 1 || len > 32)
        return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!is_lower(c) && !is_digit(c) && !(c >= 'A' && c <= 'Z') && c != '_' && c != '.' && c != '-')
            return false;
    }
    return true;
}

__attribute__((weak)) void hal_led(bool on) { (void)on; }
__attribute__((weak)) bool hal_button_brush(void) { return false; }

/* Panels without a box partial (hal.h): the press overlay on them takes its own refresh rule */
__attribute__((weak)) bool hal_panel_box_partial(void) { return false; }

/* Boards that cannot choose the partial drive or count gate clocks (hal.h) */
__attribute__((weak)) bool hal_panel_partial_all(bool all)
{
    (void)all;
    return false;
}
__attribute__((weak)) void hal_panel_fresh_ram(bool ram_only) { (void)ram_only; }
__attribute__((weak)) bool hal_panel_gate_clocks(hal_gate_clocks_t *g)
{
    (void)g;
    return false;
}
