/*
 * boards/trmnl_x/board.h - TRMNL X (ESP32-S3 N16R8: 16 MB flash, 8 MB octal PSRAM; 10.3" 1872x1404
 * parallel panel, 16 grays; ESP32-C5 5 GHz WiFi modem running ESP-AT over UART).
 *
 * Sources (C = confirmed in code, G = guessed / inferred, check in M7 bring-up):
 *   T = usetrmnl/trmnl-firmware f5f87b7: include/config.h, src/display.cpp, src/bl.cpp config_gpio_for_lp,
 *       lib/trmnl_x/include/modem.h, sdkconfigs/sdkconfig.TRMNL_X
 *   F = bitbank2/FastEPD 8dc8c74 (the commit stock pins): src/FastEPD.inl BB_PANEL_TRMNL_X, EPDiyV7*
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BOARD_TRMNL_X_H
#define BOARD_TRMNL_X_H

#define BOARD_ID            "trmnl_x"
#define BOARD_PANEL_FASTEPD 1           /* panel through components/epd FastEPD glue, not UC8179 */
/* N16R8: octal PSRAM, so GPIO26..37 belong to flash/PSRAM (components/epd/include/epd_pins.h). No line
 * here may use them: static asserts at the end, the bring-up checks the data bus at runtime, the glue
 * checks FastEPD's own table after init. */
#define BOARD_PSRAM_OCTAL   1

/* Panel: 16-bit parallel through the S3 LCD (i80) peripheral, driven by FastEPD (F, C). */
#define BOARD_EPD_W         1872
#define BOARD_EPD_H         1404
/* Bus bit 0..15 (FastEPD order; stock bl.cpp names 9..14,21,47 "d0-d7": byte order may differ, G) */
#define BOARD_EPD_DATA_LIST 8, 18, 17, 16, 15, 7, 6, 5, 47, 21, 14, 13, 12, 11, 10, 9
#define BOARD_EPD_DATA      { BOARD_EPD_DATA_LIST }
#define BOARD_EPD_CL        4   /* XCL, i80 WR (C) */
#define BOARD_EPD_SPH       41  /* XSTL, i80 CS (C) */
#define BOARD_EPD_CKV       45  /* strapping pin (C) */
#define BOARD_EPD_SPV       48  /* STV (C) */
#define BOARD_EPD_LE        42  /* XLE (C) */
#define BOARD_EPD_DC_DUMMY  46  /* unused pin esp_lcd toggles as DC; strapping pin (C) */
#define BOARD_EPD_VCOM_MV   (-1100) /* FastEPD table value (C); per-panel VCOM label not read (G) */

/* Internal I2C bus (C: T config.h, F) */
#define BOARD_I2C_SDA       39
#define BOARD_I2C_SCL       40
#define BOARD_I2C_ADDR_ACCEL   0x18 /* Bosch BMA530 (C) */
#define BOARD_I2C_ADDR_IOX     0x20 /* TCA9535 IO expander, INT on GPIO38 (C) */
#define BOARD_I2C_ADDR_TOUCH   0x44 /* Azoteq IQS323 touchbar (C) */
#define BOARD_I2C_ADDR_GAUGE   0x55 /* TI BQ27427 fuel gauge (C) */
#define BOARD_I2C_ADDR_PMIC    0x68 /* TPS6518x panel PMIC; TPS65185 part number is G. Answers only while
                                       * PMIC_WAKEUP is high, so a plain scan does not see it */
/* Scan 2026-09-25 also found 0x42 and 0x45: not named anywhere in stock f5f87b7 or FastEPD 8dc8c74 (their
 * code only addresses 0x18, 0x20, 0x44, 0x55, 0x68). Unknown; never written by the bring-up. */
#define BOARD_IOX_INT       38

/* TCA9535 pins, FastEPD numbering (port 0 = 0..7, port 1 = 8..15) (C unless marked) */
#define IOX_CHG_PG          0   /* BQ25616 power good, low = VBUS OK */
#define IOX_OTG_EN          1   /* charger OTG (dock 5 V out): never touch in bring-up */
#define IOX_CHG_STAT        2   /* low = charging */
#define IOX_ACCEL_INT       3   /* BMA530 INT1 (G) */
#define IOX_C5_SPI_BOOT     4   /* C5 strap */
#define IOX_C5_USB_BOOT     5   /* C5 strap */
#define IOX_C5_EN           6   /* C5 enable; stock drives it low in sleep */
#define IOX_BATT_DET        7   /* RC timing: 1 vs 2 cells */
#define IOX_EPD_OE          8
#define IOX_EPD_GMODE       9
#define IOX_GAUGE_RST       10  /* BQ27427 reset, low = reset. FastEPD init makes it an output low: glue restores input */
#define IOX_PMIC_PWRUP      11
#define IOX_PMIC_VCOM_CTRL  12
#define IOX_PMIC_WAKEUP     13
#define IOX_PMIC_PWR_GOOD   14  /* input */
#define IOX_PMIC_NINT       15  /* input */

/* App (hal_esp). Depths 1, 2 (as 4 grays of the 16) and 4 (16 grays). Frames live in PSRAM (no banding).
 * No `code` partition: with PSRAM the plugin module stays in RAM. */
#define BOARD_DEPTHS        0x7
#define BOARD_PARTIAL_REFRESH 1
#define BOARD_CODE_PARTITION 0
#define BOARD_BIG           1   /* manifest.md "Limits per board": trmnl_x row (PSRAM) */
/* FastEPD passes (stock display.cpp setPasses(3, 3)); overridden live by "!tp passes <p> <f>" (NVS).
 * Partial 4 (hardware 2026-09-25: 3 leaves new blacks gray, 5 better). */
#define BOARD_EPD_PARTIAL_PASSES 4
#define BOARD_EPD_FULL_PASSES    3
/* 1 bpp partial waveform around the passes (epd_parwave.h), "!tp black <kick> <extra> [redrive]" (NVS):
 * 2 white frames before new blacks, no extra black frames, each partial black re-driven in the kick frames
 * of the next 2 partials (not more: unbounded redrive drew lines, 2026-09-25). About 53 ms per frame + 45 ms
 * rails: (2+1) + (4+1) frames ~ 0.5 s per partial. */
#define BOARD_EPD_WAVE_KICK      2
#define BOARD_EPD_WAVE_EXTRA     0
#define BOARD_EPD_WAVE_REDRIVE   2
/* "!tp every <n>" (NVS "xevery"): a full refresh after n partials on top of the plugin's cadence; 0 = off. */
#define BOARD_EPD_FULL_EVERY     0
/* Partial row step settle times in us (epd_rowtime.h), "!tp rows <settle> <hold>" (NVS): gate off before LE,
 * sources settled before the next gate. 0 0 = FastEPD as is. ~5 ms more per frame at 2 2. */
#define BOARD_EPD_ROW_SETTLE     2
#define BOARD_EPD_ROW_HOLD       2

/* Buttons: no user button. BOOT (GPIO0) and EN are the dock's magnet "reset points" (G: wiring).
 * The IQS323 touchbar RDY line is GPIO3, active low, stock ext0 deep-sleep wake (C). */
#define BOARD_BUTTON        3
#define BOARD_BUTTON_ALT    0
#define BOARD_BUTTON_EXT1   1   /* S3: deep-sleep wake through the RTC IO (app: ext0 as stock, bring-up test: ext1) */
/* GPIO3 is not a button level: in streaming mode RDY is low ~210 ms / high ~10 ms forever (bring-up
 * 2026-09-25). "Pressed" = a touch bit in the IQS323 SYSTEM_STATUS, read over I2C (hal_esp x_board.c);
 * before deep sleep the IQS323 goes to event mode (touch events only) so RDY only falls on a touch. */
#define BOARD_BUTTON_TOUCHBAR 1

/* Battery: no ADC; BQ27427 gauge on I2C (C). BYPASS_BQ27427_SOC in stock build flags. */
#define BOARD_BATT_GAUGE    1
/* VBUS is sensed (IOX_CHG_PG, hal_x_vbus): on the dock or a charger the chip stays awake through a quiet USB bus
 * (hal_esp sys.c hal_esp_stay_awake), so a host that suspended the bus finds it enumerated again (issue #133) */
#define BOARD_VBUS_SENSE    1
/* Cell: 6000 mAh (one or two), "3.8V LiPo" (trmnl.com X spec sheet); stock's BQ27427 golden file: design capacity
 * 6000 mAh, design energy 23100 mWh (3.85 V nominal). A 4.35 V LiHV cell: 4334..4347 mV right after a charge
 * (device logs 2026-09-27..29). power.c LiHV curve, charging below 4300 mV */
#define BOARD_BATT_CELL     HAL_BATT_LIHV

/* ESP32-C5 modem link: ESP-AT (5.1.0.0-dev), 115200 at boot, then AT+UART_CUR=5000000,8,1,0,3 with RTS/CTS (C: T
 * modem.h, modem.cpp; measured 2026-09-30, docs/design/x-5ghz.md). RTS and CTS as seen from the S3: stock's macros
 * AT_UART_RTS 1 / AT_UART_CTS 2 go into Arduino setPins(rx, tx, cts, rts), so GPIO1 is the S3's CTS input and
 * GPIO2 its RTS output; the other way round nothing gets through with flow control on. Any S3 UART can take the
 * pins (hal_esp esp/net_c5.c: UART1). Power: expander IOX_C5_EN, straps IOX_C5_SPI_BOOT/USB_BOOT as inputs. */
#define BOARD_C5_TX         43
#define BOARD_C5_RX         44
#define BOARD_C5_RTS        2
#define BOARD_C5_CTS        1

/* USB: native USB Serial/JTAG on GPIO19/20, only through the dock pogo pins (C). 32 kHz crystal: none (G,
 * stock uses the internal RC; the S3 XTAL32K pins 15/16 are panel data here, so never probe them). */

/* Flash/PSRAM pins (26..37 with octal PSRAM) must stay untouched: every scalar line checked at build time. */
#if !defined(__cplusplus)
#define BOARD_PIN_FREE_(p) ((p) < 26 || (p) > 37)
_Static_assert(BOARD_PIN_FREE_(BOARD_EPD_CL) && BOARD_PIN_FREE_(BOARD_EPD_SPH) && BOARD_PIN_FREE_(BOARD_EPD_CKV) &&
               BOARD_PIN_FREE_(BOARD_EPD_SPV) && BOARD_PIN_FREE_(BOARD_EPD_LE) && BOARD_PIN_FREE_(BOARD_EPD_DC_DUMMY),
               "panel control line on a flash/PSRAM pin");
_Static_assert(BOARD_PIN_FREE_(BOARD_I2C_SDA) && BOARD_PIN_FREE_(BOARD_I2C_SCL) && BOARD_PIN_FREE_(BOARD_IOX_INT) &&
               BOARD_PIN_FREE_(BOARD_BUTTON) && BOARD_PIN_FREE_(BOARD_BUTTON_ALT) && BOARD_PIN_FREE_(BOARD_C5_TX) &&
               BOARD_PIN_FREE_(BOARD_C5_RX) && BOARD_PIN_FREE_(BOARD_C5_RTS) && BOARD_PIN_FREE_(BOARD_C5_CTS),
               "board line on a flash/PSRAM pin");
#define BOARD_PINS_FREE16_(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p)                                         \
    (BOARD_PIN_FREE_(a) && BOARD_PIN_FREE_(b) && BOARD_PIN_FREE_(c) && BOARD_PIN_FREE_(d) && BOARD_PIN_FREE_(e) &&   \
     BOARD_PIN_FREE_(f) && BOARD_PIN_FREE_(g) && BOARD_PIN_FREE_(h) && BOARD_PIN_FREE_(i) && BOARD_PIN_FREE_(j) &&   \
     BOARD_PIN_FREE_(k) && BOARD_PIN_FREE_(l) && BOARD_PIN_FREE_(m) && BOARD_PIN_FREE_(n) && BOARD_PIN_FREE_(o) &&   \
     BOARD_PIN_FREE_(p))
#define BOARD_PINS_FREE16(...) BOARD_PINS_FREE16_(__VA_ARGS__)
_Static_assert(BOARD_PINS_FREE16(BOARD_EPD_DATA_LIST), "panel data line on a flash/PSRAM pin");
#endif

#endif
