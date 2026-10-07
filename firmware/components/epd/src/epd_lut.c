/* epd_lut.c - see epd_lut.h (tables ported from bb_epaper and GxEPD2, GPL-3.0).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_lut.h"

#include <string.h>

typedef struct {
    uint8_t frames[4];
    uint8_t level[EPD_LUT_N];
} lut_src_t;

/* bb_epaper epd75_init_sequence_partial: VCOM 0x00, WW 0x00, BW 0x80, WB 0x04 (rest 0) */
static const lut_src_t BB = {{30, 1, 30, 1}, {0x00, 0x00, 0x80, 0x04, 0x00, 0x00}};
/* GxEPD2_750_GDEY075T7: T1 30, T2 5, T3 30, T4 5; LUTKW 0x5A "more white", LUTWK 0x84 */
static const lut_src_t GX = {{30, 5, 30, 5}, {0x00, 0x00, 0x5A, 0x84, 0x00, 0x00}};

unsigned epd_lut_reps_clamp(unsigned reps)
{
    return reps < EPD_LUT_REPS_MIN ? EPD_LUT_REPS_MIN : reps > EPD_LUT_REPS_MAX ? EPD_LUT_REPS_MAX : reps;
}

unsigned epd_lut_build(epd_lutset_t set, unsigned reps, uint8_t out[EPD_LUT_N][EPD_LUT_LEN])
{
    const lut_src_t *src = set == EPD_LUTSET_GX ? &GX : &BB;
    unsigned k = epd_lut_reps_clamp(reps);
    for (int p = 0; p < 4; p++)
        while (k > 1 && src->frames[p] * k > 255)
            k--;
    for (int i = 0; i < EPD_LUT_N; i++) {
        memset(out[i], 0, EPD_LUT_LEN);
        out[i][0] = src->level[i];
        for (int p = 0; p < 4; p++)
            out[i][1 + p] = (uint8_t)(src->frames[p] * k);
        out[i][5] = 1;
    }
    return k;
}

int epd_lut_vcom(epd_lutset_t set, uint8_t setting)
{
    if (setting == EPD_VCOM_AUTO)
        return set == EPD_LUTSET_GX ? 0x30 : -1;
    return setting < EPD_VCOM_MIN ? EPD_VCOM_MIN : setting > EPD_VCOM_MAX ? EPD_VCOM_MAX : setting;
}

int epd_lut_dc(const uint8_t lut[EPD_LUT_LEN])
{
    int dc = 0;
    for (int g = 0; g + 6 <= EPD_LUT_LEN; g += 6) {
        int sum = 0;
        for (int p = 0; p < 4; p++) {
            int lv = (lut[g] >> (6 - 2 * p)) & 3;
            sum += lv == 1 ? lut[g + 1 + p] : lv == 2 ? -lut[g + 1 + p] : 0;
        }
        dc += sum * lut[g + 5];
    }
    return dc;
}

unsigned epd_lut_frames(const uint8_t lut[EPD_LUT_LEN])
{
    unsigned n = 0;
    for (int g = 0; g + 6 <= EPD_LUT_LEN; g += 6)
        n += (unsigned)(lut[g + 1] + lut[g + 2] + lut[g + 3] + lut[g + 4]) * lut[g + 5];
    return n;
}

unsigned epd_lut_drive_frames(const uint8_t lut[EPD_LUT_LEN])
{
    unsigned n = 0;
    for (int g = 0; g + 6 <= EPD_LUT_LEN; g += 6)
        for (int p = 0; p < 4; p++)
            if ((lut[g] >> (6 - 2 * p)) & 3)
                n += (unsigned)lut[g + 1 + p] * lut[g + 5];
    return n;
}
