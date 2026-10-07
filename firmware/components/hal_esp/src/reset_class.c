/* Reset classes (reset_class.h). SPDX-License-Identifier: GPL-3.0-or-later */
#include "reset_class.h"

rst_class_t rst_classify(rst_kind_t kind, bool radio_start)
{
    switch (kind) {
    case RST_KIND_BROWNOUT: return RST_CLASS_POWER;
    case RST_KIND_WDT: return radio_start ? RST_CLASS_POWER : RST_CLASS_CRASH;
    case RST_KIND_PANIC: return RST_CLASS_CRASH;
    default: return RST_CLASS_NONE;
    }
}
