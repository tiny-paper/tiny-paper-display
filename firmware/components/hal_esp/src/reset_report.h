/* Reset reporting across USB wake cycles. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TINY_RESET_REPORT_H
#define TINY_RESET_REPORT_H

#include <stdbool.h>

typedef struct {
    const char *reason;
    const char *step;
    bool boot_logged;
} reset_report_t;

/* Strings must outlive the cycle. Reset history is kept separately. */
void reset_report_boot(reset_report_t *r, const char *reason, const char *step);
void reset_report_logged(reset_report_t *r);
const char *reset_report_summary(const reset_report_t *r);
/* A completed cycle consumes the current fault, including the interrupted step. */
void reset_report_cycle_done(reset_report_t *r);

#endif
