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
#include "ble_hs_priv.h"
#include "ble_br_priv.h"

/* Address field */
#define BLE_BR_RFCOMM_ADDR_EA         0x01
#define BLE_BR_RFCOMM_ADDR_CR         0x02
#define BLE_BR_RFCOMM_ADDR_DLCI_SHIFT 2

/* Control field, without the poll/final bit */
#define BLE_BR_RFCOMM_CTRL_SABM 0x2f
#define BLE_BR_RFCOMM_CTRL_UA   0x63
#define BLE_BR_RFCOMM_CTRL_DM   0x0f
#define BLE_BR_RFCOMM_CTRL_DISC 0x43
#define BLE_BR_RFCOMM_CTRL_UIH  0xef
#define BLE_BR_RFCOMM_CTRL_PF   0x10

#define BLE_BR_RFCOMM_LEN_EA        0x01
#define BLE_BR_RFCOMM_LEN_SHORT_MAX 127

/* Address, control, two bytes of length, credits and FCS */
#define BLE_BR_RFCOMM_FRAME_OVERHEAD 6
/* What a frame takes of the L2CAP MTU around its payload */
#define BLE_BR_RFCOMM_MTU_OVERHEAD 5

/* Multiplexer commands, on DLCI 0 */
#define BLE_BR_RFCOMM_MCC_EA         0x01
#define BLE_BR_RFCOMM_MCC_CR         0x02
#define BLE_BR_RFCOMM_MCC_TYPE_SHIFT 2
#define BLE_BR_RFCOMM_MCC_NSC        0x04
#define BLE_BR_RFCOMM_MCC_TEST       0x08
#define BLE_BR_RFCOMM_MCC_RLS        0x14
#define BLE_BR_RFCOMM_MCC_FCOFF      0x18
#define BLE_BR_RFCOMM_MCC_PN         0x20
#define BLE_BR_RFCOMM_MCC_RPN        0x24
#define BLE_BR_RFCOMM_MCC_FCON       0x28
#define BLE_BR_RFCOMM_MCC_MSC        0x38

#define BLE_BR_RFCOMM_MCC_MAX_LEN 32

struct ble_br_rfcomm_pn {
    uint8_t dlci;
    uint8_t flow;
    uint8_t priority;
    uint8_t ack_timer;
    uint16_t mtu;
    uint8_t max_retrans;
    uint8_t credits;
} __attribute__((packed));

#define BLE_BR_RFCOMM_PN_FLOW_CREDIT_REQ 0xf0
#define BLE_BR_RFCOMM_PN_FLOW_CREDIT_RSP 0xe0
#define BLE_BR_RFCOMM_PN_CREDITS_MASK    0x07

/* Modem status: ready to communicate, ready to receive, data valid */
#define BLE_BR_RFCOMM_MSC_SIGNALS 0x8d

/* Port settings given when a peer asks: 9600 baud, 8N1, no flow control */
#define BLE_BR_RFCOMM_RPN_LEN       8
#define BLE_BR_RFCOMM_RPN_BAUD_9600 0x03
#define BLE_BR_RFCOMM_RPN_8N1       0x03
#define BLE_BR_RFCOMM_RPN_XON       0x11
#define BLE_BR_RFCOMM_RPN_XOFF      0x13

/* How long the session and a link take to open at most (T1) */
#define BLE_BR_RFCOMM_OPEN_TIMEOUT 20000

#define BLE_BR_RFCOMM_MTU_DEFAULT    127
#define BLE_BR_RFCOMM_RX_CREDITS     7
#define BLE_BR_RFCOMM_RX_CREDITS_LOW 2
#define BLE_BR_RFCOMM_MAX_CHANNEL    30

#define BLE_BR_RFCOMM_SESSION_CONNECTING 1
#define BLE_BR_RFCOMM_SESSION_OPENING    2
#define BLE_BR_RFCOMM_SESSION_OPEN       3

struct ble_br_rfcomm_session {
    bool in_use;
    uint8_t state;
    bool initiator;
    ble_npl_time_t exp;
    uint16_t conn_handle;
    struct ble_br_l2cap_chan *chan;
};

#define BLE_BR_RFCOMM_DLC_WAIT_SESSION 1
#define BLE_BR_RFCOMM_DLC_PN_SENT      2
#define BLE_BR_RFCOMM_DLC_SABM_SENT    3
#define BLE_BR_RFCOMM_DLC_ACCEPTING    4
#define BLE_BR_RFCOMM_DLC_OPEN         5

struct ble_br_rfcomm_dlc {
    struct ble_br_rfcomm_session *session;
    uint8_t dlci;
    uint8_t state;
    bool credit_flow;
    uint8_t rx_credits;
    uint16_t tx_credits;
    uint16_t mtu;
    ble_npl_time_t exp;
    const struct ble_br_rfcomm_dlc_ops *ops;
    void *arg;

    /* A frame waiting for a credit */
    uint8_t tx_buf[MYNEWT_VAL(BLE_BREDR_RFCOMM_MTU)];
    uint16_t tx_len;
};

static struct ble_br_rfcomm_session ble_br_rfcomm_sessions[MYNEWT_VAL(BLE_BREDR_MAX_CONNECTIONS)];
static struct ble_br_rfcomm_dlc ble_br_rfcomm_dlcs[MYNEWT_VAL(BLE_BREDR_RFCOMM_MAX_DLCS)];

static SLIST_HEAD(, ble_br_rfcomm_server)
ble_br_rfcomm_servers;

static const struct ble_br_l2cap_chan_ops ble_br_rfcomm_l2cap_ops;

static uint8_t
ble_br_rfcomm_fcs(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0xff;
    int i;

    while (len--) {
        crc ^= *data++;
        for (i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xe0 : 0);
        }
    }

    return 0xff - crc;
}

/*
 * credits is negative when the frame carries none. A command of the
 * initiator and a response of the responder have the C/R bit set.
 */
static int
ble_br_rfcomm_frame_tx(struct ble_br_rfcomm_session *session, uint8_t dlci,
                       uint8_t ctrl, bool command, int credits,
                       const uint8_t *data, uint16_t len)
{
    uint8_t hdr[5];
    struct os_mbuf *om;
    uint8_t hdr_len;
    uint8_t fcs;

    if (session->chan == NULL) {
        return BLE_HS_ENOTCONN;
    }

    hdr[0] = BLE_BR_RFCOMM_ADDR_EA | (dlci << BLE_BR_RFCOMM_ADDR_DLCI_SHIFT);
    if (command == session->initiator) {
        hdr[0] |= BLE_BR_RFCOMM_ADDR_CR;
    }
    hdr[1] = ctrl;
    if (credits >= 0) {
        hdr[1] |= BLE_BR_RFCOMM_CTRL_PF;
    }
    if (len <= BLE_BR_RFCOMM_LEN_SHORT_MAX) {
        hdr[2] = (len << 1) | BLE_BR_RFCOMM_LEN_EA;
        hdr_len = 3;
    } else {
        hdr[2] = len << 1;
        hdr[3] = len >> 7;
        hdr_len = 4;
    }

    fcs = ble_br_rfcomm_fcs(
        hdr, (ctrl & ~BLE_BR_RFCOMM_CTRL_PF) == BLE_BR_RFCOMM_CTRL_UIH ? 2 : hdr_len);

    if (credits >= 0) {
        hdr[hdr_len++] = credits;
    }

    om = ble_br_l2cap_alloc();
    if (om == NULL) {
        return BLE_HS_ENOMEM;
    }

    if (os_mbuf_append(om, hdr, hdr_len) != 0 || os_mbuf_append(om, data, len) != 0 ||
        os_mbuf_append(om, &fcs, sizeof(fcs)) != 0) {
        os_mbuf_free_chain(om);
        return BLE_HS_ENOMEM;
    }

    return ble_br_l2cap_send(session->chan, om);
}

static int
ble_br_rfcomm_ctrl_tx(struct ble_br_rfcomm_session *session, uint8_t dlci,
                      uint8_t ctrl, bool command)
{
    return ble_br_rfcomm_frame_tx(session, dlci, ctrl | BLE_BR_RFCOMM_CTRL_PF,
                                  command, -1, NULL, 0);
}

static int
ble_br_rfcomm_mcc_tx(struct ble_br_rfcomm_session *session, uint8_t type,
                     bool command, const uint8_t *data, uint8_t len)
{
    uint8_t buf[2 + BLE_BR_RFCOMM_MCC_MAX_LEN];

    if (len > BLE_BR_RFCOMM_MCC_MAX_LEN) {
        return BLE_HS_EINVAL;
    }

    buf[0] = BLE_BR_RFCOMM_MCC_EA | (type << BLE_BR_RFCOMM_MCC_TYPE_SHIFT);
    if (command) {
        buf[0] |= BLE_BR_RFCOMM_MCC_CR;
    }
    buf[1] = BLE_BR_RFCOMM_LEN_EA | (len << 1);
    memcpy(&buf[2], data, len);

    return ble_br_rfcomm_frame_tx(session, 0, BLE_BR_RFCOMM_CTRL_UIH, true, -1,
                                  buf, 2 + len);
}

static struct ble_br_rfcomm_session *
ble_br_rfcomm_session_find(uint16_t conn_handle)
{
    struct ble_br_rfcomm_session *session;
    int i;

    for (i = 0; i < MYNEWT_VAL(BLE_BREDR_MAX_CONNECTIONS); i++) {
        session = &ble_br_rfcomm_sessions[i];
        if (session->in_use && session->conn_handle == conn_handle) {
            return session;
        }
    }

    return NULL;
}

static struct ble_br_rfcomm_session *
ble_br_rfcomm_session_alloc(uint16_t conn_handle)
{
    struct ble_br_rfcomm_session *session;
    int i;

    for (i = 0; i < MYNEWT_VAL(BLE_BREDR_MAX_CONNECTIONS); i++) {
        session = &ble_br_rfcomm_sessions[i];
        if (!session->in_use) {
            memset(session, 0, sizeof(*session));
            session->in_use = true;
            session->conn_handle = conn_handle;
            session->exp = ble_br_hs_deadline(BLE_BR_RFCOMM_OPEN_TIMEOUT);
            return session;
        }
    }

    return NULL;
}

static struct ble_br_rfcomm_dlc *
ble_br_rfcomm_dlc_find(struct ble_br_rfcomm_session *session, uint8_t dlci)
{
    struct ble_br_rfcomm_dlc *dlc;
    int i;

    for (i = 0; i < MYNEWT_VAL(BLE_BREDR_RFCOMM_MAX_DLCS); i++) {
        dlc = &ble_br_rfcomm_dlcs[i];
        if (dlc->session == session && dlc->dlci == dlci) {
            return dlc;
        }
    }

    return NULL;
}

static struct ble_br_rfcomm_dlc *
ble_br_rfcomm_dlc_alloc(struct ble_br_rfcomm_session *session, uint8_t dlci)
{
    struct ble_br_rfcomm_dlc *dlc;
    int i;

    for (i = 0; i < MYNEWT_VAL(BLE_BREDR_RFCOMM_MAX_DLCS); i++) {
        dlc = &ble_br_rfcomm_dlcs[i];
        if (dlc->session == NULL) {
            memset(dlc, 0, sizeof(*dlc));
            dlc->session = session;
            dlc->dlci = dlci;
            dlc->mtu = BLE_BR_RFCOMM_MTU_DEFAULT;
            dlc->exp = ble_br_hs_deadline(BLE_BR_RFCOMM_OPEN_TIMEOUT);
            return dlc;
        }
    }

    return NULL;
}

/* Frees the link and tells its owner, whichever state it was in */
static void
ble_br_rfcomm_dlc_close(struct ble_br_rfcomm_dlc *dlc, int status)
{
    const struct ble_br_rfcomm_dlc_ops *ops = dlc->ops;
    void *arg = dlc->arg;
    uint8_t state = dlc->state;

    dlc->session = NULL;

    if (state == BLE_BR_RFCOMM_DLC_OPEN) {
        ops->disconnected(dlc, arg);
    } else {
        ops->connected(dlc, status, arg);
    }
}

static void
ble_br_rfcomm_session_close(struct ble_br_rfcomm_session *session, int status)
{
    struct ble_br_rfcomm_dlc *dlc;
    int i;

    if (session->chan != NULL) {
        ble_br_l2cap_disconnect(session->chan);
        session->chan = NULL;
    }
    session->in_use = false;

    for (i = 0; i < MYNEWT_VAL(BLE_BREDR_RFCOMM_MAX_DLCS); i++) {
        dlc = &ble_br_rfcomm_dlcs[i];
        if (dlc->session == session) {
            ble_br_rfcomm_dlc_close(dlc, status);
        }
    }
}

/* A session without links is of no use */
static void
ble_br_rfcomm_session_check_idle(struct ble_br_rfcomm_session *session)
{
    int i;

    for (i = 0; i < MYNEWT_VAL(BLE_BREDR_RFCOMM_MAX_DLCS); i++) {
        if (ble_br_rfcomm_dlcs[i].session == session) {
            return;
        }
    }

    ble_br_rfcomm_session_close(session, BLE_HS_EDONE);
}

static uint16_t
ble_br_rfcomm_session_mtu(const struct ble_br_rfcomm_session *session)
{
    uint16_t mtu;

    mtu = BLE_BR_MIN(ble_br_l2cap_chan_peer_mtu(session->chan),
                     MYNEWT_VAL(BLE_BREDR_L2CAP_MTU)) -
          BLE_BR_RFCOMM_MTU_OVERHEAD;

    return BLE_BR_MIN(mtu, MYNEWT_VAL(BLE_BREDR_RFCOMM_MTU));
}

static int
ble_br_rfcomm_pn_tx(struct ble_br_rfcomm_dlc *dlc)
{
    struct ble_br_rfcomm_pn pn;

    memset(&pn, 0, sizeof(pn));
    pn.dlci = dlc->dlci;
    pn.flow = BLE_BR_RFCOMM_PN_FLOW_CREDIT_REQ;
    pn.mtu = htole16(ble_br_rfcomm_session_mtu(dlc->session));
    pn.credits = BLE_BR_RFCOMM_RX_CREDITS;

    dlc->state = BLE_BR_RFCOMM_DLC_PN_SENT;
    dlc->rx_credits = BLE_BR_RFCOMM_RX_CREDITS;

    return ble_br_rfcomm_mcc_tx(dlc->session, BLE_BR_RFCOMM_MCC_PN, true,
                                (const uint8_t *)&pn, sizeof(pn));
}

static void
ble_br_rfcomm_dlc_open(struct ble_br_rfcomm_dlc *dlc)
{
    uint8_t msc[2];

    dlc->state = BLE_BR_RFCOMM_DLC_OPEN;

    msc[0] = BLE_BR_RFCOMM_ADDR_EA | BLE_BR_RFCOMM_ADDR_CR |
             (dlc->dlci << BLE_BR_RFCOMM_ADDR_DLCI_SHIFT);
    msc[1] = BLE_BR_RFCOMM_MSC_SIGNALS;
    ble_br_rfcomm_mcc_tx(dlc->session, BLE_BR_RFCOMM_MCC_MSC, true, msc, sizeof(msc));

    dlc->ops->connected(dlc, 0, dlc->arg);
}

static void
ble_br_rfcomm_dlc_flush(struct ble_br_rfcomm_dlc *dlc)
{
    int rc;

    if (dlc->tx_len == 0 || (dlc->credit_flow && dlc->tx_credits == 0)) {
        return;
    }

    rc = ble_br_rfcomm_frame_tx(dlc->session, dlc->dlci, BLE_BR_RFCOMM_CTRL_UIH,
                                true, -1, dlc->tx_buf, dlc->tx_len);
    if (rc == 0 && dlc->credit_flow) {
        dlc->tx_credits--;
    }
    dlc->tx_len = 0;
}

/* Starts a link the peer asks for, if a server takes it */
static struct ble_br_rfcomm_dlc *
ble_br_rfcomm_dlc_accept(struct ble_br_rfcomm_session *session, uint8_t dlci)
{
    struct ble_br_rfcomm_server *server;
    struct ble_br_rfcomm_dlc *dlc;

    /* The direction bit tells the servers of the two sides apart */
    if ((dlci & 1) != (session->initiator ? 1 : 0)) {
        return NULL;
    }

    SLIST_FOREACH(server, &ble_br_rfcomm_servers, next) {
        if (server->channel == dlci >> 1) {
            break;
        }
    }
    if (server == NULL) {
        return NULL;
    }

    dlc = ble_br_rfcomm_dlc_alloc(session, dlci);
    if (dlc == NULL) {
        return NULL;
    }

    if (server->accept(session->conn_handle, &dlc->ops, &dlc->arg) != 0) {
        dlc->session = NULL;
        return NULL;
    }

    dlc->state = BLE_BR_RFCOMM_DLC_ACCEPTING;

    return dlc;
}

static void
ble_br_rfcomm_rx_pn(struct ble_br_rfcomm_session *session, bool command,
                    const uint8_t *data, uint8_t len)
{
    const struct ble_br_rfcomm_pn *pn = (const void *)data;
    struct ble_br_rfcomm_pn rsp;
    struct ble_br_rfcomm_dlc *dlc;
    uint16_t mtu;

    if (len != sizeof(*pn)) {
        return;
    }

    dlc = ble_br_rfcomm_dlc_find(session, pn->dlci);
    mtu = le16toh(pn->mtu);

    if (!command) {
        if (dlc == NULL || dlc->state != BLE_BR_RFCOMM_DLC_PN_SENT) {
            return;
        }

        if (mtu == 0 || mtu > ble_br_rfcomm_session_mtu(session)) {
            ble_br_rfcomm_dlc_close(dlc, BLE_HS_EREJECT);
            ble_br_rfcomm_session_check_idle(session);
            return;
        }

        dlc->mtu = mtu;
        dlc->credit_flow = pn->flow == BLE_BR_RFCOMM_PN_FLOW_CREDIT_RSP;
        dlc->tx_credits = pn->credits & BLE_BR_RFCOMM_PN_CREDITS_MASK;
        dlc->state = BLE_BR_RFCOMM_DLC_SABM_SENT;
        ble_br_rfcomm_ctrl_tx(session, dlc->dlci, BLE_BR_RFCOMM_CTRL_SABM, true);
        return;
    }

    if (dlc == NULL) {
        dlc = ble_br_rfcomm_dlc_accept(session, pn->dlci);
    }
    if (dlc == NULL || dlc->state != BLE_BR_RFCOMM_DLC_ACCEPTING || mtu == 0) {
        ble_br_rfcomm_ctrl_tx(session, pn->dlci, BLE_BR_RFCOMM_CTRL_DM, false);
        return;
    }

    dlc->mtu = BLE_BR_MIN(mtu, ble_br_rfcomm_session_mtu(session));
    dlc->credit_flow = pn->flow == BLE_BR_RFCOMM_PN_FLOW_CREDIT_REQ;
    dlc->tx_credits = pn->credits & BLE_BR_RFCOMM_PN_CREDITS_MASK;
    dlc->rx_credits = BLE_BR_RFCOMM_RX_CREDITS;

    rsp = *pn;
    rsp.flow = dlc->credit_flow ? BLE_BR_RFCOMM_PN_FLOW_CREDIT_RSP : 0;
    rsp.mtu = htole16(dlc->mtu);
    rsp.credits = dlc->credit_flow ? BLE_BR_RFCOMM_RX_CREDITS : 0;

    ble_br_rfcomm_mcc_tx(session, BLE_BR_RFCOMM_MCC_PN, false,
                         (const uint8_t *)&rsp, sizeof(rsp));
}

static void
ble_br_rfcomm_rx_mcc(struct ble_br_rfcomm_session *session,
                     const uint8_t *data, uint16_t len)
{
    uint8_t rpn[BLE_BR_RFCOMM_RPN_LEN];
    bool command;
    uint8_t type;
    uint8_t mcc_len;

    if (len < 2 || !(data[1] & BLE_BR_RFCOMM_LEN_EA)) {
        return;
    }

    type = data[0] >> BLE_BR_RFCOMM_MCC_TYPE_SHIFT;
    command = (data[0] & BLE_BR_RFCOMM_MCC_CR) != 0;
    mcc_len = data[1] >> 1;
    if (mcc_len != len - 2) {
        return;
    }
    data += 2;

    switch (type) {
    case BLE_BR_RFCOMM_MCC_PN:
        ble_br_rfcomm_rx_pn(session, command, data, mcc_len);
        break;

    case BLE_BR_RFCOMM_MCC_RPN:
        if (!command) {
            break;
        }
        if (mcc_len == BLE_BR_RFCOMM_RPN_LEN) {
            ble_br_rfcomm_mcc_tx(session, type, false, data, mcc_len);
        } else if (mcc_len == 1) {
            memset(rpn, 0, sizeof(rpn));
            rpn[0] = data[0];
            rpn[1] = BLE_BR_RFCOMM_RPN_BAUD_9600;
            rpn[2] = BLE_BR_RFCOMM_RPN_8N1;
            rpn[4] = BLE_BR_RFCOMM_RPN_XON;
            rpn[5] = BLE_BR_RFCOMM_RPN_XOFF;
            ble_br_rfcomm_mcc_tx(session, type, false, rpn, sizeof(rpn));
        }
        break;

    case BLE_BR_RFCOMM_MCC_MSC:
    case BLE_BR_RFCOMM_MCC_RLS:
    case BLE_BR_RFCOMM_MCC_TEST:
    case BLE_BR_RFCOMM_MCC_FCON:
    case BLE_BR_RFCOMM_MCC_FCOFF:
        if (command) {
            ble_br_rfcomm_mcc_tx(session, type, false, data, mcc_len);
        }
        break;

    case BLE_BR_RFCOMM_MCC_NSC:
        break;

    default:
        if (command) {
            type = BLE_BR_RFCOMM_MCC_EA | BLE_BR_RFCOMM_MCC_CR |
                   (type << BLE_BR_RFCOMM_MCC_TYPE_SHIFT);
            ble_br_rfcomm_mcc_tx(session, BLE_BR_RFCOMM_MCC_NSC, false, &type,
                                 sizeof(type));
        }
        break;
    }
}

static void
ble_br_rfcomm_rx_sabm(struct ble_br_rfcomm_session *session, uint8_t dlci)
{
    struct ble_br_rfcomm_dlc *dlc;

    if (dlci == 0) {
        ble_br_rfcomm_ctrl_tx(session, 0, BLE_BR_RFCOMM_CTRL_UA, false);
        session->state = BLE_BR_RFCOMM_SESSION_OPEN;
        return;
    }

    dlc = ble_br_rfcomm_dlc_find(session, dlci);
    if (dlc == NULL) {
        /* The peer may skip the parameter negotiation */
        dlc = ble_br_rfcomm_dlc_accept(session, dlci);
    }
    if (dlc == NULL || dlc->state != BLE_BR_RFCOMM_DLC_ACCEPTING) {
        ble_br_rfcomm_ctrl_tx(session, dlci, BLE_BR_RFCOMM_CTRL_DM, false);
        return;
    }

    ble_br_rfcomm_ctrl_tx(session, dlci, BLE_BR_RFCOMM_CTRL_UA, false);
    ble_br_rfcomm_dlc_open(dlc);
}

static void
ble_br_rfcomm_rx_ua(struct ble_br_rfcomm_session *session, uint8_t dlci)
{
    struct ble_br_rfcomm_dlc *dlc;
    int i;

    if (dlci == 0) {
        if (session->state != BLE_BR_RFCOMM_SESSION_OPENING) {
            return;
        }
        session->state = BLE_BR_RFCOMM_SESSION_OPEN;

        for (i = 0; i < MYNEWT_VAL(BLE_BREDR_RFCOMM_MAX_DLCS); i++) {
            dlc = &ble_br_rfcomm_dlcs[i];
            if (dlc->session == session && dlc->state == BLE_BR_RFCOMM_DLC_WAIT_SESSION) {
                ble_br_rfcomm_pn_tx(dlc);
            }
        }
        return;
    }

    dlc = ble_br_rfcomm_dlc_find(session, dlci);
    if (dlc != NULL && dlc->state == BLE_BR_RFCOMM_DLC_SABM_SENT) {
        ble_br_rfcomm_dlc_open(dlc);
    }
}

static void
ble_br_rfcomm_rx_data(struct ble_br_rfcomm_session *session, uint8_t dlci,
                      bool pf, const uint8_t *data, uint16_t len)
{
    struct ble_br_rfcomm_dlc *dlc;

    dlc = ble_br_rfcomm_dlc_find(session, dlci);
    if (dlc == NULL || dlc->state != BLE_BR_RFCOMM_DLC_OPEN) {
        ble_br_rfcomm_ctrl_tx(session, dlci, BLE_BR_RFCOMM_CTRL_DM, false);
        return;
    }

    if (dlc->credit_flow && pf) {
        if (len < 1) {
            return;
        }
        dlc->tx_credits += data[0];
        data++;
        len--;
        ble_br_rfcomm_dlc_flush(dlc);
    }

    if (len == 0) {
        return;
    }

    if (dlc->credit_flow) {
        if (dlc->rx_credits > 0) {
            dlc->rx_credits--;
        }
        if (dlc->rx_credits <= BLE_BR_RFCOMM_RX_CREDITS_LOW) {
            if (ble_br_rfcomm_frame_tx(session, dlci, BLE_BR_RFCOMM_CTRL_UIH, true,
                                       BLE_BR_RFCOMM_RX_CREDITS - dlc->rx_credits,
                                       NULL, 0) == 0) {
                dlc->rx_credits = BLE_BR_RFCOMM_RX_CREDITS;
            }
        }
    }

    dlc->ops->recv(dlc, data, len, dlc->arg);
}

static void
ble_br_rfcomm_l2cap_recv(struct ble_br_l2cap_chan *chan, struct os_mbuf *om, void *arg)
{
    uint8_t buf[MYNEWT_VAL(BLE_BREDR_RFCOMM_MTU) + BLE_BR_RFCOMM_FRAME_OVERHEAD];
    struct ble_br_rfcomm_session *session = arg;
    struct ble_br_rfcomm_dlc *dlc;
    uint16_t frame_len;
    uint16_t len;
    uint8_t hdr_len;
    uint8_t dlci;
    uint8_t ctrl;
    bool uih;
    bool pf;

    frame_len = OS_MBUF_PKTLEN(om);
    if (frame_len < 4 || frame_len > sizeof(buf)) {
        return;
    }
    os_mbuf_copydata(om, 0, frame_len, buf);

    dlci = buf[0] >> BLE_BR_RFCOMM_ADDR_DLCI_SHIFT;
    ctrl = buf[1] & ~BLE_BR_RFCOMM_CTRL_PF;
    pf = (buf[1] & BLE_BR_RFCOMM_CTRL_PF) != 0;
    uih = ctrl == BLE_BR_RFCOMM_CTRL_UIH;

    len = buf[2] >> 1;
    hdr_len = 3;
    if (!(buf[2] & BLE_BR_RFCOMM_LEN_EA)) {
        len |= (uint16_t)buf[3] << 7;
        hdr_len = 4;
    }

    /* A data frame with the final bit set carries a byte of credits */
    if (uih && pf && dlci != 0) {
        len++;
    }

    if (frame_len != hdr_len + len + 1 ||
        buf[frame_len - 1] != ble_br_rfcomm_fcs(buf, uih ? 2 : hdr_len)) {
        return;
    }

    switch (ctrl) {
    case BLE_BR_RFCOMM_CTRL_SABM:
        ble_br_rfcomm_rx_sabm(session, dlci);
        break;

    case BLE_BR_RFCOMM_CTRL_UA:
        ble_br_rfcomm_rx_ua(session, dlci);
        break;

    case BLE_BR_RFCOMM_CTRL_DM:
        if (dlci == 0) {
            ble_br_rfcomm_session_close(session, BLE_HS_EREJECT);
            break;
        }
        dlc = ble_br_rfcomm_dlc_find(session, dlci);
        if (dlc != NULL) {
            ble_br_rfcomm_dlc_close(dlc, BLE_HS_EREJECT);
            ble_br_rfcomm_session_check_idle(session);
        }
        break;

    case BLE_BR_RFCOMM_CTRL_DISC:
        ble_br_rfcomm_ctrl_tx(session, dlci, BLE_BR_RFCOMM_CTRL_UA, false);
        if (dlci == 0) {
            ble_br_rfcomm_session_close(session, BLE_HS_EDONE);
            break;
        }
        dlc = ble_br_rfcomm_dlc_find(session, dlci);
        if (dlc != NULL) {
            ble_br_rfcomm_dlc_close(dlc, BLE_HS_EDONE);
        }
        break;

    case BLE_BR_RFCOMM_CTRL_UIH:
        if (dlci == 0) {
            ble_br_rfcomm_rx_mcc(session, &buf[hdr_len], len);
        } else {
            ble_br_rfcomm_rx_data(session, dlci, pf, &buf[hdr_len], len);
        }
        break;

    default:
        break;
    }
}

static void
ble_br_rfcomm_l2cap_connected(struct ble_br_l2cap_chan *chan, int status, void *arg)
{
    struct ble_br_rfcomm_session *session = arg;

    if (status != 0) {
        session->chan = NULL;
        ble_br_rfcomm_session_close(session, status);
        return;
    }

    session->chan = chan;

    if (session->initiator) {
        session->state = BLE_BR_RFCOMM_SESSION_OPENING;
        ble_br_rfcomm_ctrl_tx(session, 0, BLE_BR_RFCOMM_CTRL_SABM, true);
    }
}

static void
ble_br_rfcomm_l2cap_disconnected(struct ble_br_l2cap_chan *chan, void *arg)
{
    struct ble_br_rfcomm_session *session = arg;

    session->chan = NULL;
    ble_br_rfcomm_session_close(session, BLE_HS_ENOTCONN);
}

static const struct ble_br_l2cap_chan_ops ble_br_rfcomm_l2cap_ops = {
    .connected = ble_br_rfcomm_l2cap_connected,
    .disconnected = ble_br_rfcomm_l2cap_disconnected,
    .recv = ble_br_rfcomm_l2cap_recv,
};

static int
ble_br_rfcomm_l2cap_accept(uint16_t conn_handle,
                           const struct ble_br_l2cap_chan_ops **ops, void **arg)
{
    struct ble_br_rfcomm_session *session;

    if (ble_br_rfcomm_session_find(conn_handle) != NULL) {
        return BLE_HS_EALREADY;
    }

    session = ble_br_rfcomm_session_alloc(conn_handle);
    if (session == NULL) {
        return BLE_HS_ENOMEM;
    }

    session->state = BLE_BR_RFCOMM_SESSION_CONNECTING;
    *ops = &ble_br_rfcomm_l2cap_ops;
    *arg = session;

    return 0;
}

static struct ble_br_l2cap_server ble_br_rfcomm_l2cap_server = {
    .psm = BLE_BR_L2CAP_PSM_RFCOMM,
    .flags = BLE_BR_L2CAP_SERVER_F_ENCRYPT,
    .accept = ble_br_rfcomm_l2cap_accept,
};

int
ble_br_rfcomm_server_register(struct ble_br_rfcomm_server *server)
{
    if (server->channel == 0 || server->channel > BLE_BR_RFCOMM_MAX_CHANNEL) {
        return BLE_HS_EINVAL;
    }

    SLIST_INSERT_HEAD(&ble_br_rfcomm_servers, server, next);

    return 0;
}

int
ble_br_rfcomm_connect(uint16_t conn_handle, uint8_t channel,
                      const struct ble_br_rfcomm_dlc_ops *ops, void *arg,
                      struct ble_br_rfcomm_dlc **out_dlc)
{
    struct ble_br_rfcomm_session *session;
    struct ble_br_rfcomm_dlc *dlc;
    bool created;
    int rc;

    if (channel == 0 || channel > BLE_BR_RFCOMM_MAX_CHANNEL) {
        return BLE_HS_EINVAL;
    }

    created = false;
    session = ble_br_rfcomm_session_find(conn_handle);
    if (session == NULL) {
        session = ble_br_rfcomm_session_alloc(conn_handle);
        if (session == NULL) {
            return BLE_HS_ENOMEM;
        }
        session->initiator = true;
        session->state = BLE_BR_RFCOMM_SESSION_CONNECTING;
        created = true;
    }

    /* The servers of the peer are on the other side of the direction bit */
    dlc = ble_br_rfcomm_dlc_alloc(session, (channel << 1) |
                                               (session->initiator ? 0 : 1));
    if (dlc == NULL) {
        rc = BLE_HS_ENOMEM;
        goto err;
    }
    dlc->ops = ops;
    dlc->arg = arg;
    dlc->state = BLE_BR_RFCOMM_DLC_WAIT_SESSION;

    if (created) {
        rc = ble_br_l2cap_connect(conn_handle, BLE_BR_L2CAP_PSM_RFCOMM,
                                  &ble_br_rfcomm_l2cap_ops, session, NULL);
        if (rc != 0) {
            dlc->session = NULL;
            goto err;
        }
    } else if (session->state == BLE_BR_RFCOMM_SESSION_OPEN) {
        rc = ble_br_rfcomm_pn_tx(dlc);
        if (rc != 0) {
            dlc->session = NULL;
            return rc;
        }
    }

    if (out_dlc != NULL) {
        *out_dlc = dlc;
    }

    return 0;

err:
    if (created) {
        session->in_use = false;
    }
    return rc;
}

/* The owner is not told: it asked */
int
ble_br_rfcomm_disconnect(struct ble_br_rfcomm_dlc *dlc)
{
    struct ble_br_rfcomm_session *session = dlc->session;

    if (session == NULL) {
        return BLE_HS_ENOTCONN;
    }

    if (dlc->state == BLE_BR_RFCOMM_DLC_OPEN ||
        dlc->state == BLE_BR_RFCOMM_DLC_SABM_SENT) {
        ble_br_rfcomm_ctrl_tx(session, dlc->dlci, BLE_BR_RFCOMM_CTRL_DISC, true);
    }

    dlc->session = NULL;
    ble_br_rfcomm_session_check_idle(session);

    return 0;
}

int
ble_br_rfcomm_send(struct ble_br_rfcomm_dlc *dlc, const uint8_t *data, uint16_t len)
{
    if (dlc->session == NULL || dlc->state != BLE_BR_RFCOMM_DLC_OPEN) {
        return BLE_HS_ENOTCONN;
    }

    if (len == 0 || len > dlc->mtu || len > sizeof(dlc->tx_buf)) {
        return BLE_HS_EMSGSIZE;
    }

    if (dlc->tx_len != 0) {
        return BLE_HS_EBUSY;
    }

    memcpy(dlc->tx_buf, data, len);
    dlc->tx_len = len;
    ble_br_rfcomm_dlc_flush(dlc);

    return 0;
}

uint16_t
ble_br_rfcomm_dlc_conn_handle(const struct ble_br_rfcomm_dlc *dlc)
{
    return dlc->session != NULL ? dlc->session->conn_handle : BLE_HS_CONN_HANDLE_NONE;
}

/* Gives up on the sessions and the links that did not open in time */
void
ble_br_rfcomm_timer(ble_npl_time_t now, int32_t *ticks)
{
    struct ble_br_rfcomm_session *session;
    struct ble_br_rfcomm_dlc *dlc;
    int i;

    for (i = 0; i < MYNEWT_VAL(BLE_BREDR_MAX_CONNECTIONS); i++) {
        session = &ble_br_rfcomm_sessions[i];
        if (session->in_use && session->state != BLE_BR_RFCOMM_SESSION_OPEN &&
            ble_br_hs_expired(session->exp, now, ticks)) {
            ble_br_rfcomm_session_close(session, BLE_HS_ETIMEOUT);
        }
    }

    for (i = 0; i < MYNEWT_VAL(BLE_BREDR_RFCOMM_MAX_DLCS); i++) {
        dlc = &ble_br_rfcomm_dlcs[i];
        session = dlc->session;
        if (session != NULL && dlc->state != BLE_BR_RFCOMM_DLC_OPEN &&
            ble_br_hs_expired(dlc->exp, now, ticks)) {
            ble_br_rfcomm_dlc_close(dlc, BLE_HS_ETIMEOUT);
            ble_br_rfcomm_session_check_idle(session);
        }
    }
}

void
ble_br_rfcomm_init(void)
{
    memset(ble_br_rfcomm_sessions, 0, sizeof(ble_br_rfcomm_sessions));
    memset(ble_br_rfcomm_dlcs, 0, sizeof(ble_br_rfcomm_dlcs));
    SLIST_INIT(&ble_br_rfcomm_servers);

    ble_br_l2cap_server_register(&ble_br_rfcomm_l2cap_server);
}

#endif
