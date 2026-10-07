/* hal_esp: flash filesystem, LittleFS (joltwallet/littlefs) on partition "storage",
 * mounted at /fs. hal paths are absolute inside it. SPDX-License-Identifier: GPL-3.0-or-later */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_littlefs.h"
#include "sdkconfig.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fs_safe.h"
#include "hal_esp_priv.h"

#define BASE "/fs"
#define LABEL "storage"
#define PATH_MAX_LEN 96
/* tiny/hal.h states what this build refuses (the simulator refuses the same, issue #183) */
_Static_assert(PATH_MAX_LEN == sizeof BASE + HAL_FS_PATH_MAX, "HAL_FS_PATH_MAX: BASE, the path, NUL");
_Static_assert(CONFIG_LITTLEFS_OBJ_NAME_LEN == HAL_FS_NAME_MAX + 1, "HAL_FS_NAME_MAX: the LittleFS name limit");
#define MAX_FILES 8

struct hal_file {
    int fd;
    bool used;
    char path[PATH_MAX_LEN];
    /* the write loop of this file (issues #114 #122), reported at close by fs_report */
    uint32_t wr_calls, wr_bytes, sync_calls;
    uint64_t wr_us, wr_worst_us, sync_us;
    hal_esp_flash_stats_t fl0; /* the flash counters at open */
};

/* A file's write loop at its close: the calls and their time, the syncs, and the flash operations behind them, so a
 * slow write on a device says its cost per block (erase_ms / erases: LittleFS erases each 4 KiB block it takes), per
 * program (prog_ms / progs) and in reads (thousands of them: the allocator's scan of the filesystem). Only a file
 * with FS_REPORT_BYTES written or FS_REPORT_US spent writing, syncing and closing: a small file logs nothing.
 * Fixed (issue #122, a 1161x1404 photo raster of 408 KiB written in 7.7 s): ms well under 2000 for that size. */
#define FS_REPORT_BYTES 32768u
#define FS_REPORT_US 300000u
static void fs_report(const struct hal_file *f, uint64_t close_us)
{
    if (f->wr_bytes < FS_REPORT_BYTES && f->wr_us + f->sync_us + close_us < FS_REPORT_US)
        return;
    const hal_esp_flash_stats_t *a = &f->fl0, *b = &hal_esp_flash_stats;
    hal_logf("I fs_file path=%s bytes=%u writes=%u ms=%u worst_ms=%u syncs=%u sync_ms=%u close_ms=%u erases=%u "
             "erase_ms=%u progs=%u prog_ms=%u reads=%u read_ms=%u",
             f->path, (unsigned)f->wr_bytes, (unsigned)f->wr_calls, (unsigned)(f->wr_us / 1000u),
             (unsigned)(f->wr_worst_us / 1000u), (unsigned)f->sync_calls, (unsigned)(f->sync_us / 1000u),
             (unsigned)(close_us / 1000u), (unsigned)(b->erases - a->erases),
             (unsigned)((b->erase_us - a->erase_us) / 1000u), (unsigned)(b->progs - a->progs),
             (unsigned)((b->prog_us - a->prog_us) / 1000u), (unsigned)(b->reads - a->reads),
             (unsigned)((b->read_us - a->read_us) / 1000u));
}

/* Flash erase/GC and VFS sync cannot yield safely. Pin any remaining stall to
 * the operation and file instead of attributing it to the whole install. */
static void fs_slow(const char *op, const char *path, int64_t start, size_t bytes)
{
    static bool reporting;
    int64_t us = esp_timer_get_time() - start;
    if (us > 2000000 && !reporting) {
        reporting = true;
        hal_logf("W slow step=fs_%s ms=%llu path=%s bytes=%u", op,
                 (unsigned long long)(us / 1000), path, (unsigned)bytes);
        reporting = false;
    }
}

/* A step that failed, with the platform's error (issue #183: "io" from an install did not say which step or why).
 * Not for a file that is merely missing. The log may itself write a file: never from inside its own report. */
static void fs_failed(const char *op, const char *path, int err)
{
    static bool reporting;
    if (reporting)
        return;
    reporting = true;
    hal_logf("W fs %s %s: errno=%d", op, path, err);
    reporting = false;
}

static struct hal_file s_files[MAX_FILES];
static bool s_mounted;

/* First boot (the partition holds no filesystem, e.g. old stock data on the PicPak): format here, not
 * inside the mount. Real PicPak 2026-09-24: a first boot ended in a watchdog reset (TG0WDT_SYS_RST,
 * reset=task_wdt crash=1) and the next one found the storage still unformatted. The format and the
 * rest of that first wake cycle (first render, code partition) run with the task watchdog relaxed,
 * and a reset in that window is not counted as a crash (sys.c hal_esp_first_boot_begin).
 * Any other failed mount is not a first boot (issue #54: it used to format, and one bad mount erased plugins,
 * settings and secrets): src/fs_safe.h decides, firmware.md "Storage recovery". */
#define KV_MARK "fsmk"
#define LFS_BLOCK 4096u

static esp_vfs_littlefs_conf_t s_conf = {
    .base_path = BASE,
    .partition_label = LABEL,
    .format_if_mount_failed = false,
    .dont_mount = false,
};
static esp_err_t s_mount_err;

static int m_mount(void *ctx)
{
    s_mount_err = esp_vfs_littlefs_register(&s_conf);
    /* esp_littlefs: ESP_FAIL is lfs_mount's refusal (no filesystem, or a corrupt one); others never reach it */
    return s_mount_err == ESP_OK ? FSS_MNT_OK : s_mount_err == ESP_FAIL ? FSS_MNT_NOFS : FSS_MNT_ERR;
}

/* LittleFS keeps its superblock in blocks 0 and 1 */
static int m_probe(void *ctx)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, LABEL);
    uint8_t b[256];
    bool ok = p != NULL, magic = false, erased = true;
    for (uint32_t off = 0; ok && off < 2 * LFS_BLOCK; off += sizeof b) {
        ok = esp_partition_read(p, off, b, sizeof b) == ESP_OK;
        if (ok && off % LFS_BLOCK == 0 && fss_lfs_magic(b, sizeof b))
            magic = true;
        if (ok && !fss_all_ff(b, sizeof b))
            erased = false;
    }
    return fss_probe_class(ok, magic, erased);
}

static int m_format(void *ctx)
{
    hal_logf("W littlefs: no filesystem on \"%s\", formatting (first boot)", LABEL);
    int64_t t = esp_timer_get_time();
    esp_err_t e = esp_littlefs_format(LABEL);
    hal_logf("I littlefs format %s ms=%lld", esp_err_to_name(e), (long long)((esp_timer_get_time() - t) / 1000));
    return e == ESP_OK ? 0 : -1;
}

static int m_mark_get(void *ctx)
{
    uint8_t m = FSS_MARK_NEVER;
    return hal_kv_get(KV_MARK, &m, 1) == 1 ? m : FSS_MARK_NEVER;
}

static void m_mark_set(void *ctx, int m)
{
    uint8_t v = (uint8_t)m;
    hal_kv_set(KV_MARK, &v, 1);
}

static void m_first_boot(void *ctx) { hal_esp_first_boot_begin(); }
static void m_delay(void *ctx, unsigned ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

void hal_fs_init(void)
{
    static const struct fss_mnt_ops ops = {m_mount, m_probe, m_format, m_mark_get, m_mark_set, m_first_boot, m_delay, NULL};
    int tries = 0;
    s_mounted = fss_mount(&ops, &tries) != FSS_RECOVERY;
    if (!s_mounted)
        hal_logf("E littlefs mount: %s tries=%d, storage recovery (nothing erased)", esp_err_to_name(s_mount_err), tries);
    else if (tries > 1)
        hal_logf("W littlefs mount: ok after tries=%d", tries);
}

hal_fs_state_t hal_fs_state(int *err)
{
    if (err)
        *err = s_mounted ? 0 : (int)s_mount_err;
    return s_mounted ? HAL_FS_OK : HAL_FS_RECOVERY;
}

static bool full(char *out, const char *path)
{
    if (!s_mounted || !path || path[0] != '/')
        return false;
    int n = snprintf(out, PATH_MAX_LEN, BASE "%s", path);
    return n > 0 && n < PATH_MAX_LEN;
}

hal_file_t *hal_fs_open(const char *path, const char *mode)
{
    char p[PATH_MAX_LEN];
    if (!full(p, path) || !mode)
        return NULL;
    int flags;
    if (!strcmp(mode, "r"))
        flags = O_RDONLY;
    else if (!strcmp(mode, "w"))
        flags = O_RDWR | O_CREAT | O_TRUNC;
    else if (!strcmp(mode, "r+"))
        flags = O_RDWR;
    else if (!strcmp(mode, "a"))
        flags = O_RDWR | O_CREAT | O_APPEND;
    else
        return NULL;
    struct hal_file *f = NULL;
    for (int i = 0; i < MAX_FILES; i++)
        if (!s_files[i].used) {
            f = &s_files[i];
            break;
        }
    if (!f)
        return NULL;
    int64_t start = esp_timer_get_time();
    int fd = open(p, flags, 0644);
    if (fd < 0) {
        int err = errno;
        fs_slow("open", path, start, 0);
        if ((flags & O_CREAT) || err != ENOENT)
            fs_failed("open", path, err);
        return NULL;
    }
    f->fd = fd;
    f->used = true;
    snprintf(f->path, sizeof f->path, "%s", path);
    f->wr_calls = f->wr_bytes = f->sync_calls = 0;
    f->wr_us = f->wr_worst_us = f->sync_us = 0;
    f->fl0 = hal_esp_flash_stats;
    fs_slow("open", path, start, 0);
    return f;
}

long hal_fs_read(hal_file_t *f, void *buf, size_t len)
{
    int64_t start = esp_timer_get_time();
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(f->fd, (uint8_t *)buf + got, len - got);
        if (n < 0) {
            fs_slow("read", f->path, start, len);
            return HAL_E_IO;
        }
        if (n == 0)
            break;
        got += (size_t)n;
    }
    fs_slow("read", f->path, start, len);
    return (long)got;
}

long hal_fs_write(hal_file_t *f, const void *buf, size_t len)
{
    int64_t start = esp_timer_get_time();
    size_t put = 0;
    while (put < len) {
        ssize_t n = write(f->fd, (const uint8_t *)buf + put, len - put);
        if (n <= 0) {
            int err = errno, rc = err == ENOSPC ? HAL_E_NOSPACE : HAL_E_IO;
            fs_slow("write", f->path, start, len);
            fs_failed("write", f->path, err);
            return rc;
        }
        put += (size_t)n;
    }
    fs_slow("write", f->path, start, len);
    uint64_t us = (uint64_t)(esp_timer_get_time() - start);
    f->wr_calls++;
    f->wr_bytes += (uint32_t)put;
    f->wr_us += us;
    if (us > f->wr_worst_us)
        f->wr_worst_us = us;
    return (long)put;
}

int hal_fs_seek(hal_file_t *f, long off) { return lseek(f->fd, off, SEEK_SET) < 0 ? HAL_E_IO : HAL_OK; }

long hal_fs_tell(hal_file_t *f)
{
    off_t o = lseek(f->fd, 0, SEEK_CUR);
    return o < 0 ? HAL_E_IO : (long)o;
}

int hal_fs_truncate(hal_file_t *f, long len) { return ftruncate(f->fd, len) ? HAL_E_IO : HAL_OK; }

int hal_fs_sync(hal_file_t *f)
{
    int64_t start = esp_timer_get_time();
    int rc = fsync(f->fd) ? HAL_E_IO : HAL_OK;
    fs_slow("sync", f->path, start, 0);
    f->sync_calls++;
    f->sync_us += (uint64_t)(esp_timer_get_time() - start);
    return rc;
}

int hal_fs_close(hal_file_t *f)
{
    if (!f || !f->used)
        return HAL_E_FAIL;
    int64_t start = esp_timer_get_time();
    int rc = fsync(f->fd), err = rc ? errno : 0;
    if (close(f->fd)) {
        rc = -1;
        err = err ? err : errno;
    }
    fs_slow("close", f->path, start, 0);
    if (rc)
        fs_failed("close", f->path, err);
    fs_report(f, (uint64_t)(esp_timer_get_time() - start));
    f->used = false;
    return rc ? HAL_E_IO : HAL_OK;
}

long hal_fs_size(const char *path)
{
    char p[PATH_MAX_LEN];
    struct stat st;
    if (!full(p, path) || stat(p, &st) || S_ISDIR(st.st_mode))
        return HAL_E_NOTFOUND;
    return (long)st.st_size;
}

static int r_rename(void *ctx, const char *a, const char *b) { return rename(a, b) ? errno : 0; }
static int r_unlink(void *ctx, const char *p) { return unlink(p) ? errno : 0; }
static bool r_exists(void *ctx, const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

int hal_fs_rename(const char *from, const char *to)
{
    char a[PATH_MAX_LEN], b[PATH_MAX_LEN];
    if (!full(a, from) || !full(b, to))
        return HAL_E_FAIL;
    int64_t start = esp_timer_get_time();
    /* LittleFS renames atomically over an existing file; a failure keeps `to` (src/fs_safe.h, issue #54) */
    static const struct fss_ren_ops ops = {r_rename, r_unlink, r_exists, NULL};
    int rc = fss_rename(&ops, a, b);
    if (rc != HAL_OK && rc != HAL_E_NOTFOUND)
        hal_logf("W fs rename %s: rc=%d", to, rc);
    fs_slow("rename", to, start, 0);
    return rc;
}

int hal_fs_remove(const char *path)
{
    char p[PATH_MAX_LEN];
    if (!full(p, path))
        return HAL_E_FAIL;
    int64_t start = esp_timer_get_time();
    int rc = HAL_OK;
    if (unlink(p) && rmdir(p))
        rc = errno == ENOENT ? HAL_E_NOTFOUND : HAL_E_IO;
    fs_slow("remove", path, start, 0);
    return rc;
}

int hal_fs_mkdir(const char *path)
{
    char p[PATH_MAX_LEN];
    if (!full(p, path))
        return HAL_E_FAIL;
    if (mkdir(p, 0755) == 0)
        return HAL_OK;
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode) ? HAL_OK : HAL_E_IO;
}

int hal_fs_list(const char *dir, hal_fs_list_fn fn, void *ctx)
{
    char p[PATH_MAX_LEN], f[PATH_MAX_LEN + 64];
    if (!full(p, dir))
        return HAL_E_FAIL;
    DIR *d = opendir(p);
    if (!d)
        return HAL_E_NOTFOUND;
    struct dirent *e;
    struct stat st;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type == DT_DIR) {
            fn(ctx, e->d_name, -1);
            continue;
        }
        int n = snprintf(f, sizeof f, "%s/%s", p, e->d_name);
        fn(ctx, e->d_name, n <= 0 || n >= (int)sizeof f || stat(f, &st) ? 0 : (long)st.st_size);
    }
    closedir(d);
    return HAL_OK;
}

int hal_fs_usage(uint32_t *total, uint32_t *used)
{
    size_t t = 0, u = 0;
    if (!s_mounted || esp_littlefs_info(LABEL, &t, &u) != ESP_OK)
        return HAL_E_IO;
    if (total)
        *total = (uint32_t)t;
    if (used)
        *used = (uint32_t)u;
    return HAL_OK;
}
