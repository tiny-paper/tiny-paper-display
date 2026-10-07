/* See epd_pins.h. C11. SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_pins.h"

int epd_pins_first_reserved(const int *pins, int n, bool octal)
{
    for (int i = 0; i < n; i++)
        if (EPD_S3_PIN_RESERVED(pins[i], octal)) return i;
    return -1;
}
