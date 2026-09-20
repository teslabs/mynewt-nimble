/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */
#include "host/ble_hs_classic.h"
#if MYNEWT_VAL(BLE_CLASSIC)
#include "ble_hs_priv.h"
#include "nimble/transport.h"

#define NO_HANDLE 0xffff
static const struct ble_hs_classic_callbacks *callbacks;
static uint16_t acl_handle = NO_HANDLE, sco_handle = NO_HANDLE;
static uint16_t acl_mtu, acl_limit, acl_available, acl_outstanding;
static bool supported, shared_pool;

void
ble_hs_classic_register(const struct ble_hs_classic_callbacks *cb)
{
    callbacks = cb;
}

void
ble_hs_classic_features(uint64_t features)
{
    /* BR/EDR Not Supported, LMP feature bit 37. */
    supported = !(features & (UINT64_C(1) << 37));
}

bool
ble_hs_classic_supported(void)
{
    return supported;
}

uint64_t
ble_hs_classic_event_mask(void)
{
    /* Connection, pairing, synchronous connection and authentication events. */
    return supported ? UINT64_C(0x002f180000e0002c) : 0;
}

uint16_t
ble_hs_classic_acl_mtu(void)
{
    return acl_mtu;
}

int
ble_hs_classic_buffers(uint16_t mtu, uint16_t packets, bool shared)
{
    if (!mtu || !packets) {
        return BLE_HS_ECONTROLLER;
    }
    acl_mtu = mtu;
    acl_limit = acl_available = packets;
    acl_outstanding = 0;
    shared_pool = shared;
    return 0;
}

void
ble_hs_classic_reset(void)
{
    acl_handle = sco_handle = NO_HANDLE;
    acl_mtu = acl_limit = acl_available = acl_outstanding = 0;
    supported = shared_pool = false;
    if (callbacks && callbacks->reset) {
        callbacks->reset();
    }
}

static bool
release_packets(uint16_t count)
{
    if (count > acl_outstanding) {
        ble_hs_sched_reset(BLE_HS_ECONTROLLER);
        return false;
    }
    acl_outstanding -= count;
    if (shared_pool) {
        ble_hs_hci_add_avail_pkts(count);
    } else {
        if (count > acl_limit - acl_available) {
            ble_hs_sched_reset(BLE_HS_ECONTROLLER);
            return false;
        }
        acl_available += count;
    }
    return true;
}

int
ble_hs_classic_acl_tx(struct os_mbuf *om)
{
    uint8_t header[4];
    uint16_t *available;
    int rc;

    if (OS_MBUF_PKTLEN(om) < sizeof(header) ||
        os_mbuf_copydata(om, 0, sizeof(header), header)) {
        return BLE_HS_EINVAL;
    }
    ble_hs_lock();
    if ((get_le16(header) & 0x0fff) != acl_handle ||
        get_le16(header + 2) != OS_MBUF_PKTLEN(om) - 4 ||
        get_le16(header + 2) > acl_mtu) {
        ble_hs_unlock();
        return BLE_HS_EINVAL;
    }
    available = shared_pool ? &ble_hs_hci_avail_pkts : &acl_available;
    if (!*available) {
        ble_hs_unlock();
        return BLE_HS_EAGAIN;
    }
    --*available;
    ++acl_outstanding;
    /* The transport consumes the mbuf even on error. */
    rc = ble_transport_to_ll_acl(om);
    if (rc) {
        release_packets(1);
        ble_hs_sched_reset(BLE_HS_ECONTROLLER);
    }
    ble_hs_unlock();
    return 0;
}

bool
ble_hs_classic_rx_acl(struct os_mbuf *om)
{
    uint8_t header[4];
    if (os_mbuf_copydata(om, 0, sizeof(header), header) ||
        (get_le16(header) & 0x0fff) != acl_handle) {
        return false;
    }
    if (callbacks && callbacks->acl) {
        callbacks->acl(om);
    }
    os_mbuf_free_chain(om);
    return true;
}

bool
ble_hs_classic_event(struct ble_hci_ev *ev)
{
    const uint8_t *p = ev->data;
    unsigned n = ev->length;
    bool consume = false;
    bool notify = false;

    if (!supported) {
        return false;
    }
    switch (ev->opcode) {
    case 0x03: /* Connection Complete. */
        /* Only BD_ADDR is valid when connection creation fails or is canceled. */
        if (n != 11 || (!p[0] && p[9] != 1)) {
            break;
        }
        if (!p[0]) {
            ble_hs_lock();
            bool collision = get_le16(p + 1) > 0x0eff ||
                             get_le16(p + 1) == sco_handle ||
                             ble_hs_conn_find(get_le16(p + 1)) != NULL;
            ble_hs_unlock();
            if (acl_handle != NO_HANDLE || collision) {
                ble_hs_sched_reset(BLE_HS_ECONTROLLER);
                return true;
            }
            acl_handle = get_le16(p + 1);
        }
        consume = notify = true;
        break;
    case 0x05: /* Disconnection Complete. */
        if (n != 4) {
            break;
        }
        if (get_le16(p + 1) == acl_handle) {
            consume = notify = true;
            if (!p[0]) {
                ble_hs_lock();
                release_packets(acl_outstanding);
                acl_handle = NO_HANDLE;
                ble_hs_unlock();
                ble_hs_wakeup_tx();
            }
        } else if (get_le16(p + 1) == sco_handle) {
            consume = notify = true;
            if (!p[0]) {
                sco_handle = NO_HANDLE;
            }
        }
        break;
    case 0x13: /* Mixed LE, BR/EDR and synchronous completions. */
        if (!n || n != 1u + 4u * p[0]) {
            break;
        }
        ble_hs_lock();
        for (unsigned i = 1; i < n; i += 4) {
            if (get_le16(p + i) == acl_handle &&
                !release_packets(get_le16(p + i + 2))) {
                break;
            }
        }
        ble_hs_unlock();
        /* LE entries are still processed by the normal handler. */
        break;
    case 0x2c: /* Synchronous Connection Complete. */
        if (n == 17) {
            if (!p[0]) {
                ble_hs_lock();
                bool collision = get_le16(p + 1) > 0x0eff ||
                                 get_le16(p + 1) == acl_handle ||
                                 ble_hs_conn_find(get_le16(p + 1)) != NULL;
                ble_hs_unlock();
                if (sco_handle != NO_HANDLE || collision) {
                    ble_hs_sched_reset(BLE_HS_ECONTROLLER);
                    return true;
                }
                sco_handle = get_le16(p + 1);
            }
            consume = notify = true;
        }
        break;
    case 0x06: /* Authentication Complete. */
    case 0x08: /* Encryption Change. */
    case 0x30: /* Encryption Key Refresh. */
        consume = n >= 3 && get_le16(p + 1) == acl_handle;
        notify = consume;
        break;
    case 0x04:
    case 0x16:
    case 0x17:
    case 0x18:
    case 0x31:
    case 0x32:
    case 0x33:
    case 0x34:
    case 0x36:
        consume = notify = true;
        break;
    }
    if (notify && callbacks && callbacks->event) {
        callbacks->event(ev);
    }
    return consume;
}
#endif
