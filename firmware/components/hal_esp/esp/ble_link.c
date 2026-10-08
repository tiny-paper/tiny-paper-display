/* hal_esp: the BLE link transport of config mode (link-protocol.md "Bluetooth"), NimBLE peripheral.
 * One GATT service: RX (write, host to device, into a ring), TX (notify, device to host, MTU-3 pieces),
 * Pair (read, needs an authenticated encrypted link: the host reads it to make its OS pair).
 * LE Secure Connections only, passkey entry (DisplayOnly, MITM), bonding. The whole stack (host and
 * controller) is brought up by hal_link_open(HAL_LINK_BLE) and torn down and freed by hal_link_close.
 * Callbacks run in the NimBLE host task; the app reads and writes from its own task (one reader, one writer).
 * Without CONFIG_TINY_BLE (every board by default, QEMU) everything answers "no BLE" and NimBLE is not in the image.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>

#include "ble_heap.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal_esp_priv.h"
#include "nvs.h"

#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

void ble_store_config_init(void);

#define RX_CAP 8192               /* >= one 6 KiB frame plus the next writes */
#define TX_DEADLINE_MS 5000
#define SYNC_TIMEOUT_MS 3000

/* 48770001-4516-40a8-8cc6-57e06fe2ff0d and siblings, little endian */
#define TP_UUID(n) BLE_UUID128_INIT(0x0d, 0xff, 0xe2, 0x6f, 0xe0, 0x57, 0xc6, 0x8c, 0xa8, 0x40, 0x16, 0x45, n, 0x00, 0x77, 0x48)
static const ble_uuid128_t UUID_SVC = TP_UUID(0x01);
static const ble_uuid128_t UUID_RX = TP_UUID(0x02);
static const ble_uuid128_t UUID_TX = TP_UUID(0x03);
static const ble_uuid128_t UUID_PAIR = TP_UUID(0x04);

static char s_name[32] = "tiny-paper";
static volatile uint32_t s_passkey;
static volatile bool s_open, s_closing, s_subscribed;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile uint16_t s_mtu = 23, s_fails;
static volatile uint32_t s_connects;
static uint32_t s_heap_min;
static uint16_t s_tx_handle;
/* CONFIG_TINY_BLE_GATED: pairing is taken only while the window is open (hal_esp_ble_pairing) */
#define GATED 1
#define RX_SEC (BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN)
#define ADV_ITVL 160 /* x 0.625 ms = 100 ms */
static volatile bool s_pair_open;
static uint8_t s_own_addr;

/* RX ring: the host task writes (head), the app reads (tail) */
static uint8_t *s_rx;
static volatile uint32_t s_head, s_tail;
static SemaphoreHandle_t s_rx_sem, s_sync_sem;

static void heap_sample(void)
{
    uint32_t f = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (!s_heap_min || f < s_heap_min)
        s_heap_min = f;
}

static void heap_log(const char *at)
{
    hal_logf("ble heap at=%s free=%u largest=%u min=%u", at, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

/* ------------------------------------------------------------------ GATT */

static size_t rx_used(void) { return (s_head - s_tail) % RX_CAP; }

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int which = (int)(intptr_t)arg;
    if (which == 2 && ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (!s_rx || len > RX_CAP - 1 - rx_used())
            return BLE_ATT_ERR_INSUFFICIENT_RES; /* full: the host sends it again (spec) */
        uint32_t h = s_head, first = RX_CAP - h < len ? RX_CAP - h : len;
        if (os_mbuf_copydata(ctxt->om, 0, first, s_rx + h) ||
            (len > first && os_mbuf_copydata(ctxt->om, first, len - first, s_rx)))
            return BLE_ATT_ERR_UNLIKELY;
        __atomic_store_n(&s_head, (h + len) % RX_CAP, __ATOMIC_RELEASE);
        xSemaphoreGive(s_rx_sem);
        return 0;
    }
    if (which == 4 && ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        static const uint8_t one = 1;
        return os_mbuf_append(ctxt->om, &one, 1) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def SVCS[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = &UUID_RX.u, .access_cb = chr_access, .arg = (void *)2,
             .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | RX_SEC},
            {.uuid = &UUID_TX.u, .access_cb = chr_access, .arg = (void *)3, .flags = BLE_GATT_CHR_F_NOTIFY,
             .val_handle = &s_tx_handle},
            {.uuid = &UUID_PAIR.u, .access_cb = chr_access, .arg = (void *)4,
             .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN},
            {0},
        },
    },
    {0},
};

/* ------------------------------------------------------------------ GAP */

static int gap_event(struct ble_gap_event *ev, void *arg);

static void adv_start(void)
{
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = &UUID_SVC;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    struct ble_hs_adv_fields r = {0};
    size_t nl = strlen(s_name);
    r.name = (uint8_t *)s_name;
    r.name_len = nl > 29 ? 29 : nl; /* 31 bytes minus the AD header */
    r.name_is_complete = nl <= 29;
    if (!rc)
        rc = ble_gap_adv_rsp_set_fields(&r);
    struct ble_gap_adv_params p = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN,
                                   .itvl_min = ADV_ITVL, .itvl_max = ADV_ITVL};
    if (!rc)
        rc = ble_gap_adv_start(s_own_addr, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc && rc != BLE_HS_EALREADY)
        hal_logf("E ble adv start rc=%d", rc);
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    heap_sample();
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            s_mtu = 23;
            s_connects++;
            hal_logf("ble connect n=%u", (unsigned)s_connects);
            if (GATED) /* longer packets when the host takes them; nothing depends on it */
                ble_gap_set_data_len(s_conn, 251, 2120);
        } else if (s_open && !s_closing) {
            adv_start();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        hal_logf("ble disconnect reason=0x%x", ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        if (s_open && !s_closing)
            adv_start();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_open && !s_closing && s_conn == BLE_HS_CONN_HANDLE_NONE)
            adv_start();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_tx_handle)
            s_subscribed = ev->subscribe.cur_notify;
        break;
    case BLE_GAP_EVENT_MTU:
        s_mtu = ev->mtu.value;
        break;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc d;
        bool ok = ev->enc_change.status == 0 && ble_gap_conn_find(ev->enc_change.conn_handle, &d) == 0;
        if (!ok && s_fails < 0xFFFF)
            s_fails++;
        hal_logf("ble enc status=%d auth=%d bonded=%d key=%d fails=%u", ev->enc_change.status,
                 ok ? d.sec_state.authenticated : 0, ok ? d.sec_state.bonded : 0, ok ? d.sec_state.key_size : 0,
                 (unsigned)s_fails);
        /* gated: an encrypted link without an authenticated bond (Just Works, a short key) is not kept */
        if (GATED && ok && !(d.sec_state.authenticated && d.sec_state.bonded && d.sec_state.key_size == 16)) {
            ble_store_util_delete_peer(&d.peer_id_addr);
            ble_gap_terminate(ev->enc_change.conn_handle, BLE_ERR_AUTH_FAIL);
        }
        break;
    }
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io io = {.action = ev->passkey.params.action};
        if (GATED && !s_pair_open) { /* no window: no pairing, the host is dropped and nothing is stored */
            hal_logf("W ble pairing refused: the window is closed");
            ble_gap_terminate(ev->passkey.conn_handle, BLE_ERR_AUTH_FAIL);
        } else if (io.action == BLE_SM_IOACT_DISP) {
            io.passkey = s_passkey % 1000000u; /* shown on the panel; never logged */
            ble_sm_inject_io(ev->passkey.conn_handle, &io);
        } else {
            hal_logf("W ble passkey action %d refused", io.action);
        }
        break;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* a known host lost its bond: forget ours and pair again (with the passkey) */
        struct ble_gap_conn_desc d;
        if (GATED && !s_pair_open)
            return BLE_GAP_REPEAT_PAIRING_IGNORE; /* our bond stays, the host must wait for a window */
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0)
            ble_store_util_delete_peer(&d.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr);
    adv_start();
    xSemaphoreGive(s_sync_sem);
}

static void on_reset(int reason) { hal_logf("W ble host reset reason=%d", reason); }

static void host_task(void *arg)
{
    nimble_port_run(); /* returns after nimble_port_stop */
    nimble_port_freertos_deinit();
}

/* ------------------------------------------------------------------ HAL */

static void free_bufs(void)
{
    free(s_rx);
    s_rx = NULL;
    if (s_rx_sem)
        vSemaphoreDelete(s_rx_sem);
    if (s_sync_sem)
        vSemaphoreDelete(s_sync_sem);
    s_rx_sem = s_sync_sem = NULL;
}

void hal_link_ble_setup(const char *name, uint32_t passkey)
{
    snprintf(s_name, sizeof s_name, "%s", name && name[0] ? name : "tiny-paper");
    s_passkey = passkey % 1000000u;
}

int hal_ble_open(void)
{
    if (s_open)
        return HAL_OK;
    heap_log("open_before");
    if (!ble_open_fits(heap_caps_get_free_size(MALLOC_CAP_8BIT))) { /* membudget.h MB_BLE_OPEN */
        hal_logf("E ble open: heap too small");
        return HAL_E_NOSPACE;
    }
    heap_caps_monitor_local_minimum_free_size_start();
    s_heap_min = 0;
    heap_sample();
    s_head = s_tail = 0;
    s_rx = malloc(RX_CAP);
    s_rx_sem = xSemaphoreCreateBinary();
    s_sync_sem = xSemaphoreCreateBinary();
    if (!s_rx || !s_rx_sem || !s_sync_sem) {
        free_bufs();
        heap_caps_monitor_local_minimum_free_size_stop();
        return HAL_E_NOSPACE;
    }
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_subscribed = s_closing = false;
    s_pair_open = false;
    s_mtu = 23;
    s_fails = 0;
    s_connects = 0;
    esp_err_t e = nimble_port_init(); /* controller + host; frees what it took on failure */
    if (e != ESP_OK) {
        hal_logf("E ble init: %s", esp_err_to_name(e));
        free_bufs();
        heap_caps_monitor_local_minimum_free_size_stop();
        heap_log("open_failed");
        return e == ESP_ERR_NO_MEM ? HAL_E_NOSPACE : HAL_E_FAIL;
    }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(SVCS);
    if (!rc)
        rc = ble_gatts_add_svcs(SVCS);
    if (!rc)
        rc = ble_svc_gap_device_name_set(s_name);
    if (rc)
        hal_logf("E ble gatt rc=%d", rc);
    ble_store_config_init();
    s_open = true;
    nimble_port_freertos_init(host_task);
    if (rc || xSemaphoreTake(s_sync_sem, pdMS_TO_TICKS(SYNC_TIMEOUT_MS)) != pdTRUE) {
        hal_logf("E ble: no host sync");
        hal_link_close(HAL_LINK_BLE);
        return HAL_E_FAIL;
    }
    heap_log("open");
    return HAL_OK;
}

void hal_ble_close(void)
{
    if (!s_open)
        return;
    s_closing = true;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        vTaskDelay(pdMS_TO_TICKS(200)); /* let the last notifications (a response) go out */
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        for (int i = 0; i < 50 && s_conn != BLE_HS_CONN_HANDLE_NONE; i++)
            vTaskDelay(pdMS_TO_TICKS(20));
    }
    ble_gap_adv_stop();
    heap_sample();
    if (nimble_port_stop() != 0)
        hal_logf("E ble stop failed");
    esp_err_t e = nimble_port_deinit();
    if (e != ESP_OK)
        hal_logf("E ble deinit: %s", esp_err_to_name(e));
    s_open = false;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_subscribed = false;
    free_bufs();
    uint32_t lmin = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    if (lmin && (!s_heap_min || lmin < s_heap_min))
        s_heap_min = lmin;
    heap_caps_monitor_local_minimum_free_size_stop();
    hal_logf("ble closed heap_min=%u", (unsigned)s_heap_min);
    heap_log("closed");
}

long hal_ble_read(uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    if (!s_open || !s_rx)
        return HAL_E_NOTFOUND;
    if (!rx_used())
        xSemaphoreTake(s_rx_sem, pdMS_TO_TICKS(timeout_ms));
    uint32_t head = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE), t = s_tail;
    size_t n = 0;
    while (n < cap && t != head) {
        buf[n++] = s_rx[t];
        t = (t + 1) % RX_CAP;
    }
    __atomic_store_n(&s_tail, t, __ATOMIC_RELEASE);
    return (long)n;
}

long hal_ble_write(const uint8_t *buf, size_t len)
{
    if (!s_open)
        return HAL_E_NOTFOUND;
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(TX_DEADLINE_MS);
    size_t pos = 0;
    while (pos < len) {
        uint16_t conn = s_conn;
        if (conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed)
            break; /* no peer listening: dropped */
        size_t piece = len - pos, max = s_mtu > 3 ? s_mtu - 3u : 20u;
        if (piece > max)
            piece = max;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf + pos, (uint16_t)piece);
        int rc = om ? ble_gatts_notify_custom(conn, s_tx_handle, om) : BLE_HS_ENOMEM; /* om is consumed */
        if (rc == 0) {
            pos += piece;
            continue;
        }
        if ((rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) || (int32_t)(xTaskGetTickCount() - end) >= 0) {
            hal_logf("W ble tx dropped rc=%d at=%u/%u", rc, (unsigned)pos, (unsigned)len);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5)); /* buffers busy with earlier notifications: flow control */
    }
    heap_sample();
    return (long)len;
}

void hal_esp_ble_pairing(bool open) { s_pair_open = open; }

void hal_link_ble_state(hal_ble_state_t *out)
{
    memset(out, 0, sizeof *out);
    out->open = s_open;
    out->connected = s_open && s_conn != BLE_HS_CONN_HANDLE_NONE;
    out->secure = hal_link_ble_secure();
    out->mtu = out->connected ? s_mtu : 23;
    out->pair_fails = s_fails;
    out->connects = s_connects;
    out->heap_min = s_heap_min;
}

bool hal_link_ble_secure(void)
{
    uint16_t c = s_conn;
    struct ble_gap_conn_desc d;
    if (!s_open || c == BLE_HS_CONN_HANDLE_NONE || ble_gap_conn_find(c, &d) != 0)
        return false;
    return d.sec_state.encrypted && d.sec_state.authenticated && d.sec_state.bonded && d.sec_state.key_size == 16;
}

void hal_link_ble_forget(void)
{
    if (s_open) {
        ble_store_clear();
        return;
    }
    nvs_handle_t h;
    if (nvs_open("nimble_bond", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}


/* "!tp ble open|close|state" (bring-up and QEMU: the stack alone, without the app's config mode). */
bool hal_ble_textcmd(const uint8_t *buf, size_t len)
{
    if (len < 7 || memcmp(buf, "!tp ble", 7))
        return false;
    hal_logf("ble textcmd: not in this build");
    return true;
}
