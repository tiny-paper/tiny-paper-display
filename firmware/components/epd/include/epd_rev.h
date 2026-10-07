/* Panel ID from the UC8179 REV command (0x70), as stock trmnl-firmware v1.8.17 sends it in the Panel-Rev header.
 * Hardware-free. C11. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef EPD_REV_H
#define EPD_REV_H

#include <stdint.h>

/* `raw` = the 7 bytes read after 0x70: 3 filler bytes (0xff), then the 4 ID bytes, first one most significant.
 * Returns the ID, or 0 (unknown) for a short read: all ID bytes 0x00 or 0xff, or filler other than 0xff. */
uint32_t epd_rev_parse(const uint8_t raw[7]);
/* 8 lowercase hex digits and a NUL; empty for 0. */
void epd_rev_str(uint32_t rev, char out[9]);

#endif
