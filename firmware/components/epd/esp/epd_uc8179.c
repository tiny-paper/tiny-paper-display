/*
 * epd_uc8179.c - UC8179 driver for the TRMNL OG panel (see epd.h).
 * Register values: bitbank2/bb_epaper a9863c9 (GPL-3.0) and ZinggJM/GxEPD2 GxEPD2_750_GDEY075T7 (GPL-3.0).
 * 4-gray: bb_epaper epd75_gray_init / epd75_old_gray_init, data layout as usetrmnl/trmnl-firmware f5f87b7 (epd_gray.h).
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "epd.h"
#include "epd_gray.h"
#include "epd_lut.h"
#include "epd_rects.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd";

/* UC8179 commands */
enum {
    C_PSR = 0x00, C_PWR = 0x01, C_POF = 0x02, C_PON = 0x04, C_BTST = 0x06, C_DSLP = 0x07,
    C_DTM1 = 0x10, C_DRF = 0x12, C_DTM2 = 0x13, C_DUSPI = 0x15, C_LUTC = 0x20,
    C_VDCS = 0x82, C_TSE = 0x41, C_CDI = 0x50, C_TCON = 0x60, C_TRES = 0x61,
    C_PTL = 0x90, C_PTIN = 0x91, C_PTOU = 0x92, C_CCSET = 0xe0, C_PWS = 0xe3, C_TSSET = 0xe5,
};

#define T_RESET_MS    200
#define T_PON_MS      1000
#define T_POF_MS      1000
#define T_FULL_MS     40000
#define T_PARTIAL_MS  10000
#define CHUNK         4096

static struct {
    epd_pins_t pins;
    epd_cfg_t cfg;
    spi_device_handle_t dev;
    bool open, powered, asleep, ram_valid, begun;
    epd_mode_t mode;
    epd_rects_t wr;               /* written areas and their union (epd_rects.h) */
    epd_stats_t st;
} s;

static uint8_t s_buf[CHUNK];   /* DMA bounce buffer (internal RAM) */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static int spi_send(const uint8_t *p, size_t n)
{
    while (n) {
        size_t k = n > CHUNK ? CHUNK : n;
        if (p != s_buf) memcpy(s_buf, p, k);
        spi_transaction_t t = { .length = k * 8, .tx_buffer = s_buf };
        if (spi_device_polling_transmit(s.dev, &t) != ESP_OK) return EPD_E_IO;
        p += k;
        n -= k;
    }
    return EPD_OK;
}

static int cmd(uint8_t c)
{
    gpio_set_level(s.pins.dc, 0);
    gpio_set_level(s.pins.cs, 0);
    int rc = spi_send(&c, 1);
    gpio_set_level(s.pins.cs, 1);
    return rc;
}

static int data(const uint8_t *p, size_t n)
{
    gpio_set_level(s.pins.dc, 1);
    gpio_set_level(s.pins.cs, 0);
    int rc = spi_send(p, n);
    gpio_set_level(s.pins.cs, 1);
    s.st.bytes += n;
    return rc;
}

static int cmdv(uint8_t c, const uint8_t *p, size_t n)
{
    int rc = cmd(c);
    if (rc == EPD_OK && n) rc = data(p, n);
    return rc;
}
#define CMD(c, ...) do { static const uint8_t d_[] = { __VA_ARGS__ }; int r_ = cmdv((c), d_, sizeof d_); if (r_) return r_; } while (0)

__attribute__((weak)) void epd_wait_hook(void) {}
__attribute__((weak)) bool epd_wait_sleep(int busy_pin, uint32_t max_ms)
{
    (void)busy_pin;
    (void)max_ms;
    return false;
}

/* BUSY_N is low while the controller works. */
static int wait_busy(uint32_t timeout_ms, uint32_t *out_ms, const char *what)
{
    uint32_t t0 = now_ms();
    esp_rom_delay_us(200);
    while (gpio_get_level(s.pins.busy) == 0) {
        uint32_t el = now_ms() - t0;
        if (el > timeout_ms) {
            s.st.timeouts++;
            ESP_LOGE(TAG, "BUSY timeout after %lu ms (%s)", (unsigned long)el, what);
            if (out_ms) *out_ms = el;
            return EPD_E_TIMEOUT;
        }
        if (epd_wait_sleep(s.pins.busy, timeout_ms - el + 1))
            continue; /* woke on BUSY high (or the timeout): check again */
        vTaskDelay(1);
        epd_wait_hook();
    }
    uint32_t dt = now_ms() - t0;
    if (out_ms) *out_ms = dt;
    ESP_LOGI(TAG, "%s: BUSY %lu ms", what, (unsigned long)dt);
    return EPD_OK;
}

static int hw_reset(void)
{
    gpio_set_level(s.pins.rst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(s.pins.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    s.powered = false;
    s.asleep = false;
    s.ram_valid = false;
    s.begun = false;
    return wait_busy(T_RESET_MS, &s.st.reset_ms, "reset");
}

/* After any failure: stop driving (POF), then reset the controller. */
static int fail(int rc, const char *what)
{
    ESP_LOGE(TAG, "%s failed (%d): power off + reset", what, rc);
    s.st.last_err = rc;
    cmd(C_POF);
    wait_busy(T_POF_MS, NULL, "POF (recovery)");
    s.powered = false;
    hw_reset();
    return rc;
}

bool epd_ram_valid(void) { return s.ram_valid; }
bool epd_is_open(void) { return s.open; }
const epd_stats_t *epd_stats(void) { return &s.st; }

const char *epd_part_wave_name(epd_part_wave_t w)
{
    return w == EPD_PART_GX ? "GX" : w == EPD_PART_LUT ? "LUT" : "OTP";
}

epd_gray_wave_t epd_gray_wave(epd_variant_t v, epd_gray_wave_t w)
{
    if (w == EPD_GRAY_OTP || w == EPD_GRAY_LUT)
        return w;
    (void)v; /* LUT on both: OTP on gen2 drew the drop shadows black (hardware check, 2026-09-24) */
    return EPD_GRAY_LUT;
}

const char *epd_gray_wave_name(epd_gray_wave_t w)
{
    return w == EPD_GRAY_OTP ? "OTP" : w == EPD_GRAY_LUT ? "LUT" : "AUTO";
}

const char *epd_variant_name(epd_variant_t v)
{
    return v == EPD_GEN2 ? "GEN2 (GEDY075-D2)" : "GDEY075T7";
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

int epd_open(const epd_pins_t *pins, const epd_cfg_t *cfg)
{
    if (s.open) return EPD_E_STATE;
    memset(&s, 0, sizeof s);
    s.pins = *pins;
    s.cfg = *cfg;
    out_pin(pins->cs, 1);
    out_pin(pins->dc, 1);
    out_pin(pins->rst, 1);
    in_pin(pins->busy);
    spi_bus_config_t bus = {
        .mosi_io_num = pins->mosi, .miso_io_num = -1, .sclk_io_num = pins->sck,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = CHUNK,
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return EPD_E_IO;
    spi_device_interface_config_t dc = {
        .mode = 0, .clock_speed_hz = (int)pins->spi_hz, .spics_io_num = -1, .queue_size = 1,
    };
    if (spi_bus_add_device(SPI2_HOST, &dc, &s.dev) != ESP_OK) {
        spi_bus_free(SPI2_HOST);
        return EPD_E_IO;
    }
    s.open = true;
    ESP_LOGI(TAG, "open: %s, partial %s%s, reps %u, vcom 0x%02x, gray %s temp 0x%02x, SPI %lu Hz",
             epd_variant_name(cfg->variant), epd_part_wave_name(cfg->part_wave), cfg->window_refresh ? ", windowed" : "",
             epd_lut_reps_clamp(cfg->lut_reps), cfg->lut_vcom, epd_gray_wave_name(epd_gray_wave(cfg->variant, cfg->gray_wave)),
             cfg->gray_temp ? cfg->gray_temp : EPD_GRAY_TEMP_DEFAULT, (unsigned long)pins->spi_hz);
    int rc = hw_reset();
    if (rc) {
        ESP_LOGE(TAG, "BUSY stuck low after reset: panel missing or not a UC81xx?");
        epd_close();
    }
    return rc;
}

/* 4-gray full refresh. GEN2: bb_epaper epd75_gray_init (EP75_800x480_4GRAY_GEN2, what stock TRMNL firmware
 * uses on the OG for 2-bit images): OTP waveform at forced temperature 0x5f, CDI DDX 00. GDEY075T7:
 * bb_epaper epd75_old_gray_init (EP75_800x480_4GRAY): register LUTs. TRES is sent in both (bb_epaper
 * relies on the controller state for it). PON follows in epd_refresh. */
static int begin_gray(void)
{
    int rc;
    epd_gray_wave_t w = epd_gray_wave(s.cfg.variant, s.cfg.gray_wave);
    if (w == EPD_GRAY_LUT) {
        CMD(C_PWR, 0x07, 0x07, 0x3f, 0x3f, 0x03);
        CMD(C_PSR, 0x3f);                          /* LUT from registers */
        CMD(C_TRES, 0x03, 0x20, 0x01, 0xe0);
        CMD(C_DUSPI, 0x00);
        CMD(C_CDI, 0x00, 0x07);                    /* white border, DDX 00 */
        CMD(C_TCON, 0x22);
        CMD(C_VDCS, EPD_GRAY_VCOM_LUT);
        static uint8_t l[EPD_LUT_N][EPD_LUT_LEN];
        epd_gray_luts(l);
        for (int i = 0; i < EPD_LUT_N; i++)
            if ((rc = cmdv(C_LUTC + i, l[i], EPD_LUT_LEN))) return fail(rc, "LUT");
        CMD(C_CCSET, 0x00);
        CMD(C_TSE, 0x00);
    } else {
        uint8_t t = s.cfg.gray_temp ? s.cfg.gray_temp : EPD_GRAY_TEMP_DEFAULT;
        CMD(C_PSR, 0x1f);                          /* LUT from OTP */
        CMD(C_TRES, 0x03, 0x20, 0x01, 0xe0);
        CMD(C_CDI, 0x90, 0x07);                    /* border floating, DDX 00 */
        CMD(C_BTST, 0x27, 0x27, 0x18, 0x17);
        CMD(C_CCSET, 0x02);                        /* temperature from TSSET */
        if ((rc = cmdv(C_TSSET, &t, 1))) return fail(rc, "TSSET");
    }
    s.begun = true;
    ESP_LOGI(TAG, "begin GRAY4 (%s%s)", epd_gray_wave_name(w), w == EPD_GRAY_OTP ? ", forced temperature" : "");
    return EPD_OK;
}

int epd_begin(epd_mode_t mode)
{
    if (!s.open) return EPD_E_STATE;
    int rc;
    if (s.asleep && (rc = hw_reset())) return fail(rc, "reset");
    s.mode = mode;
    epd_rects_reset(&s.wr);
    s.st.bytes = 0;
    if (mode == EPD_GRAY4) {
        s.ram_valid = false; /* the planes will hold gray data, not the last 1-bit frame */
        return begin_gray();
    }
    bool lut = mode == EPD_PARTIAL && s.cfg.part_wave != EPD_PART_OTP;
    bool gx = lut && s.cfg.part_wave == EPD_PART_GX;

    if (gx) CMD(C_PWR, 0x07, 0x07, 0x3f, 0x3f, 0x09);  /* GxEPD2 _InitDisplay: + VDHR 4.2 V */
    else CMD(C_PWR, 0x07, 0x07, 0x3f, 0x3f);       /* VGH/VGL 20 V, VDH/VDL 15 V */
    if (s.cfg.variant == EPD_GEN2) CMD(C_BTST, 0x27, 0x27, 0x18, 0x17);   /* bb_epaper GEN2 */
    else CMD(C_BTST, 0x17, 0x17, 0x28, 0x17);                           /* GxEPD2 GDEY075T7 */
    if (lut) CMD(C_PSR, 0x3f); else CMD(C_PSR, 0x1f);  /* KW mode; LUT from registers / OTP */
    CMD(C_TRES, 0x03, 0x20, 0x01, 0xe0);           /* 800 x 480 */
    CMD(C_DUSPI, 0x00);
    if (gx) CMD(C_CDI, 0x39, 0x07);                /* GxEPD2 _Init_Part: LUTBD, N2OCP */
    else CMD(C_CDI, 0x29, 0x07);                   /* N2OCP: new copied to old after refresh; 1 = white */
    CMD(C_TCON, 0x22);
    CMD(C_PWS, 0x22);
    switch (mode) {
    case EPD_FULL:
        CMD(C_CCSET, 0x00);                        /* temperature from the internal sensor */
        CMD(C_TSE, 0x00);
        break;
    case EPD_FULL_FAST:
        CMD(C_CCSET, 0x02);
        CMD(C_TSSET, 0x5a);                        /* forced 90: fast full OTP waveform */
        break;
    case EPD_PARTIAL:
        if (lut) {
            static uint8_t l[EPD_LUT_N][EPD_LUT_LEN];
            epd_lutset_t set = gx ? EPD_LUTSET_GX : EPD_LUTSET_BB;
            unsigned k = epd_lut_build(set, s.cfg.lut_reps, l);
            int vcom = epd_lut_vcom(set, s.cfg.lut_vcom);
            if (vcom >= 0) {
                uint8_t v = (uint8_t)vcom;
                if ((rc = cmdv(C_VDCS, &v, 1))) return fail(rc, "VDCS");
            }
            for (int i = 0; i < EPD_LUT_N; i++)
                if ((rc = cmdv(C_LUTC + i, l[i], EPD_LUT_LEN))) return fail(rc, "LUT");
            ESP_LOGI(TAG, "LUT %s x%u: %u frames, vcom %d", gx ? "GX" : "BB", k, epd_lut_frames(l[3]), vcom);
        } else {
            CMD(C_CCSET, 0x02);
            CMD(C_TSSET, 0x6e);                    /* forced 110: fast partial OTP waveform */
        }
        break;
    default:
        return EPD_E_ARG;
    }
    s.begun = true;
    ESP_LOGI(TAG, "begin %s", mode == EPD_FULL ? "FULL" : mode == EPD_FULL_FAST ? "FULL_FAST" :
                              gx ? "PARTIAL (GX)" : lut ? "PARTIAL (LUT)" : "PARTIAL (OTP)");
    return EPD_OK;
}

static int set_window(int x, int y, int w, int h)
{
    int xe = (x + w - 1) | 7, ye = y + h - 1;
    uint8_t d[9] = { (uint8_t)(x >> 8), (uint8_t)(x & 0xf8), (uint8_t)(xe >> 8), (uint8_t)xe,
                     (uint8_t)(y >> 8), (uint8_t)y, (uint8_t)(ye >> 8), (uint8_t)ye, 0x01 };
    return cmdv(C_PTL, d, sizeof d);
}

/* gray: 0 = src is 1-bit, 1 = 2-bit src to its DTM1 plane, 2 = to its DTM2 plane */
static int send_plane(uint8_t c, const tiny_rect_t *r, const uint8_t *src, size_t stride, uint8_t fill, int gray)
{
    int rc = cmd(c);
    if (rc) return rc;
    size_t wb = r->w / 8, n = 0;
    gpio_set_level(s.pins.dc, 1);
    gpio_set_level(s.pins.cs, 0);
    for (int row = 0; row < r->h && rc == EPD_OK; row++) {
        if (n + wb > CHUNK) { rc = spi_send(s_buf, n); n = 0; }
        if (src && gray) epd_gray_planes(src + (size_t)row * stride, r->w, gray == 1 ? s_buf + n : NULL,
                                         gray == 2 ? s_buf + n : NULL);
        else if (src) memcpy(s_buf + n, src + (size_t)row * stride, wb);
        else memset(s_buf + n, fill, wb);
        n += wb;
    }
    if (rc == EPD_OK && n) rc = spi_send(s_buf, n);
    gpio_set_level(s.pins.cs, 1);
    s.st.bytes += wb * r->h;
    return rc;
}

static int write_area(const tiny_rect_t *r, const uint8_t *prev, uint8_t prev_fill, bool old_plane,
                      const uint8_t *next, uint8_t next_fill, size_t stride)
{
    if (!s.begun) return EPD_E_STATE;
    if (r->x < 0 || r->y < 0 || (r->x & 7) || (r->w & 7) || r->w == 0 || r->h == 0 ||
        r->x + r->w > EPD_W || r->y + r->h > EPD_H) {
        ESP_LOGE(TAG, "bad area %d,%d %ux%u (x, w must be multiples of 8)", r->x, r->y, r->w, r->h);
        return EPD_E_ARG;
    }
    int rc;
    if ((rc = cmd(C_PTIN)) || (rc = set_window(r->x, r->y, r->w, r->h))) return fail(rc, "window");
    if (s.mode == EPD_GRAY4 && next) {
        if ((rc = send_plane(C_DTM1, r, next, stride, 0, 1))) return fail(rc, "DTM1");
        if ((rc = send_plane(C_DTM2, r, next, stride, 0, 2))) return fail(rc, "DTM2");
    } else {
        if (old_plane && (rc = send_plane(C_DTM1, r, prev, stride, prev_fill, 0))) return fail(rc, "DTM1");
        if ((rc = send_plane(C_DTM2, r, next, stride, next_fill, 0))) return fail(rc, "DTM2");
    }
    if ((rc = cmd(C_PTOU))) return fail(rc, "PTOU");
    epd_rects_add(&s.wr, r);
    return EPD_OK;
}

int epd_write(const tiny_rect_t *r, const uint8_t *prev, const uint8_t *next, size_t stride)
{
    if (!next) return EPD_E_ARG;
    if (s.mode == EPD_PARTIAL && !prev && !s.ram_valid) {
        ESP_LOGE(TAG, "partial write without old data while controller RAM is not valid");
        return EPD_E_ARG;
    }
    /* FULL without prev: OLD = 0 so every pixel goes through a transition */
    bool old_plane = prev != NULL || s.mode != EPD_PARTIAL;
    return write_area(r, prev, 0x00, old_plane, next, 0, stride);
}

int epd_fill(uint8_t old_byte, uint8_t new_byte)
{
    tiny_rect_t r = { 0, 0, EPD_W, EPD_H };
    return write_area(&r, NULL, old_byte, true, NULL, new_byte, 0);
}

int epd_refresh(void)
{
    if (!s.begun) return EPD_E_STATE;
    if (s.wr.writes == 0) {
        ESP_LOGW(TAG, "refresh with nothing written");
        return EPD_E_STATE;
    }
    bool whole = s.wr.ux0 == 0 && s.wr.uy0 == 0 && s.wr.ux1 == EPD_W && s.wr.uy1 == EPD_H;
    bool windowed = s.mode == EPD_PARTIAL && !whole && (s.cfg.window_refresh || !s.ram_valid);
    /* Controller RAM outside the written areas is garbage after its deep sleep: refreshing the union of
     * several areas showed that garbage as noise in the gaps on the real OG (2026-09-24). Refresh each
     * written area in its own window instead. */
    tiny_rect_t win[EPD_MAX_RECTS];
    int passes = windowed ? epd_rects_windows(&s.wr, s.ram_valid, win, EPD_MAX_RECTS) : 1;
    if (s.mode != EPD_PARTIAL && !whole && !s.ram_valid)
        ESP_LOGW(TAG, "full refresh of a partly written RAM: unwritten areas show noise");
    int rc;
    if ((rc = cmd(C_PON))) return fail(rc, "PON");
    s.powered = true;
    if ((rc = wait_busy(T_PON_MS, &s.st.power_on_ms, "PON"))) return fail(rc, "PON");
    uint32_t total_ms = 0;
    for (int i = 0; i < passes; i++) {
        if (windowed) {
            tiny_rect_t w = win[i];
            ESP_LOGI(TAG, "windowed refresh %d,%d %dx%d (%d/%d)", w.x, w.y, w.w, w.h, i + 1, passes);
            if ((rc = cmd(C_PTIN)) || (rc = set_window(w.x, w.y, w.w, w.h)))
                return fail(rc, "window");
        }
        static const uint8_t drf0 = 0x00;           /* bb_epaper sends DRF 0x00 for 4-gray */
        if ((rc = s.mode == EPD_GRAY4 ? cmdv(C_DRF, &drf0, 1) : cmd(C_DRF))) return fail(rc, "DRF");
        rc = wait_busy(s.mode == EPD_PARTIAL ? T_PARTIAL_MS : T_FULL_MS, &s.st.refresh_ms, "refresh");
        if (rc) return fail(rc, "refresh");
        total_ms += s.st.refresh_ms;
        if (windowed) cmd(C_PTOU);
    }
    s.st.refresh_ms = total_ms;
    if ((rc = cmd(C_POF))) return fail(rc, "POF");
    rc = wait_busy(T_POF_MS, &s.st.power_off_ms, "POF");
    s.powered = false;
    if (rc) return fail(rc, "POF");
    /* N2OCP copied NEW to OLD: RAM matches the screen if the whole panel was written (not in 4-gray) */
    if (whole && s.mode != EPD_GRAY4) s.ram_valid = true;
    s.begun = false;
    return EPD_OK;
}

int epd_sleep(void)
{
    if (!s.open) return EPD_E_STATE;
    if (s.asleep) return EPD_OK;
    if (s.powered) {
        cmd(C_POF);
        wait_busy(T_POF_MS, &s.st.power_off_ms, "POF");
        s.powered = false;
    }
    uint8_t chk = 0xa5;
    int rc = cmdv(C_DSLP, &chk, 1);
    s.asleep = true;
    s.ram_valid = false;
    s.begun = false;
    ESP_LOGI(TAG, "deep sleep");
    return rc;
}

void epd_close(void)
{
    if (!s.open) return;
    epd_sleep();
    spi_bus_remove_device(s.dev);
    spi_bus_free(SPI2_HOST);
    in_pin(s.pins.sck);
    in_pin(s.pins.mosi);
    in_pin(s.pins.dc);
    /* CS and RST stay high: panel deselected, not in reset */
    s.open = false;
    ESP_LOGI(TAG, "closed");
}

/* ---------------------------------------------------------------- bit-banged register read */

static void bb_write(const epd_pins_t *p, uint8_t v)
{
    for (int i = 0; i < 8; i++, v <<= 1) {
        gpio_set_level(p->mosi, (v & 0x80) ? 1 : 0);
        esp_rom_delay_us(1);
        gpio_set_level(p->sck, 1);
        esp_rom_delay_us(1);
        gpio_set_level(p->sck, 0);
    }
}

static uint8_t bb_read(const epd_pins_t *p)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) {
        gpio_set_level(p->sck, 1);
        esp_rom_delay_us(1);
        gpio_set_level(p->sck, 0);
        esp_rom_delay_us(1);
        v = (uint8_t)((v << 1) | gpio_get_level(p->mosi));
    }
    return v;
}

int epd_read_reg(const epd_pins_t *p, uint8_t c, uint8_t *out, size_t n)
{
    if (s.open) return EPD_E_STATE;
    out_pin(p->sck, 0);
    out_pin(p->cs, 1);
    out_pin(p->rst, 1);
    out_pin(p->dc, 1);
    in_pin(p->busy);
    gpio_set_level(p->rst, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(p->rst, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    int busy = gpio_get_level(p->busy);
    ESP_LOGI(TAG, "after reset BUSY=%d (%s)", busy, busy ? "idle: UC81xx" : "low: SSD16xx or no panel");
    int rc = EPD_OK;
    if (!busy) {
        rc = EPD_E_STATE;
    } else {
        out_pin(p->mosi, 0);
        gpio_set_level(p->dc, 0);
        gpio_set_level(p->cs, 0);
        bb_write(p, c);
        gpio_set_level(p->dc, 1);
        in_pin(p->mosi);
        for (size_t i = 0; i < n; i++) out[i] = bb_read(p);
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
    return rc;
}
