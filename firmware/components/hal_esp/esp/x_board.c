/* hal_esp: boards with a fuel gauge on the internal I2C bus (BOARD_BATT_GAUGE: TRMNL X). Owns I2C bus 0 (FastEPD
 * reuses it, components/epd), and drives, per boards/trmnl_x/board.h:
 *   BQ27427 gauge (0x55)    battery mV (Voltage 0x04); SoC, flags, capacities logged only (stock builds with
 *                           BYPASS_BQ27427_SOC)
 *   IQS323 touchbar (0x44)  the wake "button" (GPIO3 is its RDY line, not a button level): stock init and event
 *                           mode, touch bits read over I2C; see the IQS323 section below
 *   TCA9535 expander (0x20) charger power good (P0_0), C5 modem off (P0_6 low), panel/PMIC lines low in sleep
 *                           (stock display.cpp config_tca95535_pins_for_lp)
 * and the GPIOs before deep sleep (stock bl.cpp config_gpio_for_lp). Nothing here touches OTG (P0_1) or the
 * charger. SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"
#include "hal_esp_priv.h"
#include "soc/gpio_struct.h"
#include "touch_btn.h"

#if BOARD_BATT_GAUGE && !BOARD_X4P /* whole file (X4 Pro: x4p_board.c) */

#define I2C_HZ 100000
#define I2C_TMO_MS 50

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_gauge, s_iox, s_touch;
/* Our transfers on the bus (gauge, expander, touch bar) from the app and the touch bar's task (IQS323 below): one at a
 * time, and an IQS323 window (ask, wait for RDY, transfer, wait for it to close) as one. Recursive: the init and the
 * "!tp touch" commands hold it around many. FastEPD's own transfers are kept apart by the IDF bus lock. */
static SemaphoreHandle_t s_mx;

static void bus_lock(void)
{
    if (s_mx)
        xSemaphoreTakeRecursive(s_mx, portMAX_DELAY);
}

static void bus_unlock(void)
{
    if (s_mx)
        xSemaphoreGiveRecursive(s_mx);
}

static int64_t s_vbus_at = -10000000;
static bool s_vbus;

static i2c_master_dev_handle_t add_dev(uint8_t addr)
{
    i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = I2C_HZ};
    i2c_master_dev_handle_t d = NULL;
    return i2c_master_bus_add_device(s_bus, &dc, &d) == ESP_OK ? d : NULL;
}

int hal_x_i2c_init(void)
{
    if (s_bus)
        return HAL_OK;
    if (!s_mx)
        s_mx = xSemaphoreCreateRecursiveMutex();
    /* zeroed config (all IDF defaults) except what is set here: see components/epd/CMakeLists.txt */
    i2c_master_bus_config_t c = {
        .i2c_port = I2C_NUM_0, .sda_io_num = BOARD_I2C_SDA, .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&c, &s_bus) != ESP_OK) {
        s_bus = NULL;
        hal_logf("E i2c bus init failed");
        return HAL_E_IO;
    }
    /* the IQS323 NACKs outside its window by design: our callers report failures themselves */
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    s_gauge = add_dev(BOARD_I2C_ADDR_GAUGE);
    s_iox = add_dev(BOARD_I2C_ADDR_IOX);
    s_touch = add_dev(BOARD_I2C_ADDR_TOUCH);
    return s_gauge && s_iox && s_touch ? HAL_OK : HAL_E_IO;
}

static int rd(i2c_master_dev_handle_t d, uint8_t reg, uint8_t *buf, size_t n)
{
    if (!d)
        return HAL_E_IO;
    bus_lock();
    esp_err_t e = i2c_master_transmit_receive(d, &reg, 1, buf, n, I2C_TMO_MS);
    bus_unlock();
    return e == ESP_OK ? HAL_OK : HAL_E_IO;
}

static int wr(i2c_master_dev_handle_t d, uint8_t reg, const uint8_t *buf, size_t n)
{
    uint8_t b[24];
    if (!d || n + 1 > sizeof b)
        return HAL_E_IO;
    b[0] = reg;
    memcpy(b + 1, buf, n);
    bus_lock();
    esp_err_t e = i2c_master_transmit(d, b, n + 1, I2C_TMO_MS);
    bus_unlock();
    return e == ESP_OK ? HAL_OK : HAL_E_IO;
}

/* ------------------------------------------------------------------ TCA9535 */

/* One register at a time: the register pointer only toggles within a pair (bring-up cmd_i2c.c). */
static int iox_rd(uint8_t reg, uint8_t *v) { return rd(s_iox, reg, v, 1); }
static int iox_wr(uint8_t reg, uint8_t v) { return wr(s_iox, reg, &v, 1); }

/* Stock config_tca95535_pins_for_lp: P0 0,2,3,4,5,7 inputs, P0_6 (C5_EN) output low, P0_1 (OTG) as it is;
 * P1 8,9,11,12,13 (EPD_OE, GMODE, PMIC PWRUP, VCOM_CTRL, WAKEUP) outputs low, 10 (gauge reset), 14, 15 inputs.
 * Only while the panel rails are off (boot before the panel, deep sleep). Output registers first. */
static void iox_lowpower(void)
{
    uint8_t o0, c0, o1;
    if (iox_rd(2, &o0) || iox_rd(6, &c0) || iox_rd(3, &o1)) {
        hal_logf("W TCA9535 does not answer: pins not set for low power");
        return;
    }
    int rc = iox_wr(2, (uint8_t)(o0 & ~0x40u));
    rc |= iox_wr(6, (uint8_t)((c0 & 0x02u) | 0xbdu));
    rc |= iox_wr(3, (uint8_t)(o1 & ~0x3bu));
    rc |= iox_wr(7, 0xc4);
    if (rc)
        hal_logf("W TCA9535 low-power write failed");
}

/* Charger power good (P0_0, low = VBUS present), cached for a second. */
/* ESP32-C5 modem power (esp/net_c5.c): EN (P0_6) low; on: the straps (P0_4 SPI_BOOT, P0_5 USB_BOOT) as inputs
 * (normal boot, stock modem_reset_target) and EN high after 100 ms off. Only P0 4..6 change. */
int hal_x_c5_power(bool on)
{
    uint8_t o0, c0;
    bus_lock();
    int rc = iox_rd(2, &o0) || iox_rd(6, &c0) ? HAL_E_IO : HAL_OK;
    if (!rc) {
        rc |= iox_wr(2, (uint8_t)(o0 & ~(1u << IOX_C5_EN)));
        rc |= iox_wr(6, (uint8_t)((c0 & ~(1u << IOX_C5_EN)) | (1u << IOX_C5_SPI_BOOT) | (1u << IOX_C5_USB_BOOT)));
    }
    bus_unlock();
    if (rc || !on)
        return rc ? HAL_E_IO : HAL_OK;
    vTaskDelay(pdMS_TO_TICKS(100));
    bus_lock();
    rc = iox_wr(2, (uint8_t)(o0 | (1u << IOX_C5_EN)));
    bus_unlock();
    return rc ? HAL_E_IO : HAL_OK;
}

static bool vbus(void)
{
    int64_t now = esp_timer_get_time();
    if (now - s_vbus_at < 1000000)
        return s_vbus;
    uint8_t in;
    if (iox_rd(0, &in) == HAL_OK) {
        s_vbus = !(in & 0x01);
        s_vbus_at = now;
    }
    return s_vbus;
}

bool hal_x_vbus(void) { return s_iox && vbus(); }

/* ------------------------------------------------------------------ BQ27427 */

int hal_battery_mv(void)
{
    uint8_t b[2];
    if (rd(s_gauge, 0x04, b, 2)) {
        hal_stats.batt_mv = HAL_E_IO;
        return HAL_E_IO;
    }
    int mv = b[0] | b[1] << 8;
    if (mv < 2500 || mv > 5000)
        mv = HAL_E_IO; /* not configured or in reset */
    hal_stats.batt_mv = mv;
    return mv;
}

/* Log only (the percentage comes from mV, power.c): whether the gauge is configured. Flags 0x06 (ITPOR 0x0020: reset,
 * defaults until a config is written), RemainingCapacity 0x0c, FullChargeCapacity 0x0e (mAh). -1: not read. */
static void gauge_log(void)
{
    static const uint8_t regs[] = {0x04, 0x1c, 0x06, 0x0c, 0x0e};
    int v[sizeof regs];
    for (size_t i = 0; i < sizeof regs; i++) {
        uint8_t b[2];
        v[i] = rd(s_gauge, regs[i], b, 2) ? -1 : b[0] | b[1] << 8;
    }
    if (v[0] >= 0)
        hal_logf("I gauge mv=%d soc=%d flags=%04x itpor=%d rem=%d full=%d", v[0], v[1], v[2] < 0 ? 0 : v[2],
                 v[2] < 0 ? -1 : !!(v[2] & 0x0020), v[3], v[4]);
}

/* ------------------------------------------------------------------ IQS323 touchbar
 * Follows stock trmnl-firmware f5f87b7 (lib/IQS323/IQS323.cpp v1.5.2 by Azoteq, lib/IQS323/IQS323_config_LP.h,
 * lib/trmnl_x/src/iqs323_task.cpp, src/bl.cpp goToSleep):
 *   init   SW reset, 100 ms, product number (1106 or 1462), wait for SHOW_RESET, write the whole config
 *          (writeMM, the LP config below), ACK_RESET, re-ATI, wait for ATI done, read the data, event mode on
 *          (read back), then the gesture config: EVENTS_ENABLE 0x02 (touch events only, as stock tap mode) and
 *          GESTURE_SELECT 0x09 (tap, hold: stock tap mode's, always: swipes come from the slider coordinate,
 *          touch_btn.h; NVS "xswipe" 0 turns them off; on by default since 2026-10-01, docs/devices/trmnl-x-touch.md).
 *          Up to 3 attempts, an MCLR pulse on the RDY line between them (stock
 *          iqs323_hardware_reset). Every transfer asks for a window first if RDY is high (write 0xFF, wait up to
 *          100 ms for RDY low: stock force_I2C_communication) and ends with a STOP, which closes the window.
 *   sleep  event mode keeps RDY high until a touch or release; ext0 wake on RDY low (sys.c).
 * Ours on top: after a STOP we wait for RDY to go high again before the next transfer (a transfer started while
 * the window is still closing reads 0xEE filler bytes: hardware 2026-09-25 "sysctrl=eeee", whose bit 7 and touch
 * bits made a chip reset and a touch that never ended); a read of only 0xEE bytes is invalid. Stock runs the full
 * init on every boot; we run it on a cold boot and when the chip shows a reset, and trust the config on deep-sleep
 * wakes (a timer wake then does no I2C with the chip at all: few forced windows, each of which may be NACKed, and
 * the IDF 5.5 i2c_master spins without yielding until the bus is idle after a NACK). Awake, only windows the chip
 * opens are used (x_touch_iqs.h iqs_service: the touch event's window starts the stream, the release's next report
 * ends it; datasheet 8.13, issue #140); forced only where no window can come: the wake's press, down without the
 * stream (once a second, touch_btn.h), once an awake hour and before the sleep. A touch longer than 30 s is
 * treated as released; 8 touch wakes within a minute turn the touch wake off until the next restart. NVS "xtouch"
 * = 0 turns the touch bar off ("!tp touch off"). Every read takes status, gestures and the slider coordinate (6 bytes).
 * A press's zone (its first touched read: left, right, else the middle) is its input (hal_inputs_down).
 * Awake, a task reads the bar (hardware 2026-09-28: every touch during a 35 s refresh was lost, nothing read it): woken
 * by RDY falling (GPIO interrupt) or every 20 ms while down; the app's hal_button_down reads it as before. Every press
 * that ends by a release goes to the queue (board.c, src/btn_queue.h) for hal_button_take, also the one the app
 * watched (it takes it back at the release: presses never merge, none is lost). The task
 * logs through a small buffer the app flushes (hal_logf reaches the device log from the app's thread only). Around a
 * light sleep the bus is held (no transfer in flight) and the interrupt off; before the deep sleep the task stops.
 * Lock-up (hardware 2026-09-28: after a busy session every register read 0, status 0000 without SHOW_RESET, no
 * touch reported until "!tp touch init", which needed an MCLR; the datasheet's known issue for v1.3, issue #140): a
 * status read of one constant byte is invalid; it, or a read without an answer, gets a second opinion (a probe read
 * of a register that does not exist, x_touch_iqs.h iqs_health) and a confirmed lock-up a hard reset and a full init
 * (log `W touchbar lockup ...`, then `I touchbar recovered`). Once an awake hour and before every deep sleep one
 * forced status read checks for it (2026-10-07: the 2 s check was what forced, and all six lock-ups of the day
 * were found by it with nothing else running; forcing is what makes the lock-up likelier).
 * Log: a line per press (`touchbar press`), `touchbar hour` with the counters, every read with NVS "xtlog" = 1
 * ("!tp touch log on"). */

static bool s_touch_ok;           /* initialised (this boot or before the deep sleep) and in event mode */
static tbtn_t s_btn;              /* the bar as a button (src/touch_btn.h): every status read goes in */
static const char *s_btn_why;     /* last logged forced end of a press */
static uint8_t s_st[2], s_g[2];   /* last valid SYSTEM_STATUS and GESTURES */
static uint16_t s_sl = 0xffff;    /* last valid slider coordinate */
static bool s_tlog;               /* NVS xtlog: every read logged */
static bool s_swipe;              /* NVS xswipe (default 1): swipes by the slider; 0: taps by zone only */
static uint16_t s_prod;
static uint8_t s_ver[2];
static volatile uint32_t s_task_loops; /* reader wakes (hal_x_diag) */
static uint32_t s_n_reads, s_n_bad, s_n_nowin, s_n_ev; /* status reads, invalid reads, no window, event windows */
static uint32_t s_n_forced;                             /* windows we asked for (the lock-up's likely trigger) */
static RTC_DATA_ATTR bool s_iqs_ready;          /* configured by an earlier boot of this power-on */
static RTC_DATA_ATTR uint64_t s_storm_t0;       /* RTC time of the first touch wake of the current minute */
static RTC_DATA_ATTR uint8_t s_storm_n;
static RTC_DATA_ATTR bool s_storm_off;
static TaskHandle_t s_task;       /* the reader (reader_start) */
static volatile bool s_stop;      /* the deep sleep is near: the task stays out */
static uint8_t s_zero_run;        /* constant status reads in a row (a lock-up, iqs_status_rd) */
static uint8_t s_n_lost;          /* lock-ups since boot (hal_x_diag, the log) */
static bool s_init_busy;          /* iqs_init runs: its status reads are taken as they are */

/* The task's log lines wait here for the app's thread (tlog_flush); lines beyond the buffer are counted. The app
 * flushes when it reads the bar (hal_button_down): during WiFi or a task that can be half a minute, so a line written
 * 1 s or more before its flush says how long before (`late_ms=`; hardware 2026-09-28: presses read during a 33 s
 * photo fetch all logged at its end, timed right). */
#define TLOG_N 24
static char s_tlog_buf[TLOG_N][176];
static int64_t s_tlog_at[TLOG_N];
static uint8_t s_tlog_n;
static uint32_t s_tlog_lost;
static portMUX_TYPE s_tlog_mux = portMUX_INITIALIZER_UNLOCKED;

static void tlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void tlog(const char *fmt, ...)
{
    char b[sizeof s_tlog_buf[0]];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (!s_task || xTaskGetCurrentTaskHandle() != s_task) {
        hal_logf("%s", b);
        return;
    }
    portENTER_CRITICAL(&s_tlog_mux);
    if (s_tlog_n < TLOG_N) {
        s_tlog_at[s_tlog_n] = esp_timer_get_time();
        memcpy(s_tlog_buf[s_tlog_n++], b, sizeof b);
    } else
        s_tlog_lost++;
    portEXIT_CRITICAL(&s_tlog_mux);
}

static void tlog_flush(void)
{
    char b[sizeof s_tlog_buf[0]];
    for (uint8_t i = 0;; i++) {
        bool more;
        uint32_t lost = 0;
        int64_t at = 0;
        portENTER_CRITICAL(&s_tlog_mux);
        more = i < s_tlog_n;
        if (more) {
            memcpy(b, s_tlog_buf[i], sizeof b);
            at = s_tlog_at[i];
        } else {
            s_tlog_n = 0;
            lost = s_tlog_lost;
            s_tlog_lost = 0;
        }
        portEXIT_CRITICAL(&s_tlog_mux);
        if (!more) {
            if (lost)
                hal_logf("W touchbar log lines lost=%u", (unsigned)lost);
            return;
        }
        int64_t late = (esp_timer_get_time() - at) / 1000;
        if (late >= 1000)
            hal_logf("%s late_ms=%u", b, (unsigned)late);
        else
            hal_logf("%s", b);
    }
}

static void press_end(void);
#include "x_touch_iqs.h"

static bool kv_flag(const char *key, bool dflt)
{
    uint8_t v = dflt;
    return hal_kv_get(key, &v, 1) == 1 ? v != 0 : dflt;
}

static bool touch_enabled(void)
{
#if CONFIG_TINY_LINK_UART0
    return false;
#else
    uint8_t v = 1;
    return !(hal_kv_get("xtouch", &v, 1) == 1 && v == 0);
#endif
}

static void reader_start(void);

static void touch_init(void)
{
    gpio_config_t c = {.pin_bit_mask = 1ull << HAL_PIN_BUTTON, .mode = GPIO_MODE_INPUT,
                       .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&c);
    s_touch_ok = false;
    s_tlog = kv_flag("xtlog", false);
    s_swipe = kv_flag("xswipe", true);
    uint16_t sl = 0;
    if (hal_kv_get("xswsl", &sl, sizeof sl) == sizeof sl)
        s_btn.sl_min = sl; /* the slider travel of a swipe (touch_btn.h), "!tp touch sl <n>" */
    if (!touch_enabled()) {
        hal_logf("W touchbar off (NVS xtouch=0, \"!tp touch on\"): no button, no touch wake");
        return;
    }
    bool deep = esp_reset_reason() == ESP_RST_DEEPSLEEP;
    if (deep && s_iqs_ready) {
        s_touch_ok = true;
        if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
            /* a touch woke us: its state now (the event window may have timed out during the boot) */
            if (iqs_status_rd(true) == HAL_OK) {
                iqs_apply_status();
                if (s_btn.down) {
                    s_btn.since_us = 0; /* still touched: the press began at the wake (esp_timer counts from the boot) */
                    iqs_event_mode(false); /* the event's window is gone (the boot): one forced window for the stream */
                } else
                    tbtn_wake(&s_btn, s_st[1], s_g[0]); /* over already: a swipe the chip still shows */
                hal_logf("touchbar wake st=%02x%02x g=%02x%02x sl=%04x down=%d swipe=%d", s_st[1], s_st[0], s_g[1],
                         s_g[0], s_sl, s_btn.down, s_btn.swipe);
            }
            uint64_t now = hal_rtc_us();
            if (!s_storm_n || now - s_storm_t0 > 60000000ull) {
                s_storm_t0 = now;
                s_storm_n = 0;
            }
            if (++s_storm_n >= IQS_STORM_WAKES && !s_storm_off) {
                s_storm_off = true;
                hal_logf("W touchbar woke the device %u times in a minute: touch wake off until a restart", s_storm_n);
            }
        }
        reader_start();
        return;
    }
    s_storm_off = false;
    s_storm_n = 0;
    s_touch_ok = iqs_init();
    reader_start();
}

/* A press's zone (docs/devices/trmnl-x-touch.md): its first touched read, left (CH0) or right (CH2) alone; when that
 * read is two zones, its last read if that is one zone (a hold that starts on an edge and ends in one zone); else (the
 * middle, none read: a tap from sleep released before the first read) the middle */
static uint8_t zone_input(void)
{
    uint8_t z = s_btn.ntrail ? s_btn.trail[0] : 0;
    uint8_t l = s_btn.ntrail ? s_btn.trail[s_btn.ntrail - 1] : 0;
    if (z != 1 && z != 4 && (l == 1 || l == 4))
        z = l;
    return z == 1 ? TINY_INPUT_LEFT : z == 4 ? TINY_INPUT_RIGHT : TINY_INPUT_TOUCH;
}

/* A press ended (released, or ended by a rule): one log line (its length, the zones as they changed: bit 0 left, 1
 * middle, 2 right; the chip's hold gesture and the zone input; with swipes on also the first and last slider
 * coordinate, the chip's swipe gesture, the swipe and what made it one). One that ended by a release goes to the
 * queue (hal_button_take) with its zone and swipe, watched by the app or not; else its zone is the input
 * hal_inputs_woke reports. */
static void press_end(void)
{
    char z[TBTN_TRAIL * 2 + 1] = "";
    for (int i = 0; i < s_btn.ntrail; i++)
        z[i * 2] = (char)('0' + s_btn.trail[i]), z[i * 2 + 1] = i + 1 < s_btn.ntrail ? '>' : 0;
    s_btn.ended = false;
    uint8_t in = zone_input();
    int8_t sw = tbtn_swipe_take(&s_btn);
    if (s_btn.why && !strcmp(s_btn.why, "rest")) { /* a resting object (touch_btn.h): no input, no press mark */
        tlog("touch resting ms=%u zone=%s", (unsigned)((s_btn.last_us - s_btn.since_us) / 1000),
             in == TINY_INPUT_LEFT ? "left" : in == TINY_INPUT_RIGHT ? "right" : "middle");
        return;
    }
    bool queue = !s_btn.why, full = false, brush = tbtn_brush(&s_btn) && !(s_swipe && sw);
    if (queue) {
        int64_t now = esp_timer_get_time();
        uint64_t rtc = hal_rtc_us();
        full = !hal_esp_button_queue(rtc - (uint64_t)(now - s_btn.since_us), rtc - (uint64_t)(now - s_btn.last_us), in,
                                     s_swipe ? sw : 0, brush);
    } else {
        hal_esp_button_input = in;
        hal_esp_button_brush = brush;
    }
    unsigned ms = (unsigned)((s_btn.last_us - s_btn.since_us) / 1000);
    const char *zn = z[0] ? z : "-", *inn = in == TINY_INPUT_LEFT ? "left" : in == TINY_INPUT_RIGHT ? "right" : "middle",
               *end = s_btn.why ? s_btn.why : "release", *drop = full ? " dropped=1" : "", *br = brush ? " brush=1" : "";
    char odd[40] = ""; /* samples skipped, a late release (touch_btn.h "Rejected samples", "Late release") */
    if (s_btn.jumps || s_btn.late_us)
        snprintf(odd, sizeof odd, " jumps=%u late_ms=%u", s_btn.jumps, (unsigned)(s_btn.late_us / 1000));
    if (s_swipe)
        tlog("touchbar press ms=%u zones=%s sl=%d..%d gswipe=%d hold=%d swipe=%d by=%s input=%s end=%s%s%s%s", ms, zn,
             s_btn.sl0, s_btn.sl1, s_btn.gswipe, s_btn.hold, sw, tbtn_by_name(s_btn.by), inn, end, br, drop, odd);
    else
        tlog("touchbar press ms=%u zones=%s sl=%d..%d hold=%d input=%s end=%s%s%s%s", ms, zn, s_btn.sl0, s_btn.sl1,
             s_btn.hold, inn, end, br, drop, odd);
}

/* RDY fell (a window): wake the task. A level type left by a light-sleep wake (sys.c) would fire again and again:
 * off then (hal_esp_light_sleep_pins turns it on again as an edge). */
static void rdy_isr(void *arg)
{
    (void)arg;
    if (GPIO.pin[HAL_PIN_BUTTON].int_type >= GPIO_INTR_LOW_LEVEL)
        gpio_ll_intr_disable(&GPIO, HAL_PIN_BUTTON);
    BaseType_t hp = pdFALSE;
    if (s_task)
        vTaskNotifyGiveFromISR(s_task, &hp);
    if (hp)
        portYIELD_FROM_ISR();
}

static void reader(void *arg)
{
    (void)arg;
    for (;;) {
        /* down: a report streams every ~16 ms and a forced read may be due; up: an event window wakes us */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s_btn.down ? 20 : 200));
        s_task_loops++;
        if (s_stop)
            continue;
        bus_lock();
        iqs_step_begin();
        if (!s_stop && s_touch_ok)
            iqs_service(esp_timer_get_time());
        if (!s_stop && !s_touch_ok)
            iqs_check("retry");
        else if (!s_stop && !s_btn.down)
            iqs_idle_turn(esp_timer_get_time());
        iqs_step_end();
        bus_unlock();
    }
}

/* The task, pinned to core 0 (FastEPD drives rows on core 1), above the app's thread (5), and the RDY interrupt. */
static void reader_start(void)
{
    if (s_task || !s_mx)
        return;
    esp_err_t e = gpio_install_isr_service(0);
    if ((e == ESP_OK || e == ESP_ERR_INVALID_STATE) &&
        xTaskCreatePinnedToCore(reader, "touchbar", 4096, NULL, 6, &s_task, 0) == pdPASS) {
        gpio_set_intr_type(HAL_PIN_BUTTON, GPIO_INTR_NEGEDGE);
        gpio_isr_handler_add(HAL_PIN_BUTTON, rdy_isr, NULL);
        gpio_intr_enable(HAL_PIN_BUTTON);
        return;
    }
    s_task = NULL;
    hal_logf("W touchbar task not started: the bar is read only while the app looks");
}

/* Around every light sleep (sys.c): no transfer in flight while the chip sleeps (the bus held), the interrupt off
 * (the sleep arms a level wake on RDY); after it, an edge again and the task looks at once. */
void hal_esp_light_sleep_pins(bool hold)
{
    if (!s_task)
        return;
    if (hold) {
        bus_lock();
        gpio_intr_disable(HAL_PIN_BUTTON);
        return;
    }
    gpio_set_intr_type(HAL_PIN_BUTTON, GPIO_INTR_NEGEDGE);
    gpio_intr_enable(HAL_PIN_BUTTON);
    bus_unlock();
    xTaskNotifyGive(s_task);
}

void hal_x_diag(char *out, size_t cap)
{
    snprintf(out, cap, "task_alive=%d loops=%u stop=%d touch_ok=%d reads=%u ev=%u bad=%u forced=%u lockups=%u rdy=%d",
             s_task != NULL, (unsigned)s_task_loops, s_stop, s_touch_ok, (unsigned)s_n_reads, (unsigned)s_n_ev,
             (unsigned)s_n_bad, (unsigned)s_n_forced, (unsigned)s_n_lost, !rdy_low());
}

uint8_t hal_x_inputs_down(void) { return hal_button_down() ? (uint8_t)(1u << zone_input()) : 0; }

int8_t hal_button_swipe(void)
{
#if CONFIG_TINY_LINK_UART0
    return 0;
#else
    int8_t d;
    if (hal_esp_button_taken_swipe(&d)) /* the queued press hal_button_take returned */
        return d;
    bus_lock();
    d = tbtn_swipe_take(&s_btn);
    bus_unlock();
    return s_swipe ? d : 0;
#endif
}

bool hal_button_down(void)
{
#if CONFIG_TINY_LINK_UART0
    return false;
#else
    if (!s_touch_ok && (s_task || !touch_enabled()))
        return false;
    bus_lock();
    if (!s_task) {
        iqs_step_begin();
        if (s_touch_ok)
            iqs_service(esp_timer_get_time());
        else
            iqs_check("app retry");
        iqs_step_end();
    } /* the reader already services input; do not add another recovery after waiting for it */
    int64_t now = esp_timer_get_time(), since = s_btn.since_us;
    bool down = tbtn_down(&s_btn, now);
    bus_unlock();
    /* hal_button_since: this press dated to its first read (a touch wake: to the wake, touch_init); a new press
     * after one released unseen (a hint's refresh) has its own start */
    hal_esp_button_note_at(false, 0);
    hal_esp_button_note_at(down, down ? hal_rtc_us() - (uint64_t)(now - since) : 0);
    tlog_flush();
    return down;
#endif
}

/* "!tp touch" diagnostics. */
static void touch_line(const char *what)
{
    hal_logf("touch %s ok=%d ready=%d prod=%u v%u.%u status=%02x%02x gestures=%02x%02x down=%d hold=%d end=%s rdy=%d "
             "reads=%u bad=%u nowin=%u forced=%u lockups=%u storm=%u/%d",
             what, s_touch_ok, s_iqs_ready, s_prod, s_ver[0], s_ver[1], s_st[1], s_st[0], s_g[1], s_g[0], s_btn.down,
             s_btn.hold, s_btn.why ? s_btn.why : "-", !rdy_low(),
             (unsigned)s_n_reads, (unsigned)s_n_bad, (unsigned)s_n_nowin, (unsigned)s_n_forced, s_n_lost, s_storm_n,
             s_storm_off);
}

/* Watch the bar for `secs`: RDY sampled every 5 ms, each window (RDY low) read, a forced read every second. One
 * line per read: t (ms), how (event/forced), rc, the 18 bytes from 0x10 (status, gestures, slider, ch0..2
 * counts and LTA). */
static void touch_watch(int secs)
{
    int64_t t0 = esp_timer_get_time(), next_forced = t0;
    unsigned lows = 0, events = 0, forced = 0, bad = 0, touched = 0;
    bool was_low = false;
    hal_logf("touch watch s=%d: touch and hold the bar, release, tap; then leave it alone", secs);
    while (esp_timer_get_time() - t0 < (int64_t)secs * 1000000) {
        bool low = rdy_low();
        lows += low;
        bool ev = low && !was_low;
        was_low = low;
        int64_t now = esp_timer_get_time();
        if (ev || now >= next_forced) {
            uint8_t b[18] = {0};
            int rc = iqs_rd(IQS_STATUS, b, sizeof b, !ev);
            if (!ev) {
                forced++;
                next_forced = now + 1000000;
            } else {
                events++;
            }
            if (rc == HAL_E_IO)
                bad++;
            if (rc == HAL_OK) {
                iqs_feed(b); /* the button sees these reads too: a press across the watch ends by the gap rule */
                touched += (b[1] & IQS_TOUCH_BITS) != 0;
            }
            hal_logf("touch t=%u %s rc=%d st=%02x%02x g=%02x%02x sl=%02x%02x c0=%02x%02x/%02x%02x c1=%02x%02x/%02x%02x "
                     "c2=%02x%02x/%02x%02x",
                     (unsigned)((now - t0) / 1000), ev ? "event" : "forced", rc, b[1], b[0], b[3], b[2], b[5], b[4],
                     b[7], b[6], b[9], b[8], b[11], b[10], b[13], b[12], b[15], b[14], b[17], b[16]);
            hal_link_flush();
            was_low = rdy_low();
        }
        wait_us(5000);
    }
    hal_logf("touch watch end rdy_low_samples=%u events=%u forced=%u bad=%u touched=%u", lows, events, forced, bad,
             touched);
}

static void touch_cmd(int argc, char **argv);

void hal_x_touch_cmd(int argc, char **argv)
{
    bus_lock(); /* the task waits: the command's reads and the watch are the only ones */
    touch_cmd(argc, argv);
    bus_unlock();
    tlog_flush();
}

static void touch_cmd(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[0], "sl")) {
        uint16_t v = (uint16_t)atoi(argv[1]);
        hal_kv_set("xswsl", &v, sizeof v);
        s_btn.sl_min = v;
        hal_logf("I touchbar swipe slider travel %u (NVS xswsl, 0: %d)", v, TBTN_SWIPE_SL);
        return;
    }
    if (argc >= 1 && (!strcmp(argv[0], "on") || !strcmp(argv[0], "off"))) {
        uint8_t v = argv[0][1] == 'n';
        hal_kv_set("xtouch", &v, 1);
        hal_logf("I touchbar %s (NVS xtouch=%u), applies at the next restart (\"!tp reboot\")", v ? "on" : "off", v);
        return;
    }
    if (argc >= 2 && (!strcmp(argv[0], "log") || !strcmp(argv[0], "swipe"))) {
        uint8_t v = !strcmp(argv[1], "on");
        bool log = argv[0][0] == 'l';
        hal_kv_set(log ? "xtlog" : "xswipe", &v, 1);
        if (log)
            s_tlog = v;
        else
            s_swipe = v;
        hal_logf("I touchbar %s %s (NVS %s=%u)%s", argv[0], v ? "on" : "off", log ? "xtlog" : "xswipe", v,
                 log ? "" : ", applies at the next init (\"!tp touch init\")");
        return;
    }
    if (argc >= 1 && !strcmp(argv[0], "init")) {
        s_touch_ok = iqs_init();
        reader_start();
        touch_line("init");
        return;
    }
    if (argc >= 1 && !strcmp(argv[0], "watch")) {
        int secs = argc >= 2 ? atoi(argv[1]) : 10;
        touch_watch(secs < 1 ? 1 : secs > 60 ? 60 : secs);
        touch_line("after");
        return;
    }
    uint8_t sc[2] = {0}, ev[2] = {0}, gs[2] = {0};
    int a = iqs_status_rd(true), b = iqs_rd(IQS_SYSCTRL, sc, 2, true), c = iqs_rd(IQS_EVENTS, ev, 2, true),
        d = iqs_rd(IQS_GESTURE_SEL, gs, 2, true);
    hal_logf("touch rc=%d,%d,%d,%d sysctrl=%02x%02x events=%02x%02x gsel=%02x swipe=%d tlog=%d sl=%04x", a, b, c, d,
             sc[1], sc[0], ev[1], ev[0], gs[0], s_swipe, s_tlog, s_sl);
    touch_line("now");
}

/* ------------------------------------------------------------------ init, sleep */

void hal_io_init(void)
{
#if !CONFIG_TINY_LINK_UART0
    if (hal_x_i2c_init())
        return;
    iox_lowpower(); /* C5 modem off, panel lines low until the panel is opened */
    touch_init();
    gauge_log();
#endif
}

/* Stock config_gpio_for_lp: panel bus and control lines, BOOT, I2C and the C5 UART as plain inputs
 * (no pulls). USB (19/20) and the touch RDY (3) stay as they are. */
static void gpio_lowpower(void)
{
    static const uint8_t pins[] = {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 47,
                                   48, 45, 0, 39, 40, 41, 42, 43, 44, 1, 2};
    uint64_t mask = 0;
    for (size_t i = 0; i < sizeof pins; i++)
        mask |= 1ull << pins[i];
    gpio_config_t c = {.pin_bit_mask = mask, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_DISABLE,
                       .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&c);
}

bool hal_io_sleep_prep(bool button_wake)
{
#if CONFIG_TINY_LINK_UART0
    return button_wake;
#else
    /* the task out for good (the lock is never given back): the bar's last reads are ours */
    s_stop = true;
    if (s_task)
        gpio_intr_disable(HAL_PIN_BUTTON);
    bus_lock();
    tlog_flush();
    iqs_step_begin();
    if (s_touch_ok && s_stream && iqs_event_mode(true))
        hal_logf("W touchbar event mode not restored before the sleep");
    iqs_check("sleep"); /* never asleep with the touch wake dead (hardware 2026-09-28: twice after a busy cycle) */
    tlog_flush();
    /* one line per cycle: the lock-up rate against the forced windows (issue #140), count both per day */
    hal_logf("touchbar sleep forced=%u reads=%u ev=%u bad=%u nowin=%u lockups=%u ok=%d", (unsigned)s_n_forced,
             (unsigned)s_n_reads, (unsigned)s_n_ev, (unsigned)s_n_bad, (unsigned)s_n_nowin, s_n_lost, s_touch_ok);
    if (s_touch_ok && rdy_low() && iqs_status_rd(false) == HAL_OK)
        iqs_apply_status(); /* serve an open window, so RDY is high while we sleep (a reset: init again) */
    iqs_step_end();
    if (s_bus)
        iox_lowpower();
    gpio_lowpower();
    if (button_wake && (!s_touch_ok || s_storm_off))
        button_wake = false; /* not in event mode (RDY may pulse every ~220 ms), or a wake storm */
    if (button_wake) {
        esp_rom_delay_us(2000);
        if (rdy_low()) {
            hal_logf("W touch RDY stays low: no touch wake this sleep");
            button_wake = false;
        }
    }
    return button_wake;
#endif
}

/* link diag `input.extra` (esp/diag.c): the touch bar as "!tp touch" shows it */
void hal_esp_board_diag(char *out, size_t cap)
{
    snprintf(out, cap, "touch ok=%d ready=%d prod=%u v%u.%u status=%02x%02x gestures=%02x%02x down=%d hold=%d rdy=%d "
             "reads=%u bad=%u nowin=%u forced=%u lockups=%u storm=%u/%d vbus=%d",
             s_touch_ok, s_iqs_ready, s_prod, s_ver[0], s_ver[1], s_st[1], s_st[0], s_g[1], s_g[0], s_btn.down,
             s_btn.hold, !rdy_low(), (unsigned)s_n_reads, (unsigned)s_n_bad, (unsigned)s_n_nowin,
             (unsigned)s_n_forced, s_n_lost, s_storm_n, s_storm_off, (int)s_vbus);
}

#endif /* BOARD_BATT_GAUGE && !BOARD_X4P */
