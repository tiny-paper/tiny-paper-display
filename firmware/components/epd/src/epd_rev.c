/* See epd_rev.h. C11. SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_rev.h"

#include <stdio.h>

uint32_t epd_rev_parse(const uint8_t raw[7])
{
    if (raw[0] != 0xff || raw[1] != 0xff || raw[2] != 0xff)
        return 0;
    uint32_t v = (uint32_t)raw[3] << 24 | (uint32_t)raw[4] << 16 | (uint32_t)raw[5] << 8 | raw[6];
    return v == 0xffffffffu ? 0 : v;
}

void epd_rev_str(uint32_t rev, char out[9])
{
    if (rev == 0)
        out[0] = 0;
    else
        snprintf(out, 9, "%08x", (unsigned)rev);
}
