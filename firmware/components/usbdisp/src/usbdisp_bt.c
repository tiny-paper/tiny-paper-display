/* usbdisp: Bluetooth behind the setting (docs/spec/usb-display.md "Bluetooth"). The setting `bt` is stored, off by
 * default and changed over USB only; while it is off no bt_* call of the configuration is made (the test counts
 * them), so the radio is never started. On: the stack runs, every request of that link needs a bonded host, and
 * pairing is taken only in a window (the button, or bt { pair } over the cable) that the panel announces.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <string.h>

#include "usbdisp_priv.h"

void usbdisp_bt_init(usbdisp_t *d)
{
    uint8_t v = 0;
    d->bt_on = d->c.bt_open && d->c.kv_get && d->c.kv_get(d->c.ctx, USBDISP_KV_BT, &v, 1) == 1 && v == 1;
}

bool usbdisp_bt_on(const usbdisp_t *d) { return d->bt_on; }

uint32_t usbdisp_bt_pairing(const usbdisp_t *d)
{
    uint64_t now = d->c.now_us(d->c.ctx);
    if (!d->bt_pair_until || now >= d->bt_pair_until)
        return 0;
    return (uint32_t)((d->bt_pair_until - now + 999999) / 1000000);
}

bool usbdisp_bt_awake(const usbdisp_t *d)
{
    if (!d->bt)
        return false;
    if (d->bt_pair_until)
        return true;
    return d->c.bt_connected(d->c.ctx) && d->c.now_us(d->c.ctx) - d->bt_last_us < USBDISP_BT_QUIET_US;
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
    link_parser_init(&d->bt->parser);
    d->bt_was_connected = false;
    usbdisp_log(d, "bt: on, advertising");
}

void usbdisp_bt_stop(usbdisp_t *d)
{
    if (!d->bt)
        return;
    if (d->bt_pair_until)
        d->c.bt_pairing(d->c.ctx, false, 0);
    d->bt_pair_until = 0;
    d->c.bt_close(d->c.ctx);
    d->c.free(d->c.ctx, d->bt);
    d->bt = NULL;
}

/* The window closes; the panel says how it ended (a failed screen is only logged: the window is closed either way). */
static void window_close(usbdisp_t *d, bool paired)
{
    if (!d->bt_pair_until)
        return;
    d->bt_pair_until = 0;
    d->c.bt_pairing(d->c.ctx, false, 0);
    if (usbdisp_bt_screen(d, "BLUETOOTH", paired ? "PAIRED" : "PAIRING ENDED", d->c.bt_name))
        usbdisp_log(d, "W bt: the closing screen was not shown");
}

/* Opens the window: a new passkey, and the panel shows it. Without the screen there is no window (nobody could read
 * the passkey, and the panel must say that the device is pairing). */
static bool window_open(usbdisp_t *d)
{
    char key[8];
    if (!d->bt)
        return false;
    if (d->c.bt_connected(d->c.ctx)) { /* the connected host leaves: the one that pairs needs the connection */
        usbdisp_bt_stop(d);
        start(d);
        if (!d->bt)
            return false;
    }
    uint32_t pass = d->c.rand32(d->c.ctx) % 1000000u;
    d->c.bt_pairing(d->c.ctx, true, pass);
    d->bt_pair_until = d->c.now_us(d->c.ctx) + USBDISP_BT_PAIR_S * 1000000ull;
    snprintf(key, sizeof key, "%03u %03u", (unsigned)(pass / 1000), (unsigned)(pass % 1000));
    if (usbdisp_bt_screen(d, "BLUETOOTH PAIRING", key, d->c.bt_name)) {
        usbdisp_log(d, "E bt: no pairing screen, the window stays closed");
        d->bt_pair_until = 0;
        d->c.bt_pairing(d->c.ctx, false, 0);
        return false;
    }
    /* the window counts from when the passkey can be read */
    d->bt_pair_until = d->c.now_us(d->c.ctx) + USBDISP_BT_PAIR_S * 1000000ull;
    return true;
}

bool usbdisp_bt_button(usbdisp_t *d)
{
    if (!d->bt_on || d->in_panel)
        return false;
    if (d->bt_pair_until)
        return true; /* open already: the passkey on the panel stays */
    start(d);
    return window_open(d);
}

void usbdisp_bt_poll(usbdisp_t *d)
{
    if (!d->bt_on)
        return;
    start(d);
    if (!d->bt)
        return;
    uint64_t now = d->c.now_us(d->c.ctx);
    bool conn = d->c.bt_connected(d->c.ctx);
    if (conn && !d->bt_was_connected) {
        d->bt_last_us = now;
        link_parser_init(&d->bt->parser); /* a new host starts a new stream */
        d->bt->rx_len = d->bt->rx_pos = 0;
    }
    d->bt_was_connected = conn;
    if (d->bt_pair_until) {
        if (conn && d->c.bt_secure(d->c.ctx))
            window_close(d, true);
        else if (now >= d->bt_pair_until)
            window_close(d, false);
    }
}

bool usbdisp_bt_allowed(usbdisp_t *d, const link_frame_t *f)
{
    if (!d->c.bt_secure(d->c.ctx)) {
        usbdisp_refuse(d, f, USBDISP_E_AUTH, "pair first: this Bluetooth link is not bonded", NULL, 0);
        return false;
    }
    d->bt_last_us = d->c.now_us(d->c.ctx);
    if (f->cmd == USBDISP_CMD_BT) {
        usbdisp_refuse(d, f, USBDISP_E_CABLE, "bt works only over the USB cable", NULL, 0);
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

/* { on?, pair?, forget? } over USB: applied in the order forget, on, pair; answered before a pairing screen is drawn
 * (a refresh takes seconds). */
void usbdisp_bt_cmd(usbdisp_t *d, const link_frame_t *f)
{
    bool on = d->bt_on, pair = false, forget = false;
    int has_on = get_bool(f, "on", &on), has_pair = get_bool(f, "pair", &pair), has_forget = get_bool(f, "forget", &forget);
    if (has_on < 0 || has_pair < 0 || has_forget < 0) {
        usbdisp_refuse(d, f, USBDISP_E_BAD_REQUEST, "bt takes on, pair, forget: true or false", NULL, 0);
        return;
    }
    if (has_pair && pair && !on) {
        usbdisp_refuse(d, f, USBDISP_E_MODE, "Bluetooth is off: bt { on: true } first", NULL, 0);
        return;
    }
    bool was_open = d->bt_pair_until != 0;
    if (forget) {
        usbdisp_bt_stop(d); /* the connection of a host that is no longer bonded ends */
        d->c.bt_forget(d->c.ctx);
    }
    if (has_on && on != d->bt_on) {
        uint8_t b = on;
        if (d->c.kv_set && d->c.kv_set(d->c.ctx, USBDISP_KV_BT, &b, 1) < 0) {
            usbdisp_refuse(d, f, USBDISP_E_IO, "the setting could not be stored", NULL, 0);
            return;
        }
        d->bt_on = on;
    }
    if (has_on && on)
        d->bt_failed = false; /* asked again: try again */
    if (!d->bt_on)
        usbdisp_bt_stop(d);
    else
        start(d);
    if (d->bt_on && !d->bt) {
        usbdisp_refuse(d, f, USBDISP_E_IO, "the setting is on, but Bluetooth did not start (memory): restart the device",
                       NULL, 0);
        return;
    }
    bool open = has_pair && pair;
    cbor_w_t w;
    cbor_w_init(&w, d->tx, sizeof d->tx);
    cbor_put_map(&w, 3);
    cbor_put_str(&w, "on");
    cbor_put_bool(&w, d->bt_on);
    cbor_put_str(&w, "pairing");
    cbor_put_uint(&w, open ? USBDISP_BT_PAIR_S : has_pair ? 0 : usbdisp_bt_pairing(d));
    cbor_put_str(&w, "name");
    cbor_put_str(&w, d->c.bt_name);
    usbdisp_send(d, f, &w);
    if (open)
        window_open(d);
    else if (has_pair)
        window_close(d, false);
    else if (was_open && !d->bt_pair_until && usbdisp_bt_screen(d, "BLUETOOTH", "PAIRING ENDED", d->c.bt_name))
        usbdisp_log(d, "W bt: the closing screen was not shown"); /* forget or off closed the window */
}
