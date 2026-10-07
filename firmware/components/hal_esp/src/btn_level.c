/* Shared button / battery pin (btn_level.h). SPDX-License-Identifier: GPL-3.0-or-later */
#include "btn_level.h"

int btn_threshold_mv(int batt_mv, int div_x100, int max_mv)
{
    if (batt_mv <= 0 || div_x100 <= 0)
        return max_mv;
    int half = batt_mv * 100 / div_x100 / 2;
    return half < max_mv ? half : max_mv;
}

bool btn_pressed_mv(int pin_mv, int batt_mv, int div_x100, int max_mv)
{
    return pin_mv < btn_threshold_mv(batt_mv, div_x100, max_mv);
}

btn_lvl_t btn_level_read(int pin_mv, bool dig_low, int batt_mv, int div_x100, int max_mv)
{
    if (pin_mv < 0)
        return dig_low ? BTN_LVL_DOWN : BTN_LVL_UP;
    if (!btn_pressed_mv(pin_mv, batt_mv, div_x100, max_mv))
        return BTN_LVL_UP;
    return dig_low ? BTN_LVL_DOWN : BTN_LVL_ADC_BAD;
}

bool btn_digital_in_spec(int batt_mv, int div_x100)
{
    if (batt_mv <= 0 || div_x100 <= 0)
        return false;
    return batt_mv * 100 / div_x100 >= BTN_VDD_MV * BTN_VIH_PCT / 100;
}
