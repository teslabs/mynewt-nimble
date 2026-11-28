/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "syscfg/syscfg.h"

#if MYNEWT_VAL(BLE_BREDR)

#include <string.h>
#include "os/os.h"
#include "mem/mem.h"
#include "nimble/transport.h"
#include "ble_hs_priv.h"
#include "ble_br_priv.h"

static struct ble_br_conn ble_br_conns[MYNEWT_VAL(BLE_BREDR_MAX_CONNECTIONS)];

static bool ble_br_hs_supported;

/* ACL buffers of the controller; LE takes from the same ones if shared */
static uint16_t ble_br_hs_acl_mtu;
static uint16_t ble_br_hs_acl_avail;
static bool ble_br_hs_acl_shared;

static struct ble_npl_mutex ble_br_hs_call_mutex;
static struct ble_npl_sem ble_br_hs_call_sem;
static struct ble_npl_event ble_br_hs_call_ev;
static ble_br_hs_call_fn *ble_br_hs_call_cb;
static void *ble_br_hs_call_arg;
static int ble_br_hs_call_rc;

static void
ble_br_hs_call_event(struct ble_npl_event *ev)
{
    ble_br_hs_call_rc = ble_br_hs_call_cb(ble_br_hs_call_arg);
    ble_npl_sem_release(&ble_br_hs_call_sem);
}

int
ble_br_hs_call(ble_br_hs_call_fn *fn, void *arg)
{
    int rc;

    /* Nothing runs yet while the application sets the host up */
    if (!ble_hs_is_enabled() || ble_hs_is_parent_task()) {
        return fn(arg);
    }

    ble_npl_mutex_pend(&ble_br_hs_call_mutex, BLE_NPL_TIME_FOREVER);

    ble_br_hs_call_cb = fn;
    ble_br_hs_call_arg = arg;
    ble_npl_eventq_put(ble_hs_evq_get(), &ble_br_hs_call_ev);
    ble_npl_sem_pend(&ble_br_hs_call_sem, BLE_NPL_TIME_FOREVER);
    rc = ble_br_hs_call_rc;

    ble_npl_mutex_release(&ble_br_hs_call_mutex);

    return rc;
}

int
ble_br_hs_cmd(uint8_t ogf, uint16_t ocf, const void *cmd, uint8_t cmd_len,
              void *rsp, uint8_t rsp_len)
{
    return ble_hs_hci_cmd_tx(BLE_HCI_OP(ogf, ocf), cmd, cmd_len, rsp, rsp_len);
}

ble_npl_time_t
ble_br_hs_deadline(uint32_t ms)
{
    /* The timer asks each module again for what it waits for */
    ble_hs_timer_resched();

    return ble_npl_time_get() + ble_npl_time_ms_to_ticks32(ms);
}

bool
ble_br_hs_expired(ble_npl_time_t exp, ble_npl_time_t now, int32_t *ticks)
{
    int32_t left;

    left = (ble_npl_stime_t)(exp - now);
    if (left <= 0) {
        return true;
    }

    if (left < *ticks) {
        *ticks = left;
    }

    return false;
}

int32_t
ble_br_hs_timer(void)
{
    ble_npl_time_t now;
    int32_t ticks;

    now = ble_npl_time_get();
    ticks = BLE_HS_FOREVER;

    ble_br_l2cap_timer(now, &ticks);
    ble_br_sdp_timer(now, &ticks);
    ble_br_rfcomm_timer(now, &ticks);
#if MYNEWT_VAL(BLE_BREDR_HFP_HF)
    ble_br_hfp_hf_timer(now, &ticks);
#endif

    return ticks;
}

struct ble_br_conn *
ble_br_conn_alloc(const uint8_t *addr)
{
    struct ble_br_conn *conn;
    int i;

    for (i = 0; i < MYNEWT_VAL(BLE_BREDR_MAX_CONNECTIONS); i++) {
        conn = &ble_br_conns[i];
        if (!conn->in_use) {
            memset(conn, 0, sizeof(*conn));
            conn->in_use = true;
            conn->handle = BLE_HS_CONN_HANDLE_NONE;
            memcpy(conn->addr, addr, BLE_DEV_ADDR_LEN);
            STAILQ_INIT(&conn->txq);
            return conn;
        }
    }

    return NULL;
}

static void
ble_br_hs_acl_give(uint16_t num_pkts)
{
    if (num_pkts == 0) {
        return;
    }

    if (ble_br_hs_acl_shared) {
        ble_hs_lock();
        ble_hs_hci_add_avail_pkts(num_pkts);
        ble_hs_unlock();
        ble_hs_wakeup_tx();
    } else {
        ble_br_hs_acl_avail += num_pkts;
    }
}

static bool
ble_br_hs_acl_take(void)
{
    bool taken;

    if (ble_br_hs_acl_shared) {
        ble_hs_lock();
        taken = ble_hs_hci_avail_pkts > 0;
        if (taken) {
            ble_hs_hci_avail_pkts--;
        }
        ble_hs_unlock();
    } else {
        taken = ble_br_hs_acl_avail > 0;
        if (taken) {
            ble_br_hs_acl_avail--;
        }
    }

    return taken;
}

void
ble_br_conn_free(struct ble_br_conn *conn)
{
    struct os_mbuf_pkthdr *omp;

    while ((omp = STAILQ_FIRST(&conn->txq)) != NULL) {
        STAILQ_REMOVE_HEAD(&conn->txq, omp_next);
        os_mbuf_free_chain(OS_MBUF_PKTHDR_TO_MBUF(omp));
    }

    os_mbuf_free_chain(conn->rx_om);

    /* The controller drops what it holds for a connection that ended */
    ble_br_hs_acl_give(conn->outstanding_pkts);

    conn->in_use = false;
}

struct ble_br_conn *
ble_br_conn_find(uint16_t handle)
{
    struct ble_br_conn *conn;
    int i;

    for (i = 0; i < MYNEWT_VAL(BLE_BREDR_MAX_CONNECTIONS); i++) {
        conn = &ble_br_conns[i];
        if (conn->in_use && (conn->flags & BLE_BR_CONN_F_CONNECTED) &&
            conn->handle == handle) {
            return conn;
        }
    }

    return NULL;
}

struct ble_br_conn *
ble_br_conn_find_by_addr(const uint8_t *addr)
{
    struct ble_br_conn *conn;
    int i;

    for (i = 0; i < MYNEWT_VAL(BLE_BREDR_MAX_CONNECTIONS); i++) {
        conn = &ble_br_conns[i];
        if (conn->in_use && memcmp(conn->addr, addr, BLE_DEV_ADDR_LEN) == 0) {
            return conn;
        }
    }

    return NULL;
}

static struct os_mbuf *
ble_br_hs_frag_alloc(uint16_t frag_size, void *arg)
{
    return ble_hs_mbuf_acl_pkt();
}

static void
ble_br_hs_tx_kick(struct ble_br_conn *conn)
{
    struct os_mbuf_pkthdr *omp;
    struct hci_data_hdr hdr;
    struct os_mbuf *frag;
    struct os_mbuf *om;
    uint8_t pb;

    while ((omp = STAILQ_FIRST(&conn->txq)) != NULL) {
        if (!ble_br_hs_acl_take()) {
            break;
        }

        STAILQ_REMOVE_HEAD(&conn->txq, omp_next);
        om = OS_MBUF_PKTHDR_TO_MBUF(omp);

        pb = (conn->flags & BLE_BR_CONN_F_TX_CONT) ? BLE_HCI_PB_MIDDLE
                                                   : BLE_HCI_PB_FIRST_FLUSH;

        frag = mem_split_frag(&om, ble_br_hs_acl_mtu, ble_br_hs_frag_alloc, NULL);
        if (frag != NULL) {
            hdr.hdh_handle_pb_bc =
                htole16(ble_hs_hci_util_handle_pb_bc_join(conn->handle, pb, 0));
            hdr.hdh_len = htole16(OS_MBUF_PKTLEN(frag));
            frag = os_mbuf_prepend(frag, sizeof(hdr));
        }
        if (frag == NULL) {
            /* Out of buffers: the PDU is lost */
            os_mbuf_free_chain(om);
            conn->flags &= ~BLE_BR_CONN_F_TX_CONT;
            ble_br_hs_acl_give(1);
            continue;
        }
        memcpy(frag->om_data, &hdr, sizeof(hdr));

        if (om != NULL) {
            STAILQ_INSERT_HEAD(&conn->txq, OS_MBUF_PKTHDR(om), omp_next);
            conn->flags |= BLE_BR_CONN_F_TX_CONT;
        } else {
            conn->flags &= ~BLE_BR_CONN_F_TX_CONT;
        }

        conn->outstanding_pkts++;
        if (ble_transport_to_ll_acl(frag) != 0) {
            conn->outstanding_pkts--;
            ble_br_hs_acl_give(1);
        }
    }
}

int
ble_br_hs_acl_tx(struct ble_br_conn *conn, struct os_mbuf *om)
{
    if (!(conn->flags & BLE_BR_CONN_F_CONNECTED)) {
        os_mbuf_free_chain(om);
        return BLE_HS_ENOTCONN;
    }

    STAILQ_INSERT_TAIL(&conn->txq, OS_MBUF_PKTHDR(om), omp_next);
    ble_br_hs_tx_kick(conn);

    return 0;
}

void
ble_br_hs_rx_num_completed_pkts(uint16_t handle, uint16_t num_pkts)
{
    struct ble_br_conn *conn;

    conn = ble_br_conn_find(handle);
    if (conn == NULL) {
        return;
    }

    if (num_pkts > conn->outstanding_pkts) {
        ble_hs_sched_reset(BLE_HS_ECONTROLLER);
        return;
    }

    conn->outstanding_pkts -= num_pkts;
    ble_br_hs_acl_give(num_pkts);
    ble_br_hs_tx_kick(conn);
}

int
ble_br_hs_rx_acl(uint16_t handle, uint8_t pb, struct os_mbuf *om)
{
    struct ble_br_conn *conn;

    conn = ble_br_conn_find(handle);
    if (conn == NULL) {
        return BLE_HS_ENOTCONN;
    }

    ble_br_l2cap_rx(conn, pb, om);

    return 0;
}

int
ble_br_hs_rx_evt(const struct ble_hci_ev *ev)
{
    int rc;

    if (!ble_br_hs_supported) {
        return BLE_HS_ENOTSUP;
    }

    rc = ble_br_gap_rx_evt(ev->opcode, ev->data, ev->length);
#if MYNEWT_VAL(BLE_BREDR_SCO)
    if (rc == BLE_HS_ENOTSUP) {
        rc = ble_br_sco_rx_evt(ev->opcode, ev->data, ev->length);
    }
#endif

    return rc;
}

int
ble_br_hs_rx_disconn_cmp(const struct ble_hci_ev_disconn_cmp *ev)
{
    struct ble_br_conn *conn;
    uint16_t handle;

    handle = le16toh(ev->conn_handle);

    conn = ble_br_conn_find(handle);
    if (conn != NULL) {
        if (ev->status == 0) {
            ble_br_gap_rx_disconn_cmp(conn, ev->reason);
        }
        return 0;
    }

#if MYNEWT_VAL(BLE_BREDR_SCO)
    if (ev->status == 0) {
        return ble_br_sco_rx_disconn_cmp(handle, ev->reason);
    }
#endif

    return BLE_HS_ENOENT;
}

int
ble_br_hs_rx_enc_change(const struct ble_hci_ev_enrypt_chg *ev)
{
    struct ble_br_conn *conn;

    conn = ble_br_conn_find(le16toh(ev->connection_handle));
    if (conn == NULL) {
        return BLE_HS_ENOENT;
    }

    ble_br_gap_rx_enc_change(conn, ev->status, ev->enabled != 0);

    return 0;
}

void
ble_br_hs_set_features(uint64_t features)
{
    ble_br_hs_supported = !(features & BLE_HCI_LMP_FEAT_BREDR_NOT_SUPP);
}

uint64_t
ble_br_hs_event_mask(void)
{
    if (!ble_br_hs_supported) {
        return 0;
    }

    return BLE_HCI_EVMASK_CONN_DONE | BLE_HCI_EVMASK_CONN_REQUEST |
           BLE_HCI_EVMASK_AUTH_CMP | BLE_HCI_EVMASK_MODE_CHANGE |
           BLE_HCI_EVMASK_PIN_CODE_REQ | BLE_HCI_EVMASK_LINK_KEY_REQ |
           BLE_HCI_EVMASK_LINK_KEY_NOTIFY | BLE_HCI_EVMASK_SYNCH_CONN_COMP |
           BLE_HCI_EVMASK_SYNCH_CONN_CHG | BLE_HCI_EVMASK_IO_CAP_REQ |
           BLE_HCI_EVMASK_IO_CAP_RSP | BLE_HCI_EVMASK_USER_CONFIRM_REQ |
           BLE_HCI_EVMASK_PASSKEY_REQ | BLE_HCI_EVMASK_SIMPLE_PAIR_COMP;
}

int
ble_br_hs_set_buf_sz(uint16_t pktlen, uint16_t max_pkts, bool shared)
{
    if (pktlen == 0 || max_pkts == 0) {
        return BLE_HS_ECONTROLLER;
    }

    ble_br_hs_acl_mtu = pktlen;
    ble_br_hs_acl_avail = max_pkts;
    ble_br_hs_acl_shared = shared;

    return 0;
}

bool
ble_br_gap_supported(void)
{
    return ble_br_hs_supported;
}

int
ble_br_hs_startup(void)
{
    if (!ble_br_hs_supported) {
        return 0;
    }

    return ble_br_gap_startup();
}

void
ble_br_hs_reset(void)
{
    struct ble_br_conn *conn;
    int i;

    for (i = 0; i < MYNEWT_VAL(BLE_BREDR_MAX_CONNECTIONS); i++) {
        conn = &ble_br_conns[i];
        if (!conn->in_use) {
            continue;
        }

        /* The controller is being reset: nothing is owed anymore */
        conn->outstanding_pkts = 0;
        if (conn->flags & BLE_BR_CONN_F_CONNECTED) {
            ble_br_gap_rx_disconn_cmp(conn, BLE_ERR_HW_FAIL);
        } else {
            ble_br_conn_free(conn);
        }
    }

#if MYNEWT_VAL(BLE_BREDR_SCO)
    ble_br_sco_reset();
#endif
    ble_br_gap_reset();

    ble_br_hs_supported = false;
    ble_br_hs_acl_avail = 0;
}

void
ble_br_hs_init(void)
{
    ble_npl_mutex_init(&ble_br_hs_call_mutex);
    ble_npl_sem_init(&ble_br_hs_call_sem, 0);
    ble_npl_event_init(&ble_br_hs_call_ev, ble_br_hs_call_event, NULL);

    memset(ble_br_conns, 0, sizeof(ble_br_conns));
    ble_br_hs_supported = false;
    ble_br_hs_acl_avail = 0;

    ble_br_gap_init();
#if MYNEWT_VAL(BLE_BREDR_SCO)
    ble_br_sco_set_ops(NULL);
    ble_br_sco_reset();
#endif
    ble_br_l2cap_init();
    ble_br_sdp_init();
    ble_br_rfcomm_init();
#if MYNEWT_VAL(BLE_BREDR_HFP_HF)
    ble_br_hfp_hf_reset();
#endif
}

#endif
