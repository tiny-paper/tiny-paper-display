/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Stand-ins for the managed FastEPD component under ESP-IDF, force-included for that component only
 * (../CMakeLists.txt). Why: upstream 3d2a63a calls Arduino's pinMode and digitalWrite in bbepInitPanel for
 * BB_PANEL_M5PAPER (upstream commit 7422433), which ESP-IDF does not have; the library is not patched for it.
 * No board of ours selects that panel, but the calls are mapped to what the library uses itself, so the path is
 * correct if ever reached. Goes when upstream head builds under ESP-IDF without it (the build then still passes
 * with this file removed). Macros: they resolve where the library has bbepPinMode and driver/gpio.h in scope. */
#ifndef FASTEPD_IDF_STANDIN_H
#define FASTEPD_IDF_STANDIN_H
#ifndef ARDUINO
#define pinMode(pin, mode) bbepPinMode((pin), (mode))
#define digitalWrite(pin, level) ((void)gpio_set_level((gpio_num_t)(pin), (uint32_t)(level)))
#endif
#endif
