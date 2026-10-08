/* fresh_save.c: see fresh_save.h. SPDX-License-Identifier: GPL-3.0-or-later */
#include "fresh_save.h"

#include <stdlib.h>
#include <string.h>

#include "epd_parwave.h"
#include "tiny/hal.h"

long fresh_save_file(const uint8_t *f, size_t n, uint32_t tag, bool ram_only)
{
    if (ram_only)
        return 0;
    uint8_t *buf = f ? malloc(12 + epd_rle_bound(n)) : NULL;
    size_t l = buf ? epd_rle_enc(f, n, buf + 12, epd_rle_bound(n)) : 0;
    uint32_t hdr[3] = {FRESH_MAGIC, tag, (uint32_t)n};
    long saved = -1;
    if (l) {
        memcpy(buf, hdr, sizeof hdr);
        hal_file_t *fp = hal_fs_open(FRESH_TMP, "w");
        long w = fp ? hal_fs_write(fp, buf, 12 + l) : -1;
        int c = fp ? hal_fs_close(fp) : -1;
        if (w == (long)(12 + l) && c == HAL_OK && hal_fs_rename(FRESH_TMP, FRESH_PATH) == HAL_OK)
            saved = (long)(12 + l);
        else
            hal_fs_remove(FRESH_TMP);
    }
    free(buf);
    if (saved < 0)
        hal_fs_remove(FRESH_PATH); /* no stale bitmap */
    return saved;
}
