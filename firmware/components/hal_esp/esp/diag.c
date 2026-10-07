/* hal_esp: link diag (tiny/hal.h hal_diag): chip, resets since power-on, the last panic, NVS, partitions, heap map.
 * The panic record: esp_panic_handler is wrapped (-Wl,--wrap, CMakeLists.txt) and writes RTC memory before the
 * normal panic output and reset. SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_private/panic_internal.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_esp_priv.h"
#include "nvs.h"
#include "reset_report.h"
#if CONFIG_IDF_TARGET_ARCH_XTENSA
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "xtensa_context.h"
#else
#include "riscv/rvruntime-frames.h"
#endif

#define DIAG_MAGIC 0x54504447u /* "TPDG" */

/* Survives software, panic and watchdog resets and deep sleep (not power loss); cleared at power-on. */
typedef struct {
    uint32_t magic;
    uint8_t n;
    bool panic, panic_new;       /* panic_new: its boot time not stamped yet */
    uint8_t n_bt;
    struct {
        char r[12];
        char step[40];
        uint32_t s;
    } resets[HAL_DIAG_RESETS];
    char step[40];               /* the core step running now (hal_diag_step) */
    char reason[48], task[16];
    uint32_t pc, ra, s;
    uint32_t bt[HAL_DIAG_BT];
} diag_rtc_t;

static RTC_NOINIT_ATTR diag_rtc_t D;
static char s_step[40]; /* this boot's reset_step */
static reset_report_t s_reset_report;

static void d_init(bool force)
{
    if (force || D.magic != DIAG_MAGIC) {
        memset(&D, 0, sizeof D);
        D.magic = DIAG_MAGIC;
    }
}

void hal_esp_diag_boot(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    d_init(r == ESP_RST_POWERON);
    D.step[sizeof D.step - 1] = 0;
    bool interrupted = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
                       r == ESP_RST_BROWNOUT || r == ESP_RST_USB || r == ESP_RST_EXT;
    const char *radio = hal_esp_reset_step();
    snprintf(s_step, sizeof s_step, "%s", radio[0] ? radio : interrupted ? D.step : "");
    reset_report_boot(&s_reset_report, hal_esp_reset_name(), s_step);
    D.step[0] = 0;
    uint32_t now_s = (uint32_t)(hal_stats.boot_rtc_us / 1000000u);
    if (D.panic_new) {
        D.s = now_s;
        D.panic_new = false;
    }
    if (r == ESP_RST_DEEPSLEEP)
        return; /* every minute: not a reset worth keeping */
    memmove(&D.resets[1], &D.resets[0], sizeof D.resets[0] * (HAL_DIAG_RESETS - 1));
    snprintf(D.resets[0].r, sizeof D.resets[0].r, "%s", hal_esp_reset_name());
    snprintf(D.resets[0].step, sizeof D.resets[0].step, "%s", s_step);
    D.resets[0].s = now_s;
    if (D.n < HAL_DIAG_RESETS)
        D.n++;
}

void hal_esp_diag_boot_logged(void) { reset_report_logged(&s_reset_report); }
const char *hal_esp_diag_wake_reset(void) { return reset_report_summary(&s_reset_report); }
void hal_esp_diag_cycle_done(void) { reset_report_cycle_done(&s_reset_report); }

void hal_diag_step(const char *step)
{
    d_init(false);
    snprintf(D.step, sizeof D.step, "%s", step ? step : "");
}

/* ------------------------------------------------------------------ panic record */

void __real_esp_panic_handler(panic_info_t *info);

void __wrap_esp_panic_handler(panic_info_t *info)
{
    d_init(false);
    D.panic = D.panic_new = true;
    const char *why = g_panic_abort && g_panic_abort_details ? g_panic_abort_details : info->reason;
    snprintf(D.reason, sizeof D.reason, "%s", why ? why : "?");
    const char *t = xTaskGetCurrentTaskHandle() ? pcTaskGetName(NULL) : NULL;
    snprintf(D.task, sizeof D.task, "%s", t ? t : "");
    D.pc = (uint32_t)(uintptr_t)info->addr;
    D.ra = 0;
    D.n_bt = 0;
    if (info->frame) {
#if CONFIG_IDF_TARGET_ARCH_XTENSA
        const XtExcFrame *f = info->frame;
        D.ra = f->a0;
        esp_backtrace_frame_t bf = {.pc = f->pc, .sp = f->a1, .next_pc = f->a0, .exc_frame = f};
        while (D.n_bt < HAL_DIAG_BT) {
            D.bt[D.n_bt++] = esp_cpu_process_stack_pc(bf.pc);
            if (!bf.next_pc || !esp_backtrace_get_next_frame(&bf))
                break;
        }
#else
        const RvExcFrame *f = info->frame;
        D.ra = f->ra;
#endif
    }
    __real_esp_panic_handler(info);
}

/* ------------------------------------------------------------------ hal_diag */

__attribute__((weak)) void hal_esp_board_diag(char *out, size_t cap)
{
    if (cap)
        out[0] = 0;
}

static const char *chip_name(esp_chip_model_t m)
{
    switch (m) {
    case CHIP_ESP32: return "esp32";
    case CHIP_ESP32S2: return "esp32s2";
    case CHIP_ESP32S3: return "esp32s3";
    case CHIP_ESP32C3: return "esp32c3";
    default: return "esp32?";
    }
}

void hal_diag(hal_diag_t *d)
{
    d_init(false);
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    snprintf(d->chip, sizeof d->chip, "%s rev %u.%u, %u MB flash%s", chip_name(ci.model), ci.revision / 100,
             ci.revision % 100, (unsigned)(flash >> 20), heap_caps_get_total_size(MALLOC_CAP_SPIRAM) ? ", PSRAM" : "");
    snprintf(d->sdk, sizeof d->sdk, "%s", esp_get_idf_version());
    d->uptime_s = (uint32_t)((hal_rtc_us() - hal_stats.boot_rtc_us) / 1000000u);
    snprintf(d->reset, sizeof d->reset, "%s", s_reset_report.reason);
    snprintf(d->reset_step, sizeof d->reset_step, "%s", s_reset_report.step);
    d->crashes = hal_esp_crash_count();
    d->n_resets = D.n;
    for (int i = 0; i < D.n && i < HAL_DIAG_RESETS; i++) {
        memcpy(d->resets[i].r, D.resets[i].r, sizeof d->resets[i].r);
        memcpy(d->resets[i].step, D.resets[i].step, sizeof d->resets[i].step);
        d->resets[i].r[sizeof d->resets[i].r - 1] = d->resets[i].step[sizeof d->resets[i].step - 1] = 0;
        d->resets[i].s = D.resets[i].s;
    }
    if (D.panic) {
        d->panic = true;
        snprintf(d->panic_reason, sizeof d->panic_reason, "%.*s", (int)sizeof D.reason, D.reason);
        snprintf(d->panic_task, sizeof d->panic_task, "%.*s", (int)sizeof D.task, D.task);
        d->panic_pc = D.pc;
        d->panic_ra = D.ra;
        d->panic_s = D.s;
        d->n_bt = D.n_bt <= HAL_DIAG_BT ? D.n_bt : HAL_DIAG_BT;
        memcpy(d->bt, D.bt, sizeof d->bt[0] * d->n_bt);
    }
    d->heap_min = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    for (int i = 0; i < 4 && i < HAL_ESP_PH_COUNT; i++)
        d->phase_min[i] = hal_stats.heap_free[i];
    nvs_stats_t ns;
    if (nvs_get_stats(NULL, &ns) == ESP_OK) {
        d->nvs = true;
        d->nvs_used = (uint32_t)ns.used_entries;
        d->nvs_free = (uint32_t)ns.free_entries;
        d->nvs_total = (uint32_t)ns.total_entries;
        d->nvs_ns = (uint32_t)ns.namespace_count;
    }
    const esp_partition_t *run = esp_ota_get_running_partition(), *boot = esp_ota_get_boot_partition();
    snprintf(d->running, sizeof d->running, "%s", run ? run->label : "");
    snprintf(d->boot, sizeof d->boot, "%s", boot ? boot->label : "");
    d->slot = hal_ota_slot_size();
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it && d->n_parts < HAL_DIAG_PARTS; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        snprintf(d->parts[d->n_parts].label, sizeof d->parts[0].label, "%s", p->label);
        d->parts[d->n_parts].type = (uint8_t)p->type;
        d->parts[d->n_parts].subtype = (uint8_t)p->subtype;
        d->parts[d->n_parts].off = p->address;
        d->parts[d->n_parts].size = p->size;
        d->n_parts++;
    }
    esp_partition_iterator_release(it);
    snprintf(d->refresh, sizeof d->refresh, "%s",
             hal_stats.refresh_kind == 1 ? "full" : hal_stats.refresh_kind == 2 ? "partial" : "none");
    d->refresh_ms = hal_stats.refresh_ms;
    hal_esp_board_diag(d->extra, sizeof d->extra);
    hal_esp_heap_blocks(d);
}
