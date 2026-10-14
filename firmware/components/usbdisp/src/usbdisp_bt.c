/* usbdisp: Bluetooth behind the setting (docs/spec/usb-display.md "Bluetooth", "Bluetooth key"). The setting `bt` is
 * stored, off by default and changed over USB only; while it is off no bt_* call of the configuration is made (the
 * test counts them), so the radio is never started. On: the stack runs and a host must prove it holds the key made
 * over the cable (bt_key) with bt_auth; after that every frame both ways is sealed (btseal.h), and any frame that does
 * not open drops the link. The key lives in the settings store only (read at each handshake), never in RAM between.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>

#include "mbedtls/platform_util.h"
#include "usbdisp_priv.h"

void usbdisp_bt_init(usbdisp_t *d)
{
    uint8_t v = 0;
    d->bt_on = d->c.bt_open && d->c.kv_get && d->c.kv_get(d->c.ctx, USBDISP_KV_BT, &v, 1) == 1 && v == 1;
}

bool usbdisp_bt_on(const usbdisp_t *d) { return d->bt_on; }

bool usbdisp_bt_awake(const usbdisp_t *d)
{
    if (!d->bt)
        return false;
    return d->c.bt_connected(d->c.ctx) && d->c.now_us(d->c.ctx) - d->bt_last_us < USBDISP_BT_QUIET_US;
}

static void new_stream(usbdisp_bt_t *b)
{
    btseal_end(&b->seal);
    link_parser_init(&b->rx.parser);
    b->rx.rx_len = b->rx.rx_pos = 0;
}

static void start(usbdisp_t *d)
{
    if (d->bt || !d->bt_on || d->bt_failed)
        return;
    d->bt = d->c.alloc(d->c.ctx, sizeof *d->bt);
    if (d->bt && d->c.bt_open(d->c.ctx, d->c.bt_name)) {
        d->c.free(d->c.ctx, d->bt);
        d->bt = NULL;
    }
    if (!d->bt) {
        d->bt_failed = true;
        usbdisp_log(d, "E bt: did not start");
        return;
    }
    memset(d->bt, 0, sizeof *d->bt);
    new_stream(d->bt);
    usbdisp_log(d, "bt: on, advertising");
}

void usbdisp_bt_stop(usbdisp_t *d)
{
    if (!d->bt)
        return;
    d->c.bt_close(d->c.ctx);
    btseal_end(&d->bt->seal);
    mbedtls_platform_zeroize(d->bt->plain, sizeof d->bt->plain);
    d->c.free(d->c.ctx, d->bt);
    d->bt = NULL;
}

/* The connection ends; its session and any byte of it still unread go too. */
static void drop(usbdisp_t *d, const char *why)
{
    usbdisp_log(d, why);
    d->c.bt_drop(d->c.ctx);
    new_stream(d->bt);
}

void usbdisp_bt_poll(usbdisp_t *d)
{
    if (!d->bt_on)
        return;
    start(d);
    if (!d->bt)
        return;
    uint32_t conn = d->c.bt_connected(d->c.ctx);
    if (conn && conn != d->bt->conn) { /* a new host starts a new stream, with no session */
        d->bt_last_us = d->c.now_us(d->c.ctx);
        new_stream(d->bt);
        d->bt->conn = conn;
    } else if (!conn && d->bt->seal.up) {
        new_stream(d->bt);
    }
}

/* A frame on the Bluetooth link: as `out`, sealed when a session is up (the handshake's answer and the refusals before
 * it go plain). */
void usbdisp_bt_send(usbdisp_t *d, uint8_t *out, size_t cap, uint8_t seq, uint8_t cmd, const uint8_t *payload, size_t n)
{
    size_t len = n;
    if (d->bt && d->bt->seal.up) {
        if (cap < n + BTSEAL_TAG + LINK_FRAME_OVERHEAD || btseal_seal(&d->bt->seal, seq, cmd, payload, n, out + 6)) {
            drop(d, "E bt: an answer could not be sealed, link dropped");
            return;
        }
        payload = out + 6;
        len = n + BTSEAL_TAG;
    }
    size_t k = link_frame_encode(out, cap, seq, cmd, payload, len);
    if (k)
        d->c.bt_write(d->c.ctx, out, k);
}

/* The stored key: true when there is one (BTSEAL_KEY bytes) */
static bool stored_key(usbdisp_t *d, uint8_t key[BTSEAL_KEY])
{
    return d->c.kv_get && d->c.kv_get(d->c.ctx, USBDISP_KV_BTKEY, key, BTSEAL_KEY) == BTSEAL_KEY;
}

/* A byte string of exactly n bytes under `key` of the request */
static bool get_bytes(const link_frame_t *f, const char *key, uint8_t *out, size_t n)
{
    const uint8_t *v;
    size_t vn;
    cbor_r_t r;
    cbor_item_t it;
    if (!cbor_map_get(f->payload, f->len, key, strlen(key), &v, &vn))
        return false;
    cbor_r_init(&r, v, vn);
    if (!cbor_read(&r, &it) || it.type != CBOR_BSTR || it.len != n)
        return false;
    memcpy(out, it.ptr, n);
    return true;
}

/* The handshake: { kid, nonce } with the stored key. Answered plain with { nonce, proof }, then the session is up. */
static bool auth(usbdisp_t *d, const link_frame_t *f)
{
    uint8_t key[BTSEAL_KEY], kid[BTSEAL_KID], want[BTSEAL_KID], hn[BTSEAL_NONCE], dn[BTSEAL_NONCE], proof[BTSEAL_TAG];
    bool ok = f->len && f->payload[0] >> 5 == 5 && cbor_valid(f->payload, f->len) && get_bytes(f, "kid", kid, sizeof kid) &&
              get_bytes(f, "nonce", hn, sizeof hn) && stored_key(d, key);
    ok = ok && !btseal_kid(key, want) && !memcmp(kid, want, sizeof kid);
    if (ok) {
        d->c.random(d->c.ctx, dn, sizeof dn);
        ok = !btseal_start(&d->bt->seal, true, key, hn, dn, proof);
    }
    mbedtls_platform_zeroize(key, sizeof key);
    if (!ok)
        return false;
    btseal_t s = d->bt->seal; /* the answer goes plain: the host needs the nonce to have the keys */
    d->bt->seal.up = false;
    cbor_w_t w;
    cbor_w_init(&w, d->tx, sizeof d->tx);
    cbor_put_map(&w, 2);
    cbor_put_str(&w, "nonce");
    cbor_put_bstr(&w, dn, sizeof dn);
    cbor_put_str(&w, "proof");
    cbor_put_bstr(&w, proof, sizeof proof);
    usbdisp_send(d, f, &w);
    d->bt->seal = s;
    d->bt->conn = d->c.bt_connected(d->c.ctx); /* the session belongs to this connection */
    btseal_end(&s);
    d->bt_last_us = d->c.now_us(d->c.ctx);
    usbdisp_log(d, "bt: host authenticated");
    return true;
}

bool usbdisp_bt_unseal(usbdisp_t *d, link_frame_t *f)
{
    usbdisp_bt_t *b = d->bt;
    if (b->seal.up) {
        if (f->len < BTSEAL_TAG || d->c.bt_connected(d->c.ctx) != b->conn ||
            btseal_open(&b->seal, f->seq, f->cmd, f->payload, f->len, b->plain)) {
            drop(d, "W bt: a frame did not open (forged, replayed, out of order or another key): link dropped");
            return false;
        }
        f->payload = b->plain;
        f->len -= BTSEAL_TAG;
        return true;
    }
    if (f->cmd == USBDISP_CMD_BT_AUTH && f->seq && auth(d, f))
        return false;
    if (f->cmd >= 0x01 && f->cmd <= 0x3F && f->seq)
        usbdisp_refuse(d, f, USBDISP_E_AUTH, "authenticate first: bt_auth with the key made over the USB cable", NULL, 0);
    drop(d, "W bt: not authenticated, link dropped");
    return false;
}

bool usbdisp_bt_allowed(usbdisp_t *d, const link_frame_t *f)
{
    d->bt_last_us = d->c.now_us(d->c.ctx);
    if (f->cmd == USBDISP_CMD_BT || f->cmd == USBDISP_CMD_BT_KEY) {
        usbdisp_refuse(d, f, USBDISP_E_CABLE, "bt and bt_key work only over the USB cable", NULL, 0);
        return false;
    }
    return true;
}

/* A bool key of the request: 1 present (*out), 0 absent, -1 not a bool. */
static int get_bool(const link_frame_t *f, const char *key, bool *out)
{
    const uint8_t *v;
    size_t n;
    cbor_r_t r;
    cbor_item_t it;
    if (!cbor_map_get(f->payload, f->len, key, strlen(key), &v, &n))
        return 0;
    cbor_r_init(&r, v, n);
    if (!cbor_read(&r, &it) || (it.type != CBOR_TRUE && it.type != CBOR_FALSE))
        return -1;
    *out = it.type == CBOR_TRUE;
    return 1;
}

/* Store the setting `on`; false when the store failed (refused IO). */
static bool set_on(usbdisp_t *d, const link_frame_t *f, bool on)
{
    if (on != d->bt_on) {
        uint8_t b = on;
        if (d->c.kv_set && d->c.kv_set(d->c.ctx, USBDISP_KV_BT, &b, 1) < 0) {
            usbdisp_refuse(d, f, USBDISP_E_IO, "the setting could not be stored", NULL, 0);
            return false;
        }
        d->bt_on = on;
    }
    if (on)
        d->bt_failed = false; /* asked again: try again */
    if (!d->bt_on)
        usbdisp_bt_stop(d);
    else
        start(d);
    if (d->bt_on && !d->bt) {
        usbdisp_refuse(d, f, USBDISP_E_IO, "the setting is on, but Bluetooth did not start (memory): restart the device",
                       NULL, 0);
        return false;
    }
    return true;
}

/* { on, name, kid?, key? } */
static void state(usbdisp_t *d, const link_frame_t *f, const uint8_t *new_key)
{
    uint8_t key[BTSEAL_KEY], kid[BTSEAL_KID];
    bool has = new_key ? true : stored_key(d, key);
    has = has && !btseal_kid(new_key ? new_key : key, kid);
    mbedtls_platform_zeroize(key, sizeof key);
    cbor_w_t w;
    cbor_w_init(&w, d->tx, sizeof d->tx);
    cbor_put_map(&w, 2 + (size_t)has + (new_key != NULL));
    cbor_put_str(&w, "on");
    cbor_put_bool(&w, d->bt_on);
    cbor_put_str(&w, "name");
    cbor_put_str(&w, d->c.bt_name);
    if (has) {
        cbor_put_str(&w, "kid");
        cbor_put_bstr(&w, kid, sizeof kid);
    }
    if (new_key) {
        cbor_put_str(&w, "key");
        cbor_put_bstr(&w, new_key, BTSEAL_KEY);
    }
    usbdisp_send(d, f, &w);
    mbedtls_platform_zeroize(d->tx, (size_t)w.len);
}

/* { on?, forget? } over USB: applied in the order forget, on. */
void usbdisp_bt_cmd(usbdisp_t *d, const link_frame_t *f)
{
    bool on = d->bt_on, forget = false;
    int has_on = get_bool(f, "on", &on), has_forget = get_bool(f, "forget", &forget);
    if (has_on < 0 || has_forget < 0) {
        usbdisp_refuse(d, f, USBDISP_E_BAD_REQUEST, "bt takes on, forget: true or false", NULL, 0);
        return;
    }
    if (forget) {
        usbdisp_bt_stop(d); /* the connection of a host with the old key ends */
        if (d->c.kv_erase && d->c.kv_erase(d->c.ctx, USBDISP_KV_BTKEY) < 0) {
            usbdisp_refuse(d, f, USBDISP_E_IO, "the key could not be erased", NULL, 0);
            return;
        }
        d->c.bt_forget(d->c.ctx);
    }
    if (has_on || forget) {
        if (!set_on(d, f, on))
            return;
    } else if (d->bt_on) {
        start(d);
    }
    state(d, f, NULL);
}

/* bt_key over USB: the setting on (the radio runs: the RNG's entropy), a new key stored and answered once. */
void usbdisp_bt_key_cmd(usbdisp_t *d, const link_frame_t *f)
{
    uint8_t key[BTSEAL_KEY];
    if (!set_on(d, f, true))
        return;
    if (d->c.bt_connected(d->c.ctx))
        drop(d, "bt: a new key, the connected host leaves"); /* its session was made with the old key */
    d->c.random(d->c.ctx, key, sizeof key);
    if (!d->c.kv_set || d->c.kv_set(d->c.ctx, USBDISP_KV_BTKEY, key, sizeof key) < 0) {
        mbedtls_platform_zeroize(key, sizeof key);
        usbdisp_refuse(d, f, USBDISP_E_IO, "the key could not be stored", NULL, 0);
        return;
    }
    usbdisp_log(d, "bt: a new key");
    state(d, f, key);
    mbedtls_platform_zeroize(key, sizeof key);
}

void usbdisp_bt_auth_cmd(usbdisp_t *d, const link_frame_t *f)
{
    usbdisp_refuse(d, f, USBDISP_E_BAD_REQUEST, d->via_bt ? "bt_auth: this link is authenticated already"
                                                           : "bt_auth is for Bluetooth only", NULL, 0);
}
