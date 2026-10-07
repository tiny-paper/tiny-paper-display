/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tiny/busy.h"
#include "tiny/hal.h"

static bool reporting;

__attribute__((weak)) void tiny_busy_plugin(char out[65]) { out[0] = '-'; out[1] = 0; }
__attribute__((weak)) void tiny_busy_log(const char *name, uint64_t ms, const char *plugin, bool slow)
{
    (void)name; (void)ms; (void)plugin; (void)slow;
}

tiny_busy_step_t tiny_busy_begin(void)
{
    tiny_busy_step_t step = {.enabled = !reporting};
    if (step.enabled) {
        tiny_busy_plugin(step.plugin);
        step.rtc_us = hal_rtc_us();
    }
    return step;
}

void tiny_busy_end(const char *name, const tiny_busy_step_t *step)
{
    if (!step->enabled || reporting)
        return;
    uint64_t now = hal_rtc_us();
    uint64_t us = now >= step->rtc_us ? now - step->rtc_us : 0;
    reporting = true; /* logging can itself flush to flash */
    tiny_busy_log(name, us / 1000 + (us % 1000 != 0), step->plugin, us > TINY_BUSY_LIMIT_US);
    reporting = false;
}
