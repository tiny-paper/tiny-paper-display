/* bt.h - the display firmware's Bluetooth on the device (bt.c). SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "usbdisp.h"

/* Bind cfg's bt_* calls to the HAL's BLE link; the name is tp-display- and 4 hex digits of the chip's id. */
void display_bt_cfg(usbdisp_cfg_t *cfg);
