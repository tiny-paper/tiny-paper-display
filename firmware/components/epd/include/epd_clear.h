/* TRMNL X clear choice of a full refresh (panel_par.c "!tp clear auto"). Hardware-free.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef EPD_CLEAR_H
#define EPD_CLEAR_H

#include <stdbool.h>
#include <stdint.h>

#include "epd_par.h"

/* Auto: slow clear when the full refresh shows new image content (`image`, hal_panel_new_image: a photo is never
 * shown over traces of the one before) and on every 8th full (`fulls` = fulls since power-on, the backstop); else
 * fast. `why` gets "image", "every8" or "" (fast). Returns EPDP_CLEAR_SLOW or EPDP_CLEAR_FAST. */
static inline int epd_clear_auto(uint8_t fulls, bool image, const char **why)
{
    const char *w = image ? "image" : (fulls & 7) == 0 ? "every8" : "";
    if (why)
        *why = w;
    return *w ? EPDP_CLEAR_SLOW : EPDP_CLEAR_FAST;
}

#endif
