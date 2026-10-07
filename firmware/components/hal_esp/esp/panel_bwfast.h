/* panel_bwfast.h - the experimental fast black/white partial of the PicPak ("!tp bwfast", NVS "bwfast") for
 * panel_bwry.c: the LUT for one partial, the colour gate on its rows, the setting commands. Off by default, a release
 * never sets it (issue #190, epd_bwfast.h). SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TINY_PANEL_BWFAST_H
#define TINY_PANEL_BWFAST_H

#include <stdbool.h>
#include <stdint.h>

#include "epd_bwpart.h"

/* "!tp bwfast on" saved (the default path never calls the rest, so its bytes and log lines stay as they are) */
bool bwfast_on(void);
/* The LUT for the partial about to begin, or NULL (why logged): every and state as epd_bwpart.h, temp_c the panel
 * reading, full the register LUT the full refreshes use (NULL = OTP). The pointer stays valid until bwfast_done. */
const uint8_t *bwfast_lut(uint8_t every, const bwp_state_t *ps, int16_t temp_c, const uint8_t *full);
bool bwfast_active(void);                   /* this partial goes out with the fast LUT */
void bwfast_row(const uint8_t *row, int bytes); /* a window row of panel codes as it goes out: the colour gate */
bool bwfast_colour(void);                   /* active and a yellow or red code was seen: refuse the refresh */
void bwfast_done(void);                     /* the partial ended or was dropped */
const char *bwfast_name(void);              /* log lut=: "bwfast" while active, else "full" */
void bwfast_show(void);                     /* one log line with the setting */
/* "!tp bwfast ...": 1 = a setting saved (the caller restarts for a full refresh), 0 = shown or usage logged */
int bwfast_cmd(int argc, char **argv);

#endif
