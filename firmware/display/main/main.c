/*
 * main.c - ESP-IDF entry of the display firmware (docs/usb-display.md): the board as a plain display driven over
 * the USB cable. One job: answer the USB display protocol (components/usbdisp).
 *
 * Power: with a USB host attached the chip stays awake (the C3's USB Serial/JTAG is gone in any sleep), so it
 * answers at once. Without one it deep-sleeps, the panel keeps its picture, and it wakes every DISPLAY_LOOK_S
 * seconds (or by the button) to look for a host: plugged in again it answers hello within that time, no reset
 * button. A deep-sleep wake is a fresh boot, so a transfer the cable cut leaves nothing behind.
 * No WiFi and no network: neither stack is in the image (sdkconfig.defaults, tools/check_no_radio.py). Bluetooth
 * (bt.c) is in the image and stays off until the stored setting is on: then each look also advertises for
 * DISPLAY_BT_ADV_MS, a connected host or an open pairing window keeps the device up, and a button press opens the
 * pairing window. Off, this loop does what it did before Bluetooth was added.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "board.h"
#include "bt.h"
#include "hal_esp.h"
#include "tiny/hal.h"
#include "usbdisp.h"

#ifndef TINY_FW_VERSION
#define TINY_FW_VERSION "0.0.0"
#endif
/* Unplugged: seconds asleep between two looks for a USB host */
#define DISPLAY_LOOK_S 20
/* A timer wake finds the host only once it has enumerated the freshly woken chip: stay up this long for it */
#define DISPLAY_ENUM_MS 600
/* Bluetooth on: each look advertises this long (every 100 ms), time for a scanning host to see it and connect */
#define DISPLAY_BT_ADV_MS 2000
/* A full refresh that returns sooner was not driven (the SPI panels wait on BUSY for seconds); the TRMNL X times its
 * own waveform, so its time says nothing */
#if BOARD_PANEL_FASTEPD
#define DISPLAY_FAST_MS 0
#else
#define DISPLAY_FAST_MS 300
#endif

static usbdisp_t D;

/* The HAL's waits (a panel refresh): hello is answered, any other request gets BUSY */
void hal_busy_hook(void) { usbdisp_busy(&D); }

void app_main(void)
{
    static usbdisp_cfg_t cfg;
    hal_esp_init();
    usbdisp_hal_cfg(&cfg, TINY_FW_VERSION);
    cfg.fast_ms = DISPLAY_FAST_MS;
    display_bt_cfg(&cfg);
    usbdisp_init(&D, &cfg);
    uint64_t up = hal_rtc_us();
    bool pressed = usbdisp_bt_on(&D) && hal_wake_cause() == HAL_WAKE_BUTTON, was_down = pressed;
    for (;;) {
        usbdisp_poll(&D, 100);
        if (usbdisp_bt_on(&D)) { /* a press (the one that woke the device too) opens the pairing window */
            uint64_t t0, t1;
            bool down = hal_button_down();
            pressed |= (down && !was_down) || hal_button_take(&t0, &t1);
            was_down = down;
            if (pressed)
                usbdisp_bt_button(&D);
            pressed = false;
        }
        if (usbdisp_transfer(&D) || hal_usb_power() || usbdisp_bt_awake(&D))
            continue; /* a host is there: awake and reachable */
        if (hal_rtc_us() - up < (usbdisp_bt_on(&D) ? DISPLAY_BT_ADV_MS : DISPLAY_ENUM_MS) * 1000ull)
            continue;
        usbdisp_bt_stop(&D);
        usbdisp_reset(&D);
        /* no host: deep sleep (does not return); with one appearing meanwhile it returns on its first bytes */
        hal_sleep_until(hal_rtc_us() + DISPLAY_LOOK_S * 1000000ull, true);
        up = hal_rtc_us();
    }
}
