/*
 * epd_pins.h - ESP32-S3 GPIOs that belong to the in-package flash and PSRAM, and a check of a pin list
 * against them. Hardware-free, host-tested. Driving one of these, or even gpio_reset_pin() on it, breaks
 * the cache path to flash/PSRAM and crashes the chip.
 *   26..32  SPI0/1 flash (and quad PSRAM): CS1, HD, WP, CS0, CLK, Q, D   always
 *   33..37  SPIIO4..7, DQS: octal flash/PSRAM (N16R8, TRMNL X)            with octal PSRAM or flash
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_EPD_PINS_H
#define TINY_EPD_PINS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPD_S3_FLASH_PIN_LO        26
#define EPD_S3_FLASH_PIN_HI_QUAD   32
#define EPD_S3_FLASH_PIN_HI_OCTAL  37

/* True if GPIO pin belongs to flash/PSRAM (octal: 26..37, else 26..32). Negative = unused, never reserved. */
#define EPD_S3_PIN_RESERVED(pin, octal) \
    ((pin) >= EPD_S3_FLASH_PIN_LO && (pin) <= ((octal) ? EPD_S3_FLASH_PIN_HI_OCTAL : EPD_S3_FLASH_PIN_HI_QUAD))

/* Index of the first reserved pin in pins[0..n-1], or -1 if none. */
int epd_pins_first_reserved(const int *pins, int n, bool octal);

#ifdef __cplusplus
}
#endif
#endif
