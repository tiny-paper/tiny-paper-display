/* hal_esp: wake button (active low) and battery ADC (divider, eFuse calibrated curve fitting, median of 5 means),
 * pins and divider from board.h. OG: button GPIO2 with the internal pull-up, battery GPIO3 x2. PicPak: button and
 * battery share GPIO2 (no pulls: a pull-up would bias the divider), battery x1.45, the button read through
 * the ADC (src/btn_level.h); LED off (hal_led), IMU CS high.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include <stdlib.h>

#include "btn_level.h"
#include "hal_esp_priv.h"

#if !BOARD_BATT_GAUGE /* whole file; gauge boards (TRMNL X): x_board.c */

#define BATT_SAMPLES 8
#define BATT_GROUPS 5
#define BATT_CH ((adc_channel_t)HAL_PIN_BATT_ADC)

#ifndef BOARD_BUTTON_PULLUP
#define BOARD_BUTTON_PULLUP 0
#endif

/* A button wake's release, timed by a GPIO interrupt until the app first sees the button up (hal_button_take): the
 * OG boots ~1.5 s before the app looks, and a 1.2 s hold released meanwhile was a click (2026-09-27). The PicPak
 * too (its first look ~1.53 s after the wake: a 1 s hold in the menu was a click, "next item", 2026-09-27), when
 * the last battery reading puts the released level above VIH (src/btn_level.h; below, no latch: a click as before).
 * Its ADC reads make the pad analog for a moment; a release then is seen when the pad is digital again. */
#define UP_IRQ (!CONFIG_TINY_LINK_UART0)
#if BOARD_BUTTON_SHARED_ADC
static RTC_DATA_ATTR int s_batt_rtc; /* the last good battery reading, kept over deep sleep (0: none yet) */
#endif
#if UP_IRQ
static volatile int64_t s_up_us; /* esp_timer time of the first rising edge, 0: none yet */
static volatile int64_t s_up_last; /* ... of the last one */
static volatile uint8_t s_up_more; /* releases after the first (a press count: manifest.md "Actions") */
static bool s_up_armed;
static intr_handle_t s_up_irq;

/* the GPIO interrupt of the whole bank (no per-pin ISR service: ~1.1 KB less flash); only this pin enables one */
static void up_isr(void *arg)
{
    (void)arg;
    uint32_t st;
    gpio_ll_get_intr_status(&GPIO, 0, &st);
    gpio_ll_clear_intr_status(&GPIO, st);
    if (st & (1u << HAL_PIN_BUTTON)) {
        int64_t t = esp_timer_get_time();
        if (!s_up_us)
            s_up_us = t;
        else if (t - s_up_last > 40000 && s_up_more < 255)
            s_up_more++; /* another press's release, not a bounce */
        s_up_last = t;
    }
}
#endif

static void button_input(void)
{
    gpio_config_t c = {
        .pin_bit_mask = 1ull << HAL_PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = BOARD_BUTTON_PULLUP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
#if UP_IRQ
        .intr_type = s_up_armed ? GPIO_INTR_POSEDGE : GPIO_INTR_DISABLE,
#else
        .intr_type = GPIO_INTR_DISABLE,
#endif
    };
    gpio_config(&c);
}

#if UP_IRQ
static void up_arm(void)
{
    s_up_armed = s_up_irq || gpio_isr_register(up_isr, NULL, 0, &s_up_irq) == ESP_OK;
    if (s_up_armed)
        button_input(); /* rising edge, enabled */
}

/* The button read up: its release as the interrupt saw it (latched), then no more interrupts: the light-sleep wake
 * on this pin uses a level interrupt */
static void up_seen(void)
{
    if (!s_up_armed)
        return;
    s_up_armed = false;
    gpio_intr_disable(HAL_PIN_BUTTON); /* the handler stays, idle */
    gpio_set_intr_type(HAL_PIN_BUTTON, GPIO_INTR_DISABLE);
    int64_t t = s_up_us, now = esp_timer_get_time();
    if (t && t <= now) {
        hal_esp_button_latch(hal_rtc_us() - (uint64_t)(now - t));
        hal_esp_button_latch_more(s_up_more);
    }
}
#endif

void hal_led(bool on)
{
#ifdef BOARD_LED
    gpio_set_level(BOARD_LED, (on != 0) != (BOARD_LED_ACTIVE_LOW != 0));
#else
    (void)on;
#endif
}

void hal_io_init(void)
{
    button_input();
#ifdef BOARD_LED
    /* off: driven to its inactive level while awake */
    gpio_reset_pin(BOARD_LED);
    gpio_set_direction(BOARD_LED, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_LED, BOARD_LED_ACTIVE_LOW ? 1 : 0);
#endif
#ifdef BOARD_IMU_CS
    /* the IMU shares the panel's SPI bus: keep it deselected */
    gpio_reset_pin(BOARD_IMU_CS);
    gpio_set_direction(BOARD_IMU_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_IMU_CS, 1);
#endif
}

#if !CONFIG_TINY_LINK_UART0 /* the QEMU build has no ADC and no button: these helpers are compiled only where used (-Werror) */
/* ADC1 unit and calibration: created once and kept (a oneshot read powers the ADC only while it converts), so the
 * PicPak button can be read through the ADC on every poll. */
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cal;
static bool s_cal_tried;

static bool adc_ready(void)
{
    if (!s_adc) {
        adc_oneshot_unit_init_cfg_t uc = {.unit_id = ADC_UNIT_1, .ulp_mode = ADC_ULP_MODE_DISABLE};
        if (adc_oneshot_new_unit(&uc, &s_adc) != ESP_OK) {
            s_adc = NULL;
            return false;
        }
    }
    adc_oneshot_chan_cfg_t cc = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_oneshot_config_channel(s_adc, BATT_CH, &cc) != ESP_OK) /* the pad is analog from here */
        return false;
    if (!s_cal_tried) {
        s_cal_tried = true;
        /* per chip: the eFuse calibration (curve fitting, C3 and S3) of this unit and attenuation */
        adc_cali_curve_fitting_config_t cf = {.unit_id = ADC_UNIT_1, .chan = BATT_CH, .atten = ADC_ATTEN_DB_12,
                                              .bitwidth = ADC_BITWIDTH_DEFAULT};
        if (adc_cali_create_scheme_curve_fitting(&cf, &s_cal) != ESP_OK) {
            s_cal = NULL;
            hal_logf("W adc: no eFuse calibration, battery unknown");
        }
    }
    return true;
}

/* Mean of n readings in mV (calibrated; without a calibration the nominal 12 dB scale, only good enough for the
 * button), < 0 on failure. */
static int adc_mean_mv(int n, bool need_cal)
{
    if (need_cal && !s_cal)
        return HAL_E_IO;
    int sum = 0, got = 0, raw0;
    /* the first conversion after adc_ready switched the pad to analog is dropped: the sampling capacitor charges
     * through the divider's high impedance (cheap, a few us) */
    (void)adc_oneshot_read(s_adc, BATT_CH, &raw0);
    for (int i = 0; i < n; i++) {
        int raw, v;
        if (adc_oneshot_read(s_adc, BATT_CH, &raw) != ESP_OK)
            continue;
        if (!s_cal)
            v = raw * 2500 / 4095;
        else if (adc_cali_raw_to_voltage(s_cal, raw, &v) != ESP_OK)
            continue;
        sum += v;
        got++;
    }
    return got ? sum / got : HAL_E_IO;
}

#if BOARD_BUTTON_SHARED_ADC
/* An ADC reading of a pressed level the digital input contradicts (src/btn_level.h BTN_LVL_ADC_BAD): logged once
 * per boot, counted (hal_stats.adc_bad) */
static void adc_bad(const char *what, int mv)
{
    if (!hal_stats.adc_bad++)
        hal_logf("W adc_bad at=%s mv=%d dig=1 batt_mv=%d", what, mv, hal_stats.batt_mv);
}
#endif

/* The button's level, not noted (hal_button_down, hal_esp_button_wake). */
static bool button_level(void)
{
#if BOARD_BUTTON_SHARED_ADC
    /* PicPak: through the ADC (src/btn_level.h: the released level is below the digital VIH on a battery under
     * ~3.6 V and could read as pressed, starting a hold or a reset), and a press only when the digital input reads
     * the node low too (btn_level_read: an ADC reading of 0 alone was a "held" button, 2026-09-27) */
    int mv = adc_ready() ? adc_mean_mv(2, false) : HAL_E_IO;
    button_input(); /* back to a digital input: the GPIO wake and the cross-check read it */
    btn_lvl_t l = btn_level_read(mv, gpio_get_level(HAL_PIN_BUTTON) == 0, hal_stats.batt_mv, BOARD_BATT_DIV_X100,
                                 BOARD_BUTTON_ADC_MV);
    if (l == BTN_LVL_ADC_BAD)
        adc_bad("button", mv);
    return l == BTN_LVL_DOWN;
#else
    return gpio_get_level(HAL_PIN_BUTTON) == 0;
#endif
}

#endif /* !CONFIG_TINY_LINK_UART0 */

void hal_esp_button_wake(void)
{
#if !CONFIG_TINY_LINK_UART0
    button_input();
    uint64_t now = hal_rtc_us(), up = (uint64_t)esp_timer_get_time(); /* since this boot started */
    bool d = button_level();
    /* the wake was the press (the GPIO wake fires on its low level): dated to the wake whatever this first read
     * says. The PicPak's read here said up for presses still held (user log 2026-09-27, every press a button wake:
     * a ~1 s hold 767 ms, a click; a 3 s hold 1553 ms): the press then counted from a later read, 1 to 1.5 s short.
     * Released by the first look: its release is timed by the interrupt below, else it was a click (app) */
    hal_esp_button_note_at(true, now > up ? now - up : now);
    if (!d)
        hal_logf("I button wake read=up (dated to the wake)");
#if UP_IRQ
    bool arm = true;
#if BOARD_BUTTON_SHARED_ADC
    arm = btn_digital_in_spec(s_batt_rtc, BOARD_BATT_DIV_X100); /* a release is a rising edge */
#endif
    if (arm)
        up_arm();
#endif
#endif
}

bool hal_button_down(void)
{
#if CONFIG_TINY_LINK_UART0
    return false; /* QEMU reads every GPIO as low */
#else
    bool d = button_level();
#if UP_IRQ
    if (d)
        s_up_us = 0; /* still (or again) down: an edge before was a bounce */
    else
        up_seen();
#endif
    hal_esp_button_note(d);
    return d;
#endif
}

void hal_esp_button_poll(void)
{
#if !CONFIG_TINY_LINK_UART0
    bool d = button_level();
    if (!d)
        hal_esp_button_latch(hal_rtc_us());
    hal_esp_button_note(d);
#endif
}

#if !CONFIG_TINY_LINK_UART0
static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
#endif

int hal_battery_mv(void)
{
#if CONFIG_TINY_LINK_UART0
    return -1; /* QEMU: no ADC */
#else
    /* median of 5 means of 8 (open-PicPak device.md 6: median of 5 x 20): a WiFi or panel spike moves one mean */
    int g[BATT_GROUPS], mv = HAL_E_IO;
    bool ok = adc_ready();
    for (int i = 0; ok && i < BATT_GROUPS; i++)
        ok = (g[i] = adc_mean_mv(BATT_SAMPLES, true)) >= 0;
    if (ok) {
        qsort(g, BATT_GROUPS, sizeof g[0], cmp_int);
        /* (pin * div + 50) / 100: exactly pin * 2 for the OG's x2 */
        mv = (g[BATT_GROUPS / 2] * BOARD_BATT_DIV_X100 + 50) / 100;
    }
#if BOARD_BUTTON_SHARED_ADC
    button_input(); /* back to a digital input for the button */
    /* a group under the button's level: pressed during the reading (the digital input low too; every button wake
     * sampled the battery with the button still down: batt=-1, 2026-09-27), else a bad ADC reading
     * (btn_level_read). No new value: the last good one (RTC) stands in, else unknown */
    if (ok && btn_pressed_mv(g[0], hal_stats.batt_mv, BOARD_BATT_DIV_X100, BOARD_BUTTON_ADC_MV)) {
        if (btn_level_read(g[0], gpio_get_level(HAL_PIN_BUTTON) == 0, hal_stats.batt_mv, BOARD_BATT_DIV_X100,
                           BOARD_BUTTON_ADC_MV) == BTN_LVL_ADC_BAD)
            adc_bad("batt", g[0]);
        mv = s_batt_rtc > 0 ? s_batt_rtc : HAL_E_IO;
    }
#endif
    hal_stats.batt_mv = mv;
#if BOARD_BUTTON_SHARED_ADC
    if (mv > 0)
        s_batt_rtc = mv;
#endif
    return mv;
#endif
}

#if BOARD_BUTTON_SHARED_ADC
bool hal_esp_button_wake_ok(void)
{
#if !CONFIG_TINY_LINK_UART0
    /* the GPIO wake reads the pin digitally: a released level that already reads low would wake at once, again
     * and again. In spec (battery >= ~3.6 V) it cannot; below, check the digital level against the ADC */
    if (btn_digital_in_spec(hal_stats.batt_mv, BOARD_BATT_DIV_X100))
        return true;
    bool down = hal_button_down(); /* leaves the pin a digital input */
    return down || gpio_get_level(HAL_PIN_BUTTON) != 0;
#else
    return true;
#endif
}
#endif

bool hal_io_sleep_prep(bool button_wake)
{
    if (button_wake && !hal_esp_button_wake_ok()) {
        hal_logf("W button wake off: the released pin reads low (batt_mv=%d)", hal_stats.batt_mv);
        return false;
    }
    return button_wake;
}

#endif /* !BOARD_BATT_GAUGE */
