/* See epd_gridcell.h. C11. SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_gridcell.h"

#include <string.h>

/* 3x5 digits, one row per byte, bit 2 the left pixel */
static const uint8_t FONT[10][5] = {
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
    {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
};

static void black_rect(uint8_t *buf, size_t stride, int x0, int y0, int w, int h)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            buf[(size_t)y * stride + (size_t)(x >> 3)] &= (uint8_t)~(0x80u >> (x & 7));
}

static void digit(uint8_t *buf, size_t stride, int x0, int y0, int d)
{
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 3; c++)
            if (FONT[d][r] & (4 >> c))
                black_rect(buf, stride, x0 + c * EPD_GRID_SCALE, y0 + r * EPD_GRID_SCALE, EPD_GRID_SCALE, EPD_GRID_SCALE);
}

int epd_grid_cell(uint8_t *buf, size_t stride, int w, int h, int col, int row)
{
    if (!buf || w < EPD_GRID_MIN_W || h < EPD_GRID_MIN_H || w % 8 || stride < (size_t)w / 8 || col < 0 || col > 9 ||
        row < 0 || row > 9)
        return -1;
    for (int y = 0; y < h; y++)
        memset(buf + (size_t)y * stride, 0xff, (size_t)w / 8);
    black_rect(buf, stride, 0, 0, w, EPD_GRID_FRAME);
    black_rect(buf, stride, 0, h - EPD_GRID_FRAME, w, EPD_GRID_FRAME);
    black_rect(buf, stride, 0, 0, EPD_GRID_FRAME, h);
    black_rect(buf, stride, w - EPD_GRID_FRAME, 0, EPD_GRID_FRAME, h);
    const int gw = 3 * EPD_GRID_SCALE, gh = 5 * EPD_GRID_SCALE, gap = 2 * EPD_GRID_SCALE;
    int x0 = (w - (2 * gw + gap)) / 2, y0 = (h - gh) / 2;
    digit(buf, stride, x0, y0, col);
    digit(buf, stride, x0 + gw + gap, y0, row);
    return 0;
}

int epd_grid_black(const uint8_t *buf, size_t stride, int x, int y)
{
    return !(buf[(size_t)y * stride + (size_t)(x >> 3)] & (0x80u >> (x & 7)));
}
