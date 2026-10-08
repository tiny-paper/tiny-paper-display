/* Inert HAL for the host tests of the display repository (no simulator here): the symbols the core calls outside the
 * device binding, so the shared core links. Panel calls do nothing and succeed; storage is a small in-memory store; the link is silent.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stddef.h>
#include <stdint.h>
#include "tiny/hal.h"

static const hal_board_t stub_board;

const hal_board_t *hal_board(void) { return &stub_board; }

void hal_console_write(const char *s, size_t len) { (void)s; (void)len; }

/* no files: every open fails, so callers take their error path */
hal_file_t *hal_fs_open(const char *path, const char *mode) { (void)path; (void)mode; return NULL; }
long hal_fs_write(hal_file_t *f, const void *buf, size_t len) { (void)f; (void)buf; (void)len; return -1; }
int hal_fs_close(hal_file_t *f) { (void)f; return -1; }
int hal_fs_rename(const char *from, const char *to) { (void)from; (void)to; return -1; }
int hal_fs_remove(const char *path) { (void)path; return -1; }

/* a small in-memory key-value store (a zero length removes a key): the settings tests need one */
#include <string.h>
#define KV_N 8
#define KV_MAX 32
static struct { char key[24]; uint8_t val[KV_MAX]; size_t len; } kv[KV_N];

int hal_kv_get(const char *key, void *buf, size_t cap) {
    for (int i = 0; i < KV_N; i++)
        if (kv[i].len && !strcmp(kv[i].key, key)) {
            size_t n = kv[i].len < cap ? kv[i].len : cap;
            memcpy(buf, kv[i].val, n);
            return (int)kv[i].len;
        }
    return -1;
}
int hal_kv_set(const char *key, const void *buf, size_t len) {
    if (strlen(key) >= sizeof kv[0].key || len > KV_MAX) return -1;
    int free_i = -1;
    for (int i = 0; i < KV_N; i++) {
        if (kv[i].len && !strcmp(kv[i].key, key)) { free_i = i; break; }
        if (!kv[i].len && free_i < 0) free_i = i;
    }
    if (free_i < 0) return -1;
    strcpy(kv[free_i].key, key);
    if (len) memcpy(kv[free_i].val, buf, len);
    kv[free_i].len = len;
    return (int)len;
}

long hal_link_read(hal_link_t t, uint8_t *buf, size_t cap, uint32_t timeout_ms) {
    (void)t; (void)buf; (void)cap; (void)timeout_ms;
    return 0;
}
long hal_link_write(hal_link_t t, const uint8_t *buf, size_t len) { (void)t; (void)buf; return (long)len; }

int hal_panel_begin(uint8_t depth, hal_refresh_t mode) { (void)depth; (void)mode; return 0; }
bool hal_panel_gray_partial(void) { return false; }
size_t hal_panel_tune_count(void) { return 0; }
const hal_tune_t *hal_panel_tune_info(size_t i) { (void)i; return NULL; }
int hal_panel_tune_get(size_t i) { (void)i; return 0; }
bool hal_panel_tune_set(size_t i, int v) { (void)i; (void)v; return false; }
void hal_panel_tune_defaults(void) {}
bool hal_panel_gray_rows_set(const char *s, size_t len, char *why, size_t cap)
{
    (void)s;
    (void)len;
    if (cap)
        why[0] = 0;
    return false;
}
void hal_panel_gray_rows_get(char *out, size_t cap)
{
    if (cap)
        out[0] = 0;
}
void hal_panel_new_image(bool image) { (void)image; }
void hal_panel_palette(uint8_t palette) { (void)palette; }
int hal_panel_refresh(void) { return 0; }
void hal_panel_sleep(void) {}
int hal_panel_write(const tiny_rect_t *r, const uint8_t *prev, const uint8_t *next, size_t stride) {
    (void)r; (void)prev; (void)next; (void)stride;
    return 0;
}
