/*
 * boards/trmnl_og/board.h - TRMNL OG (ESP32-C3, 4 MB flash, 7.5" 800x480 UC8179 panel).
 * Pins confirmed against usetrmnl/trmnl-firmware f5f87b7 (src/display.cpp device_list "og",
 * src/battery/adc_battery.cpp, src/pins.cpp) and jesserockz/esphome-trmnl trmnl.yaml.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BOARD_TRMNL_OG_H
#define BOARD_TRMNL_OG_H

#define BOARD_ID            "trmnl_og"

/* Panel (SPI2, write-only; MOSI doubles as the data line for register reads) */
#define BOARD_EPD_SCK       7
#define BOARD_EPD_MOSI      8
#define BOARD_EPD_CS        6
#define BOARD_EPD_DC        5
#define BOARD_EPD_RST       10
#define BOARD_EPD_BUSY      4   /* UC8179 BUSY_N: low = busy */
#define BOARD_EPD_SPI_HZ    4000000 /* stock uses 8 MHz; bring-up is conservative */
#define BOARD_EPD_W         800
#define BOARD_EPD_H         480

/* App (hal_esp board.c): depths 1-bit and 2-bit (4-gray full refresh), partial refresh, `code` partition bytes */
#define BOARD_DEPTHS        0x3
#define BOARD_PARTIAL_REFRESH 1
#define BOARD_CODE_PARTITION 0xe000

/* Button: stock wakes on GPIO2 (active low, internal pull-up in stock pins.cpp).
 * esphome-trmnl reads GPIO9 (the BOOT strap). Both are reported by the bring-up app. */
#define BOARD_BUTTON        2
#define BOARD_BUTTON_ALT    9   /* BOOT strap: held low at reset = ROM download mode */
#define BOARD_BUTTON_PULLUP 1   /* the app enables the internal pull-up */

/* Battery: ADC1 channel 3 on GPIO3, x2 divider, stock averages 8 x analogReadMilliVolts (12 dB). */
#define BOARD_BATT_ADC_GPIO 3
#define BOARD_BATT_DIV_X100 200

/* Qwiic I2C (unused here) */
#define BOARD_I2C_SDA       21
#define BOARD_I2C_SCL       20

/* 32 kHz crystal pins if fitted (ESP32-C3 XTAL_32K_P/N). None known on the OG. */
#define BOARD_XTAL32K_P     0
#define BOARD_XTAL32K_N     1

#endif
