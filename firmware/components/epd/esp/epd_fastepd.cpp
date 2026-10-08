/*
 * epd_fastepd.cpp - C glue over bitbank2/FastEPD (Apache-2.0) for parallel panels (TRMNL X).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "epd_par.h"
#include "epd_parwin.h"
#include "epd_parwave.h"
#include "epd_parjob.h"
#include "epd_scan.h"
#include "epd_split.h"
#include "esp_heap_caps.h"
#include "epd_pins.h"
#include "FastEPD.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/pulse_cnt.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_sig_map.h"
#include "esp_rom_gpio.h"
#include "sdkconfig.h"
#include <string.h>

/* Same sequence as stock trmnl-firmware f5f87b7 src/display.cpp display_init(): initPanel(BB_PANEL_TRMNL_X),
 * setPasses(3, 3); FastEPD 8dc8c74 then runs EPDiyV7IOInit / EPDiyV7EinkPower (TCA9535 at 0x20 pins
 * 8..15, TPS65185 at 0x68). Differences: stock is Arduino, so FastEPD's I2C goes through Wire; here it
 * goes through the IDF i2c_master driver on the bus the caller created (FastEPD built with zeroed locals,
 * components/epd/CMakeLists.txt). The glue checks the expander before and after FastEPD touches it, so a
 * dead bus fails a command instead of hanging in FastEPD's PWR_GOOD wait. */

static const char *TAG = "epdp";

/* FastEPD 8dc8c74 src/FastEPD.inl, not in its header (external linkage in FastEPD.cpp) */
void bbepIODeInit(void);
int bbepIOInit(FASTEPDSTATE *pState);
/* FastEPD 8dc8c74 src/FastEPD.inl: false while a row's data is on its way to the source drivers (bbepWriteRow
 * waits for it before every row; the sequencer of epd_scan.h does before the rows it clocks through itself). A
 * FastEPD that no longer has it fails the link, not the panel. */
extern volatile bool dma_is_done;

/* FastEPD keeps its state protected: read the pins it actually uses. */
class FastEpdX : public FASTEPD {
public:
    int pins(int *out, int max) const
    {
        const BBPANELDEF &d = _state.panelDef;
        int n = 0;
        for (int i = 0; i < d.bus_width && n < max; i++) out[n++] = d.data[i];
        const int ctl[] = { d.ioSPV, d.ioCKV, d.ioSPH, d.ioLE, d.ioCL, d.ioDCDummy, d.ioSDA, d.ioSCL };
        for (int p : ctl)
            if (n < max) out[n++] = (p == BB_NOT_USED) ? -1 : p;
        return n;
    }
    /* Our row control in place of FastEPD's (epd_rowtime.h); bbepWriteRow and bbepClear call it. */
    /* The LCD bus again with hardware chip select at hz (FastEPD 8f76e90's TRMNL X entry: 26.67 MHz, no
     * BB_PANEL_FLAG_SLOW_SPH; 8f76e90 itself does not build under ESP-IDF, pinMode in its M5Paper code). 8dc8c74's
     * entry (20 MHz, SPH toggled by software from the DMA interrupt) is what initPanel set up. Runs FastEPD's IO init
     * again (pin modes, the expander's port 1 directions: iox_setup checks them after). FastEPD keeps toggling SPH
     * by software (its bSlowSPH stays set), which does nothing while the LCD peripheral owns the pad. */
    int rebus(uint32_t hz)
    {
        bbepIODeInit();
        _state.panelDef.bus_speed = hz;
        _state.panelDef.flags &= ~BB_PANEL_FLAG_SLOW_SPH;
        _state.iFlags &= ~BB_PANEL_FLAG_SLOW_SPH;
        return bbepIOInit(&_state);
    }
    uint32_t bus_hz() const { return _state.panelDef.bus_speed; }
    int powered() const { return _state.pwr_on; }
    bool soft_cs() const { return _state.panelDef.flags & BB_PANEL_FLAG_SLOW_SPH; }
    bool mirror_y() const { return _state.iFlags & BB_PANEL_FLAG_MIRROR_Y; }
    /* One FastEPD partial over the `careful` rows of a scan plan (epd_scan.h), `passes` drive frames (0: only its
     * neutral scan), rails left on. FastEPD 8dc8c74 bbepPartialUpdate and bbepClear take the rows to write and
     * their count from the state on every scan, so for the call the frames start at the first careful row and the
     * height is their count; the sequencer (row_ctl) clocks the rows before and after, and sets the height to 0
     * for a neutral scan it leaves out (the row loop of bbepClear then writes nothing). All put back after. */
    int scan_partial(const epd_scan_rows_t &r, int passes)
    {
        const size_t off = (size_t)r.lead * (size_t)(_state.native_width / 8);
        const int h = _state.native_height, pp = _state.iPartialPasses;
        _state.pCurrent += off;
        _state.pPrevious += off;
        _state.native_height = r.careful;
        _state.iPartialPasses = passes;
        int rc = partialUpdate(true, 0, r.careful - 1);
        _state.pCurrent -= off;
        _state.pPrevious -= off;
        _state.native_height = h;
        _state.iPartialPasses = pp;
        return rc;
    }
    void hook_rows(BB_ROW_CONTROL *f)
    {
        s_ckv = (gpio_num_t)_state.panelDef.ioCKV;
        s_spv = (gpio_num_t)_state.panelDef.ioSPV;
        s_le = (gpio_num_t)_state.panelDef.ioLE;
        _state.pfnRowControl = f;
    }
    static gpio_num_t s_ckv, s_spv, s_le;
};
gpio_num_t FastEpdX::s_ckv, FastEpdX::s_spv, FastEpdX::s_le;

/* Row timing (epd_rowtime.h): delays and the fixed drive applied while s_rows_on (partials only), periods and
 * drive times recorded always. */
static epd_row_delay_t s_rowd = {0, 0};
static volatile bool s_rows_on;
static epd_row_stats_t s_row_stats;
static epd_row_drive_stats_t s_drive;
static uint32_t s_drive_set = EPD_ROW_DRIVE_DEFAULT; /* epdp_set_drive */
static volatile uint32_t s_drive_us;                 /* this update: 0 = CKV high until the next step */
static uint32_t s_on_us;                             /* the current row's CKV on */

static inline uint32_t IRAM_ATTR now_us(void) { return (uint32_t)esp_timer_get_time(); }

/* CKV rising edges counted in hardware (PCNT on the pad's input) over each update: one per row and 4 per frame
 * start, or the gate driver is not clocked as the frames assume (issue #203; epd_row_edges_ok). */
static pcnt_unit_handle_t s_pcnt;
static bool ckv_pcnt_init(gpio_num_t ckv)
{
    pcnt_unit_config_t u = {};
    u.low_limit = -1;
    u.high_limit = 32767;
    u.flags.accum_count = 1;
    pcnt_channel_handle_t ch = nullptr;
    pcnt_chan_config_t c = {};
    c.edge_gpio_num = ckv;
    c.level_gpio_num = -1;
    if (pcnt_new_unit(&u, &s_pcnt) != ESP_OK || pcnt_new_channel(s_pcnt, &c, &ch) != ESP_OK ||
        pcnt_channel_set_edge_action(ch, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD) != ESP_OK ||
        pcnt_unit_add_watch_point(s_pcnt, 32767) != ESP_OK || pcnt_unit_enable(s_pcnt) != ESP_OK ||
        pcnt_unit_clear_count(s_pcnt) != ESP_OK || pcnt_unit_start(s_pcnt) != ESP_OK) {
        ESP_LOGW(TAG, "no pulse counter for CKV: edges not counted");
        s_pcnt = nullptr;
        return false;
    }
    return true;
}
static void ckv_pcnt_clear(void)
{
    if (s_pcnt) pcnt_unit_clear_count(s_pcnt);
}
static uint32_t ckv_pcnt_read(void)
{
    int c = 0;
    if (s_pcnt && pcnt_unit_get_count(s_pcnt, &c) != ESP_OK) c = 0;
    return (uint32_t)c;
}

/* Fixed drive: each row's CKV is a GPIO pulse of exactly drive_us, timed with interrupts masked on the panel core
 * (ROM delay, nothing else runs), so a stall cannot lengthen it; the LCD DMA interrupt waits at most that long.
 * It was an RMT symbol restarted per row from its registers (2026-09-26): on the X that put TWO rising edges on
 * CKV per row (hardware count ckv=2811 per 1404-row frame, issue #203), the gate driver advanced two rows per
 * data row and every partial came out twice as tall and twice as far down, rows from 702 past the bottom edge. */
static portMUX_TYPE s_ckv_mux = portMUX_INITIALIZER_UNLOCKED;
static inline void IRAM_ATTR ckv_pulse(gpio_num_t ckv, uint32_t us)
{
    portENTER_CRITICAL(&s_ckv_mux);
    gpio_set_level(ckv, 1);
    s_on_us = now_us();
    esp_rom_delay_us(us);
    gpio_set_level(ckv, 0);
    uint32_t high = now_us() - s_on_us;
    portEXIT_CRITICAL(&s_ckv_mux);
    epd_row_drive_note(&s_drive, high);
}
/* FastEPD 8dc8c74 EPDiyV7RowControl (BB_PANEL_TRMNL_X), ROW_START's pulses unchanged; ROW_STEP with the settle
 * (CKV off -> LE) and hold (LE -> CKV on) delays. Fixed drive: the frame's first and every row's CKV is one
 * ckv_pulse; free: bbepWriteRow turns CKV on after the step returns and it stays on until the next step.
 * What the pad carries with the fixed drive (the hardware count says so: ckv=30977/22 is 22 x 1408 + 1): bbepWriteRow
 * sets CKV high again right after every step, so the pad is high from there through the row's data and the next
 * step's pulse, low only between the pulse's end and that call; one rising edge per row, made by bbepWriteRow. A
 * scan ends with CKV high, so the first start pulse of the next scan is no edge (4 edges in an update's first
 * scan, 3 in the later ones, plus the start's own pulse). */
static void IRAM_ATTR frame_start(void)
{
    const gpio_num_t ckv = FastEpdX::s_ckv, spv = FastEpdX::s_spv;
    const uint32_t drive = s_drive_us;
    s_drive.frames++;
    gpio_set_level(ckv, 1);
    esp_rom_delay_us(7);
    gpio_set_level(spv, 0);
    esp_rom_delay_us(10);
    gpio_set_level(ckv, 0);
    gpio_set_level(ckv, 1);
    esp_rom_delay_us(8);
    gpio_set_level(spv, 1);
    esp_rom_delay_us(10);
    gpio_set_level(ckv, 0);
    gpio_set_level(ckv, 1);
    esp_rom_delay_us(10);
    gpio_set_level(ckv, 0);
    gpio_set_level(ckv, 1);
    esp_rom_delay_us(10);
    gpio_set_level(ckv, 0);
    if (drive)
        ckv_pulse(ckv, drive);
    else {
        gpio_set_level(ckv, 1);
        s_on_us = now_us();
    }
    epd_row_stats_start(&s_row_stats, s_on_us);
}

static void IRAM_ATTR row_step(void)
{
    const gpio_num_t ckv = FastEpdX::s_ckv, le = FastEpdX::s_le;
    const uint32_t drive = s_drive_us;
    uint32_t now = now_us();
    epd_row_stats_step(&s_row_stats, now);
    if (drive)
        epd_row_drive_step(&s_drive, s_on_us, now, drive); /* the pulse ended inside ckv_pulse: stalls only count */
    else {
        epd_row_drive_step(&s_drive, s_on_us, now, 0);
        gpio_set_level(ckv, 0);
    }
    const bool on = s_rows_on;
    if (on && s_rowd.settle) esp_rom_delay_us(s_rowd.settle);
    gpio_set_level(le, 1);
    gpio_set_level(le, 0);
    if (on && s_rowd.hold) esp_rom_delay_us(s_rowd.hold);
    if (drive)
        ckv_pulse(ckv, drive);
    else
        s_on_us = now_us(); /* bbepWriteRow: CKV on right after this returns */
}

/* The scans of a partial (epd_scan.h). Default settings: FastEPD's own scans, row_ctl only counts them. Else the
 * sequencer s_seq runs them (s_seq_on) through SCAN_IO: the frame start and row step above, and the bare gate
 * clocks of the rows it skips. */
static epd_scan_cfg_t s_scan_cfg = EPD_SCAN_CFG_DEFAULT; /* epdp_set_scan */
static epd_scan_cfg_t s_scan_used = EPD_SCAN_CFG_DEFAULT; /* of the update running or run last */
static epd_scan_seq_t s_seq;
static volatile bool s_seq_on;
static bool s_scan_skip; /* this update skips rows */

static void io_frame_start(void *) { frame_start(); }
static void io_row_step(void *) { row_step(); }
static void io_data_wait(void *)
{
    while (!dma_is_done) esp_rom_delay_us(1);
}
static void io_ckv(void *, int level) { gpio_set_level(FastEpdX::s_ckv, (uint32_t)level); }
static void io_delay(void *, uint32_t us) { esp_rom_delay_us(us); }
static uint32_t io_now(void *) { return now_us(); }
static void io_resume(void *, int row)
{
    s_on_us = s_row_stats.last_us = now_us(); /* the skipped rows are no row period and no stall */
    s_row_stats.row = row;
}
static const epd_scan_io_t SCAN_IO = {io_frame_start, io_row_step, io_data_wait, io_ckv, io_delay, io_now, io_resume};

/* Where the time of the update went (epd_split.h). s_first_us: when the library call in progress began its first
 * scan (0: not yet), stamped by row_ctl. */
static epd_split_t s_split;
static volatile uint32_t s_first_us;
static uint32_t s_lap;

static void IRAM_ATTR row_ctl(void *p, int type)
{
    if (type == ROW_START && !s_first_us) s_first_us = now_us() | 1u;
    if (s_seq_on) {
        if (type == ROW_START) {
            if (!epd_scan_seq_start(&s_seq)) ((FASTEPDSTATE *)p)->native_height = 0; /* FastEpdX::scan_partial */
        } else if (type == ROW_STEP) {
            epd_scan_seq_step(&s_seq);
        }
        return;
    }
    if (type == ROW_START) {
        frame_start();
        s_seq.st.frames++;
        s_seq.st.careful++;
    } else if (type == ROW_STEP) {
        row_step();
        s_seq.st.careful++;
    }
}

/* Rows of the next update: partials use the fixed drive when set. */
static void rows_begin(bool partial)
{
    epd_row_stats_reset(&s_row_stats);
    epd_row_drive_reset(&s_drive);
    ckv_pcnt_clear();
    s_drive_us = partial ? s_drive_set : 0;
    s_rows_on = partial;
    s_seq_on = false;
    s_scan_skip = false;
    epd_scan_seq_init(&s_seq, &SCAN_IO, nullptr);
}

static void rows_end(void)
{
    s_rows_on = false;
    s_seq_on = false;
    gpio_set_level(FastEpdX::s_ckv, 0);
    s_drive_us = 0;
    s_drive.edges = ckv_pcnt_read();
}

/* Panel updates run on core 1 at a high priority: the row loop away from WiFi and the app (core 0), and the LCD
 * DMA interrupt with it (esp_lcd allocates it on the core that creates the bus: initPanel runs there too). */
#if CONFIG_FREERTOS_UNICORE
template <class F> static int on_panel_core(F &&f) { return f(); }
#else
#define PANEL_CORE 1
#define PANEL_PRIO (configMAX_PRIORITIES - 2)
template <class F> static int on_panel_core(F &&f)
{
    if (xPortGetCoreID() == PANEL_CORE)
        return f();
    static SemaphoreHandle_t done;
    if (!done)
        done = xSemaphoreCreateBinary();
    struct Job {
        F *f;
        int rc;
    } j = {&f, EPDP_E_INIT};
    auto run = [](void *p) {
        Job *jb = (Job *)p;
        jb->rc = (*jb->f)();
        xSemaphoreGive(done);
        vTaskDelete(nullptr);
    };
    if (!done || xTaskCreatePinnedToCore(run, "epdrows", 16384, &j, PANEL_PRIO, nullptr, PANEL_CORE) != pdPASS) {
        ESP_LOGW(TAG, "no panel task: update on this core");
        return f();
    }
    xSemaphoreTake(done, portMAX_DELAY);
    return j.rc;
}
#endif

static FastEpdX s_epd;

#define IOX_ADDR      0x20
/* TCA9535 pin 10 (P1_2) is the BQ27427 fuel-gauge reset on the X. FastEPD's EPDiy V7 IO init makes
 * pins 8..13 outputs driven low, which would hold the gauge in reset: hand it back as an input. */
#define IOX_GAUGE_RST 10
#define IOX_TRIES     3

/* The time since the last lap goes to `part`. */
static void lap(int part)
{
    uint32_t n = now_us();
    epd_split_add(&s_split, part, n - s_lap);
    s_lap = n;
}
static void split_begin(void)
{
    epd_split_reset(&s_split);
    s_lap = now_us();
}
/* One library call on the panel core: the time before it goes to `part`, its own to lib and scans. */
template <class F> static int lib_call(int part, F &&f)
{
    lap(part);
    s_first_us = 0;
    int rc = on_panel_core(static_cast<F &&>(f));
    uint32_t n = now_us();
    epd_split_call(&s_split, s_lap, s_first_us, n);
    s_lap = n;
    return rc;
}
/* Rails up before the first scan, so the time it takes is seen on its own (the library's own power up at the start
 * of each update then finds them up and returns). The time before goes to `part`. */
static int rails_up(int part)
{
    lap(part);
    int rc = s_epd.einkPower(1);
    lap(EPD_SPLIT_UP);
    return rc;
}
static void rails_down(int part)
{
    lap(part);
    s_epd.einkPower(0);
    s_epd.ioPinMode(IOX_GAUGE_RST, 1);
    lap(EPD_SPLIT_DOWN);
}
extern "C" const epd_split_t *epdp_last_split(void) { return &s_split; }

static bool s_open;
static uint32_t s_bus_hz = EPDP_BUS_HW_CS; /* epdp_set_bus_hz */
static uint32_t s_last_ms;
static uint8_t s_full_passes = 3;
static epd_wave_t s_wave = {0, 3, 0, 0};
/* 1 bpp partial scratch (PSRAM, w*h/8 each): the caller's old and new frames, and the blacks made by partials
 * since the last full refresh with their redrives left (epd_parwave.h, EPD_FRESH_PLANES * w*h/8). Allocated at
 * the first partial. */
static uint8_t *s_p, *s_c, *s_fresh;
static epdp_wave_ms_t s_wave_ms;
static bool s_fresh_dirty; /* fresh changed since epdp_fresh_take_dirty */

static epd_fresh_info_t s_redrive; /* pixels the last partial re-drove (epd_parwave.h) */
/* The frames of a partial update (epd_parjob.h): s_work_rows (epdp_set_work) lets its passes cover only the rows that
 * can hold anything instead of whole frames. s_job is set up with the scratch buffers. */
static epd_parjob_t s_job;
static bool s_work_rows;
static epdp_given_t s_given;       /* what the last partial handed to FastEPD (epd_par.h) */

/* FNV-1a over a frame: a fingerprint for the log (this component has no CRC of its own). */
static uint32_t fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* The frames as given: fingerprints, differing pixels by direction, their row span, the library's state. */
static void given_scan(const uint8_t *P, const uint8_t *C, size_t n, size_t row_bytes, int row0)
{
    memset(&s_given, 0, sizeof s_given);
    s_given.crc_prev = fnv1a(P, n);
    s_given.crc_cur = fnv1a(C, n);
    s_given.y0 = s_given.y1 = -1;
    for (size_t i = 0; i < n; i++) {
        uint8_t x = (uint8_t)(P[i] ^ C[i]);
        if (!x) continue;
        s_given.diff_px += (uint32_t)__builtin_popcount(x);
        s_given.wb_px += (uint32_t)__builtin_popcount((uint8_t)(P[i] & ~C[i]));
        s_given.bw_px += (uint32_t)__builtin_popcount((uint8_t)(~P[i] & C[i]));
        int y = row0 + (int)(i / row_bytes);
        if (s_given.y0 < 0) s_given.y0 = y;
        s_given.y1 = y;
    }
    s_given.mode = (uint8_t)s_epd.getMode();
    s_given.prev_mode = (uint8_t)s_epd.getPreviousMode();
    s_given.pwr_before = (uint8_t)s_epd.powered();
}

extern "C" const epdp_given_t *epdp_last_given(void) { return &s_given; }

/* After a partial: which of the slow rows (epd_rowtime.h) had pixels driven (changed, or re-driven). */
static void mark_slow_driven(size_t n, bool redriven)
{
    const size_t rb = (size_t)(s_epd.width() / 8);
    s_row_stats.slow_driven = 0;
    for (int i = 0; i < s_row_stats.nslow_rows; i++) {
        size_t o = (size_t)s_row_stats.slow_rows[i] * rb;
        /* the saved frames hold the worked rows only (epd_parjob.h); outside them nothing changed */
        const bool in = s_row_stats.slow_rows[i] >= s_job.a && s_row_stats.slow_rows[i] <= s_job.b;
        uint8_t d = 0;
        for (size_t x = 0; o + rb <= n && x < rb; x++)
            d |= (uint8_t)((in ? s_p[o + x] ^ s_c[o + x] : 0) | (redriven ? s_fresh[o + x] | s_fresh[n + o + x] : 0));
        if (d) s_row_stats.slow_driven |= (uint8_t)(1u << i);
    }
}

/* The partial scratch buffers (PSRAM), once. */
static bool scratch(size_t n)
{
    if (s_p) return true;
    s_p = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    s_c = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    s_fresh = (uint8_t *)heap_caps_calloc(EPD_FRESH_PLANES, n, MALLOC_CAP_SPIRAM);
    if (s_p && s_c && s_fresh) {
        s_job.P = s_p;
        s_job.C = s_c;
        s_job.fresh = s_fresh;
        s_job.n = n;
        s_job.rb = (size_t)(s_epd.width() / 8);
        s_job.h = s_epd.height();
        epd_parjob_init(&s_job);
        return true;
    }
    ESP_LOGE(TAG, "no PSRAM for the partial waveform");
    heap_caps_free(s_p);
    heap_caps_free(s_c);
    heap_caps_free(s_fresh);
    s_p = s_c = s_fresh = nullptr;
    return false;
}


#if CONFIG_SPIRAM_MODE_OCT || CONFIG_ESPTOOLPY_OCT_FLASH
#define OCTAL true
#else
#define OCTAL false
#endif

static i2c_master_bus_handle_t bus(void)
{
    i2c_master_bus_handle_t b = nullptr;
    return i2c_master_get_bus_handle(I2C_NUM_0, &b) == ESP_OK ? b : nullptr;
}

/* Read one TCA9535 register through our own device handle (FastEPD's handle is left alone). */
static int iox_read(uint8_t reg, uint8_t *v)
{
    i2c_master_bus_handle_t b = bus();
    if (!b) return -1;
    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = IOX_ADDR;
    dc.scl_speed_hz = 100000;
    i2c_master_dev_handle_t d;
    if (i2c_master_bus_add_device(b, &dc, &d) != ESP_OK) return -1;
    esp_err_t e = i2c_master_transmit_receive(d, &reg, 1, v, 1, 100);
    i2c_master_bus_rm_device(d);
    return e == ESP_OK ? 0 : -1;
}

/* The expander answers (a few tries: the bus may be recovering from a timeout). */
static bool iox_alive(void)
{
    uint8_t v;
    for (int i = 0; i < IOX_TRIES; i++) {
        if (iox_read(0, &v) == 0) return true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGE(TAG, "TCA9535 at 0x%02x does not answer", IOX_ADDR);
    return false;
}

/* Port 1 direction after init: 8, 9, 11, 12, 13 outputs (OE, GMODE, PWRUP, VCOM_CTRL, WAKEUP),
 * 10 (gauge reset), 14 (PWR_GOOD), 15 (nINT) inputs. Re-applied if a FastEPD write got lost. */
static int iox_setup(void)
{
    for (int i = 0; i < IOX_TRIES; i++) {
        uint8_t cfg;
        if (iox_read(7, &cfg) == 0 && (cfg & 0x3b) == 0 && (cfg & 0xc4) == 0xc4) return 0;
        ESP_LOGW(TAG, "TCA9535 port 1 config not as set (try %d), writing it again", i + 1);
        for (int p = 8; p < 14; p++) s_epd.ioPinMode((uint8_t)p, p == IOX_GAUGE_RST ? 1 /* INPUT */ : 3 /* OUTPUT */);
        s_epd.ioPinMode(14, 1);
        s_epd.ioPinMode(15, 1);
    }
    return -1;
}

extern "C" int epdp_open_trmnl_x(int partial_passes, int full_passes)
{
    if (s_open) return EPDP_OK;
    /* FastEPD would create I2C_NUM_0 itself if missing; the board's bus (one config, one owner) must exist. */
    if (!bus()) {
        ESP_LOGE(TAG, "I2C bus 0 not created: the caller must create it before the panel");
        return EPDP_E_INIT;
    }
    if (!iox_alive()) return EPDP_E_IO;
    int rc = on_panel_core([] {
        int r = s_epd.initPanel(BB_PANEL_TRMNL_X);
        return r == BBEP_SUCCESS && s_bus_hz ? s_epd.rebus(s_bus_hz) : r;
    });
    if (rc != BBEP_SUCCESS) {
        ESP_LOGE(TAG, "initPanel failed: %d", rc);
        return EPDP_E_INIT;
    }
    /* Pin guard, after the fact (FastEPD has no way to read its table before init): a FastEPD bump that
     * moves the X onto flash/PSRAM pins must never get to drive the panel. The bring-up checks board.h
     * before init. */
    int pins[24];
    int n = s_epd.pins(pins, 24);
    int bad = epd_pins_first_reserved(pins, n, OCTAL);
    if (bad >= 0) {
        ESP_LOGE(TAG, "FastEPD uses GPIO%d, a flash/PSRAM pin: panel refused", pins[bad]);
        return EPDP_E_INIT;
    }
    s_epd.hook_rows(row_ctl);
    if (!s_pcnt)
        ckv_pcnt_init(FastEpdX::s_ckv);
    s_epd.ioPinMode(IOX_GAUGE_RST, 1 /* INPUT */);
    if (iox_setup()) {
        ESP_LOGE(TAG, "TCA9535 port 1 config does not stick");
        s_epd.ioPinMode(IOX_GAUGE_RST, 1);
        return EPDP_E_IO;
    }
    s_full_passes = (uint8_t)full_passes;
    s_wave.main = (uint8_t)partial_passes;
    s_epd.setPasses((uint8_t)partial_passes, (uint8_t)full_passes);
    s_epd.setMode(BB_MODE_4BPP);
    s_open = true;
    return EPDP_OK;
}

extern "C" bool epdp_is_open(void) { return s_open; }

extern "C" int epdp_set_depth(int bpp)
{
    if (!s_open) return EPDP_E_STATE;
    if (bpp != 1 && bpp != 4) return EPDP_E_ARG;
    epd_parjob_touched(&s_job); /* the caller writes a whole picture next */
    return s_epd.setMode(bpp == 1 ? BB_MODE_1BPP : BB_MODE_4BPP) == BBEP_SUCCESS ? EPDP_OK : EPDP_E_ARG;
}

extern "C" uint8_t *epdp_buffer(void) { return s_open ? s_epd.currentBuffer() : nullptr; }
extern "C" int epdp_width(void) { return s_epd.width(); }
extern "C" int epdp_height(void) { return s_epd.height(); }
extern "C" int epdp_stride(void) { return s_epd.getMode() == BB_MODE_1BPP ? (s_epd.width() + 7) / 8 : (s_epd.width() + 1) / 2; }
extern "C" uint32_t epdp_last_ms(void) { return s_last_ms; }

extern "C" int epdp_full(int clear_mode)
{
    if (!s_open) return EPDP_E_STATE;
    static const int modes[] = { CLEAR_NONE, CLEAR_FAST, CLEAR_SLOW, CLEAR_WHITE };
    if (clear_mode < 0 || clear_mode > 3) return EPDP_E_ARG;
    if (!iox_alive()) return EPDP_E_IO;
    int64_t t0 = esp_timer_get_time();
    split_begin();
    if (s_fresh) memset(s_fresh, 0, EPD_FRESH_PLANES * (size_t)(s_epd.width() / 8) * s_epd.height()); /* all driven by the full waveform */
    epd_parjob_fresh_none(&s_job);
    epd_parjob_touched(&s_job);
    s_fresh_dirty = false;
    rows_begin(false);
    int rc = lib_call(EPD_SPLIT_PREP, [&] { return s_epd.fullUpdate(modes[clear_mode], false); });
    rows_end();
    rails_down(EPD_SPLIT_AFTER);
    s_last_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    return rc == BBEP_SUCCESS ? EPDP_OK : EPDP_E_IO;
}

extern "C" int epdp_gray_to_1bit(void)
{
    if (!s_open || s_epd.getMode() != BB_MODE_4BPP) return EPDP_E_STATE;
    /* FastEPD keeps pPrevious in the second half of the 4 bpp buffer, so build the 1 bpp frame in
     * pTemp (w*h/4 bytes, unused between updates) first. */
    const int w = s_epd.width(), h = s_epd.height(), s4 = w / 2, s1 = w / 8;
    const uint8_t *src = s_epd.currentBuffer();
    uint8_t *tmp = s_epd.tempBuffer();
    for (int y = 0; y < h; y++) {
        const uint8_t *s = src + (size_t)y * s4;
        uint8_t *d = tmp + (size_t)y * s1;
        for (int x = 0; x < s1; x++) {
            uint8_t b = 0;
            for (int i = 0; i < 4; i++) {
                uint8_t v = s[x * 4 + i];
                b = (uint8_t)((b << 2) | ((v >> 4) >= 8 ? 2 : 0) | ((v & 15) >= 8 ? 1 : 0));
            }
            d[x] = b;
        }
    }
    s_epd.setMode(BB_MODE_1BPP);
    epd_parjob_touched(&s_job);
    memcpy(s_epd.currentBuffer(), tmp, (size_t)s1 * h);
    memcpy(s_epd.previousBuffer(), tmp, (size_t)s1 * h);
    s_epd.setPreviousMode(BB_MODE_1BPP);
    return EPDP_OK;
}

/* One step of a partial through the sequencer: FastEPD's partial over the careful rows, the others clocked
 * through, the neutral scan after it or not (epd_scan.h). On the panel core, the trail rows included. */
static int scan_step(const epd_scan_rows_t &r, int passes, bool neutral)
{
    return lib_call(EPD_SPLIT_MASKS, [&] {
        epd_scan_seq_call(&s_seq, &r, passes, neutral, s_scan_used.skip_us, s_scan_skip ? s_scan_used.min_ms * 1000u : 0);
        s_seq_on = true;
        int rc = s_epd.scan_partial(r, passes);
        epd_scan_seq_end(&s_seq);
        s_seq_on = false;
        return rc;
    });
}

/* After rows_begin(true): whether this partial's scans go through the sequencer (false: FastEPD's own, the
 * default). Rows are skipped only with the fixed drive (its row step leaves CKV low, which the bare clocks of
 * the skipped rows start from) and a panel scanned top down; then the prime scan runs here (*rc). */
static bool scan_begin(int *rc)
{
    s_scan_used = s_scan_cfg;
    if (epd_scan_cfg_stock(&s_scan_used)) return false;
    s_scan_skip = s_scan_used.skip && s_drive_us && !s_epd.mirror_y();
    if (s_scan_skip) {
        epd_scan_rows_t r;
        epd_scan_rows(s_epd.height(), -1, -1, true, &r);
        *rc = scan_step(r, 0, true);
    }
    return true;
}

/* A step's scans through the sequencer: the rows FastEPD's frames differ in now, joined with those of the
 * update's earlier steps (a row driven before is still written with the timed step and reaches the neutral
 * scan). */
static int scan_run(const uint8_t *prev, const uint8_t *cur, size_t n, int row0, int *y0, int *y1, int frames, int step,
                    int steps)
{
    int a0, a1;
    epd_scan_rows_t r;
    /* n bytes from row0 on: the rows the frames can differ in (all of them, or the worked rows of epd_parjob.h) */
    epd_scan_span(prev, cur, n, (size_t)(s_epd.width() / 8), &a0, &a1);
    if (a0 >= 0) a0 += row0, a1 += row0;
    epd_scan_union(y0, y1, a0, a1);
    epd_scan_rows(s_epd.height(), *y0, *y1, s_scan_skip, &r);
    return scan_step(r, frames, epd_scan_step_neutral(s_scan_used.neutral, step, steps));
}

extern "C" int epdp_partial(int y0, int y1)
{
    if (!s_open) return EPDP_E_STATE;
    if (s_epd.getMode() != BB_MODE_1BPP) return EPDP_E_ARG;
    if (!iox_alive()) return EPDP_E_IO;
    const size_t n = (size_t)(s_epd.width() / 8) * s_epd.height();
    if (!scratch(n)) return EPDP_E_INIT;
    /* all rows, not [y0, y1]: FastEPD drives rows outside the range with stale DMA data (epd_parwin.h) */
    int first, last;
    epd_par_rows(s_epd.height(), y0, y1, &first, &last);
    int64_t t0 = esp_timer_get_time();
    split_begin();
    uint8_t *cur = s_epd.currentBuffer(), *prev = s_epd.previousBuffer();
    s_job.prev = prev;
    s_job.cur = cur;
    /* the frames saved, the search for new blacks and the plan of steps (epd_parjob.h): over whole frames, or with
     * epdp_set_work over the rows that can hold anything */
    epd_parjob_begin(&s_job, &s_wave, y0, y1, s_work_rows);
    given_scan(s_p + s_job.off, s_c + s_job.off, s_job.len, s_job.rb, s_job.a);
    const epd_wave_step_t *st = s_job.st;
    const int ns = s_job.ns;
    const bool redriven = s_job.redriven;
    memset(&s_wave_ms, 0, sizeof s_wave_ms);
    s_redrive = s_job.redrive;
    int rc = rails_up(EPD_SPLIT_PREP);
    rows_begin(true);
    int sy0 = -1, sy1 = -1;
    const bool seq = rc == BBEP_SUCCESS && scan_begin(&rc);
    /* One FastEPD partial per step (epd_parwave.h), rails kept on in between. FastEPD copies current into
     * previous as it goes, so every step rebuilds both from the saved frames. */
    for (int i = 0; i < ns && rc == BBEP_SUCCESS; i++) {
        int64_t t = esp_timer_get_time();
        epd_parjob_step(&s_job, i, &s_given.drv[st[i].phase]);
        s_epd.setPasses(st[i].frames, s_full_passes);
        if (seq)
            rc = scan_run(prev + s_job.off, cur + s_job.off, s_job.len, s_job.a, &sy0, &sy1, st[i].frames, i, ns);
        else
            rc = lib_call(EPD_SPLIT_MASKS, [&] { return s_epd.partialUpdate(i + 1 < ns, first, last); });
        s_wave_ms.ms[st[i].phase] = (uint16_t)((esp_timer_get_time() - t) / 1000);
        s_wave_ms.frames[st[i].phase] = st[i].frames;
    }
    rows_end();
    s_given.pwr_after = (uint8_t)s_epd.powered();
    s_epd.setPasses(s_wave.main, s_full_passes);
    rails_down(EPD_SPLIT_MASKS);
    mark_slow_driven(n, redriven);
    if (epd_parjob_end(&s_job, rc == BBEP_SUCCESS)) s_fresh_dirty = true;
    lap(EPD_SPLIT_AFTER);
    s_last_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    return rc == BBEP_SUCCESS ? EPDP_OK : EPDP_E_IO;
}

extern "C" int epdp_set_passes(int partial_passes, int full_passes)
{
    if (partial_passes < 1 || partial_passes > 14 || full_passes < 1 || full_passes > 14) return EPDP_E_ARG;
    s_full_passes = (uint8_t)full_passes;
    s_wave.main = (uint8_t)partial_passes;
    s_epd.setPasses((uint8_t)partial_passes, (uint8_t)full_passes);
    return EPDP_OK;
}

extern "C" int epdp_set_wave(int kick, int extra, int redrive)
{
    if (kick < 0 || extra < 0 || redrive < 0) return EPDP_E_ARG;
    epd_wave_t w = s_wave;
    w.kick = (uint8_t)(kick > 255 ? 255 : kick);
    w.extra = (uint8_t)(extra > 255 ? 255 : extra);
    w.redrive = (uint8_t)(redrive > 255 ? 255 : redrive);
    if (!epd_wave_valid(&w)) return EPDP_E_ARG;
    s_wave = w;
    return EPDP_OK;
}

extern "C" const epdp_wave_ms_t *epdp_last_wave(void) { return &s_wave_ms; }

extern "C" const epd_fresh_info_t *epdp_last_redrive(void) { return &s_redrive; }

static epdp_gpart_info_t s_gpart;

extern "C" int epdp_gray_partial(const epdp_gwin_t *win, int nw, const uint8_t *matrix, int passes, int dark, int light)
{
    if (!s_open) return EPDP_E_STATE;
    if (s_epd.getMode() != BB_MODE_1BPP) return EPDP_E_ARG;
    const int W = s_epd.width(), H = s_epd.height();
    const size_t ds = (size_t)W / 8, n = ds * H;
    if (!matrix || passes < 1 || passes > 16 || dark < 0 || dark > 14 || light < 0 || light > 14 || nw < 0) return EPDP_E_ARG;
    for (int i = 0; i < nw; i++) {
        const epdp_gwin_t &g = win[i];
        if (g.x < 0 || g.y < 0 || g.x % 8 || g.w % 8 || g.w <= 0 || g.h <= 0 || g.x + g.w > W || g.y + g.h > H ||
            !g.old4 || !g.new4 || g.stride < (size_t)g.w / 2)
            return EPDP_E_ARG;
    }
    if (!iox_alive()) return EPDP_E_IO;
    if (!scratch(n)) return EPDP_E_INIT;
    struct { uint8_t kind, j, frames; } st[2 + 16];
    int ns = 0;
    if (dark) st[ns++] = {EPD_GP_DARK, 0, (uint8_t)dark};
    if (light) st[ns++] = {EPD_GP_LIGHT, 0, (uint8_t)light};
    uint8_t first[16], len[16];
    int ng = epd_gpart_groups(matrix, passes, first, len);
    for (int i = 0; i < ng; i++) {
        uint8_t f = len[i];
        while (f) { /* FastEPD takes 1..14 passes per call */
            uint8_t k = f > 14 ? 14 : f;
            st[ns++] = {EPD_GP_PASS, first[i], k};
            f = (uint8_t)(f - k);
        }
    }
    int64_t t0 = esp_timer_get_time();
    split_begin();
    uint8_t *cur = s_epd.currentBuffer(), *prev = s_epd.previousBuffer();
    int rc = rails_up(EPD_SPLIT_PREP);
    rows_begin(true);
    memset(&s_gpart, 0, sizeof s_gpart);
    int sy0 = -1, sy1 = -1;
    const bool seq = rc == BBEP_SUCCESS && scan_begin(&rc);
    for (int i = 0; i < ns && rc == BBEP_SUCCESS; i++) {
        memset(prev, 0xff, n);
        memset(cur, 0xff, n);
        uint32_t drv = 0;
        for (int k = 0; k < nw; k++) {
            const epdp_gwin_t &g = win[k];
            for (int r = 0; r < g.h; r++) {
                size_t o = (size_t)(g.y + r) * ds + (size_t)g.x / 8;
                epd_gpart_row(st[i].kind, st[i].j, matrix, passes, g.old4 + (size_t)r * g.stride,
                              g.new4 + (size_t)r * g.stride, g.w, prev + o, cur + o, nullptr);
                drv += epd_gpart_driven(prev + o, cur + o, g.w);
            }
        }
        if (drv > s_gpart.driven_px) s_gpart.driven_px = drv;
        s_epd.setPasses(st[i].frames, s_full_passes);
        if (seq)
            rc = scan_run(prev, cur, n, 0, &sy0, &sy1, st[i].frames, i, ns);
        else
            rc = lib_call(EPD_SPLIT_MASKS, [&] { return s_epd.partialUpdate(i + 1 < ns, 0, H - 1); });
        s_gpart.steps++;
        s_gpart.frames = (uint8_t)(s_gpart.frames + st[i].frames);
    }
    rows_end();
    s_epd.setPasses(s_wave.main, s_full_passes);
    rails_down(EPD_SPLIT_MASKS);
    /* changed pixels went through the whole table: no longer fresh partial blacks */
    uint8_t chg[1872 / 8], tp[1872 / 8], tc[1872 / 8];
    for (int k = 0; k < nw && (size_t)win[k].w / 8 <= sizeof chg; k++) {
        const epdp_gwin_t &g = win[k];
        for (int r = 0; r < g.h; r++) {
            epd_gpart_row(EPD_GP_DARK, 0, matrix, passes, g.old4 + (size_t)r * g.stride, g.new4 + (size_t)r * g.stride,
                          g.w, tp, tc, chg);
            uint8_t *f = s_fresh + (size_t)(g.y + r) * ds + (size_t)g.x / 8, *fh = f + n;
            for (int x = 0; x < g.w / 8; x++) {
                s_gpart.changed_px += (uint32_t)__builtin_popcount(chg[x]);
                if ((f[x] | fh[x]) & chg[x]) s_fresh_dirty = true;
                f[x] = (uint8_t)(f[x] & ~chg[x]);
                fh[x] = (uint8_t)(fh[x] & ~chg[x]);
            }
        }
    }
    memset(prev, 0xff, n);
    memset(cur, 0xff, n);
    epd_parjob_is_white(&s_job);
    lap(EPD_SPLIT_AFTER);
    s_last_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    return rc == BBEP_SUCCESS ? EPDP_OK : EPDP_E_IO;
}

extern "C" const epdp_gpart_info_t *epdp_last_gpart(void) { return &s_gpart; }

extern "C" uint8_t *epdp_fresh(size_t *n)
{
    if (!s_open) return nullptr;
    size_t plane = (size_t)(s_epd.width() / 8) * s_epd.height();
    *n = EPD_FRESH_PLANES * plane;
    return scratch(plane) ? s_fresh : nullptr;
}

extern "C" void epdp_fresh_loaded(void)
{
    if (s_fresh) epd_parjob_fresh_unknown(&s_job);
}

extern "C" int epdp_set_work(bool rows)
{
    s_work_rows = rows;
    return EPDP_OK;
}

extern "C" bool epdp_fresh_take_dirty(void)
{
    bool d = s_fresh_dirty;
    s_fresh_dirty = false;
    return d;
}

extern "C" int epdp_set_rows(int settle_us, int hold_us)
{
    if (settle_us < 0 || hold_us < 0) return EPDP_E_ARG;
    epd_row_delay_t d = {(uint8_t)(settle_us > 255 ? 255 : settle_us), (uint8_t)(hold_us > 255 ? 255 : hold_us)};
    if (!epd_row_delay_valid(&d)) return EPDP_E_ARG;
    s_rowd = d;
    return EPDP_OK;
}

extern "C" const epd_row_stats_t *epdp_last_rows(void) { return &s_row_stats; }

extern "C" int epdp_set_drive(int drive_us)
{
    if (!epd_row_drive_valid(drive_us)) return EPDP_E_ARG;
    s_drive_set = (uint32_t)drive_us;
    return EPDP_OK;
}

extern "C" const epd_row_drive_stats_t *epdp_last_drive(uint32_t *drive_us)
{
    if (drive_us) *drive_us = s_drive_set;
    return &s_drive;
}

extern "C" bool epdp_drive_ok(void) { return s_drive_set != 0; }

extern "C" int epdp_set_scan(const epd_scan_cfg_t *c)
{
    if (!epd_scan_cfg_valid(c)) return EPDP_E_ARG;
    s_scan_cfg = *c;
    return EPDP_OK;
}

extern "C" const epd_scan_stats_t *epdp_last_scan(epd_scan_cfg_t *used, bool *skipped)
{
    if (used) *used = s_scan_used;
    if (skipped) *skipped = s_scan_skip;
    return &s_seq.st;
}

extern "C" int epdp_set_bus_hz(uint32_t hz)
{
    if (s_open) return EPDP_E_STATE;
    if (hz && (hz < 10000000 || hz > 40000000)) return EPDP_E_ARG;
    s_bus_hz = hz;
    return EPDP_OK;
}

extern "C" uint32_t epdp_bus_hz(bool *soft_cs)
{
    if (soft_cs) *soft_cs = s_epd.soft_cs();
    return s_open ? s_epd.bus_hz() : 0;
}

extern "C" int epdp_set_matrix(const uint8_t *matrix, int n)
{
    if (!s_open) return EPDP_E_STATE;
    if (!matrix || n <= 0 || (n & 15)) return EPDP_E_ARG;
    return s_epd.setCustomMatrix(matrix, (size_t)n) == BBEP_SUCCESS ? EPDP_OK : EPDP_E_ARG;
}

extern "C" int epdp_partial_begin(void)
{
    if (!s_open) return EPDP_E_STATE;
    s_epd.setMode(BB_MODE_1BPP);
    const size_t n = (size_t)(s_epd.width() / 8) * s_epd.height();
    if (scratch(n)) { /* with epdp_set_work only the rows the last partial left not white (epd_parjob.h) */
        s_job.prev = s_epd.previousBuffer();
        s_job.cur = s_epd.currentBuffer();
        epd_parjob_white(&s_job, s_work_rows);
    } else {
        memset(s_epd.currentBuffer(), 0xff, n);
        memset(s_epd.previousBuffer(), 0xff, n);
    }
    s_epd.setPreviousMode(BB_MODE_1BPP); /* no conversion of an older frame in partialUpdate */
    return EPDP_OK;
}

extern "C" uint8_t *epdp_prev_buffer(void) { return s_open ? s_epd.previousBuffer() : nullptr; }

extern "C" void epdp_close(void)
{
    if (!s_open) return;
    s_epd.einkPower(0);
    s_epd.ioPinMode(IOX_GAUGE_RST, 1);
    /* deInit() would free the buffers and the LCD bus; the bring-up keeps the panel object for the session. */
}
