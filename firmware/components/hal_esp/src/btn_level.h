/*
 * btn_level.h - a button that shares its pin with the battery divider (PicPak GPIO2, board.h
 * BOARD_BUTTON_SHARED_ADC): released, the pin sits at Vbat x 100 / BOARD_BATT_DIV_X100 (Vbat / 1.45), pressed at
 * ~0 V. The C3's digital input needs VIH = 0.75 x VDD (~2.48 V at 3.3 V): below a ~3.6 V battery a released button
 * is out of spec as a digital high and may read pressed (research 2026-09-26 #14). esp/io.c reads the pin through
 * the ADC and decides here. Hardware-free, host-tested. C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_BTN_LEVEL_H
#define TINY_BTN_LEVEL_H

#include <stdbool.h>

#define BTN_VDD_MV 3300
#define BTN_VIH_PCT 75

/* Pressed-below threshold of the pin in mV: board's `max_mv` (BOARD_BUTTON_ADC_MV), lowered to half the released
 * level for a battery so low that max_mv is not far enough below it. batt_mv <= 0: unknown, max_mv. */
int btn_threshold_mv(int batt_mv, int div_x100, int max_mv);
/* pin_mv: the ADC reading of the pin. */
bool btn_pressed_mv(int pin_mv, int batt_mv, int div_x100, int max_mv);
/* The released level is a valid digital high (VIH) at this battery: the GPIO deep-sleep wake cannot fire on its
 * own. batt_mv <= 0: unknown, false. */
bool btn_digital_in_spec(int batt_mv, int div_x100);

/* One reading of the pin: the ADC (pin_mv; < 0: the read failed) and right after it the digital input (dig_low).
 * A press shorts the node to ~0 V, which the digital input reads low at any battery; the ADC alone is not trusted
 * for a press (PicPak 2026-09-26/27: after a watchdog and a software reset the pin read below 1000 mV, i.e. "held":
 * safe mode, batt=-1, and one 10 s "hold" that factory-reset; no cell can be missing, no divider is switched, a
 * supply sag cannot bring Vbat / 1.45 under 1 V). So:
 *   ADC pressed, digital low         BTN_LVL_DOWN
 *   ADC pressed, digital high        BTN_LVL_ADC_BAD: not a press, the ADC reading is wrong (logged by io.c)
 *   ADC released                     BTN_LVL_UP (also a digital low: a released level under VIH on a weak cell)
 *   ADC failed                       the digital level */
typedef enum { BTN_LVL_UP, BTN_LVL_DOWN, BTN_LVL_ADC_BAD } btn_lvl_t;
btn_lvl_t btn_level_read(int pin_mv, bool dig_low, int batt_mv, int div_x100, int max_mv);

#endif
