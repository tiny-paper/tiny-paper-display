/*
 * bt.c - the display firmware's Bluetooth on the device (docs/spec/usb-display.md "Bluetooth"): usbdisp's bt_* calls
 * on the HAL's BLE link (hal_esp esp/ble_link.c). The only place of this firmware
 * that starts the Bluetooth stack is bt_open below, and usbdisp calls it only while the stored setting is on
 * (components/usbdisp/test/test_usbdisp_bt.c counts the calls; firmware/tools/check_no_radio.py checks the link).
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "bt.h"

#include <stdio.h>

#include "hal_esp.h"
#include "tiny/hal.h"

static char s_name[20];

static int bt_open(void *ctx, const char *name)
{
    (void)ctx;
    hal_link_ble_setup(name);
    return hal_link_open(HAL_LINK_BLE);
}

static void bt_close(void *ctx)
{
    (void)ctx;
    hal_link_close(HAL_LINK_BLE);
}

static long bt_read(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    (void)ctx;
    return hal_link_read(HAL_LINK_BLE, buf, cap, timeout_ms);
}

static long bt_write(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    return hal_link_write(HAL_LINK_BLE, buf, len);
}

/* the connection's number: connects since the stack started, new for every connection */
static uint32_t bt_connected(void *ctx)
{
    (void)ctx;
    hal_ble_state_t s;
    hal_link_ble_state(&s);
    return s.connected ? s.connects : 0;
}

static void bt_drop(void *ctx)
{
    (void)ctx;
    hal_link_ble_drop();
}

static void bt_forget(void *ctx)
{
    (void)ctx;
    hal_link_ble_forget();
}

/* esp_fill_random: the hardware RNG, true random while the radio runs (usbdisp asks only then) */
static void bt_random(void *ctx, void *buf, size_t n)
{
    (void)ctx;
    hal_random_bytes(buf, n);
}

void display_bt_cfg(usbdisp_cfg_t *cfg)
{
    uint8_t id[6];
    hal_chip_id(id);
    snprintf(s_name, sizeof s_name, "tp-display-%02X%02X", id[4], id[5]);
    cfg->bt_name = s_name;
    cfg->bt_open = bt_open;
    cfg->bt_close = bt_close;
    cfg->bt_read = bt_read;
    cfg->bt_write = bt_write;
    cfg->bt_connected = bt_connected;
    cfg->bt_drop = bt_drop;
    cfg->bt_forget = bt_forget;
    cfg->random = bt_random;
}
