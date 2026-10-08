/* epd_driver.c - see epd_driver.h. SPDX-License-Identifier: GPL-3.0-or-later */
#include "epd_driver.h"

const char *const EPD_DRIVER_WORDS[EPD_DRIVER_COUNT] = {"ours", "plain"};

bool epd_driver_valid(int driver) { return driver >= 0 && driver < EPD_DRIVER_COUNT; }

const char *epd_driver_name(int driver) { return epd_driver_valid(driver) ? EPD_DRIVER_WORDS[driver] : "?"; }

epd_driver_parts_t epd_driver_parts(int driver)
{
    const bool o = driver == EPD_DRIVER_OURS;
    return (epd_driver_parts_t){
        .row_hook = o, .bus = o, .sequencer = o, .wave = o, .redrive = o, .rails = o, .gray_partial = o};
}

epd_driver_rows_fn *epd_driver_rows(int driver, epd_driver_rows_fn *ours, epd_driver_rows_fn *lib)
{
    return epd_driver_parts(driver).row_hook ? ours : lib;
}

uint32_t epd_driver_bus_hz(int driver, uint32_t ours_hz, uint32_t lib_hz)
{
    return epd_driver_parts(driver).bus && ours_hz ? ours_hz : lib_hz;
}

uint8_t epd_driver_partial_depths(int driver, uint8_t depths)
{
    return (uint8_t)(epd_driver_parts(driver).gray_partial ? depths : depths & 1);
}

uint32_t epd_driver_lib_scans(bool partial, int clear, int passes)
{
    static const uint8_t CLEAR[] = {0, 16, 32, 8}; /* EPDP_CLEAR_NONE, _FAST, _SLOW, _WHITE */
    if (passes < 1)
        return 0;
    if (partial)
        return (uint32_t)passes + 1;
    if (clear < 0 || clear > 3)
        return 0;
    return (uint32_t)CLEAR[clear] + 1 + (uint32_t)passes + 1;
}
