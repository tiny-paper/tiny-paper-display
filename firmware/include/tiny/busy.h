/* Busy operation timing. Internal firmware diagnostics, no scene or wire keys.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TINY_BUSY_H
#define TINY_BUSY_H
#include <stdbool.h>
#include <stdint.h>

#define TINY_BUSY_LIMIT_US UINT64_C(2000000)

typedef struct {
    uint64_t rtc_us;
    char plugin[65];
    bool enabled;
} tiny_busy_step_t;

tiny_busy_step_t tiny_busy_begin(void);
void tiny_busy_end(const char *name, const tiny_busy_step_t *step);
/* App hooks. The default sink is silent for component-only tests/bringup. */
void tiny_busy_plugin(char out[65]);
void tiny_busy_log(const char *name, uint64_t ms, const char *plugin, bool slow);

/* Wrap an operation once, including all early returns and failure paths. */
#define TINY_BUSY_WRAP(type, fn, args, call, name) \
    static type fn##_busy_inner args; \
    type fn args { \
        tiny_busy_step_t step = tiny_busy_begin(); \
        type rc = fn##_busy_inner call; \
        tiny_busy_end(name, &step); \
        return rc; \
    } \
    static type fn##_busy_inner args
#endif
