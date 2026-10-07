/*
 * usb_detect.h - is a USB host attached (hal_link_host_present on the C3's USB Serial/JTAG)?
 * Hardware-free, host-tested; esp/link.c supplies the frame counter, clock and delays.
 * A host sends a start-of-frame every 1 ms while the device is enumerated and the USJ block latches
 * its 11-bit frame index: the host is there when the index moves. ESP-IDF's usb_serial_jtag_is_connected()
 * said "no host" on the real OG while plugged into a laptop (2026-09-24).
 * After a reset the host needs up to ~1.5 s to enumerate again; only a deep-sleep timer wake (battery)
 * gets the quick 60 ms check. A host that was there is gone after USBDET_LOST_US of still samples, counted from
 * the first still sample (a bus the host suspended, a dock contact that bounced, or a cable pulled: the chip cannot
 * tell them apart; esp/link.c logs the loss, sys.c keeps a TRMNL X on VBUS awake through it).
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_USB_DETECT_H
#define TINY_USB_DETECT_H

#include <stdbool.h>
#include <stdint.h>

#define USBDET_FRAME_MASK 0x7ffu
#define USBDET_SAMPLE_US 2500      /* > 2 SOF periods */
#define USBDET_POLL_MS 20
#define USBDET_QUICK_US 60000      /* deep-sleep timer wake */
#define USBDET_ENUM_US 1500000     /* any other boot: wait for enumeration */
#define USBDET_RECHECK_US 100000   /* later calls: cached this long */
#define USBDET_LOST_US 3000000     /* a host that was there is gone only after no SOF for this long (bus suspend, host stall) */

typedef struct {
    uint32_t (*frame)(void *ctx);          /* raw frame index register */
    int64_t (*now_us)(void *ctx);          /* since boot */
    void (*delay_us)(void *ctx, uint32_t us);  /* busy wait */
    void (*sleep_ms)(void *ctx, uint32_t ms);  /* yield */
    void *ctx;
    bool waited, last, had;                /* had: the frame index moved at least once (`seen` is a time) */
    int64_t at, seen;                      /* last check / last time the frame index moved */
    int64_t still;                         /* first still sample since the last movement (0: none); the grace runs from it */
} usbdet_t;

/* A zeroed state (static storage, as esp/link.c keeps it) is a valid start: no host seen yet (issue #118). */
void usbdet_init(usbdet_t *d);
/* quick: this boot is a deep-sleep timer wake. */
bool usbdet_present(usbdet_t *d, bool quick);

#endif
