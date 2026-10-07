/*
 * epd_bwseq.h - command sequences of the BWRY panel driver (epd_bwry.h), hardware-free and host-tested.
 * esp/epd_bwry.c runs them; this file only says what goes out, in which order.
 *
 * Refresh order (docs/hardware/picpak-refresh.md): init, [AREA], DTM + pixel data, then PON, [CDI], DRF, POF.
 * PicPak stock V1.1.13 (ota_1 of the owner's backup, 0x42020be4..0x42020fe8) and tesserae epd_display() send the
 * pixel data before PON, so the charge pumps are off while the app renders its bands. Exception: BWRY_FAST_E6P keeps
 * PON before the data (Good Display GDEM042F52 EPD_init_Fast: PON, then 0xE0/0xE6/0xA5, then data).
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_EPD_BWSEQ_H
#define TINY_EPD_BWSEQ_H

#include <stdbool.h>
#include <stdint.h>

enum { BWQ_CMD = 0, BWQ_WAIT = 1, BWQ_LUT = 2, BWQ_DELAY = 3, BWQ_PON = 4 };
/* BWQ_WAIT kinds (the driver maps them to timeouts): register write, power on, refresh */
enum { BWQ_W_INIT = 0, BWQ_W_PON = 1, BWQ_W_REFRESH = 2 };

typedef struct {
    uint8_t op;    /* BWQ_* */
    uint8_t cmd;   /* BWQ_CMD: command byte; BWQ_WAIT: BWQ_W_*; BWQ_DELAY: ms */
    uint8_t n;     /* data bytes */
    uint8_t d[9];
} bwq_op_t;

#define BWQ_MAX 32
#define BWQ_LUT_OUT 535 /* 0x20 data: 7 timing bytes, then 48 groups of 11 */

typedef struct {
    uint8_t init;          /* bwry_init_t */
    uint8_t fast;          /* bwry_fast_t */
    const uint8_t *lut;    /* register LUT (BWRY_LUT_BYTES), stock profile only; NULL = OTP */
    uint8_t pll;           /* PLL (0x30) in place of the profile's or the LUT's (experiment, 0x01..0x3f); 0 = as is */
} bwq_cfg_t;

/* Init after a reset or wake: profile registers, the fast variant before PON, and for BWRY_FAST_E6P also PON and
 * its after-PON registers (bwq_early_pon). Returns the op count. */
int bwq_init(const bwq_cfg_t *c, bwq_op_t *ops);
/* Opens the data stream: AREA (stock profile, full window), DTM. */
int bwq_stream_head(const bwq_cfg_t *c, bwq_op_t *ops);
/* After the data: PON (unless early), border (CDI 0x37 full, 0x97 partial), DRF, the refresh wait. */
int bwq_refresh(const bwq_cfg_t *c, bool partial, bwq_op_t *ops);
/* PON goes before the data (only BWRY_FAST_E6P without a LUT). */
bool bwq_early_pon(const bwq_cfg_t *c);
/* The 0x20 payload of a register LUT: 7 timing bytes (L[528..534]), then row r of 48: L[r + 48 * c], c = 0..10. */
void bwq_lut_payload(const uint8_t *lut, uint8_t out[BWQ_LUT_OUT]);

/* Driver entry gate. A BUSY wait runs epd_wait_hook, which answers the link: a "!tp" command there re-entered the
 * driver (PicPak 2049, 2026-09-28: the test card closed the SPI device under a running refresh, the refresh timed out
 * and its POF went to the freed device: assert in xQueueSemaphoreTake). Inside a wait no entry runs; SPI only while
 * open. */
typedef enum { BWQ_E_OPEN, BWQ_E_CLOSE, BWQ_E_BEGIN, BWQ_E_WRITE, BWQ_E_END, BWQ_E_SLEEP, BWQ_E_SPI } bwq_entry_t;
typedef struct {
    bool open, waiting;
} bwq_gate_t;
bool bwq_allowed(const bwq_gate_t *g, bwq_entry_t e);
/* After a failure: power off and reset only while open (a closed device gets no SPI). */
bool bwq_fail_recover(const bwq_gate_t *g);

#endif
