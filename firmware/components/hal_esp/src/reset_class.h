/*
 * reset_class.h - what a reset means for the next boot (esp/sys.c). A watchdog reset (task, interrupt or
 * RTC/timer group) while the radio starts is a supply sag (PicPak: TG1WDT_SYS_RST right after portal_before,
 * 2026-09-25; tesserae main.c treats INT_WDT/TASK_WDT/WDT like a brownout): a power fault like a brownout, not a
 * crash (no crash count toward safe mode), and the next boot lets the supply settle before the radio and panel.
 * Hardware-free, host-tested. C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_RESET_CLASS_H
#define TINY_RESET_CLASS_H

#include <stdbool.h>

typedef enum { RST_KIND_OTHER, RST_KIND_PANIC, RST_KIND_WDT, RST_KIND_BROWNOUT } rst_kind_t;
typedef enum { RST_CLASS_NONE, RST_CLASS_CRASH, RST_CLASS_POWER } rst_class_t;

/* radio_start: the reset hit esp_wifi_init/start, a scan or the access point start (hal_esp_stage). */
rst_class_t rst_classify(rst_kind_t kind, bool radio_start);

#endif
