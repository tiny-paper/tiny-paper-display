/* hal_esp: "!tp box" and "!tp grid" on the TRMNL X (panel_par.c): 1 bpp partials from the console, so a partial of a
 * known shape and place can be driven without a finger or a layout, its log lines (panel win, panel partial, panel
 * given) compared with a layout tick's, and a photo of a labelled grid tells which places reach the glass (issue #203).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdlib.h>
#include <string.h>

#include "hal_esp_priv.h"

#if BOARD_PANEL_FASTEPD /* whole file */

#include "epd_gridcell.h"
#include "epd_par.h"

/* One 1 bpp partial from the console: the window shows `next` (stride w/8). inv: the previous frame is the new one
 * inverted, every pixel driven, the press mark's shape (app_mark); else the previous frame is white, only the black
 * pixels are driven, a layout tick's shape. A pending "next refresh is full" of the app does not refuse it
 * (hal_esp_par_console). The error line names the step that failed and the panel state. */
static int drive(int x, int y, int w, int h, bool inv, const uint8_t *next)
{
    size_t stride = (size_t)w / 8, cap = stride * (size_t)h;
    uint8_t *prev = malloc(cap);
    if (!prev) {
        hal_logf("E console partial: no memory for %u bytes", (unsigned)cap);
        return HAL_E_FAIL;
    }
    for (size_t i = 0; i < cap; i++)
        prev[i] = inv ? (uint8_t)~next[i] : 0xff;
    const char *step = "begin";
    hal_esp_par_console(true);
    int rc = hal_panel_begin(1, HAL_REFRESH_PARTIAL);
    if (!rc) {
        tiny_rect_t r = {(int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h};
        step = "write";
        rc = hal_panel_write(&r, prev, next, stride);
        if (!rc) {
            step = "refresh";
            rc = hal_panel_refresh();
        }
        hal_panel_sleep();
    }
    hal_esp_par_console(false);
    free(prev);
    if (rc) {
        char st[96];
        hal_esp_par_state_str(st, sizeof st);
        hal_logf("E console partial x=%d y=%d w=%d h=%d: %s rc=%d %s", x, y, w, h, step, rc, st);
    }
    return rc;
}

/* The window white with a black rectangle inset 16 px. The app's next refresh is a full one (the panel no longer
 * shows its frame). */
static void box(int x, int y, int w, int h, bool inv)
{
    x &= ~7;
    w &= ~7;
    if (w < 32 || h < 32 || x < 0 || y < 0 || x + w > epdp_width() || y + h > epdp_height()) {
        hal_logf("E box: window off the panel or under 32 px");
        return;
    }
    size_t stride = (size_t)w / 8, cap = stride * (size_t)h;
    uint8_t *next = malloc(cap);
    int rc = next ? HAL_OK : HAL_E_FAIL;
    if (!rc) {
        memset(next, 0xff, cap);
        for (int j = 16; j < h - 16; j++)
            memset(next + (size_t)j * stride + 2, 0x00, stride - 4);
        rc = drive(x, y, w, h, inv, next);
    }
    free(next);
    if (!rc)
        hal_esp_par_force_full();
    hal_logf("%s box x=%d y=%d w=%d h=%d %s: look for a black bar there (rc=%d)", rc ? "E" : "I", x, y, w, h,
             inv ? "inv" : "diff", rc);
}

/* "!tp grid [inv|diff] [r0 [r1]]": GRID_COLS x GRID_ROWS cells over the whole panel, each its own partial window
 * showing its column and row number (epd_gridcell.h), rows r0..r1 of them (a batch fits between two ticks). One
 * photo tells which windows reach the glass and whether x or y are mirrored. The app is not told: its next refresh
 * stays what it would have been (gray_mode full: a full wipes the grid at the next tick; hybrid: it stays until the
 * next full, and a clock tick only redraws its own pixels). */
#define GRID_COLS 8
#define GRID_ROWS 6

static void grid(bool inv, int r0, int r1)
{
    const int W = epdp_width(), H = epdp_height();
    const int cw = (W / GRID_COLS) & ~7, ch = H / GRID_ROWS;
    size_t stride = (size_t)cw / 8, cap = stride * (size_t)ch;
    uint8_t *next = malloc(cap);
    if (!next) {
        hal_logf("E grid: no memory for %u bytes", (unsigned)cap);
        return;
    }
    int ok = 0, fail = 0;
    for (int r = r0; r <= r1; r++)
        for (int c = 0; c < GRID_COLS; c++) {
            int x = c * cw, y = r * ch;
            int rc = epd_grid_cell(next, stride, cw, ch, c, r) ? HAL_E_FAIL : drive(x, y, cw, ch, inv, next);
            hal_logf("%s grid cell %d%d x=%d y=%d w=%d h=%d rc=%d", rc ? "E" : "I", c, r, x, y, cw, ch, rc);
            if (rc)
                fail++;
            else
                ok++;
        }
    free(next);
    hal_logf("%s grid %s rows %d..%d: %d cells driven, %d failed; look: each cell a frame with its column 0..%d and row "
             "0..%d",
             fail ? "E" : "I", inv ? "inv" : "diff", r0, r1, ok, fail, GRID_COLS - 1, GRID_ROWS - 1);
}

static bool num(const char *s, long lo, long hi, long *out)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (*s == 0 || *end || v < lo || v > hi)
        return false;
    *out = v;
    return true;
}

/* "!tp win all|diff" (NVS "xwinall"): the app's 1 bpp partials drive every pixel of their windows (all) or only the
 * changed ones (diff, the default). Read at boot and on every change. */
static uint8_t s_win_all;
static bool s_win_read;
static int8_t s_win_over = -1; /* hal_panel_partial_all (the display firmware): wins over the setting; -1 none */

bool hal_panel_partial_all(bool all)
{
    s_win_over = all;
    return true;
}

bool hal_esp_par_win_all(void)
{
    if (s_win_over >= 0)
        return s_win_over;
    if (!s_win_read) {
        uint8_t v;
        if (hal_kv_get("xwinall", &v, 1) == 1)
            s_win_all = v == 1;
        s_win_read = true;
    }
    return s_win_all;
}

uint32_t hal_esp_par_win_diff(const uint8_t *prev, const uint8_t *next, size_t stride, int w, int h)
{
    uint32_t diff = 0;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w / 8; i++)
            diff += (uint32_t)__builtin_popcount((uint8_t)(prev[(size_t)j * stride + i] ^ next[(size_t)j * stride + i]));
    return diff;
}

bool hal_esp_par_box_cmd(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[0], "win") && (!strcmp(argv[1], "all") || !strcmp(argv[1], "diff"))) {
        s_win_all = argv[1][0] == 'a';
        s_win_read = true;
        hal_kv_set("xwinall", &s_win_all, 1);
        hal_logf("I win %s: the app's partials drive %s", s_win_all ? "all" : "diff",
                 s_win_all ? "every pixel of their windows" : "the changed pixels");
        return true;
    }
    if (argc >= 1 && !strcmp(argv[0], "grid")) {
        int i = 1;
        bool inv = true;
        if (argc > i && (!strcmp(argv[i], "inv") || !strcmp(argv[i], "diff")))
            inv = argv[i++][0] == 'i';
        long r0 = 0, r1 = GRID_ROWS - 1;
        if ((argc > i && !num(argv[i], 0, GRID_ROWS - 1, &r0)) || (argc > i + 1 && !num(argv[i + 1], r0, GRID_ROWS - 1, &r1)) ||
            argc > i + 2) {
            hal_logf("E usage: grid [inv|diff] [r0 [r1]] (rows 0..%d)", GRID_ROWS - 1);
            return true;
        }
        if (argc == i + 1)
            r1 = r0;
        grid(inv, (int)r0, (int)r1);
        return true;
    }
    if (argc < 1 || strcmp(argv[0], "box"))
        return false;
    long x, y, w, h;
    if (argc < 5 || argc > 6 || !num(argv[1], 0, 4095, &x) || !num(argv[2], 0, 4095, &y) || !num(argv[3], 32, 4096, &w) ||
        !num(argv[4], 32, 4096, &h) || (argc == 6 && strcmp(argv[5], "inv") && strcmp(argv[5], "diff"))) {
        hal_logf("E usage: box <x> <y> <w> <h> [inv|diff]");
        return true;
    }
    box((int)x, (int)y, (int)w, (int)h, argc < 6 || !strcmp(argv[5], "inv"));
    return true;
}

#endif /* BOARD_PANEL_FASTEPD */
