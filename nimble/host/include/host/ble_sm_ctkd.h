/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef H_BLE_SM_CTKD_
#define H_BLE_SM_CTKD_
#include <stdbool.h>
#include <stdint.h>

#define BLE_SM_CTKD_NEGOTIATED 1
#define BLE_SM_CTKD_CT2        2

/* Pairing Request/Response payloads, six octets each, excluding the opcode.
 * Returns zero unless both peers negotiate bonded, 128-bit LE SC and LinkKey. */
uint8_t ble_sm_ctkd_pairing(const uint8_t request[6], const uint8_t response[6]);
/* Core Vol 3, Part H, 2.4.2.4. Keys use HCI/SMP little-endian octet order.
 * Returns zero on success; clears the output on cryptographic failure. */
int ble_sm_ctkd_derive(const uint8_t ltk[16], bool ct2, uint8_t link_key[16]);
#endif
