/* See epd_gpartlut.h. C11. SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_gpartlut.h"

#include "epd_parwave.h"

void epd_gpart_lut(int kind, int j, const uint8_t *matrix, int passes, epd_gpart_lut_t *lut)
{
    uint8_t dark[16], light[16]; /* per gray: this step drives it dark, light */
    for (int g = 0; g < 16; g++) {
        int code = kind == EPD_GP_DARK ? 1 : kind == EPD_GP_LIGHT ? 2 : matrix[g * passes + j];
        dark[g] = code == 1;
        light[g] = code == 2;
    }
    for (int b = 0; b < 256; b++) {
        lut->dark[b] = (uint8_t)(dark[b >> 4] << 1 | dark[b & 15]);
        lut->light[b] = (uint8_t)(light[b >> 4] << 1 | light[b & 15]);
    }
}

uint32_t epd_gpart_changed(const uint8_t *old4, const uint8_t *new4, int w, uint8_t *changed)
{
    uint32_t n = 0;
    for (int x = 0; x < w / 8; x++) {
        uint8_t ch = 0;
        for (int k = 0; k < 4; k++) {
            uint8_t d = (uint8_t)(old4[x * 4 + k] ^ new4[x * 4 + k]);
            ch = (uint8_t)(ch << 2 | ((d & 0xf0) ? 2 : 0) | ((d & 0x0f) ? 1 : 0));
        }
        changed[x] = ch;
        n += (uint32_t)__builtin_popcount(ch);
    }
    return n;
}

uint32_t epd_gpart_row_lut(const epd_gpart_lut_t *lut, const uint8_t *new4, const uint8_t *changed, int w, uint8_t *P,
                           uint8_t *C)
{
    uint32_t n = 0;
    for (int x = 0; x < w / 8; x++) {
        const uint8_t *s = new4 + x * 4;
        uint8_t d = (uint8_t)(lut->dark[s[0]] << 6 | lut->dark[s[1]] << 4 | lut->dark[s[2]] << 2 | lut->dark[s[3]]);
        uint8_t l = (uint8_t)(lut->light[s[0]] << 6 | lut->light[s[1]] << 4 | lut->light[s[2]] << 2 | lut->light[s[3]]);
        d &= changed[x];
        l &= changed[x];
        C[x] = (uint8_t)~d; /* dark: previous white, current black */
        P[x] = (uint8_t)~l; /* light: previous black, current white */
        n += (uint32_t)__builtin_popcount((uint8_t)(d | l));
    }
    return n;
}
