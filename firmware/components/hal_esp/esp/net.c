/* hal_esp: WiFi (station), SNTP (one raw UDP exchange), HTTP(S) via esp_http_client and
 * mbedTLS with the certificate bundle. WiFi is initialised only when a session needs it
 * and fully deinitialised by hal_wifi_off. TRMNL X: the ESP32-C5 modem as a second radio for 5 GHz (esp/net_c5.c),
 * SNTP and HTTP(S) over its socket with TLS here (docs/design/x-5ghz.md). SPDX-License-Identifier: GPL-3.0-or-later */
#include "tiny/busy.h"
#include "sdkconfig.h"
#include "hal_esp_priv.h"
#include "url_host.h"
#include "tiny/hal.h"
int hal_wifi_connect(const hal_wifi_cfg_t *cfg, uint32_t timeout_ms, hal_wifi_info_t *out) { return HAL_E_IO; }
void hal_wifi_off(void) {}
int hal_wifi_scan(hal_ap_t *out, size_t max, size_t *count) { *count = 0; return HAL_E_IO; }
int hal_sntp(int64_t *unix_ms, uint32_t timeout_ms) { return HAL_E_IO; }
int hal_http(const hal_http_req_t *req, hal_http_resp_t *resp) { return HAL_E_IO; }
int hal_http_open(const hal_http_req_t *req, hal_http_resp_t *resp) { return HAL_E_IO; }
long hal_http_read(uint8_t *buf, size_t cap) { return HAL_E_IO; }
void hal_http_close(void) {}
int hal_ap_start(const hal_ap_cfg_t *cfg) { return HAL_E_NOTFOUND; }
void hal_ap_stop(void) {}
int hal_ap_request(hal_ap_req_t *req, uint32_t timeout_ms) { return HAL_E_IO; }
int hal_ap_respond(int status, const char *type, const char *headers, const void *body, size_t len) { return HAL_E_IO; }
int hal_ap_clients(void) { return 0; }
void hal_esp_net_init(void) {}
bool hal_esp_wifi_on(void) { return false; }
