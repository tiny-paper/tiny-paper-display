/*
 * btseal.h - the Bluetooth key's crypto (docs/spec/usb-display.md "Bluetooth key"): the key id, the handshake's session
 * keys and proof, and sealing and opening one frame's payload with AES-128-CCM. All primitives are mbedtls's (HKDF-SHA256,
 * AES-CCM; on the ESP32 its AES runs on the hardware). No state but the session keys and the two counters.
 * C11. SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef TINY_BTSEAL_H
#define TINY_BTSEAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BTSEAL_KEY 32   /* the stored key */
#define BTSEAL_KID 8    /* its id */
#define BTSEAL_NONCE 16 /* a handshake nonce */
#define BTSEAL_TAG 16   /* the CCM tag a sealed payload grows by; also the proof's length */
#define BTSEAL_H2D 0x48 /* the direction byte of the CCM nonce: host to device */
#define BTSEAL_D2H 0x44 /* device to host */

typedef struct {
    uint8_t k_rx[16], k_tx[16]; /* the session keys of what this side opens and seals */
    uint64_t n_rx, n_tx;        /* the next counter of each direction */
    uint8_t d_rx, d_tx;         /* their direction bytes */
    bool up;                    /* a session: false before btseal_start and after a frame that did not open */
} btseal_t;

/* The key's id: HKDF-SHA256(key, no salt, "tpd-bt-kid-1"), 8 bytes. 0 = ok. */
int btseal_kid(const uint8_t key[BTSEAL_KEY], uint8_t kid[BTSEAL_KID]);
/* The handshake from both nonces. device: this side is the device and `proof` receives the proof; else this side is
 * the host and `proof` is checked (-1 and no session when it is wrong: the device has another key). 0 = up. */
int btseal_start(btseal_t *s, bool device, const uint8_t key[BTSEAL_KEY], const uint8_t host_nonce[BTSEAL_NONCE],
                 const uint8_t device_nonce[BTSEAL_NONCE], uint8_t proof[BTSEAL_TAG]);
/* Seal n plain bytes of the frame (seq, cmd) into out (n + BTSEAL_TAG bytes, not overlapping plain). 0 = ok; -1
 * without a session. */
int btseal_seal(btseal_t *s, uint8_t seq, uint8_t cmd, const uint8_t *plain, size_t n, uint8_t *out);
/* Open n sealed bytes of the frame (seq, cmd) into plain (n - BTSEAL_TAG bytes). -1 when it does not open with the
 * next counter: the session ends (s->up false, the keys wiped). 0 = ok. */
int btseal_open(btseal_t *s, uint8_t seq, uint8_t cmd, const uint8_t *in, size_t n, uint8_t *plain);
/* End the session, the keys wiped. */
void btseal_end(btseal_t *s);

#endif
