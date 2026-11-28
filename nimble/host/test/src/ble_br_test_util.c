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
#include "ble_hs_test_util.h"
#include "ble_br_test_util.h"

#if MYNEWT_VAL(BLE_BREDR)

#define BLE_BR_TEST_UTIL_GAP_EVENT_MAX 16
#define BLE_BR_TEST_UTIL_PDU_MAX       1024

static struct ble_br_gap_event ble_br_test_util_gap_events[BLE_BR_TEST_UTIL_GAP_EVENT_MAX];
static int ble_br_test_util_gap_event_head;
static int ble_br_test_util_gap_event_tail;

static uint16_t ble_br_test_util_acl_len;
static uint8_t ble_br_test_util_tx_buf[BLE_BR_TEST_UTIL_PDU_MAX];
static uint8_t ble_br_test_util_ident;

#if MYNEWT_VAL(BLE_BREDR_SCO)
static uint8_t ble_br_test_util_sco_tx_buf[BLE_HCI_SCO_HDR_SZ + 255];
static uint16_t ble_br_test_util_sco_tx_len;
static int ble_br_test_util_sco_tx_count;
#endif

/*****************************************************************************
 * $gap events                                                               *
 *****************************************************************************/

static void
ble_br_test_util_gap_event_cb(const struct ble_br_gap_event *event, void *arg)
{
    TEST_ASSERT_FATAL(ble_br_test_util_gap_event_tail < BLE_BR_TEST_UTIL_GAP_EVENT_MAX);

    ble_br_test_util_gap_events[ble_br_test_util_gap_event_tail++] = *event;
}

/**
 * Retrieves the oldest event the application has not looked at yet.
 */
const struct ble_br_gap_event *
ble_br_test_util_gap_event_next(void)
{
    if (ble_br_test_util_gap_event_head == ble_br_test_util_gap_event_tail) {
        return NULL;
    }

    return &ble_br_test_util_gap_events[ble_br_test_util_gap_event_head++];
}

int
ble_br_test_util_gap_event_cnt(void)
{
    return ble_br_test_util_gap_event_tail - ble_br_test_util_gap_event_head;
}

/*****************************************************************************
 * $hci                                                                      *
 *****************************************************************************/

void
ble_br_test_util_rx_evt(uint8_t code, const void *params, uint8_t len)
{
    struct ble_hci_ev *ev;
    int rc;

    ev = ble_transport_alloc_evt(0);
    TEST_ASSERT_FATAL(ev != NULL);

    ev->opcode = code;
    ev->length = len;
    memcpy(ev->data, params, len);

    rc = ble_hs_hci_evt_process(ev);
    TEST_ASSERT_FATAL(rc == 0);
}

void
ble_br_test_util_ack_set(uint16_t ocf, uint8_t status)
{
    ble_hs_test_util_hci_ack_set(BLE_BR_TEST_UTIL_LC_OPCODE(ocf), status);
}

/**
 * Acknowledges a command whose return parameters are the address it took.
 */
void
ble_br_test_util_ack_set_addr(uint16_t ocf, const uint8_t *addr)
{
    uint8_t params[BLE_DEV_ADDR_LEN];

    memcpy(params, addr, sizeof params);
    ble_hs_test_util_hci_ack_set_params(BLE_BR_TEST_UTIL_LC_OPCODE(ocf), 0,
                                        params, sizeof params);
}

/**
 * Verifies a command that only takes an address got sent.
 */
void
ble_br_test_util_verify_tx_addr(uint16_t ocf, const uint8_t *addr)
{
    uint8_t param_len;
    uint8_t *param;

    param = ble_hs_test_util_hci_verify_tx(BLE_HCI_OGF_LINK_CTRL, ocf, &param_len);
    TEST_ASSERT(param_len == BLE_DEV_ADDR_LEN);
    TEST_ASSERT(memcmp(param, addr, BLE_DEV_ADDR_LEN) == 0);
}

void
ble_br_test_util_rx_conn_request(const uint8_t *addr, uint8_t link_type)
{
    struct ble_hci_ev_conn_request ev;

    memset(&ev, 0, sizeof ev);
    memcpy(ev.bd_addr, addr, BLE_DEV_ADDR_LEN);
    ev.link_type = link_type;

    ble_br_test_util_rx_evt(BLE_HCI_EVCODE_CONN_REQUEST, &ev, sizeof ev);
}

void
ble_br_test_util_rx_conn_cmp(uint8_t status, uint16_t conn_handle,
                             const uint8_t *addr, uint8_t link_type, uint8_t encrypted)
{
    struct ble_hci_ev_conn_done ev;

    ev.status = status;
    ev.conn_handle = htole16(conn_handle);
    memcpy(ev.bd_addr, addr, BLE_DEV_ADDR_LEN);
    ev.link_type = link_type;
    ev.encryption_enabled = encrypted;

    ble_br_test_util_rx_evt(BLE_HCI_EVCODE_CONN_DONE, &ev, sizeof ev);
}

void
ble_br_test_util_rx_enc_change(uint16_t conn_handle, uint8_t status, uint8_t enabled)
{
    struct ble_hci_ev_enrypt_chg ev;

    ev.status = status;
    ev.connection_handle = htole16(conn_handle);
    ev.enabled = enabled;

    ble_br_test_util_rx_evt(BLE_HCI_EVCODE_ENCRYPT_CHG, &ev, sizeof ev);
}

/**
 * Has the peer connect to the host, which accepts the connection.
 */
void
ble_br_test_util_create_conn(uint16_t conn_handle, const uint8_t *addr, int encrypted)
{
    const struct ble_br_gap_event *event;
    uint8_t param_len;
    uint8_t *param;

    ble_br_test_util_ack_set(BLE_HCI_OCF_ACCEPT_CONN_REQ, 0);
    ble_br_test_util_rx_conn_request(addr, BLE_HCI_LINK_TYPE_ACL);

    param = ble_hs_test_util_hci_verify_tx(
        BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_ACCEPT_CONN_REQ, &param_len);
    TEST_ASSERT(param_len == sizeof(struct ble_hci_lc_accept_conn_req_cp));
    TEST_ASSERT(memcmp(param, addr, BLE_DEV_ADDR_LEN) == 0);
    TEST_ASSERT(param[6] == BLE_HCI_ROLE_PERIPHERAL);

    ble_br_test_util_rx_conn_cmp(0, conn_handle, addr, BLE_HCI_LINK_TYPE_ACL, encrypted);

    event = ble_br_test_util_gap_event_next();
    TEST_ASSERT_FATAL(event != NULL);
    TEST_ASSERT(event->type == BLE_BR_GAP_EVENT_CONNECT);
    TEST_ASSERT(event->connect.status == 0);
    TEST_ASSERT(event->connect.conn_handle == conn_handle);
    TEST_ASSERT(memcmp(event->connect.addr, addr, BLE_DEV_ADDR_LEN) == 0);
}

void
ble_br_test_util_rx_num_completed_pkts(uint16_t conn_handle, uint16_t num_pkts)
{
    struct ble_hs_test_util_hci_num_completed_pkts_entry entries[] = {
        { conn_handle, num_pkts },
        { 0 },
    };

    ble_hs_test_util_hci_rx_num_completed_pkts_event(entries);
}

/*****************************************************************************
 * $l2cap                                                                    *
 *****************************************************************************/

int
ble_br_test_util_rx_acl(uint16_t conn_handle, uint8_t pb, const void *data, uint16_t len)
{
    struct hci_data_hdr hci_hdr;
    struct os_mbuf *om;
    int rc;

    om = ble_hs_mbuf_acl_pkt();
    TEST_ASSERT_FATAL(om != NULL);

    hci_hdr.hdh_handle_pb_bc =
        htole16(ble_hs_hci_util_handle_pb_bc_join(conn_handle, pb, 0));
    hci_hdr.hdh_len = htole16(len);

    rc = os_mbuf_append(om, &hci_hdr, sizeof hci_hdr);
    TEST_ASSERT_FATAL(rc == 0);
    rc = os_mbuf_append(om, data, len);
    TEST_ASSERT_FATAL(rc == 0);

    return ble_hs_hci_evt_acl_process(om);
}

/**
 * Receives an L2CAP PDU that fits in a single ACL data packet.
 */
void
ble_br_test_util_rx_l2cap(uint16_t conn_handle, uint16_t cid, const void *data,
                          uint16_t len)
{
    uint8_t buf[BLE_BR_TEST_UTIL_PDU_MAX];
    int rc;

    TEST_ASSERT_FATAL(BLE_L2CAP_HDR_SZ + len <= sizeof buf);

    put_le16(buf + 0, len);
    put_le16(buf + 2, cid);
    memcpy(buf + BLE_L2CAP_HDR_SZ, data, len);

    rc = ble_br_test_util_rx_acl(conn_handle, BLE_HCI_PB_FIRST_FLUSH, buf,
                                 BLE_L2CAP_HDR_SZ + len);
    TEST_ASSERT_FATAL(rc == 0);
}

void
ble_br_test_util_rx_sig(uint16_t conn_handle, uint8_t code, uint8_t ident,
                        const void *data, uint16_t len)
{
    uint8_t buf[BLE_BR_TEST_UTIL_PDU_MAX];

    TEST_ASSERT_FATAL(4 + len <= sizeof buf);

    buf[0] = code;
    buf[1] = ident;
    put_le16(buf + 2, len);
    memcpy(buf + 4, data, len);

    ble_br_test_util_rx_l2cap(conn_handle, BLE_BR_TEST_UTIL_CID_SIG, buf, 4 + len);
}

/**
 * Retrieves the identifier for the next signaling request of the peer.
 */
uint8_t
ble_br_test_util_next_ident(void)
{
    if (++ble_br_test_util_ident == 0) {
        ble_br_test_util_ident = 1;
    }

    return ble_br_test_util_ident;
}

/**
 * Removes the oldest L2CAP PDU the host sent from the transmit queue.  The
 * PDU is put together from the ACL data packets it got fragmented into.
 *
 * @return                      The payload of the PDU; NULL if nothing was
 *                                  sent.
 */
const uint8_t *
ble_br_test_util_tx_dequeue(uint16_t *out_conn_handle, uint16_t *out_cid,
                            uint16_t *out_len)
{
    struct hci_data_hdr hci_hdr;
    struct os_mbuf *om;
    uint16_t conn_handle;
    uint16_t frag_len;
    uint16_t off;
    uint8_t pb;
    int rc;

    conn_handle = BLE_HS_CONN_HANDLE_NONE;
    off = 0;

    do {
        om = ble_hs_test_util_prev_tx_dequeue_once(&hci_hdr);
        if (om == NULL) {
            TEST_ASSERT_FATAL(off == 0);
            return NULL;
        }

        pb = BLE_HCI_DATA_PB(hci_hdr.hdh_handle_pb_bc);
        if (off == 0) {
            TEST_ASSERT_FATAL(pb == BLE_HCI_PB_FIRST_FLUSH);
            conn_handle = BLE_HCI_DATA_HANDLE(hci_hdr.hdh_handle_pb_bc);
        } else {
            TEST_ASSERT_FATAL(pb == BLE_HCI_PB_MIDDLE);
            TEST_ASSERT_FATAL(BLE_HCI_DATA_HANDLE(hci_hdr.hdh_handle_pb_bc) ==
                              conn_handle);
        }

        frag_len = OS_MBUF_PKTLEN(om);
        TEST_ASSERT_FATAL(frag_len > 0);
        TEST_ASSERT_FATAL(frag_len <= ble_br_test_util_acl_len);
        TEST_ASSERT_FATAL(off + frag_len <= sizeof ble_br_test_util_tx_buf);

        rc = os_mbuf_copydata(om, 0, frag_len, ble_br_test_util_tx_buf + off);
        TEST_ASSERT_FATAL(rc == 0);
        off += frag_len;

        rc = os_mbuf_free_chain(om);
        TEST_ASSERT_FATAL(rc == 0);
    } while (off < BLE_L2CAP_HDR_SZ ||
             off < BLE_L2CAP_HDR_SZ + get_le16(ble_br_test_util_tx_buf));

    TEST_ASSERT_FATAL(off == BLE_L2CAP_HDR_SZ + get_le16(ble_br_test_util_tx_buf));

    *out_conn_handle = conn_handle;
    *out_cid = get_le16(ble_br_test_util_tx_buf + 2);
    *out_len = off - BLE_L2CAP_HDR_SZ;

    return ble_br_test_util_tx_buf + BLE_L2CAP_HDR_SZ;
}

void
ble_br_test_util_verify_tx_l2cap(uint16_t conn_handle, uint16_t cid,
                                 const void *data, uint16_t len)
{
    const uint8_t *pdu;
    uint16_t pdu_handle;
    uint16_t pdu_cid;
    uint16_t pdu_len;

    pdu = ble_br_test_util_tx_dequeue(&pdu_handle, &pdu_cid, &pdu_len);
    TEST_ASSERT_FATAL(pdu != NULL);
    TEST_ASSERT(pdu_handle == conn_handle);
    TEST_ASSERT(pdu_cid == cid);
    TEST_ASSERT_FATAL(pdu_len == len);
    TEST_ASSERT(memcmp(pdu, data, len) == 0);
}

/**
 * Verifies the host sent a signaling command.
 *
 * @param len                   The length the command is expected to have.
 *
 * @return                      The data of the command.
 */
const uint8_t *
ble_br_test_util_verify_tx_sig(uint16_t conn_handle, uint8_t code,
                               uint8_t *out_ident, uint16_t len)
{
    const uint8_t *pdu;
    uint16_t pdu_handle;
    uint16_t pdu_cid;
    uint16_t pdu_len;

    pdu = ble_br_test_util_tx_dequeue(&pdu_handle, &pdu_cid, &pdu_len);
    TEST_ASSERT_FATAL(pdu != NULL);
    TEST_ASSERT(pdu_handle == conn_handle);
    TEST_ASSERT(pdu_cid == BLE_BR_TEST_UTIL_CID_SIG);
    TEST_ASSERT_FATAL(pdu_len == 4 + len);
    TEST_ASSERT_FATAL(pdu[0] == code);
    TEST_ASSERT(pdu[1] != 0);
    TEST_ASSERT(get_le16(pdu + 2) == len);

    if (out_ident != NULL) {
        *out_ident = pdu[1];
    }

    return pdu + 4;
}

/**
 * Drops what the host sent, whole PDUs or not.
 */
void
ble_br_test_util_tx_queue_clear(void)
{
    struct hci_data_hdr hci_hdr;
    struct os_mbuf *om;

    while ((om = ble_hs_test_util_prev_tx_dequeue_once(&hci_hdr)) != NULL) {
        os_mbuf_free_chain(om);
    }
}

void
ble_br_test_util_verify_tx_none(void)
{
    TEST_ASSERT(ble_hs_test_util_prev_tx_queue_sz() == 0);
}

/**
 * Configures a channel in both directions: the peer accepts the request of
 * the host, then sends its own.
 */
void
ble_br_test_util_l2cap_config(const struct ble_br_test_util_chan *chan)
{
    const uint8_t *cmd;
    uint8_t buf[8];
    uint8_t ident;

    /* Ensure the host asks for its MTU. */
    cmd = ble_br_test_util_verify_tx_sig(
        chan->conn_handle, BLE_BR_TEST_UTIL_SIG_CONFIG_REQ, &ident, 8);
    TEST_ASSERT(get_le16(cmd + 0) == chan->peer_cid);
    TEST_ASSERT(get_le16(cmd + 2) == 0);
    TEST_ASSERT(cmd[4] == 0x01);
    TEST_ASSERT(cmd[5] == 2);
    TEST_ASSERT(get_le16(cmd + 6) == MYNEWT_VAL(BLE_BREDR_L2CAP_MTU));

    put_le16(buf + 0, chan->our_cid);
    put_le16(buf + 2, 0);
    put_le16(buf + 4, 0);
    ble_br_test_util_rx_sig(chan->conn_handle, BLE_BR_TEST_UTIL_SIG_CONFIG_RSP,
                            ident, buf, 6);

    ident = ble_br_test_util_next_ident();
    put_le16(buf + 0, chan->our_cid);
    put_le16(buf + 2, 0);
    buf[4] = 0x01;
    buf[5] = 2;
    put_le16(buf + 6, 672);
    ble_br_test_util_rx_sig(chan->conn_handle, BLE_BR_TEST_UTIL_SIG_CONFIG_REQ,
                            ident, buf, 8);

    cmd = ble_br_test_util_verify_tx_sig(
        chan->conn_handle, BLE_BR_TEST_UTIL_SIG_CONFIG_RSP, &ident, 6);
    TEST_ASSERT(ident == ble_br_test_util_ident);
    TEST_ASSERT(get_le16(cmd + 0) == chan->peer_cid);
    TEST_ASSERT(get_le16(cmd + 2) == 0);
    TEST_ASSERT(get_le16(cmd + 4) == 0);
}

/**
 * Has the peer open a channel to a server of the host.
 */
void
ble_br_test_util_l2cap_rx_connect(struct ble_br_test_util_chan *chan,
                                  uint16_t conn_handle, uint16_t psm, uint16_t peer_cid)
{
    const uint8_t *cmd;
    uint8_t buf[4];
    uint8_t ident;

    put_le16(buf + 0, psm);
    put_le16(buf + 2, peer_cid);
    ble_br_test_util_rx_sig(conn_handle, BLE_BR_TEST_UTIL_SIG_CONN_REQ,
                            ble_br_test_util_next_ident(), buf, 4);

    cmd = ble_br_test_util_verify_tx_sig(conn_handle,
                                         BLE_BR_TEST_UTIL_SIG_CONN_RSP, &ident, 8);
    TEST_ASSERT(ident == ble_br_test_util_ident);
    TEST_ASSERT(get_le16(cmd + 0) >= 0x0040);
    TEST_ASSERT(get_le16(cmd + 2) == peer_cid);
    TEST_ASSERT(get_le16(cmd + 4) == 0);
    TEST_ASSERT(get_le16(cmd + 6) == 0);

    chan->conn_handle = conn_handle;
    chan->our_cid = get_le16(cmd + 0);
    chan->peer_cid = peer_cid;

    ble_br_test_util_l2cap_config(chan);
}

/**
 * Has the peer accept the channel the host just asked for.
 */
void
ble_br_test_util_l2cap_rx_connect_rsp(struct ble_br_test_util_chan *chan,
                                      uint16_t conn_handle, uint16_t psm,
                                      uint16_t peer_cid)
{
    const uint8_t *cmd;
    uint8_t buf[8];
    uint8_t ident;

    cmd = ble_br_test_util_verify_tx_sig(conn_handle,
                                         BLE_BR_TEST_UTIL_SIG_CONN_REQ, &ident, 4);
    TEST_ASSERT(get_le16(cmd + 0) == psm);
    TEST_ASSERT(get_le16(cmd + 2) >= 0x0040);

    chan->conn_handle = conn_handle;
    chan->our_cid = get_le16(cmd + 2);
    chan->peer_cid = peer_cid;

    put_le16(buf + 0, chan->peer_cid);
    put_le16(buf + 2, chan->our_cid);
    put_le16(buf + 4, 0);
    put_le16(buf + 6, 0);
    ble_br_test_util_rx_sig(conn_handle, BLE_BR_TEST_UTIL_SIG_CONN_RSP, ident, buf, 8);

    ble_br_test_util_l2cap_config(chan);
}

/**
 * Verifies the host asked to close a channel.
 */
void
ble_br_test_util_l2cap_verify_tx_disconnect(const struct ble_br_test_util_chan *chan)
{
    const uint8_t *cmd;

    cmd = ble_br_test_util_verify_tx_sig(
        chan->conn_handle, BLE_BR_TEST_UTIL_SIG_DISCONN_REQ, NULL, 4);
    TEST_ASSERT(get_le16(cmd + 0) == chan->peer_cid);
    TEST_ASSERT(get_le16(cmd + 2) == chan->our_cid);
}

/*****************************************************************************
 * $rfcomm                                                                   *
 *****************************************************************************/

uint8_t
ble_br_test_util_rfcomm_fcs(const uint8_t *data, int len)
{
    uint8_t fcs;
    int i;
    int j;

    /* CRC with the polynomial x^8 + x^2 + x + 1, bits reversed. */
    fcs = 0xff;
    for (i = 0; i < len; i++) {
        fcs ^= data[i];
        for (j = 0; j < 8; j++) {
            if (fcs & 0x01) {
                fcs = (fcs >> 1) ^ 0xe0;
            } else {
                fcs >>= 1;
            }
        }
    }

    return 0xff - fcs;
}

/**
 * Receives an RFCOMM frame.
 *
 * @param credits               The credits the frame carries; negative if it
 *                                  carries none.
 */
void
ble_br_test_util_rfcomm_rx(const struct ble_br_test_util_chan *chan, uint8_t addr,
                           uint8_t ctrl, int credits, const void *data, uint16_t len)
{
    uint8_t buf[BLE_BR_TEST_UTIL_PDU_MAX];
    int fcs_len;
    int off;

    TEST_ASSERT_FATAL(len + 6 <= sizeof buf);

    off = 0;
    buf[off++] = addr;
    buf[off++] = ctrl;
    if (len <= 127) {
        buf[off++] = (len << 1) | 0x01;
    } else {
        buf[off++] = len << 1;
        buf[off++] = len >> 7;
    }

    /* The checksum of a UIH frame does not cover the length. */
    if ((ctrl & ~BLE_BR_TEST_UTIL_RFCOMM_PF) == BLE_BR_TEST_UTIL_RFCOMM_UIH) {
        fcs_len = 2;
    } else {
        fcs_len = off;
    }

    if (credits >= 0) {
        buf[off++] = credits;
    }
    memcpy(buf + off, data, len);
    off += len;
    buf[off++] = ble_br_test_util_rfcomm_fcs(buf, fcs_len);

    ble_br_test_util_rx_l2cap(chan->conn_handle, chan->our_cid, buf, off);
}

/**
 * Verifies the host sent an RFCOMM frame.
 *
 * @param credits               The credits the frame is expected to carry;
 *                                  negative if none.
 *
 * @return                      The information field of the frame.
 */
const uint8_t *
ble_br_test_util_rfcomm_verify_tx(const struct ble_br_test_util_chan *chan,
                                  uint8_t addr, uint8_t ctrl, int credits,
                                  uint16_t *out_len)
{
    const uint8_t *pdu;
    uint16_t pdu_handle;
    uint16_t pdu_cid;
    uint16_t pdu_len;
    uint16_t len;
    int fcs_len;
    int off;

    pdu = ble_br_test_util_tx_dequeue(&pdu_handle, &pdu_cid, &pdu_len);
    TEST_ASSERT_FATAL(pdu != NULL);
    TEST_ASSERT(pdu_handle == chan->conn_handle);
    TEST_ASSERT(pdu_cid == chan->peer_cid);
    TEST_ASSERT_FATAL(pdu_len >= 4);
    TEST_ASSERT_FATAL(pdu[0] == addr);
    TEST_ASSERT_FATAL(pdu[1] == ctrl);

    off = 2;
    len = pdu[off] >> 1;
    if (!(pdu[off++] & 0x01)) {
        len |= pdu[off++] << 7;
    }

    if ((ctrl & ~BLE_BR_TEST_UTIL_RFCOMM_PF) == BLE_BR_TEST_UTIL_RFCOMM_UIH) {
        fcs_len = 2;
    } else {
        fcs_len = off;
    }

    if (credits >= 0) {
        TEST_ASSERT_FATAL(pdu_len == off + 1 + len + 1);
        TEST_ASSERT(pdu[off] == credits);
        off++;
    } else {
        TEST_ASSERT_FATAL(pdu_len == off + len + 1);
    }

    TEST_ASSERT(pdu[off + len] == ble_br_test_util_rfcomm_fcs(pdu, fcs_len));

    if (out_len != NULL) {
        *out_len = len;
    }

    return pdu + off;
}

/**
 * Receives a multiplexer command or response, on DLCI 0.
 *
 * @param addr                  The address field of the frame.
 * @param type                  The type field of the command or response.
 */
void
ble_br_test_util_rfcomm_rx_mcc(const struct ble_br_test_util_chan *chan, uint8_t addr,
                               uint8_t type, const void *data, uint8_t len)
{
    uint8_t buf[2 + 127];

    TEST_ASSERT_FATAL(len <= 127);

    buf[0] = type;
    buf[1] = (len << 1) | 0x01;
    memcpy(buf + 2, data, len);

    ble_br_test_util_rfcomm_rx(chan, addr, BLE_BR_TEST_UTIL_RFCOMM_UIH, -1,
                               buf, 2 + len);
}

void
ble_br_test_util_rfcomm_verify_tx_mcc(const struct ble_br_test_util_chan *chan,
                                      uint8_t addr, uint8_t type,
                                      const void *data, uint8_t len)
{
    const uint8_t *info;
    uint16_t info_len;

    info = ble_br_test_util_rfcomm_verify_tx(
        chan, addr, BLE_BR_TEST_UTIL_RFCOMM_UIH, -1, &info_len);
    TEST_ASSERT_FATAL(info_len == 2 + len);
    TEST_ASSERT(info[0] == type);
    TEST_ASSERT(info[1] == ((len << 1) | 0x01));
    TEST_ASSERT(memcmp(info + 2, data, len) == 0);
}

/**
 * Has the peer start an RFCOMM session with the host.  The connection has
 * to be encrypted.
 */
void
ble_br_test_util_rfcomm_rx_session(struct ble_br_test_util_chan *chan, uint16_t conn_handle)
{
    uint16_t len;

    ble_br_test_util_l2cap_rx_connect(chan, conn_handle,
                                      BLE_BR_L2CAP_PSM_RFCOMM, 0x0050);

    ble_br_test_util_rfcomm_rx(
        chan, BLE_BR_TEST_UTIL_RFCOMM_ADDR(0, 1),
        BLE_BR_TEST_UTIL_RFCOMM_SABM | BLE_BR_TEST_UTIL_RFCOMM_PF, -1, NULL, 0);
    ble_br_test_util_rfcomm_verify_tx(
        chan, BLE_BR_TEST_UTIL_RFCOMM_ADDR(0, 1),
        BLE_BR_TEST_UTIL_RFCOMM_UA | BLE_BR_TEST_UTIL_RFCOMM_PF, -1, &len);
    TEST_ASSERT(len == 0);
}

/**
 * Has the peer, which started the session, open a link to a server of the
 * host.
 *
 * @param credits               The credits the peer gives the host.
 */
void
ble_br_test_util_rfcomm_rx_open(const struct ble_br_test_util_chan *chan,
                                uint8_t dlci, uint8_t credits)
{
    uint8_t pn[8];
    uint8_t msc[2];
    uint16_t len;

    /* Parameter negotiation, with credit based flow control. */
    memset(pn, 0, sizeof pn);
    pn[0] = dlci;
    pn[1] = 0xf0;
    put_le16(pn + 4, MYNEWT_VAL(BLE_BREDR_RFCOMM_MTU));
    pn[7] = credits;
    ble_br_test_util_rfcomm_rx_mcc(
        chan, BLE_BR_TEST_UTIL_RFCOMM_ADDR(0, 1),
        BLE_BR_TEST_UTIL_RFCOMM_MCC(BLE_BR_TEST_UTIL_RFCOMM_MCC_PN, 1), pn,
        sizeof pn);

    pn[1] = 0xe0;
    pn[7] = BLE_BR_TEST_UTIL_RFCOMM_CREDITS;
    ble_br_test_util_rfcomm_verify_tx_mcc(
        chan, BLE_BR_TEST_UTIL_RFCOMM_ADDR(0, 0),
        BLE_BR_TEST_UTIL_RFCOMM_MCC(BLE_BR_TEST_UTIL_RFCOMM_MCC_PN, 0), pn,
        sizeof pn);

    ble_br_test_util_rfcomm_rx(
        chan, BLE_BR_TEST_UTIL_RFCOMM_ADDR(dlci, 1),
        BLE_BR_TEST_UTIL_RFCOMM_SABM | BLE_BR_TEST_UTIL_RFCOMM_PF, -1, NULL, 0);
    ble_br_test_util_rfcomm_verify_tx(
        chan, BLE_BR_TEST_UTIL_RFCOMM_ADDR(dlci, 1),
        BLE_BR_TEST_UTIL_RFCOMM_UA | BLE_BR_TEST_UTIL_RFCOMM_PF, -1, &len);
    TEST_ASSERT(len == 0);

    /* The host tells the state of its side of the link. */
    msc[0] = BLE_BR_TEST_UTIL_RFCOMM_ADDR(dlci, 1);
    msc[1] = 0x8d;
    ble_br_test_util_rfcomm_verify_tx_mcc(
        chan, BLE_BR_TEST_UTIL_RFCOMM_ADDR(0, 0),
        BLE_BR_TEST_UTIL_RFCOMM_MCC(BLE_BR_TEST_UTIL_RFCOMM_MCC_MSC, 1), msc,
        sizeof msc);
}

/**
 * Has the peer accept the session and the link the host just asked for.
 *
 * @param credits               The credits the peer gives the host.
 */
void
ble_br_test_util_rfcomm_rx_accept(struct ble_br_test_util_chan *chan,
                                  uint16_t conn_handle, uint8_t dlci, uint8_t credits)
{
    uint8_t pn[8];
    uint8_t msc[2];
    uint16_t len;

    ble_br_test_util_l2cap_rx_connect_rsp(chan, conn_handle,
                                          BLE_BR_L2CAP_PSM_RFCOMM, 0x0060);

    ble_br_test_util_rfcomm_verify_tx(
        chan, BLE_BR_TEST_UTIL_RFCOMM_ADDR(0, 1),
        BLE_BR_TEST_UTIL_RFCOMM_SABM | BLE_BR_TEST_UTIL_RFCOMM_PF, -1, &len);
    TEST_ASSERT(len == 0);
    ble_br_test_util_rfcomm_rx(chan, BLE_BR_TEST_UTIL_RFCOMM_ADDR(0, 1),
                               BLE_BR_TEST_UTIL_RFCOMM_UA | BLE_BR_TEST_UTIL_RFCOMM_PF,
                               -1, NULL, 0);

    /* Parameter negotiation, with credit based flow control. */
    memset(pn, 0, sizeof pn);
    pn[0] = dlci;
    pn[1] = 0xf0;
    put_le16(pn + 4, MYNEWT_VAL(BLE_BREDR_RFCOMM_MTU));
    pn[7] = BLE_BR_TEST_UTIL_RFCOMM_CREDITS;
    ble_br_test_util_rfcomm_verify_tx_mcc(
        chan, BLE_BR_TEST_UTIL_RFCOMM_ADDR(0, 1),
        BLE_BR_TEST_UTIL_RFCOMM_MCC(BLE_BR_TEST_UTIL_RFCOMM_MCC_PN, 1), pn,
        sizeof pn);

    pn[1] = 0xe0;
    pn[7] = credits;
    ble_br_test_util_rfcomm_rx_mcc(
        chan, BLE_BR_TEST_UTIL_RFCOMM_ADDR(0, 0),
        BLE_BR_TEST_UTIL_RFCOMM_MCC(BLE_BR_TEST_UTIL_RFCOMM_MCC_PN, 0), pn,
        sizeof pn);

    ble_br_test_util_rfcomm_verify_tx(
        chan, BLE_BR_TEST_UTIL_RFCOMM_ADDR(dlci, 1),
        BLE_BR_TEST_UTIL_RFCOMM_SABM | BLE_BR_TEST_UTIL_RFCOMM_PF, -1, &len);
    TEST_ASSERT(len == 0);
    ble_br_test_util_rfcomm_rx(chan, BLE_BR_TEST_UTIL_RFCOMM_ADDR(dlci, 1),
                               BLE_BR_TEST_UTIL_RFCOMM_UA | BLE_BR_TEST_UTIL_RFCOMM_PF,
                               -1, NULL, 0);

    /* The host tells the state of its side of the link. */
    msc[0] = BLE_BR_TEST_UTIL_RFCOMM_ADDR(dlci, 1);
    msc[1] = 0x8d;
    ble_br_test_util_rfcomm_verify_tx_mcc(
        chan, BLE_BR_TEST_UTIL_RFCOMM_ADDR(0, 1),
        BLE_BR_TEST_UTIL_RFCOMM_MCC(BLE_BR_TEST_UTIL_RFCOMM_MCC_MSC, 1), msc,
        sizeof msc);
}

#if MYNEWT_VAL(BLE_BREDR_SCO)

/*****************************************************************************
 * $sco                                                                      *
 *****************************************************************************/

void
ble_br_test_util_rx_synch_conn_cmp(uint8_t status, uint16_t sco_handle,
                                   const uint8_t *addr, uint8_t link_type)
{
    struct ble_hci_ev_synch_conn_comp ev;

    memset(&ev, 0, sizeof ev);
    ev.status = status;
    ev.conn_handle = htole16(sco_handle);
    memcpy(ev.bd_addr, addr, BLE_DEV_ADDR_LEN);
    ev.link_type = link_type;
    ev.tx_interval = 12;
    ev.retrans_window = 2;
    ev.rx_pkt_len = htole16(60);
    ev.tx_pkt_len = htole16(60);

    ble_br_test_util_rx_evt(BLE_HCI_EVCODE_SYNCH_CONN_COMP, &ev, sizeof ev);
}

/**
 * Verifies the host accepted a synchronous connection, for CVSD at 8 kHz.
 */
void
ble_br_test_util_verify_tx_accept_synch_conn(const uint8_t *addr,
                                             const struct ble_br_sco_params *params)
{
    uint8_t param_len;
    uint8_t *param;

    param = ble_hs_test_util_hci_verify_tx(
        BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_ACCEPT_SYNCH_CONN_REQ, &param_len);
    TEST_ASSERT(param_len == sizeof(struct ble_hci_lc_accept_synch_conn_req_cp));
    TEST_ASSERT(memcmp(param, addr, BLE_DEV_ADDR_LEN) == 0);
    TEST_ASSERT(get_le32(param + 6) == 8000);
    TEST_ASSERT(get_le32(param + 10) == 8000);
    TEST_ASSERT(get_le16(param + 14) == params->max_latency);
    TEST_ASSERT(get_le16(param + 16) == BLE_HCI_VOICE_SETTING_PCM16_CVSD);
    TEST_ASSERT(param[18] == params->retrans_effort);
    TEST_ASSERT(get_le16(param + 19) == params->pkt_type);
}

int
ble_br_test_util_sco_tx_cnt(void)
{
    return ble_br_test_util_sco_tx_count;
}

/**
 * Retrieves the last synchronous data packet the host sent.
 */
const uint8_t *
ble_br_test_util_sco_tx_last(uint16_t *out_len)
{
    *out_len = ble_br_test_util_sco_tx_len;

    return ble_br_test_util_sco_tx_buf;
}

int
ble_transport_to_ll_sco_impl(const uint8_t *pkt, uint16_t len)
{
    TEST_ASSERT_FATAL(len <= sizeof ble_br_test_util_sco_tx_buf);

    memcpy(ble_br_test_util_sco_tx_buf, pkt, len);
    ble_br_test_util_sco_tx_len = len;
    ble_br_test_util_sco_tx_count++;

    return 0;
}

#endif

/*****************************************************************************
 * $time                                                                     *
 *****************************************************************************/

/**
 * Lets time pass and runs the timers of the BR/EDR host.
 *
 * @return                      The ticks until the next timer expires.
 */
int32_t
ble_br_test_util_time_advance(int32_t ticks)
{
    os_time_advance(ticks);

    return ble_br_hs_timer();
}

/*****************************************************************************
 * $init                                                                     *
 *****************************************************************************/

/**
 * Starts the host on a dual-mode controller with the given ACL buffers.
 *
 * @param shared                Whether LE uses the same buffers.
 */
void
ble_br_test_util_init_buf(uint16_t acl_len, uint16_t acl_pkts, int shared)
{
    ble_hs_test_util_hci_set_bredr(acl_len, acl_pkts, shared);
    ble_hs_test_util_init();
    ble_hs_test_util_hci_set_bredr(0, 0, 0);

    TEST_ASSERT_FATAL(ble_hs_synced());
    TEST_ASSERT_FATAL(ble_br_gap_supported());

    ble_br_test_util_acl_len = acl_len;
    ble_br_test_util_ident = 0;
    ble_br_test_util_gap_event_head = 0;
    ble_br_test_util_gap_event_tail = 0;
#if MYNEWT_VAL(BLE_BREDR_SCO)
    ble_br_test_util_sco_tx_len = 0;
    ble_br_test_util_sco_tx_count = 0;
#endif

    ble_br_gap_set_event_cb(ble_br_test_util_gap_event_cb, NULL);
}

void
ble_br_test_util_init(void)
{
    ble_br_test_util_init_buf(BLE_BR_TEST_UTIL_ACL_LEN, BLE_BR_TEST_UTIL_ACL_PKTS, 0);
}

#endif
