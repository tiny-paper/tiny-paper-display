/*
 * fs_safe.h - the two storage decisions that can lose a device's data (esp/fs.c), with the platform calls passed
 * in. Hardware-free, host-tested with injected errors (test/test_fs_safe.c). Issue #54, audit 2026-10-04:
 * - rename: the old code removed the destination and tried again on every error but ENOENT, so a rename that
 *   failed on a full or failing disk lost the good file (active slot marker, meta.cbor, settings, secrets).
 * - mount: the old code formatted on every failed mount, so one bad mount (corrupt block, read glitch, power dip)
 *   erased plugins, settings and secrets as if it were a first boot.
 * firmware.md "Storage recovery", store.md "Atomic writes".
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_FS_SAFE_H
#define TINY_FS_SAFE_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "tiny/hal.h"

/* ---- rename ---- */

struct fss_ren_ops {
    int (*rename)(void *ctx, const char *from, const char *to); /* 0, or the errno */
    int (*unlink)(void *ctx, const char *path);                 /* 0, or the errno */
    bool (*exists)(void *ctx, const char *path);
    void *ctx;
};

static inline int fss_errno_rc(int e)
{
    return e == 0 ? HAL_OK : e == ENOENT ? HAL_E_NOTFOUND : e == ENOSPC ? HAL_E_NOSPACE : HAL_E_IO;
}

/* hal_fs_rename. A failure leaves `to` as it was and `from` in place (the caller removes its .tmp). The
 * destination is removed in one case only: the platform refuses to replace (EEXIST; LittleFS never does, it
 * renames over an existing file itself) and the source is there. */
static inline int fss_rename(const struct fss_ren_ops *o, const char *from, const char *to)
{
    int e = o->rename(o->ctx, from, to);
    if (e == EEXIST && o->exists(o->ctx, from)) {
        e = o->unlink(o->ctx, to);
        if (e == 0 || e == ENOENT)
            e = o->rename(o->ctx, from, to);
    }
    return fss_errno_rc(e);
}

/* ---- mount ---- */

enum { FSS_MNT_OK = 0, FSS_MNT_NOFS = 1, FSS_MNT_ERR = 2 }; /* NOFS: the filesystem's own "cannot mount this" */
enum { FSS_PROBE_ERASED = 0, FSS_PROBE_FOREIGN = 1, FSS_PROBE_LFS = 2, FSS_PROBE_ERR = 3 };
/* The mark in NVS ("fsmk"): was this storage ever ours, is a format under way */
enum { FSS_MARK_NEVER = 0, FSS_MARK_FORMATTING = 1, FSS_MARK_MOUNTED = 2 };
enum { FSS_OK = 0, FSS_FIRST_BOOT = 1, FSS_RECOVERY = 2 };

#define FSS_MOUNT_TRIES 3
#define FSS_RETRY_MS 300u

/* A LittleFS superblock block: "littlefs" at offset 8 */
static inline bool fss_lfs_magic(const uint8_t *blk, size_t n) { return n >= 16 && !memcmp(blk + 8, "littlefs", 8); }

static inline bool fss_all_ff(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (b[i] != 0xFF)
            return false;
    return true;
}

/* The two superblock blocks, read whole: either holds the magic, both erased, else something else's data */
static inline int fss_probe_class(bool read_ok, bool magic, bool erased)
{
    return !read_ok ? FSS_PROBE_ERR : magic ? FSS_PROBE_LFS : erased ? FSS_PROBE_ERASED : FSS_PROBE_FOREIGN;
}

/* May this failed mount be answered with a format? Only when no data of ours can be there:
 * a format of ours was cut short, the partition is erased, or it never held our filesystem and holds none now. */
static inline bool fss_may_format(int mount, int mark, int probe)
{
    if (mount != FSS_MNT_NOFS)
        return false;
    if (mark == FSS_MARK_FORMATTING)
        return true;
    return probe == FSS_PROBE_ERASED || (probe == FSS_PROBE_FOREIGN && mark == FSS_MARK_NEVER);
}

struct fss_mnt_ops {
    int (*mount)(void *ctx);            /* FSS_MNT_* */
    int (*probe)(void *ctx);            /* FSS_PROBE_* */
    int (*format)(void *ctx);           /* 0 ok */
    int (*mark_get)(void *ctx);         /* FSS_MARK_*; unreadable: FSS_MARK_NEVER */
    void (*mark_set)(void *ctx, int m);
    void (*first_boot)(void *ctx);      /* the format window opens (sys.c hal_esp_first_boot_begin) */
    void (*delay_ms)(void *ctx, unsigned ms);
    void *ctx;
};

/* hal_fs_init. FSS_OK mounted; FSS_FIRST_BOOT formatted (or a cut format finished) and mounted; FSS_RECOVERY not
 * mounted and nothing erased. *tries: mounts attempted. */
static inline int fss_mount(const struct fss_mnt_ops *o, int *tries)
{
    for (int t = 0; t < FSS_MOUNT_TRIES; t++) {
        if (t)
            o->delay_ms(o->ctx, FSS_RETRY_MS);
        if (tries)
            *tries = t + 1;
        int m = o->mount(o->ctx);
        int mark = o->mark_get(o->ctx);
        if (m == FSS_MNT_OK) {
            /* power was lost after a format and before its mark: still the first wake cycle */
            if (mark == FSS_MARK_FORMATTING)
                o->first_boot(o->ctx);
            if (mark != FSS_MARK_MOUNTED)
                o->mark_set(o->ctx, FSS_MARK_MOUNTED);
            return mark == FSS_MARK_FORMATTING ? FSS_FIRST_BOOT : FSS_OK;
        }
        if (m != FSS_MNT_NOFS)
            continue;
        if (!fss_may_format(m, mark, mark == FSS_MARK_FORMATTING ? FSS_PROBE_ERR : o->probe(o->ctx)))
            continue;
        if (mark != FSS_MARK_FORMATTING)
            o->mark_set(o->ctx, FSS_MARK_FORMATTING);
        o->first_boot(o->ctx);
        if (o->format(o->ctx) == 0 && o->mount(o->ctx) == FSS_MNT_OK) {
            o->mark_set(o->ctx, FSS_MARK_MOUNTED);
            return FSS_FIRST_BOOT;
        }
    }
    return FSS_RECOVERY;
}

#endif
