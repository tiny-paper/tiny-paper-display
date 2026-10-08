/* panel_cfg.c: see panel_cfg.h. SPDX-License-Identifier: GPL-3.0-or-later */
#include "panel_cfg.h"

#include "tiny/hal.h"

gpart_cfg_t panel_gpart_load(gpart_cfg_t def)
{
    gpart_cfg_t g;
    if (hal_kv_get(PANEL_KEY_GPART, &g, sizeof g) == (int)sizeof g && g.on <= 1 && g.dark <= 14 && g.light <= 14)
        return g;
    return def;
}

void panel_gpart_save(const gpart_cfg_t *g) { hal_kv_set(PANEL_KEY_GPART, g, sizeof *g); }
