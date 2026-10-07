/*
 * hal_esp.h - ESP-IDF side of tiny/hal.h (ESP32-C3, TRMNL OG). Only firmware/main
 * uses this header; core code sees tiny/hal.h alone.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef HAL_ESP_H
#define HAL_ESP_H

#include <stdbool.h>
#include <stdint.h>

/* Bring up console + link transport, NVS, LittleFS, the button and the wake cause.
 * Call first in app_main. */
void hal_esp_init(void);

/* Consecutive boots that ended in a panic or watchdog reset (RTC memory, cleared when
 * a wake cycle reaches hal_sleep_until). */
uint8_t hal_esp_crash_count(void);
/* The reset reason as a short name ("poweron", "deepsleep", "panic", ...). */
const char *hal_esp_reset_name(void);

/* Stay awake between ticks (no deep sleep, no light sleep) because a USB host is
 * attached, the build forces it, or the link runs on UART0 (QEMU). */
bool hal_esp_stay_awake(void);
/* Called by hal_reboot before a software restart (weak, default empty): the app commits its RTC state. */
void hal_esp_pre_reboot(void);

/* Heap watermark bookkeeping (plan 3.9): call at the end of a phase. */
typedef enum { HAL_ESP_PH_BOOT = 0, HAL_ESP_PH_RENDER, HAL_ESP_PH_NET, HAL_ESP_PH_SLEEP, HAL_ESP_PH_COUNT } hal_esp_phase_t;
void hal_esp_heap_mark(hal_esp_phase_t ph);

#endif
