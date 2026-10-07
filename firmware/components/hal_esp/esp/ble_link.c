/* hal_esp: the BLE link transport of config mode (link-protocol.md "Bluetooth"), NimBLE peripheral.
 * One GATT service: RX (write, host to device, into a ring), TX (notify, device to host, MTU-3 pieces),
 * Pair (read, needs an authenticated encrypted link: the host reads it to make its OS pair).
 * LE Secure Connections only, passkey entry (DisplayOnly, MITM), bonding. The whole stack (host and
 * controller) is brought up by hal_link_open(HAL_LINK_BLE) and torn down and freed by hal_link_close.
 * Callbacks run in the NimBLE host task; the app reads and writes from its own task (one reader, one writer).
 * Without CONFIG_TINY_BLE (every board by default, QEMU) everything answers "no BLE" and NimBLE is not in the image.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>

#include "ble_heap.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal_esp_priv.h"
#include "nvs.h"


void hal_link_ble_setup(const char *name, uint32_t passkey)
{
    (void)name;
    (void)passkey;
}
int hal_ble_open(void) { return HAL_E_NOTFOUND; }
void hal_ble_close(void) {}
long hal_ble_read(uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    (void)buf;
    (void)cap;
    (void)timeout_ms;
    return HAL_E_NOTFOUND;
}
long hal_ble_write(const uint8_t *buf, size_t len)
{
    (void)buf;
    (void)len;
    return HAL_E_NOTFOUND;
}
void hal_link_ble_state(hal_ble_state_t *out) { memset(out, 0, sizeof *out); }
bool hal_link_ble_secure(void) { return false; }
void hal_link_ble_forget(void)
{
    nvs_handle_t h;
    if (nvs_open("nimble_bond", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}


/* "!tp ble open|close|state" (bring-up and QEMU: the stack alone, without the app's config mode). */
bool hal_ble_textcmd(const uint8_t *buf, size_t len)
{
    if (len < 7 || memcmp(buf, "!tp ble", 7))
        return false;
    char a[16] = "";
    size_t i = 7, k = 0;
    while (i < len && buf[i] == ' ')
        i++;
    while (i < len && k < sizeof a - 1 && buf[i] > ' ')
        a[k++] = (char)buf[i++];
    if (!strcmp(a, "open")) {
        hal_link_ble_setup("tiny-paper-test", 123456);
        hal_logf("ble textcmd open rc=%d", hal_ble_open());
    } else if (!strcmp(a, "close")) {
        hal_ble_close();
    } else {
        hal_ble_state_t s;
        hal_link_ble_state(&s);
        hal_logf("ble state open=%d conn=%d secure=%d mtu=%u fails=%u connects=%u heap_min=%u", s.open, s.connected,
                 s.secure, s.mtu, s.pair_fails, (unsigned)s.connects, (unsigned)s.heap_min);
    }
    return true;
}
