/* IQS323 transport, configuration and recovery, shared with the fault-injection host test.
 * Private to x_board.c. SPDX-License-Identifier: GPL-3.0-or-later */
#define IQS_PROD_NUM    0x00
#define IQS_STATUS      0x10  /* byte 0: 5 ATI_ACTIVE, 6 ATI_ERROR, 7 SHOW_RESET; byte 1: touch bits 1, 3, 5 */
#define IQS_GESTURE_SEL 0xa0
#define IQS_SYSCTRL     0xc0  /* byte 0: 0 ACK_RESET, 1 SW_RESET, 2 RE_ATI, 7 EVENT_MODE */
#define IQS_EVENTS      0xd3  /* byte 0: EVENTS_ENABLE */
#define IQS_TOUCH_BITS  0x2a
#define IQS_SHOW_RESET  0x80
#define IQS_ATI_ACTIVE  0x20
#define IQS_EVENT_MODE  0x80
#define IQS_STORM_WAKES 8
#define IQS_PROBE_REG   0xfe  /* no such register: a healthy chip answers IQS_EE (datasheet 8.10) */
#define IQS_EE          0xee
/* Lock-up (datasheet v1.12 known issues, IQS323-00x v1.3 and below: "all bytes read over I2C return a constant
 * value", more likely with forced communication; issue #140: 57 such reads in 9 days, every one all zeros, none a
 * sane config with a wrong value): a constant status read is checked by a probe read of IQS_PROBE_REG and a
 * confirmed lock-up gets a hard reset (MCLR) before the init, since a soft reset over I2C never took in the log.
 * Forced windows (datasheet 8.13: "ideally, communication with the IQS323 should only be initiated in a RDY
 * window"; the stock firmware never forces while running): device 2026-10-07, awake on USB, six lock-ups in 30
 * min were all found by the 2 s idle check itself, 4 to 40 s after a panel refresh with nothing else running, and
 * that check made 1125 of the 1125 forced windows against 962 reads. Awake and idle nothing is forced any more;
 * once an awake hour one forced status read (a locked chip that opens no window, on USB without a sleep, is found
 * within the hour) and a counter line; a press uses the chip's own windows (iqs_service); before every deep sleep
 * one forced status read stays (a locked chip gives no touch wake). */
#define IQS_HOUR_US 3600000000LL
#define IQS_STREAM_GONE_US 500000 /* a stream shows a window every report (16..60 ms): none this long, it is off */

/* Stock IQS323_config_LP.h, in writeMM order: {start address, bytes...}. */
static const uint8_t IQS_S0[] = {0x01, 0x04, 0x7F, 0x0C, 0x90, 0x13, 0xCF, 0x04, 0x0A, 0x03,
                                 0x00, 0x00, 0x74, 0x17, 0x64, 0x00, 0x44, 0x64, 0xD9, 0x53};
static const uint8_t IQS_S1[] = {0x01, 0x01, 0x7F, 0x0C, 0x90, 0x13, 0xCF, 0x01, 0x0A, 0x03,
                                 0x00, 0x00, 0xC4, 0x12, 0x64, 0x00, 0x47, 0x5E, 0xD6, 0x53};
static const uint8_t IQS_S2[] = {0x01, 0x02, 0x7F, 0x0C, 0x90, 0x13, 0xCF, 0x02, 0x0A, 0x03,
                                 0x00, 0x00, 0x74, 0x17, 0x64, 0x00, 0x44, 0x5C, 0xA2, 0x4B};
static const uint8_t IQS_CH0[] = {0x00, 0x00, 0x14, 0x44, 0x1B, 0x00, 0xC8, 0x00};
static const uint8_t IQS_CH1[] = {0x00, 0x00, 0x14, 0x44, 0x1A, 0x00, 0xC8, 0x00};
static const uint8_t IQS_CH2[] = {0x00, 0x00, 0x14, 0x44, 0x1A, 0x00, 0xC8, 0x00};
static const uint8_t IQS_SLIDER[] = {0x0B, 0x00, 0x00, 0x14, 0xC8, 0x00, 0x00, 0x08, 0x07,
                                     0x00, 0x52, 0x05, 0x30, 0x04, 0x72, 0x04, 0xB4, 0x04};
static const uint8_t IQS_GESTURE[] = {0x0B, 0x00, 0x32, 0x00, 0x5E, 0x01, 0xF4, 0x01, 0x58, 0x02, 0x90, 0x01, 0xF4, 0x01};
static const uint8_t IQS_FILTER[] = {0x02, 0x01, 0x0C, 0x0C, 0x00, 0x00, 0x02, 0x01, 0x14, 0x00};
static const uint8_t IQS_GENERAL[] = {0x00, 0x00, 0xC8, 0x00, 0x75, 0x75, 0x04, 0x18, 0x08, 0x0A};
static const uint8_t IQS_POWER[] = {0x10, 0x00, 0x10, 0x00, 0x3C, 0x00, 0x64, 0x00, 0xB8, 0x0B, 0xD0, 0x07};
static const uint8_t IQS_I2C_SETUP[] = {0x00};
static const struct {
    uint8_t reg;
    const uint8_t *b;
    uint8_t n;
} IQS_MM[] = {
    {0x30, IQS_S0, sizeof IQS_S0},           {0x40, IQS_S1, sizeof IQS_S1},         {0x50, IQS_S2, sizeof IQS_S2},
    {0x60, IQS_CH0, sizeof IQS_CH0},         {0x70, IQS_CH1, sizeof IQS_CH1},       {0x80, IQS_CH2, sizeof IQS_CH2},
    {0x90, IQS_SLIDER, sizeof IQS_SLIDER},   {0xa0, IQS_GESTURE, sizeof IQS_GESTURE},
    {0xb0, IQS_FILTER, sizeof IQS_FILTER},   {0xd0, IQS_GENERAL, sizeof IQS_GENERAL},
    {0xc0, IQS_POWER, sizeof IQS_POWER},     {0xe0, IQS_I2C_SETUP, sizeof IQS_I2C_SETUP},
};

/* Whole bus-owning turn, including config readback. Leave 300 ms for scheduling and callers. */
#define IQS_STEP_US 1700000
#define IQS_TRANSFER_US 55000 /* 50 ms I2C timeout plus window close */
static int64_t s_iqs_deadline, s_retry_at, s_lost_at;
static bool s_retry_mclr;
static uint8_t s_odd[6]; /* the last status read that was not valid (iqs_health logs it) */
static int s_odd_rc;

static void iqs_step_begin(void) { s_iqs_deadline = esp_timer_get_time() + IQS_STEP_US; }
static void iqs_step_end(void) { s_iqs_deadline = 0; }
static bool iqs_expired(void)
{
    return s_iqs_deadline && esp_timer_get_time() + IQS_TRANSFER_US >= s_iqs_deadline;
}

static bool rdy_low(void) { return gpio_get_level(HAL_PIN_BUTTON) == 0; }

static void wait_us(uint32_t us)
{
    if (s_iqs_deadline) {
        int64_t left = s_iqs_deadline - esp_timer_get_time() - IQS_TRANSFER_US;
        if (left <= 0)
            return;
        if ((int64_t)us > left)
            us = (uint32_t)left;
    }
    if (us >= 2000)
        vTaskDelay(pdMS_TO_TICKS((us + 999) / 1000));
    else
        esp_rom_delay_us(us);
}

/* After a STOP: the window closes (RDY high) within a few hundred us; do not start a transfer before. */
static void wait_window_closed(void)
{
    for (int i = 0; i < 50 && rdy_low(); i++)
        esp_rom_delay_us(100);
}

/* A communication window: RDY low, or ask for one (write 0xFF, may be NACKed) and wait up to 100 ms. */
static bool iqs_window(bool force)
{
    if (iqs_expired())
        return false;
    if (rdy_low())
        return true;
    if (!force)
        return false;
    s_n_forced++;
    uint8_t ff = 0xff;
    i2c_master_transmit(s_touch, &ff, 1, I2C_TMO_MS);
    int64_t until = esp_timer_get_time() + 100000;
    while (!iqs_expired() && esp_timer_get_time() < until) {
        if (rdy_low())
            return true;
        wait_us(2000);
    }
    s_n_nowin++;
    return false;
}

static bool all_ee(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (b[i] != 0xee)
            return false;
    return true;
}

/* One read in a window (iqs_window), the bytes as the chip sent them. */
static int iqs_rd_raw(uint8_t reg, uint8_t *buf, size_t n, bool force)
{
    bus_lock();
    if (!iqs_window(force)) {
        bus_unlock();
        return HAL_E_TIMEOUT;
    }
    int rc = iqs_expired() ? HAL_E_TIMEOUT : rd(s_touch, reg, buf, n);
    wait_window_closed();
    bus_unlock();
    return rc;
}

/* A read of registers that exist: only filler bytes (a closing window, datasheet 8.10) is invalid. */
static int iqs_rd(uint8_t reg, uint8_t *buf, size_t n, bool force)
{
    int rc = iqs_rd_raw(reg, buf, n, force);
    if (rc == HAL_OK && all_ee(buf, n)) {
        s_n_bad++;
        return HAL_E_IO;
    }
    return rc;
}

static int iqs_wr(uint8_t reg, const uint8_t *buf, size_t n)
{
    bus_lock();
    int rc = iqs_window(true) && !iqs_expired() ? wr(s_touch, reg, buf, n) : HAL_E_TIMEOUT;
    if (rc != HAL_E_TIMEOUT)
        wait_window_closed();
    bus_unlock();
    return rc;
}

/* SYSTEM_CONTROL as last read or written (datasheet A.30: bits 0..3 commands that clear themselves, 4..6 the power
 * mode, 7 the interface: 1 events, 0 streaming; byte 1 the channel timeouts). The init reads it; the mode writes
 * below need no read first. */
static uint8_t s_sysctrl[2] = {0x10, 0x00};

/* Read SYSTEM_CONTROL, set bits, write it back (stock SW_Reset, acknowledgeReset, ReATI, setEventMode). */
static int iqs_sysctrl_set(uint8_t bits)
{
    uint8_t sc[2];
    int rc = iqs_rd(IQS_SYSCTRL, sc, 2, true);
    if (rc)
        return rc;
    s_sysctrl[0] = sc[0];
    s_sysctrl[1] = sc[1];
    sc[0] |= bits;
    return iqs_wr(IQS_SYSCTRL, sc, 2);
}

/* Event mode on or off (SYSTEM_CONTROL bit 7), one write from the shadow, no read first. Off while the bar is down
 * (trmnl-firmware PR #508): the chip then opens a window every report (~16 ms), so the release and the slider
 * trail are read without forcing; on again when it is up and before the sleep (touch wake). Hardware 2026-09-26:
 * with event mode on and a forced read every 100 ms no release was ever read (stock warns forced reads that often
 * make the chip stop answering). `force` false: only in a window the chip has open now (iqs_service), else
 * HAL_E_TIMEOUT and nothing sent. */
static bool s_stream;     /* event mode is off (a press is being resolved) */
static bool s_want_event; /* the press is over: event mode again in the next window the chip opens */
static int64_t s_win_us;  /* the last window seen (a stream without windows is none) */

static int iqs_mode_wr(bool event, bool force)
{
    uint8_t b[2] = {(uint8_t)((s_sysctrl[0] & 0x70) | (event ? IQS_EVENT_MODE : 0)), s_sysctrl[1]};
    bus_lock();
    int rc = iqs_window(force) && !iqs_expired() ? wr(s_touch, IQS_SYSCTRL, b, 2) : HAL_E_TIMEOUT;
    if (rc != HAL_E_TIMEOUT)
        wait_window_closed();
    bus_unlock();
    if (!rc) {
        s_stream = !event;
        s_want_event = false;
        s_sysctrl[0] = b[0];
    }
    return rc;
}

static int iqs_event_mode(bool on) { return iqs_mode_wr(on, true); }

/* A valid status + gestures read: kept, and fed to the button unless the chip shows a reset. */
static void iqs_feed(const uint8_t *b)
{
    s_n_reads++;
    s_st[0] = b[0];
    s_st[1] = b[1];
    s_g[0] = b[2];
    s_g[1] = b[3];
    s_sl = (uint16_t)(b[4] | b[5] << 8);
    if (!s_init_busy && !(b[0] & IQS_SHOW_RESET))
        tbtn_read_sl(&s_btn, esp_timer_get_time(), b[1], b[2], s_sl);
    if (s_tlog)
        tlog("touch rd t=%u st=%02x%02x g=%02x%02x sl=%04x down=%d", (unsigned)(esp_timer_get_time() / 1000), b[1],
                 b[0], b[3], b[2], s_sl, s_btn.down);
}

static bool all_same(const uint8_t *b, size_t n)
{
    for (size_t i = 1; i < n; i++)
        if (b[i] != b[0])
            return false;
    return true;
}

/* status, gestures, slider coordinate (0x10..0x12). Not valid (no window, NACK, 0xEE filler, or six equal bytes:
 * the slider reads 0xffff untouched, nothing valid is one constant; the lock-up's signature): the bytes and the rc
 * stay in s_odd for iqs_health, which decides. */
static int iqs_status_rd(bool force)
{
    uint8_t st[6] = {0};
    int rc = iqs_rd(IQS_STATUS, st, 6, force);
    if (rc == HAL_OK && !s_init_busy && all_same(st, 6)) {
        s_n_bad++;
        if (s_zero_run < 255)
            s_zero_run++;
        rc = HAL_E_IO;
    }
    if (rc) {
        memcpy(s_odd, st, sizeof s_odd);
        s_odd_rc = rc;
        return rc;
    }
    s_zero_run = 0;
    iqs_feed(st);
    return HAL_OK;
}

/* Datasheet workaround for the lock-up: a read of a register that does not exist answers IQS_EE from a healthy chip.
 * 1: locked up (another byte), 0: healthy, -1: no answer (no window, NACK). One forced window. */
static int iqs_probe(void)
{
    uint8_t b = 0;
    if (iqs_rd_raw(IQS_PROBE_REG, &b, 1, true))
        return -1;
    return b == IQS_EE ? 0 : 1;
}

/* Stock iqs323_hardware_reset: RDY doubles as MCLR. Wait for RDY high, pull it low 500 us, let go, 50 ms. */
static bool iqs_mclr(void)
{
    int64_t until = esp_timer_get_time() + 100000;
    while (rdy_low() && !iqs_expired() && esp_timer_get_time() < until)
        wait_us(2000);
    if (rdy_low() || iqs_expired())
        return false; /* RDY must be released before driving its shared MCLR pin. */
    tlog("W touchbar MCLR reset: intentional init retry");
    esp_rom_delay_us(150);
    gpio_set_direction(HAL_PIN_BUTTON, GPIO_MODE_OUTPUT);
    gpio_set_level(HAL_PIN_BUTTON, 0);
    esp_rom_delay_us(500);
    gpio_set_direction(HAL_PIN_BUTTON, GPIO_MODE_INPUT);
    gpio_set_pull_mode(HAL_PIN_BUTTON, GPIO_PULLUP_ONLY);
    wait_us(50000);
    return !iqs_expired();
}

/* One pass of the stock init state machine; a short name of the failed step, or NULL. */
static const char *iqs_init_once(void)
{
    if (iqs_sysctrl_set(0x02)) /* SW reset */
        return "sw_reset";
    wait_us(100000);
    uint8_t b[18];
    if (iqs_rd(IQS_PROD_NUM, b, 2, true))
        return "product";
    s_prod = (uint16_t)(b[0] | b[1] << 8);
    if (s_prod != 1106 && s_prod != 1462)
        return "product";
    if (!iqs_rd(0x01, b, 2, true))
        s_ver[0] = b[0];
    if (!iqs_rd(0x02, b, 2, true))
        s_ver[1] = b[0];
    /* READ_RESET: the reset flag must show (else another SW reset) */
    bool reset = false;
    for (int i = 0; i < 5 && !reset && !iqs_expired(); i++) {
        if (iqs_status_rd(true) == HAL_OK && (s_st[0] & IQS_SHOW_RESET)) {
            reset = true;
        } else {
            iqs_sysctrl_set(0x02);
            wait_us(100000);
        }
    }
    if (!reset)
        return "read_reset";
    for (size_t i = 0; i < sizeof IQS_MM / sizeof IQS_MM[0]; i++)
        if (iqs_wr(IQS_MM[i].reg, IQS_MM[i].b, IQS_MM[i].n))
            return "settings";
    if (iqs_sysctrl_set(0x01)) /* ACK_RESET */
        return "ack_reset";
    if (iqs_sysctrl_set(0x04)) /* RE_ATI */
        return "ati";
    int64_t t0 = esp_timer_get_time();
    for (;;) {
        wait_us(10000);
        if (iqs_status_rd(true) == HAL_OK && !(s_st[0] & IQS_ATI_ACTIVE))
            break;
        if (iqs_expired() || esp_timer_get_time() - t0 > IQS_STEP_US)
            return "ati_wait";
    }
    if (iqs_rd(IQS_STATUS, b, 18, true)) /* READ_DATA (queueValueUpdates) */
        return "read_data";
    if (iqs_sysctrl_set(IQS_EVENT_MODE))
        return "event_mode";
    wait_us(10000);
    uint8_t sc[2];
    if (iqs_rd(IQS_SYSCTRL, sc, 2, true) || !(sc[0] & IQS_EVENT_MODE))
        return "event_mode_readback";
    s_sysctrl[0] = sc[0];
    s_sysctrl[1] = sc[1];
    /* stock goToSleep setGestureConfig: touch events only (tap mode) */
    uint8_t v = 0x09;
    if (iqs_wr(IQS_GESTURE_SEL, &v, 1))
        return "gesture";
    v = 0x02;
    if (iqs_wr(IQS_EVENTS, &v, 1))
        return "events";
    return NULL;
}

/* One bounded attempt. Failed attempts schedule MCLR and a fresh init in a later turn. */
static bool iqs_init(void)
{
    if (esp_timer_get_time() < s_retry_at)
        return false;
    bool own_step = !s_iqs_deadline;
    if (own_step)
        iqs_step_begin();
    bus_lock();
    int64_t t0 = esp_timer_get_time();
    if (s_btn.ended)
        press_end(); /* a valid release read before the loss still belongs to the input queue */
    tbtn_drop(&s_btn);
    s_init_busy = true;
    s_iqs_ready = false;
    const char *err = NULL;
    if (s_retry_mclr && !iqs_mclr())
        err = "mclr_rdy";
    if (!err)
        err = iqs_init_once();
    bool ok = !err;
    if (ok) {
        s_iqs_ready = true;
        s_stream = s_want_event = false;
        tbtn_reset(&s_btn);
        tbtn_drop(&s_btn);
        s_zero_run = 0;
        s_retry_mclr = false;
        s_retry_at = 0;
        tlog("I touchbar IQS323 product=%u v%u.%u ready ms=%u", s_prod, s_ver[0], s_ver[1],
             (unsigned)((esp_timer_get_time() - t0) / 1000));
    } else {
        s_retry_mclr = true;
        s_retry_at = esp_timer_get_time() + TBTN_RETRY_US;
        tlog("W touchbar init failed at %s ms=%u: no touch wake, retry_ms=200", err,
             (unsigned)((esp_timer_get_time() - t0) / 1000));
    }
    s_init_busy = false;
    bus_unlock();
    if (own_step)
        iqs_step_end();
    return ok;
}

/* After a valid status read: a chip reset means a full init again (the button state starts over). */
static void iqs_apply_status(void)
{
    if (s_st[0] & IQS_SHOW_RESET) {
        tlog("W touchbar reset seen (status=%02x%02x): init again", s_st[1], s_st[0]);
        s_touch_ok = iqs_init();
        tbtn_drop(&s_btn); /* a touch read around the reset is no press (hardware 2026-10-01: hold=1, nobody there) */
    }
}

/* The bar is re-initialised (the press state starts over); `mclr`: hard reset first (a lock-up: the soft reset over
 * I2C never took, issue #140). Log: `I touchbar recovered (why)` or `W touchbar not recovered (why)`. */
static void iqs_recover(const char *why, bool mclr)
{
    int64_t t0 = esp_timer_get_time();
    s_touch_ok = false;
    if (mclr)
        s_retry_mclr = true;
    s_touch_ok = iqs_init();
    tbtn_drop(&s_btn);
    tlog("%s touchbar %s (%s) ms=%u lockups=%u", s_touch_ok ? "I" : "W", s_touch_ok ? "recovered" : "not recovered",
         why, (unsigned)((esp_timer_get_time() - t0) / 1000), (unsigned)s_n_lost);
}

/* A status read was not valid (iqs_status_rd): a second opinion before anything is done, the probe (one forced
 * window). Healthy: the read was a fluke (a closing window, a late answer), nothing else. Locked up, or no answer
 * twice: `W touchbar lockup ...` (count these; before 2026-10-07 the same event was `W touchbar config lost`) and a
 * recovery with a hard reset first. */
static void iqs_health(const char *why)
{
    int p = iqs_probe();
    if (p == 0) {
        s_zero_run = 0;
        return;
    }
    if (s_n_lost < 255)
        s_n_lost++;
    int64_t now = esp_timer_get_time();
    unsigned gap_ms = s_lost_at ? (unsigned)((now - s_lost_at) / 1000) : 0;
    s_lost_at = now;
    tlog("W touchbar lockup (%s st=%02x%02x%02x%02x%02x%02x rc=%d probe=%s run=%u) lockups=%u gap_ms=%u forced=%u "
         "reads=%u awake_ms=%u",
         why, s_odd[0], s_odd[1], s_odd[2], s_odd[3], s_odd[4], s_odd[5], s_odd_rc, p > 0 ? "locked" : "silent",
         s_zero_run, (unsigned)s_n_lost, gap_ms, (unsigned)s_n_forced, (unsigned)s_n_reads, (unsigned)(now / 1000));
    s_zero_run = 0;
    iqs_recover(why, true);
}

/* One forced status read: a reset shows (iqs_apply_status), a lock-up or a silent chip goes through iqs_health. Not
 * initialised: the init retried (after its 200 ms). Used idle every IQS_CHECK_US and before the deep sleep. */
static void iqs_check(const char *why)
{
    if (!s_touch_ok && esp_timer_get_time() < s_retry_at)
        return;
    bool own_step = !s_iqs_deadline;
    if (own_step)
        iqs_step_begin();
    if (!s_touch_ok)
        iqs_recover(why, false);
    else if (iqs_status_rd(true) == HAL_OK)
        iqs_apply_status();
    else
        iqs_health(why);
    if (own_step)
        iqs_step_end();
}

/* A resting object (touch_btn.h): the chip re-ATIs so it stops reading it as a touch; touches are ignored for 1 s. */
static void iqs_reseed(void)
{
    bool ok = !iqs_sysctrl_set(0x04); /* RE_ATI */
    int64_t t0 = esp_timer_get_time();
    while (ok) {
        wait_us(10000);
        if (iqs_status_rd(true) == HAL_OK && !(s_st[0] & IQS_ATI_ACTIVE))
            break;
        if (iqs_expired() || esp_timer_get_time() - t0 > IQS_STEP_US) {
            ok = false;
            break;
        }
    }
    tbtn_quiet(&s_btn, esp_timer_get_time() + TBTN_QUIET_US);
    if (!ok) {
        s_touch_ok = s_iqs_ready = false;
        s_retry_mclr = true;
        s_retry_at = esp_timer_get_time() + TBTN_RETRY_US;
        tlog("W touchbar reseed failed: no touch wake, retry_ms=200");
        return;
    }
    tlog("touchbar reseeded after a resting touch");
}

/* One look at the bar, under the bus lock (the task, or the app's hal_button_down when there is no task), at `now`.
 * Only windows the chip opens are used (issue #140): idle in event mode, the first window (a touch, or a reset)
 * gets the write that starts the stream and the data comes in the next window, a report later; while down every
 * report is read; the press over, the next report's window gets the write back to event mode. A press forces
 * nothing (hardware 2026-09-26: forced reads during a touch turned taps into holds). Forced only as a fallback:
 * down without the stream (the write did not take, or the wake's press with the event window gone) every
 * TBTN_POLL_US one window, for the stream's write, else a status read (touch_btn.h). */
static void iqs_service(int64_t now)
{
    bool ev = rdy_low();
    if (ev) {
        s_n_ev++;
        s_win_us = now;
    } else if (s_stream && !s_btn.down && now - s_win_us > IQS_STREAM_GONE_US) {
        s_stream = s_want_event = false; /* no report for a while: the chip is in event mode, whatever we wrote */
    }
    if (ev && s_touch_ok && !s_btn.down && !s_init_busy) {
        if (s_want_event) { /* a report after the release: event mode again, in its window */
            iqs_mode_wr(true, false);
            return;
        }
        if (!s_stream && !iqs_mode_wr(false, false))
            return; /* the touch's window: the stream starts, its first report is read next */
    }
    bool poll = !ev && tbtn_poll_due(&s_btn, now);
    if (poll)
        tbtn_tried(&s_btn, now);
    if (ev || poll) {
        if (poll && !s_stream && s_touch_ok)
            iqs_mode_wr(false, true); /* down without the stream: one forced window a second asks for it */
        else if (iqs_status_rd(!ev) == HAL_OK)
            iqs_apply_status();
        else if (s_touch_ok)
            iqs_health(ev ? "event" : "poll"); /* a lock-up shows in the next read, whatever asked for it */
    }
    bool down = tbtn_down(&s_btn, esp_timer_get_time());
    if (!down && s_stream && s_touch_ok)
        s_want_event = true; /* the next report's window */
    if (s_btn.why && s_btn.why != s_btn_why)
        tlog("W touchbar press ended without a release read (%s) after %u ms: st=%02x%02x g=%02x reads=%u "
             "events=%u bad=%u nowin=%u", s_btn.why, (unsigned)((s_btn.last_us - s_btn.since_us) / 1000),
             s_st[1], s_st[0], s_g[0], (unsigned)s_n_reads, (unsigned)s_n_ev, (unsigned)s_n_bad,
             (unsigned)s_n_nowin);
    s_btn_why = s_btn.why;
    if (s_btn.ended)
        press_end();
    if (s_touch_ok && tbtn_reseed_take(&s_btn))
        iqs_reseed();
}

/* Idle awake, a turn of the reader (nothing while a press runs): no forced window (see IQS_HOUR_US); once an awake
 * hour one forced status read (iqs_check) and the counters, totals and the hour's own: count `touchbar hour` lines
 * with `lockups_h=` above 0 per day, and `forced_h=` against `reads_h=`. */
static int64_t s_hour_at;
static uint32_t s_hour_forced, s_hour_reads, s_hour_lost;

static void iqs_idle_turn(int64_t now)
{
    if (now - s_hour_at < IQS_HOUR_US)
        return;
    s_hour_at = now;
    iqs_check("hour");
    tlog("touchbar hour awake_h=%u forced=%u reads=%u ev=%u bad=%u nowin=%u lockups=%u forced_h=%u reads_h=%u "
         "lockups_h=%u",
         (unsigned)(now / IQS_HOUR_US), (unsigned)s_n_forced, (unsigned)s_n_reads, (unsigned)s_n_ev, (unsigned)s_n_bad,
         (unsigned)s_n_nowin, (unsigned)s_n_lost, (unsigned)(s_n_forced - s_hour_forced),
         (unsigned)(s_n_reads - s_hour_reads), (unsigned)(s_n_lost - s_hour_lost));
    s_hour_forced = s_n_forced;
    s_hour_reads = s_n_reads;
    s_hour_lost = s_n_lost;
}
