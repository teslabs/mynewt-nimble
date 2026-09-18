/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */
#include "syscfg/syscfg.h"
#if MYNEWT_VAL(BLE_SM_SC)
#include "host/ble_sm_ctkd.h"
#include <mbedtls/cmac.h>
#include <mbedtls/platform_util.h>
#include <string.h>

uint8_t
ble_sm_ctkd_pairing(const uint8_t request[6], const uint8_t response[6])
{
    /* Bonding + Secure Connections, full-length key, LinkKey in both directions. */
    if ((request[2] & response[2] & 0x09) != 0x09 || request[3] != 16 ||
        response[3] != 16 ||
        !(request[4] & request[5] & response[4] & response[5] & 0x08)) {
        return 0;
    }
    return BLE_SM_CTKD_NEGOTIATED |
           ((request[2] & response[2] & 0x20) ? BLE_SM_CTKD_CT2 : 0);
}

int
ble_sm_ctkd_derive(const uint8_t ltk[16], bool ct2, uint8_t link_key[16])
{
    static const uint8_t salt[16] = { 0, 0, 0, 0, 0,   0,   0,   0,
                                      0, 0, 0, 0, 't', 'm', 'p', '1' };
    static const uint8_t key_id[4] = { 'l', 'e', 'b', 'r' };
    uint8_t key[16], intermediate[16], result[16];
    const mbedtls_cipher_info_t *cipher;
    int rc;

    for (unsigned i = 0; i < 16; ++i) {
        key[i] = ltk[15 - i];
    }
    cipher = mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB);
    if (ct2) {
        rc = mbedtls_cipher_cmac(cipher, salt, 128, key, sizeof(key), intermediate);
    } else {
        rc = mbedtls_cipher_cmac(cipher, key, 128, salt + 12, 4, intermediate);
    }
    if (!rc) {
        rc = mbedtls_cipher_cmac(cipher, intermediate, 128, key_id,
                                 sizeof(key_id), result);
    }
    memset(link_key, 0, 16);
    if (!rc) {
        for (unsigned i = 0; i < 16; ++i) {
            link_key[i] = result[15 - i];
        }
    }
    mbedtls_platform_zeroize(key, sizeof(key));
    mbedtls_platform_zeroize(intermediate, sizeof(intermediate));
    mbedtls_platform_zeroize(result, sizeof(result));
    return rc;
}
#endif
