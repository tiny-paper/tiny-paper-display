/* hal_esp: the BLE link transport of the display firmware (usb-display.md "Bluetooth"), NimBLE peripheral.
 * One GATT service: RX (write, host to device, into a ring), TX (notify, device to host, MTU-3 pieces).
 * No pairing and no bonds (NimBLE built without its security manager): the core seals every frame with the key made
 * over the cable (usb-display.md "Bluetooth key"). The whole stack (host and controller) is brought up by
 * hal_link_open(HAL_LINK_BLE) and torn down and freed by hal_link_close.
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
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define RX_CAP 8192               /* >= one 6 KiB frame plus the next writes */
#define TX_DEADLINE_MS 5000
#define SYNC_TIMEOUT_MS 3000

/* 48770001-4516-40a8-8cc6-57e06fe2ff0d and siblings, little endian */
#define TP_UUID(n) BLE_UUID128_INIT(0x0d, 0xff, 0xe2, 0x6f, 0xe0, 0x57, 0xc6, 0x8c, 0xa8, 0x40, 0x16, 0x45, n, 0x00, 0x77, 0x48)
static const ble_uuid128_t UUID_SVC = TP_UUID(0x01);
static const ble_uuid128_t UUID_RX = TP_UUID(0x02);
static const ble_uuid128_t UUID_TX = TP_UUID(0x03);

static char s_name[32] = "tiny-paper";
static volatile bool s_open, s_closing, s_subscribed;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile uint16_t s_mtu = 23;
static volatile uint16_t s_dropped = BLE_HS_CONN_HANDLE_NONE; /* the connection hal_link_ble_drop ended: RX refuses it */
static volatile uint32_t s_connects;
static uint32_t s_heap_min;
static uint16_t s_tx_handle;
#define ADV_ITVL 160 /* x 0.625 ms = 100 ms */
static uint8_t s_own_addr;

/* RX ring: the host task writes (head), the app reads (tail). s_mark: where the latest connection's bytes start (set
 * by the host task at its connect, s_marked true until the app skips what came before) */
static uint8_t *s_rx;
static volatile uint32_t s_head, s_tail, s_mark;
static volatile bool s_marked;
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
        if (conn == s_dropped)
            return BLE_ATT_ERR_UNLIKELY; /* ended by the core (a refusal): nothing more of it is read */
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
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def SVCS[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = &UUID_RX.u, .access_cb = chr_access, .arg = (void *)2,
             .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
            {.uuid = &UUID_TX.u, .access_cb = chr_access, .arg = (void *)3, .flags = BLE_GATT_CHR_F_NOTIFY,
             .val_handle = &s_tx_handle},
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
            s_mark = s_head; /* what is in the ring now belongs to an earlier connection */
            __atomic_store_n(&s_marked, true, __ATOMIC_RELEASE);
            s_conn = ev->connect.conn_handle;
            s_mtu = 23;
            s_connects++;
            hal_logf("ble connect n=%u", (unsigned)s_connects);
            ble_gap_set_data_len(s_conn, 251, 2120); /* longer packets when the host takes them; nothing depends on it */
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

void hal_link_ble_setup(const char *name)
{
    snprintf(s_name, sizeof s_name, "%s", name && name[0] ? name : "tiny-paper");
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
    s_head = s_tail = s_mark = 0;
    s_marked = false;
    s_dropped = BLE_HS_CONN_HANDLE_NONE;
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
    s_mtu = 23;
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
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(SVCS);
    if (!rc)
        rc = ble_gatts_add_svcs(SVCS);
    if (!rc)
        rc = ble_svc_gap_device_name_set(s_name);
    if (rc)
        hal_logf("E ble gatt rc=%d", rc);
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
    if (__atomic_exchange_n(&s_marked, false, __ATOMIC_ACQ_REL)) { /* a new connection: skip the bytes of the last */
        uint32_t h = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE), m = s_mark;
        if ((m - s_tail) % RX_CAP <= (h - s_tail) % RX_CAP) /* never back over bytes already read */
            __atomic_store_n(&s_tail, m, __ATOMIC_RELEASE);
    }
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

void hal_link_ble_state(hal_ble_state_t *out)
{
    memset(out, 0, sizeof *out);
    out->open = s_open;
    out->connected = s_open && s_conn != BLE_HS_CONN_HANDLE_NONE;
    out->mtu = out->connected ? s_mtu : 23;
    out->connects = s_connects;
    out->heap_min = s_heap_min;
}

void hal_link_ble_drop(void)
{
    uint16_t c = s_conn;
    if (!s_open || c == BLE_HS_CONN_HANDLE_NONE)
        return;
    s_dropped = c;
    vTaskDelay(pdMS_TO_TICKS(100)); /* let the last notification (a refusal) go out */
    ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
    __atomic_store_n(&s_tail, __atomic_load_n(&s_head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE); /* its unread bytes */
}

/* The bonds of an older firmware that paired (NimBLE's store in NVS): erased, nothing else uses them now */
void hal_link_ble_forget(void)
{
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
