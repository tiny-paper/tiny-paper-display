/* Reset reporting across USB wake cycles. SPDX-License-Identifier: GPL-3.0-or-later */
#include "reset_report.h"

void reset_report_boot(reset_report_t *r, const char *reason, const char *step)
{
    *r = (reset_report_t){.reason = reason, .step = step};
}

void reset_report_logged(reset_report_t *r) { r->boot_logged = true; }

const char *reset_report_summary(const reset_report_t *r)
{
    return r->boot_logged ? "deepsleep" : r->reason;
}

void reset_report_cycle_done(reset_report_t *r)
{
    r->reason = "deepsleep";
    r->step = "";
}
