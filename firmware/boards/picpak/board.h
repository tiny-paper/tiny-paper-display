/*
 * boards/picpak/board.h - PicPak (ESP32-C3, 32 MB flash chip of which 16 MB addressable,
 * 4.2" 400x300 BWRY panel, GDEY042F51 family, panel id 0x060401).
 * Pins from GottZ/open-PicPak 7b76daa documentation/device.md (sections 4.2, 5, 6), cross-checked with
 * varanu5/picpak-tesserae-client 834811c README + firmware/main/board.h and kohlhofer/picpak-semiotic
 * d13798e firmware/main/board.h (facts only; no code taken). All three agree on every pin.
 * Confirmed on a real PicPak (2026-09-24, rev v0.4, flash JEDEC 0xc22019, panel id 0x060401):
 * panel "stock" profile correct (colors, orientation, 18.1 s refresh); button GPIO2 active low (digital);
 * battery 4182 mV on USB via GPIO2 x1.45; LED active LOW (open-PicPak said high); IMU LSM6DS3TR-C
 * WHO_AM_I 0x6a in SPI mode 3; GPIO5 idle high, data-ready test inconclusive (INT1 or INT2).
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BOARD_PICPAK_H
#define BOARD_PICPAK_H

#define BOARD_ID            "picpak"
#define BOARD_PANEL_BWRY    1   /* panel driver: epd_bwry.h (not the UC8179 one) */

/* Shared SPI bus (SPI2): panel + IMU */
#define BOARD_EPD_SCK       6
#define BOARD_EPD_MOSI      3
#define BOARD_SPI_MISO      4   /* IMU SDO; tesserae also reads the panel id on it */
#define BOARD_EPD_CS        9
#define BOARD_EPD_DC        8
#define BOARD_EPD_RST       10
#define BOARD_EPD_BUSY      20  /* low = busy (GxEPD2 busy_level LOW, open-PicPak) */
#define BOARD_EPD_SPI_HZ    1000000 /* stock and tesserae use 1 MHz */
#define BOARD_EPD_W         400
#define BOARD_EPD_H         300

/* App (hal_esp board.c). Depth 1: black/white as is. Depth 2: black and white as is, the two grays as a
 * 4x4 ordered dither (panel_bwry.c); frames that use color (docs/spec/assets.md "Palette frames": scene 1.8 inks,
 * BWRY photos, OS screen accents) hold palette codes: red and yellow. No partial refresh: every change is a full
 * refresh. */
#define BOARD_DEPTHS        0x3
#define BOARD_PARTIAL_REFRESH 0 /* "!tp part on": experimental windowed partial at runtime (panel_bwry.c) */
/* Box partial for the press mark (hal_panel_box_partial, panel_bwry.c): its label as one window at the refresh
 * speed, the rest of the panel not driven. Off until the hardware check (responsiveness plan task 16) */
#define BOARD_BOX_PARTIAL   0
#define BOARD_CODE_PARTITION 0x10000 /* partitions.csv `code` */
/* album mode: photos kept in `storage` (docs/spec/album.md "Budget": 11.6 MiB, about 247 photos at 2 bits) */
#define BOARD_PHOTO_STORE   1
/* Experimental faster refresh (epd_bwry.h bwry_fast_t), default off until judged on hardware.
 * Overridden at runtime by "!tp bwry fast <off|tse|e6|e6p>" (NVS). */
#define BOARD_BWRY_FAST     0

/* IMU: ST LSM6DS3TR-C (WHO_AM_I 0x6a) on the same bus. Its CS must stay high while the panel talks. */
#define BOARD_IMU_CS        7
#define BOARD_IMU_INT       5   /* INT1 per open-PicPak, active high, deep-sleep wake capable */
#define BOARD_IMU_WHOAMI    0x6a

/* Button and battery share GPIO2 (ADC1 ch2): the battery reaches the pin through a divider
 * (battery = pin x 1.45) and the button shorts the node to ground. No internal pulls: a pull-up
 * would bias the divider. Released: pin ~ Vbat / 1.45 (2.2 to 2.9 V, reads high). Pressed: ~0 V.
 * Stock reads the ADC with GPIO2 out of key mode and restores it after (open-PicPak 6.3). */
#define BOARD_BUTTON        2   /* active low, deep-sleep wake (GPIO0..5 only on the C3) */
#define BOARD_BUTTON_SHARED_ADC 1
#define BOARD_BATT_ADC_GPIO 2
#define BOARD_BATT_DIV_X100 145 /* stock: battery_mv = (pin_mv * 145 + 50) / 100 */
#define BOARD_BUTTON_ADC_MV 1000 /* pin below this = pressed (or no battery) */

/* Status LED: the app keeps it off */
#define BOARD_LED           21  /* also U0TXD: console stays on USB */
#define BOARD_LED_ACTIVE_LOW 1  /* confirmed on hardware */

/* WiFi TX power cap in 0.25 dBm (esp_wifi_set_max_tx_power). The board browns out at the TX peak:
 * open-PicPak measured 14 dBm OK / 15 dBm flapping and starts at 11; semiotic caps at 10. */
#define BOARD_WIFI_MAX_TX_QDBM 40
/* CPU clock while the radio starts (hal_esp net.c cpu_slow): less current at the PHY calibration and first TX
 * peak (brownout on USB connect, watchdog at portal start, 2026-09-25). tesserae runs at 80 MHz always. */
#define BOARD_RADIO_START_CPU_MHZ 80

/* 32 kHz crystal pins if fitted (XTAL_32K_P/N). None documented. */
#define BOARD_XTAL32K_P     0
#define BOARD_XTAL32K_N     1

#endif
