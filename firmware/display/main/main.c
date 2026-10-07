/*
 * main.c - ESP-IDF entry of the display firmware (docs/usb-display.md): the board as a plain display driven over
 * the USB cable. One job: answer the USB display protocol (components/usbdisp).
 *
 * Power: with a USB host attached the chip stays awake (the C3's USB Serial/JTAG is gone in any sleep), so it
 * answers at once. Without one it deep-sleeps, the panel keeps its picture, and it wakes every DISPLAY_LOOK_S
 * seconds (or by the button) to look for a host: plugged in again it answers hello within that time, no reset
 * button. A deep-sleep wake is a fresh boot, so a transfer the cable cut leaves nothing behind.
 * No radio: the WiFi stack is not in the image (sdkconfig.defaults, tools/check_no_radio.py).
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "board.h"
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
    usbdisp_init(&D, &cfg);
    uint64_t up = hal_rtc_us();
    for (;;) {
        usbdisp_poll(&D, 100);
        if (usbdisp_transfer(&D) || hal_usb_power())
            continue; /* a host is there: awake and reachable */
        if (hal_rtc_us() - up < DISPLAY_ENUM_MS * 1000ull)
            continue;
        usbdisp_reset(&D);
        /* no host: deep sleep (does not return); with one appearing meanwhile it returns on its first bytes */
        hal_sleep_until(hal_rtc_us() + DISPLAY_LOOK_S * 1000000ull, true);
        up = hal_rtc_us();
    }
}
