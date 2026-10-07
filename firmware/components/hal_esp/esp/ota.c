/* hal_esp: firmware update slot (docs/spec/firmware.md) on ESP-IDF app_update, with the
 * bootloader's app rollback (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "hal_esp_priv.h"

static esp_ota_handle_t s_h;
static const esp_partition_t *s_part;

static const esp_partition_t *next_slot(void) { return esp_ota_get_next_update_partition(NULL); }

uint32_t hal_ota_slot_size(void)
{
    const esp_partition_t *p = next_slot();
    return p ? (uint32_t)p->size : 0;
}

int hal_ota_begin(uint32_t size)
{
    if (s_part)
        return HAL_E_FAIL;
    const esp_partition_t *p = next_slot();
    if (!p || size > p->size)
        return HAL_E_NOSPACE;
    /* sequential writes: sectors are erased as the image arrives, no long erase up front */
    esp_err_t e = esp_ota_begin(p, OTA_WITH_SEQUENTIAL_WRITES, &s_h);
    if (e != ESP_OK) {
        hal_logf("E ota begin err=0x%x", (unsigned)e);
        return HAL_E_IO;
    }
    s_part = p;
    return HAL_OK;
}

int hal_ota_write(const void *data, size_t len)
{
    if (!s_part)
        return HAL_E_FAIL;
    return esp_ota_write(s_h, data, len) == ESP_OK ? HAL_OK : HAL_E_IO;
}

int hal_ota_end(bool commit)
{
    if (!s_part)
        return HAL_E_FAIL;
    const esp_partition_t *p = s_part;
    s_part = NULL;
    if (!commit) {
        esp_ota_abort(s_h);
        return HAL_OK;
    }
    esp_err_t e = esp_ota_end(s_h); /* checks the image (format, chip, appended hash) */
    if (e == ESP_OK)
        e = esp_ota_set_boot_partition(p);
    if (e != ESP_OK) {
        hal_logf("E ota end err=0x%x", (unsigned)e);
        return HAL_E_IO;
    }
    return HAL_OK;
}

bool hal_ota_pending(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK)
        return false;
    /* NEW: booted by a bootloader without rollback (it never marks PENDING_VERIFY) */
    return st == ESP_OTA_IMG_PENDING_VERIFY || st == ESP_OTA_IMG_NEW;
}

int hal_ota_confirm(void) { return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK ? HAL_OK : HAL_E_IO; }

_Noreturn void hal_ota_rollback(void)
{
    hal_link_flush();
    /* marks the running image invalid and restarts into the previous one */
    esp_ota_mark_app_invalid_rollback_and_reboot();
    esp_restart(); /* no previous image: a plain restart */
}
