/* Light sleep through a BUSY wait (busy_sleep.h). C11. SPDX-License-Identifier: GPL-3.0-or-later */
#include "busy_sleep.h"

bsl_plan_t bsl_plan(const bsl_in_t *in)
{
    bsl_plan_t p = {0, false};
    if (in->wifi_on || in->stay_awake)
        return p;
    p.button_wake = !in->button_down && in->button_wake_ok;
    uint32_t cap = p.button_wake ? BSL_MAX_MS : BSL_POLL_MS;
    p.ms = in->left_ms < cap ? in->left_ms : cap;
    if (p.ms < BSL_MIN_MS)
        p = (bsl_plan_t){0, false};
    return p;
}
