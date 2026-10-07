/*
 * touch_btn.h - the TRMNL X touch bar (IQS323) as the wake button: when is it down. Hardware-free, host-tested;
 * esp/x_board.c feeds it every status read (SYSTEM_STATUS 0x10 and GESTURES 0x11, the first 4 bytes).
 *
 * Bits (Azoteq IQS323 lib v1.5.2 in stock trmnl-firmware f5f87b7, lib/IQS323/IQS323.h), confirmed on hardware
 * 2026-09-25 (`!tp touch watch`, printed as st=<byte1><byte0> g=<byte3><byte2>):
 *   status byte 1   touch bits 1, 3, 5 (CH0..2, mask 0x2a); prox 0, 2, 4; power mode 6..7
 *                   idle 0x41/0x51, CH0 touched 0x57, CH1 touched 0x5d, released 0x55/0x51
 *   status byte 0   bit 7 SHOW_RESET (x_board.c handles it), 2 slider event
 *   gestures byte 0 TAP 0, SWIPE 1/2, FLICK 3/4, HOLD 5, EVENT 6, BUSY 7 (stock GESTURE_SELECT 0x09: tap + hold)
 *                   a hold: 0x80 (busy) at touch, 0xe0 (busy, event, hold) from ~0.6 s (MINIMUM_HOLD_TIME
 *                   0x258 ms), 0x20 at the release
 * Rules:
 *   - down on a read with a touch bit; up on the first read without one (event or forced, every read counts)
 *   - while down, a forced read every TBTN_POLL_US (1 s, the cadence of the hardware watch runs that saw every
 *     release and hold); the release itself comes as an event window. Hardware 2026-09-26 (fw 0435, 1548):
 *     forced reads every 100 ms never showed a release (every tap was held_ms ~1495, hold1) and failed after
 *     ~1.5 s of a hold (gap): the chip seems not to finish its conversions when forced that often. A failed
 *     forced read is retried after TBTN_RETRY_US, not at once.
 *   - safety net: while down, valid reads must come at most TBTN_GAP_US apart: a longer gap (reads failed, or
 *     something else held the bus) ends the press, and a new one needs a read without touch first. A missed
 *     release so never grows into a long hold (hardware 2026-09-25: a 10 s watch command ran inside the press
 *     wait and a tap was taken as a 10.8 s press)
 *   - a hold is the touch bits for that long (as stock and trmnl-firmware PR #508), not the chip's HOLD gesture:
 *     the old rule (no HOLD gesture by 1.5 s: up) turned every tap whose release was not read into a 1.5 s hold.
 *     `hold` still notes the gesture, for the logs
 *   - TBTN_MAX_US at most, then up until a read without touch
 * Swipes (docs/devices/trmnl-x-touch.md). The slider coordinate rises from left to right (hardware 2026-09-28, fw
 * 1938, confirmed by the user: a slide right read 548..693 over two zones, a slide left 297..66 within the left zone,
 * taps 121..119 and 135..112); the chip's swipe gesture bit was 0 in every one. A press released within
 * TBTN_SWIPE_MAX_US is a swipe when, first match (`by`, logged):
 *   1. slider   its coordinate (0x12, 0xffff: none) moved >= the threshold between the first and last valid read:
 *               TBTN_SWIPE_SL, 1/32 of the slider's range (TBTN_SL_RANGE, stock IQS323_config_LP.h), or `sl_min`
 *               (NVS xswsl, x_board.c). Within one zone counts (the left slide above)
 *   2. gesture  the chip reported its swipe or flick gesture in a read of it (GESTURE_SELECT 0x0b, x_board.c): the
 *               last one seen (bits 1/3 positive = right, 2/4 negative = left)
 *   3. zones    only when no read of the press had a slider coordinate: the zone position (CH0 0, CH1 1024, CH2
 *               2048, the mean of the touched) moved >= TBTN_SWIPE_ZONE between the first and last read
 *   `swipe` then holds -1 (the finger moved left) or +1 (right) until the next press starts (tbtn_swipe_take reads it
 *   once). A touch wake whose press ended before the first read: tbtn_wake takes a swipe gesture that read shows.
 *   A press from sleep is read from the first read after the wake: the start of its movement is not seen.
 *   `trail` keeps the zone masks of the press (bit 0 CH0, 1 CH1, 2 CH2) as they changed, for the log.
 * Rejected samples (issue #140, device 2026-10-07: a tap logged `zones=1>7>6>4 swipe=1`, another `1>3>7>3>1`): a
 *   finger covers one zone or two next to each other. A press that starts on all three zones is a brush whatever
 *   its length (`wide`: never acts; it still rests and reseeds after 8 s); a read of all three in a press is
 *   skipped, as is a read less than TBTN_JUMP_US after the previous one whose zones are not next to the last kept
 *   ones (the stream reads every report: a finger cannot cross a zone between two).
 *   Reads farther apart may have missed the way between (windows lost while the bus was busy, device 2026-10-03
 *   `zones=4>1 ms=399`, a real swipe): a far zone is then a move. `jumps` counts the skipped samples, for the log.
 * Late release (same day: a tap logged `ms=3008 zones=4 sl=2016..2006 hold=1`): a release read more than
 *   TBTN_LATE_US after the last kept touched read ends the press at that read (its length is what the chip
 *   confirmed, `late_us` says how late the release came); the stream (16..60 ms) and the forced polls (1 s) never
 *   leave such a hole, a stopped stream does.
 * Brush (docs/spec/firmware.md "TRMNL X touch bar", vectors inputs.brush): the finger picking the X up, not a press
 * meant for a zone: the press touched 2 or more zones (`zones` with more than one bit) or its slider coordinate moved
 * >= TBTN_BRUSH_SL between the first and last valid read (a fixed 64, not the swipe's sl_min), and it lasted less
 * than TBTN_BRUSH_MAX_MS: a longer press is a hold (2026-10-01: real 2..4 s holds drift 70..220 and graze the
 * neighbour zone), judged by its zone, and a slide that is a swipe is a swipe first (app_input.c). tbtn_brush reads the press that ended last (or the one down
 * now) until the next press starts.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_TOUCH_BTN_H
#define TINY_TOUCH_BTN_H

#include <stdbool.h>
#include <stdint.h>

#define TBTN_TOUCH_BITS 0x2a
#define TBTN_G_HOLD 0x20
#define TBTN_G_BUSY 0x80
#define TBTN_GAP_US 2500000
#define TBTN_POLL_US 1000000       /* x_board.c: a forced read this often while down */
#define TBTN_RETRY_US 200000       /* after a failed forced read */
#define TBTN_MAX_US 30000000
#define TBTN_REST_US 8000000       /* a press this long is a resting object: never acts (X, owner 2026-10-03) */
#define TBTN_QUIET_US 1000000      /* presses ignored this long after the reseed */
#define TBTN_G_SWIPE_POS 0x0a      /* swipe + (bit 1), flick + (bit 3) */
#define TBTN_G_SWIPE_NEG 0x14      /* swipe - (bit 2), flick - (bit 4) */
#define TBTN_SL_NONE 0xffff
#define TBTN_SWIPE_MAX_US 1500000  /* as a touch screen stroke (input.h IN_TOUCH_MAX_MS) */
#define TBTN_SL_RANGE 2048         /* slider resolution (stock IQS323_config_LP.h) */
#define TBTN_SWIPE_SL (TBTN_SL_RANGE / 32) /* 64: taps moved 2 and 23, slides 145 and 231 (Swipes) */
#define TBTN_SWIPE_ZONE 1000       /* zone positions (0, 1024, 2048) */
#define TBTN_TRAIL 8
#define TBTN_BRUSH_MAX_MS 600      /* input.h IN_BRUSH_MAX_MS: a longer press is a hold, never a brush */
#define TBTN_BRUSH_SL 64           /* input.h IN_BRUSH_SLIDE, vectors inputs.brush `slide` */
#define TBTN_JUMP_US 200000        /* touched reads this close must show zones next to each other (Rejected samples) */
#define TBTN_LATE_US 1250000       /* a release read this long after the last touched read ends the press there */

enum { TBTN_BY_NONE, TBTN_BY_SLIDER, TBTN_BY_GESTURE, TBTN_BY_ZONES }; /* what made the last press a swipe */

typedef struct {
    bool down;
    bool need_up;      /* ended without a release (gap, cap, no hold gesture): wait for a read without touch */
    int64_t since_us;  /* press start (first touched read) */
    int64_t last_us;   /* last valid read */
    int64_t try_us;    /* last forced read tried (tbtn_tried) */
    bool hold;         /* the chip reported its hold gesture during this press */
    const char *why;   /* last forced end: "gap", "rest", "max"; NULL after a release */
    /* the press's movement (Swipes) */
    int8_t gswipe;     /* the chip's last swipe or flick gesture in this press: -1, +1, 0 none */
    int16_t sl0, sl1;  /* first and last valid slider coordinate, -1 none */
    int16_t z0, z1;    /* first and last zone position, -1 none */
    uint8_t trail[TBTN_TRAIL], ntrail; /* zone masks as they changed (more than TBTN_TRAIL: the last one replaced) */
    uint8_t zones;     /* every zone touched in this press */
    int8_t swipe;      /* the last press that ended by a release: -1 left, +1 right, 0 none (tbtn_swipe_take) */
    uint8_t by;        /* ... what made it one (TBTN_BY_*), for the log */
    uint16_t sl_min;   /* slider travel of a swipe; 0: TBTN_SWIPE_SL */
    bool rest;         /* the press was a resting object (ended "rest"): the release re-baselines again */
    bool reseed;       /* the chip should be re-baselined (tbtn_reseed_take) */
    int64_t quiet_us;  /* touched reads before this time are ignored (tbtn_quiet) */
    bool ended;        /* a press ended (released or forced); x_board.c logs it and clears this */
    int64_t touched_us; /* last touched read that was kept */
    int64_t late_us;   /* the release read came this long after the last touched read (0: in time), for the log */
    uint8_t jumps;     /* samples of this press rejected (Rejected samples), for the log */
    bool wide;         /* the press started on all three zones: a brush (Rejected samples) */
} tbtn_t;

void tbtn_reset(tbtn_t *b);
/* A valid read at `now`: status byte 1 and gestures byte 0 (no slider coordinate). */
void tbtn_read(tbtn_t *b, int64_t now, uint8_t st1, uint8_t g0);
/* ... with the slider coordinate (0x12..0x13, TBTN_SL_NONE: none). */
void tbtn_read_sl(tbtn_t *b, int64_t now, uint8_t st1, uint8_t g0, uint16_t sl);
/* The first read after a touch wake, when it shows no touch (the press ended before): its swipe gesture, if any. */
void tbtn_wake(tbtn_t *b, uint8_t st1, uint8_t g0);
/* The swipe of the press that ended last (-1 left, +1 right, 0 none); once (then 0). */
int8_t tbtn_swipe_take(tbtn_t *b);
/* A contact of `zones` (the zone mask, bit 0 CH0) whose slider moved `slide`, lasting `ms`, is a brush (Brush above). */
bool tbtn_brush_of(uint8_t zones, unsigned slide, unsigned ms);
/* Drop the press in progress (a chip reset): it never acts, the next one starts after a read without touch. */
void tbtn_drop(tbtn_t *b);
/* True once after a resting touch started (8 s) or ended: x_board.c re-ATIs the chip, then calls tbtn_quiet. */
bool tbtn_reseed_take(tbtn_t *b);
/* Ignore touched reads until `until_us` (then a read without touch is needed first). */
void tbtn_quiet(tbtn_t *b, int64_t until_us);
/* The press that ended last (or is down now) is a brush. */
bool tbtn_brush(const tbtn_t *b);
/* The log word of TBTN_BY_*: "slider", "gesture", "zones", "-". */
const char *tbtn_by_name(uint8_t by);
/* The zone mask (bit 0 CH0 left, 1 CH1 middle, 2 CH2 right) of status byte 1. */
uint8_t tbtn_zones(uint8_t st1);
/* Down at `now` (also ends a press whose reads stopped or whose hold the chip does not confirm). */
bool tbtn_down(tbtn_t *b, int64_t now);
/* A forced read is due (down, the last read TBTN_POLL_US old and the last try TBTN_RETRY_US old). */
bool tbtn_poll_due(const tbtn_t *b, int64_t now);
/* A forced read is started at `now` (tbtn_read follows if it is valid). */
void tbtn_tried(tbtn_t *b, int64_t now);

#endif
