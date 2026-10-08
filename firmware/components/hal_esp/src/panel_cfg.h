/*
 * panel_cfg.h - the stored panel settings of the display firmware (TRMNL X). Hardware-free, host-tested.
 * A display driven by a host behaves the same on every device of a board, so the settings area of a device that ran
 * another firmware must not change it: the X driver's tuning keys of that firmware ("xpass", "xwave", "xrow",
 * "xdrive", "xbus", "xclear", "xgray2", "xsec", "xevery", "xwinall", "xgpart") are never read here; the console
 * commands change them for the running session only. The one setting kept is the gray partial switch, under a key
 * of this firmware's own. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_PANEL_CFG_H
#define TINY_PANEL_CFG_H

#include <stdint.h>

#define PANEL_KEY_GPART "dgpart"

typedef struct {
    uint8_t on, dark, light; /* epdp_gray_partial: switch, frames pushing the changed pixels dark, then light */
} gpart_cfg_t;

/* The stored gray partial setting, or `def` when none is stored or it is invalid. */
gpart_cfg_t panel_gpart_load(gpart_cfg_t def);
void panel_gpart_save(const gpart_cfg_t *g);

#endif
