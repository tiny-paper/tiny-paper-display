/* hal_esp: clock, sleep, reset, RTC memory, random, init.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_rtc_time.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_esp_priv.h"
#include "reset_class.h"
#include "rtclock.h"
#include "wakecal.h"
#include "esp_private/esp_clk.h"
#include "soc/rtc.h"
#include "soc/rtc_cntl_reg.h"
#if BOARD_BUTTON_EXT1
#include "driver/rtc_io.h"
#endif

/* ------------------------------------------------------------------ RTC memory */

#define NOINIT_MAGIC 0x54504E49u /* "TPNI" */
#define STASH_CAP 512
#define HAL_ESP_BROWNOUT_SETTLE_MS 2000 /* after a brownout reset: before the first radio start and panel drive */

/* Survives deep sleep and software resets (not power loss). RTC_NOINIT: the startup
 * code never touches it. */
typedef struct {
    uint32_t magic;
    uint8_t pseudo_wake;       /* hal_wake_t + 1 after hal_restart_as, 0 = none */
    uint8_t crash_count;
    uint16_t stash_len;
    uint8_t stash[STASH_CAP];
    uint64_t clock_base_us;    /* CONFIG_TINY_CLOCK_ESP_TIMER: time before this boot */
    /* fast test mode: hal_rtc_us = fast_base + (raw - fast_raw0) * fast_k (all 0 after power-on: identity) */
    uint64_t fast_base, fast_raw0;
    uint8_t fast_k;
    uint32_t first_boot;       /* FIRST_BOOT_MAGIC from the storage format to the end of that wake cycle */
    uint8_t deep_restart;      /* restart() went through a deep sleep: the next wake is a restart */
    uint8_t stage;             /* hal_esp_stage: the radio step running (layout of older images up to here) */
    rtclock_t clk;             /* hal_rtc_us from raw ticks (src/rtclock.h); magic: absent in older images */
    wakecal_t wc;              /* deep-sleep timer margin (timekeeper wakecal.h); magic: absent in older images */
} hal_noinit_t;

#define FIRST_BOOT_MAGIC 0x46495253u /* "FIRS" */

static RTC_NOINIT_ATTR hal_noinit_t N;
static RTC_NOINIT_ATTR uint8_t rtc_state[1024] __attribute__((aligned(8)));

hal_wake_stats_t hal_stats;
static hal_wake_t s_wake = HAL_WAKE_COLD;
static esp_reset_reason_t s_reset;
static bool s_force_awake;
static uint32_t s_cycles;      /* in-place wakes since this boot */
static bool s_first_reset;     /* this boot follows a reset during a first boot's format window */
static bool s_wdt_relaxed;
static TaskHandle_t s_core;    /* the core's thread, subscribed to the task watchdog (hal_esp_wdt_feed) */
static int64_t s_fed_us;
static uint8_t s_reset_stage;  /* the step a crash, watchdog or brownout reset hit (hal_esp_stage) */
static bool s_power_fault;     /* src/reset_class.h: a brownout, or a watchdog while the radio started */

void hal_esp_stage(hal_esp_stage_t st) { N.stage = (uint8_t)st; }
hal_esp_stage_t hal_esp_stage_now(void) { return (hal_esp_stage_t)N.stage; }

/* Panel driver calls from the app (linker --wrap, CMakeLists.txt): HAL_ESP_ST_PANEL around them. The app renders a
 * band before hal_panel_write, never inside, so a panic or watchdog reset in one is the OS's (the driver), not the
 * plugin's: not counted toward the crash loop (firmware.md "Safe mode"). */
int __real_hal_panel_begin(uint8_t depth, hal_refresh_t mode);
int __real_hal_panel_write(const tiny_rect_t *r, const uint8_t *prev, const uint8_t *next, size_t stride);
int __real_hal_panel_refresh(void);
void __real_hal_panel_sleep(void);

int __wrap_hal_panel_begin(uint8_t depth, hal_refresh_t mode)
{
    hal_esp_stage_t was = hal_esp_stage_now();
    hal_esp_stage(HAL_ESP_ST_PANEL);
    int rc = __real_hal_panel_begin(depth, mode);
    hal_esp_stage(was);
    return rc;
}

int __wrap_hal_panel_write(const tiny_rect_t *r, const uint8_t *prev, const uint8_t *next, size_t stride)
{
    hal_esp_stage_t was = hal_esp_stage_now();
    hal_esp_stage(HAL_ESP_ST_PANEL);
    int rc = __real_hal_panel_write(r, prev, next, stride);
    hal_esp_stage(was);
    return rc;
}

int __wrap_hal_panel_refresh(void)
{
    hal_esp_stage_t was = hal_esp_stage_now();
    hal_esp_stage(HAL_ESP_ST_PANEL);
    int rc = __real_hal_panel_refresh();
    hal_esp_stage(was);
    return rc;
}

void __wrap_hal_panel_sleep(void)
{
    hal_esp_stage_t was = hal_esp_stage_now();
    hal_esp_stage(HAL_ESP_ST_PANEL);
    __real_hal_panel_sleep();
    hal_esp_stage(was);
}

static const char *stage_name(uint8_t st)
{
    static const char *const n[HAL_ESP_ST_COUNT] = {"none", "wifi_init", "wifi_start", "wifi_scan", "ap_start",
                                                    "wifi_connect", "http", "sntp", "panel"};
    return st < HAL_ESP_ST_COUNT ? n[st] : "?";
}

void *hal_rtc_mem(size_t *size)
{
    if (size)
        *size = sizeof rtc_state;
    return rtc_state;
}

uint8_t *hal_sys_stash(uint16_t **len, uint16_t *cap)
{
    *len = &N.stash_len;
    *cap = STASH_CAP;
    return N.stash;
}

uint8_t hal_esp_crash_count(void) { return N.crash_count; }

const char *hal_esp_reset_step(void)
{
    return s_reset_stage && s_reset_stage < HAL_ESP_ST_COUNT ? stage_name(s_reset_stage) : "";
}

const char *hal_esp_reset_name(void)
{
    switch (s_reset) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "ext";
    case ESP_RST_SW: return "sw";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    default: return "unknown";
    }
}

/* ------------------------------------------------------------------ clock */

static uint64_t raw_us(void)
{
    /* Slow-clock time, accumulated across calibration changes, deep sleep and
     * software resets (esp_clk.c keeps its own retained state). */
#if CONFIG_TINY_CLOCK_ESP_TIMER
    return N.clock_base_us + (uint64_t)esp_timer_get_time();
#else
    /* Raw ticks with our calibration (src/rtclock.h: ESP-IDF times sleeps with a 10-cycle one). */
    static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL_SAFE(&lock);
    uint64_t t = rtc_time_get();
    if (!rtclock_ok(&N.clk, t)) /* power-on, or the first boot of this image: continue the IDF clock */
        rtclock_init(&N.clk, t, esp_rtc_get_time_us(), esp_clk_slowclk_cal_get());
    uint64_t us = rtclock_us(&N.clk, t);
    portEXIT_CRITICAL_SAFE(&lock);
    return us;
#endif
}

/* Once per boot, before the first hal_rtc_us: the sleep so far with the previous boot's calibration, then this
 * boot's (CONFIG_RTC_CLK_CAL_CYCLES, set by the startup code) blended in. */
static int32_t s_wake_err_ppm; /* this timer wake against its timer (wakecal.h), for the log */
static uint64_t s_alarm_us;    /* when it fired, in our clock (0: unknown) */

static void clock_boot(void)
{
    if (!wakecal_ok(&N.wc))
        wakecal_reset(&N.wc);
#if !CONFIG_TINY_CLOCK_ESP_TIMER
    uint64_t t = rtc_time_get();
    if (rtclock_ok(&N.clk, t)) {
        /* a timer wake: the alarm tick is still in the sleep timer registers; with the old calibration it is the
         * moment the timer fired in our clock (the ticks after it were counted at the same rate) */
        if (s_wake == HAL_WAKE_TIMER && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
            uint64_t alarm = ((uint64_t)(REG_READ(RTC_CNTL_SLP_TIMER1_REG) & RTC_CNTL_SLP_VAL_HI_V) << 32) |
                             REG_READ(RTC_CNTL_SLP_TIMER0_REG);
            if (alarm >= N.clk.ticks0 && alarm <= t) {
                s_alarm_us = rtclock_us(&N.clk, alarm);
                wakecal_woke(&N.wc, s_alarm_us, &s_wake_err_ppm);
            }
        }
        rtclock_recal(&N.clk, t, esp_clk_slowclk_cal_get());
    }
#endif
    if (s_wake != HAL_WAKE_TIMER)
        wakecal_clear(&N.wc);
}

static unsigned fast_k(void) { return N.fast_k > 1 ? N.fast_k : 1; }

/* Fast test mode (!tp fast k): the clock runs k times faster than real time, continuous across
 * changes of k. Only for panel testing; the core never knows. */
uint64_t hal_rtc_us(void)
{
    hal_esp_wdt_feed(); /* every wait loop of the core reads the clock */
    uint64_t raw = raw_us();
    return N.fast_base + (raw >= N.fast_raw0 ? raw - N.fast_raw0 : 0) * fast_k();
}

/* Scaled duration -> real duration. */
static uint64_t real_us(uint64_t scaled) { return scaled / fast_k(); }

unsigned hal_esp_fast(void) { return fast_k(); }

void hal_esp_fast_set(unsigned k)
{
    if (k < 1) k = 1;
    if (k > HAL_ESP_FAST_MAX) k = HAL_ESP_FAST_MAX;
    uint64_t now = hal_rtc_us();
    N.fast_raw0 = raw_us();
    N.fast_base = now;
    N.fast_k = (uint8_t)k;
}

static void fast_init(void)
{
    uint8_t k = 1;
    if (hal_kv_get("fast", &k, 1) != 1 || k < 1 || k > HAL_ESP_FAST_MAX)
        k = 1;
    if (k != fast_k())
        hal_esp_fast_set(k);
    if (k > 1)
        hal_logf("W FAST MODE x%u: clock is not real time (time sync off; '!tp fast 1' to leave)", k);
}

/* Before a software restart: carry the emulated clock over (QEMU). */
static void clock_carry(void)
{
#if CONFIG_TINY_CLOCK_ESP_TIMER
    N.clock_base_us += (uint64_t)esp_timer_get_time() + 300000; /* ~boot time */
#endif
}

hal_wake_t hal_wake_cause(void) { return s_wake; }

void hal_random_bytes(void *buf, size_t len) { esp_fill_random(buf, len); }

/* The app's RTC state is CRC-protected and committed at the end of a wake cycle; a restart in the
 * middle of one (the "!tp" commands) must commit it first, or the next boot drops it as corrupt
 * and the time is lost (seen on the real OG, 2026-09-24). The app provides this hook. */
__attribute__((weak)) void hal_esp_pre_reboot(void) {}

/* A new image on trial (hal_ota_pending): a reset through the bootloader would roll it back, so
 * restart through a 1 ms deep sleep instead (the bootloader then boots the same slot without its
 * rollback check). USB drops for a moment. */
static _Noreturn void restart(void)
{
    hal_link_flush();
    clock_carry();
    if (hal_ota_pending()) {
        N.deep_restart = 1;
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
        esp_sleep_enable_timer_wakeup(1000);
        hal_esp_panel_rails_off();
        esp_deep_sleep_start();
    }
    esp_restart();
}

_Noreturn void hal_restart_as(hal_wake_t as)
{
    hal_esp_pre_reboot();
    N.pseudo_wake = (uint8_t)(as + 1);
    restart();
}

size_t hal_heap_largest(void) { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT); }

size_t hal_heap_free(void) { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }

_Noreturn void hal_reboot(void)
{
    N.pseudo_wake = 0;
    hal_esp_pre_reboot();
    restart();
}

void hal_esp_heap_mark(hal_esp_phase_t ph)
{
    if (ph < HAL_ESP_PH_COUNT)
        hal_stats.heap_free[ph] = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
}

/* Also on VBUS (TRMNL X dock): awake through a quiet bus, so a host that suspended it finds the chip enumerated (#133) */
bool hal_esp_stay_awake(void) { return s_force_awake || hal_link_host_present() || hal_esp_vbus_state() == 1; }

/* ------------------------------------------------------------------ init */

static void wake_init(void)
{
    s_reset = esp_reset_reason();
    if (s_reset == ESP_RST_POWERON || N.magic != NOINIT_MAGIC) {
        memset(&N, 0, sizeof N);
        N.magic = NOINIT_MAGIC;
    }
    switch (s_reset) {
    case ESP_RST_DEEPSLEEP: {
        if (N.deep_restart) /* restart() of an image on trial: as esp_restart would give */
            s_wake = N.pseudo_wake ? (hal_wake_t)(N.pseudo_wake - 1) : HAL_WAKE_COLD;
        else if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO ||
                 esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1 ||
                 esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0)
            s_wake = HAL_WAKE_BUTTON;
        else
            s_wake = HAL_WAKE_TIMER;
        break;
    }
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: {
        s_reset_stage = N.stage;
        /* a watchdog while the radio started: a supply sag, counted as a brownout, not a crash (reset_class.h) */
        bool radio = N.stage == HAL_ESP_ST_WIFI_INIT || N.stage == HAL_ESP_ST_WIFI_START ||
                     N.stage == HAL_ESP_ST_WIFI_SCAN || N.stage == HAL_ESP_ST_AP_START;
        s_power_fault = rst_classify(s_reset == ESP_RST_PANIC ? RST_KIND_PANIC : RST_KIND_WDT, radio) ==
                        RST_CLASS_POWER;
        /* not a crash when it hit the first boot's format window (hal_esp_first_boot_begin) */
        s_first_reset = N.first_boot == FIRST_BOOT_MAGIC;
        /* in a panel driver call (HAL_ESP_ST_PANEL): the OS's, not the plugin's; logged "W reset=... during panel" */
        if (!s_first_reset && !s_power_fault && N.stage != HAL_ESP_ST_PANEL && N.crash_count < 255)
            N.crash_count++;
        s_wake = HAL_WAKE_COLD;
        break;
    }
    case ESP_RST_SW:
        s_wake = N.pseudo_wake ? (hal_wake_t)(N.pseudo_wake - 1) : HAL_WAKE_COLD;
        break;
    default:
        s_wake = HAL_WAKE_COLD;
        break;
    }
    if (s_reset == ESP_RST_BROWNOUT) {
        s_reset_stage = N.stage;
        s_power_fault = rst_classify(RST_KIND_BROWNOUT, false) == RST_CLASS_POWER;
    }
    if (s_reset == ESP_RST_USB || s_reset == ESP_RST_EXT)
        s_reset_stage = N.stage;
    N.stage = HAL_ESP_ST_NONE;
    N.pseudo_wake = 0;
    N.deep_restart = 0;
    N.stash_len = 0;
    N.first_boot = 0;
}

static void wdt_idle(bool watch)
{
    if (watch == !s_wdt_relaxed)
        return;
    esp_task_wdt_config_t c = {
        .timeout_ms = watch ? CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000 : 600000,
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
        .idle_core_mask = watch ? 1u : 0u,
#endif
#if CONFIG_ESP_TASK_WDT_PANIC
        .trigger_panic = true,
#endif
    };
    esp_err_t e = esp_task_wdt_reconfigure(&c);
    s_wdt_relaxed = !watch && e == ESP_OK;
    if (e != ESP_OK)
        hal_logf("W task wdt reconfigure: %s", esp_err_to_name(e));
}

void hal_esp_wdt_feed(void)
{
    if (!s_core || xPortInIsrContext() || xTaskGetCurrentTaskHandle() != s_core)
        return;
    int64_t t = esp_timer_get_time();
    if (t - s_fed_us < 1000000)
        return;
    s_fed_us = t;
    esp_task_wdt_reset();
}

/* The core's thread on the task watchdog: a hang in it (a blocked wait or a loop) panics and restarts after
 * CONFIG_ESP_TASK_WDT_TIMEOUT_S instead of freezing the device with its last frame on the panel (PicPak
 * 2026-09-26: 27 min until a USB reconnect; only the idle task was watched, without a panic). */
static void wdt_core_init(void)
{
    s_fed_us = esp_timer_get_time();
    esp_err_t e = esp_task_wdt_add(NULL);
    if (e == ESP_OK)
        s_core = xTaskGetCurrentTaskHandle();
    else
        hal_logf("W task wdt add core: %s", esp_err_to_name(e));
}

void hal_esp_first_boot_begin(void)
{
    N.first_boot = FIRST_BOOT_MAGIC;
    N.crash_count = 0; /* fresh storage: crashes of an earlier install do not count */
    wdt_idle(false);
}

/* End of a wake cycle: a first boot's format window is over. */
static void first_boot_end(void)
{
    N.first_boot = 0;
    wdt_idle(true);
}

/* 32 kHz crystal (XTAL_32K_P/N, GPIO0/1 on the C3): none is documented on the OG or the PicPak. Probed once per
 * device (NVS "x32k": 1 absent, 2 present), at a power-on, as the bring-up's `xtal32k` does: the result is in the
 * log. When present the board should use it (CONFIG_RTC_CLK_SRC_EXT_CRYS=y in its sdkconfig.defaults: ESP-IDF
 * selects the slow clock at every boot, so it cannot be switched from here without miscounting the boot). */
static void xtal32k_probe(void)
{
#if defined(BOARD_XTAL32K_P) && !CONFIG_RTC_CLK_SRC_EXT_CRYS
    uint8_t v = 0;
    if (s_reset != ESP_RST_POWERON || hal_kv_get("x32k", &v, 1) == 1)
        return;
    rtc_clk_32k_enable(true);
    uint32_t cal = 0;
    for (int i = 0; i < 4 && !cal; i++) {
        vTaskDelay(pdMS_TO_TICKS(250));
        cal = rtc_clk_cal(RTC_CAL_32K_XTAL, 256);
    }
    rtc_clk_32k_enable(false);
    v = cal ? 2 : 1;
    hal_kv_set("x32k", &v, 1);
    if (cal) {
        double hz = 1e6 * (double)(1u << RTC_CLK_CAL_FRACT) / cal;
        hal_logf("W clock xtal32k=present hz=%.1f ppm=%+.0f: build this board with CONFIG_RTC_CLK_SRC_EXT_CRYS=y", hz,
                 (hz / 32768.0 - 1) * 1e6);
    } else {
        hal_logf("I clock xtal32k=absent (GPIO%d/%d): internal RC, early wakes (wakecal)", BOARD_XTAL32K_P,
                 BOARD_XTAL32K_N);
    }
#endif
}

/* A timer wake before its plan (the margin, wakecal.h): light-sleep to the planned time, so the core wakes as
 * planned. A button press meanwhile makes it a button wake. */
static void wake_wait(void)
{
    uint64_t plan = wakecal_plan(&N.wc), raw = raw_us();
    wakecal_clear(&N.wc);
    if (!plan || s_wake != HAL_WAKE_TIMER)
        return;
    /* fire_ms: the timer fired this long before the plan (negative: after it); boot time comes on top */
    uint64_t l0 = hal_light_us();
    if (plan > raw + 1000 && plan - raw < 600000000ull &&
        hal_light_sleep_until(hal_rtc_us() + (plan - raw) * fast_k()) == 1)
        s_wake = HAL_WAKE_BUTTON;
    hal_logf("clock wake err_ppm=%ld fire_ms=%lld wait_ms=%llu margin_ppm=%lu n=%lu", (long)s_wake_err_ppm,
             s_alarm_us ? ((long long)plan - (long long)s_alarm_us) / 1000 : 0LL,
             (unsigned long long)((hal_light_us() - l0) / 1000), (unsigned long)wakecal_margin_ppm(&N.wc),
             (unsigned long)N.wc.n);
}

void hal_esp_init(void)
{
#if CONFIG_TINY_FORCE_STAY_AWAKE || CONFIG_TINY_LINK_UART0
    s_force_awake = true;
#endif
    wake_init();
    clock_boot(); /* after wake_init: N is valid (cleared at power-on) */
    /* after wake_init: at power-on the RTC no-init memory (the fast mode clock terms) holds random bytes until
     * wake_init clears it (rtc_ms=13154401724791770 and a garbage awake_ms on a PicPak power-on, 2026-09-26) */
    hal_stats.boot_rtc_us = hal_rtc_us();
    if (s_wake == HAL_WAKE_BUTTON)
        hal_esp_button_wake(); /* before anything slow: is the press still down, since when */
    hal_esp_diag_boot(); /* resets since power-on, the core step a crash hit (esp/diag.c) */
    wdt_core_init();
    hal_esp_net_init(); /* first: its long-lived blocks at the start of the heap, not inside the big block */
    hal_link_init();
    hal_kv_init(); /* before hal_io_init: the X touch bar reads its NVS switch */
    hal_io_init();
    hal_fs_init();
    fast_init();
    hal_panel_init();
    xtal32k_probe();
    wake_wait(); /* after the pins and the link: it light-sleeps */
    hal_esp_heap_mark(HAL_ESP_PH_BOOT);
    if (s_first_reset)
        hal_logf("W reset during the first boot's storage format: not counted as a crash");
    /* slot: the app partition the bootloader chose (X 0459: after a task_wdt reset it booted the other one) */
    const esp_partition_t *run = esp_ota_get_running_partition();
    hal_logf("hal boot reset=%s wake=%d crash=%u rtc_ms=%llu heap=%u min=%u largest=%u fw=%s slot=%s",
             hal_esp_reset_name(), (int)s_wake, N.crash_count, (unsigned long long)(hal_stats.boot_rtc_us / 1000),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)hal_stats.heap_free[HAL_ESP_PH_BOOT],
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), esp_app_get_description()->version,
             run ? run->label : "?");
    hal_esp_diag_boot_logged(); /* reset belongs to this boot, not the following USB timer wakes */
    if (s_reset_stage && s_reset_stage < HAL_ESP_ST_COUNT)
        hal_logf("W reset=%s during %s", hal_esp_reset_name(), stage_name(s_reset_stage));
    if (s_power_fault) {
        /* PicPak: reset=brownout on USB connect (2026-09-25), a watchdog reset at portal start. Let the supply
         * settle before the first radio start (TX peak) and panel drive; the battery reading says whether it was
         * the battery or the plug-in step */
        hal_logf("W power fault (brownout class): reset=%s during %s, batt_mv=%d usb=%d, waiting %d ms before radio "
                 "and panel", hal_esp_reset_name(), stage_name(s_reset_stage), hal_battery_mv(), (int)hal_usb_power(),
                 HAL_ESP_BROWNOUT_SETTLE_MS);
        vTaskDelay(pdMS_TO_TICKS(HAL_ESP_BROWNOUT_SETTLE_MS));
    }
}

/* ------------------------------------------------------------------ sleep */

static void button_wake_arm_light(bool on)
{
    if (on) {
        gpio_wakeup_enable(HAL_PIN_BUTTON, GPIO_INTR_LOW_LEVEL);
        esp_sleep_enable_gpio_wakeup();
    } else {
        gpio_wakeup_disable(HAL_PIN_BUTTON);
    }
}

static uint64_t s_light_us; /* this boot's light sleep, hal_rtc_us units */

uint64_t hal_light_us(void) { return s_light_us; }

__attribute__((weak)) void hal_esp_light_sleep_pins(bool hold) { (void)hold; }
__attribute__((weak)) void hal_esp_button_wake(void) {}

/* One light sleep with the wake sources already armed; counted in hal_light_us. */
static void light_sleep_counted(void)
{
    hal_link_flush();
    hal_esp_light_sleep_pins(true);
    uint64_t t0 = hal_rtc_us();
    hal_esp_panel_rails_off(); /* its timer does not run while asleep */
    esp_light_sleep_start();
    s_light_us += hal_rtc_us() - t0;
    hal_esp_light_sleep_pins(false);
}

bool hal_esp_light_sleep_pin(int pin, int level, uint64_t max_us)
{
    (void)hal_button_down(); /* panel waits: a press already down is noted (hal_button_since) */
    if (hal_esp_stay_awake() || max_us < 5000 || gpio_get_level(pin) == level)
        return false;
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_timer_wakeup(max_us);
    gpio_wakeup_enable(pin, level ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    light_sleep_counted();
    gpio_wakeup_disable(pin);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    return true;
}

bool hal_esp_light_sleep_busy(int pin, uint64_t max_us, bool button)
{
    if (hal_esp_stay_awake() || max_us < 5000 || gpio_get_level(pin) == 1)
        return false;
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_timer_wakeup(max_us);
    gpio_wakeup_enable(pin, GPIO_INTR_HIGH_LEVEL);
    button_wake_arm_light(button);
    light_sleep_counted();
    button_wake_arm_light(false);
    gpio_wakeup_disable(pin);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    return true;
}

int hal_light_sleep_until(uint64_t rtc_us)
{
    bool btn_armed = !hal_button_down() && hal_esp_button_wake_ok();
    for (;;) {
        uint64_t now = hal_rtc_us();
        if (now >= rtc_us)
            break;
        uint64_t left = real_us(rtc_us - now);
        if (btn_armed && hal_button_down())
            return 1;
        if (left <= (hal_esp_stay_awake() ? 15000u : 3000u)) {
            /* spin the last few ms for precision */
            while (hal_rtc_us() < rtc_us) {
            }
            break;
        }
        if (hal_esp_stay_awake() || hal_esp_wifi_on()) {
            /* USB Serial/JTAG drops in light sleep: idle the CPU in FreeRTOS instead, and answer
             * quick link requests meanwhile; WiFi kept on (a press's hold, firmware.md "Buttons") stays connected */
            vTaskDelay(1);
            hal_busy_hook();
            continue;
        }
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
        /* IDF times it with a 10-cycle calibration (wakecal.h): up to ~0.5 % off, so stop short by 0.8 % */
        esp_sleep_enable_timer_wakeup(left - 2000 - left / 128);
        button_wake_arm_light(btn_armed);
        light_sleep_counted();
        button_wake_arm_light(false);
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
        if (btn_armed && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO && hal_button_down())
            return 1;
    }
    hal_stats.tick_late_us = (int64_t)(hal_rtc_us() - rtc_us);
    return 0;
}

static void summary(uint64_t until)
{
    uint64_t started = hal_rtc_us();
    hal_esp_heap_mark(HAL_ESP_PH_SLEEP);
    uint64_t now = hal_rtc_us();
    const hal_wake_stats_t *s = &hal_stats;
    char fk[12] = "", ov[32] = "";
    if (fast_k() > 1)
        snprintf(fk, sizeof fk, " fast=%u", fast_k());
    /* the planned wake already passed (link requests or a session served after the cycle planned it): sleep_s=0
     * and by how much it is late, not a negative sleep (sleep_s=-22 on the PicPak, 2026-09-26) */
    bool over = until && until < now;
    if (over)
        snprintf(ov, sizeof ov, " over_ms=%llu", (unsigned long long)real_us((now - until) / 1000));
    /* awake_ms in real time (fast test mode scales hal_rtc_us: 26 s "awake" at x6 was 4.3 s); light_ms: the part
     * of it in light sleep (minute wait, panel BUSY on battery) */
    hal_logf("wake reset=%s wake=%d awake_ms=%llu light_ms=%llu late_ms=%lld refresh=%s rects=%u box=%d,%d,%d,%d "
             "refresh_ms=%u batt_mv=%d wifi_ms=%u heap_min boot=%u render=%u net=%u sleep=%u sleep_s=%lld%s cycle=%u%s%s",
             hal_esp_diag_wake_reset(), (int)s_wake, (unsigned long long)real_us((now - s->boot_rtc_us) / 1000),
             (unsigned long long)real_us((s_light_us - s->light_at_boot_us) / 1000),
             (long long)(s->tick_late_us / 1000), s->refresh_kind == 1 ? "full" : s->refresh_kind == 2 ? "partial" : "none",
             s->rects, s->bx0, s->by0, s->bx1, s->by1, (unsigned)s->refresh_ms, s->batt_mv, (unsigned)s->wifi_ms,
             (unsigned)s->heap_free[0], (unsigned)s->heap_free[1], (unsigned)s->heap_free[2], (unsigned)s->heap_free[3],
             until ? (over ? 0LL : (long long)((until - now) / 1000000)) : -1LL, ov, (unsigned)s_cycles,
             hal_esp_stay_awake() ? " awake=1" : "", fk);
    hal_esp_diag_cycle_done(); /* consume the fault after its boot cycle, retaining the reset history */
    hal_esp_log_stacks();
    hal_logf("step name=reset_report ms=%llu", (unsigned long long)(real_us(hal_rtc_us() - started) / 1000));
}

/* Stay awake until the wake time, a button press or link bytes; RAM kept, no restart.
 * HAL_WAKE_COLD: the host went away (deep sleep for the rest). */
static hal_wake_t stay_awake_until(uint64_t rtc_us, bool button_wake)
{
    bool btn_armed = button_wake && !hal_button_down();
    for (;;) {
        if (rtc_us && hal_rtc_us() >= rtc_us)
            return HAL_WAKE_TIMER;
        if (btn_armed && hal_button_down())
            return HAL_WAKE_BUTTON;
        if (!btn_armed && button_wake && !hal_button_down())
            btn_armed = true;
        if (button_wake && hal_esp_touch_ready())
            return HAL_WAKE_BUTTON; /* a touch stroke (the rail is on while on USB, firmware.md "Touch") */
        if (hal_link_stash_wait(20))
            return HAL_WAKE_USB;
        if (!hal_esp_stay_awake())
            return HAL_WAKE_COLD;
    }
}

static _Noreturn void deep_sleep(uint64_t rtc_us, bool button_wake)
{
    /* a held button would wake us at once: wait up to 5 s for release, else no button wake */
    if (button_wake) {
        for (int i = 0; i < 500 && hal_button_down(); i++)
            vTaskDelay(pdMS_TO_TICKS(10));
        if (hal_button_down())
            button_wake = false;
    }
    button_wake = hal_io_sleep_prep(button_wake); /* board pins and chips to low power */
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (rtc_us) {
        uint64_t now = hal_rtc_us(), left = rtc_us > now ? real_us(rtc_us - now) : 0, raw = raw_us();
        /* shorter by the margin: the boot light-sleeps the rest (wake_wait) */
        left = wakecal_timer(&N.wc, raw, raw + left);
        esp_sleep_enable_timer_wakeup(left > 1000 ? left : 1000);
    } else {
        wakecal_clear(&N.wc);
    }
    if (button_wake) {
#if BOARD_X4P
        /* X4 Pro: ext1 on the released keys (x4p_board.c keeps their RTC pull-ups powered) */
        esp_sleep_enable_ext1_wakeup_io(hal_x4p_wake_mask(), ESP_EXT1_WAKEUP_ANY_LOW);
#elif BOARD_BUTTON_EXT1
        /* S3: deep-sleep wake through the RTC IO, ext0 as stock does on the X: ext0 keeps the RTC
         * peripherals powered, so the pad's pull-up holds the touch RDY line high while asleep */
        esp_sleep_enable_ext0_wakeup((gpio_num_t)HAL_PIN_BUTTON, 0);
        rtc_gpio_pullup_en((gpio_num_t)HAL_PIN_BUTTON);
        rtc_gpio_pulldown_dis((gpio_num_t)HAL_PIN_BUTTON);
#else
        esp_deep_sleep_enable_gpio_wakeup(1ull << HAL_PIN_BUTTON, ESP_GPIO_WAKEUP_GPIO_LOW);
#endif
    }
    hal_link_flush();
    hal_esp_panel_rails_off();
    esp_deep_sleep_start();
}

_Noreturn void hal_deep_sleep_until(uint64_t rtc_us, bool button_wake)
{
    N.crash_count = 0; /* a wake cycle completed */
    first_boot_end();
    summary(rtc_us);
    deep_sleep(rtc_us, button_wake);
}

hal_wake_t hal_sleep_until(uint64_t rtc_us, bool button_wake)
{
    N.crash_count = 0; /* a wake cycle completed */
    first_boot_end();
    summary(rtc_us);
    if (!hal_esp_stay_awake())
        deep_sleep(rtc_us, button_wake);
    hal_wake_t w = stay_awake_until(rtc_us, button_wake);
    if (w == HAL_WAKE_COLD)
        deep_sleep(rtc_us, button_wake);
    /* in place: fresh per-wake numbers for the next summary line */
    uint32_t heap[HAL_ESP_PH_COUNT];
    memcpy(heap, hal_stats.heap_free, sizeof heap);
    memset(&hal_stats, 0, sizeof hal_stats);
    memcpy(hal_stats.heap_free, heap, sizeof heap);
    hal_stats.boot_rtc_us = hal_rtc_us();
    hal_stats.light_at_boot_us = s_light_us;
    s_cycles++;
    s_wake = w;
    return w;
}

__attribute__((weak)) void hal_busy_hook(void) {}
__attribute__((weak)) void hal_log_hook(const char *line, size_t len) { (void)line, (void)len; }

#if !BOARD_PANEL_FASTEPD
void hal_esp_panel_rails_off(void) {} /* TRMNL X only: panel_par.c */
bool hal_live_seconds(void) { return false; } /* TRMNL X only: panel_par.c */
uint8_t hal_panel_partial_now(uint8_t depths) { return depths; } /* TRMNL X only: panel_par.c */
size_t hal_panel_tune_count(void) { return 0; } /* TRMNL X only: panel_par.c */
const hal_tune_t *hal_panel_tune_info(size_t i) { (void)i; return NULL; }
int hal_panel_tune_get(size_t i) { (void)i; return 0; }
bool hal_panel_tune_set(size_t i, int v) { (void)i; (void)v; return false; }
void hal_panel_tune_defaults(void) {}
bool hal_panel_gray_rows_set(const char *s, size_t len, char *why, size_t cap)
{
    (void)s;
    (void)len;
    if (cap)
        why[0] = 0;
    return false;
}
void hal_panel_gray_rows_get(char *out, size_t cap)
{
    if (cap)
        out[0] = 0;
}
#if !BOARD_PANEL_BWRY
bool hal_panel_gray_partial(void) { return false; } /* TRMNL X (panel_par.c), PicPak partial (panel_bwry.c) */
#endif
#endif
#if BOARD_PANEL_BWRY || BOARD_PANEL_FASTEPD || BOARD_X4P
uint32_t hal_panel_rev(void) { return 0; } /* no UC8179 REV read (OG: panel.c) */
#endif
#if !BOARD_PANEL_BWRY
void hal_panel_palette(uint8_t palette) { (void)palette; } /* gray panels: every frame is gray */
#endif
#if !BOARD_X4P /* X4 Pro: x4p_board.c */
int hal_frontlight(uint8_t level, uint8_t warm)
{
    (void)level;
    (void)warm;
    return HAL_E_NOTFOUND;
}

int hal_wallclock_get(int64_t *unix_ms, uint32_t *err_ms)
{
    (void)unix_ms;
    (void)err_ms;
    return HAL_E_NOTFOUND;
}

int hal_wallclock_set(int64_t unix_ms)
{
    (void)unix_ms;
    return HAL_E_NOTFOUND;
}

/* no touch screen (hal_board_t touch_screen false) */
int hal_touch_power(bool on)
{
    (void)on;
    return HAL_E_NOTFOUND;
}

bool hal_touch_take(hal_touch_t *t)
{
    (void)t;
    return false;
}

bool hal_esp_touch_ready(void) { return false; }
#endif

/* "!tp heap": the heap map, for fragmentation on the device (which block splits the largest one). Each
 * region, then its blocks of 1 KiB and more in address order, collected during the walk (no logging under
 * the heap lock) and printed after it:
 *   # heapmap region=<start>-<end> free=<bytes> largest=<bytes>
 *   # heapmap at=<offset in region> size=<bytes> used|free [owner=<name>]        (blocks >= 1024)
 * owner: a task's stack or TCB (stack:tiny is the firmware's own thread, main/app_main.c), else what the core
 * says (app_heap_owner: link = the USB link buffers, link_ble, fb_a/fb_b, dlog). TLSF rounds a request up to its
 * class: the ~25.2 KB link shows as 25600. */
__attribute__((weak)) const char *app_heap_owner(uintptr_t addr, size_t size)
{
    (void)addr;
    (void)size;
    return NULL;
}

static const char *heap_owner(uintptr_t a, size_t n)
{
    static const char *const names[] = {"tiny", "main", "wifi", "tiT", "esp_timer", "sys_evt", "nimble_host", "ipc0"};
    static char out[24];
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        TaskHandle_t t = xTaskGetHandle(names[i]);
        if (!t)
            continue;
        uintptr_t st = (uintptr_t)pxTaskGetStackStart(t);
        if (st >= a && st < a + n) {
            snprintf(out, sizeof out, "stack:%s", names[i]);
            return out;
        }
        if ((uintptr_t)t >= a && (uintptr_t)t < a + n) {
            snprintf(out, sizeof out, "tcb:%s", names[i]);
            return out;
        }
    }
    return app_heap_owner(a, n);
}
#define HEAPMAP_MAX 96
typedef struct {
    uint32_t start[4], end[4];
    int regions;
    struct {
        uint32_t addr, size;
        bool used;
    } b[HEAPMAP_MAX];
    int n, dropped;
} heapmap_t;

static bool heapmap_walk(walker_heap_into_t h, walker_block_info_t b, void *user)
{
    heapmap_t *m = user;
    if (!m->regions || m->start[m->regions - 1] != (uint32_t)h.start) {
        if (m->regions < 4) {
            m->start[m->regions] = (uint32_t)h.start;
            m->end[m->regions] = (uint32_t)h.end;
            m->regions++;
        }
    }
    if (b.size < 1024)
        return true;
    if (m->n < HEAPMAP_MAX) {
        m->b[m->n].addr = (uint32_t)(uintptr_t)b.ptr;
        m->b[m->n].size = (uint32_t)b.size;
        m->b[m->n].used = b.used;
        m->n++;
    } else {
        m->dropped++;
    }
    return true;
}

static bool heap_textcmd(const uint8_t *buf, size_t len)
{
    if (len < 8 || memcmp(buf, "!tp heap", 8))
        return false;
    heapmap_t *m = calloc(1, sizeof *m); /* ~1.2 KB, only for the command */
    if (!m) {
        hal_logf("E heapmap: no memory");
        return true;
    }
    heap_caps_walk(MALLOC_CAP_8BIT, heapmap_walk, m);
    hal_logf("heapmap free=%u largest=%u min=%u regions=%d blocks>=1024: %d (%d more)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT), m->regions, m->n, m->dropped);
    for (int r = 0; r < m->regions; r++) {
        hal_logf("heapmap region=%08x-%08x size=%u", (unsigned)m->start[r], (unsigned)m->end[r],
                 (unsigned)(m->end[r] - m->start[r]));
        for (int i = 0; i < m->n; i++)
            if (m->b[i].addr >= m->start[r] && m->b[i].addr < m->end[r]) {
                const char *o = m->b[i].used ? heap_owner(m->b[i].addr, m->b[i].size) : NULL;
                hal_logf("heapmap at=%u size=%u %s%s%s", (unsigned)(m->b[i].addr - m->start[r]),
                         (unsigned)m->b[i].size, m->b[i].used ? "used" : "free", o ? " owner=" : "", o ? o : "");
            }
    }
    free(m);
    return true;
}

/* the same map for the remote command diag_upload (remote.md "Commands"): into the log, so the debug log carries it */
void hal_heap_map_log(void) { heap_textcmd((const uint8_t *)"!tp heap", 8); }

bool hal_esp_sys_textcmd(const uint8_t *buf, size_t len) { return heap_textcmd(buf, len); }

/* link diag: the heap map (blocks >= 1 KiB, as "!tp heap") into d->blocks */
void hal_esp_heap_blocks(hal_diag_t *d)
{
    heapmap_t *m = calloc(1, sizeof *m);
    if (!m)
        return;
    heap_caps_walk(MALLOC_CAP_8BIT, heapmap_walk, m);
    for (int r = 0; r < m->regions; r++)
        for (int i = 0; i < m->n && d->n_blocks < HAL_DIAG_BLOCKS; i++) {
            if (m->b[i].addr < m->start[r] || m->b[i].addr >= m->end[r])
                continue;
            const char *o = m->b[i].used ? heap_owner(m->b[i].addr, m->b[i].size) : NULL;
            d->blocks[d->n_blocks].at = m->b[i].addr - m->start[r];
            d->blocks[d->n_blocks].size = m->b[i].size;
            d->blocks[d->n_blocks].used = m->b[i].used;
            snprintf(d->blocks[d->n_blocks].owner, sizeof d->blocks[0].owner, "%s", o ? o : "");
            d->n_blocks++;
        }
    free(m);
}

/* components/app's heap probe (weak there) */
void app_heap_probe(const char *where)
{
#ifdef CONFIG_TINY_FAULT_OVERRUN
    static int probes;
    if (++probes == 3) { /* guard self-test: write one byte past a 16 byte block and leak it */
        volatile uint8_t *bad = malloc(16);
        if (bad)
        {
            volatile size_t at = 16; /* not a constant: the compiler would refuse the overrun */
            bad[at] = 0x5a;
        }
    }
#endif
#ifdef CONFIG_TINY_HEAP_CHECK
    /* every probe (session, render and install steps): corruption is caught at the step that follows the bad write */
    if (!heap_caps_check_integrity_all(true)) {
        hal_logf("E heap corrupt at=%s", where);
        abort();
    }
#endif
    hal_logf("heap at=%s free=%u largest=%u min=%u", where, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}
