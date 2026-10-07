/*
 * epd_bwry.c - GDEY042F51-family BWRY panel driver (see epd_bwry.h for sources and profiles).
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "epd_bwry.h"
#include "epd_bwpart.h"
#include "epd_bwseq.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bwry";

enum { C_POF = 0x02, C_PON = 0x04, C_DSLP = 0x07, C_DTM = 0x10, C_AREA = 0x83 }; /* the rest: epd_bwseq.c */

#define T_RESET_MS     1000
#define T_PON_MS       2000
#define T_INIT_MS      5000    /* waits inside the init (LUT registers, 0xA5, TSC); tesserae EPD_INIT_TIMEOUT_MS */
#define T_POF_MS       2000
#define T_TSC_MS       500     /* temperature measurement (TSC) */
#define T_REFRESH_MS   45000   /* measured ~18.8 s (open-PicPak); GxEPD2 allows 30 s; cold panels are slower */
#define CHUNK          4000    /* 40 rows */

static struct {
    bwry_pins_t pins;
    bwry_cfg_t cfg;
    spi_device_handle_t dev;
    bool open, powered, asleep, streaming, part;
    bool waiting;              /* inside wait_busy: epd_wait_hook may run link commands (bwq_allowed) */
    int sent;                  /* rows of the open stream */
    bwry_stats_t st;
} s;

static uint8_t s_buf[CHUNK];   /* DMA bounce buffer (internal RAM) */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* The entry gate (epd_bwseq.h bwq_allowed): false, logged, when e may not run now */
static bool may(bwq_entry_t e, const char *what)
{
    bwq_gate_t g = { s.open, s.waiting };
    if (bwq_allowed(&g, e)) return true;
    if (e != BWQ_E_SPI || s.waiting) ESP_LOGE(TAG, "%s refused: %s", what, s.waiting ? "inside a BUSY wait" : "not open");
    return false;
}

static int spi_raw(const uint8_t *p, size_t n)
{
    if (!may(BWQ_E_SPI, "SPI")) return BWRY_E_STATE;   /* never a transfer to a removed device */
    spi_transaction_t t = { .length = n * 8, .tx_buffer = p };
    return spi_device_polling_transmit(s.dev, &t) == ESP_OK ? BWRY_OK : BWRY_E_IO;
}

static int cmd(uint8_t c)
{
    s_buf[0] = c;
    gpio_set_level(s.pins.dc, 0);
    gpio_set_level(s.pins.cs, 0);
    int rc = spi_raw(s_buf, 1);
    gpio_set_level(s.pins.cs, 1);
    return rc;
}

static int cmdv(uint8_t c, const uint8_t *p, size_t n)
{
    int rc = cmd(c);
    if (rc || !n) return rc;
    memcpy(s_buf, p, n);
    gpio_set_level(s.pins.dc, 1);
    gpio_set_level(s.pins.cs, 0);
    rc = spi_raw(s_buf, n);
    gpio_set_level(s.pins.cs, 1);
    return rc;
}

__attribute__((weak)) void epd_wait_hook(void) {}
/* Light sleep through a BUSY wait (hal_esp panel_bwry.c): true = it slept (woke on BUSY high, a press or its cap). */
__attribute__((weak)) bool bwry_wait_sleep(int busy_pin, uint32_t max_ms)
{
    (void)busy_pin;
    (void)max_ms;
    return false;
}

/* BUSY is low while the controller works. */
static int wait_busy(uint32_t timeout_ms, uint32_t *out_ms, const char *what)
{
    uint32_t t0 = now_ms();
    esp_rom_delay_us(200);
    s.waiting = true;
    while (gpio_get_level(s.pins.busy) == 0) {
        if (now_ms() - t0 > timeout_ms) {
            s.waiting = false;
            s.st.timeouts++;
            ESP_LOGE(TAG, "BUSY timeout after %lu ms (%s)", (unsigned long)(now_ms() - t0), what);
            if (out_ms) *out_ms = now_ms() - t0;
            return BWRY_E_TIMEOUT;
        }
        bool slept = bwry_wait_sleep(s.pins.busy, timeout_ms - (now_ms() - t0) + 1);
        if (!slept) vTaskDelay(pdMS_TO_TICKS(5));
        epd_wait_hook();
    }
    s.waiting = false;
    uint32_t dt = now_ms() - t0;
    if (out_ms) *out_ms = dt;
    ESP_LOGI(TAG, "%s: BUSY %lu ms", what, (unsigned long)dt);
    return BWRY_OK;
}

static int hw_reset(void)
{
    gpio_set_level(s.pins.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(s.pins.rst, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(s.pins.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    s.powered = false;
    s.asleep = false;
    return wait_busy(T_RESET_MS, &s.st.reset_ms, "reset");
}

static int power_off(void)
{
    if (!s.powered) return BWRY_OK;
    s.powered = false;
    int rc = cmdv(C_POF, (const uint8_t[]){ 0x00 }, 1);
    if (!rc) rc = wait_busy(T_POF_MS, &s.st.power_off_ms, "POF");
    return rc;
}

/* After any failure: stop driving (POF), then reset the controller. */
static int fail(int rc, const char *what)
{
    s.st.last_err = rc;
    s.streaming = false;
    s.part = false;
    bwq_gate_t g = { s.open, s.waiting };
    if (!bwq_fail_recover(&g)) {
        ESP_LOGE(TAG, "%s failed (%d): driver closed, no power off", what, rc);
        return rc;
    }
    ESP_LOGE(TAG, "%s failed (%d): power off + reset", what, rc);
    s.powered = true;
    power_off();
    hw_reset();
    return rc;
}

bool bwry_is_open(void) { return s.open; }
bool bwry_busy(void) { return s.waiting; }
const bwry_stats_t *bwry_stats(void) { return &s.st; }
const char *bwry_init_name(bwry_init_t i) { return i == BWRY_INIT_GD ? "gd (GxEPD2 GDEY0420F51)" : "stock (0x060401)"; }
const char *bwry_fast_name(uint8_t f)
{
    return f == BWRY_FAST_TSE ? "tse" : f == BWRY_FAST_E6 ? "e6" : f == BWRY_FAST_E6P ? "e6p" : "off";
}

static void out_pin(int pin, int level)
{
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, level);
}

static void in_pin(int pin)
{
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_FLOATING);
}

int bwry_open(const bwry_pins_t *pins, const bwry_cfg_t *cfg)
{
    if (s.open || !may(BWQ_E_OPEN, "open")) return BWRY_E_STATE;
    memset(&s, 0, sizeof s);
    s.pins = *pins;
    s.cfg = *cfg;
    if (pins->other_cs >= 0) out_pin(pins->other_cs, 1);
    out_pin(pins->cs, 1);
    out_pin(pins->dc, 1);
    out_pin(pins->rst, 1);
    in_pin(pins->busy);
    spi_bus_config_t bus = {
        .mosi_io_num = pins->mosi, .miso_io_num = -1, .sclk_io_num = pins->sck,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = CHUNK,
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return BWRY_E_IO;
    spi_device_interface_config_t dc = {
        .mode = 0, .clock_speed_hz = (int)pins->spi_hz, .spics_io_num = -1, .queue_size = 1,
    };
    if (spi_bus_add_device(SPI2_HOST, &dc, &s.dev) != ESP_OK) {
        spi_bus_free(SPI2_HOST);
        return BWRY_E_IO;
    }
    s.open = true;
    ESP_LOGI(TAG, "open: init %s, SPI %lu Hz", bwry_init_name(cfg->init), (unsigned long)pins->spi_hz);
    int rc = hw_reset();
    if (rc) {
        ESP_LOGE(TAG, "BUSY stuck low after reset: ribbon cable or panel?");
        bwry_close();
    }
    return rc;
}

void bwry_set_speed(uint8_t fast, const uint8_t *lut)
{
    s.cfg.fast = fast;
    s.cfg.lut = s.cfg.init == BWRY_INIT_STOCK ? lut : NULL;
}

void bwry_set_pll(uint8_t pll) { s.cfg.pll = pll; }

static bwq_cfg_t seq_cfg(void) { return (bwq_cfg_t){ (uint8_t)s.cfg.init, s.cfg.fast, s.cfg.lut, s.cfg.pll }; }

/* Run a command sequence (epd_bwseq.h: the order and the bytes are host-tested there). */
static int run(const bwq_op_t *o, int n)
{
    int rc = BWRY_OK;
    for (int i = 0; i < n && !rc; i++, o++) {
        uint32_t ms;
        switch (o->op) {
        case BWQ_CMD:
            rc = cmdv(o->cmd, o->d, o->n);
            break;
        case BWQ_PON:
            rc = cmd(C_PON);
            s.powered = true;   /* also when the command failed: fail() powers off */
            break;
        case BWQ_DELAY:
            vTaskDelay(pdMS_TO_TICKS(o->cmd));
            break;
        case BWQ_WAIT:
            if (o->cmd == BWQ_W_PON) rc = wait_busy(T_PON_MS, &s.st.power_on_ms, "PON");
            else if (o->cmd == BWQ_W_REFRESH)
                rc = wait_busy(s.cfg.refresh_timeout_ms ? s.cfg.refresh_timeout_ms : T_REFRESH_MS, &s.st.refresh_ms,
                               "refresh");
            else rc = wait_busy(T_INIT_MS, &ms, "init");
            break;
        case BWQ_LUT:
            if (!s.cfg.lut) return BWRY_E_STATE;
            if ((rc = cmd(o->cmd))) break;
            bwq_lut_payload(s.cfg.lut, s_buf);
            gpio_set_level(s.pins.dc, 1);
            gpio_set_level(s.pins.cs, 0);
            rc = spi_raw(s_buf, BWQ_LUT_OUT);
            gpio_set_level(s.pins.cs, 1);
            break;
        default:
            return BWRY_E_ARG;
        }
    }
    return rc;
}

/* [reset if asleep], init; PON only for BWRY_FAST_E6P (bwq_early_pon), else after the data (bwq_refresh). */
static int begin(void)
{
    int rc = s.asleep ? hw_reset() : BWRY_OK;
    if (rc) return rc;
    bwq_cfg_t c = seq_cfg();
    bwq_op_t ops[BWQ_MAX];
    return run(ops, bwq_init(&c, ops));
}

/* After the data: PON (unless early), border, DRF, the refresh wait. */
static int refresh(bool partial)
{
    bwq_cfg_t c = seq_cfg();
    bwq_op_t ops[BWQ_MAX];
    return run(ops, bwq_refresh(&c, partial, ops));
}

bool bwry_rows_bottom_up(const bwry_cfg_t *cfg)
{
    if (cfg->flip_y >= 0) return cfg->flip_y != 0;
    return cfg->init == BWRY_INIT_STOCK;   /* stock PSR 0x07 scans bottom-up (open-PicPak device.md 4) */
}

typedef struct {
    const uint8_t *fb;
    uint8_t fill;
} fb_src_t;

/* fb == NULL: `fill` everywhere */
static void fb_row(void *ctx, int y, uint8_t *row)
{
    const fb_src_t *f = ctx;
    if (f->fb) memcpy(row, f->fb + y * BWRY_STRIDE, BWRY_STRIDE);
    else memset(row, f->fill, BWRY_STRIDE);
}

/* Streaming: init, AREA (stock), DTM; then data bytes in chunks, CS low per chunk only (DC stays high: the controller
 * keeps writing DTM data until the next command, as with one CS per byte); PON after the data (bwry_stream_end). */
int bwry_stream_begin(void)
{
    if (!may(BWQ_E_BEGIN, "stream begin") || s.streaming || s.part) return BWRY_E_STATE;
    int rc = begin();
    if (rc) return fail(rc, "init");
    bwq_cfg_t c = seq_cfg();
    bwq_op_t ops[BWQ_MAX];
    if ((rc = run(ops, bwq_stream_head(&c, ops)))) return fail(rc, "DTM");
    s.streaming = true;
    s.sent = 0;
    return BWRY_OK;
}

int bwry_stream_rows(bwry_row_fn fn, void *ctx, int n)
{
    if (!s.streaming || !may(BWQ_E_WRITE, "write")) return BWRY_E_STATE;
    if (!fn || n < 0 || s.sent + n > BWRY_H) return BWRY_E_ARG;
    bool flip = bwry_rows_bottom_up(&s.cfg);
    const int rows = CHUNK / BWRY_STRIDE;
    int rc = BWRY_OK;
    while (n > 0 && !rc) {
        int k = n < rows ? n : rows;
        for (int i = 0; i < k; i++) {
            int p = s.sent + i;
            fn(ctx, flip ? BWRY_H - 1 - p : p, s_buf + i * BWRY_STRIDE);
        }
        gpio_set_level(s.pins.dc, 1);
        gpio_set_level(s.pins.cs, 0);
        rc = spi_raw(s_buf, (size_t)k * BWRY_STRIDE);
        gpio_set_level(s.pins.cs, 1);
        s.sent += k;
        s.st.bytes += (uint32_t)k * BWRY_STRIDE;
        n -= k;
    }
    if (rc) return fail(rc, "write");
    return BWRY_OK;
}

int bwry_stream_sent(void) { return s.streaming ? s.sent : 0; }

void bwry_stream_abort(void)
{
    if ((!s.streaming && !s.part) || !may(BWQ_E_END, "abort")) return;
    s.streaming = false;
    s.part = false;
    power_off();
}

/* ---------------------------------------------------------------- windowed partial (experimental) */

static int ptlw(int x, int y, int w, int h, uint8_t pmode)
{
    bwp_win_t win;
    if (bwp_map(x, y, w, h, BWRY_W, BWRY_H, bwry_rows_bottom_up(&s.cfg), 8, &win) != BWP_OK) return BWRY_E_ARG;
    uint8_t b[9];
    bwp_ptlw_bytes(&win, pmode, b);
    return cmdv(C_AREA, b, sizeof b);
}

int bwry_part_begin(void)
{
    if (!may(BWQ_E_BEGIN, "partial begin") || s.streaming || s.part) return BWRY_E_STATE;
    int rc = begin();
    if (rc) return fail(rc, "init/PON");
    s.part = true;
    return BWRY_OK;
}

int bwry_part_window(int x, int y, int w, int h, bwry_wrow_fn fn, void *ctx)
{
    if (!s.part || !may(BWQ_E_WRITE, "window")) return BWRY_E_STATE;
    if (!fn) return BWRY_E_ARG;
    int rc = ptlw(x, y, w, h, 0x01);
    if (rc == BWRY_E_ARG) return rc;   /* nothing sent: the caller aborts */
    if (!rc) rc = cmd(C_DTM);
    if (rc) return fail(rc, "window");
    bool flip = bwry_rows_bottom_up(&s.cfg);
    const int bpr = w / 4, rows = CHUNK / bpr;
    for (int done = 0; done < h && !rc;) {
        int k = h - done < rows ? h - done : rows;
        for (int i = 0; i < k; i++) {
            int p = done + i;
            fn(ctx, flip ? y + h - 1 - p : y + p, s_buf + i * bpr, x, w);
        }
        gpio_set_level(s.pins.dc, 1);
        gpio_set_level(s.pins.cs, 0);
        rc = spi_raw(s_buf, (size_t)k * bpr);
        gpio_set_level(s.pins.cs, 1);
        s.st.bytes += (uint32_t)k * bpr;
        done += k;
    }
    if (rc) return fail(rc, "window write");
    return BWRY_OK;
}

int bwry_part_end(int x, int y, int w, int h)
{
    if (!s.part || !may(BWQ_E_END, "partial end")) return BWRY_E_STATE;
    s.part = false;
    int rc = ptlw(x, y, w, h, 0x01);   /* the refresh runs on the last window set: the union */
    if (!rc) rc = refresh(true);
    if (rc) return fail(rc, "partial refresh");
    s.st.refreshes++;
    if ((rc = power_off())) return fail(rc, "POF");
    return BWRY_OK;
}

int bwry_stream_end(void)
{
    if (!s.streaming || !may(BWQ_E_END, "stream end")) return BWRY_E_STATE;
    int rc = BWRY_OK;
    if (s.sent < BWRY_H) {
        ESP_LOGW(TAG, "stream: %d of %d rows, rest white", s.sent, BWRY_H);
        fb_src_t f = { NULL, bwry_fill_byte(BWRY_WHITE) };
        rc = bwry_stream_rows(fb_row, &f, BWRY_H - s.sent);
        if (rc) return rc;   /* already failed and reset */
    }
    s.streaming = false;
    if ((rc = refresh(false))) return fail(rc, "refresh");
    s.st.refreshes++;
    if ((rc = power_off())) return fail(rc, "POF");
    return BWRY_OK;
}

static int show(bwry_row_fn fn, void *ctx)
{
    int rc = bwry_stream_begin();
    if (!rc) rc = bwry_stream_rows(fn, ctx, BWRY_H);
    if (!rc) rc = bwry_stream_end();
    return rc;
}

int bwry_show(const uint8_t *fb)
{
    if (!fb) return BWRY_E_ARG;
    fb_src_t f = { fb, 0 };
    return show(fb_row, &f);
}

int bwry_show_rows(bwry_row_fn fn, void *ctx)
{
    if (!fn) return BWRY_E_ARG;
    return show(fn, ctx);
}

int bwry_fill(uint8_t color)
{
    fb_src_t f = { NULL, bwry_fill_byte(color) };
    return show(fb_row, &f);
}

int bwry_sleep(void)
{
    if (!may(BWQ_E_SLEEP, "sleep")) return BWRY_E_STATE;
    if (s.asleep) return BWRY_OK;
    s.streaming = false;
    s.part = false;
    int rc = power_off();
    int rc2 = cmdv(C_DSLP, (const uint8_t[]){ 0xa5 }, 1);
    s.asleep = true;
    return rc ? rc : rc2;
}

void bwry_close(void)
{
    if (!s.open || !may(BWQ_E_CLOSE, "close")) return;
    if (!s.asleep) bwry_sleep();
    spi_bus_remove_device(s.dev);
    spi_bus_free(SPI2_HOST);
    gpio_set_level(s.pins.cs, 1);
    gpio_set_level(s.pins.rst, 1);
    s.open = false;
    ESP_LOGI(TAG, "closed");
}

/* ---------------------------------------------------------------- bit-banged register read */

static void bb_write(const bwry_pins_t *p, uint8_t v)
{
    for (int i = 0; i < 8; i++, v <<= 1) {
        gpio_set_level(p->mosi, (v & 0x80) ? 1 : 0);
        esp_rom_delay_us(2);
        gpio_set_level(p->sck, 1);
        esp_rom_delay_us(2);
        gpio_set_level(p->sck, 0);
    }
}

static uint8_t bb_read(const bwry_pins_t *p, int pin)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) {
        gpio_set_level(p->sck, 1);
        esp_rom_delay_us(2);
        v = (uint8_t)((v << 1) | gpio_get_level(pin));
        gpio_set_level(p->sck, 0);
        esp_rom_delay_us(2);
    }
    return v;
}

/* measure: the command starts a measurement (TSC): CS high, wait for BUSY, then read in a new CS frame. */
static int read_cmd(const bwry_pins_t *p, uint8_t c, uint8_t *out, size_t n, bool use_miso, bool measure)
{
    if (s.open || s.waiting) return BWRY_E_STATE;
    if (use_miso && p->miso < 0) return BWRY_E_ARG;
    if (p->other_cs >= 0) out_pin(p->other_cs, 1);
    out_pin(p->sck, 0);
    out_pin(p->mosi, 0);
    out_pin(p->cs, 1);
    out_pin(p->rst, 1);
    out_pin(p->dc, 1);
    in_pin(p->busy);
    if (use_miso) in_pin(p->miso);
    gpio_set_level(p->rst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(p->rst, 1);
    uint32_t t0 = now_ms();
    while (gpio_get_level(p->busy) == 0 && now_ms() - t0 < T_RESET_MS) vTaskDelay(pdMS_TO_TICKS(5));
    int busy = gpio_get_level(p->busy);
    ESP_LOGI(TAG, "after reset BUSY=%d (%lu ms)", busy, (unsigned long)(now_ms() - t0));
    int rc = BWRY_OK;
    if (!busy) {
        rc = BWRY_E_STATE;
    } else {
        gpio_set_level(p->dc, 0);
        gpio_set_level(p->cs, 0);
        bb_write(p, c);
        if (measure) {
            gpio_set_level(p->cs, 1);
            t0 = now_ms();
            esp_rom_delay_us(200);
            while (gpio_get_level(p->busy) == 0 && now_ms() - t0 < T_TSC_MS) vTaskDelay(pdMS_TO_TICKS(2));
            if (gpio_get_level(p->busy) == 0) rc = BWRY_E_TIMEOUT;
            gpio_set_level(p->cs, 0);
        }
        gpio_set_level(p->dc, 1);
        if (!use_miso) in_pin(p->mosi);
        for (size_t i = 0; i < n; i++) out[i] = bb_read(p, use_miso ? p->miso : p->mosi);
        gpio_set_level(p->cs, 1);
        /* back to controller deep sleep */
        out_pin(p->mosi, 0);
        gpio_set_level(p->dc, 0);
        gpio_set_level(p->cs, 0);
        bb_write(p, C_DSLP);
        gpio_set_level(p->dc, 1);
        bb_write(p, 0xa5);
        gpio_set_level(p->cs, 1);
    }
    in_pin(p->sck);
    in_pin(p->mosi);
    in_pin(p->dc);
    if (use_miso) in_pin(p->miso);
    return rc;
}

int bwry_read_reg(const bwry_pins_t *p, uint8_t c, uint8_t *out, size_t n, bool use_miso)
{
    return read_cmd(p, c, out, n, use_miso, false);
}

int bwry_read_temp(const bwry_pins_t *p, int16_t *temp_c, uint8_t raw[2])
{
    raw[0] = raw[1] = 0;
    *temp_c = -128;
    int rc = read_cmd(p, 0x40, raw, 2, false, true);   /* TSC, 3-wire on SDA as the REV read */
    if (!rc) *temp_c = (int8_t)raw[0];                /* D[10:3]: whole degrees, two's complement */
    return rc;
}
