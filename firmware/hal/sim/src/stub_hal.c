/* Inert HAL for the host tests of the display repository (no simulator here): the symbols the core calls outside the
 * device binding, so the shared core links. Panel calls do nothing and succeed; storage is empty; the link is silent.
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

/* empty store: nothing is found, nothing is kept */
int hal_kv_get(const char *key, void *buf, size_t cap) { (void)key; (void)buf; (void)cap; return -1; }
int hal_kv_set(const char *key, const void *buf, size_t len) { (void)key; (void)buf; (void)len; return -1; }

long hal_link_read(hal_link_t t, uint8_t *buf, size_t cap, uint32_t timeout_ms) {
    (void)t; (void)buf; (void)cap; (void)timeout_ms;
    return 0;
}
long hal_link_write(hal_link_t t, const uint8_t *buf, size_t len) { (void)t; (void)buf; return (long)len; }

int hal_panel_begin(uint8_t depth, hal_refresh_t mode) { (void)depth; (void)mode; return 0; }
bool hal_panel_gray_partial(void) { return false; }
void hal_panel_new_image(bool image) { (void)image; }
void hal_panel_palette(uint8_t palette) { (void)palette; }
int hal_panel_refresh(void) { return 0; }
void hal_panel_sleep(void) {}
int hal_panel_write(const tiny_rect_t *r, const uint8_t *prev, const uint8_t *next, size_t stride) {
    (void)r; (void)prev; (void)next; (void)stride;
    return 0;
}
