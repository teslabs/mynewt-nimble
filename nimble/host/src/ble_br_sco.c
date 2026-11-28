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

#if MYNEWT_VAL(BLE_BREDR_SCO)

#include <string.h>
#include "os/os.h"
#include "nimble/transport.h"
#include "host/ble_br_sco.h"
#include "host/ble_hs_log.h"
#include "ble_hs_priv.h"
#include "ble_br_priv.h"

/* One synchronous connection at a time */
static const struct ble_br_sco_ops *ble_br_sco_ops;
static uint8_t ble_br_sco_addr[BLE_DEV_ADDR_LEN];
static uint16_t ble_br_sco_handle = BLE_HS_CONN_HANDLE_NONE;
static bool ble_br_sco_pending;

static ble_br_sco_rx_fn *ble_br_sco_rx_cb;
static void *ble_br_sco_rx_cb_arg;

void
ble_br_sco_set_ops(const struct ble_br_sco_ops *ops)
{
    ble_br_sco_ops = ops;
}

void
ble_br_sco_set_rx_cb(ble_br_sco_rx_fn *cb, void *cb_arg)
{
    ble_br_sco_rx_cb_arg = cb_arg;
    ble_br_sco_rx_cb = cb;
}

int
ble_br_sco_tx(uint16_t sco_handle, const uint8_t *data, uint16_t len)
{
    uint8_t pkt[BLE_HCI_SCO_HDR_SZ + BLE_BR_SCO_MAX_DATA_LEN];

    if (len > BLE_BR_SCO_MAX_DATA_LEN) {
        return BLE_HS_EMSGSIZE;
    }

    if (sco_handle != ble_br_sco_handle || sco_handle == BLE_HS_CONN_HANDLE_NONE) {
        return BLE_HS_ENOTCONN;
    }

    put_le16(pkt, sco_handle);
    pkt[2] = len;
    memcpy(&pkt[BLE_HCI_SCO_HDR_SZ], data, len);

    return ble_transport_to_ll_sco(pkt, BLE_HCI_SCO_HDR_SZ + len);
}

int
ble_transport_to_hs_sco_impl(const uint8_t *pkt, uint16_t len)
{
    ble_br_sco_rx_fn *cb = ble_br_sco_rx_cb;
    uint16_t handle_flags;

    if (len < BLE_HCI_SCO_HDR_SZ || len != BLE_HCI_SCO_HDR_SZ + pkt[2]) {
        return BLE_HS_EBADDATA;
    }

    handle_flags = get_le16(pkt);
    if (cb != NULL) {
        cb(BLE_HCI_SCO_HANDLE(handle_flags), &pkt[BLE_HCI_SCO_HDR_SZ], pkt[2],
           BLE_HCI_SCO_STATUS(handle_flags), ble_br_sco_rx_cb_arg);
    }

    return 0;
}

static void
ble_br_sco_reject(const uint8_t *addr)
{
    struct ble_hci_lc_reject_conn_req_cp cmd;

    memcpy(cmd.bd_addr, addr, BLE_DEV_ADDR_LEN);
    cmd.reason = BLE_ERR_CONN_REJ_RESOURCES;

    ble_br_hs_cmd(BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_REJECT_SYNCH_CONN_REQ,
                  &cmd, sizeof(cmd), NULL, 0);
}

void
ble_br_sco_rx_conn_request(const struct ble_hci_ev_conn_request *ev)
{
    struct ble_hci_lc_accept_synch_conn_req_cp cmd;
    struct ble_br_sco_params params;
    int rc;

    if (ble_br_sco_ops == NULL || ble_br_sco_pending ||
        ble_br_sco_handle != BLE_HS_CONN_HANDLE_NONE ||
        ble_br_sco_ops->accept(ev->bd_addr, ev->link_type, &params) != 0) {
        ble_br_sco_reject(ev->bd_addr);
        return;
    }

    memcpy(cmd.bd_addr, ev->bd_addr, BLE_DEV_ADDR_LEN);
    cmd.tx_bandwidth = htole32(BLE_HCI_SYNCH_BANDWIDTH_8K);
    cmd.rx_bandwidth = htole32(BLE_HCI_SYNCH_BANDWIDTH_8K);
    cmd.max_latency = htole16(params.max_latency);
    cmd.voice_setting = htole16(BLE_HCI_VOICE_SETTING_PCM16_CVSD);
    cmd.retrans_effort = params.retrans_effort;
    cmd.pkt_type = htole16(params.pkt_type);

    rc = ble_br_hs_cmd(BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_ACCEPT_SYNCH_CONN_REQ,
                       &cmd, sizeof(cmd), NULL, 0);
    if (rc == 0) {
        memcpy(ble_br_sco_addr, ev->bd_addr, BLE_DEV_ADDR_LEN);
        ble_br_sco_pending = true;
    }
}

int
ble_br_sco_connect(uint16_t conn_handle, const struct ble_br_sco_params *params)
{
    struct ble_hci_lc_setup_synch_conn_cp cmd;
    struct ble_br_conn *conn;
    int rc;

    conn = ble_br_conn_find(conn_handle);
    if (conn == NULL) {
        return BLE_HS_ENOTCONN;
    }

    if (ble_br_sco_pending || ble_br_sco_handle != BLE_HS_CONN_HANDLE_NONE) {
        return BLE_HS_EALREADY;
    }

    cmd.conn_handle = htole16(conn_handle);
    cmd.tx_bandwidth = htole32(BLE_HCI_SYNCH_BANDWIDTH_8K);
    cmd.rx_bandwidth = htole32(BLE_HCI_SYNCH_BANDWIDTH_8K);
    cmd.max_latency = htole16(params->max_latency);
    cmd.voice_setting = htole16(BLE_HCI_VOICE_SETTING_PCM16_CVSD);
    cmd.retrans_effort = params->retrans_effort;
    cmd.pkt_type = htole16(params->pkt_type);

    rc = ble_br_hs_cmd(BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_SETUP_SYNCH_CONN,
                       &cmd, sizeof(cmd), NULL, 0);
    if (rc == 0) {
        memcpy(ble_br_sco_addr, conn->addr, BLE_DEV_ADDR_LEN);
        ble_br_sco_pending = true;
    }

    return rc;
}

int
ble_br_sco_disconnect(uint16_t sco_handle)
{
    struct ble_hci_lc_disconnect_cp cmd;

    if (sco_handle != ble_br_sco_handle || sco_handle == BLE_HS_CONN_HANDLE_NONE) {
        return BLE_HS_ENOTCONN;
    }

    cmd.conn_handle = htole16(sco_handle);
    cmd.reason = BLE_ERR_REM_USER_CONN_TERM;

    return ble_br_hs_cmd(BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_DISCONNECT_CMD,
                         &cmd, sizeof(cmd), NULL, 0);
}

static void
ble_br_sco_rx_conn_cmp(const struct ble_hci_ev_synch_conn_comp *ev)
{
    if (!ble_br_sco_pending ||
        memcmp(ev->bd_addr, ble_br_sco_addr, BLE_DEV_ADDR_LEN) != 0) {
        return;
    }

    ble_br_sco_pending = false;

    BLE_HS_LOG(DEBUG,
               "sco: status=%u link_type=%u interval=%u retrans_window=%u "
               "rx_len=%u tx_len=%u air_mode=%u\n",
               ev->status, ev->link_type, ev->tx_interval, ev->retrans_window,
               le16toh(ev->rx_pkt_len), le16toh(ev->tx_pkt_len), ev->air_mode);

    if (ev->status != 0) {
        ble_br_sco_ops->connected(ev->bd_addr, BLE_HS_HCI_ERR(ev->status),
                                  BLE_HS_CONN_HANDLE_NONE);
        return;
    }

    ble_br_sco_handle = le16toh(ev->conn_handle);
    ble_br_sco_ops->connected(ev->bd_addr, 0, ble_br_sco_handle);
}

int
ble_br_sco_rx_evt(uint8_t code, const void *data, unsigned int len)
{
    switch (code) {
    case BLE_HCI_EVCODE_SYNCH_CONN_COMP:
        if (len != sizeof(struct ble_hci_ev_synch_conn_comp)) {
            return BLE_HS_ECONTROLLER;
        }
        ble_br_sco_rx_conn_cmp(data);
        return 0;

    case BLE_HCI_EVCODE_SYNCH_CONN_CHG:
        return 0;

    default:
        return BLE_HS_ENOTSUP;
    }
}

int
ble_br_sco_rx_disconn_cmp(uint16_t handle, uint8_t reason)
{
    if (handle != ble_br_sco_handle || handle == BLE_HS_CONN_HANDLE_NONE) {
        return BLE_HS_ENOENT;
    }

    ble_br_sco_handle = BLE_HS_CONN_HANDLE_NONE;
    ble_br_sco_ops->disconnected(handle, BLE_HS_HCI_ERR(reason));

    return 0;
}

void
ble_br_sco_conn_broken(const uint8_t *addr)
{
    uint16_t handle = ble_br_sco_handle;

    if (memcmp(addr, ble_br_sco_addr, BLE_DEV_ADDR_LEN) != 0) {
        return;
    }

    ble_br_sco_pending = false;

    if (handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_br_sco_handle = BLE_HS_CONN_HANDLE_NONE;
        ble_br_sco_ops->disconnected(handle, BLE_HS_ENOTCONN);
    }
}

void
ble_br_sco_reset(void)
{
    ble_br_sco_pending = false;
    ble_br_sco_handle = BLE_HS_CONN_HANDLE_NONE;
}

#endif
