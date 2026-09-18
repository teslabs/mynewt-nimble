/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */
#ifndef H_BLE_HS_CLASSIC_
#define H_BLE_HS_CLASSIC_
#include <stdbool.h>
#include <stdint.h>
#include "syscfg/syscfg.h"
#ifndef MYNEWT_VAL_BLE_CLASSIC
#define MYNEWT_VAL_BLE_CLASSIC 0
#endif
struct os_mbuf;
struct ble_hci_ev;

#if MYNEWT_VAL(BLE_CLASSIC)
/* Callbacks run on the host event queue, without the host lock held.
 * The receiver borrows each buffer for the duration of the callback. */
struct ble_hs_classic_callbacks {
    void (*event)(const struct ble_hci_ev *event);
    void (*acl)(const struct os_mbuf *packet);
    void (*reset)(void);
};
void ble_hs_classic_register(const struct ble_hs_classic_callbacks *callbacks);
bool ble_hs_classic_supported(void);
uint16_t ble_hs_classic_acl_mtu(void);
/* Takes ownership only on success. EAGAIN means controller backpressure. */
int ble_hs_classic_acl_tx(struct os_mbuf *packet);

/* Host-internal integration points. */
void ble_hs_classic_features(uint64_t features);
uint64_t ble_hs_classic_event_mask(void);
int ble_hs_classic_buffers(uint16_t mtu, uint16_t packets, bool shared);
void ble_hs_classic_reset(void);
bool ble_hs_classic_event(struct ble_hci_ev *event);
bool ble_hs_classic_rx_acl(struct os_mbuf *packet);
#endif
#endif
