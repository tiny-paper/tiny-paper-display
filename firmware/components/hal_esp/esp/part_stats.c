/* hal_esp: counters of the flash operations under the filesystem and the partitions (esp_partition_read, _write and
 * _erase_range through the linker's --wrap, CMakeLists.txt), so a slow file write on a device says what it cost per
 * block erase and per program (issues #114 #122; fs.c books them per file as "fs_file"). A wrapper adds two clock
 * reads to each operation. SPDX-License-Identifier: GPL-3.0-or-later */
#include "esp_partition.h"
#include "esp_timer.h"
#include "hal_esp_priv.h"

hal_esp_flash_stats_t hal_esp_flash_stats;

esp_err_t __real_esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t n);
esp_err_t __wrap_esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t n)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t e = __real_esp_partition_read(p, off, dst, n);
    hal_esp_flash_stats.read_us += (uint64_t)(esp_timer_get_time() - t0);
    hal_esp_flash_stats.reads++;
    return e;
}

esp_err_t __real_esp_partition_write(const esp_partition_t *p, size_t off, const void *src, size_t n);
esp_err_t __wrap_esp_partition_write(const esp_partition_t *p, size_t off, const void *src, size_t n)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t e = __real_esp_partition_write(p, off, src, n);
    hal_esp_flash_stats.prog_us += (uint64_t)(esp_timer_get_time() - t0);
    hal_esp_flash_stats.progs++;
    return e;
}

esp_err_t __real_esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t n);
esp_err_t __wrap_esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t n)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t e = __real_esp_partition_erase_range(p, off, n);
    hal_esp_flash_stats.erase_us += (uint64_t)(esp_timer_get_time() - t0);
    hal_esp_flash_stats.erases++;
    return e;
}
