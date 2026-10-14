/* btseal: the Bluetooth key's crypto on mbedtls (usb-display.md "Bluetooth key"; btseal.h). Vectors: bt-key.json.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "btseal.h"

#include <string.h>

#include "mbedtls/ccm.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"

#define INFO_KID "tpd-bt-kid-1"
#define INFO_SESSION "tpd-bt-session-1"
#define PROOF_AAD "tpd-bt-auth-1"
#define CCM_NONCE 13

static int hkdf(const uint8_t *salt, size_t salt_len, const uint8_t *key, const char *info, uint8_t *out, size_t n)
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md)
        return -1;
    return mbedtls_hkdf(md, salt, salt_len, key, BTSEAL_KEY, (const unsigned char *)info, strlen(info), out, n) ? -1 : 0;
}

/* direction, 4 zero bytes, the counter big endian */
static void ccm_nonce(uint8_t dir, uint64_t ctr, uint8_t out[CCM_NONCE])
{
    memset(out, 0, CCM_NONCE);
    out[0] = dir;
    for (int i = 0; i < 8; i++)
        out[5 + i] = (uint8_t)(ctr >> (56 - 8 * i));
}

/* tag_out: seal and write the tag there; else open and check tag_in */
static int ccm(const uint8_t key[16], uint8_t dir, uint64_t ctr, const uint8_t *aad, size_t aad_len, const uint8_t *in,
               size_t n, uint8_t *out, uint8_t *tag_out, const uint8_t *tag_in)
{
    uint8_t nonce[CCM_NONCE];
    mbedtls_ccm_context c;
    ccm_nonce(dir, ctr, nonce);
    mbedtls_ccm_init(&c);
    int rc = mbedtls_ccm_setkey(&c, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (!rc)
        rc = tag_out ? mbedtls_ccm_encrypt_and_tag(&c, n, nonce, CCM_NONCE, aad, aad_len, in, out, tag_out, BTSEAL_TAG)
                     : mbedtls_ccm_auth_decrypt(&c, n, nonce, CCM_NONCE, aad, aad_len, in, out, tag_in, BTSEAL_TAG);
    mbedtls_ccm_free(&c);
    return rc ? -1 : 0;
}

int btseal_kid(const uint8_t key[BTSEAL_KEY], uint8_t kid[BTSEAL_KID])
{
    return hkdf(NULL, 0, key, INFO_KID, kid, BTSEAL_KID);
}

void btseal_end(btseal_t *s) { mbedtls_platform_zeroize(s, sizeof *s); }

int btseal_start(btseal_t *s, bool device, const uint8_t key[BTSEAL_KEY], const uint8_t host_nonce[BTSEAL_NONCE],
                 const uint8_t device_nonce[BTSEAL_NONCE], uint8_t proof[BTSEAL_TAG])
{
    uint8_t salt[2 * BTSEAL_NONCE], keys[32], aad[sizeof PROOF_AAD - 1 + BTSEAL_KID + 2 * BTSEAL_NONCE], tag[BTSEAL_TAG];
    btseal_end(s);
    memcpy(salt, host_nonce, BTSEAL_NONCE);
    memcpy(salt + BTSEAL_NONCE, device_nonce, BTSEAL_NONCE);
    memcpy(aad, PROOF_AAD, sizeof PROOF_AAD - 1);
    int rc = btseal_kid(key, aad + sizeof PROOF_AAD - 1);
    memcpy(aad + sizeof PROOF_AAD - 1 + BTSEAL_KID, salt, sizeof salt);
    if (!rc)
        rc = hkdf(salt, sizeof salt, key, INFO_SESSION, keys, sizeof keys);
    /* the proof: the tag of nothing, device to host key, counter 0 */
    if (!rc)
        rc = ccm(keys + 16, BTSEAL_D2H, 0, aad, sizeof aad, NULL, 0, NULL, tag, NULL);
    if (!rc && device)
        memcpy(proof, tag, BTSEAL_TAG);
    else if (!rc && mbedtls_ct_memcmp(proof, tag, BTSEAL_TAG))
        rc = -1;
    if (!rc) {
        memcpy(device ? s->k_rx : s->k_tx, keys, 16);
        memcpy(device ? s->k_tx : s->k_rx, keys + 16, 16);
        s->d_rx = device ? BTSEAL_H2D : BTSEAL_D2H;
        s->d_tx = device ? BTSEAL_D2H : BTSEAL_H2D;
        s->n_rx = device ? 0 : 1;
        s->n_tx = device ? 1 : 0;
        s->up = true;
    }
    mbedtls_platform_zeroize(keys, sizeof keys);
    return rc;
}

int btseal_seal(btseal_t *s, uint8_t seq, uint8_t cmd, const uint8_t *plain, size_t n, uint8_t *out)
{
    const uint8_t aad[2] = {seq, cmd};
    if (!s->up || ccm(s->k_tx, s->d_tx, s->n_tx, aad, 2, plain, n, out, out + n, NULL)) {
        btseal_end(s);
        return -1;
    }
    s->n_tx++;
    return 0;
}

int btseal_open(btseal_t *s, uint8_t seq, uint8_t cmd, const uint8_t *in, size_t n, uint8_t *plain)
{
    const uint8_t aad[2] = {seq, cmd};
    if (!s->up || n < BTSEAL_TAG ||
        ccm(s->k_rx, s->d_rx, s->n_rx, aad, 2, in, n - BTSEAL_TAG, plain, NULL, in + n - BTSEAL_TAG)) {
        btseal_end(s);
        return -1;
    }
    s->n_rx++;
    return 0;
}
