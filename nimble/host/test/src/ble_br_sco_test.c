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

#include <string.h>
#include "testutil/testutil.h"
#include "nimble/hci_common.h"
#include "nimble/transport.h"
#include "host/ble_br_sco.h"
#include "ble_hs_test.h"
#include "ble_hs_test_util.h"
#include "ble_br_test_util.h"

#if MYNEWT_VAL(BLE_BREDR_SCO)

#define BLE_BR_SCO_TEST_HANDLE 0x0101

static const uint8_t ble_br_sco_test_addr[BLE_DEV_ADDR_LEN] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
};

static const uint8_t ble_br_sco_test_addr2[BLE_DEV_ADDR_LEN] = {
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
};

static const struct ble_br_sco_params ble_br_sco_test_params = {
    .max_latency = 12,
    .retrans_effort = BLE_HCI_SYNCH_RETRANS_QUALITY,
    .pkt_type = BLE_HCI_SYNCH_PKT_NO_3EV3 | BLE_HCI_SYNCH_PKT_NO_2EV5 |
                BLE_HCI_SYNCH_PKT_NO_3EV5,
};

static int ble_br_sco_test_accept_cnt;
static int ble_br_sco_test_accept_rc;
static uint8_t ble_br_sco_test_accept_link_type;
static int ble_br_sco_test_connected_cnt;
static int ble_br_sco_test_connected_status;
static uint16_t ble_br_sco_test_connected_handle;
static int ble_br_sco_test_disconnected_cnt;
static uint16_t ble_br_sco_test_disconnected_handle;
static int ble_br_sco_test_disconnected_reason;
static int ble_br_sco_test_rx_cnt;
static uint16_t ble_br_sco_test_rx_handle;
static uint8_t ble_br_sco_test_rx_buf[BLE_BR_SCO_MAX_DATA_LEN];
static uint16_t ble_br_sco_test_rx_len;
static uint8_t ble_br_sco_test_rx_status;
static int ble_br_sco_test_cb_arg;

static int
ble_br_sco_test_accept(const uint8_t *addr, uint8_t link_type,
                       struct ble_br_sco_params *params)
{
    TEST_ASSERT(memcmp(addr, ble_br_sco_test_addr, BLE_DEV_ADDR_LEN) == 0);

    ble_br_sco_test_accept_cnt++;
    ble_br_sco_test_accept_link_type = link_type;
    *params = ble_br_sco_test_params;

    return ble_br_sco_test_accept_rc;
}

static void
ble_br_sco_test_connected(const uint8_t *addr, int status, uint16_t sco_handle)
{
    TEST_ASSERT(memcmp(addr, ble_br_sco_test_addr, BLE_DEV_ADDR_LEN) == 0);

    ble_br_sco_test_connected_cnt++;
    ble_br_sco_test_connected_status = status;
    ble_br_sco_test_connected_handle = sco_handle;
}

static void
ble_br_sco_test_disconnected(uint16_t sco_handle, int reason)
{
    ble_br_sco_test_disconnected_cnt++;
    ble_br_sco_test_disconnected_handle = sco_handle;
    ble_br_sco_test_disconnected_reason = reason;
}

static const struct ble_br_sco_ops ble_br_sco_test_ops = {
    .accept = ble_br_sco_test_accept,
    .connected = ble_br_sco_test_connected,
    .disconnected = ble_br_sco_test_disconnected,
};

static void
ble_br_sco_test_rx(uint16_t sco_handle, const uint8_t *data, uint16_t len,
                   uint8_t status, void *arg)
{
    TEST_ASSERT(arg == &ble_br_sco_test_cb_arg);
    TEST_ASSERT_FATAL(len <= sizeof ble_br_sco_test_rx_buf);

    ble_br_sco_test_rx_cnt++;
    ble_br_sco_test_rx_handle = sco_handle;
    memcpy(ble_br_sco_test_rx_buf, data, len);
    ble_br_sco_test_rx_len = len;
    ble_br_sco_test_rx_status = status;
}

static void
ble_br_sco_test_util_init(void)
{
    ble_br_test_util_init();

    ble_br_sco_test_accept_cnt = 0;
    ble_br_sco_test_accept_rc = 0;
    ble_br_sco_test_connected_cnt = 0;
    ble_br_sco_test_connected_status = -1;
    ble_br_sco_test_disconnected_cnt = 0;
    ble_br_sco_test_rx_cnt = 0;

    ble_br_sco_set_ops(&ble_br_sco_test_ops);
    ble_br_sco_set_rx_cb(NULL, NULL);

    ble_br_test_util_create_conn(2, ble_br_sco_test_addr, 1);
}

static void
ble_br_sco_test_util_verify_tx_reject(const uint8_t *addr)
{
    uint8_t param_len;
    uint8_t *param;

    param = ble_hs_test_util_hci_verify_tx(
        BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_REJECT_SYNCH_CONN_REQ, &param_len);
    TEST_ASSERT(param_len == sizeof(struct ble_hci_lc_reject_conn_req_cp));
    TEST_ASSERT(memcmp(param, addr, BLE_DEV_ADDR_LEN) == 0);
    TEST_ASSERT(param[6] == BLE_ERR_CONN_REJ_RESOURCES);
}

/**
 * Has the peer set up a synchronous connection, which the host accepts.
 */
static void
ble_br_sco_test_util_rx_connect(void)
{
    ble_br_test_util_ack_set(BLE_HCI_OCF_ACCEPT_SYNCH_CONN_REQ, 0);
    ble_br_test_util_rx_conn_request(ble_br_sco_test_addr, BLE_HCI_LINK_TYPE_ESCO);
    ble_br_test_util_verify_tx_accept_synch_conn(ble_br_sco_test_addr,
                                                 &ble_br_sco_test_params);

    ble_br_test_util_rx_synch_conn_cmp(
        0, BLE_BR_SCO_TEST_HANDLE, ble_br_sco_test_addr, BLE_HCI_LINK_TYPE_ESCO);
}

TEST_CASE_SELF(ble_br_sco_test_incoming)
{
    uint8_t conn_chg[5];

    ble_br_sco_test_util_init();

    /*** The request is accepted, with the settings of the upper layer. */
    ble_br_test_util_ack_set(BLE_HCI_OCF_ACCEPT_SYNCH_CONN_REQ, 0);
    ble_br_test_util_rx_conn_request(ble_br_sco_test_addr, BLE_HCI_LINK_TYPE_ESCO);

    TEST_ASSERT(ble_br_sco_test_accept_cnt == 1);
    TEST_ASSERT(ble_br_sco_test_accept_link_type == BLE_HCI_LINK_TYPE_ESCO);
    ble_br_test_util_verify_tx_accept_synch_conn(ble_br_sco_test_addr,
                                                 &ble_br_sco_test_params);
    TEST_ASSERT(ble_br_sco_test_connected_cnt == 0);

    /* One connection at a time. */
    ble_br_test_util_ack_set(BLE_HCI_OCF_REJECT_SYNCH_CONN_REQ, 0);
    ble_br_test_util_rx_conn_request(ble_br_sco_test_addr, BLE_HCI_LINK_TYPE_ESCO);
    ble_br_sco_test_util_verify_tx_reject(ble_br_sco_test_addr);
    TEST_ASSERT(ble_br_sco_test_accept_cnt == 1);

    /* The completion of a connection nothing asked for is ignored. */
    ble_br_test_util_rx_synch_conn_cmp(0, BLE_BR_SCO_TEST_HANDLE + 1,
                                       ble_br_sco_test_addr2, BLE_HCI_LINK_TYPE_ESCO);
    TEST_ASSERT(ble_br_sco_test_connected_cnt == 0);

    ble_br_test_util_rx_synch_conn_cmp(
        0, BLE_BR_SCO_TEST_HANDLE, ble_br_sco_test_addr, BLE_HCI_LINK_TYPE_ESCO);
    TEST_ASSERT(ble_br_sco_test_connected_cnt == 1);
    TEST_ASSERT(ble_br_sco_test_connected_status == 0);
    TEST_ASSERT(ble_br_sco_test_connected_handle == BLE_BR_SCO_TEST_HANDLE);

    ble_br_test_util_ack_set(BLE_HCI_OCF_REJECT_SYNCH_CONN_REQ, 0);
    ble_br_test_util_rx_conn_request(ble_br_sco_test_addr, BLE_HCI_LINK_TYPE_ESCO);
    ble_br_sco_test_util_verify_tx_reject(ble_br_sco_test_addr);

    /* A change of the connection needs nothing. */
    memset(conn_chg, 0, sizeof conn_chg);
    ble_br_test_util_rx_evt(BLE_HCI_EVCODE_SYNCH_CONN_CHG, conn_chg, sizeof conn_chg);
    TEST_ASSERT(ble_hs_test_util_hci_out_first() == NULL);

    /*** The peer disconnects. */
    ble_hs_test_util_hci_rx_disconn_complete_event(BLE_BR_SCO_TEST_HANDLE, 0,
                                                   BLE_ERR_REM_USER_CONN_TERM);
    TEST_ASSERT(ble_br_sco_test_disconnected_cnt == 1);
    TEST_ASSERT(ble_br_sco_test_disconnected_handle == BLE_BR_SCO_TEST_HANDLE);
    TEST_ASSERT(ble_br_sco_test_disconnected_reason ==
                BLE_HS_HCI_ERR(BLE_ERR_REM_USER_CONN_TERM));

    /* The ACL connection is still there. */
    TEST_ASSERT(ble_br_test_util_gap_event_cnt() == 0);

    /*** The connection fails to get set up. */
    ble_br_test_util_ack_set(BLE_HCI_OCF_ACCEPT_SYNCH_CONN_REQ, 0);
    ble_br_test_util_rx_conn_request(ble_br_sco_test_addr, BLE_HCI_LINK_TYPE_SCO);
    TEST_ASSERT(ble_br_sco_test_accept_cnt == 2);
    TEST_ASSERT(ble_br_sco_test_accept_link_type == BLE_HCI_LINK_TYPE_SCO);
    ble_br_test_util_verify_tx_accept_synch_conn(ble_br_sco_test_addr,
                                                 &ble_br_sco_test_params);

    ble_br_test_util_rx_synch_conn_cmp(BLE_ERR_CONN_ACCEPT_TMO, 0,
                                       ble_br_sco_test_addr, BLE_HCI_LINK_TYPE_SCO);
    TEST_ASSERT(ble_br_sco_test_connected_cnt == 2);
    TEST_ASSERT(ble_br_sco_test_connected_status ==
                BLE_HS_HCI_ERR(BLE_ERR_CONN_ACCEPT_TMO));
    TEST_ASSERT(ble_br_sco_test_connected_handle == BLE_HS_CONN_HANDLE_NONE);

    /*** The ACL connection is lost with the synchronous one. */
    ble_br_sco_test_util_rx_connect();
    TEST_ASSERT(ble_br_sco_test_connected_cnt == 3);

    ble_hs_test_util_hci_rx_disconn_complete_event(2, 0, BLE_ERR_CONN_SPVN_TMO);
    TEST_ASSERT(ble_br_sco_test_disconnected_cnt == 2);
    TEST_ASSERT(ble_br_sco_test_disconnected_handle == BLE_BR_SCO_TEST_HANDLE);
    TEST_ASSERT(ble_br_sco_test_disconnected_reason == BLE_HS_ENOTCONN);

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_sco_test_reject)
{
    ble_br_sco_test_util_init();

    /*** The upper layer does not take the connection. */
    ble_br_sco_test_accept_rc = BLE_HS_EREJECT;

    ble_br_test_util_ack_set(BLE_HCI_OCF_REJECT_SYNCH_CONN_REQ, 0);
    ble_br_test_util_rx_conn_request(ble_br_sco_test_addr, BLE_HCI_LINK_TYPE_ESCO);
    TEST_ASSERT(ble_br_sco_test_accept_cnt == 1);
    ble_br_sco_test_util_verify_tx_reject(ble_br_sco_test_addr);

    /*** No upper layer. */
    ble_br_sco_set_ops(NULL);

    ble_br_test_util_ack_set(BLE_HCI_OCF_REJECT_SYNCH_CONN_REQ, 0);
    ble_br_test_util_rx_conn_request(ble_br_sco_test_addr, BLE_HCI_LINK_TYPE_SCO);
    TEST_ASSERT(ble_br_sco_test_accept_cnt == 1);
    ble_br_sco_test_util_verify_tx_reject(ble_br_sco_test_addr);

    TEST_ASSERT(ble_br_sco_test_connected_cnt == 0);
    TEST_ASSERT(ble_hs_test_util_hci_out_first() == NULL);

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_sco_test_outgoing)
{
    uint8_t param_len;
    uint8_t *param;
    int rc;

    ble_br_sco_test_util_init();

    rc = ble_br_sco_connect(3, &ble_br_sco_test_params);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);
    rc = ble_br_sco_disconnect(BLE_BR_SCO_TEST_HANDLE);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);
    rc = ble_br_sco_disconnect(BLE_HS_CONN_HANDLE_NONE);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);
    TEST_ASSERT(ble_hs_test_util_hci_out_first() == NULL);

    /*** We set the connection up. */
    ble_br_test_util_ack_set(BLE_HCI_OCF_SETUP_SYNCH_CONN, 0);
    rc = ble_br_sco_connect(2, &ble_br_sco_test_params);
    TEST_ASSERT_FATAL(rc == 0);

    param = ble_hs_test_util_hci_verify_tx(
        BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_SETUP_SYNCH_CONN, &param_len);
    TEST_ASSERT(param_len == sizeof(struct ble_hci_lc_setup_synch_conn_cp));
    TEST_ASSERT(get_le16(param + 0) == 2);
    TEST_ASSERT(get_le32(param + 2) == BLE_HCI_SYNCH_BANDWIDTH_8K);
    TEST_ASSERT(get_le32(param + 6) == BLE_HCI_SYNCH_BANDWIDTH_8K);
    TEST_ASSERT(get_le16(param + 10) == ble_br_sco_test_params.max_latency);
    TEST_ASSERT(get_le16(param + 12) == BLE_HCI_VOICE_SETTING_PCM16_CVSD);
    TEST_ASSERT(param[14] == ble_br_sco_test_params.retrans_effort);
    TEST_ASSERT(get_le16(param + 15) == ble_br_sco_test_params.pkt_type);

    rc = ble_br_sco_connect(2, &ble_br_sco_test_params);
    TEST_ASSERT(rc == BLE_HS_EALREADY);

    ble_br_test_util_rx_synch_conn_cmp(
        0, BLE_BR_SCO_TEST_HANDLE, ble_br_sco_test_addr, BLE_HCI_LINK_TYPE_ESCO);
    TEST_ASSERT(ble_br_sco_test_connected_cnt == 1);
    TEST_ASSERT(ble_br_sco_test_connected_status == 0);
    TEST_ASSERT(ble_br_sco_test_connected_handle == BLE_BR_SCO_TEST_HANDLE);
    TEST_ASSERT(ble_br_sco_test_accept_cnt == 0);

    rc = ble_br_sco_connect(2, &ble_br_sco_test_params);
    TEST_ASSERT(rc == BLE_HS_EALREADY);

    /*** We disconnect. */
    rc = ble_br_sco_disconnect(BLE_BR_SCO_TEST_HANDLE + 1);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);

    ble_hs_test_util_hci_ack_set_disconnect(0);
    rc = ble_br_sco_disconnect(BLE_BR_SCO_TEST_HANDLE);
    TEST_ASSERT(rc == 0);
    ble_hs_test_util_hci_verify_tx_disconnect(BLE_BR_SCO_TEST_HANDLE,
                                              BLE_ERR_REM_USER_CONN_TERM);

    ble_hs_test_util_hci_rx_disconn_complete_event(BLE_BR_SCO_TEST_HANDLE, 0,
                                                   BLE_ERR_CONN_TERM_LOCAL);
    TEST_ASSERT(ble_br_sco_test_disconnected_cnt == 1);
    TEST_ASSERT(ble_br_sco_test_disconnected_handle == BLE_BR_SCO_TEST_HANDLE);
    TEST_ASSERT(ble_br_sco_test_disconnected_reason ==
                BLE_HS_HCI_ERR(BLE_ERR_CONN_TERM_LOCAL));

    /*** The controller refuses to set a connection up. */
    ble_br_test_util_ack_set(BLE_HCI_OCF_SETUP_SYNCH_CONN, BLE_ERR_CMD_DISALLOWED);
    rc = ble_br_sco_connect(2, &ble_br_sco_test_params);
    TEST_ASSERT(rc == BLE_HS_HCI_ERR(BLE_ERR_CMD_DISALLOWED));
    ble_hs_test_util_hci_out_clear();

    /* Another attempt can be made. */
    ble_br_test_util_ack_set(BLE_HCI_OCF_SETUP_SYNCH_CONN, 0);
    rc = ble_br_sco_connect(2, &ble_br_sco_test_params);
    TEST_ASSERT(rc == 0);

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_sco_test_data)
{
    uint8_t pkt[BLE_HCI_SCO_HDR_SZ + BLE_BR_SCO_MAX_DATA_LEN + 1];
    const uint8_t *tx_pkt;
    uint16_t tx_len;
    int rc;
    int i;

    ble_br_sco_test_util_init();

    for (i = 0; i < sizeof pkt; i++) {
        pkt[i] = i;
    }

    /*** Nothing can be sent without a connection. */
    rc = ble_br_sco_tx(BLE_BR_SCO_TEST_HANDLE, pkt, 60);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);
    rc = ble_br_sco_tx(BLE_HS_CONN_HANDLE_NONE, pkt, 60);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);
    TEST_ASSERT(ble_br_test_util_sco_tx_cnt() == 0);

    ble_br_sco_test_util_rx_connect();

    /*** Send. */
    rc = ble_br_sco_tx(BLE_BR_SCO_TEST_HANDLE, pkt, 60);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(ble_br_test_util_sco_tx_cnt() == 1);

    tx_pkt = ble_br_test_util_sco_tx_last(&tx_len);
    TEST_ASSERT_FATAL(tx_len == BLE_HCI_SCO_HDR_SZ + 60);
    TEST_ASSERT(get_le16(tx_pkt) == BLE_BR_SCO_TEST_HANDLE);
    TEST_ASSERT(tx_pkt[2] == 60);
    TEST_ASSERT(memcmp(tx_pkt + BLE_HCI_SCO_HDR_SZ, pkt, 60) == 0);

    rc = ble_br_sco_tx(BLE_BR_SCO_TEST_HANDLE, pkt, BLE_BR_SCO_MAX_DATA_LEN);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(ble_br_test_util_sco_tx_cnt() == 2);

    tx_pkt = ble_br_test_util_sco_tx_last(&tx_len);
    TEST_ASSERT_FATAL(tx_len == BLE_HCI_SCO_HDR_SZ + BLE_BR_SCO_MAX_DATA_LEN);
    TEST_ASSERT(tx_pkt[2] == BLE_BR_SCO_MAX_DATA_LEN);
    TEST_ASSERT(memcmp(tx_pkt + BLE_HCI_SCO_HDR_SZ, pkt, BLE_BR_SCO_MAX_DATA_LEN) == 0);

    rc = ble_br_sco_tx(BLE_BR_SCO_TEST_HANDLE, pkt, BLE_BR_SCO_MAX_DATA_LEN + 1);
    TEST_ASSERT(rc == BLE_HS_EMSGSIZE);
    rc = ble_br_sco_tx(BLE_BR_SCO_TEST_HANDLE + 1, pkt, 60);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);
    TEST_ASSERT(ble_br_test_util_sco_tx_cnt() == 2);

    /*** Receive; nothing takes the data yet. */
    put_le16(pkt, BLE_BR_SCO_TEST_HANDLE);
    pkt[2] = 60;
    rc = ble_transport_to_hs_sco(pkt, BLE_HCI_SCO_HDR_SZ + 60);
    TEST_ASSERT(rc == 0);

    ble_br_sco_set_rx_cb(ble_br_sco_test_rx, &ble_br_sco_test_cb_arg);

    rc = ble_transport_to_hs_sco(pkt, BLE_HCI_SCO_HDR_SZ + 60);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(ble_br_sco_test_rx_cnt == 1);
    TEST_ASSERT(ble_br_sco_test_rx_handle == BLE_BR_SCO_TEST_HANDLE);
    TEST_ASSERT(ble_br_sco_test_rx_len == 60);
    TEST_ASSERT(memcmp(ble_br_sco_test_rx_buf, pkt + BLE_HCI_SCO_HDR_SZ, 60) == 0);
    TEST_ASSERT(ble_br_sco_test_rx_status == BLE_BR_SCO_STATUS_GOOD);

    /* Data the controller marks as partially lost. */
    put_le16(pkt, BLE_BR_SCO_TEST_HANDLE | (BLE_BR_SCO_STATUS_PARTIAL << 12));
    pkt[2] = BLE_BR_SCO_MAX_DATA_LEN;
    rc = ble_transport_to_hs_sco(pkt, sizeof pkt - 1);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(ble_br_sco_test_rx_cnt == 2);
    TEST_ASSERT(ble_br_sco_test_rx_handle == BLE_BR_SCO_TEST_HANDLE);
    TEST_ASSERT(ble_br_sco_test_rx_len == BLE_BR_SCO_MAX_DATA_LEN);
    TEST_ASSERT(ble_br_sco_test_rx_status == BLE_BR_SCO_STATUS_PARTIAL);

    /* An empty packet. */
    pkt[2] = 0;
    rc = ble_transport_to_hs_sco(pkt, BLE_HCI_SCO_HDR_SZ);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(ble_br_sco_test_rx_cnt == 3);
    TEST_ASSERT(ble_br_sco_test_rx_len == 0);

    /*** Packets whose length is not the one they tell are dropped. */
    pkt[2] = 60;
    rc = ble_transport_to_hs_sco(pkt, BLE_HCI_SCO_HDR_SZ + 59);
    TEST_ASSERT(rc == BLE_HS_EBADDATA);
    rc = ble_transport_to_hs_sco(pkt, BLE_HCI_SCO_HDR_SZ + 61);
    TEST_ASSERT(rc == BLE_HS_EBADDATA);
    rc = ble_transport_to_hs_sco(pkt, BLE_HCI_SCO_HDR_SZ - 1);
    TEST_ASSERT(rc == BLE_HS_EBADDATA);
    TEST_ASSERT(ble_br_sco_test_rx_cnt == 3);

    ble_br_sco_set_rx_cb(NULL, NULL);

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

#endif

TEST_SUITE(ble_br_sco_test_suite)
{
#if MYNEWT_VAL(BLE_BREDR_SCO)
    ble_br_sco_test_incoming();
    ble_br_sco_test_reject();
    ble_br_sco_test_outgoing();
    ble_br_sco_test_data();
#endif
}
