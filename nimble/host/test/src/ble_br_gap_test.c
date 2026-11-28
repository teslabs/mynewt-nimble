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
#include "host/ble_br_gap.h"
#include "ble_hs_test.h"
#include "ble_hs_test_util.h"
#include "ble_br_test_util.h"

#if MYNEWT_VAL(BLE_BREDR)

#define BLE_BR_GAP_TEST_CB_OPCODE(ocf)                                        \
    ble_hs_hci_util_opcode_join(BLE_HCI_OGF_CTLR_BASEBAND, (ocf))

#define BLE_BR_GAP_TEST_LP_OPCODE(ocf)                                        \
    ble_hs_hci_util_opcode_join(BLE_HCI_OGF_LINK_POLICY, (ocf))

static const uint8_t ble_br_gap_test_addr[BLE_DEV_ADDR_LEN] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
};

static const uint8_t ble_br_gap_test_addr2[BLE_DEV_ADDR_LEN] = {
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
};

static const uint8_t ble_br_gap_test_key[BLE_BR_GAP_LINK_KEY_LEN] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
};

static void
ble_br_gap_test_util_rx_addr_evt(uint8_t code, const uint8_t *addr)
{
    struct ble_hci_ev_bd_addr ev;

    memcpy(ev.bd_addr, addr, BLE_DEV_ADDR_LEN);
    ble_br_test_util_rx_evt(code, &ev, sizeof ev);
}

static void
ble_br_gap_test_util_rx_link_key_notify(const uint8_t *addr,
                                        const uint8_t *key, uint8_t key_type)
{
    struct ble_hci_ev_link_key_notify ev;

    memcpy(ev.bd_addr, addr, BLE_DEV_ADDR_LEN);
    memcpy(ev.key, key, sizeof ev.key);
    ev.key_type = key_type;

    ble_br_test_util_rx_evt(BLE_HCI_EVCODE_LINK_KEY_NOTIFY, &ev, sizeof ev);
}

static void
ble_br_gap_test_util_rx_auth_cmp(uint16_t conn_handle, uint8_t status)
{
    struct ble_hci_ev_auth_cmp ev;

    ev.status = status;
    ev.conn_handle = htole16(conn_handle);

    ble_br_test_util_rx_evt(BLE_HCI_EVCODE_AUTH_CMP, &ev, sizeof ev);
}

static void
ble_br_gap_test_util_rx_simple_pair_cmp(const uint8_t *addr, uint8_t status)
{
    struct ble_hci_ev_simple_pair_comp ev;

    ev.status = status;
    memcpy(ev.bd_addr, addr, BLE_DEV_ADDR_LEN);

    ble_br_test_util_rx_evt(BLE_HCI_EVCODE_SIMPLE_PAIR_COMP, &ev, sizeof ev);
}

static void
ble_br_gap_test_util_verify_link_key(const uint8_t *addr, const uint8_t *key,
                                     uint8_t key_type)
{
    struct ble_store_value_br_link_key value;
    struct ble_store_key_br_link_key store_key;
    int rc;

    memset(&store_key, 0, sizeof store_key);
    store_key.peer_addr.type = BLE_ADDR_PUBLIC;
    memcpy(store_key.peer_addr.val, addr, BLE_DEV_ADDR_LEN);

    rc = ble_store_read_br_link_key(&store_key, &value);
    if (key == NULL) {
        TEST_ASSERT(rc == BLE_HS_ENOENT);
        return;
    }

    TEST_ASSERT_FATAL(rc == 0);
    TEST_ASSERT(memcmp(value.key, key, sizeof value.key) == 0);
    TEST_ASSERT(value.key_type == key_type);
}

/**
 * Removes commands from the head of the transmit queue until the specified
 * one is found.
 */
static uint8_t *
ble_br_gap_test_util_verify_tx_find(uint8_t ogf, uint16_t ocf, uint8_t *out_param_len)
{
    uint16_t opcode;
    uint8_t *cmd;

    while (1) {
        cmd = ble_hs_test_util_hci_out_first();
        TEST_ASSERT_FATAL(cmd != NULL);

        opcode = get_le16(cmd);
        if (opcode == ble_hs_hci_util_opcode_join(ogf, ocf)) {
            *out_param_len = cmd[2];
            return cmd + 3;
        }
    }
}

TEST_CASE_SELF(ble_br_gap_test_startup)
{
    static const uint8_t exp_eir[] = {
        0x06, BLE_HCI_EIR_TYPE_NAME_COMPLETE, 'w', 'a', 't', 'c', 'h', 0x00,
    };
    uint64_t exp_mask;
    uint8_t param_len;
    uint8_t *param;
    int rc;

    ble_hs_test_util_hci_set_bredr(BLE_BR_TEST_UTIL_ACL_LEN,
                                   BLE_BR_TEST_UTIL_ACL_PKTS, 0);
    ble_hs_test_util_init_no_start();
    ble_hs_test_util_hci_set_bredr(0, 0, 0);

    /* The settings are kept until the host is in sync. */
    rc = ble_br_gap_set_name("watch");
    TEST_ASSERT_FATAL(rc == 0);
    rc = ble_br_gap_set_class_of_device(0x240404);
    TEST_ASSERT_FATAL(rc == 0);
    rc = ble_br_gap_set_scan_mode(BLE_BR_GAP_SCAN_PAGE);
    TEST_ASSERT_FATAL(rc == 0);
    TEST_ASSERT(ble_hs_test_util_hci_out_first() == NULL);

    rc = ble_hs_start();
    TEST_ASSERT_FATAL(rc == 0);
    TEST_ASSERT(ble_br_gap_supported());

    /* Ensure the BR/EDR events got unmasked. */
    param = ble_br_gap_test_util_verify_tx_find(
        BLE_HCI_OGF_CTLR_BASEBAND, BLE_HCI_OCF_CB_SET_EVENT_MASK, &param_len);
    exp_mask = 0x2000800002008090 | BLE_HCI_EVMASK_CONN_DONE |
               BLE_HCI_EVMASK_CONN_REQUEST | BLE_HCI_EVMASK_AUTH_CMP |
               BLE_HCI_EVMASK_MODE_CHANGE | BLE_HCI_EVMASK_PIN_CODE_REQ |
               BLE_HCI_EVMASK_LINK_KEY_REQ | BLE_HCI_EVMASK_LINK_KEY_NOTIFY |
               BLE_HCI_EVMASK_SYNCH_CONN_COMP | BLE_HCI_EVMASK_SYNCH_CONN_CHG |
               BLE_HCI_EVMASK_IO_CAP_REQ | BLE_HCI_EVMASK_IO_CAP_RSP |
               BLE_HCI_EVMASK_USER_CONFIRM_REQ | BLE_HCI_EVMASK_PASSKEY_REQ |
               BLE_HCI_EVMASK_SIMPLE_PAIR_COMP;
    TEST_ASSERT(param_len == 8);
    TEST_ASSERT(get_le64(param) == exp_mask);

    /* Ensure the BR/EDR buffers got read, after the LE ones. */
    ble_br_gap_test_util_verify_tx_find(BLE_HCI_OGF_LE,
                                        BLE_HCI_OCF_LE_RD_BUF_SIZE, &param_len);
    ble_hs_test_util_hci_verify_tx(BLE_HCI_OGF_INFO_PARAMS,
                                   BLE_HCI_OCF_IP_RD_BUF_SIZE, &param_len);
    TEST_ASSERT(param_len == 0);

    /* Ensure the controller got set up for BR/EDR. */
    param = ble_br_gap_test_util_verify_tx_find(
        BLE_HCI_OGF_CTLR_BASEBAND, BLE_HCI_OCF_CB_WR_SSP_MODE, &param_len);
    TEST_ASSERT(param_len == 1);
    TEST_ASSERT(param[0] == 1);

    param = ble_hs_test_util_hci_verify_tx(
        BLE_HCI_OGF_LINK_POLICY, BLE_HCI_OCF_LP_WR_DEFAULT_LINK_POLICY, &param_len);
    TEST_ASSERT(param_len == 2);
    /* Ensure the peer may switch roles and ask for sniff mode. */
    TEST_ASSERT(get_le16(param) ==
                (BLE_HCI_LINK_POLICY_ROLE_SWITCH | BLE_HCI_LINK_POLICY_SNIFF));

    param = ble_hs_test_util_hci_verify_tx(
        BLE_HCI_OGF_CTLR_BASEBAND, BLE_HCI_OCF_CB_WR_CLASS_OF_DEVICE, &param_len);
    TEST_ASSERT(param_len == 3);
    TEST_ASSERT(get_le24(param) == 0x240404);

    param = ble_hs_test_util_hci_verify_tx(
        BLE_HCI_OGF_CTLR_BASEBAND, BLE_HCI_OCF_CB_WR_LOCAL_NAME, &param_len);
    TEST_ASSERT(param_len == BLE_HCI_LOCAL_NAME_LEN);
    TEST_ASSERT(memcmp(param, "watch", 6) == 0);

    param = ble_hs_test_util_hci_verify_tx(BLE_HCI_OGF_CTLR_BASEBAND,
                                           BLE_HCI_OCF_CB_WR_EIR, &param_len);
    TEST_ASSERT(param_len == 1 + BLE_HCI_EIR_LEN);
    TEST_ASSERT(param[0] == 0);
    TEST_ASSERT(memcmp(param + 1, exp_eir, sizeof exp_eir) == 0);

    param = ble_hs_test_util_hci_verify_tx(
        BLE_HCI_OGF_CTLR_BASEBAND, BLE_HCI_OCF_CB_WR_SCAN_ENABLE, &param_len);
    TEST_ASSERT(param_len == 1);
    TEST_ASSERT(param[0] == BLE_BR_GAP_SCAN_PAGE);

    TEST_ASSERT(ble_hs_test_util_hci_out_first() == NULL);

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_gap_test_not_supported)
{
    struct ble_hci_ev_conn_request conn_req;
    struct ble_hci_ev *ev;
    int rc;

    /* The controller only does LE. */
    ble_hs_test_util_init();

    TEST_ASSERT(!ble_br_gap_supported());

    rc = ble_br_gap_connect(ble_br_gap_test_addr);
    TEST_ASSERT(rc == BLE_HS_ENOTSUP);

    /* The settings are accepted, but nothing is sent. */
    rc = ble_br_gap_set_scan_mode(BLE_BR_GAP_SCAN_PAGE);
    TEST_ASSERT(rc == 0);
    rc = ble_br_gap_set_name("watch");
    TEST_ASSERT(rc == 0);
    rc = ble_br_gap_set_class_of_device(0x240404);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(ble_hs_test_util_hci_out_first() == NULL);

    /* BR/EDR events are not handled. */
    memset(&conn_req, 0, sizeof conn_req);
    memcpy(conn_req.bd_addr, ble_br_gap_test_addr, BLE_DEV_ADDR_LEN);
    conn_req.link_type = BLE_HCI_LINK_TYPE_ACL;

    ev = ble_transport_alloc_evt(0);
    TEST_ASSERT_FATAL(ev != NULL);
    ev->opcode = BLE_HCI_EVCODE_CONN_REQUEST;
    ev->length = sizeof conn_req;
    memcpy(ev->data, &conn_req, sizeof conn_req);

    rc = ble_hs_hci_evt_process(ev);
    TEST_ASSERT(rc == BLE_HS_ENOTSUP);
    TEST_ASSERT(ble_hs_test_util_hci_out_first() == NULL);

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_gap_test_settings)
{
    static const uint8_t exp_eir[] = {
        0x06, BLE_HCI_EIR_TYPE_NAME_COMPLETE,   'w',  'a',  't',  'c', 'h',
        0x03, BLE_HCI_EIR_TYPE_UUID16_COMPLETE, 0x1e, 0x11, 0x00,
    };
    uint8_t param_len;
    uint8_t *param;
    int rc;

    ble_br_test_util_init();

    /*** Scan mode. */
    ble_hs_test_util_hci_ack_set(
        BLE_BR_GAP_TEST_CB_OPCODE(BLE_HCI_OCF_CB_WR_SCAN_ENABLE), 0);
    rc = ble_br_gap_set_scan_mode(BLE_BR_GAP_SCAN_INQUIRY | BLE_BR_GAP_SCAN_PAGE);
    TEST_ASSERT(rc == 0);

    param = ble_hs_test_util_hci_verify_tx(
        BLE_HCI_OGF_CTLR_BASEBAND, BLE_HCI_OCF_CB_WR_SCAN_ENABLE, &param_len);
    TEST_ASSERT(param_len == 1);
    TEST_ASSERT(param[0] == 0x03);

    rc = ble_br_gap_set_scan_mode(0x04);
    TEST_ASSERT(rc == BLE_HS_EINVAL);
    TEST_ASSERT(ble_hs_test_util_hci_out_first() == NULL);

    /* The controller refuses. */
    ble_hs_test_util_hci_ack_set(BLE_BR_GAP_TEST_CB_OPCODE(BLE_HCI_OCF_CB_WR_SCAN_ENABLE),
                                 BLE_ERR_CMD_DISALLOWED);
    rc = ble_br_gap_set_scan_mode(BLE_BR_GAP_SCAN_NONE);
    TEST_ASSERT(rc == BLE_HS_HCI_ERR(BLE_ERR_CMD_DISALLOWED));
    ble_hs_test_util_hci_out_clear();

    /*** Class of device. */
    ble_hs_test_util_hci_ack_set(
        BLE_BR_GAP_TEST_CB_OPCODE(BLE_HCI_OCF_CB_WR_CLASS_OF_DEVICE), 0);
    rc = ble_br_gap_set_class_of_device(0x240404);
    TEST_ASSERT(rc == 0);

    param = ble_hs_test_util_hci_verify_tx(
        BLE_HCI_OGF_CTLR_BASEBAND, BLE_HCI_OCF_CB_WR_CLASS_OF_DEVICE, &param_len);
    TEST_ASSERT(param_len == 3);
    TEST_ASSERT(get_le24(param) == 0x240404);

    /*** Name; it is part of the extended inquiry response as well. */
    ble_hs_test_util_hci_ack_set(
        BLE_BR_GAP_TEST_CB_OPCODE(BLE_HCI_OCF_CB_WR_LOCAL_NAME), 0);
    ble_hs_test_util_hci_ack_append(
        BLE_BR_GAP_TEST_CB_OPCODE(BLE_HCI_OCF_CB_WR_EIR), 0);
    rc = ble_br_gap_set_name("watch");
    TEST_ASSERT(rc == 0);

    param = ble_hs_test_util_hci_verify_tx(
        BLE_HCI_OGF_CTLR_BASEBAND, BLE_HCI_OCF_CB_WR_LOCAL_NAME, &param_len);
    TEST_ASSERT(param_len == BLE_HCI_LOCAL_NAME_LEN);
    TEST_ASSERT(memcmp(param, "watch", 6) == 0);

    ble_hs_test_util_hci_verify_tx(BLE_HCI_OGF_CTLR_BASEBAND,
                                   BLE_HCI_OCF_CB_WR_EIR, &param_len);
    TEST_ASSERT(param_len == 1 + BLE_HCI_EIR_LEN);

    rc = ble_br_gap_set_name("a name which is longer than 31 bytes");
    TEST_ASSERT(rc == BLE_HS_EINVAL);
    TEST_ASSERT(ble_hs_test_util_hci_out_first() == NULL);

    /*** Service class in the extended inquiry response. */
    ble_hs_test_util_hci_ack_set(BLE_BR_GAP_TEST_CB_OPCODE(BLE_HCI_OCF_CB_WR_EIR), 0);
    rc = ble_br_gap_eir_add_uuid16(0x111e);
    TEST_ASSERT(rc == 0);

    param = ble_hs_test_util_hci_verify_tx(BLE_HCI_OGF_CTLR_BASEBAND,
                                           BLE_HCI_OCF_CB_WR_EIR, &param_len);
    TEST_ASSERT(param_len == 1 + BLE_HCI_EIR_LEN);
    TEST_ASSERT(param[0] == 0);
    TEST_ASSERT(memcmp(param + 1, exp_eir, sizeof exp_eir) == 0);

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_gap_test_conn_incoming)
{
    const struct ble_br_gap_event *event;
    uint8_t param_len;
    uint8_t *param;

    ble_br_test_util_init();

    /*** The controller fails to set the connection up. */
    ble_br_test_util_ack_set(BLE_HCI_OCF_ACCEPT_CONN_REQ, 0);
    ble_br_test_util_rx_conn_request(ble_br_gap_test_addr, BLE_HCI_LINK_TYPE_ACL);
    ble_hs_test_util_hci_verify_tx(BLE_HCI_OGF_LINK_CTRL,
                                   BLE_HCI_OCF_ACCEPT_CONN_REQ, &param_len);
    TEST_ASSERT(ble_br_test_util_gap_event_cnt() == 0);

    ble_br_test_util_rx_conn_cmp(BLE_ERR_PAGE_TMO, 0, ble_br_gap_test_addr,
                                 BLE_HCI_LINK_TYPE_ACL, 0);

    event = ble_br_test_util_gap_event_next();
    TEST_ASSERT_FATAL(event != NULL);
    TEST_ASSERT(event->type == BLE_BR_GAP_EVENT_CONNECT);
    TEST_ASSERT(event->connect.status == BLE_HS_HCI_ERR(BLE_ERR_PAGE_TMO));
    TEST_ASSERT(memcmp(event->connect.addr, ble_br_gap_test_addr,
                       BLE_DEV_ADDR_LEN) == 0);

    /*** The connection is accepted and gets established. */
    ble_br_test_util_create_conn(2, ble_br_gap_test_addr, 0);

    /*** Further requests are rejected: there is no room for them. */
    ble_br_test_util_ack_set(BLE_HCI_OCF_REJECT_CONN_REQ, 0);
    ble_br_test_util_rx_conn_request(ble_br_gap_test_addr2, BLE_HCI_LINK_TYPE_ACL);

    param = ble_hs_test_util_hci_verify_tx(
        BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_REJECT_CONN_REQ, &param_len);
    TEST_ASSERT(param_len == sizeof(struct ble_hci_lc_reject_conn_req_cp));
    TEST_ASSERT(memcmp(param, ble_br_gap_test_addr2, BLE_DEV_ADDR_LEN) == 0);
    TEST_ASSERT(param[6] == BLE_ERR_CONN_REJ_RESOURCES);

    ble_br_test_util_ack_set(BLE_HCI_OCF_REJECT_CONN_REQ, 0);
    ble_br_test_util_rx_conn_request(ble_br_gap_test_addr, BLE_HCI_LINK_TYPE_ACL);
    ble_hs_test_util_hci_verify_tx(BLE_HCI_OGF_LINK_CTRL,
                                   BLE_HCI_OCF_REJECT_CONN_REQ, &param_len);

    /* A completion event nothing asked for is ignored. */
    ble_br_test_util_rx_conn_cmp(0, 3, ble_br_gap_test_addr2,
                                 BLE_HCI_LINK_TYPE_ACL, 0);
    TEST_ASSERT(ble_br_test_util_gap_event_cnt() == 0);

    /* So is a malformed event. */
    ble_br_test_util_rx_evt(BLE_HCI_EVCODE_CONN_REQUEST, ble_br_gap_test_addr2,
                            BLE_DEV_ADDR_LEN);
    TEST_ASSERT(ble_hs_test_util_hci_out_first() == NULL);
    TEST_ASSERT(ble_br_test_util_gap_event_cnt() == 0);

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_gap_test_conn_outgoing)
{
    const struct ble_br_gap_event *event;
    uint8_t param_len;
    uint8_t *param;
    int rc;

    ble_br_test_util_init();

    /*** The controller refuses the command. */
    ble_br_test_util_ack_set(BLE_HCI_OCF_CREATE_CONN, BLE_ERR_CMD_DISALLOWED);
    rc = ble_br_gap_connect(ble_br_gap_test_addr);
    TEST_ASSERT(rc == BLE_HS_HCI_ERR(BLE_ERR_CMD_DISALLOWED));
    ble_hs_test_util_hci_out_clear();

    /*** The connection gets established. */
    ble_br_test_util_ack_set(BLE_HCI_OCF_CREATE_CONN, 0);
    rc = ble_br_gap_connect(ble_br_gap_test_addr);
    TEST_ASSERT_FATAL(rc == 0);

    param = ble_hs_test_util_hci_verify_tx(BLE_HCI_OGF_LINK_CTRL,
                                           BLE_HCI_OCF_CREATE_CONN, &param_len);
    TEST_ASSERT(param_len == sizeof(struct ble_hci_lc_create_conn_cp));
    TEST_ASSERT(memcmp(param, ble_br_gap_test_addr, BLE_DEV_ADDR_LEN) == 0);
    TEST_ASSERT(get_le16(param + 6) == BLE_HCI_ACL_PKT_TYPES);
    TEST_ASSERT(param[8] == BLE_HCI_PSR_MODE_R1);
    TEST_ASSERT(param[9] == 0);
    TEST_ASSERT(get_le16(param + 10) == 0);
    TEST_ASSERT(param[12] == 1);

    /* One attempt per peer at a time. */
    rc = ble_br_gap_connect(ble_br_gap_test_addr);
    TEST_ASSERT(rc == BLE_HS_EALREADY);

    /* No room for another connection. */
    rc = ble_br_gap_connect(ble_br_gap_test_addr2);
    TEST_ASSERT(rc == BLE_HS_ENOMEM);
    TEST_ASSERT(ble_hs_test_util_hci_out_first() == NULL);

    ble_br_test_util_rx_conn_cmp(0, 2, ble_br_gap_test_addr, BLE_HCI_LINK_TYPE_ACL, 0);

    event = ble_br_test_util_gap_event_next();
    TEST_ASSERT_FATAL(event != NULL);
    TEST_ASSERT(event->type == BLE_BR_GAP_EVENT_CONNECT);
    TEST_ASSERT(event->connect.status == 0);
    TEST_ASSERT(event->connect.conn_handle == 2);
    TEST_ASSERT(memcmp(event->connect.addr, ble_br_gap_test_addr,
                       BLE_DEV_ADDR_LEN) == 0);

    rc = ble_br_gap_connect(ble_br_gap_test_addr);
    TEST_ASSERT(rc == BLE_HS_EALREADY);

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_gap_test_disconnect)
{
    const struct ble_br_gap_event *event;
    int rc;

    ble_br_test_util_init();

    rc = ble_br_gap_terminate(2, BLE_ERR_REM_USER_CONN_TERM);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);

    ble_br_test_util_create_conn(2, ble_br_gap_test_addr, 0);

    /*** We terminate the connection. */
    ble_hs_test_util_hci_ack_set_disconnect(0);
    rc = ble_br_gap_terminate(2, BLE_ERR_REM_USER_CONN_TERM);
    TEST_ASSERT_FATAL(rc == 0);
    ble_hs_test_util_hci_verify_tx_disconnect(2, BLE_ERR_REM_USER_CONN_TERM);

    /* A failed disconnection leaves the connection as it is. */
    ble_hs_test_util_hci_rx_disconn_complete_event(2, BLE_ERR_CMD_DISALLOWED, 0);
    TEST_ASSERT(ble_br_test_util_gap_event_cnt() == 0);

    ble_hs_test_util_hci_rx_disconn_complete_event(2, 0, BLE_ERR_CONN_TERM_LOCAL);

    event = ble_br_test_util_gap_event_next();
    TEST_ASSERT_FATAL(event != NULL);
    TEST_ASSERT(event->type == BLE_BR_GAP_EVENT_DISCONNECT);
    TEST_ASSERT(event->disconnect.conn_handle == 2);
    TEST_ASSERT(event->disconnect.reason == BLE_HS_HCI_ERR(BLE_ERR_CONN_TERM_LOCAL));
    TEST_ASSERT(memcmp(event->disconnect.addr, ble_br_gap_test_addr,
                       BLE_DEV_ADDR_LEN) == 0);

    rc = ble_br_gap_terminate(2, BLE_ERR_REM_USER_CONN_TERM);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);

    /*** The peer terminates the connection; there is room for it again. */
    ble_br_test_util_create_conn(3, ble_br_gap_test_addr2, 0);

    ble_hs_test_util_hci_rx_disconn_complete_event(3, 0, BLE_ERR_REM_USER_CONN_TERM);

    event = ble_br_test_util_gap_event_next();
    TEST_ASSERT_FATAL(event != NULL);
    TEST_ASSERT(event->type == BLE_BR_GAP_EVENT_DISCONNECT);
    TEST_ASSERT(event->disconnect.conn_handle == 3);
    TEST_ASSERT(event->disconnect.reason == BLE_HS_HCI_ERR(BLE_ERR_REM_USER_CONN_TERM));
    TEST_ASSERT(memcmp(event->disconnect.addr, ble_br_gap_test_addr2,
                       BLE_DEV_ADDR_LEN) == 0);

    TEST_ASSERT(ble_br_test_util_gap_event_cnt() == 0);

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_gap_test_link_key_req)
{
    uint8_t param_len;
    uint8_t *param;
    int rc;

    ble_br_test_util_init();

    /*** No key for the peer: negative reply. */
    ble_br_test_util_ack_set_addr(BLE_HCI_OCF_LINK_KEY_REQ_NEG_REPLY,
                                  ble_br_gap_test_addr);
    ble_br_gap_test_util_rx_addr_evt(BLE_HCI_EVCODE_LINK_KEY_REQ, ble_br_gap_test_addr);
    ble_br_test_util_verify_tx_addr(BLE_HCI_OCF_LINK_KEY_REQ_NEG_REPLY,
                                    ble_br_gap_test_addr);

    /*** A stored key: positive reply. */
    rc = ble_br_gap_set_link_key(ble_br_gap_test_addr, ble_br_gap_test_key,
                                 BLE_HCI_LINK_KEY_TYPE_UNAUTH_P256);
    TEST_ASSERT_FATAL(rc == 0);

    ble_br_test_util_ack_set_addr(BLE_HCI_OCF_LINK_KEY_REQ_REPLY, ble_br_gap_test_addr);
    ble_br_gap_test_util_rx_addr_evt(BLE_HCI_EVCODE_LINK_KEY_REQ, ble_br_gap_test_addr);

    param = ble_hs_test_util_hci_verify_tx(
        BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_LINK_KEY_REQ_REPLY, &param_len);
    TEST_ASSERT(param_len == sizeof(struct ble_hci_lc_link_key_req_reply_cp));
    TEST_ASSERT(memcmp(param, ble_br_gap_test_addr, BLE_DEV_ADDR_LEN) == 0);
    TEST_ASSERT(memcmp(param + 6, ble_br_gap_test_key, BLE_BR_GAP_LINK_KEY_LEN) == 0);

    /* The key of a peer is not given to another one. */
    ble_br_test_util_ack_set_addr(BLE_HCI_OCF_LINK_KEY_REQ_NEG_REPLY,
                                  ble_br_gap_test_addr2);
    ble_br_gap_test_util_rx_addr_evt(BLE_HCI_EVCODE_LINK_KEY_REQ, ble_br_gap_test_addr2);
    ble_br_test_util_verify_tx_addr(BLE_HCI_OCF_LINK_KEY_REQ_NEG_REPLY,
                                    ble_br_gap_test_addr2);

    /*** The key is deleted: negative reply again. */
    rc = ble_br_gap_delete_link_key(ble_br_gap_test_addr);
    TEST_ASSERT(rc == 0);
    rc = ble_br_gap_delete_link_key(ble_br_gap_test_addr);
    TEST_ASSERT(rc == BLE_HS_ENOENT);

    ble_br_test_util_ack_set_addr(BLE_HCI_OCF_LINK_KEY_REQ_NEG_REPLY,
                                  ble_br_gap_test_addr);
    ble_br_gap_test_util_rx_addr_evt(BLE_HCI_EVCODE_LINK_KEY_REQ, ble_br_gap_test_addr);
    ble_br_test_util_verify_tx_addr(BLE_HCI_OCF_LINK_KEY_REQ_NEG_REPLY,
                                    ble_br_gap_test_addr);

    TEST_ASSERT(ble_br_test_util_gap_event_cnt() == 0);

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_gap_test_link_key_notify)
{
    static const uint8_t key2[BLE_BR_GAP_LINK_KEY_LEN] = {
        0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
        0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
    };
    const struct ble_br_gap_event *event;
    uint8_t param_len;
    uint8_t *param;
    int count;
    int rc;

    ble_br_test_util_init();

    ble_br_gap_test_util_rx_link_key_notify(ble_br_gap_test_addr, ble_br_gap_test_key,
                                            BLE_HCI_LINK_KEY_TYPE_AUTH_P256);

    /* Ensure the application got told. */
    event = ble_br_test_util_gap_event_next();
    TEST_ASSERT_FATAL(event != NULL);
    TEST_ASSERT(event->type == BLE_BR_GAP_EVENT_LINK_KEY);
    TEST_ASSERT(memcmp(event->link_key.addr, ble_br_gap_test_addr,
                       BLE_DEV_ADDR_LEN) == 0);
    TEST_ASSERT(memcmp(event->link_key.key, ble_br_gap_test_key,
                       BLE_BR_GAP_LINK_KEY_LEN) == 0);
    TEST_ASSERT(event->link_key.key_type == BLE_HCI_LINK_KEY_TYPE_AUTH_P256);

    /* Ensure the key got stored. */
    ble_br_gap_test_util_verify_link_key(ble_br_gap_test_addr, ble_br_gap_test_key,
                                         BLE_HCI_LINK_KEY_TYPE_AUTH_P256);

    /* A new key for the peer replaces the one it had. */
    ble_br_gap_test_util_rx_link_key_notify(ble_br_gap_test_addr, key2,
                                            BLE_HCI_LINK_KEY_TYPE_UNAUTH_P256);

    event = ble_br_test_util_gap_event_next();
    TEST_ASSERT_FATAL(event != NULL);
    TEST_ASSERT(event->type == BLE_BR_GAP_EVENT_LINK_KEY);
    TEST_ASSERT(memcmp(event->link_key.key, key2, BLE_BR_GAP_LINK_KEY_LEN) == 0);

    ble_br_gap_test_util_verify_link_key(ble_br_gap_test_addr, key2,
                                         BLE_HCI_LINK_KEY_TYPE_UNAUTH_P256);
    rc = ble_store_util_count(BLE_STORE_OBJ_TYPE_BR_LINK_KEY, &count);
    TEST_ASSERT_FATAL(rc == 0);
    TEST_ASSERT(count == 1);

    /* Ensure the key is the one given to the controller. */
    ble_br_test_util_ack_set_addr(BLE_HCI_OCF_LINK_KEY_REQ_REPLY, ble_br_gap_test_addr);
    ble_br_gap_test_util_rx_addr_evt(BLE_HCI_EVCODE_LINK_KEY_REQ, ble_br_gap_test_addr);

    param = ble_hs_test_util_hci_verify_tx(
        BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_LINK_KEY_REQ_REPLY, &param_len);
    TEST_ASSERT(param_len == sizeof(struct ble_hci_lc_link_key_req_reply_cp));
    TEST_ASSERT(memcmp(param + 6, key2, BLE_BR_GAP_LINK_KEY_LEN) == 0);

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_gap_test_pairing)
{
    const struct ble_br_gap_event *event;
    struct ble_hci_ev_user_confirm_req confirm_req;
    struct ble_hs_cfg cfg;
    uint8_t io_cap_rsp[9];
    uint8_t param_len;
    uint8_t *param;
    int rc;

    ble_br_test_util_init();

    cfg = ble_hs_cfg;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_KEYBOARD_DISPLAY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;

    ble_br_test_util_create_conn(2, ble_br_gap_test_addr, 0);

    /*** IO capabilities. */
    ble_br_test_util_ack_set_addr(BLE_HCI_OCF_IO_CAP_REQ_REPLY, ble_br_gap_test_addr);
    ble_br_gap_test_util_rx_addr_evt(BLE_HCI_EVOCDE_IO_CAP_REQ, ble_br_gap_test_addr);

    param = ble_hs_test_util_hci_verify_tx(
        BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_IO_CAP_REQ_REPLY, &param_len);
    TEST_ASSERT(param_len == sizeof(struct ble_hci_lc_io_cap_req_reply_cp));
    TEST_ASSERT(memcmp(param, ble_br_gap_test_addr, BLE_DEV_ADDR_LEN) == 0);
    TEST_ASSERT(param[6] == BLE_HCI_IO_CAP_DISPLAY_YESNO);
    TEST_ASSERT(param[7] == 0);
    TEST_ASSERT(param[8] == (BLE_HCI_AUTH_REQ_GENERAL_BONDING | BLE_HCI_AUTH_REQ_MITM));

    /* The capabilities of the peer need no answer. */
    memset(io_cap_rsp, 0, sizeof io_cap_rsp);
    memcpy(io_cap_rsp, ble_br_gap_test_addr, BLE_DEV_ADDR_LEN);
    ble_br_test_util_rx_evt(BLE_HCI_EVCODE_IO_CAP_RSP, io_cap_rsp, sizeof io_cap_rsp);
    TEST_ASSERT(ble_hs_test_util_hci_out_first() == NULL);
    TEST_ASSERT(ble_br_test_util_gap_event_cnt() == 0);

    /*** Numeric comparison; the application accepts. */
    memcpy(confirm_req.bd_addr, ble_br_gap_test_addr, BLE_DEV_ADDR_LEN);
    confirm_req.numeric_value = htole32(123456);
    ble_br_test_util_rx_evt(BLE_HCI_EVCODE_USER_CONFIRM_REQ, &confirm_req,
                            sizeof confirm_req);
    TEST_ASSERT(ble_hs_test_util_hci_out_first() == NULL);

    event = ble_br_test_util_gap_event_next();
    TEST_ASSERT_FATAL(event != NULL);
    TEST_ASSERT(event->type == BLE_BR_GAP_EVENT_USER_CONFIRM);
    TEST_ASSERT(memcmp(event->user_confirm.addr, ble_br_gap_test_addr,
                       BLE_DEV_ADDR_LEN) == 0);
    TEST_ASSERT(event->user_confirm.numeric_value == 123456);

    ble_br_test_util_ack_set_addr(BLE_HCI_OCF_USER_CONFIRM_REPLY, ble_br_gap_test_addr);
    rc = ble_br_gap_user_confirm(ble_br_gap_test_addr, true);
    TEST_ASSERT(rc == 0);
    ble_br_test_util_verify_tx_addr(BLE_HCI_OCF_USER_CONFIRM_REPLY,
                                    ble_br_gap_test_addr);

    ble_br_gap_test_util_rx_simple_pair_cmp(ble_br_gap_test_addr, 0);

    event = ble_br_test_util_gap_event_next();
    TEST_ASSERT_FATAL(event != NULL);
    TEST_ASSERT(event->type == BLE_BR_GAP_EVENT_PAIRING_COMPLETE);
    TEST_ASSERT(event->pairing_complete.status == 0);
    TEST_ASSERT(memcmp(event->pairing_complete.addr, ble_br_gap_test_addr,
                       BLE_DEV_ADDR_LEN) == 0);

    /*** Numeric comparison; the application refuses. */
    ble_br_test_util_rx_evt(BLE_HCI_EVCODE_USER_CONFIRM_REQ, &confirm_req,
                            sizeof confirm_req);

    event = ble_br_test_util_gap_event_next();
    TEST_ASSERT_FATAL(event != NULL);
    TEST_ASSERT(event->type == BLE_BR_GAP_EVENT_USER_CONFIRM);

    ble_br_test_util_ack_set_addr(BLE_HCI_OCF_USER_CONFIRM_NEG_REPLY,
                                  ble_br_gap_test_addr);
    rc = ble_br_gap_user_confirm(ble_br_gap_test_addr, false);
    TEST_ASSERT(rc == 0);
    ble_br_test_util_verify_tx_addr(BLE_HCI_OCF_USER_CONFIRM_NEG_REPLY,
                                    ble_br_gap_test_addr);

    ble_br_gap_test_util_rx_simple_pair_cmp(ble_br_gap_test_addr, BLE_ERR_AUTH_FAIL);

    event = ble_br_test_util_gap_event_next();
    TEST_ASSERT_FATAL(event != NULL);
    TEST_ASSERT(event->type == BLE_BR_GAP_EVENT_PAIRING_COMPLETE);
    TEST_ASSERT(event->pairing_complete.status == BLE_HS_HCI_ERR(BLE_ERR_AUTH_FAIL));

    /*** Legacy pairing is refused. */
    ble_br_test_util_ack_set_addr(BLE_HCI_OCF_PIN_CODE_REQ_NEG_REPLY,
                                  ble_br_gap_test_addr);
    ble_br_gap_test_util_rx_addr_evt(BLE_HCI_EVCODE_PIN_CODE_REQ, ble_br_gap_test_addr);
    ble_br_test_util_verify_tx_addr(BLE_HCI_OCF_PIN_CODE_REQ_NEG_REPLY,
                                    ble_br_gap_test_addr);

    TEST_ASSERT(ble_br_test_util_gap_event_cnt() == 0);

    ble_hs_cfg = cfg;

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_gap_test_pairing_no_io)
{
    struct ble_hci_ev_user_confirm_req confirm_req;
    struct ble_hs_cfg cfg;
    uint8_t param_len;
    uint8_t *param;

    ble_br_test_util_init();

    cfg = ble_hs_cfg;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 1;

    ble_br_test_util_create_conn(2, ble_br_gap_test_addr, 0);

    /* Protection against MITM can not be asked for without IO. */
    ble_br_test_util_ack_set_addr(BLE_HCI_OCF_IO_CAP_REQ_REPLY, ble_br_gap_test_addr);
    ble_br_gap_test_util_rx_addr_evt(BLE_HCI_EVOCDE_IO_CAP_REQ, ble_br_gap_test_addr);

    param = ble_hs_test_util_hci_verify_tx(
        BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_IO_CAP_REQ_REPLY, &param_len);
    TEST_ASSERT(param_len == sizeof(struct ble_hci_lc_io_cap_req_reply_cp));
    TEST_ASSERT(param[6] == BLE_HCI_IO_CAP_NO_INPUT_OUTPUT);
    TEST_ASSERT(param[7] == 0);
    TEST_ASSERT(param[8] == BLE_HCI_AUTH_REQ_NO_BONDING);

    /* The value is confirmed without asking the application. */
    memcpy(confirm_req.bd_addr, ble_br_gap_test_addr, BLE_DEV_ADDR_LEN);
    confirm_req.numeric_value = htole32(123456);
    ble_br_test_util_ack_set_addr(BLE_HCI_OCF_USER_CONFIRM_REPLY, ble_br_gap_test_addr);
    ble_br_test_util_rx_evt(BLE_HCI_EVCODE_USER_CONFIRM_REQ, &confirm_req,
                            sizeof confirm_req);
    ble_br_test_util_verify_tx_addr(BLE_HCI_OCF_USER_CONFIRM_REPLY,
                                    ble_br_gap_test_addr);

    TEST_ASSERT(ble_br_test_util_gap_event_cnt() == 0);

    ble_hs_cfg = cfg;

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_gap_test_security)
{
    const struct ble_br_gap_event *event;
    uint8_t param_len;
    uint8_t *param;
    int rc;

    ble_br_test_util_init();

    rc = ble_br_gap_security_initiate(2);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);

    ble_br_test_util_create_conn(2, ble_br_gap_test_addr, 0);

    /*** Authentication, then encryption. */
    ble_br_test_util_ack_set(BLE_HCI_OCF_AUTH_REQUESTED, 0);
    rc = ble_br_gap_security_initiate(2);
    TEST_ASSERT_FATAL(rc == 0);

    param = ble_hs_test_util_hci_verify_tx(BLE_HCI_OGF_LINK_CTRL,
                                           BLE_HCI_OCF_AUTH_REQUESTED, &param_len);
    TEST_ASSERT(param_len == 2);
    TEST_ASSERT(get_le16(param) == 2);

    /* Nothing more is sent while the procedure is in progress. */
    rc = ble_br_gap_security_initiate(2);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(ble_hs_test_util_hci_out_first() == NULL);

    ble_br_test_util_ack_set(BLE_HCI_OCF_SET_CONN_ENCRYPT, 0);
    ble_br_gap_test_util_rx_auth_cmp(2, 0);

    param = ble_hs_test_util_hci_verify_tx(
        BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_SET_CONN_ENCRYPT, &param_len);
    TEST_ASSERT(param_len == 3);
    TEST_ASSERT(get_le16(param) == 2);
    TEST_ASSERT(param[2] == 1);
    TEST_ASSERT(ble_br_test_util_gap_event_cnt() == 0);

    ble_br_test_util_rx_enc_change(2, 0, 1);

    event = ble_br_test_util_gap_event_next();
    TEST_ASSERT_FATAL(event != NULL);
    TEST_ASSERT(event->type == BLE_BR_GAP_EVENT_ENC_CHANGE);
    TEST_ASSERT(event->enc_change.status == 0);
    TEST_ASSERT(event->enc_change.conn_handle == 2);
    TEST_ASSERT(event->enc_change.encrypted);

    rc = ble_br_gap_security_initiate(2);
    TEST_ASSERT(rc == BLE_HS_EALREADY);

    /*** The peer turns encryption off. */
    ble_br_test_util_rx_enc_change(2, 0, 0);

    event = ble_br_test_util_gap_event_next();
    TEST_ASSERT_FATAL(event != NULL);
    TEST_ASSERT(event->type == BLE_BR_GAP_EVENT_ENC_CHANGE);
    TEST_ASSERT(event->enc_change.status == 0);
    TEST_ASSERT(!event->enc_change.encrypted);

    /*** The peer lost its key: ours is of no use anymore. */
    rc = ble_br_gap_set_link_key(ble_br_gap_test_addr, ble_br_gap_test_key,
                                 BLE_HCI_LINK_KEY_TYPE_UNAUTH_P256);
    TEST_ASSERT_FATAL(rc == 0);

    ble_br_test_util_ack_set(BLE_HCI_OCF_AUTH_REQUESTED, 0);
    rc = ble_br_gap_security_initiate(2);
    TEST_ASSERT_FATAL(rc == 0);
    ble_hs_test_util_hci_verify_tx(BLE_HCI_OGF_LINK_CTRL,
                                   BLE_HCI_OCF_AUTH_REQUESTED, &param_len);

    ble_br_gap_test_util_rx_auth_cmp(2, BLE_ERR_PINKEY_MISSING);
    TEST_ASSERT(ble_hs_test_util_hci_out_first() == NULL);

    event = ble_br_test_util_gap_event_next();
    TEST_ASSERT_FATAL(event != NULL);
    TEST_ASSERT(event->type == BLE_BR_GAP_EVENT_ENC_CHANGE);
    TEST_ASSERT(event->enc_change.status == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING));
    TEST_ASSERT(event->enc_change.conn_handle == 2);
    TEST_ASSERT(!event->enc_change.encrypted);

    ble_br_gap_test_util_verify_link_key(ble_br_gap_test_addr, NULL, 0);

    /* The procedure can be started again. */
    ble_br_test_util_ack_set(BLE_HCI_OCF_AUTH_REQUESTED, 0);
    rc = ble_br_gap_security_initiate(2);
    TEST_ASSERT(rc == 0);
    ble_hs_test_util_hci_verify_tx(BLE_HCI_OGF_LINK_CTRL,
                                   BLE_HCI_OCF_AUTH_REQUESTED, &param_len);

    TEST_ASSERT(ble_br_test_util_gap_event_cnt() == 0);

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

static void
ble_br_gap_test_util_rx_mode_change(uint8_t status, uint16_t conn_handle,
                                    uint8_t mode, uint16_t interval)
{
    struct ble_hci_ev_mode_change ev;

    ev.status = status;
    ev.conn_handle = htole16(conn_handle);
    ev.mode = mode;
    ev.interval = htole16(interval);

    ble_br_test_util_rx_evt(BLE_HCI_EVCODE_MODE_CHANGE, &ev, sizeof ev);
}

static void
ble_br_gap_test_util_verify_mode_change(int status, bool sniff, uint16_t interval)
{
    const struct ble_br_gap_event *event;

    event = ble_br_test_util_gap_event_next();
    TEST_ASSERT_FATAL(event != NULL);
    TEST_ASSERT(event->type == BLE_BR_GAP_EVENT_MODE_CHANGE);
    TEST_ASSERT(event->mode_change.status == status);
    TEST_ASSERT(event->mode_change.conn_handle == 2);
    TEST_ASSERT(event->mode_change.sniff == sniff);
    TEST_ASSERT(event->mode_change.interval == interval);
}

TEST_CASE_SELF(ble_br_gap_test_sniff)
{
    struct ble_hci_ev_mode_change mode_change;
    uint8_t rsp[2];
    uint8_t param_len;
    uint8_t *param;
    int rc;

    ble_br_test_util_init();

    /*** No connection. */
    rc = ble_br_gap_sniff(2, 0x0320, 0x0640, 4, 1);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);
    rc = ble_br_gap_exit_sniff(2);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);
    rc = ble_br_gap_sniff_subrating(2, 0x0c80, 0x0140, 0x0280);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);
    TEST_ASSERT(ble_hs_test_util_hci_out_first() == NULL);

    ble_br_test_util_create_conn(2, ble_br_gap_test_addr, 0);

    /*** Sniff mode. */
    ble_hs_test_util_hci_ack_set(
        BLE_BR_GAP_TEST_LP_OPCODE(BLE_HCI_OCF_LP_SNIFF_MODE), 0);
    rc = ble_br_gap_sniff(2, 0x0320, 0x0640, 4, 1);
    TEST_ASSERT(rc == 0);

    /* The longest interval comes first in the command. */
    param = ble_hs_test_util_hci_verify_tx(BLE_HCI_OGF_LINK_POLICY,
                                           BLE_HCI_OCF_LP_SNIFF_MODE, &param_len);
    TEST_ASSERT(param_len == sizeof(struct ble_hci_lp_sniff_mode_cp));
    TEST_ASSERT(get_le16(param + 0) == 2);
    TEST_ASSERT(get_le16(param + 2) == 0x0640);
    TEST_ASSERT(get_le16(param + 4) == 0x0320);
    TEST_ASSERT(get_le16(param + 6) == 4);
    TEST_ASSERT(get_le16(param + 8) == 1);
    TEST_ASSERT(ble_br_test_util_gap_event_cnt() == 0);

    /* The controller refuses. */
    ble_hs_test_util_hci_ack_set(BLE_BR_GAP_TEST_LP_OPCODE(BLE_HCI_OCF_LP_SNIFF_MODE),
                                 BLE_ERR_CMD_DISALLOWED);
    rc = ble_br_gap_sniff(2, 0x0320, 0x0640, 4, 1);
    TEST_ASSERT(rc == BLE_HS_HCI_ERR(BLE_ERR_CMD_DISALLOWED));
    ble_hs_test_util_hci_out_clear();

    /*** Sniff subrating. */
    put_le16(rsp, 2);
    ble_hs_test_util_hci_ack_set_params(
        BLE_BR_GAP_TEST_LP_OPCODE(BLE_HCI_OCF_LP_SNIFF_SUBRATING), 0, rsp, sizeof rsp);
    rc = ble_br_gap_sniff_subrating(2, 0x0c80, 0x0140, 0x0280);
    TEST_ASSERT(rc == 0);

    param = ble_hs_test_util_hci_verify_tx(
        BLE_HCI_OGF_LINK_POLICY, BLE_HCI_OCF_LP_SNIFF_SUBRATING, &param_len);
    TEST_ASSERT(param_len == sizeof(struct ble_hci_lp_sniff_subrating_cp));
    TEST_ASSERT(get_le16(param + 0) == 2);
    TEST_ASSERT(get_le16(param + 2) == 0x0c80);
    TEST_ASSERT(get_le16(param + 4) == 0x0140);
    TEST_ASSERT(get_le16(param + 6) == 0x0280);

    /*** Exit sniff mode. */
    ble_hs_test_util_hci_ack_set(
        BLE_BR_GAP_TEST_LP_OPCODE(BLE_HCI_OCF_LP_EXIT_SNIFF_MODE), 0);
    rc = ble_br_gap_exit_sniff(2);
    TEST_ASSERT(rc == 0);

    param = ble_hs_test_util_hci_verify_tx(
        BLE_HCI_OGF_LINK_POLICY, BLE_HCI_OCF_LP_EXIT_SNIFF_MODE, &param_len);
    TEST_ASSERT(param_len == sizeof(struct ble_hci_lp_exit_sniff_mode_cp));
    TEST_ASSERT(get_le16(param) == 2);

    TEST_ASSERT(ble_hs_test_util_hci_out_first() == NULL);

    /*** The connection enters sniff mode, then leaves it. */
    ble_br_gap_test_util_rx_mode_change(0, 2, BLE_HCI_MODE_SNIFF, 0x0320);
    ble_br_gap_test_util_verify_mode_change(0, true, 0x0320);

    ble_br_gap_test_util_rx_mode_change(0, 2, BLE_HCI_MODE_ACTIVE, 0);
    ble_br_gap_test_util_verify_mode_change(0, false, 0);

    /*** The mode fails to change. */
    ble_br_gap_test_util_rx_mode_change(BLE_ERR_UNSUPP_REM_FEATURE, 2,
                                        BLE_HCI_MODE_ACTIVE, 0);
    ble_br_gap_test_util_verify_mode_change(
        BLE_HS_HCI_ERR(BLE_ERR_UNSUPP_REM_FEATURE), false, 0);

    /*** Events for no connection, or malformed, are not reported. */
    ble_br_gap_test_util_rx_mode_change(0, 3, BLE_HCI_MODE_SNIFF, 0x0320);

    mode_change.status = 0;
    mode_change.conn_handle = htole16(2);
    mode_change.mode = BLE_HCI_MODE_SNIFF;
    mode_change.interval = htole16(0x0320);
    ble_br_test_util_rx_evt(BLE_HCI_EVCODE_MODE_CHANGE, &mode_change,
                            sizeof mode_change - 1);

    TEST_ASSERT(ble_br_test_util_gap_event_cnt() == 0);
    TEST_ASSERT(ble_hs_test_util_hci_out_first() == NULL);

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

#endif

TEST_SUITE(ble_br_gap_test_suite)
{
#if MYNEWT_VAL(BLE_BREDR)
    ble_br_gap_test_startup();
    ble_br_gap_test_not_supported();
    ble_br_gap_test_settings();
    ble_br_gap_test_conn_incoming();
    ble_br_gap_test_conn_outgoing();
    ble_br_gap_test_disconnect();
    ble_br_gap_test_link_key_req();
    ble_br_gap_test_link_key_notify();
    ble_br_gap_test_pairing();
    ble_br_gap_test_pairing_no_io();
    ble_br_gap_test_security();
    ble_br_gap_test_sniff();
#endif
}
