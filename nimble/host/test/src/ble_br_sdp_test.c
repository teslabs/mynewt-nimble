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
#include "host/ble_br_hfp_hf.h"
#include "ble_hs_test.h"
#include "ble_hs_test_util.h"
#include "ble_br_test_util.h"
#include "ble_br_sdp_priv.h"

#if MYNEWT_VAL(BLE_BREDR)

#define BLE_BR_SDP_TEST_PDU_ERROR_RSP       0x01
#define BLE_BR_SDP_TEST_PDU_SEARCH_REQ      0x02
#define BLE_BR_SDP_TEST_PDU_SEARCH_RSP      0x03
#define BLE_BR_SDP_TEST_PDU_ATTR_REQ        0x04
#define BLE_BR_SDP_TEST_PDU_ATTR_RSP        0x05
#define BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_REQ 0x06
#define BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_RSP 0x07

#define BLE_BR_SDP_TEST_ERR_INVALID_HANDLE 0x0002
#define BLE_BR_SDP_TEST_ERR_INVALID_SYNTAX 0x0003
#define BLE_BR_SDP_TEST_ERR_INVALID_CONT   0x0005

#define BLE_BR_SDP_TEST_HDR_SZ 5

/* An attribute of a service record, from its ID and its value. */
#define BLE_BR_SDP_TEST_ATTR(id, value)                                       \
    { BLE_BR_SDP_ATTR_##id, (value), sizeof(value) }

/* What the client can take of the attributes of the peer. */
#define BLE_BR_SDP_TEST_CLIENT_BUF_SZ 160

#define BLE_BR_SDP_TEST_CLIENT_TIMEOUT_MS 30000

static const uint8_t ble_br_sdp_test_addr[BLE_DEV_ADDR_LEN] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
};

/* A serial port service, on RFCOMM channel 5. */
static const uint8_t ble_br_sdp_test_classes[] = {
    0x35, 0x03, 0x19, 0x11, 0x01,
};

static const uint8_t ble_br_sdp_test_protocols[] = {
    0x35, 0x0c, 0x35, 0x03, 0x19, 0x01, 0x00,
    0x35, 0x05, 0x19, 0x00, 0x03, 0x08, 0x05,
};

static const uint8_t ble_br_sdp_test_name[] = {
    0x25, 0x04, 'T', 'e', 's', 't',
};

static const struct ble_br_sdp_attr ble_br_sdp_test_attrs[] = {
    BLE_BR_SDP_TEST_ATTR(SERVICE_CLASS_LIST, ble_br_sdp_test_classes),
    BLE_BR_SDP_TEST_ATTR(PROTOCOL_DESC_LIST, ble_br_sdp_test_protocols),
    BLE_BR_SDP_TEST_ATTR(SERVICE_NAME, ble_br_sdp_test_name),
};

static const uint16_t ble_br_sdp_test_uuids[] = { 0x1101, BLE_BR_SDP_UUID_L2CAP,
                                                  BLE_BR_SDP_UUID_RFCOMM };

static const struct ble_br_sdp_record ble_br_sdp_test_record = {
    .attrs = ble_br_sdp_test_attrs,
    .num_attrs = sizeof ble_br_sdp_test_attrs / sizeof ble_br_sdp_test_attrs[0],
    .uuids = ble_br_sdp_test_uuids,
    .num_uuids = sizeof ble_br_sdp_test_uuids / sizeof ble_br_sdp_test_uuids[0],
};

/* All the attributes of the record above, as the server gives them. */
static const uint8_t ble_br_sdp_test_record_lists[] = {
    0x35, 0x2c, 0x35, 0x2a, 0x09, 0x00, 0x00, 0x0a, 0x00, 0x01, 0x00, 0x00,
    0x09, 0x00, 0x01, 0x35, 0x03, 0x19, 0x11, 0x01, 0x09, 0x00, 0x04, 0x35,
    0x0c, 0x35, 0x03, 0x19, 0x01, 0x00, 0x35, 0x05, 0x19, 0x00, 0x03, 0x08,
    0x05, 0x09, 0x01, 0x00, 0x25, 0x04, 'T',  'e',  's',  't',
};

/* Patterns that select the record above. */
static const uint8_t ble_br_sdp_test_pattern[] = {
    0x35, 0x03, 0x19, 0x11, 0x01,
};

static const uint8_t ble_br_sdp_test_attrs_all[] = {
    0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff,
};

/* The protocols of a peer with a service on RFCOMM channel 3. */
static const uint8_t ble_br_sdp_test_peer_lists[] = {
    0x35, 0x13, 0x35, 0x11, 0x09, 0x00, 0x04, 0x35, 0x0c, 0x35, 0x03,
    0x19, 0x01, 0x00, 0x35, 0x05, 0x19, 0x00, 0x03, 0x08, 0x03,
};

static struct ble_br_test_util_chan ble_br_sdp_test_chan;
static uint16_t ble_br_sdp_test_tid;
static int ble_br_sdp_test_cb_cnt;
static uint16_t ble_br_sdp_test_cb_conn_handle;
static int ble_br_sdp_test_cb_status;
static uint8_t ble_br_sdp_test_cb_channel;
static int ble_br_sdp_test_cb_arg;

static void
ble_br_sdp_test_cb(uint16_t conn_handle, int status, uint8_t channel, void *arg)
{
    TEST_ASSERT(arg == &ble_br_sdp_test_cb_arg);

    ble_br_sdp_test_cb_cnt++;
    ble_br_sdp_test_cb_conn_handle = conn_handle;
    ble_br_sdp_test_cb_status = status;
    ble_br_sdp_test_cb_channel = channel;
}

static void
ble_br_sdp_test_util_init(void)
{
    int rc;

    ble_br_test_util_init();

    ble_br_sdp_test_tid = 0;
    ble_br_sdp_test_cb_cnt = 0;
    ble_br_sdp_test_cb_status = -1;
    ble_br_sdp_test_cb_channel = 0xff;

    rc = ble_br_sdp_register(&ble_br_sdp_test_record);
    TEST_ASSERT_FATAL(rc == 0);

    ble_br_test_util_create_conn(2, ble_br_sdp_test_addr, 0);
}

/**
 * Has the peer open a channel to the server.
 */
static void
ble_br_sdp_test_util_server_connect(void)
{
    ble_br_test_util_l2cap_rx_connect(&ble_br_sdp_test_chan, 2,
                                      BLE_BR_L2CAP_PSM_SDP, 0x0050);
}

static void
ble_br_sdp_test_util_rx_pdu(uint8_t pdu_id, uint16_t tid, const void *params,
                            uint16_t len)
{
    uint8_t buf[BLE_BR_SDP_TEST_HDR_SZ + 256];

    TEST_ASSERT_FATAL(len <= 256);

    buf[0] = pdu_id;
    put_be16(buf + 1, tid);
    put_be16(buf + 3, len);
    memcpy(buf + BLE_BR_SDP_TEST_HDR_SZ, params, len);

    ble_br_test_util_rx_l2cap(2, ble_br_sdp_test_chan.our_cid, buf,
                              BLE_BR_SDP_TEST_HDR_SZ + len);
}

/**
 * Verifies the host sent a PDU, and retrieves its parameters.
 */
static const uint8_t *
ble_br_sdp_test_util_verify_tx_pdu(uint8_t pdu_id, uint16_t tid, uint16_t *out_len)
{
    const uint8_t *pdu;
    uint16_t conn_handle;
    uint16_t cid;
    uint16_t len;

    pdu = ble_br_test_util_tx_dequeue(&conn_handle, &cid, &len);
    TEST_ASSERT_FATAL(pdu != NULL);
    TEST_ASSERT(conn_handle == 2);
    TEST_ASSERT(cid == ble_br_sdp_test_chan.peer_cid);
    TEST_ASSERT_FATAL(len >= BLE_BR_SDP_TEST_HDR_SZ);
    TEST_ASSERT(pdu[0] == pdu_id);
    TEST_ASSERT(get_be16(pdu + 1) == tid);
    TEST_ASSERT(get_be16(pdu + 3) == len - BLE_BR_SDP_TEST_HDR_SZ);

    *out_len = len - BLE_BR_SDP_TEST_HDR_SZ;

    return pdu + BLE_BR_SDP_TEST_HDR_SZ;
}

static void
ble_br_sdp_test_util_verify_tx_error(uint16_t tid, uint16_t code)
{
    const uint8_t *params;
    uint16_t len;

    params = ble_br_sdp_test_util_verify_tx_pdu(BLE_BR_SDP_TEST_PDU_ERROR_RSP,
                                                tid, &len);
    TEST_ASSERT_FATAL(len == 2);
    TEST_ASSERT(get_be16(params) == code);
}

/**
 * Receives a request for attributes; the continuation state, if any, is the
 * offset the server gave.
 */
static void
ble_br_sdp_test_util_rx_attr_req(uint8_t pdu_id, uint16_t tid, const uint8_t *first,
                                 uint8_t first_len, uint16_t max,
                                 const uint8_t *ids, uint8_t ids_len, int offset)
{
    uint8_t params[64];
    int off;

    TEST_ASSERT_FATAL(first_len + 2 + ids_len + 3 <= sizeof params);

    off = 0;
    memcpy(params + off, first, first_len);
    off += first_len;
    put_be16(params + off, max);
    off += 2;
    memcpy(params + off, ids, ids_len);
    off += ids_len;
    if (offset > 0) {
        params[off++] = 2;
        put_be16(params + off, offset);
        off += 2;
    } else {
        params[off++] = 0;
    }

    ble_br_sdp_test_util_rx_pdu(pdu_id, tid, params, off);
}

/**
 * Verifies a response that carries all the attribute bytes asked for.
 */
static void
ble_br_sdp_test_util_verify_tx_attr_rsp(uint8_t pdu_id, uint16_t tid,
                                        const uint8_t *lists, uint16_t lists_len)
{
    const uint8_t *params;
    uint16_t len;

    params = ble_br_sdp_test_util_verify_tx_pdu(pdu_id, tid, &len);
    TEST_ASSERT_FATAL(len == 2 + lists_len + 1);
    TEST_ASSERT(get_be16(params) == lists_len);
    TEST_ASSERT(memcmp(params + 2, lists, lists_len) == 0);
    TEST_ASSERT(params[2 + lists_len] == 0);
}

/**
 * Searches the attributes of the services that match a pattern, and verifies
 * what the server answers.
 */
static void
ble_br_sdp_test_util_search_attr(const uint8_t *pattern, uint8_t pattern_len,
                                 const uint8_t *ids, uint8_t ids_len,
                                 const uint8_t *lists, uint16_t lists_len)
{
    uint16_t tid;

    tid = ++ble_br_sdp_test_tid;
    ble_br_sdp_test_util_rx_attr_req(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_REQ, tid,
                                     pattern, pattern_len, 0xffff, ids, ids_len, 0);
    ble_br_sdp_test_util_verify_tx_attr_rsp(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_RSP,
                                            tid, lists, lists_len);
}

/**
 * Receives a request, and verifies the server answers with an error.
 */
static void
ble_br_sdp_test_util_rx_bad_req(uint8_t pdu_id, const void *params,
                                uint16_t len, uint16_t code)
{
    uint16_t tid;

    tid = ++ble_br_sdp_test_tid;
    ble_br_sdp_test_util_rx_pdu(pdu_id, tid, params, len);
    ble_br_sdp_test_util_verify_tx_error(tid, code);
}

TEST_CASE_SELF(ble_br_sdp_test_server_search_attr)
{
    static const uint8_t pattern_uuid32[] = {
        0x35, 0x05, 0x1a, 0x00, 0x00, 0x11, 0x01,
    };
    static const uint8_t pattern_uuid128[] = {
        0x35, 0x11, 0x1c, 0x00, 0x00, 0x11, 0x01, 0x00, 0x00, 0x10,
        0x00, 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb,
    };
    static const uint8_t pattern_two[] = {
        0x35, 0x06, 0x19, 0x11, 0x01, 0x19, 0x00, 0x03,
    };
    static const uint8_t pattern_two_no_match[] = {
        0x35, 0x06, 0x19, 0x11, 0x01, 0x19, 0x11, 0x1e,
    };
    static const uint8_t pattern_no_match[] = {
        0x35, 0x03, 0x19, 0x12, 0x34,
    };
    static const uint8_t pattern_other_base[] = {
        0x35, 0x11, 0x1c, 0x00, 0x00, 0x11, 0x01, 0x00, 0x00, 0x10,
        0x00, 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfc,
    };
    static const uint8_t pattern_empty[] = { 0x35, 0x00 };
    static const uint8_t ids_protocols[] = { 0x35, 0x03, 0x09, 0x00, 0x04 };
    static const uint8_t ids_two[] = {
        0x35, 0x08, 0x09, 0x01, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t ids_none[] = { 0x35, 0x03, 0x09, 0x03, 0x11 };
    static const uint8_t lists_protocols[] = {
        0x35, 0x13, 0x35, 0x11, 0x09, 0x00, 0x04, 0x35, 0x0c, 0x35, 0x03,
        0x19, 0x01, 0x00, 0x35, 0x05, 0x19, 0x00, 0x03, 0x08, 0x05
    };
    static const uint8_t lists_two[] = {
        0x35, 0x13, 0x35, 0x11, 0x09, 0x00, 0x00, 0x0a, 0x00, 0x01, 0x00,
        0x00, 0x09, 0x01, 0x00, 0x25, 0x04, 'T',  'e',  's',  't',
    };
    static const uint8_t lists_no_attrs[] = { 0x35, 0x02, 0x35, 0x00 };
    static const uint8_t lists_empty[] = { 0x35, 0x00 };

    ble_br_sdp_test_util_init();
    ble_br_sdp_test_util_server_connect();

    /*** All the attributes of the service. */
    ble_br_sdp_test_util_search_attr(
        ble_br_sdp_test_pattern, sizeof ble_br_sdp_test_pattern,
        ble_br_sdp_test_attrs_all, sizeof ble_br_sdp_test_attrs_all,
        ble_br_sdp_test_record_lists, sizeof ble_br_sdp_test_record_lists);

    /*** The UUID of the service in its other forms. */
    ble_br_sdp_test_util_search_attr(
        pattern_uuid32, sizeof pattern_uuid32, ble_br_sdp_test_attrs_all,
        sizeof ble_br_sdp_test_attrs_all, ble_br_sdp_test_record_lists,
        sizeof ble_br_sdp_test_record_lists);
    ble_br_sdp_test_util_search_attr(
        pattern_uuid128, sizeof pattern_uuid128, ble_br_sdp_test_attrs_all,
        sizeof ble_br_sdp_test_attrs_all, ble_br_sdp_test_record_lists,
        sizeof ble_br_sdp_test_record_lists);

    /*** Some attributes only. */
    ble_br_sdp_test_util_search_attr(
        ble_br_sdp_test_pattern, sizeof ble_br_sdp_test_pattern, ids_protocols,
        sizeof ids_protocols, lists_protocols, sizeof lists_protocols);
    ble_br_sdp_test_util_search_attr(ble_br_sdp_test_pattern,
                                     sizeof ble_br_sdp_test_pattern, ids_two,
                                     sizeof ids_two, lists_two, sizeof lists_two);
    ble_br_sdp_test_util_search_attr(
        ble_br_sdp_test_pattern, sizeof ble_br_sdp_test_pattern, ids_none,
        sizeof ids_none, lists_no_attrs, sizeof lists_no_attrs);

    /*** A service has to match all the UUIDs of the pattern. */
    ble_br_sdp_test_util_search_attr(pattern_two, sizeof pattern_two,
                                     ids_protocols, sizeof ids_protocols,
                                     lists_protocols, sizeof lists_protocols);
    ble_br_sdp_test_util_search_attr(pattern_two_no_match, sizeof pattern_two_no_match,
                                     ids_protocols, sizeof ids_protocols,
                                     lists_empty, sizeof lists_empty);

    /*** No service matches. */
    ble_br_sdp_test_util_search_attr(
        pattern_no_match, sizeof pattern_no_match, ble_br_sdp_test_attrs_all,
        sizeof ble_br_sdp_test_attrs_all, lists_empty, sizeof lists_empty);
    ble_br_sdp_test_util_search_attr(
        pattern_other_base, sizeof pattern_other_base, ble_br_sdp_test_attrs_all,
        sizeof ble_br_sdp_test_attrs_all, lists_empty, sizeof lists_empty);
    ble_br_sdp_test_util_search_attr(
        pattern_empty, sizeof pattern_empty, ble_br_sdp_test_attrs_all,
        sizeof ble_br_sdp_test_attrs_all, lists_empty, sizeof lists_empty);

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_sdp_test_server_cont)
{
    static const uint8_t bad_cont_len[] = {
        0x35, 0x03, 0x19, 0x11, 0x01, 0xff, 0xff, 0x35,
        0x05, 0x0a, 0x00, 0x00, 0xff, 0xff, 0x01, 0x14,
    };
    static const uint8_t no_cont[] = {
        0x35, 0x03, 0x19, 0x11, 0x01, 0xff, 0xff,
        0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff,
    };
    uint8_t lists[sizeof ble_br_sdp_test_record_lists];
    const uint8_t *params;
    uint16_t count;
    uint16_t tid;
    uint16_t len;
    int rounds;
    int off;

    ble_br_sdp_test_util_init();
    ble_br_sdp_test_util_server_connect();

    /*** The attributes, 20 bytes at a time. */
    off = 0;
    rounds = 0;
    do {
        tid = ++ble_br_sdp_test_tid;
        ble_br_sdp_test_util_rx_attr_req(
            BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_REQ, tid, ble_br_sdp_test_pattern,
            sizeof ble_br_sdp_test_pattern, 20, ble_br_sdp_test_attrs_all,
            sizeof ble_br_sdp_test_attrs_all, off);

        params = ble_br_sdp_test_util_verify_tx_pdu(
            BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_RSP, tid, &len);
        TEST_ASSERT_FATAL(len >= 3);

        count = get_be16(params);
        TEST_ASSERT_FATAL(count > 0 && count <= 20);
        TEST_ASSERT_FATAL(off + count <= sizeof lists);
        memcpy(lists + off, params + 2, count);
        off += count;

        /* The continuation state is the offset to go on from. */
        if (off < sizeof lists) {
            TEST_ASSERT_FATAL(len == 2 + count + 3);
            TEST_ASSERT(params[2 + count] == 2);
            TEST_ASSERT(get_be16(params + 2 + count + 1) == off);
        } else {
            TEST_ASSERT_FATAL(len == 2 + count + 1);
            TEST_ASSERT(params[2 + count] == 0);
        }

        rounds++;
    } while (off < sizeof lists);

    TEST_ASSERT(rounds == 3);
    TEST_ASSERT(memcmp(lists, ble_br_sdp_test_record_lists, sizeof lists) == 0);

    /*** A continuation state the server did not give. */
    tid = ++ble_br_sdp_test_tid;
    ble_br_sdp_test_util_rx_attr_req(
        BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_REQ, tid, ble_br_sdp_test_pattern,
        sizeof ble_br_sdp_test_pattern, 20, ble_br_sdp_test_attrs_all,
        sizeof ble_br_sdp_test_attrs_all, sizeof lists);
    ble_br_sdp_test_util_verify_tx_error(tid, BLE_BR_SDP_TEST_ERR_INVALID_CONT);

    ble_br_sdp_test_util_rx_bad_req(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_REQ,
                                    bad_cont_len, sizeof bad_cont_len,
                                    BLE_BR_SDP_TEST_ERR_INVALID_CONT);
    ble_br_sdp_test_util_rx_bad_req(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_REQ,
                                    no_cont, sizeof no_cont,
                                    BLE_BR_SDP_TEST_ERR_INVALID_CONT);

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_sdp_test_server_search)
{
    static const uint8_t search[] = {
        0x35, 0x03, 0x19, 0x11, 0x01, 0x00, 0x10, 0x00,
    };
    static const uint8_t search_rsp[] = {
        0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
    };
    static const uint8_t search_no_match[] = {
        0x35, 0x03, 0x19, 0x12, 0x34, 0x00, 0x10, 0x00,
    };
    static const uint8_t search_max_0[] = {
        0x35, 0x03, 0x19, 0x11, 0x01, 0x00, 0x00, 0x00,
    };
    static const uint8_t search_rsp_none[] = {
        0x00, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t handle[] = { 0x00, 0x01, 0x00, 0x00 };
    static const uint8_t handle_unknown[] = { 0x00, 0x01, 0x00, 0x01 };
    static const uint8_t handle_reserved[] = { 0x00, 0x00, 0x00, 0x00 };
    const uint8_t *params;
    uint16_t tid;
    uint16_t len;

    ble_br_sdp_test_util_init();
    ble_br_sdp_test_util_server_connect();

    /*** Search for the service: its record handle. */
    tid = ++ble_br_sdp_test_tid;
    ble_br_sdp_test_util_rx_pdu(BLE_BR_SDP_TEST_PDU_SEARCH_REQ, tid, search,
                                sizeof search);
    params = ble_br_sdp_test_util_verify_tx_pdu(BLE_BR_SDP_TEST_PDU_SEARCH_RSP,
                                                tid, &len);
    TEST_ASSERT_FATAL(len == sizeof search_rsp);
    TEST_ASSERT(memcmp(params, search_rsp, len) == 0);

    /*** No service matches, or no handle is asked for. */
    tid = ++ble_br_sdp_test_tid;
    ble_br_sdp_test_util_rx_pdu(BLE_BR_SDP_TEST_PDU_SEARCH_REQ, tid,
                                search_no_match, sizeof search_no_match);
    params = ble_br_sdp_test_util_verify_tx_pdu(BLE_BR_SDP_TEST_PDU_SEARCH_RSP,
                                                tid, &len);
    TEST_ASSERT_FATAL(len == sizeof search_rsp_none);
    TEST_ASSERT(memcmp(params, search_rsp_none, len) == 0);

    tid = ++ble_br_sdp_test_tid;
    ble_br_sdp_test_util_rx_pdu(BLE_BR_SDP_TEST_PDU_SEARCH_REQ, tid,
                                search_max_0, sizeof search_max_0);
    params = ble_br_sdp_test_util_verify_tx_pdu(BLE_BR_SDP_TEST_PDU_SEARCH_RSP,
                                                tid, &len);
    TEST_ASSERT_FATAL(len == sizeof search_rsp_none);
    TEST_ASSERT(memcmp(params, search_rsp_none, len) == 0);

    /*** The attributes of the record with that handle. */
    tid = ++ble_br_sdp_test_tid;
    ble_br_sdp_test_util_rx_attr_req(BLE_BR_SDP_TEST_PDU_ATTR_REQ, tid, handle,
                                     sizeof handle, 0xffff, ble_br_sdp_test_attrs_all,
                                     sizeof ble_br_sdp_test_attrs_all, 0);

    /* A list of attributes, not a list of them per record. */
    ble_br_sdp_test_util_verify_tx_attr_rsp(
        BLE_BR_SDP_TEST_PDU_ATTR_RSP, tid, ble_br_sdp_test_record_lists + 2,
        sizeof ble_br_sdp_test_record_lists - 2);

    /*** Handles of no record. */
    tid = ++ble_br_sdp_test_tid;
    ble_br_sdp_test_util_rx_attr_req(
        BLE_BR_SDP_TEST_PDU_ATTR_REQ, tid, handle_unknown, sizeof handle_unknown,
        0xffff, ble_br_sdp_test_attrs_all, sizeof ble_br_sdp_test_attrs_all, 0);
    ble_br_sdp_test_util_verify_tx_error(tid, BLE_BR_SDP_TEST_ERR_INVALID_HANDLE);

    tid = ++ble_br_sdp_test_tid;
    ble_br_sdp_test_util_rx_attr_req(BLE_BR_SDP_TEST_PDU_ATTR_REQ, tid,
                                     handle_reserved, sizeof handle_reserved,
                                     0xffff, ble_br_sdp_test_attrs_all,
                                     sizeof ble_br_sdp_test_attrs_all, 0);
    ble_br_sdp_test_util_verify_tx_error(tid, BLE_BR_SDP_TEST_ERR_INVALID_HANDLE);

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_sdp_test_server_malformed)
{
    /* The pattern claims more bytes than the PDU has. */
    static const uint8_t pattern_truncated[] = {
        0x35, 0x20, 0x19, 0x11, 0x01, 0xff, 0xff, 0x35,
        0x05, 0x0a, 0x00, 0x00, 0xff, 0xff, 0x00,
    };
    /* So it does, with a size of 16 and of 32 bits. */
    static const uint8_t pattern_truncated_16[] = {
        0x36, 0xff, 0xff, 0x19, 0x11, 0x01, 0xff, 0xff,
        0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff, 0x00,
    };
    static const uint8_t pattern_truncated_32[] = {
        0x37, 0xff, 0xff, 0xff, 0xff, 0x19, 0x11, 0x01, 0xff,
        0xff, 0x35, 0x05, 0x0a, 0x00, 0x00, 0xff, 0xff, 0x00,
    };
    /* The size of the pattern itself is cut short. */
    static const uint8_t pattern_no_size[] = { 0x37, 0x00, 0x00 };
    static const uint8_t pattern_not_seq[] = {
        0x19, 0x11, 0x01, 0xff, 0xff, 0x35, 0x05,
        0x0a, 0x00, 0x00, 0xff, 0xff, 0x00,
    };
    static const uint8_t no_max[] = { 0x35, 0x03, 0x19, 0x11, 0x01, 0xff };
    static const uint8_t ids_truncated[] = {
        0x35, 0x03, 0x19, 0x11, 0x01, 0xff, 0xff, 0x35, 0x05, 0x0a, 0x00, 0x00,
    };
    static const uint8_t ids_not_seq[] = {
        0x35, 0x03, 0x19, 0x11, 0x01, 0xff, 0xff,
        0x0a, 0x00, 0x00, 0xff, 0xff, 0x00,
    };
    static const uint8_t no_ids[] = {
        0x35, 0x03, 0x19, 0x11, 0x01, 0xff, 0xff,
    };
    static const uint8_t attr_no_handle[] = { 0x00, 0x01, 0x00 };
    static const uint8_t search_extra[] = {
        0x35, 0x03, 0x19, 0x11, 0x01, 0x00, 0x10, 0x00, 0x00,
    };
    static const uint8_t search_no_max[] = {
        0x35, 0x03, 0x19, 0x11, 0x01,
    };
    static const uint8_t search_cont[] = {
        0x35, 0x03, 0x19, 0x11, 0x01, 0x00, 0x10, 0x01,
    };
    /* The UUID in the pattern claims more bytes than the pattern has. */
    static const uint8_t uuid_truncated[] = {
        0x35, 0x03, 0x1a, 0x11, 0x01, 0xff, 0xff, 0x35,
        0x05, 0x0a, 0x00, 0x00, 0xff, 0xff, 0x00,
    };
    /* The ID in the list claims more bytes than the list has. */
    static const uint8_t id_truncated[] = {
        0x35, 0x03, 0x19, 0x11, 0x01, 0xff, 0xff,
        0x35, 0x03, 0x0a, 0x00, 0x00, 0x00,
    };
    static const uint8_t lists_empty[] = { 0x35, 0x00 };
    static const uint8_t lists_no_attrs[] = { 0x35, 0x02, 0x35, 0x00 };
    uint8_t buf[200];
    uint16_t tid;

    ble_br_sdp_test_util_init();
    ble_br_sdp_test_util_server_connect();

    /*** PDUs the server can not make sense of at all. */
    memset(buf, 0, sizeof buf);
    buf[0] = BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_REQ;

    /* Shorter than a header. */
    ble_br_test_util_rx_l2cap(2, ble_br_sdp_test_chan.our_cid, buf, 4);
    ble_br_test_util_verify_tx_none();

    /* Longer than a request can be. */
    put_be16(buf + 3, sizeof buf - BLE_BR_SDP_TEST_HDR_SZ);
    ble_br_test_util_rx_l2cap(2, ble_br_sdp_test_chan.our_cid, buf, sizeof buf);
    ble_br_test_util_verify_tx_none();

    /*** A parameter length that is not the one of the PDU. */
    buf[0] = BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_REQ;
    put_be16(buf + 1, 0x1234);
    put_be16(buf + 3, 20);
    ble_br_test_util_rx_l2cap(2, ble_br_sdp_test_chan.our_cid, buf,
                              BLE_BR_SDP_TEST_HDR_SZ + 10);
    ble_br_sdp_test_util_verify_tx_error(0x1234, BLE_BR_SDP_TEST_ERR_INVALID_SYNTAX);

    put_be16(buf + 3, 5);
    ble_br_test_util_rx_l2cap(2, ble_br_sdp_test_chan.our_cid, buf,
                              BLE_BR_SDP_TEST_HDR_SZ + 10);
    ble_br_sdp_test_util_verify_tx_error(0x1234, BLE_BR_SDP_TEST_ERR_INVALID_SYNTAX);

    /*** PDUs that are not requests. */
    ble_br_sdp_test_util_rx_bad_req(0x08, NULL, 0, BLE_BR_SDP_TEST_ERR_INVALID_SYNTAX);
    ble_br_sdp_test_util_rx_bad_req(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_RSP, buf,
                                    3, BLE_BR_SDP_TEST_ERR_INVALID_SYNTAX);

    /*** Requests for attributes that are cut short or have the wrong data
     * elements.
     */
    ble_br_sdp_test_util_rx_bad_req(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_REQ, NULL,
                                    0, BLE_BR_SDP_TEST_ERR_INVALID_SYNTAX);
    ble_br_sdp_test_util_rx_bad_req(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_REQ,
                                    pattern_truncated, sizeof pattern_truncated,
                                    BLE_BR_SDP_TEST_ERR_INVALID_SYNTAX);
    ble_br_sdp_test_util_rx_bad_req(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_REQ,
                                    pattern_truncated_16, sizeof pattern_truncated_16,
                                    BLE_BR_SDP_TEST_ERR_INVALID_SYNTAX);
    ble_br_sdp_test_util_rx_bad_req(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_REQ,
                                    pattern_truncated_32, sizeof pattern_truncated_32,
                                    BLE_BR_SDP_TEST_ERR_INVALID_SYNTAX);
    ble_br_sdp_test_util_rx_bad_req(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_REQ,
                                    pattern_no_size, sizeof pattern_no_size,
                                    BLE_BR_SDP_TEST_ERR_INVALID_SYNTAX);
    ble_br_sdp_test_util_rx_bad_req(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_REQ,
                                    pattern_not_seq, sizeof pattern_not_seq,
                                    BLE_BR_SDP_TEST_ERR_INVALID_SYNTAX);
    ble_br_sdp_test_util_rx_bad_req(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_REQ,
                                    no_max, sizeof no_max,
                                    BLE_BR_SDP_TEST_ERR_INVALID_SYNTAX);
    ble_br_sdp_test_util_rx_bad_req(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_REQ,
                                    no_ids, sizeof no_ids,
                                    BLE_BR_SDP_TEST_ERR_INVALID_SYNTAX);
    ble_br_sdp_test_util_rx_bad_req(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_REQ,
                                    ids_truncated, sizeof ids_truncated,
                                    BLE_BR_SDP_TEST_ERR_INVALID_SYNTAX);
    ble_br_sdp_test_util_rx_bad_req(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_REQ,
                                    ids_not_seq, sizeof ids_not_seq,
                                    BLE_BR_SDP_TEST_ERR_INVALID_SYNTAX);
    ble_br_sdp_test_util_rx_bad_req(BLE_BR_SDP_TEST_PDU_ATTR_REQ,
                                    attr_no_handle, sizeof attr_no_handle,
                                    BLE_BR_SDP_TEST_ERR_INVALID_SYNTAX);

    /*** Searches that are cut short or have something left. */
    ble_br_sdp_test_util_rx_bad_req(BLE_BR_SDP_TEST_PDU_SEARCH_REQ,
                                    search_extra, sizeof search_extra,
                                    BLE_BR_SDP_TEST_ERR_INVALID_SYNTAX);
    ble_br_sdp_test_util_rx_bad_req(BLE_BR_SDP_TEST_PDU_SEARCH_REQ,
                                    search_no_max, sizeof search_no_max,
                                    BLE_BR_SDP_TEST_ERR_INVALID_SYNTAX);
    ble_br_sdp_test_util_rx_bad_req(BLE_BR_SDP_TEST_PDU_SEARCH_REQ,
                                    search_cont, sizeof search_cont,
                                    BLE_BR_SDP_TEST_ERR_INVALID_SYNTAX);
    ble_br_sdp_test_util_rx_bad_req(BLE_BR_SDP_TEST_PDU_SEARCH_REQ,
                                    pattern_truncated, sizeof pattern_truncated,
                                    BLE_BR_SDP_TEST_ERR_INVALID_SYNTAX);

    /*** Elements that are cut short inside a well formed list match
     * nothing.
     */
    tid = ++ble_br_sdp_test_tid;
    ble_br_sdp_test_util_rx_pdu(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_REQ, tid,
                                uuid_truncated, sizeof uuid_truncated);
    ble_br_sdp_test_util_verify_tx_attr_rsp(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_RSP,
                                            tid, lists_empty, sizeof lists_empty);

    tid = ++ble_br_sdp_test_tid;
    ble_br_sdp_test_util_rx_pdu(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_REQ, tid,
                                id_truncated, sizeof id_truncated);
    ble_br_sdp_test_util_verify_tx_attr_rsp(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_RSP, tid,
                                            lists_no_attrs, sizeof lists_no_attrs);

    /*** The server still answers. */
    ble_br_sdp_test_util_search_attr(
        ble_br_sdp_test_pattern, sizeof ble_br_sdp_test_pattern,
        ble_br_sdp_test_attrs_all, sizeof ble_br_sdp_test_attrs_all,
        ble_br_sdp_test_record_lists, sizeof ble_br_sdp_test_record_lists);

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

#if MYNEWT_VAL(BLE_BREDR_HFP_HF)
static void
ble_br_sdp_test_hfp_hf_cb(const struct ble_br_hfp_hf_event *event, void *arg)
{
}

TEST_CASE_SELF(ble_br_sdp_test_server_hfp_hf)
{
    static const uint8_t pattern[] = { 0x35, 0x03, 0x19, 0x11, 0x1e };
    static const uint8_t pattern_l2cap[] = { 0x35, 0x03, 0x19, 0x01, 0x00 };
    static const uint8_t ids_protocols[] = { 0x35, 0x03, 0x09, 0x00, 0x04 };
    /* The Hands-Free record, the second one of the server: record handle,
     * service classes (Hands-Free and Generic Audio), protocols (L2CAP, and
     * RFCOMM with its server channel), browse group (the public root),
     * profile (Hands-Free 1.7), service name and supported features (CLI
     * presentation and remote volume control).
     */
    static const uint8_t lists[] = {
        0x35, 0x50, 0x35, 0x4e, 0x09, 0x00, 0x00, 0x0a, 0x00, 0x01, 0x00, 0x01,
        0x09, 0x00, 0x01, 0x35, 0x06, 0x19, 0x11, 0x1e, 0x19, 0x12, 0x03, 0x09,
        0x00, 0x04, 0x35, 0x0c, 0x35, 0x03, 0x19, 0x01, 0x00, 0x35, 0x05, 0x19,
        0x00, 0x03, 0x08, 0x01, 0x09, 0x00, 0x05, 0x35, 0x03, 0x19, 0x10, 0x02,
        0x09, 0x00, 0x09, 0x35, 0x08, 0x35, 0x06, 0x19, 0x11, 0x1e, 0x09, 0x01,
        0x07, 0x09, 0x01, 0x00, 0x25, 0x0a, 'H',  'a',  'n',  'd',  's',  '-',
        'F',  'r',  'e',  'e',  0x09, 0x03, 0x11, 0x09, 0x00, 0x14
    };
    static const uint8_t lists_protocols[] = {
        0x35, 0x13, 0x35, 0x11, 0x09, 0x00, 0x04, 0x35, 0x0c, 0x35, 0x03,
        0x19, 0x01, 0x00, 0x35, 0x05, 0x19, 0x00, 0x03, 0x08, 0x01
    };
    /* The protocols of the two records of the server. */
    static const uint8_t lists_both[] = {
        0x35, 0x26, 0x35, 0x11, 0x09, 0x00, 0x04, 0x35, 0x0c, 0x35,
        0x03, 0x19, 0x01, 0x00, 0x35, 0x05, 0x19, 0x00, 0x03, 0x08,
        0x05, 0x35, 0x11, 0x09, 0x00, 0x04, 0x35, 0x0c, 0x35, 0x03,
        0x19, 0x01, 0x00, 0x35, 0x05, 0x19, 0x00, 0x03, 0x08, 0x01
    };
    int rc;

    /* The record is expected to tell the default server channel. */
    TEST_ASSERT_FATAL(MYNEWT_VAL(BLE_BREDR_HFP_HF_RFCOMM_CHANNEL) == 1);

    ble_br_sdp_test_util_init();

    ble_hs_test_util_hci_ack_set(
        ble_hs_hci_util_opcode_join(BLE_HCI_OGF_CTLR_BASEBAND, BLE_HCI_OCF_CB_WR_EIR),
        0);
    rc = ble_br_hfp_hf_init(ble_br_sdp_test_hfp_hf_cb, NULL);
    TEST_ASSERT_FATAL(rc == 0);
    ble_hs_test_util_hci_out_clear();

    /* The server has no room for another record. */
    rc = ble_br_sdp_register(&ble_br_sdp_test_record);
    TEST_ASSERT(rc == BLE_HS_ENOMEM);

    ble_br_sdp_test_util_server_connect();

    ble_br_sdp_test_util_search_attr(
        pattern, sizeof pattern, ble_br_sdp_test_attrs_all,
        sizeof ble_br_sdp_test_attrs_all, lists, sizeof lists);
    ble_br_sdp_test_util_search_attr(pattern, sizeof pattern, ids_protocols,
                                     sizeof ids_protocols, lists_protocols,
                                     sizeof lists_protocols);
    ble_br_sdp_test_util_search_attr(pattern_l2cap, sizeof pattern_l2cap,
                                     ids_protocols, sizeof ids_protocols,
                                     lists_both, sizeof lists_both);

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}
#endif

/**
 * Starts a search for the RFCOMM channel of a service of the peer, which
 * lets the client connect to its server.
 */
static void
ble_br_sdp_test_util_client_start(void)
{
    int rc;

    rc = ble_br_sdp_find_rfcomm(2, BLE_BR_SDP_UUID_HANDSFREE_AG,
                                ble_br_sdp_test_cb, &ble_br_sdp_test_cb_arg);
    TEST_ASSERT_FATAL(rc == 0);

    ble_br_test_util_l2cap_rx_connect_rsp(&ble_br_sdp_test_chan, 2,
                                          BLE_BR_L2CAP_PSM_SDP, 0x0060);
}

/**
 * Verifies the client asked for the protocols of the service.
 *
 * @param received              The attribute bytes the client has so far.
 */
static void
ble_br_sdp_test_util_verify_tx_client_req(uint16_t received,
                                          const uint8_t *cont, uint8_t cont_len)
{
    const uint8_t *params;
    uint8_t exp[13] = {
        0x35, 0x03, 0x19, 0x11, 0x1f, 0x00, 0x00, 0x35, 0x03, 0x09, 0x00, 0x04,
    };
    uint16_t len;

    put_be16(exp + 5, BLE_BR_SDP_TEST_CLIENT_BUF_SZ - received);
    exp[12] = cont_len;

    params = ble_br_sdp_test_util_verify_tx_pdu(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_REQ,
                                                ++ble_br_sdp_test_tid, &len);
    TEST_ASSERT_FATAL(len == sizeof exp + cont_len);
    TEST_ASSERT(memcmp(params, exp, sizeof exp) == 0);
    TEST_ASSERT(memcmp(params + sizeof exp, cont, cont_len) == 0);
}

/**
 * Receives the response to the last request of the client.
 */
static void
ble_br_sdp_test_util_rx_client_rsp(const uint8_t *lists, uint16_t lists_len,
                                   const uint8_t *cont, uint8_t cont_len)
{
    uint8_t params[256];

    TEST_ASSERT_FATAL(2 + lists_len + 1 + cont_len <= sizeof params);

    put_be16(params, lists_len);
    memcpy(params + 2, lists, lists_len);
    params[2 + lists_len] = cont_len;
    memcpy(params + 2 + lists_len + 1, cont, cont_len);

    ble_br_sdp_test_util_rx_pdu(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_RSP, ble_br_sdp_test_tid,
                                params, 2 + lists_len + 1 + cont_len);
}

/**
 * Verifies the search ended: the application is told, and the channel to
 * the server of the peer is closed.
 */
static void
ble_br_sdp_test_util_verify_client_done(int cb_cnt, int status, uint8_t channel)
{
    TEST_ASSERT(ble_br_sdp_test_cb_cnt == cb_cnt);
    TEST_ASSERT(ble_br_sdp_test_cb_conn_handle == 2);
    TEST_ASSERT(ble_br_sdp_test_cb_status == status);
    TEST_ASSERT(ble_br_sdp_test_cb_channel == channel);

    ble_br_test_util_l2cap_verify_tx_disconnect(&ble_br_sdp_test_chan);
    ble_br_test_util_verify_tx_none();
}

/**
 * Runs a search the peer answers with the specified attribute lists.
 */
static void
ble_br_sdp_test_util_client_search(const uint8_t *lists, uint16_t lists_len,
                                   int exp_status, uint8_t exp_channel)
{
    int cb_cnt;

    cb_cnt = ble_br_sdp_test_cb_cnt;

    ble_br_sdp_test_util_client_start();
    ble_br_sdp_test_util_verify_tx_client_req(0, NULL, 0);
    TEST_ASSERT(ble_br_sdp_test_cb_cnt == cb_cnt);

    ble_br_sdp_test_util_rx_client_rsp(lists, lists_len, NULL, 0);
    ble_br_sdp_test_util_verify_client_done(cb_cnt + 1, exp_status, exp_channel);
}

TEST_CASE_SELF(ble_br_sdp_test_client)
{
    /* The RFCOMM UUID with 128 bits, in the second record. */
    static const uint8_t lists_uuid128[] = {
        0x35, 0x32, 0x35, 0x0a, 0x09, 0x00, 0x04, 0x35, 0x05, 0x35, 0x03,
        0x19, 0x01, 0x00, 0x35, 0x24, 0x09, 0x00, 0x09, 0x35, 0x00, 0x09,
        0x00, 0x04, 0x35, 0x1a, 0x35, 0x03, 0x19, 0x01, 0x00, 0x35, 0x13,
        0x1c, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
        0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb, 0x08, 0x07,
    };
    static const uint8_t lists_empty[] = { 0x35, 0x00 };
    static const uint8_t lists_no_rfcomm[] = {
        0x35, 0x0c, 0x35, 0x0a, 0x09, 0x00, 0x04,
        0x35, 0x05, 0x35, 0x03, 0x19, 0x01, 0x00,
    };
    /* The channel is not a byte. */
    static const uint8_t lists_bad_channel[] = {
        0x35, 0x14, 0x35, 0x12, 0x09, 0x00, 0x04, 0x35, 0x0d, 0x35, 0x03,
        0x19, 0x01, 0x00, 0x35, 0x06, 0x19, 0x00, 0x03, 0x09, 0x00, 0x03,
    };
    int rc;

    ble_br_sdp_test_util_init();

    rc = ble_br_sdp_find_rfcomm(3, BLE_BR_SDP_UUID_HANDSFREE_AG,
                                ble_br_sdp_test_cb, &ble_br_sdp_test_cb_arg);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);

    /*** The peer has the service. */
    ble_br_sdp_test_util_client_start();
    ble_br_sdp_test_util_verify_tx_client_req(0, NULL, 0);
    TEST_ASSERT(ble_br_sdp_test_cb_cnt == 0);

    /* One search at a time. */
    rc = ble_br_sdp_find_rfcomm(2, BLE_BR_SDP_UUID_HANDSFREE_AG,
                                ble_br_sdp_test_cb, &ble_br_sdp_test_cb_arg);
    TEST_ASSERT(rc == BLE_HS_EBUSY);

    ble_br_sdp_test_util_rx_client_rsp(ble_br_sdp_test_peer_lists,
                                       sizeof ble_br_sdp_test_peer_lists, NULL, 0);
    ble_br_sdp_test_util_verify_client_done(1, 0, 3);

    ble_br_sdp_test_util_client_search(lists_uuid128, sizeof lists_uuid128, 0, 7);

    /*** The peer does not have the service, or it is not on RFCOMM. */
    ble_br_sdp_test_util_client_search(lists_empty, sizeof lists_empty, 0, 0);
    ble_br_sdp_test_util_client_search(lists_no_rfcomm, sizeof lists_no_rfcomm, 0, 0);
    ble_br_sdp_test_util_client_search(lists_bad_channel,
                                       sizeof lists_bad_channel, 0, 0);

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_sdp_test_client_cont)
{
    static const uint8_t cont1[] = { 0xaa, 0xbb };
    static const uint8_t cont2[] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    };
    static const uint8_t cont_too_long[17];
    static const uint8_t zeros[BLE_BR_SDP_TEST_CLIENT_BUF_SZ + 1];
    int i;

    ble_br_sdp_test_util_init();

    /*** The attributes come in three responses. */
    ble_br_sdp_test_util_client_start();
    ble_br_sdp_test_util_verify_tx_client_req(0, NULL, 0);

    ble_br_sdp_test_util_rx_client_rsp(ble_br_sdp_test_peer_lists, 5, cont1,
                                       sizeof cont1);
    ble_br_sdp_test_util_verify_tx_client_req(5, cont1, sizeof cont1);

    ble_br_sdp_test_util_rx_client_rsp(ble_br_sdp_test_peer_lists + 5, 10,
                                       cont2, sizeof cont2);
    ble_br_sdp_test_util_verify_tx_client_req(15, cont2, sizeof cont2);
    TEST_ASSERT(ble_br_sdp_test_cb_cnt == 0);

    ble_br_sdp_test_util_rx_client_rsp(ble_br_sdp_test_peer_lists + 15,
                                       sizeof ble_br_sdp_test_peer_lists - 15,
                                       NULL, 0);
    ble_br_sdp_test_util_verify_client_done(1, 0, 3);

    /*** A continuation state longer than one can be. */
    ble_br_sdp_test_util_client_start();
    ble_br_sdp_test_util_verify_tx_client_req(0, NULL, 0);

    ble_br_sdp_test_util_rx_client_rsp(ble_br_sdp_test_peer_lists, 5,
                                       cont_too_long, sizeof cont_too_long);
    ble_br_sdp_test_util_verify_client_done(2, BLE_HS_EBADDATA, 0);

    /*** More attribute bytes than the client asked for. */
    ble_br_sdp_test_util_client_start();
    ble_br_sdp_test_util_verify_tx_client_req(0, NULL, 0);

    ble_br_sdp_test_util_rx_client_rsp(zeros, 100, cont1, sizeof cont1);
    ble_br_sdp_test_util_verify_tx_client_req(100, cont1, sizeof cont1);

    ble_br_sdp_test_util_rx_client_rsp(zeros, 61, NULL, 0);
    ble_br_sdp_test_util_verify_client_done(3, BLE_HS_EBADDATA, 0);

    ble_br_sdp_test_util_client_start();
    ble_br_sdp_test_util_verify_tx_client_req(0, NULL, 0);

    ble_br_sdp_test_util_rx_client_rsp(zeros, sizeof zeros, NULL, 0);
    ble_br_sdp_test_util_verify_client_done(4, BLE_HS_EBADDATA, 0);

    /*** A peer that never comes to an end. */
    ble_br_sdp_test_util_client_start();
    ble_br_sdp_test_util_verify_tx_client_req(0, NULL, 0);

    for (i = 1; i < 8; i++) {
        ble_br_sdp_test_util_rx_client_rsp(zeros, 1, cont1, sizeof cont1);
        ble_br_sdp_test_util_verify_tx_client_req(i, cont1, sizeof cont1);
        TEST_ASSERT(ble_br_sdp_test_cb_cnt == 4);
    }

    ble_br_sdp_test_util_rx_client_rsp(zeros, 1, cont1, sizeof cont1);
    ble_br_sdp_test_util_verify_client_done(5, BLE_HS_EBADDATA, 0);

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_sdp_test_client_malformed)
{
    /* The lists claim more bytes than the response has. */
    static const uint8_t lists_truncated[] = {
        0x35, 0x40, 0x35, 0x11, 0x09, 0x00, 0x04, 0x35, 0x0c, 0x35, 0x03,
        0x19, 0x01, 0x00, 0x35, 0x05, 0x19, 0x00, 0x03, 0x08, 0x03,
    };
    /* The record claims more bytes than the lists have. */
    static const uint8_t record_truncated[] = {
        0x35, 0x13, 0x35, 0x40, 0x09, 0x00, 0x04, 0x35, 0x0c, 0x35, 0x03,
        0x19, 0x01, 0x00, 0x35, 0x05, 0x19, 0x00, 0x03, 0x08, 0x03,
    };
    /* The protocols claim more bytes than the record has. */
    static const uint8_t protocols_truncated[] = {
        0x35, 0x13, 0x35, 0x11, 0x09, 0x00, 0x04, 0x35, 0x40, 0x35, 0x03,
        0x19, 0x01, 0x00, 0x35, 0x05, 0x19, 0x00, 0x03, 0x08, 0x03,
    };
    /* The protocol claims more bytes than the protocols have. */
    static const uint8_t protocol_truncated[] = {
        0x35, 0x13, 0x35, 0x11, 0x09, 0x00, 0x04, 0x35, 0x0c, 0x35, 0x03,
        0x19, 0x01, 0x00, 0x35, 0x40, 0x19, 0x00, 0x03, 0x08, 0x03,
    };
    /* The channel is not there. */
    static const uint8_t channel_truncated[] = {
        0x35, 0x12, 0x35, 0x10, 0x09, 0x00, 0x04, 0x35, 0x0b, 0x35,
        0x03, 0x19, 0x01, 0x00, 0x35, 0x04, 0x19, 0x00, 0x03, 0x08,
    };
    /* An attribute without a value. */
    static const uint8_t no_value[] = {
        0x35, 0x05, 0x35, 0x03, 0x09, 0x00, 0x04,
    };
    static const uint8_t not_seq[] = { 0x08, 0x03 };
    static const uint8_t error_rsp[] = { 0x00, 0x03 };
    uint8_t params[2 + sizeof ble_br_sdp_test_peer_lists + 1];

    ble_br_sdp_test_util_init();

    /*** Attributes that are cut short: the channel is not found. */
    ble_br_sdp_test_util_client_search(lists_truncated, sizeof lists_truncated, 0, 0);
    ble_br_sdp_test_util_client_search(record_truncated,
                                       sizeof record_truncated, 0, 0);
    ble_br_sdp_test_util_client_search(protocols_truncated,
                                       sizeof protocols_truncated, 0, 0);
    ble_br_sdp_test_util_client_search(protocol_truncated,
                                       sizeof protocol_truncated, 0, 0);
    ble_br_sdp_test_util_client_search(channel_truncated,
                                       sizeof channel_truncated, 0, 0);
    ble_br_sdp_test_util_client_search(no_value, sizeof no_value, 0, 0);
    ble_br_sdp_test_util_client_search(not_seq, sizeof not_seq, 0, 0);
    ble_br_sdp_test_util_client_search(NULL, 0, 0, 0);

    /*** The response has less attribute bytes than it says. */
    put_be16(params, sizeof ble_br_sdp_test_peer_lists + 1);
    memcpy(params + 2, ble_br_sdp_test_peer_lists, sizeof ble_br_sdp_test_peer_lists);
    params[2 + sizeof ble_br_sdp_test_peer_lists] = 0;

    ble_br_sdp_test_util_client_start();
    ble_br_sdp_test_util_verify_tx_client_req(0, NULL, 0);
    ble_br_sdp_test_util_rx_pdu(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_RSP,
                                ble_br_sdp_test_tid, params, sizeof params);
    ble_br_sdp_test_util_verify_client_done(9, BLE_HS_EBADDATA, 0);

    /*** The response has more bytes than it says. */
    put_be16(params, sizeof ble_br_sdp_test_peer_lists - 1);

    ble_br_sdp_test_util_client_start();
    ble_br_sdp_test_util_verify_tx_client_req(0, NULL, 0);
    ble_br_sdp_test_util_rx_pdu(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_RSP,
                                ble_br_sdp_test_tid, params, sizeof params);
    ble_br_sdp_test_util_verify_client_done(10, BLE_HS_EBADDATA, 0);

    /*** The response is too short to have attribute bytes at all. */
    ble_br_sdp_test_util_client_start();
    ble_br_sdp_test_util_verify_tx_client_req(0, NULL, 0);
    ble_br_sdp_test_util_rx_pdu(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_RSP,
                                ble_br_sdp_test_tid, params, 2);
    ble_br_sdp_test_util_verify_client_done(11, BLE_HS_EBADDATA, 0);

    ble_br_sdp_test_util_client_start();
    ble_br_sdp_test_util_verify_tx_client_req(0, NULL, 0);
    ble_br_test_util_rx_l2cap(2, ble_br_sdp_test_chan.our_cid, params, 4);
    ble_br_sdp_test_util_verify_client_done(12, BLE_HS_EBADDATA, 0);

    /*** The server of the peer answers with an error. */
    ble_br_sdp_test_util_client_start();
    ble_br_sdp_test_util_verify_tx_client_req(0, NULL, 0);
    ble_br_sdp_test_util_rx_pdu(BLE_BR_SDP_TEST_PDU_ERROR_RSP,
                                ble_br_sdp_test_tid, error_rsp, sizeof error_rsp);
    ble_br_sdp_test_util_verify_client_done(13, BLE_HS_EBADDATA, 0);

    /*** A response to another request is ignored. */
    ble_br_sdp_test_util_client_start();
    ble_br_sdp_test_util_verify_tx_client_req(0, NULL, 0);

    put_be16(params, sizeof ble_br_sdp_test_peer_lists);
    ble_br_sdp_test_util_rx_pdu(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_RSP,
                                ble_br_sdp_test_tid + 1, params, sizeof params);
    TEST_ASSERT(ble_br_sdp_test_cb_cnt == 13);
    ble_br_test_util_verify_tx_none();

    ble_br_sdp_test_util_rx_pdu(BLE_BR_SDP_TEST_PDU_SEARCH_ATTR_RSP,
                                ble_br_sdp_test_tid, params, sizeof params);
    ble_br_sdp_test_util_verify_client_done(14, 0, 3);

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_sdp_test_client_fail)
{
    const uint8_t *cmd;
    int32_t ticks_from_now;
    int32_t timeout_ticks;
    uint8_t buf[8];
    uint8_t ident;
    int rc;

    ble_br_sdp_test_util_init();

    timeout_ticks = ble_npl_time_ms_to_ticks32(BLE_BR_SDP_TEST_CLIENT_TIMEOUT_MS);

    /*** The peer has no server. */
    rc = ble_br_sdp_find_rfcomm(2, BLE_BR_SDP_UUID_HANDSFREE_AG,
                                ble_br_sdp_test_cb, &ble_br_sdp_test_cb_arg);
    TEST_ASSERT_FATAL(rc == 0);

    cmd = ble_br_test_util_verify_tx_sig(2, BLE_BR_TEST_UTIL_SIG_CONN_REQ, &ident, 4);
    TEST_ASSERT(get_le16(cmd + 0) == BLE_BR_L2CAP_PSM_SDP);

    put_le16(buf + 0, 0);
    put_le16(buf + 2, get_le16(cmd + 2));
    put_le16(buf + 4, 2);
    put_le16(buf + 6, 0);
    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_CONN_RSP, ident, buf, sizeof buf);

    TEST_ASSERT(ble_br_sdp_test_cb_cnt == 1);
    TEST_ASSERT(ble_br_sdp_test_cb_conn_handle == 2);
    TEST_ASSERT(ble_br_sdp_test_cb_status == BLE_HS_EREJECT);
    TEST_ASSERT(ble_br_sdp_test_cb_channel == 0);
    ble_br_test_util_verify_tx_none();

    /*** The server of the peer does not answer. */
    ble_br_sdp_test_util_client_start();
    ble_br_sdp_test_util_verify_tx_client_req(0, NULL, 0);

    /* Ensure timer will expire in 30 seconds. */
    ticks_from_now = ble_br_test_util_time_advance(0);
    TEST_ASSERT(ticks_from_now == timeout_ticks);

    /* Almost let the timer expire. */
    ticks_from_now = ble_br_test_util_time_advance(timeout_ticks - 1);
    TEST_ASSERT(ticks_from_now == 1);
    TEST_ASSERT(ble_br_sdp_test_cb_cnt == 1);

    /* Allow the timer to expire. */
    ticks_from_now = ble_br_test_util_time_advance(1);
    TEST_ASSERT(ticks_from_now == BLE_HS_FOREVER);
    ble_br_sdp_test_util_verify_client_done(2, BLE_HS_ETIMEOUT, 0);

    /*** The search takes too long as a whole. */
    ble_br_sdp_test_util_client_start();
    ble_br_sdp_test_util_verify_tx_client_req(0, NULL, 0);

    ticks_from_now = ble_br_test_util_time_advance(timeout_ticks - 1);
    TEST_ASSERT(ticks_from_now == 1);

    ble_br_sdp_test_util_rx_client_rsp(ble_br_sdp_test_peer_lists, 5, buf, 1);
    ble_br_sdp_test_util_verify_tx_client_req(5, buf, 1);

    ticks_from_now = ble_br_test_util_time_advance(1);
    TEST_ASSERT(ticks_from_now == BLE_HS_FOREVER);
    ble_br_sdp_test_util_verify_client_done(3, BLE_HS_ETIMEOUT, 0);

    /*** The peer closes the channel. */
    ble_br_sdp_test_util_client_start();
    ble_br_sdp_test_util_verify_tx_client_req(0, NULL, 0);

    put_le16(buf + 0, ble_br_sdp_test_chan.our_cid);
    put_le16(buf + 2, ble_br_sdp_test_chan.peer_cid);
    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_DISCONN_REQ,
                            ble_br_test_util_next_ident(), buf, 4);
    ble_br_test_util_verify_tx_sig(2, BLE_BR_TEST_UTIL_SIG_DISCONN_RSP, &ident, 4);

    TEST_ASSERT(ble_br_sdp_test_cb_cnt == 4);
    TEST_ASSERT(ble_br_sdp_test_cb_status == BLE_HS_ENOTCONN);
    TEST_ASSERT(ble_br_sdp_test_cb_channel == 0);
    ble_br_test_util_verify_tx_none();

    /*** The connection is lost. */
    ble_br_sdp_test_util_client_start();
    ble_br_sdp_test_util_verify_tx_client_req(0, NULL, 0);

    ble_hs_test_util_hci_rx_disconn_complete_event(2, 0, BLE_ERR_CONN_SPVN_TMO);

    TEST_ASSERT(ble_br_sdp_test_cb_cnt == 5);
    TEST_ASSERT(ble_br_sdp_test_cb_conn_handle == 2);
    TEST_ASSERT(ble_br_sdp_test_cb_status == BLE_HS_ENOTCONN);
    TEST_ASSERT(ble_br_sdp_test_cb_channel == 0);

    /* Nothing is left to time out. */
    ticks_from_now = ble_br_test_util_time_advance(timeout_ticks);
    TEST_ASSERT(ticks_from_now == BLE_HS_FOREVER);
    TEST_ASSERT(ble_br_sdp_test_cb_cnt == 5);

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

#endif

TEST_SUITE(ble_br_sdp_test_suite)
{
#if MYNEWT_VAL(BLE_BREDR)
    ble_br_sdp_test_server_search_attr();
    ble_br_sdp_test_server_cont();
    ble_br_sdp_test_server_search();
    ble_br_sdp_test_server_malformed();
#if MYNEWT_VAL(BLE_BREDR_HFP_HF)
    ble_br_sdp_test_server_hfp_hf();
#endif
    ble_br_sdp_test_client();
    ble_br_sdp_test_client_cont();
    ble_br_sdp_test_client_malformed();
    ble_br_sdp_test_client_fail();
#endif
}
