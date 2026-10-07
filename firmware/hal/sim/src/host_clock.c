/* The display repository's host tests run without the simulator: this is the one HAL call the exported core makes
 * outside the device binding, the clock (components/common/src/busy.c). SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <time.h>

#include "tiny/hal.h"

uint64_t hal_rtc_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
