/*
 * stacks.c - the stack high-water mark of every FreeRTOS task, one log line each (CONFIG_TINY_STACK_REPORT).
 * Read by tools/idf/qemu_heap.py against the floors of tools/diag/qemu.floors.json: a task that came within the
 * floor of its stack's end fails the QEMU session before it overflows on a device.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "hal_esp_priv.h"

#ifdef CONFIG_TINY_STACK_REPORT
#include <string.h>

#include "esp_private/freertos_debug.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tiny/hal.h"

#define STACKS_MAX 24

#ifdef CONFIG_TINY_FAULT_SMALL_STACK
/* guard self-test: a task that leaves less than the floor of its stack unused; the session must end red */
static void fault_task(void *arg)
{
    (void)arg;
    for (;;)
        vTaskDelay(portMAX_DELAY);
}
#endif

void hal_esp_log_stacks(void)
{
#ifdef CONFIG_TINY_FAULT_SMALL_STACK
    static bool planted;
    if (!planted) {
        planted = true;
        xTaskCreate(fault_task, "faultstk", CONFIG_TINY_FAULT_SMALL_STACK_BYTES, NULL, 6, NULL);
        vTaskDelay(2); /* it runs once and blocks */
    }
#endif
    static struct { char name[configMAX_TASK_NAME_LEN]; unsigned free; } t[STACKS_MAX]; /* not on the caller's stack */
    int n = 0;
    bool more = false;
    vTaskSuspendAll(); /* the task lists must not change under the iterator */
    TaskIterator_t it = {0};
    while (xTaskGetNext(&it) != -1) {
        if (n == STACKS_MAX) {
            more = true;
            break;
        }
        if (!it.pxTaskHandle)
            continue; /* a broken entry: the count below shows it */
        snprintf(t[n].name, sizeof t[n].name, "%s", pcTaskGetName(it.pxTaskHandle));
        t[n].free = (unsigned)uxTaskGetStackHighWaterMark(it.pxTaskHandle); /* bytes on ESP-IDF */
        n++;
    }
    unsigned all = (unsigned)uxTaskGetNumberOfTasks();
    xTaskResumeAll();
    for (int i = 0; i < n; i++) {
        for (char *c = t[i].name; *c; c++)
            if (!((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z')))
                *c = '_'; /* one word for the log's key=value readers */
        hal_logf("stack task=%s free=%u", t[i].name, t[i].free);
    }
    if (more || (unsigned)n != all) /* a task not reported is a task not guarded: the session fails on this line */
        hal_logf("E stack report: %d of %u tasks", n, all);
}
#else
void hal_esp_log_stacks(void) {}
#endif
