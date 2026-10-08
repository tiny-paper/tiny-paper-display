/* hal_esp: link transport (USB Serial/JTAG, or UART0 for QEMU) and the text console.
 * Frames and "# " log lines share one byte stream (link-protocol.md); a mutex keeps a
 * log line from landing inside a frame. ESP-IDF's own log output is re-prefixed.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal_esp_priv.h"

#if CONFIG_TINY_LINK_UART0
#include "driver/uart.h"
#else
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "soc/soc.h"
#include "soc/usb_serial_jtag_reg.h"
#include "usb_detect.h"
#endif

static SemaphoreHandle_t s_mu;
static bool s_ready;
static TaskHandle_t s_core_task;
#if !CONFIG_TINY_LINK_UART0
static int64_t s_rx_us = -60000000; /* last bytes from the host (tr_read) */
#endif /* the thread that runs the core: its lines also go to hal_log_hook */

/* ------------------------------------------------------------------ transport */

static int tr_read(uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
#if CONFIG_TINY_LINK_UART0
    return uart_read_bytes(UART_NUM_0, buf, (uint32_t)cap, pdMS_TO_TICKS(timeout_ms));
#else
    int n = usb_serial_jtag_read_bytes(buf, (uint32_t)cap, pdMS_TO_TICKS(timeout_ms));
    if (n > 0)
        s_rx_us = esp_timer_get_time();
    return n;
#endif
}

static int tr_write(const void *buf, size_t len)
{
#if CONFIG_TINY_LINK_UART0
    return uart_write_bytes(UART_NUM_0, buf, len);
#else
    /* with no host attached the TX FIFO never drains: drop instead of blocking. A host that keeps the port closed
     * (web app "Disconnect" with the cable in: USB frames go on, nobody reads) does not drain it either: after one
     * write that timed out, the next ones drop at once until one goes through (each console line cost 200 ms, also
     * for the WiFi and lwIP tasks' log lines, PicPak 2026-09-26) */
    static bool stalled;
    /* a host that sent bytes in the last 30 s is there, whatever usb_serial_jtag_is_connected says: with t = 0 only the
     * 1 KiB TX buffer went out and longer frames were cut (the USB sync's ~1.1 KB check-in answer never arrived,
     * PicPak 2026-09-28) */
    bool host = usb_serial_jtag_is_connected() || esp_timer_get_time() - s_rx_us < 30000000;
    TickType_t t = host && !stalled ? pdMS_TO_TICKS(200) : 0;
    /* at most half the TX ring per write: a write larger than the whole ring (1 KiB) can never fit, the driver sends
     * nothing and the frame was lost (the USB sync's ~1.1 KB check-in answer, PicPak 2026-09-28); write_all loops */
    if (len > 512)
        len = 512;
    int n = usb_serial_jtag_write_bytes(buf, len, t);
    stalled = n <= 0 && len;
    return n;
#endif
}

#if !CONFIG_TINY_LINK_UART0
/* Host detection from the SOF frame counter (src/usb_detect.c, host-tested): does not depend on
 * ESP-IDF's SOF tick hook (usb_serial_jtag_is_connected), which reported "no host" on real hardware
 * (2026-09-24). */
static uint32_t ud_frame(void *ctx) { return REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG); }
static int64_t ud_now(void *ctx) { return esp_timer_get_time(); }
static void ud_delay(void *ctx, uint32_t us) { esp_rom_delay_us(us); }
static void ud_sleep(void *ctx, uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
/* the rest zero: usb_detect.h takes a zeroed state as "no host seen yet" (host test test_zero_state_battery_wake_has_no_host) */
static usbdet_t s_ud = {.frame = ud_frame, .now_us = ud_now, .delay_us = ud_delay, .sleep_ms = ud_sleep};
#endif

bool hal_link_host_present(void)
{
#if CONFIG_TINY_LINK_UART0
    return true;
#else
    /* After a reset (and after our own esp_restart in stay-awake mode) the host needs up to
     * about 1.5 s to enumerate the device again. Only a real deep-sleep timer wake, which only
     * happens on battery, gets the quick check. */
    bool quick = esp_reset_reason() == ESP_RST_DEEPSLEEP && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
    bool p = usbdet_present(&s_ud, quick);
    /* each change once: a host lost to a quiet bus (suspended by a PC in standby, a dock contact that bounced, a
     * cable pulled) reads the same in the log as a reset does not (a reset logs `wake reset=`); issues #133, #106 */
    static bool s_was;
    static int64_t s_lost_us;
    if (p != s_was) {
        s_was = p;
        int64_t now = esp_timer_get_time();
        if (!p) {
            s_lost_us = now;
            hal_logf("usb host lost: no SOF for %lld ms vbus=%d (a quiet or unplugged bus, not a reset)",
                     (long long)((now - s_ud.still) / 1000), hal_esp_vbus_state());
        } else if (s_lost_us) {
            hal_logf("usb host back: SOF again after %lld ms", (long long)((now - s_lost_us) / 1000));
        } else {
            hal_logf("usb host seen: SOF %lld ms after boot", (long long)(now / 1000));
        }
    }
    return p;
#endif
}

/* VBUS as the board senses it (the X: the charger's power-good pin): the one sign that tells a bus the host
 * suspended, or a dock contact that bounced, from a pulled cable; sys.c keeps the chip awake on it */
int hal_esp_vbus_state(void)
{
#if BOARD_VBUS_SENSE
    return hal_x_vbus() ? 1 : 0;
#else
    return -1;
#endif
}

bool hal_usb_power(void)
{
#if CONFIG_TINY_LINK_UART0
    return false;
#elif BOARD_BATT_GAUGE
    return hal_link_host_present() || hal_x_vbus(); /* X: a charger without a USB host counts too */
#else
    return hal_link_host_present();
#endif
}

void hal_link_flush(void)
{
    if (!s_ready)
        return;
#if CONFIG_TINY_LINK_UART0
    uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(100));
#else
    if (usb_serial_jtag_is_connected())
        usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(100));
#endif
}

/* ------------------------------------------------------------------ console */

void hal_console_write(const char *s, size_t len)
{
    if (!s_ready)
        return;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int n; len && (n = tr_write(s, len)) > 0; s += n, len -= (size_t)n) {
    }
    xSemaphoreGive(s_mu);
}

static void console_line(const char *s, size_t len)
{
    char b[224];
    size_t n = 0;
    b[n++] = '#';
    b[n++] = ' ';
    if (len >= 2 && s[0] == '#' && s[1] == ' ') {
        s += 2;
        len -= 2;
    }
    for (size_t i = 0; i < len && n < sizeof b - 1; i++) {
        unsigned char c = (unsigned char)s[i];
        b[n++] = (c < 0x20 || c > 0x7E) ? '?' : (char)c;
    }
    b[n++] = '\n';
    hal_console_write(b, n);
    if (s_core_task && xTaskGetCurrentTaskHandle() == s_core_task)
        hal_log_hook(b + 2, n - 3);
}

/* Formats and splits into "# " lines. */
static void console_vprintf(const char *fmt, va_list ap)
{
    char b[256];
    int n = vsnprintf(b, sizeof b, fmt, ap);
    if (n <= 0)
        return;
    size_t len = (size_t)n < sizeof b ? (size_t)n : sizeof b - 1;
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || b[i] == '\n') {
            size_t l = i - start;
            if (l && b[start + l - 1] == '\r')
                l--;
            if (l)
                console_line(b + start, l);
            start = i + 1;
        }
    }
}

void hal_logf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    console_vprintf(fmt, ap);
    va_end(ap);
}

static int idf_log_vprintf(const char *fmt, va_list ap)
{
    console_vprintf(fmt, ap);
    return 0;
}

/* printf, vprintf and fprintf are ESP-IDF's only (its log before hal_link_init, heap and interrupt dumps): wrapped
 * (CMakeLists.txt) to newlib's integer-only variants, so the full stream formatter (~10 KB of flash) is not linked.
 * vsnprintf (console_vprintf, the core) keeps floats. */
int __wrap_vprintf(const char *fmt, va_list ap) { return viprintf(fmt, ap); }

int __wrap_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = viprintf(fmt, ap);
    va_end(ap);
    return n;
}

int __wrap_fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfiprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

/* ------------------------------------------------------------------ link */

void hal_link_init(void)
{
    s_mu = xSemaphoreCreateMutex();
    s_core_task = xTaskGetCurrentTaskHandle(); /* hal_esp_init runs in the core's thread */
#if CONFIG_TINY_LINK_UART0
    uart_driver_install(UART_NUM_0, 4096, 2048, 0, NULL, 0);
#else
    usb_serial_jtag_driver_config_t c = {.rx_buffer_size = 4096, .tx_buffer_size = 1024};
    usb_serial_jtag_driver_install(&c);
    usb_serial_jtag_vfs_use_driver();
#endif
    s_ready = true;
    esp_log_set_vprintf(idf_log_vprintf);
}

/* USB is always open; BLE (config mode) lives in ble_link.c */
int hal_link_open(hal_link_t t) { return t == HAL_LINK_USB ? HAL_OK : t == HAL_LINK_BLE ? hal_ble_open() : HAL_E_NOTFOUND; }

void hal_link_close(hal_link_t t)
{
    if (t == HAL_LINK_BLE)
        hal_ble_close();
}

/* "!tp ..." text commands at the start of a USB read: BLE bring-up ones first, "!tp heap", the flash ones (PicPak),
 * then the panel's */
static bool textcmd(const uint8_t *buf, size_t len)
{
#if CONFIG_TINY_PHOTO_PART
    if (hal_esp_part_textcmd(buf, len))
        return true;
#endif
    return hal_ble_textcmd(buf, len) || hal_esp_sys_textcmd(buf, len) || hal_textcmd(buf, len);
}

long hal_link_read(hal_link_t t, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    if (t == HAL_LINK_BLE)
        return hal_ble_read(buf, cap, timeout_ms);
    if (t != HAL_LINK_USB || !s_ready)
        return t == HAL_LINK_USB ? 0 : HAL_E_NOTFOUND;
    uint16_t *slen, scap;
    uint8_t *st = hal_sys_stash(&slen, &scap);
    if (*slen) {
        size_t n = *slen < cap ? *slen : cap;
        memcpy(buf, st, n);
        memmove(st, st + n, *slen - n);
        *slen = (uint16_t)(*slen - n);
        return (long)n;
    }
    int n = tr_read(buf, cap, timeout_ms);
    if (n > 0 && textcmd(buf, (size_t)n))
        return 0;
    return n < 0 ? HAL_E_IO : n;
}

long hal_link_write(hal_link_t t, const uint8_t *buf, size_t len)
{
    if (t == HAL_LINK_BLE)
        return hal_ble_write(buf, len);
    if (t != HAL_LINK_USB || !s_ready)
        return HAL_E_NOTFOUND;
    /* all of it under one lock (tr_write takes 512 bytes a call): a frame in two writes would let a log line of
     * another task land inside it. Stops when the link takes nothing (no host). */
    xSemaphoreTake(s_mu, portMAX_DELAY);
    size_t done = 0;
    int n = 0;
    while (done < len && (n = tr_write(buf + done, len - done)) > 0)
        done += (size_t)n;
    xSemaphoreGive(s_mu);
    return done ? (long)done : n < 0 ? HAL_E_IO : 0;
}

bool hal_link_stash_wait(uint32_t timeout_ms)
{
    uint16_t *slen, scap;
    uint8_t *st = hal_sys_stash(&slen, &scap);
    int n = tr_read(st + *slen, scap - *slen, timeout_ms);
    if (n <= 0)
        return false;
    *slen = (uint16_t)(*slen + n);
    /* take the rest of a burst (a whole small frame) */
    while (*slen < scap && (n = tr_read(st + *slen, scap - *slen, 30)) > 0)
        *slen = (uint16_t)(*slen + n);
    if (textcmd(st, *slen)) {
        *slen = 0;
        return false;
    }
    return true;
}
