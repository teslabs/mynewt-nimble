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

/* The server of the test, and its link when the peer starts the session. */
#define BLE_BR_RFCOMM_TEST_CHANNEL 3
#define BLE_BR_RFCOMM_TEST_DLCI    (BLE_BR_RFCOMM_TEST_CHANNEL << 1)

/* How long a session and a link take to open at most. */
#define BLE_BR_RFCOMM_TEST_TIMEOUT_MS 20000

/* Short names for the fields of the frames. */
#define RFCOMM_ADDR(dlci, cr) BLE_BR_TEST_UTIL_RFCOMM_ADDR(dlci, cr)
#define RFCOMM_MCC(type, cr)  BLE_BR_TEST_UTIL_RFCOMM_MCC(type, cr)
#define RFCOMM_MCC_PN         BLE_BR_TEST_UTIL_RFCOMM_MCC_PN
#define RFCOMM_MCC_RPN        BLE_BR_TEST_UTIL_RFCOMM_MCC_RPN
#define RFCOMM_MCC_MSC        BLE_BR_TEST_UTIL_RFCOMM_MCC_MSC
#define RFCOMM_MCC_NSC        BLE_BR_TEST_UTIL_RFCOMM_MCC_NSC
#define RFCOMM_PF             BLE_BR_TEST_UTIL_RFCOMM_PF
#define RFCOMM_SABM           (BLE_BR_TEST_UTIL_RFCOMM_SABM | RFCOMM_PF)
#define RFCOMM_UA             (BLE_BR_TEST_UTIL_RFCOMM_UA | RFCOMM_PF)
#define RFCOMM_DM             (BLE_BR_TEST_UTIL_RFCOMM_DM | RFCOMM_PF)
#define RFCOMM_DISC           (BLE_BR_TEST_UTIL_RFCOMM_DISC | RFCOMM_PF)
#define RFCOMM_UIH            BLE_BR_TEST_UTIL_RFCOMM_UIH
#define RFCOMM_UIH_PF         (BLE_BR_TEST_UTIL_RFCOMM_UIH | RFCOMM_PF)

static const uint8_t ble_br_rfcomm_test_addr[BLE_DEV_ADDR_LEN] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
};

static struct ble_br_test_util_chan ble_br_rfcomm_test_chan;
static struct ble_br_rfcomm_dlc *ble_br_rfcomm_test_dlc;
static int ble_br_rfcomm_test_connected_cnt;
static int ble_br_rfcomm_test_connected_status;
static int ble_br_rfcomm_test_disconnected_cnt;
static int ble_br_rfcomm_test_rx_cnt;
static uint8_t ble_br_rfcomm_test_rx_buf[MYNEWT_VAL(BLE_BREDR_RFCOMM_MTU)];
static uint16_t ble_br_rfcomm_test_rx_len;
static int ble_br_rfcomm_test_accept_rc;
static int ble_br_rfcomm_test_cb_arg;

static void
ble_br_rfcomm_test_connected(struct ble_br_rfcomm_dlc *dlc, int status, void *arg)
{
    TEST_ASSERT(arg == &ble_br_rfcomm_test_cb_arg);

    ble_br_rfcomm_test_dlc = dlc;
    ble_br_rfcomm_test_connected_cnt++;
    ble_br_rfcomm_test_connected_status = status;
}

static void
ble_br_rfcomm_test_disconnected(struct ble_br_rfcomm_dlc *dlc, void *arg)
{
    TEST_ASSERT(arg == &ble_br_rfcomm_test_cb_arg);

    ble_br_rfcomm_test_dlc = dlc;
    ble_br_rfcomm_test_disconnected_cnt++;
}

static void
ble_br_rfcomm_test_recv(struct ble_br_rfcomm_dlc *dlc, const uint8_t *data,
                        uint16_t len, void *arg)
{
    TEST_ASSERT(arg == &ble_br_rfcomm_test_cb_arg);
    TEST_ASSERT(dlc == ble_br_rfcomm_test_dlc);
    TEST_ASSERT_FATAL(len <= sizeof ble_br_rfcomm_test_rx_buf);

    memcpy(ble_br_rfcomm_test_rx_buf, data, len);
    ble_br_rfcomm_test_rx_len = len;
    ble_br_rfcomm_test_rx_cnt++;
}

static const struct ble_br_rfcomm_dlc_ops ble_br_rfcomm_test_ops = {
    .connected = ble_br_rfcomm_test_connected,
    .disconnected = ble_br_rfcomm_test_disconnected,
    .recv = ble_br_rfcomm_test_recv,
};

static int
ble_br_rfcomm_test_accept(uint16_t conn_handle,
                          const struct ble_br_rfcomm_dlc_ops **ops, void **arg)
{
    TEST_ASSERT(conn_handle == 2);

    *ops = &ble_br_rfcomm_test_ops;
    *arg = &ble_br_rfcomm_test_cb_arg;

    return ble_br_rfcomm_test_accept_rc;
}

static struct ble_br_rfcomm_server ble_br_rfcomm_test_server = {
    .channel = BLE_BR_RFCOMM_TEST_CHANNEL,
    .accept = ble_br_rfcomm_test_accept,
};

static void
ble_br_rfcomm_test_util_init(void)
{
    int rc;

    ble_br_test_util_init();

    ble_br_rfcomm_test_dlc = NULL;
    ble_br_rfcomm_test_connected_cnt = 0;
    ble_br_rfcomm_test_connected_status = -1;
    ble_br_rfcomm_test_disconnected_cnt = 0;
    ble_br_rfcomm_test_rx_cnt = 0;
    ble_br_rfcomm_test_rx_len = 0;
    ble_br_rfcomm_test_accept_rc = 0;

    rc = ble_br_rfcomm_server_register(&ble_br_rfcomm_test_server);
    TEST_ASSERT_FATAL(rc == 0);

    /* RFCOMM takes an encrypted link. */
    ble_br_test_util_create_conn(2, ble_br_rfcomm_test_addr, 1);
}

/**
 * Has the peer start a session and open a link to the server of the test.
 *
 * @param credits               The credits the peer gives the host.
 */
static void
ble_br_rfcomm_test_util_rx_open(uint8_t credits)
{
    ble_br_test_util_rfcomm_rx_session(&ble_br_rfcomm_test_chan, 2);
    ble_br_test_util_rfcomm_rx_open(&ble_br_rfcomm_test_chan,
                                    BLE_BR_RFCOMM_TEST_DLCI, credits);

    TEST_ASSERT_FATAL(ble_br_rfcomm_test_dlc != NULL);
    TEST_ASSERT(ble_br_rfcomm_test_connected_status == 0);
}

/**
 * Receives a parameter negotiation command or response.
 */
static void
ble_br_rfcomm_test_util_rx_pn(uint8_t addr, uint8_t cr, uint8_t dlci,
                              uint8_t flow, uint16_t mtu, uint8_t credits)
{
    uint8_t pn[8];

    memset(pn, 0, sizeof pn);
    pn[0] = dlci;
    pn[1] = flow;
    put_le16(pn + 4, mtu);
    pn[7] = credits;

    ble_br_test_util_rfcomm_rx_mcc(&ble_br_rfcomm_test_chan, addr,
                                   RFCOMM_MCC(RFCOMM_MCC_PN, cr), pn, sizeof pn);
}

static void
ble_br_rfcomm_test_util_verify_tx_pn(uint8_t addr, uint8_t cr, uint8_t dlci,
                                     uint8_t flow, uint16_t mtu, uint8_t credits)
{
    uint8_t pn[8];

    memset(pn, 0, sizeof pn);
    pn[0] = dlci;
    pn[1] = flow;
    put_le16(pn + 4, mtu);
    pn[7] = credits;

    ble_br_test_util_rfcomm_verify_tx_mcc(&ble_br_rfcomm_test_chan, addr,
                                          RFCOMM_MCC(RFCOMM_MCC_PN, cr), pn,
                                          sizeof pn);
}

/**
 * Receives a frame without information and verifies the host answers with
 * another one.
 */
static void
ble_br_rfcomm_test_util_rx_ctrl(uint8_t addr, uint8_t ctrl, uint8_t exp_ctrl)
{
    uint16_t len;

    ble_br_test_util_rfcomm_rx(&ble_br_rfcomm_test_chan, addr, ctrl, -1, NULL, 0);
    ble_br_test_util_rfcomm_verify_tx(&ble_br_rfcomm_test_chan, addr, exp_ctrl,
                                      -1, &len);
    TEST_ASSERT(len == 0);
}

static void
ble_br_rfcomm_test_util_verify_tx_ctrl(uint8_t addr, uint8_t ctrl)
{
    uint16_t len;

    ble_br_test_util_rfcomm_verify_tx(&ble_br_rfcomm_test_chan, addr, ctrl, -1, &len);
    TEST_ASSERT(len == 0);
}

/**
 * Verifies the host sent data on a link.
 */
static void
ble_br_rfcomm_test_util_verify_tx_data(uint8_t addr, const void *data, uint16_t len)
{
    const uint8_t *info;
    uint16_t info_len;

    info = ble_br_test_util_rfcomm_verify_tx(&ble_br_rfcomm_test_chan, addr,
                                             RFCOMM_UIH, -1, &info_len);
    TEST_ASSERT_FATAL(info_len == len);
    TEST_ASSERT(memcmp(info, data, len) == 0);
}

/**
 * Starts a session with the peer, for a link to its server on the channel.
 */
static void
ble_br_rfcomm_test_util_connect(uint8_t channel, struct ble_br_rfcomm_dlc **out_dlc)
{
    int rc;

    rc = ble_br_rfcomm_connect(2, channel, &ble_br_rfcomm_test_ops,
                               &ble_br_rfcomm_test_cb_arg, out_dlc);
    TEST_ASSERT_FATAL(rc == 0);

    ble_br_test_util_l2cap_rx_connect_rsp(&ble_br_rfcomm_test_chan, 2,
                                          BLE_BR_L2CAP_PSM_RFCOMM, 0x0060);

    ble_br_rfcomm_test_util_verify_tx_ctrl(RFCOMM_ADDR(0, 1), RFCOMM_SABM);
}

/**
 * Opens a link to the server of the peer on the channel; the host starts
 * the session.
 *
 * @param credits               The credits the peer gives the host.
 */
static void
ble_br_rfcomm_test_util_open(uint8_t channel, uint8_t credits,
                             struct ble_br_rfcomm_dlc **out_dlc)
{
    int rc;

    rc = ble_br_rfcomm_connect(2, channel, &ble_br_rfcomm_test_ops,
                               &ble_br_rfcomm_test_cb_arg, out_dlc);
    TEST_ASSERT_FATAL(rc == 0);

    ble_br_test_util_rfcomm_rx_accept(&ble_br_rfcomm_test_chan, 2,
                                      channel << 1, credits);
}

TEST_CASE_SELF(ble_br_rfcomm_test_fcs)
{
    static const uint8_t sabm[] = { 0x03, 0x3f, 0x01, 0x1c };
    static const uint8_t sabm_bad_fcs[] = { 0x03, 0x3f, 0x01, 0x1d };
    static const uint8_t ua[] = { 0x03, 0x73, 0x01, 0xd7 };
    static const uint8_t disc[] = { 0x03, 0x53, 0x01, 0xfd };
    /* A test command and its response; the checksum of a UIH frame only
     * covers the address and the control fields.
     */
    static const uint8_t test_cmd[] = {
        0x03, 0xef, 0x09, 0x23, 0x05, 'a', 'b', 0x70,
    };
    static const uint8_t test_cmd_bad_fcs[] = {
        0x03, 0xef, 0x09, 0x23, 0x05, 'a', 'b', 0x71,
    };
    static const uint8_t test_rsp[] = {
        0x01, 0xef, 0x09, 0x21, 0x05, 'a', 'b', 0xaa,
    };
    /* Frames for a link that does not exist: SABM and DM on DLCI 2. */
    static const uint8_t sabm_dlci[] = { 0x0b, 0x3f, 0x01, 0x59 };
    static const uint8_t dm_dlci[] = { 0x0b, 0x1f, 0x01, 0x73 };
    uint8_t frame[sizeof test_cmd];
    struct ble_br_test_util_chan *chan;

    ble_br_rfcomm_test_util_init();

    chan = &ble_br_rfcomm_test_chan;
    ble_br_test_util_l2cap_rx_connect(chan, 2, BLE_BR_L2CAP_PSM_RFCOMM, 0x0050);

    /*** A frame with a wrong checksum is dropped. */
    ble_br_test_util_rx_l2cap(2, chan->our_cid, sabm_bad_fcs, sizeof sabm_bad_fcs);
    ble_br_test_util_verify_tx_none();

    /*** The session starts. */
    ble_br_test_util_rx_l2cap(2, chan->our_cid, sabm, sizeof sabm);
    ble_br_test_util_verify_tx_l2cap(2, chan->peer_cid, ua, sizeof ua);

    /*** UIH frames. */
    ble_br_test_util_rx_l2cap(2, chan->our_cid, test_cmd_bad_fcs,
                              sizeof test_cmd_bad_fcs);
    ble_br_test_util_verify_tx_none();

    /* A checksum that covers the length field as well. */
    memcpy(frame, test_cmd, sizeof frame);
    frame[sizeof frame - 1] = ble_br_test_util_rfcomm_fcs(frame, 3);
    TEST_ASSERT_FATAL(frame[sizeof frame - 1] != 0x70);
    ble_br_test_util_rx_l2cap(2, chan->our_cid, frame, sizeof frame);
    ble_br_test_util_verify_tx_none();

    ble_br_test_util_rx_l2cap(2, chan->our_cid, test_cmd, sizeof test_cmd);
    ble_br_test_util_verify_tx_l2cap(2, chan->peer_cid, test_rsp, sizeof test_rsp);

    /*** Frames whose length is not the one they tell are dropped. */
    ble_br_test_util_rx_l2cap(2, chan->our_cid, test_cmd, sizeof test_cmd - 1);
    ble_br_test_util_rx_l2cap(2, chan->our_cid, sabm, sizeof sabm - 1);
    memcpy(frame, sabm, sizeof sabm);
    frame[sizeof sabm] = 0x00;
    ble_br_test_util_rx_l2cap(2, chan->our_cid, frame, sizeof sabm + 1);
    ble_br_test_util_verify_tx_none();

    /*** A link nothing listens on is refused. */
    ble_br_test_util_rx_l2cap(2, chan->our_cid, sabm_dlci, sizeof sabm_dlci);
    ble_br_test_util_verify_tx_l2cap(2, chan->peer_cid, dm_dlci, sizeof dm_dlci);

    /*** The session ends. */
    ble_br_test_util_rx_l2cap(2, chan->our_cid, disc, sizeof disc);
    ble_br_test_util_verify_tx_l2cap(2, chan->peer_cid, ua, sizeof ua);
    ble_br_test_util_l2cap_verify_tx_disconnect(chan);

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_rfcomm_test_incoming)
{
    static const uint8_t rpn_default[] = {
        RFCOMM_ADDR(BLE_BR_RFCOMM_TEST_DLCI, 1), 0x03, 0x03, 0x00, 0x11, 0x13, 0x00, 0x00
    };
    static const uint8_t rpn_set[] = {
        RFCOMM_ADDR(BLE_BR_RFCOMM_TEST_DLCI, 1), 0x07, 0x03, 0x00, 0x11, 0x13, 0x7f, 0x3f
    };
    static const uint8_t rls[] = { RFCOMM_ADDR(BLE_BR_RFCOMM_TEST_DLCI, 1), 0x01 };
    struct ble_br_test_util_chan *chan;
    const uint8_t *cmd;
    uint8_t msc[2];
    uint8_t type;
    uint8_t dlci;

    ble_br_rfcomm_test_util_init();

    chan = &ble_br_rfcomm_test_chan;
    dlci = BLE_BR_RFCOMM_TEST_DLCI;

    ble_br_test_util_rfcomm_rx_session(chan, 2);

    /* One session per connection. */
    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_CONN_REQ,
                            ble_br_test_util_next_ident(),
                            ((uint8_t[]){ 0x03, 0x00, 0x51, 0x00 }), 4);
    cmd = ble_br_test_util_verify_tx_sig(2, BLE_BR_TEST_UTIL_SIG_CONN_RSP, NULL, 8);
    TEST_ASSERT(get_le16(cmd + 0) == 0);
    TEST_ASSERT(get_le16(cmd + 4) == 4);

    /*** Links that are refused. */

    /* Nothing listens on the channel. */
    ble_br_rfcomm_test_util_rx_pn(RFCOMM_ADDR(0, 1), 1, dlci + 2, 0xf0, 127, 7);
    ble_br_rfcomm_test_util_verify_tx_ctrl(RFCOMM_ADDR(dlci + 2, 1), RFCOMM_DM);

    /* The server is on the other side of the direction bit. */
    ble_br_rfcomm_test_util_rx_pn(RFCOMM_ADDR(0, 1), 1, dlci + 1, 0xf0, 127, 7);
    ble_br_rfcomm_test_util_verify_tx_ctrl(RFCOMM_ADDR(dlci + 1, 1), RFCOMM_DM);

    /* The server does not take the link. */
    ble_br_rfcomm_test_accept_rc = BLE_HS_EBUSY;
    ble_br_rfcomm_test_util_rx_pn(RFCOMM_ADDR(0, 1), 1, dlci, 0xf0, 127, 7);
    ble_br_rfcomm_test_util_verify_tx_ctrl(RFCOMM_ADDR(dlci, 1), RFCOMM_DM);
    ble_br_rfcomm_test_util_rx_ctrl(RFCOMM_ADDR(dlci, 1), RFCOMM_SABM, RFCOMM_DM);
    ble_br_rfcomm_test_accept_rc = 0;

    TEST_ASSERT(ble_br_rfcomm_test_connected_cnt == 0);

    /*** The peer asks for a larger frame size than the host takes. */
    ble_br_rfcomm_test_util_rx_pn(RFCOMM_ADDR(0, 1), 1, dlci, 0xf0, 300, 5);
    ble_br_rfcomm_test_util_verify_tx_pn(RFCOMM_ADDR(0, 0), 0, dlci, 0xe0,
                                         MYNEWT_VAL(BLE_BREDR_RFCOMM_MTU),
                                         BLE_BR_TEST_UTIL_RFCOMM_CREDITS);
    TEST_ASSERT(ble_br_rfcomm_test_connected_cnt == 0);

    /* The parameters can be negotiated again. */
    ble_br_rfcomm_test_util_rx_pn(RFCOMM_ADDR(0, 1), 1, dlci, 0xf0, 50, 5);
    ble_br_rfcomm_test_util_verify_tx_pn(RFCOMM_ADDR(0, 0), 0, dlci, 0xe0, 50,
                                         BLE_BR_TEST_UTIL_RFCOMM_CREDITS);

    /*** The link opens; the host tells its modem status. */
    ble_br_rfcomm_test_util_rx_ctrl(RFCOMM_ADDR(dlci, 1), RFCOMM_SABM, RFCOMM_UA);

    msc[0] = RFCOMM_ADDR(dlci, 1);
    msc[1] = 0x8d;
    ble_br_test_util_rfcomm_verify_tx_mcc(
        chan, RFCOMM_ADDR(0, 0), RFCOMM_MCC(RFCOMM_MCC_MSC, 1), msc, sizeof msc);

    TEST_ASSERT(ble_br_rfcomm_test_connected_cnt == 1);
    TEST_ASSERT(ble_br_rfcomm_test_connected_status == 0);
    TEST_ASSERT_FATAL(ble_br_rfcomm_test_dlc != NULL);
    TEST_ASSERT(ble_br_rfcomm_dlc_conn_handle(ble_br_rfcomm_test_dlc) == 2);

    /* A link is opened once. */
    ble_br_rfcomm_test_util_rx_ctrl(RFCOMM_ADDR(dlci, 1), RFCOMM_SABM, RFCOMM_DM);
    TEST_ASSERT(ble_br_rfcomm_test_connected_cnt == 1);

    /*** Multiplexer commands. */

    /* Modem status: the response of the peer, then its command. */
    ble_br_test_util_rfcomm_rx_mcc(chan, RFCOMM_ADDR(0, 1),
                                   RFCOMM_MCC(RFCOMM_MCC_MSC, 0), msc, sizeof msc);
    ble_br_test_util_verify_tx_none();

    ble_br_test_util_rfcomm_rx_mcc(chan, RFCOMM_ADDR(0, 1),
                                   RFCOMM_MCC(RFCOMM_MCC_MSC, 1), msc, sizeof msc);
    ble_br_test_util_rfcomm_verify_tx_mcc(
        chan, RFCOMM_ADDR(0, 0), RFCOMM_MCC(RFCOMM_MCC_MSC, 0), msc, sizeof msc);

    /* Port settings: asked for, then set. */
    ble_br_test_util_rfcomm_rx_mcc(chan, RFCOMM_ADDR(0, 1),
                                   RFCOMM_MCC(RFCOMM_MCC_RPN, 1), rpn_default, 1);
    ble_br_test_util_rfcomm_verify_tx_mcc(chan, RFCOMM_ADDR(0, 0),
                                          RFCOMM_MCC(RFCOMM_MCC_RPN, 0),
                                          rpn_default, sizeof rpn_default);

    ble_br_test_util_rfcomm_rx_mcc(chan, RFCOMM_ADDR(0, 1),
                                   RFCOMM_MCC(RFCOMM_MCC_RPN, 1), rpn_set,
                                   sizeof rpn_set);
    ble_br_test_util_rfcomm_verify_tx_mcc(chan, RFCOMM_ADDR(0, 0),
                                          RFCOMM_MCC(RFCOMM_MCC_RPN, 0),
                                          rpn_set, sizeof rpn_set);

    /* Line status. */
    ble_br_test_util_rfcomm_rx_mcc(chan, RFCOMM_ADDR(0, 1),
                                   RFCOMM_MCC(0x14, 1), rls, sizeof rls);
    ble_br_test_util_rfcomm_verify_tx_mcc(chan, RFCOMM_ADDR(0, 0),
                                          RFCOMM_MCC(0x14, 0), rls, sizeof rls);

    /* A command the host does not know. */
    type = RFCOMM_MCC(0x30, 1);
    ble_br_test_util_rfcomm_rx_mcc(chan, RFCOMM_ADDR(0, 1), type, NULL, 0);
    ble_br_test_util_rfcomm_verify_tx_mcc(chan, RFCOMM_ADDR(0, 0),
                                          RFCOMM_MCC(RFCOMM_MCC_NSC, 0), &type, 1);

    /* Responses nothing waits for and malformed commands are ignored. */
    ble_br_test_util_rfcomm_rx_mcc(chan, RFCOMM_ADDR(0, 1),
                                   RFCOMM_MCC(0x30, 0), NULL, 0);
    ble_br_test_util_rfcomm_rx_mcc(chan, RFCOMM_ADDR(0, 1),
                                   RFCOMM_MCC(RFCOMM_MCC_NSC, 0), &type, 1);
    ble_br_test_util_rfcomm_rx_mcc(chan, RFCOMM_ADDR(0, 1),
                                   RFCOMM_MCC(RFCOMM_MCC_PN, 1), rpn_set, 7);
    ble_br_test_util_rfcomm_rx(chan, RFCOMM_ADDR(0, 1), RFCOMM_UIH, -1,
                               ((uint8_t[]){ RFCOMM_MCC(0x08, 1), 0x09, 'a' }), 3);
    ble_br_test_util_rfcomm_rx(chan, RFCOMM_ADDR(0, 1), RFCOMM_UIH, -1,
                               ((uint8_t[]){ RFCOMM_MCC(0x08, 1) }), 1);
    ble_br_test_util_verify_tx_none();

    /*** A peer that skips the parameter negotiation. */
    ble_hs_test_util_hci_rx_disconn_complete_event(2, 0, BLE_ERR_CONN_SPVN_TMO);
    TEST_ASSERT(ble_br_rfcomm_test_disconnected_cnt == 1);
    ble_br_test_util_gap_event_next();
    ble_br_test_util_create_conn(2, ble_br_rfcomm_test_addr, 1);

    ble_br_test_util_rfcomm_rx_session(chan, 2);
    ble_br_rfcomm_test_util_rx_ctrl(RFCOMM_ADDR(dlci, 1), RFCOMM_SABM, RFCOMM_UA);
    ble_br_test_util_rfcomm_verify_tx_mcc(
        chan, RFCOMM_ADDR(0, 0), RFCOMM_MCC(RFCOMM_MCC_MSC, 1), msc, sizeof msc);
    TEST_ASSERT(ble_br_rfcomm_test_connected_cnt == 2);
    TEST_ASSERT(ble_br_rfcomm_test_connected_status == 0);

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_rfcomm_test_credits_rx)
{
    struct ble_br_test_util_chan *chan;
    uint8_t data[MYNEWT_VAL(BLE_BREDR_RFCOMM_MTU)];
    uint16_t len;
    uint8_t dlci;
    int i;

    ble_br_rfcomm_test_util_init();
    ble_br_rfcomm_test_util_rx_open(0);

    chan = &ble_br_rfcomm_test_chan;
    dlci = BLE_BR_RFCOMM_TEST_DLCI;

    for (i = 0; i < sizeof data; i++) {
        data[i] = i;
    }

    /* The peer got seven credits; the host gives the ones that got used
     * back when two are left.
     */
    for (i = 1; i <= 12; i++) {
        ble_br_test_util_rfcomm_rx(chan, RFCOMM_ADDR(dlci, 1), RFCOMM_UIH, -1,
                                   data, i);

        TEST_ASSERT(ble_br_rfcomm_test_rx_cnt == i);
        TEST_ASSERT(ble_br_rfcomm_test_rx_len == i);
        TEST_ASSERT(memcmp(ble_br_rfcomm_test_rx_buf, data, i) == 0);

        if (i % 5 == 0) {
            ble_br_test_util_rfcomm_verify_tx(chan, RFCOMM_ADDR(dlci, 0),
                                              RFCOMM_UIH_PF, 5, &len);
            TEST_ASSERT(len == 0);
        }
        ble_br_test_util_verify_tx_none();
    }

    /*** A frame of the largest size. */
    ble_br_test_util_rfcomm_rx(chan, RFCOMM_ADDR(dlci, 1), RFCOMM_UIH, -1,
                               data, sizeof data);
    TEST_ASSERT(ble_br_rfcomm_test_rx_cnt == 13);
    TEST_ASSERT(ble_br_rfcomm_test_rx_len == sizeof data);
    TEST_ASSERT(memcmp(ble_br_rfcomm_test_rx_buf, data, sizeof data) == 0);

    /*** Data with credits for the host. */
    ble_br_test_util_rfcomm_rx(chan, RFCOMM_ADDR(dlci, 1), RFCOMM_UIH_PF, 3, data, 10);
    TEST_ASSERT(ble_br_rfcomm_test_rx_cnt == 14);
    TEST_ASSERT(ble_br_rfcomm_test_rx_len == 10);
    TEST_ASSERT(memcmp(ble_br_rfcomm_test_rx_buf, data, 10) == 0);

    /*** Frames without data take no credit. */
    ble_br_test_util_rfcomm_rx(chan, RFCOMM_ADDR(dlci, 1), RFCOMM_UIH_PF, 3, NULL, 0);
    ble_br_test_util_rfcomm_rx(chan, RFCOMM_ADDR(dlci, 1), RFCOMM_UIH, -1, NULL, 0);
    TEST_ASSERT(ble_br_rfcomm_test_rx_cnt == 14);
    ble_br_test_util_verify_tx_none();

    ble_br_test_util_rfcomm_rx(chan, RFCOMM_ADDR(dlci, 1), RFCOMM_UIH, -1, data, 1);
    TEST_ASSERT(ble_br_rfcomm_test_rx_cnt == 15);
    ble_br_test_util_rfcomm_verify_tx(chan, RFCOMM_ADDR(dlci, 0),
                                      RFCOMM_UIH_PF, 5, &len);
    TEST_ASSERT(len == 0);

    /*** Data for a link that is not open. */
    ble_br_test_util_rfcomm_rx(chan, RFCOMM_ADDR(dlci + 2, 1), RFCOMM_UIH, -1,
                               data, 1);
    ble_br_rfcomm_test_util_verify_tx_ctrl(RFCOMM_ADDR(dlci + 2, 1), RFCOMM_DM);
    TEST_ASSERT(ble_br_rfcomm_test_rx_cnt == 15);

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_rfcomm_test_credits_tx)
{
    struct ble_br_test_util_chan *chan;
    uint8_t data[MYNEWT_VAL(BLE_BREDR_RFCOMM_MTU) + 1];
    uint8_t dlci;
    int rc;
    int i;

    ble_br_rfcomm_test_util_init();

    /* The peer gives a credit. */
    ble_br_rfcomm_test_util_rx_open(1);

    chan = &ble_br_rfcomm_test_chan;
    dlci = BLE_BR_RFCOMM_TEST_DLCI;

    for (i = 0; i < sizeof data; i++) {
        data[i] = i;
    }

    /*** A frame takes a credit. */
    rc = ble_br_rfcomm_send(ble_br_rfcomm_test_dlc, data, 3);
    TEST_ASSERT(rc == 0);
    ble_br_rfcomm_test_util_verify_tx_data(RFCOMM_ADDR(dlci, 0), data, 3);

    /*** Out of credits: a frame waits, and no other can be sent. */
    rc = ble_br_rfcomm_send(ble_br_rfcomm_test_dlc, data, 4);
    TEST_ASSERT(rc == 0);
    ble_br_test_util_verify_tx_none();

    rc = ble_br_rfcomm_send(ble_br_rfcomm_test_dlc, data, 5);
    TEST_ASSERT(rc == BLE_HS_EBUSY);

    /*** The peer gives two credits: the frame that waits is sent. */
    ble_br_test_util_rfcomm_rx(chan, RFCOMM_ADDR(dlci, 1), RFCOMM_UIH_PF, 2, NULL, 0);
    ble_br_rfcomm_test_util_verify_tx_data(RFCOMM_ADDR(dlci, 0), data, 4);

    rc = ble_br_rfcomm_send(ble_br_rfcomm_test_dlc, data, 5);
    TEST_ASSERT(rc == 0);
    ble_br_rfcomm_test_util_verify_tx_data(RFCOMM_ADDR(dlci, 0), data, 5);

    rc = ble_br_rfcomm_send(ble_br_rfcomm_test_dlc, data, 6);
    TEST_ASSERT(rc == 0);
    ble_br_test_util_verify_tx_none();

    /*** The peer gives a credit along with data. */
    ble_br_test_util_rfcomm_rx(chan, RFCOMM_ADDR(dlci, 1), RFCOMM_UIH_PF, 1, data, 1);
    ble_br_rfcomm_test_util_verify_tx_data(RFCOMM_ADDR(dlci, 0), data, 6);
    TEST_ASSERT(ble_br_rfcomm_test_rx_cnt == 1);

    /*** Frame sizes. */
    ble_br_test_util_rfcomm_rx(chan, RFCOMM_ADDR(dlci, 1), RFCOMM_UIH_PF, 2, NULL, 0);

    rc = ble_br_rfcomm_send(ble_br_rfcomm_test_dlc, data, 0);
    TEST_ASSERT(rc == BLE_HS_EMSGSIZE);
    rc = ble_br_rfcomm_send(ble_br_rfcomm_test_dlc, data, sizeof data);
    TEST_ASSERT(rc == BLE_HS_EMSGSIZE);
    ble_br_test_util_verify_tx_none();

    rc = ble_br_rfcomm_send(ble_br_rfcomm_test_dlc, data, sizeof data - 1);
    TEST_ASSERT(rc == 0);
    ble_br_rfcomm_test_util_verify_tx_data(RFCOMM_ADDR(dlci, 0), data,
                                           sizeof data - 1);

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_rfcomm_test_no_credits)
{
    struct ble_br_test_util_chan *chan;
    uint8_t data[60];
    uint8_t msc[2];
    uint8_t dlci;
    int rc;
    int i;

    ble_br_rfcomm_test_util_init();

    chan = &ble_br_rfcomm_test_chan;
    dlci = BLE_BR_RFCOMM_TEST_DLCI;

    memset(data, 0xa5, sizeof data);

    /* The peer does not ask for credit based flow control, and takes
     * frames of 50 bytes.
     */
    ble_br_test_util_rfcomm_rx_session(chan, 2);
    ble_br_rfcomm_test_util_rx_pn(RFCOMM_ADDR(0, 1), 1, dlci, 0x00, 50, 0);
    ble_br_rfcomm_test_util_verify_tx_pn(RFCOMM_ADDR(0, 0), 0, dlci, 0x00, 50, 0);
    ble_br_rfcomm_test_util_rx_ctrl(RFCOMM_ADDR(dlci, 1), RFCOMM_SABM, RFCOMM_UA);

    msc[0] = RFCOMM_ADDR(dlci, 1);
    msc[1] = 0x8d;
    ble_br_test_util_rfcomm_verify_tx_mcc(
        chan, RFCOMM_ADDR(0, 0), RFCOMM_MCC(RFCOMM_MCC_MSC, 1), msc, sizeof msc);
    TEST_ASSERT_FATAL(ble_br_rfcomm_test_dlc != NULL);

    /* Nothing holds the host back. */
    for (i = 0; i < 20; i++) {
        rc = ble_br_rfcomm_send(ble_br_rfcomm_test_dlc, data, 50);
        TEST_ASSERT(rc == 0);
        ble_br_rfcomm_test_util_verify_tx_data(RFCOMM_ADDR(dlci, 0), data, 50);
    }

    rc = ble_br_rfcomm_send(ble_br_rfcomm_test_dlc, data, 51);
    TEST_ASSERT(rc == BLE_HS_EMSGSIZE);

    /* Nor the peer. */
    for (i = 0; i < 20; i++) {
        ble_br_test_util_rfcomm_rx(chan, RFCOMM_ADDR(dlci, 1), RFCOMM_UIH, -1,
                                   data, 50);
        TEST_ASSERT(ble_br_rfcomm_test_rx_cnt == i + 1);
        TEST_ASSERT(ble_br_rfcomm_test_rx_len == 50);
    }

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_rfcomm_test_disconnect)
{
    struct ble_br_test_util_chan *chan;
    struct ble_br_rfcomm_dlc *dlc;
    uint8_t buf[4];
    uint8_t dlci;
    int rc;

    ble_br_rfcomm_test_util_init();

    chan = &ble_br_rfcomm_test_chan;
    dlci = BLE_BR_RFCOMM_TEST_DLCI;

    /*** The peer closes the link, then the session. */
    ble_br_rfcomm_test_util_rx_open(7);
    dlc = ble_br_rfcomm_test_dlc;

    ble_br_rfcomm_test_util_rx_ctrl(RFCOMM_ADDR(dlci, 1), RFCOMM_DISC, RFCOMM_UA);
    TEST_ASSERT(ble_br_rfcomm_test_disconnected_cnt == 1);
    TEST_ASSERT(ble_br_rfcomm_dlc_conn_handle(dlc) == BLE_HS_CONN_HANDLE_NONE);
    ble_br_test_util_verify_tx_none();

    rc = ble_br_rfcomm_send(dlc, buf, sizeof buf);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);
    rc = ble_br_rfcomm_disconnect(dlc);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);

    ble_br_rfcomm_test_util_rx_ctrl(RFCOMM_ADDR(0, 1), RFCOMM_DISC, RFCOMM_UA);
    ble_br_test_util_l2cap_verify_tx_disconnect(chan);

    /*** The peer drops the link: nothing is left for the session to do. */
    ble_br_rfcomm_test_util_rx_open(7);

    ble_br_test_util_rfcomm_rx(chan, RFCOMM_ADDR(dlci, 1), RFCOMM_DM, -1, NULL, 0);
    TEST_ASSERT(ble_br_rfcomm_test_disconnected_cnt == 2);
    ble_br_test_util_l2cap_verify_tx_disconnect(chan);

    /*** The peer drops the session. */
    ble_br_rfcomm_test_util_rx_open(7);

    ble_br_test_util_rfcomm_rx(chan, RFCOMM_ADDR(0, 1), RFCOMM_DM, -1, NULL, 0);
    TEST_ASSERT(ble_br_rfcomm_test_disconnected_cnt == 3);
    ble_br_test_util_l2cap_verify_tx_disconnect(chan);

    /*** The peer closes the channel of the session. */
    ble_br_rfcomm_test_util_rx_open(7);

    put_le16(buf + 0, chan->our_cid);
    put_le16(buf + 2, chan->peer_cid);
    ble_br_test_util_rx_sig(2, BLE_BR_TEST_UTIL_SIG_DISCONN_REQ,
                            ble_br_test_util_next_ident(), buf, sizeof buf);
    ble_br_test_util_verify_tx_sig(2, BLE_BR_TEST_UTIL_SIG_DISCONN_RSP, NULL, 4);
    TEST_ASSERT(ble_br_rfcomm_test_disconnected_cnt == 4);

    /*** We close the link; the application is not told about what it
     * asked.
     */
    ble_br_rfcomm_test_util_rx_open(7);

    rc = ble_br_rfcomm_disconnect(ble_br_rfcomm_test_dlc);
    TEST_ASSERT(rc == 0);
    ble_br_rfcomm_test_util_verify_tx_ctrl(RFCOMM_ADDR(dlci, 0), RFCOMM_DISC);
    ble_br_test_util_l2cap_verify_tx_disconnect(chan);
    TEST_ASSERT(ble_br_rfcomm_test_disconnected_cnt == 4);

    rc = ble_br_rfcomm_disconnect(ble_br_rfcomm_test_dlc);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);

    /*** The connection is lost. */
    ble_br_rfcomm_test_util_rx_open(7);

    ble_hs_test_util_hci_rx_disconn_complete_event(2, 0, BLE_ERR_CONN_SPVN_TMO);
    TEST_ASSERT(ble_br_rfcomm_test_disconnected_cnt == 5);
    TEST_ASSERT(ble_br_rfcomm_test_connected_cnt == 6);

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_rfcomm_test_outgoing)
{
    static const uint8_t sabm[] = { 0x03, 0x3f, 0x01, 0x1c };
    struct ble_br_test_util_chan *chan;
    struct ble_br_rfcomm_dlc *dlc2;
    struct ble_br_rfcomm_dlc *dlc;
    uint8_t data[101];
    uint8_t msc[2];
    uint16_t len;
    int rc;

    ble_br_rfcomm_test_util_init();

    chan = &ble_br_rfcomm_test_chan;
    memset(data, 0x5a, sizeof data);

    rc = ble_br_rfcomm_connect(2, 0, &ble_br_rfcomm_test_ops,
                               &ble_br_rfcomm_test_cb_arg, &dlc);
    TEST_ASSERT(rc == BLE_HS_EINVAL);
    rc = ble_br_rfcomm_connect(2, 31, &ble_br_rfcomm_test_ops,
                               &ble_br_rfcomm_test_cb_arg, &dlc);
    TEST_ASSERT(rc == BLE_HS_EINVAL);
    rc = ble_br_rfcomm_connect(3, 4, &ble_br_rfcomm_test_ops,
                               &ble_br_rfcomm_test_cb_arg, &dlc);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);
    ble_br_test_util_verify_tx_none();

    /*** The host starts the session. */
    rc = ble_br_rfcomm_connect(2, 4, &ble_br_rfcomm_test_ops,
                               &ble_br_rfcomm_test_cb_arg, &dlc);
    TEST_ASSERT_FATAL(rc == 0);

    ble_br_test_util_l2cap_rx_connect_rsp(chan, 2, BLE_BR_L2CAP_PSM_RFCOMM, 0x0060);
    ble_br_test_util_verify_tx_l2cap(2, chan->peer_cid, sabm, sizeof sabm);
    ble_br_test_util_verify_tx_none();

    ble_br_test_util_rfcomm_rx(chan, RFCOMM_ADDR(0, 1), RFCOMM_UA, -1, NULL, 0);

    /*** The host negotiates the parameters of the link, then opens it. */
    ble_br_rfcomm_test_util_verify_tx_pn(RFCOMM_ADDR(0, 1), 1, 8, 0xf0,
                                         MYNEWT_VAL(BLE_BREDR_RFCOMM_MTU),
                                         BLE_BR_TEST_UTIL_RFCOMM_CREDITS);
    ble_br_test_util_verify_tx_none();

    /* The peer takes frames of 100 bytes and gives three credits. */
    ble_br_rfcomm_test_util_rx_pn(RFCOMM_ADDR(0, 0), 0, 8, 0xe0, 100, 3);

    ble_br_rfcomm_test_util_verify_tx_ctrl(RFCOMM_ADDR(8, 1), RFCOMM_SABM);
    TEST_ASSERT(ble_br_rfcomm_test_connected_cnt == 0);

    ble_br_test_util_rfcomm_rx(chan, RFCOMM_ADDR(8, 1), RFCOMM_UA, -1, NULL, 0);

    msc[0] = RFCOMM_ADDR(8, 1);
    msc[1] = 0x8d;
    ble_br_test_util_rfcomm_verify_tx_mcc(
        chan, RFCOMM_ADDR(0, 1), RFCOMM_MCC(RFCOMM_MCC_MSC, 1), msc, sizeof msc);

    TEST_ASSERT(ble_br_rfcomm_test_connected_cnt == 1);
    TEST_ASSERT(ble_br_rfcomm_test_connected_status == 0);
    TEST_ASSERT(ble_br_rfcomm_test_dlc == dlc);
    TEST_ASSERT(ble_br_rfcomm_dlc_conn_handle(dlc) == 2);

    /*** Data, in the frame size of the peer. */
    rc = ble_br_rfcomm_send(dlc, data, 101);
    TEST_ASSERT(rc == BLE_HS_EMSGSIZE);

    rc = ble_br_rfcomm_send(dlc, data, 100);
    TEST_ASSERT(rc == 0);
    ble_br_rfcomm_test_util_verify_tx_data(RFCOMM_ADDR(8, 1), data, 100);

    ble_br_test_util_rfcomm_rx(chan, RFCOMM_ADDR(8, 0), RFCOMM_UIH, -1, data, 20);
    TEST_ASSERT(ble_br_rfcomm_test_rx_cnt == 1);
    TEST_ASSERT(ble_br_rfcomm_test_rx_len == 20);

    /*** A second link, on the session that is there. */
    rc = ble_br_rfcomm_connect(2, 5, &ble_br_rfcomm_test_ops,
                               &ble_br_rfcomm_test_cb_arg, &dlc2);
    TEST_ASSERT_FATAL(rc == 0);

    ble_br_rfcomm_test_util_verify_tx_pn(RFCOMM_ADDR(0, 1), 1, 10, 0xf0,
                                         MYNEWT_VAL(BLE_BREDR_RFCOMM_MTU),
                                         BLE_BR_TEST_UTIL_RFCOMM_CREDITS);

    /* No more links. */
    rc = ble_br_rfcomm_connect(2, 6, &ble_br_rfcomm_test_ops,
                               &ble_br_rfcomm_test_cb_arg, NULL);
    TEST_ASSERT(rc == BLE_HS_ENOMEM);

    /* The peer refuses the link. */
    ble_br_test_util_rfcomm_rx(chan, RFCOMM_ADDR(10, 1), RFCOMM_DM, -1, NULL, 0);
    TEST_ASSERT(ble_br_rfcomm_test_connected_cnt == 2);
    TEST_ASSERT(ble_br_rfcomm_test_connected_status == BLE_HS_EREJECT);
    TEST_ASSERT(ble_br_rfcomm_test_dlc == dlc2);
    TEST_ASSERT(ble_br_rfcomm_test_disconnected_cnt == 0);
    ble_br_test_util_verify_tx_none();

    /* The peer wants frames larger than the host proposed. */
    rc = ble_br_rfcomm_connect(2, 5, &ble_br_rfcomm_test_ops,
                               &ble_br_rfcomm_test_cb_arg, &dlc2);
    TEST_ASSERT_FATAL(rc == 0);
    ble_br_rfcomm_test_util_verify_tx_pn(RFCOMM_ADDR(0, 1), 1, 10, 0xf0,
                                         MYNEWT_VAL(BLE_BREDR_RFCOMM_MTU),
                                         BLE_BR_TEST_UTIL_RFCOMM_CREDITS);

    ble_br_rfcomm_test_util_rx_pn(RFCOMM_ADDR(0, 0), 0, 10, 0xe0,
                                  MYNEWT_VAL(BLE_BREDR_RFCOMM_MTU) + 1, 3);
    TEST_ASSERT(ble_br_rfcomm_test_connected_cnt == 3);
    TEST_ASSERT(ble_br_rfcomm_test_connected_status == BLE_HS_EREJECT);
    ble_br_test_util_verify_tx_none();

    /*** We close the link, and the session with it. */
    rc = ble_br_rfcomm_disconnect(dlc);
    TEST_ASSERT(rc == 0);

    ble_br_test_util_rfcomm_verify_tx(chan, RFCOMM_ADDR(8, 1), RFCOMM_DISC, -1, &len);
    TEST_ASSERT(len == 0);
    ble_br_test_util_l2cap_verify_tx_disconnect(chan);
    TEST_ASSERT(ble_br_rfcomm_test_disconnected_cnt == 0);

    /*** The peer refuses the session. */
    ble_br_rfcomm_test_util_connect(4, &dlc);

    ble_br_test_util_rfcomm_rx(chan, RFCOMM_ADDR(0, 1), RFCOMM_DM, -1, NULL, 0);
    TEST_ASSERT(ble_br_rfcomm_test_connected_cnt == 4);
    TEST_ASSERT(ble_br_rfcomm_test_connected_status == BLE_HS_EREJECT);
    TEST_ASSERT(ble_br_rfcomm_test_dlc == dlc);
    ble_br_test_util_l2cap_verify_tx_disconnect(chan);

    /*** The peer refuses the link, the only one of the session. */
    ble_br_rfcomm_test_util_connect(4, &dlc);
    ble_br_test_util_rfcomm_rx(chan, RFCOMM_ADDR(0, 1), RFCOMM_UA, -1, NULL, 0);
    ble_br_rfcomm_test_util_verify_tx_pn(RFCOMM_ADDR(0, 1), 1, 8, 0xf0,
                                         MYNEWT_VAL(BLE_BREDR_RFCOMM_MTU),
                                         BLE_BR_TEST_UTIL_RFCOMM_CREDITS);

    ble_br_test_util_rfcomm_rx(chan, RFCOMM_ADDR(8, 1), RFCOMM_DM, -1, NULL, 0);
    TEST_ASSERT(ble_br_rfcomm_test_connected_cnt == 5);
    TEST_ASSERT(ble_br_rfcomm_test_connected_status == BLE_HS_EREJECT);
    ble_br_test_util_l2cap_verify_tx_disconnect(chan);

    /*** The peer has a server of its own on the session of the host. */
    ble_br_rfcomm_test_util_open(4, 7, &dlc);
    TEST_ASSERT(ble_br_rfcomm_test_connected_cnt == 6);
    TEST_ASSERT(ble_br_rfcomm_test_connected_status == 0);

    /* The servers of the host have the direction bit set then. */
    ble_br_rfcomm_test_util_rx_pn(RFCOMM_ADDR(0, 0), 1,
                                  BLE_BR_RFCOMM_TEST_DLCI, 0xf0, 127, 7);
    ble_br_rfcomm_test_util_verify_tx_ctrl(RFCOMM_ADDR(BLE_BR_RFCOMM_TEST_DLCI, 0),
                                           RFCOMM_DM);

    ble_br_rfcomm_test_util_rx_pn(RFCOMM_ADDR(0, 0), 1,
                                  BLE_BR_RFCOMM_TEST_DLCI + 1, 0xf0, 127, 7);
    ble_br_rfcomm_test_util_verify_tx_pn(RFCOMM_ADDR(0, 1), 0,
                                         BLE_BR_RFCOMM_TEST_DLCI + 1, 0xe0,
                                         127, BLE_BR_TEST_UTIL_RFCOMM_CREDITS);

    ble_br_rfcomm_test_util_rx_ctrl(RFCOMM_ADDR(BLE_BR_RFCOMM_TEST_DLCI + 1, 0),
                                    RFCOMM_SABM, RFCOMM_UA);
    msc[0] = RFCOMM_ADDR(BLE_BR_RFCOMM_TEST_DLCI + 1, 1);
    ble_br_test_util_rfcomm_verify_tx_mcc(
        chan, RFCOMM_ADDR(0, 1), RFCOMM_MCC(RFCOMM_MCC_MSC, 1), msc, sizeof msc);
    TEST_ASSERT(ble_br_rfcomm_test_connected_cnt == 7);
    TEST_ASSERT(ble_br_rfcomm_test_connected_status == 0);

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_rfcomm_test_timeout)
{
    struct ble_br_test_util_chan *chan;
    struct ble_br_rfcomm_dlc *dlc;
    int32_t ticks_from_now;
    int32_t timeout_ticks;
    uint8_t dlci;

    ble_br_rfcomm_test_util_init();

    chan = &ble_br_rfcomm_test_chan;
    dlci = BLE_BR_RFCOMM_TEST_DLCI;
    timeout_ticks = ble_npl_time_ms_to_ticks32(BLE_BR_RFCOMM_TEST_TIMEOUT_MS);

    /* Ensure timer is not set. */
    ticks_from_now = ble_br_test_util_time_advance(0);
    TEST_ASSERT(ticks_from_now == BLE_HS_FOREVER);

    /*** The peer does not start the session the host asks for. */
    ble_br_rfcomm_test_util_connect(4, &dlc);

    /* Ensure timer will expire in 20 seconds. */
    ticks_from_now = ble_br_test_util_time_advance(0);
    TEST_ASSERT(ticks_from_now == timeout_ticks);

    /* Almost let the timer expire. */
    ticks_from_now = ble_br_test_util_time_advance(timeout_ticks - 1);
    TEST_ASSERT(ticks_from_now == 1);
    TEST_ASSERT(ble_br_rfcomm_test_connected_cnt == 0);

    /* Allow the timer to expire. */
    ticks_from_now = ble_br_test_util_time_advance(1);
    TEST_ASSERT(ticks_from_now == BLE_HS_FOREVER);
    TEST_ASSERT(ble_br_rfcomm_test_connected_cnt == 1);
    TEST_ASSERT(ble_br_rfcomm_test_connected_status == BLE_HS_ETIMEOUT);
    TEST_ASSERT(ble_br_rfcomm_test_dlc == dlc);
    ble_br_test_util_l2cap_verify_tx_disconnect(chan);

    /*** The peer does not answer the parameter negotiation. */
    ble_br_rfcomm_test_util_connect(4, &dlc);
    ble_br_test_util_rfcomm_rx(chan, RFCOMM_ADDR(0, 1), RFCOMM_UA, -1, NULL, 0);
    ble_br_rfcomm_test_util_verify_tx_pn(RFCOMM_ADDR(0, 1), 1, 8, 0xf0,
                                         MYNEWT_VAL(BLE_BREDR_RFCOMM_MTU),
                                         BLE_BR_TEST_UTIL_RFCOMM_CREDITS);

    ticks_from_now = ble_br_test_util_time_advance(timeout_ticks - 1);
    TEST_ASSERT(ticks_from_now == 1);
    TEST_ASSERT(ble_br_rfcomm_test_connected_cnt == 1);

    ticks_from_now = ble_br_test_util_time_advance(1);
    TEST_ASSERT(ticks_from_now == BLE_HS_FOREVER);
    TEST_ASSERT(ble_br_rfcomm_test_connected_cnt == 2);
    TEST_ASSERT(ble_br_rfcomm_test_connected_status == BLE_HS_ETIMEOUT);
    ble_br_test_util_l2cap_verify_tx_disconnect(chan);

    /*** The peer does not open the link. */
    ble_br_rfcomm_test_util_connect(4, &dlc);
    ble_br_test_util_rfcomm_rx(chan, RFCOMM_ADDR(0, 1), RFCOMM_UA, -1, NULL, 0);
    ble_br_rfcomm_test_util_verify_tx_pn(RFCOMM_ADDR(0, 1), 1, 8, 0xf0,
                                         MYNEWT_VAL(BLE_BREDR_RFCOMM_MTU),
                                         BLE_BR_TEST_UTIL_RFCOMM_CREDITS);
    ble_br_rfcomm_test_util_rx_pn(RFCOMM_ADDR(0, 0), 0, 8, 0xe0, 100, 3);
    ble_br_rfcomm_test_util_verify_tx_ctrl(RFCOMM_ADDR(8, 1), RFCOMM_SABM);

    ticks_from_now = ble_br_test_util_time_advance(timeout_ticks);
    TEST_ASSERT(ticks_from_now == BLE_HS_FOREVER);
    TEST_ASSERT(ble_br_rfcomm_test_connected_cnt == 3);
    TEST_ASSERT(ble_br_rfcomm_test_connected_status == BLE_HS_ETIMEOUT);
    ble_br_test_util_l2cap_verify_tx_disconnect(chan);

    /*** The peer does not start the session it connected for. */
    ble_br_test_util_l2cap_rx_connect(chan, 2, BLE_BR_L2CAP_PSM_RFCOMM, 0x0050);

    ticks_from_now = ble_br_test_util_time_advance(0);
    TEST_ASSERT(ticks_from_now == timeout_ticks);

    ticks_from_now = ble_br_test_util_time_advance(timeout_ticks);
    TEST_ASSERT(ticks_from_now == BLE_HS_FOREVER);
    ble_br_test_util_l2cap_verify_tx_disconnect(chan);
    TEST_ASSERT(ble_br_rfcomm_test_connected_cnt == 3);

    /*** The peer does not open the link it negotiated. */
    ble_br_test_util_rfcomm_rx_session(chan, 2);
    ble_br_rfcomm_test_util_rx_pn(RFCOMM_ADDR(0, 1), 1, dlci, 0xf0, 127, 7);
    ble_br_rfcomm_test_util_verify_tx_pn(RFCOMM_ADDR(0, 0), 0, dlci, 0xe0, 127,
                                         BLE_BR_TEST_UTIL_RFCOMM_CREDITS);

    ticks_from_now = ble_br_test_util_time_advance(0);
    TEST_ASSERT(ticks_from_now == timeout_ticks);

    ticks_from_now = ble_br_test_util_time_advance(timeout_ticks);
    TEST_ASSERT(ticks_from_now == BLE_HS_FOREVER);
    TEST_ASSERT(ble_br_rfcomm_test_connected_cnt == 4);
    TEST_ASSERT(ble_br_rfcomm_test_connected_status == BLE_HS_ETIMEOUT);
    ble_br_test_util_l2cap_verify_tx_disconnect(chan);

    /*** An open link never times out. */
    ble_br_rfcomm_test_util_rx_open(7);
    TEST_ASSERT(ble_br_rfcomm_test_connected_cnt == 5);

    ticks_from_now = ble_br_test_util_time_advance(2 * timeout_ticks);
    TEST_ASSERT(ticks_from_now == BLE_HS_FOREVER);
    TEST_ASSERT(ble_br_rfcomm_test_connected_cnt == 5);
    TEST_ASSERT(ble_br_rfcomm_test_disconnected_cnt == 0);

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

#endif

TEST_SUITE(ble_br_rfcomm_test_suite)
{
#if MYNEWT_VAL(BLE_BREDR)
    ble_br_rfcomm_test_fcs();
    ble_br_rfcomm_test_incoming();
    ble_br_rfcomm_test_credits_rx();
    ble_br_rfcomm_test_credits_tx();
    ble_br_rfcomm_test_no_credits();
    ble_br_rfcomm_test_disconnect();
    ble_br_rfcomm_test_outgoing();
    ble_br_rfcomm_test_timeout();
#endif
}
