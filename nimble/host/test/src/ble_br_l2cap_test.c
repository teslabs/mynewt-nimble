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
#include "ble_hs_test.h"
#include "ble_hs_test_util.h"
#include "ble_br_test_util.h"

#if MYNEWT_VAL(BLE_BREDR)

#define BLE_BR_L2CAP_TEST_PSM     0x1001
#define BLE_BR_L2CAP_TEST_PSM_ENC 0x1003

/* How long a request waits for its response, and for a pending one. */
#define BLE_BR_L2CAP_TEST_RTX_MS  30000
#define BLE_BR_L2CAP_TEST_ERTX_MS 60000

static const uint8_t ble_br_l2cap_test_addr[BLE_DEV_ADDR_LEN] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
};

static struct ble_br_l2cap_chan *ble_br_l2cap_test_chan;
static int ble_br_l2cap_test_connected_cnt;
static int ble_br_l2cap_test_connected_status;
static int ble_br_l2cap_test_disconnected_cnt;
static int ble_br_l2cap_test_rx_cnt;
static uint8_t ble_br_l2cap_test_rx_buf[MYNEWT_VAL(BLE_BREDR_L2CAP_MTU)];
static uint16_t ble_br_l2cap_test_rx_len;
static int ble_br_l2cap_test_accept_rc;
static int ble_br_l2cap_test_cb_arg;

static void
ble_br_l2cap_test_connected(struct ble_br_l2cap_chan *chan, int status, void *arg)
{
    TEST_ASSERT(arg == &ble_br_l2cap_test_cb_arg);

    ble_br_l2cap_test_chan = chan;
    ble_br_l2cap_test_connected_cnt++;
    ble_br_l2cap_test_connected_status = status;
}

static void
ble_br_l2cap_test_disconnected(struct ble_br_l2cap_chan *chan, void *arg)
{
    TEST_ASSERT(arg == &ble_br_l2cap_test_cb_arg);
    TEST_ASSERT(chan == ble_br_l2cap_test_chan);

    ble_br_l2cap_test_disconnected_cnt++;
}

static void
ble_br_l2cap_test_recv(struct ble_br_l2cap_chan *chan, struct os_mbuf *om, void *arg)
{
    int rc;

    TEST_ASSERT(arg == &ble_br_l2cap_test_cb_arg);
    TEST_ASSERT(chan == ble_br_l2cap_test_chan);
    TEST_ASSERT_FATAL(OS_MBUF_PKTLEN(om) <= sizeof ble_br_l2cap_test_rx_buf);

    ble_br_l2cap_test_rx_len = OS_MBUF_PKTLEN(om);
    rc = os_mbuf_copydata(om, 0, ble_br_l2cap_test_rx_len, ble_br_l2cap_test_rx_buf);
    TEST_ASSERT_FATAL(rc == 0);

    ble_br_l2cap_test_rx_cnt++;
}

static const struct ble_br_l2cap_chan_ops ble_br_l2cap_test_ops = {
    .connected = ble_br_l2cap_test_connected,
    .disconnected = ble_br_l2cap_test_disconnected,
    .recv = ble_br_l2cap_test_recv,
};

static int
ble_br_l2cap_test_accept(uint16_t conn_handle,
                         const struct ble_br_l2cap_chan_ops **ops, void **arg)
{
    TEST_ASSERT(conn_handle == 2);

    *ops = &ble_br_l2cap_test_ops;
    *arg = &ble_br_l2cap_test_cb_arg;

    return ble_br_l2cap_test_accept_rc;
}

static struct ble_br_l2cap_server ble_br_l2cap_test_server = {
    .psm = BLE_BR_L2CAP_TEST_PSM,
    .accept = ble_br_l2cap_test_accept,
};

static struct ble_br_l2cap_server ble_br_l2cap_test_server_enc = {
    .psm = BLE_BR_L2CAP_TEST_PSM_ENC,
    .flags = BLE_BR_L2CAP_SERVER_F_ENCRYPT,
    .accept = ble_br_l2cap_test_accept,
};

static void
ble_br_l2cap_test_util_init_buf(uint16_t acl_len, uint16_t acl_pkts, int shared)
{
    int rc;

    ble_br_test_util_init_buf(acl_len, acl_pkts, shared);

    ble_br_l2cap_test_chan = NULL;
    ble_br_l2cap_test_connected_cnt = 0;
    ble_br_l2cap_test_connected_status = -1;
    ble_br_l2cap_test_disconnected_cnt = 0;
    ble_br_l2cap_test_rx_cnt = 0;
    ble_br_l2cap_test_rx_len = 0;
    ble_br_l2cap_test_accept_rc = 0;

    rc = ble_br_l2cap_server_register(&ble_br_l2cap_test_server);
    TEST_ASSERT_FATAL(rc == 0);
    rc = ble_br_l2cap_server_register(&ble_br_l2cap_test_server_enc);
    TEST_ASSERT_FATAL(rc == 0);
}

static void
ble_br_l2cap_test_util_init(void)
{
    ble_br_l2cap_test_util_init_buf(BLE_BR_TEST_UTIL_ACL_LEN,
                                    BLE_BR_TEST_UTIL_ACL_PKTS, 0);
}

static void
ble_br_l2cap_test_util_rx_conn_req(uint16_t psm, uint16_t scid)
{
    uint8_t buf[4];

    put_le16(buf + 0, psm);
    put_le16(buf + 2, scid);
    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_CONN_REQ,
                            ble_br_test_util_next_ident(), buf, sizeof buf);
}

static void
ble_br_l2cap_test_util_rx_conn_rsp(uint8_t ident, uint16_t dcid, uint16_t scid,
                                   uint16_t result, uint16_t status)
{
    uint8_t buf[8];

    put_le16(buf + 0, dcid);
    put_le16(buf + 2, scid);
    put_le16(buf + 4, result);
    put_le16(buf + 6, status);
    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_CONN_RSP, ident, buf, sizeof buf);
}

/**
 * Verifies a connection response, and retrieves the channel ID of the host.
 */
static uint16_t
ble_br_l2cap_test_util_verify_tx_conn_rsp(uint8_t ident, uint16_t scid,
                                          uint16_t result, uint16_t status)
{
    const uint8_t *cmd;
    uint8_t rsp_ident;

    cmd = ble_br_test_util_verify_tx_sig(2, BLE_BR_TEST_UTIL_SIG_CONN_RSP,
                                         &rsp_ident, 8);
    TEST_ASSERT(rsp_ident == ident);
    TEST_ASSERT(get_le16(cmd + 2) == scid);
    TEST_ASSERT(get_le16(cmd + 4) == result);
    TEST_ASSERT(get_le16(cmd + 6) == status);

    return get_le16(cmd + 0);
}

/**
 * Receives a configuration request and verifies the response to it.
 */
static void
ble_br_l2cap_test_util_config_req(const struct ble_br_test_util_chan *chan,
                                  uint16_t flags, const uint8_t *opts,
                                  uint8_t opts_len, uint16_t exp_result,
                                  const uint8_t *exp_opts, uint8_t exp_opts_len)
{
    const uint8_t *cmd;
    uint8_t buf[4 + 32];
    uint8_t rsp_ident;
    uint8_t ident;

    TEST_ASSERT_FATAL(opts_len <= 32);

    put_le16(buf + 0, chan->our_cid);
    put_le16(buf + 2, flags);
    memcpy(buf + 4, opts, opts_len);

    ident = ble_br_test_util_next_ident();
    ble_br_test_util_rx_sig(chan->conn_handle, BLE_BR_TEST_UTIL_SIG_CONFIG_REQ,
                            ident, buf, 4 + opts_len);

    cmd = ble_br_test_util_verify_tx_sig(chan->conn_handle,
                                         BLE_BR_TEST_UTIL_SIG_CONFIG_RSP,
                                         &rsp_ident, 6 + exp_opts_len);
    TEST_ASSERT(rsp_ident == ident);
    TEST_ASSERT(get_le16(cmd + 0) == chan->peer_cid);
    TEST_ASSERT(get_le16(cmd + 2) == flags);
    TEST_ASSERT(get_le16(cmd + 4) == exp_result);
    TEST_ASSERT(memcmp(cmd + 6, exp_opts, exp_opts_len) == 0);
}

/**
 * Has the peer open a channel to the test server, up to the configuration
 * request of the host.
 */
static uint8_t
ble_br_l2cap_test_util_rx_connect_no_config(struct ble_br_test_util_chan *chan,
                                            uint16_t peer_cid)
{
    const uint8_t *cmd;
    uint8_t ident;

    ble_br_l2cap_test_util_rx_conn_req(BLE_BR_L2CAP_TEST_PSM, peer_cid);

    chan->conn_handle = 2;
    chan->peer_cid = peer_cid;
    chan->our_cid = ble_br_l2cap_test_util_verify_tx_conn_rsp(
        ble_br_test_util_next_ident() - 1, peer_cid, 0, 0);

    cmd = ble_br_test_util_verify_tx_sig(2, BLE_BR_TEST_UTIL_SIG_CONFIG_REQ,
                                         &ident, 8);
    TEST_ASSERT(get_le16(cmd + 0) == peer_cid);

    return ident;
}

static void
ble_br_l2cap_test_util_rx_config_rsp(const struct ble_br_test_util_chan *chan,
                                     uint8_t ident, uint16_t result)
{
    uint8_t buf[6];

    put_le16(buf + 0, chan->our_cid);
    put_le16(buf + 2, 0);
    put_le16(buf + 4, result);
    ble_br_test_util_rx_sig(chan->conn_handle, BLE_BR_TEST_UTIL_SIG_CONFIG_RSP,
                            ident, buf, sizeof buf);
}

TEST_CASE_SELF(ble_br_l2cap_test_incoming)
{
    static const uint8_t mtu_opt[] = { 0x01, 0x02, 0xa0, 0x02 };
    struct ble_br_test_util_chan chan;
    const uint8_t *cmd;
    uint8_t buf[4];
    uint8_t ident;

    ble_br_l2cap_test_util_init();
    ble_br_test_util_create_conn(2, ble_br_l2cap_test_addr, 0);

    /*** The peer connects; the host answers and asks for its MTU. */
    ident = ble_br_l2cap_test_util_rx_connect_no_config(&chan, 0x0050);
    TEST_ASSERT(chan.our_cid >= 0x0040);
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 0);

    /*** The channel is open once both sides are configured. */
    ble_br_l2cap_test_util_rx_config_rsp(&chan, ident, 0);
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 0);

    ble_br_l2cap_test_util_config_req(&chan, 0, mtu_opt, sizeof mtu_opt, 0, NULL, 0);
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 1);
    TEST_ASSERT(ble_br_l2cap_test_connected_status == 0);
    TEST_ASSERT_FATAL(ble_br_l2cap_test_chan != NULL);
    TEST_ASSERT(ble_br_l2cap_chan_conn_handle(ble_br_l2cap_test_chan) == 2);
    TEST_ASSERT(ble_br_l2cap_chan_peer_mtu(ble_br_l2cap_test_chan) == 672);

    /*** A request that does not name both ends of the channel is ignored. */
    put_le16(buf + 0, chan.our_cid);
    put_le16(buf + 2, chan.peer_cid + 1);
    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_DISCONN_REQ,
                            ble_br_test_util_next_ident(), buf, sizeof buf);
    ble_br_test_util_verify_tx_none();
    TEST_ASSERT(ble_br_l2cap_test_disconnected_cnt == 0);

    /*** The peer disconnects. */
    put_le16(buf + 0, chan.our_cid);
    put_le16(buf + 2, chan.peer_cid);
    ident = ble_br_test_util_next_ident();
    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_DISCONN_REQ, ident, buf,
                            sizeof buf);

    cmd = ble_br_test_util_verify_tx_sig(2, BLE_BR_TEST_UTIL_SIG_DISCONN_RSP,
                                         &ident, 4);
    TEST_ASSERT(ident == ble_br_test_util_next_ident() - 1);
    TEST_ASSERT(get_le16(cmd + 0) == chan.our_cid);
    TEST_ASSERT(get_le16(cmd + 2) == chan.peer_cid);
    TEST_ASSERT(ble_br_l2cap_test_disconnected_cnt == 1);
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 1);

    /* Data for the channel that is gone is dropped. */
    ble_br_test_util_rx_l2cap(2, chan.our_cid, buf, sizeof buf);
    TEST_ASSERT(ble_br_l2cap_test_rx_cnt == 0);

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_l2cap_test_outgoing)
{
    struct ble_br_test_util_chan chan;
    struct ble_br_l2cap_chan *l2cap_chan;
    const uint8_t *cmd;
    uint8_t buf[4];
    uint8_t ident;
    int rc;

    ble_br_l2cap_test_util_init();

    rc = ble_br_l2cap_connect(2, BLE_BR_L2CAP_TEST_PSM, &ble_br_l2cap_test_ops,
                              &ble_br_l2cap_test_cb_arg, &l2cap_chan);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);

    ble_br_test_util_create_conn(2, ble_br_l2cap_test_addr, 0);

    /*** The peer refuses the channel. */
    rc = ble_br_l2cap_connect(2, BLE_BR_L2CAP_TEST_PSM, &ble_br_l2cap_test_ops,
                              &ble_br_l2cap_test_cb_arg, &l2cap_chan);
    TEST_ASSERT_FATAL(rc == 0);

    cmd = ble_br_test_util_verify_tx_sig(2, BLE_BR_TEST_UTIL_SIG_CONN_REQ, &ident, 4);
    TEST_ASSERT(get_le16(cmd + 0) == BLE_BR_L2CAP_TEST_PSM);
    chan.conn_handle = 2;
    chan.our_cid = get_le16(cmd + 2);
    TEST_ASSERT(chan.our_cid >= 0x0040);

    /* PSM not supported. */
    ble_br_l2cap_test_util_rx_conn_rsp(ident, 0, chan.our_cid, 2, 0);
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 1);
    TEST_ASSERT(ble_br_l2cap_test_connected_status == BLE_HS_EREJECT);
    TEST_ASSERT(ble_br_l2cap_test_chan == l2cap_chan);
    ble_br_test_util_verify_tx_none();

    /*** The peer refuses the channel: the link is not secure enough. */
    rc = ble_br_l2cap_connect(2, BLE_BR_L2CAP_TEST_PSM, &ble_br_l2cap_test_ops,
                              &ble_br_l2cap_test_cb_arg, &l2cap_chan);
    TEST_ASSERT_FATAL(rc == 0);

    cmd = ble_br_test_util_verify_tx_sig(2, BLE_BR_TEST_UTIL_SIG_CONN_REQ, &ident, 4);
    chan.our_cid = get_le16(cmd + 2);

    ble_br_l2cap_test_util_rx_conn_rsp(ident, 0, chan.our_cid, 3, 0);
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 2);
    TEST_ASSERT(ble_br_l2cap_test_connected_status == BLE_HS_EAUTHEN);

    /*** The peer accepts the channel, after asking to wait. */
    rc = ble_br_l2cap_connect(2, BLE_BR_L2CAP_TEST_PSM, &ble_br_l2cap_test_ops,
                              &ble_br_l2cap_test_cb_arg, &l2cap_chan);
    TEST_ASSERT_FATAL(rc == 0);

    cmd = ble_br_test_util_verify_tx_sig(2, BLE_BR_TEST_UTIL_SIG_CONN_REQ, &ident, 4);
    TEST_ASSERT(get_le16(cmd + 0) == BLE_BR_L2CAP_TEST_PSM);
    chan.our_cid = get_le16(cmd + 2);
    chan.peer_cid = 0x0050;

    ble_br_l2cap_test_util_rx_conn_rsp(ident, chan.peer_cid, chan.our_cid, 1, 2);
    ble_br_test_util_verify_tx_none();

    /* A response to another request is ignored. */
    ble_br_l2cap_test_util_rx_conn_rsp(ident + 1, chan.peer_cid, chan.our_cid, 0, 0);
    ble_br_test_util_verify_tx_none();

    ble_br_l2cap_test_util_rx_conn_rsp(ident, chan.peer_cid, chan.our_cid, 0, 0);
    ble_br_test_util_l2cap_config(&chan);

    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 3);
    TEST_ASSERT(ble_br_l2cap_test_connected_status == 0);
    TEST_ASSERT(ble_br_l2cap_test_chan == l2cap_chan);
    TEST_ASSERT(ble_br_l2cap_chan_conn_handle(l2cap_chan) == 2);

    /*** We disconnect; the application is not told about what it asked. */
    rc = ble_br_l2cap_disconnect(l2cap_chan);
    TEST_ASSERT(rc == 0);
    ble_br_test_util_l2cap_verify_tx_disconnect(&chan);
    TEST_ASSERT(ble_br_l2cap_test_disconnected_cnt == 0);

    rc = ble_br_l2cap_disconnect(l2cap_chan);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);

    put_le16(buf + 0, chan.peer_cid);
    put_le16(buf + 2, chan.our_cid);
    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_DISCONN_RSP, 1, buf, sizeof buf);
    TEST_ASSERT(ble_br_l2cap_test_disconnected_cnt == 0);

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_l2cap_test_reject)
{
    struct ble_br_test_util_chan chan;
    uint16_t cid;
    int i;

    ble_br_l2cap_test_util_init();
    ble_br_test_util_create_conn(2, ble_br_l2cap_test_addr, 0);

    /*** Nothing listens on the PSM. */
    ble_br_l2cap_test_util_rx_conn_req(0x1005, 0x0050);
    cid = ble_br_l2cap_test_util_verify_tx_conn_rsp(
        ble_br_test_util_next_ident() - 1, 0x0050, 2, 0);
    TEST_ASSERT(cid == 0);

    /*** The server does not take the channel. */
    ble_br_l2cap_test_accept_rc = BLE_HS_ENOMEM;
    ble_br_l2cap_test_util_rx_conn_req(BLE_BR_L2CAP_TEST_PSM, 0x0050);
    cid = ble_br_l2cap_test_util_verify_tx_conn_rsp(
        ble_br_test_util_next_ident() - 1, 0x0050, 4, 0);
    TEST_ASSERT(cid == 0);
    ble_br_l2cap_test_accept_rc = 0;

    /*** No channel is left. */
    for (i = 0; i < MYNEWT_VAL(BLE_BREDR_L2CAP_MAX_CHANS); i++) {
        ble_br_test_util_l2cap_rx_connect(&chan, 2, BLE_BR_L2CAP_TEST_PSM, 0x0050 + i);
    }
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt ==
                MYNEWT_VAL(BLE_BREDR_L2CAP_MAX_CHANS));

    ble_br_l2cap_test_util_rx_conn_req(BLE_BR_L2CAP_TEST_PSM, 0x0060);
    cid = ble_br_l2cap_test_util_verify_tx_conn_rsp(
        ble_br_test_util_next_ident() - 1, 0x0060, 4, 0);
    TEST_ASSERT(cid == 0);

    /* Malformed requests are ignored. */
    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_CONN_REQ,
                            ble_br_test_util_next_ident(), &cid, 2);

    TEST_ASSERT(ble_br_l2cap_test_connected_cnt ==
                MYNEWT_VAL(BLE_BREDR_L2CAP_MAX_CHANS));
    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_l2cap_test_pending_enc)
{
    static const uint8_t mtu_opt[] = { 0x01, 0x02, 0xa0, 0x02 };
    struct ble_br_test_util_chan chan;
    uint8_t buf[8];
    uint8_t ident;
    uint16_t cid;

    ble_br_l2cap_test_util_init();
    ble_br_test_util_create_conn(2, ble_br_l2cap_test_addr, 0);

    /*** The link gets encrypted after the request: pending, then success. */
    ble_br_l2cap_test_util_rx_conn_req(BLE_BR_L2CAP_TEST_PSM_ENC, 0x0050);
    ident = ble_br_test_util_next_ident() - 1;

    chan.conn_handle = 2;
    chan.peer_cid = 0x0050;
    chan.our_cid = ble_br_l2cap_test_util_verify_tx_conn_rsp(ident, 0x0050, 1, 1);
    TEST_ASSERT(chan.our_cid >= 0x0040);
    ble_br_test_util_verify_tx_none();

    /* The channel can not be configured yet. */
    put_le16(buf + 0, chan.our_cid);
    put_le16(buf + 2, 0);
    memcpy(buf + 4, mtu_opt, sizeof mtu_opt);
    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_CONFIG_REQ,
                            ble_br_test_util_next_ident(), buf, sizeof buf);
    ble_br_test_util_verify_tx_none();

    ble_br_test_util_rx_enc_change(2, 0, 1);

    cid = ble_br_l2cap_test_util_verify_tx_conn_rsp(ident, 0x0050, 0, 0);
    TEST_ASSERT(cid == chan.our_cid);
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 0);

    ble_br_test_util_l2cap_config(&chan);
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 1);
    TEST_ASSERT(ble_br_l2cap_test_connected_status == 0);

    /*** The link is encrypted already: success. */
    ble_br_test_util_l2cap_rx_connect(&chan, 2, BLE_BR_L2CAP_TEST_PSM_ENC, 0x0051);
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 2);
    TEST_ASSERT(ble_br_l2cap_test_connected_status == 0);

    /*** The link fails to get encrypted: pending, then refused. */
    ble_br_test_util_rx_enc_change(2, 0, 0);

    ble_br_l2cap_test_util_rx_conn_req(BLE_BR_L2CAP_TEST_PSM_ENC, 0x0052);
    ident = ble_br_test_util_next_ident() - 1;
    cid = ble_br_l2cap_test_util_verify_tx_conn_rsp(ident, 0x0052, 1, 1);
    TEST_ASSERT(cid >= 0x0040);

    ble_br_test_util_rx_enc_change(2, BLE_ERR_AUTH_FAIL, 0);

    cid = ble_br_l2cap_test_util_verify_tx_conn_rsp(ident, 0x0052, 3, 0);
    TEST_ASSERT(cid == 0);
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 3);
    TEST_ASSERT(ble_br_l2cap_test_connected_status == BLE_HS_EAUTHEN);

    /* The open channels are left alone. */
    TEST_ASSERT(ble_br_l2cap_test_disconnected_cnt == 0);

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_l2cap_test_data)
{
    struct ble_br_test_util_chan chan;
    struct os_mbuf *om;
    uint8_t data[MYNEWT_VAL(BLE_BREDR_L2CAP_MTU) + 1];
    uint8_t pdu[BLE_L2CAP_HDR_SZ + 100];
    int rc;
    int i;

    ble_br_l2cap_test_util_init();
    ble_br_test_util_create_conn(2, ble_br_l2cap_test_addr, 0);
    ble_br_test_util_l2cap_rx_connect(&chan, 2, BLE_BR_L2CAP_TEST_PSM, 0x0050);
    TEST_ASSERT_FATAL(ble_br_l2cap_test_chan != NULL);

    for (i = 0; i < sizeof data; i++) {
        data[i] = i;
    }

    /*** Receive an SDU in a single ACL data packet. */
    ble_br_test_util_rx_l2cap(2, chan.our_cid, data, 10);
    TEST_ASSERT(ble_br_l2cap_test_rx_cnt == 1);
    TEST_ASSERT(ble_br_l2cap_test_rx_len == 10);
    TEST_ASSERT(memcmp(ble_br_l2cap_test_rx_buf, data, 10) == 0);

    /*** Receive an SDU in three ACL data packets. */
    put_le16(pdu + 0, 100);
    put_le16(pdu + 2, chan.our_cid);
    memcpy(pdu + BLE_L2CAP_HDR_SZ, data, 100);

    rc = ble_br_test_util_rx_acl(2, BLE_HCI_PB_FIRST_FLUSH, pdu, 40);
    TEST_ASSERT(rc == 0);
    rc = ble_br_test_util_rx_acl(2, BLE_HCI_PB_MIDDLE, pdu + 40, 40);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(ble_br_l2cap_test_rx_cnt == 1);

    rc = ble_br_test_util_rx_acl(2, BLE_HCI_PB_MIDDLE, pdu + 80, 24);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(ble_br_l2cap_test_rx_cnt == 2);
    TEST_ASSERT(ble_br_l2cap_test_rx_len == 100);
    TEST_ASSERT(memcmp(ble_br_l2cap_test_rx_buf, data, 100) == 0);

    /*** A continuation without a start is dropped. */
    rc = ble_br_test_util_rx_acl(2, BLE_HCI_PB_MIDDLE, pdu + 80, 24);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(ble_br_l2cap_test_rx_cnt == 2);

    /*** A PDU longer than its header says is dropped. */
    rc = ble_br_test_util_rx_acl(2, BLE_HCI_PB_FIRST_FLUSH, pdu, 40);
    TEST_ASSERT(rc == 0);
    rc = ble_br_test_util_rx_acl(2, BLE_HCI_PB_MIDDLE, pdu + 40, 40);
    TEST_ASSERT(rc == 0);
    rc = ble_br_test_util_rx_acl(2, BLE_HCI_PB_MIDDLE, pdu + 80, 25);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(ble_br_l2cap_test_rx_cnt == 2);

    /*** A new PDU replaces one that is incomplete. */
    rc = ble_br_test_util_rx_acl(2, BLE_HCI_PB_FIRST_FLUSH, pdu, 40);
    TEST_ASSERT(rc == 0);
    ble_br_test_util_rx_l2cap(2, chan.our_cid, data, 20);
    TEST_ASSERT(ble_br_l2cap_test_rx_cnt == 3);
    TEST_ASSERT(ble_br_l2cap_test_rx_len == 20);

    /*** A PDU larger than the MTU, or too short for a header, is dropped. */
    put_le16(pdu + 0, MYNEWT_VAL(BLE_BREDR_L2CAP_MTU) + 1);
    rc = ble_br_test_util_rx_acl(2, BLE_HCI_PB_FIRST_FLUSH, pdu, 40);
    TEST_ASSERT(rc == 0);
    rc = ble_br_test_util_rx_acl(2, BLE_HCI_PB_MIDDLE, pdu + 40, 40);
    TEST_ASSERT(rc == 0);
    rc = ble_br_test_util_rx_acl(2, BLE_HCI_PB_FIRST_FLUSH, pdu, 3);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(ble_br_l2cap_test_rx_cnt == 3);

    /*** Data for a channel that does not exist is dropped. */
    ble_br_test_util_rx_l2cap(2, chan.our_cid + 1, data, 10);
    ble_br_test_util_rx_l2cap(2, 0x0002, data, 10);
    TEST_ASSERT(ble_br_l2cap_test_rx_cnt == 3);

    /*** Data for a connection that does not exist is not ours. */
    put_le16(pdu + 0, 100);
    rc = ble_br_test_util_rx_acl(3, BLE_HCI_PB_FIRST_FLUSH, pdu, 40);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);

    /*** Send an SDU that fits in an ACL data packet. */
    om = ble_br_l2cap_alloc();
    TEST_ASSERT_FATAL(om != NULL);
    rc = os_mbuf_append(om, data, 10);
    TEST_ASSERT_FATAL(rc == 0);

    rc = ble_br_l2cap_send(ble_br_l2cap_test_chan, om);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(ble_hs_test_util_prev_tx_queue_sz() == 1);
    ble_br_test_util_verify_tx_l2cap(2, chan.peer_cid, data, 10);

    /*** Send an SDU that takes three ACL data packets. */
    om = ble_br_l2cap_alloc();
    TEST_ASSERT_FATAL(om != NULL);
    rc = os_mbuf_append(om, data, 150);
    TEST_ASSERT_FATAL(rc == 0);

    rc = ble_br_l2cap_send(ble_br_l2cap_test_chan, om);
    TEST_ASSERT(rc == 0);
    TEST_ASSERT(ble_hs_test_util_prev_tx_queue_sz() == 3);
    ble_br_test_util_verify_tx_l2cap(2, chan.peer_cid, data, 150);

    /*** An SDU larger than the MTU of the peer is not sent. */
    om = ble_br_l2cap_alloc();
    TEST_ASSERT_FATAL(om != NULL);
    rc = os_mbuf_append(om, data, sizeof data);
    TEST_ASSERT_FATAL(rc == 0);

    rc = ble_br_l2cap_send(ble_br_l2cap_test_chan, om);
    TEST_ASSERT(rc == BLE_HS_EINVAL);

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_l2cap_test_config)
{
    static const uint8_t mtu_min[] = { 0x01, 0x02, 0x30, 0x00 };
    static const uint8_t mtu_small[] = { 0x01, 0x02, 0x2f, 0x00 };
    static const uint8_t mtu_bad_len[] = { 0x01, 0x01, 0x30 };
    static const uint8_t mtu_truncated[] = { 0x01, 0x02, 0x30 };
    static const uint8_t rfc_ertm[] = {
        0x04, 0x09, 0x03, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    };
    static const uint8_t rfc_basic[] = {
        0x04, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t unknown[] = { 0x7e, 0x02, 0x00, 0x00 };
    static const uint8_t unknown_rsp[] = { 0x7e, 0x00 };
    static const uint8_t ignored[] = {
        /* Flush timeout, FCS and an unknown hint. */
        0x02, 0x02, 0xff, 0xff, 0x05, 0x01, 0x00, 0xfe, 0x02, 0x00, 0x00,
    };
    struct ble_br_test_util_chan chan;
    uint8_t ident;

    ble_br_l2cap_test_util_init();
    ble_br_test_util_create_conn(2, ble_br_l2cap_test_addr, 0);

    ident = ble_br_l2cap_test_util_rx_connect_no_config(&chan, 0x0050);
    ble_br_l2cap_test_util_rx_config_rsp(&chan, ident, 0);

    /*** Options the host refuses; the channel stays closed. */

    /* An MTU below the minimum: the minimum is proposed. */
    ble_br_l2cap_test_util_config_req(&chan, 0, mtu_small, sizeof mtu_small, 1,
                                      mtu_min, sizeof mtu_min);

    /* A mode other than the basic one: the basic one is proposed. */
    ble_br_l2cap_test_util_config_req(&chan, 0, rfc_ertm, sizeof rfc_ertm, 1,
                                      rfc_basic, sizeof rfc_basic);

    /* An unknown option. */
    ble_br_l2cap_test_util_config_req(&chan, 0, unknown, sizeof unknown, 3,
                                      unknown_rsp, sizeof unknown_rsp);

    /* Malformed options. */
    ble_br_l2cap_test_util_config_req(&chan, 0, mtu_bad_len,
                                      sizeof mtu_bad_len, 2, NULL, 0);
    ble_br_l2cap_test_util_config_req(&chan, 0, mtu_truncated,
                                      sizeof mtu_truncated, 2, NULL, 0);

    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 0);

    /*** Options spread over two requests. */
    ble_br_l2cap_test_util_config_req(&chan, 0x0001, ignored, sizeof ignored,
                                      0, NULL, 0);
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 0);

    ble_br_l2cap_test_util_config_req(&chan, 0, mtu_min, sizeof mtu_min, 0, NULL, 0);
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 1);
    TEST_ASSERT(ble_br_l2cap_test_connected_status == 0);
    TEST_ASSERT_FATAL(ble_br_l2cap_test_chan != NULL);
    TEST_ASSERT(ble_br_l2cap_chan_peer_mtu(ble_br_l2cap_test_chan) == 48);

    /*** The peer refuses the options of the host: the channel is closed. */
    ident = ble_br_l2cap_test_util_rx_connect_no_config(&chan, 0x0051);
    ble_br_l2cap_test_util_rx_config_rsp(&chan, ident, 2);

    ble_br_test_util_l2cap_verify_tx_disconnect(&chan);
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 2);
    TEST_ASSERT(ble_br_l2cap_test_connected_status == BLE_HS_EREJECT);

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_l2cap_test_sig_misc)
{
    static const uint8_t two_echo_reqs[] = {
        BLE_BR_TEST_UTIL_SIG_ECHO_REQ, 0x21, 0x02, 0x00, 0xaa, 0xbb,
        BLE_BR_TEST_UTIL_SIG_ECHO_REQ, 0x22, 0x00, 0x00,
    };
    static const uint8_t no_ident[] = { BLE_BR_TEST_UTIL_SIG_ECHO_REQ, 0x00,
                                        0x00, 0x00 };
    static const uint8_t truncated[] = {
        BLE_BR_TEST_UTIL_SIG_ECHO_REQ, 0x23, 0x04, 0x00, 0xaa, 0xbb,
    };
    static const uint8_t fixed_chans[8] = { 0x02 };
    const uint8_t *cmd;
    uint8_t buf[200];
    uint8_t ident;

    ble_br_l2cap_test_util_init();
    ble_br_test_util_create_conn(2, ble_br_l2cap_test_addr, 0);

    /*** Echo. */
    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_ECHO_REQ, 0x10, "ping", 4);
    cmd = ble_br_test_util_verify_tx_sig(2, BLE_BR_TEST_UTIL_SIG_ECHO_RSP, &ident, 4);
    TEST_ASSERT(ident == 0x10);
    TEST_ASSERT(memcmp(cmd, "ping", 4) == 0);

    /*** Information: extended features. */
    put_le16(buf, 0x0002);
    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_INFO_REQ, 0x11, buf, 2);
    cmd = ble_br_test_util_verify_tx_sig(2, BLE_BR_TEST_UTIL_SIG_INFO_RSP, &ident, 8);
    TEST_ASSERT(ident == 0x11);
    TEST_ASSERT(get_le16(cmd + 0) == 0x0002);
    TEST_ASSERT(get_le16(cmd + 2) == 0);
    TEST_ASSERT(get_le32(cmd + 4) == 0x00000080);

    /*** Information: fixed channels. */
    put_le16(buf, 0x0003);
    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_INFO_REQ, 0x12, buf, 2);
    cmd = ble_br_test_util_verify_tx_sig(2, BLE_BR_TEST_UTIL_SIG_INFO_RSP, &ident, 12);
    TEST_ASSERT(ident == 0x12);
    TEST_ASSERT(get_le16(cmd + 0) == 0x0003);
    TEST_ASSERT(get_le16(cmd + 2) == 0);
    TEST_ASSERT(memcmp(cmd + 4, fixed_chans, sizeof fixed_chans) == 0);

    /*** Information: something the host does not have. */
    put_le16(buf, 0x0001);
    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_INFO_REQ, 0x13, buf, 2);
    cmd = ble_br_test_util_verify_tx_sig(2, BLE_BR_TEST_UTIL_SIG_INFO_RSP, &ident, 4);
    TEST_ASSERT(ident == 0x13);
    TEST_ASSERT(get_le16(cmd + 0) == 0x0001);
    TEST_ASSERT(get_le16(cmd + 2) == 1);

    /*** An unknown command is rejected. */
    ble_br_test_util_rx_sig(2, 0x7f, 0x14, NULL, 0);
    cmd = ble_br_test_util_verify_tx_sig(2, BLE_BR_TEST_UTIL_SIG_CMD_REJECT,
                                         &ident, 2);
    TEST_ASSERT(ident == 0x14);
    TEST_ASSERT(get_le16(cmd) == 0);

    /*** Two commands in a PDU. */
    ble_br_test_util_rx_l2cap(2, BLE_BR_TEST_UTIL_CID_SIG, two_echo_reqs,
                              sizeof two_echo_reqs);
    cmd = ble_br_test_util_verify_tx_sig(2, BLE_BR_TEST_UTIL_SIG_ECHO_RSP, &ident, 2);
    TEST_ASSERT(ident == 0x21);
    TEST_ASSERT(cmd[0] == 0xaa && cmd[1] == 0xbb);
    ble_br_test_util_verify_tx_sig(2, BLE_BR_TEST_UTIL_SIG_ECHO_RSP, &ident, 0);
    TEST_ASSERT(ident == 0x22);

    /*** Responses nothing waits for and malformed PDUs are ignored. */
    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_ECHO_RSP, 0x15, NULL, 0);
    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_CMD_REJECT, 0x16, buf, 2);
    ble_br_test_util_rx_l2cap(2, BLE_BR_TEST_UTIL_CID_SIG, no_ident, sizeof no_ident);
    ble_br_test_util_rx_l2cap(2, BLE_BR_TEST_UTIL_CID_SIG, truncated, sizeof truncated);
    ble_br_test_util_rx_l2cap(2, BLE_BR_TEST_UTIL_CID_SIG, truncated, 3);

    /* Larger than the signaling MTU. */
    memset(buf, 0, sizeof buf);
    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_ECHO_REQ, 0x17, buf, 125);

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_l2cap_test_timeout)
{
    struct ble_br_test_util_chan chan;
    struct ble_br_l2cap_chan *l2cap_chan;
    const uint8_t *cmd;
    int32_t ticks_from_now;
    int32_t ertx_ticks;
    int32_t rtx_ticks;
    uint8_t ident;
    uint16_t cid;
    int rc;

    ble_br_l2cap_test_util_init();
    ble_br_test_util_create_conn(2, ble_br_l2cap_test_addr, 0);

    rtx_ticks = ble_npl_time_ms_to_ticks32(BLE_BR_L2CAP_TEST_RTX_MS);
    ertx_ticks = ble_npl_time_ms_to_ticks32(BLE_BR_L2CAP_TEST_ERTX_MS);

    /* Ensure timer is not set. */
    ticks_from_now = ble_br_test_util_time_advance(0);
    TEST_ASSERT(ticks_from_now == BLE_HS_FOREVER);

    /*** The peer does not answer the connection request. */
    rc = ble_br_l2cap_connect(2, BLE_BR_L2CAP_TEST_PSM, &ble_br_l2cap_test_ops,
                              &ble_br_l2cap_test_cb_arg, &l2cap_chan);
    TEST_ASSERT_FATAL(rc == 0);
    ble_br_test_util_verify_tx_sig(2, BLE_BR_TEST_UTIL_SIG_CONN_REQ, &ident, 4);

    /* Ensure timer will expire in 30 seconds. */
    ticks_from_now = ble_br_test_util_time_advance(0);
    TEST_ASSERT(ticks_from_now == rtx_ticks);

    /* Almost let the timer expire. */
    ticks_from_now = ble_br_test_util_time_advance(rtx_ticks - 1);
    TEST_ASSERT(ticks_from_now == 1);
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 0);

    /* Allow the timer to expire. */
    ticks_from_now = ble_br_test_util_time_advance(1);
    TEST_ASSERT(ticks_from_now == BLE_HS_FOREVER);
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 1);
    TEST_ASSERT(ble_br_l2cap_test_connected_status == BLE_HS_ETIMEOUT);
    TEST_ASSERT(ble_br_l2cap_test_chan == l2cap_chan);
    ble_br_test_util_verify_tx_none();

    /*** The peer asks to wait, and never gives its final answer. */
    rc = ble_br_l2cap_connect(2, BLE_BR_L2CAP_TEST_PSM, &ble_br_l2cap_test_ops,
                              &ble_br_l2cap_test_cb_arg, &l2cap_chan);
    TEST_ASSERT_FATAL(rc == 0);
    cmd = ble_br_test_util_verify_tx_sig(2, BLE_BR_TEST_UTIL_SIG_CONN_REQ, &ident, 4);

    ticks_from_now = ble_br_test_util_time_advance(rtx_ticks / 2);
    TEST_ASSERT(ticks_from_now == rtx_ticks - rtx_ticks / 2);

    ble_br_l2cap_test_util_rx_conn_rsp(ident, 0x0050, get_le16(cmd + 2), 1, 0);

    /* Ensure timer got extended to 60 seconds. */
    ticks_from_now = ble_br_test_util_time_advance(0);
    TEST_ASSERT(ticks_from_now == ertx_ticks);

    ticks_from_now = ble_br_test_util_time_advance(ertx_ticks - 1);
    TEST_ASSERT(ticks_from_now == 1);
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 1);

    ticks_from_now = ble_br_test_util_time_advance(1);
    TEST_ASSERT(ticks_from_now == BLE_HS_FOREVER);
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 2);
    TEST_ASSERT(ble_br_l2cap_test_connected_status == BLE_HS_ETIMEOUT);
    ble_br_test_util_verify_tx_none();

    /*** The peer does not answer the configuration request. */
    ble_br_l2cap_test_util_rx_connect_no_config(&chan, 0x0051);

    ticks_from_now = ble_br_test_util_time_advance(0);
    TEST_ASSERT(ticks_from_now == rtx_ticks);

    ticks_from_now = ble_br_test_util_time_advance(rtx_ticks);
    TEST_ASSERT(ticks_from_now == BLE_HS_FOREVER);
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 3);
    TEST_ASSERT(ble_br_l2cap_test_connected_status == BLE_HS_ETIMEOUT);

    /* Ensure the peer got told the channel is gone. */
    ble_br_test_util_l2cap_verify_tx_disconnect(&chan);

    /*** A link that does not get encrypted. */
    ble_br_l2cap_test_util_rx_conn_req(BLE_BR_L2CAP_TEST_PSM_ENC, 0x0052);
    ident = ble_br_test_util_next_ident() - 1;
    ble_br_l2cap_test_util_verify_tx_conn_rsp(ident, 0x0052, 1, 1);

    ticks_from_now = ble_br_test_util_time_advance(0);
    TEST_ASSERT(ticks_from_now == ertx_ticks);

    ticks_from_now = ble_br_test_util_time_advance(ertx_ticks - 1);
    TEST_ASSERT(ticks_from_now == 1);
    ble_br_test_util_verify_tx_none();

    ticks_from_now = ble_br_test_util_time_advance(1);
    TEST_ASSERT(ticks_from_now == BLE_HS_FOREVER);
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 4);
    TEST_ASSERT(ble_br_l2cap_test_connected_status == BLE_HS_ETIMEOUT);

    /* Ensure the peer got its final answer: refused, for security. */
    cid = ble_br_l2cap_test_util_verify_tx_conn_rsp(ident, 0x0052, 3, 0);
    TEST_ASSERT(cid == 0);

    /*** An open channel never times out. */
    ble_br_test_util_l2cap_rx_connect(&chan, 2, BLE_BR_L2CAP_TEST_PSM, 0x0053);
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 5);
    TEST_ASSERT(ble_br_l2cap_test_connected_status == 0);

    ticks_from_now = ble_br_test_util_time_advance(2 * ertx_ticks);
    TEST_ASSERT(ticks_from_now == BLE_HS_FOREVER);
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 5);
    TEST_ASSERT(ble_br_l2cap_test_disconnected_cnt == 0);

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_l2cap_test_conn_broken)
{
    struct ble_br_test_util_chan chan;
    struct ble_br_l2cap_chan *l2cap_chan;
    struct ble_br_l2cap_chan *open_chan;
    uint8_t ident;
    int rc;

    ble_br_l2cap_test_util_init();
    ble_br_test_util_create_conn(2, ble_br_l2cap_test_addr, 0);

    /* An open channel. */
    ble_br_test_util_l2cap_rx_connect(&chan, 2, BLE_BR_L2CAP_TEST_PSM, 0x0050);
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 1);
    open_chan = ble_br_l2cap_test_chan;

    /* A PDU that is incomplete. */
    rc = ble_br_test_util_rx_acl(2, BLE_HCI_PB_FIRST_FLUSH,
                                 ((uint8_t[]){ 0x10, 0x00, 0x40, 0x00, 0x01 }), 5);
    TEST_ASSERT(rc == 0);

    /* Lose the connection. */
    ble_hs_test_util_hci_rx_disconn_complete_event(2, 0, BLE_ERR_CONN_SPVN_TMO);
    TEST_ASSERT(ble_br_l2cap_test_disconnected_cnt == 1);
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 1);
    TEST_ASSERT(ble_br_l2cap_chan_conn_handle(open_chan) == BLE_HS_CONN_HANDLE_NONE);

    /* A channel that is being set up. */
    ble_br_test_util_gap_event_next();
    ble_br_test_util_create_conn(2, ble_br_l2cap_test_addr, 0);

    rc = ble_br_l2cap_connect(2, BLE_BR_L2CAP_TEST_PSM, &ble_br_l2cap_test_ops,
                              &ble_br_l2cap_test_cb_arg, &l2cap_chan);
    TEST_ASSERT_FATAL(rc == 0);
    ble_br_test_util_verify_tx_sig(2, BLE_BR_TEST_UTIL_SIG_CONN_REQ, &ident, 4);

    ble_hs_test_util_hci_rx_disconn_complete_event(2, 0, BLE_ERR_CONN_SPVN_TMO);
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 2);
    TEST_ASSERT(ble_br_l2cap_test_connected_status == BLE_HS_ENOTCONN);
    TEST_ASSERT(ble_br_l2cap_test_disconnected_cnt == 1);

    /* Nothing is left to time out. */
    TEST_ASSERT(ble_br_test_util_time_advance(ble_npl_time_ms_to_ticks32(
                    BLE_BR_L2CAP_TEST_ERTX_MS)) == BLE_HS_FOREVER);
    TEST_ASSERT(ble_br_l2cap_test_connected_cnt == 2);

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_l2cap_test_acl_credits)
{
    uint8_t data[100];
    uint8_t ident;
    const uint8_t *cmd;
    int i;

    /* A controller with room for two ACL data packets. */
    ble_br_l2cap_test_util_init_buf(BLE_BR_TEST_UTIL_ACL_LEN, 2, 0);
    ble_br_test_util_create_conn(2, ble_br_l2cap_test_addr, 0);

    for (i = 0; i < sizeof data; i++) {
        data[i] = i;
    }

    /* Have the host send two PDUs, of two ACL data packets each. */
    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_ECHO_REQ, 1, data, sizeof data);
    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_ECHO_REQ, 2, data, sizeof data);

    /* Ensure the host holds on to what the controller has no room for. */
    TEST_ASSERT(ble_hs_test_util_prev_tx_queue_sz() == 2);

    /* The controller sent a packet. */
    ble_br_test_util_rx_num_completed_pkts(2, 1);
    TEST_ASSERT(ble_hs_test_util_prev_tx_queue_sz() == 3);

    /* Nothing is sent for the packets of other connections. */
    ble_br_test_util_rx_num_completed_pkts(3, 1);
    TEST_ASSERT(ble_hs_test_util_prev_tx_queue_sz() == 3);

    ble_br_test_util_rx_num_completed_pkts(2, 2);
    TEST_ASSERT(ble_hs_test_util_prev_tx_queue_sz() == 4);

    cmd = ble_br_test_util_verify_tx_sig(2, BLE_BR_TEST_UTIL_SIG_ECHO_RSP,
                                         &ident, sizeof data);
    TEST_ASSERT(ident == 1);
    TEST_ASSERT(memcmp(cmd, data, sizeof data) == 0);

    cmd = ble_br_test_util_verify_tx_sig(2, BLE_BR_TEST_UTIL_SIG_ECHO_RSP,
                                         &ident, sizeof data);
    TEST_ASSERT(ident == 2);
    TEST_ASSERT(memcmp(cmd, data, sizeof data) == 0);

    /* The controller still holds a packet as the connection ends; what the
     * host held for the connection is dropped.
     */
    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_ECHO_REQ, 3, data, sizeof data);
    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_ECHO_REQ, 4, data, sizeof data);
    TEST_ASSERT(ble_hs_test_util_prev_tx_queue_sz() == 1);

    ble_hs_test_util_hci_rx_disconn_complete_event(2, 0, BLE_ERR_CONN_SPVN_TMO);
    TEST_ASSERT(ble_hs_test_util_prev_tx_queue_sz() == 1);
    ble_br_test_util_tx_queue_clear();
    ble_br_test_util_gap_event_next();

    /* Ensure a new connection has all the room. */
    ble_br_test_util_create_conn(2, ble_br_l2cap_test_addr, 0);

    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_ECHO_REQ, 5, data, sizeof data);
    TEST_ASSERT(ble_hs_test_util_prev_tx_queue_sz() == 2);

    cmd = ble_br_test_util_verify_tx_sig(2, BLE_BR_TEST_UTIL_SIG_ECHO_RSP,
                                         &ident, sizeof data);
    TEST_ASSERT(ident == 5);

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_l2cap_test_acl_credits_shared)
{
    uint8_t data[100];
    uint8_t ident;
    int i;

    /* A controller with room for two ACL data packets, LE or BR/EDR. */
    ble_br_l2cap_test_util_init_buf(BLE_BR_TEST_UTIL_ACL_LEN, 2, 1);
    ble_br_test_util_create_conn(2, ble_br_l2cap_test_addr, 0);

    TEST_ASSERT(ble_hs_hci_avail_pkts == 2);

    for (i = 0; i < sizeof data; i++) {
        data[i] = i;
    }

    /* Have the host send two PDUs, of two ACL data packets each. */
    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_ECHO_REQ, 1, data, sizeof data);
    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_ECHO_REQ, 2, data, sizeof data);

    /* Ensure BR/EDR took the room there was. */
    TEST_ASSERT(ble_hs_test_util_prev_tx_queue_sz() == 2);
    TEST_ASSERT(ble_hs_hci_avail_pkts == 0);

    /* The controller sent a packet: the next one takes its place. */
    ble_br_test_util_rx_num_completed_pkts(2, 1);
    TEST_ASSERT(ble_hs_test_util_prev_tx_queue_sz() == 3);
    TEST_ASSERT(ble_hs_hci_avail_pkts == 0);

    ble_br_test_util_rx_num_completed_pkts(2, 2);
    TEST_ASSERT(ble_hs_test_util_prev_tx_queue_sz() == 4);
    TEST_ASSERT(ble_hs_hci_avail_pkts == 1);

    ble_br_test_util_rx_num_completed_pkts(2, 1);
    TEST_ASSERT(ble_hs_hci_avail_pkts == 2);

    ble_br_test_util_verify_tx_sig(2, BLE_BR_TEST_UTIL_SIG_ECHO_RSP, &ident,
                                   sizeof data);
    TEST_ASSERT(ident == 1);
    ble_br_test_util_verify_tx_sig(2, BLE_BR_TEST_UTIL_SIG_ECHO_RSP, &ident,
                                   sizeof data);
    TEST_ASSERT(ident == 2);

    /* The controller still holds packets as the connection ends. */
    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_ECHO_REQ, 3, data, sizeof data);
    TEST_ASSERT(ble_hs_test_util_prev_tx_queue_sz() == 2);
    TEST_ASSERT(ble_hs_hci_avail_pkts == 0);

    ble_hs_test_util_hci_rx_disconn_complete_event(2, 0, BLE_ERR_CONN_SPVN_TMO);
    TEST_ASSERT(ble_hs_hci_avail_pkts == 2);

    ble_br_test_util_tx_queue_clear();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

#endif

TEST_SUITE(ble_br_l2cap_test_suite)
{
#if MYNEWT_VAL(BLE_BREDR)
    ble_br_l2cap_test_incoming();
    ble_br_l2cap_test_outgoing();
    ble_br_l2cap_test_reject();
    ble_br_l2cap_test_pending_enc();
    ble_br_l2cap_test_data();
    ble_br_l2cap_test_config();
    ble_br_l2cap_test_sig_misc();
    ble_br_l2cap_test_timeout();
    ble_br_l2cap_test_conn_broken();
    ble_br_l2cap_test_acl_credits();
    ble_br_l2cap_test_acl_credits_shared();
#endif
}
