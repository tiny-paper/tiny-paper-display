/* hal_esp: key/value on NVS, namespace "tp" (store.md). SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>

#include "esp_mac.h"
#include "hal_esp_priv.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NS "tp"

static bool s_ok;

void hal_kv_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        hal_logf("W nvs: erasing (%s)", esp_err_to_name(e));
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    s_ok = e == ESP_OK;
    if (!s_ok)
        hal_logf("E nvs init: %s", esp_err_to_name(e));
}

static int open_ns(nvs_open_mode_t m, nvs_handle_t *h)
{
    if (!s_ok)
        return HAL_E_IO;
    esp_err_t e = nvs_open(NS, m, h);
    if (e == ESP_ERR_NVS_NOT_FOUND)
        return HAL_E_NOTFOUND;
    return e == ESP_OK ? HAL_OK : HAL_E_IO;
}

int hal_kv_get(const char *key, void *buf, size_t cap)
{
    nvs_handle_t h;
    int rc = open_ns(NVS_READONLY, &h);
    if (rc)
        return rc;
    size_t len = 0;
    esp_err_t e = nvs_get_blob(h, key, NULL, &len);
    if (e == ESP_OK && len <= cap && len)
        e = nvs_get_blob(h, key, buf, &len);
    nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_FOUND)
        return HAL_E_NOTFOUND;
    return e == ESP_OK ? (int)len : HAL_E_IO;
}

int hal_kv_set(const char *key, const void *buf, size_t len)
{
    nvs_handle_t h;
    int rc = open_ns(NVS_READWRITE, &h);
    if (rc)
        return rc;
    esp_err_t e = nvs_set_blob(h, key, buf, len);
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_ENOUGH_SPACE)
        return HAL_E_NOSPACE;
    return e == ESP_OK ? HAL_OK : HAL_E_IO;
}

int hal_kv_erase(const char *key)
{
    nvs_handle_t h;
    int rc = open_ns(NVS_READWRITE, &h);
    if (rc)
        return rc == HAL_E_NOTFOUND ? HAL_OK : rc;
    esp_err_t e = nvs_erase_key(h, key);
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND ? HAL_OK : HAL_E_IO;
}

/* Identity record: its own namespace, never erased by hal_kv_erase_all (store.md "Identity"). */
#define NS_ID "tpid"

int hal_id_get(void *buf, size_t cap)
{
    if (!s_ok)
        return HAL_E_IO;
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS_ID, NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND)
        return HAL_E_NOTFOUND;
    if (e != ESP_OK)
        return HAL_E_IO;
    size_t len = 0;
    e = nvs_get_blob(h, "id", NULL, &len);
    if (e == ESP_OK && len <= cap && len)
        e = nvs_get_blob(h, "id", buf, &len);
    nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_FOUND)
        return HAL_E_NOTFOUND;
    return e == ESP_OK ? (int)len : HAL_E_IO;
}

int hal_id_set(const void *buf, size_t len)
{
    if (!s_ok)
        return HAL_E_IO;
    nvs_handle_t h;
    if (nvs_open(NS_ID, NVS_READWRITE, &h) != ESP_OK)
        return HAL_E_IO;
    esp_err_t e = nvs_set_blob(h, "id", buf, len);
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? HAL_OK : HAL_E_IO;
}

void hal_chip_id(uint8_t out[6])
{
    if (esp_efuse_mac_get_default(out) != ESP_OK)
        memset(out, 0, 6);
}

int hal_kv_erase_all(void)
{
    nvs_handle_t h;
    int rc = open_ns(NVS_READWRITE, &h);
    if (rc)
        return rc == HAL_E_NOTFOUND ? HAL_OK : rc;
    esp_err_t e = nvs_erase_all(h);
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? HAL_OK : HAL_E_IO;
}
