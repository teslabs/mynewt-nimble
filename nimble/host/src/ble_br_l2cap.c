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

/* Basic mode only: no retransmission, no streaming, no FCS */

#define BLE_BR_L2CAP_CID_SIG     0x0001
#define BLE_BR_L2CAP_CID_DYN_MIN 0x0040

#define BLE_BR_L2CAP_MTU_MIN     48
#define BLE_BR_L2CAP_MTU_DEFAULT 672
#define BLE_BR_L2CAP_SIG_MTU     128

/* How long a request waits for its response, and for a pending one */
#define BLE_BR_L2CAP_RTX_TIMEOUT  30000
#define BLE_BR_L2CAP_ERTX_TIMEOUT 60000

#define BLE_BR_L2CAP_SIG_CMD_REJECT  0x01
#define BLE_BR_L2CAP_SIG_CONN_REQ    0x02
#define BLE_BR_L2CAP_SIG_CONN_RSP    0x03
#define BLE_BR_L2CAP_SIG_CONFIG_REQ  0x04
#define BLE_BR_L2CAP_SIG_CONFIG_RSP  0x05
#define BLE_BR_L2CAP_SIG_DISCONN_REQ 0x06
#define BLE_BR_L2CAP_SIG_DISCONN_RSP 0x07
#define BLE_BR_L2CAP_SIG_ECHO_REQ    0x08
#define BLE_BR_L2CAP_SIG_ECHO_RSP    0x09
#define BLE_BR_L2CAP_SIG_INFO_REQ    0x0a
#define BLE_BR_L2CAP_SIG_INFO_RSP    0x0b

#define BLE_BR_L2CAP_REJECT_NOT_UNDERSTOOD 0x0000

#define BLE_BR_L2CAP_CONN_SUCCESS        0x0000
#define BLE_BR_L2CAP_CONN_PENDING        0x0001
#define BLE_BR_L2CAP_CONN_PSM_NOT_SUPP   0x0002
#define BLE_BR_L2CAP_CONN_SECURITY_BLOCK 0x0003
#define BLE_BR_L2CAP_CONN_NO_RESOURCES   0x0004

#define BLE_BR_L2CAP_CONN_STATUS_AUTHEN 0x0001

#define BLE_BR_L2CAP_CONFIG_SUCCESS      0x0000
#define BLE_BR_L2CAP_CONFIG_UNACCEPTABLE 0x0001
#define BLE_BR_L2CAP_CONFIG_REJECTED     0x0002
#define BLE_BR_L2CAP_CONFIG_UNKNOWN_OPTS 0x0003

#define BLE_BR_L2CAP_CONFIG_F_CONTINUATION 0x0001

#define BLE_BR_L2CAP_OPT_MTU           0x01
#define BLE_BR_L2CAP_OPT_FLUSH_TIMEOUT 0x02
#define BLE_BR_L2CAP_OPT_QOS           0x03
#define BLE_BR_L2CAP_OPT_RFC           0x04
#define BLE_BR_L2CAP_OPT_FCS           0x05
#define BLE_BR_L2CAP_OPT_HINT          0x80
#define BLE_BR_L2CAP_OPT_RFC_LEN       9
#define BLE_BR_L2CAP_RFC_MODE_BASIC    0x00

#define BLE_BR_L2CAP_INFO_EXT_FEATURES 0x0002
#define BLE_BR_L2CAP_INFO_FIXED_CHANS  0x0003
#define BLE_BR_L2CAP_INFO_SUCCESS      0x0000
#define BLE_BR_L2CAP_INFO_NOT_SUPP     0x0001
#define BLE_BR_L2CAP_FEAT_FIXED_CHANS  0x00000080
#define BLE_BR_L2CAP_FIXED_CHAN_SIG    0x02

struct ble_br_l2cap_sig_hdr {
    uint8_t code;
    uint8_t ident;
    uint16_t len;
} __attribute__((packed));

struct ble_br_l2cap_conn_req {
    uint16_t psm;
    uint16_t scid;
} __attribute__((packed));

struct ble_br_l2cap_conn_rsp {
    uint16_t dcid;
    uint16_t scid;
    uint16_t result;
    uint16_t status;
} __attribute__((packed));

struct ble_br_l2cap_config_req {
    uint16_t dcid;
    uint16_t flags;
} __attribute__((packed));

struct ble_br_l2cap_config_rsp {
    uint16_t scid;
    uint16_t flags;
    uint16_t result;
} __attribute__((packed));

struct ble_br_l2cap_disconn {
    uint16_t dcid;
    uint16_t scid;
} __attribute__((packed));

#define BLE_BR_L2CAP_CHAN_WAIT_ENC   4
#define BLE_BR_L2CAP_CHAN_CONNECTING 1
#define BLE_BR_L2CAP_CHAN_CONFIG     2
#define BLE_BR_L2CAP_CHAN_OPEN       3

#define BLE_BR_L2CAP_CHAN_F_LOCAL_CFG 0x01
#define BLE_BR_L2CAP_CHAN_F_PEER_CFG  0x02

struct ble_br_l2cap_chan {
    struct ble_br_conn *conn;
    uint16_t psm;
    uint16_t scid;
    uint16_t dcid;
    uint16_t peer_mtu;
    uint8_t state;
    uint8_t flags;
    uint8_t ident;
    /* By when the channel has to be open */
    ble_npl_time_t exp;
    const struct ble_br_l2cap_chan_ops *ops;
    void *arg;
};

static struct ble_br_l2cap_chan ble_br_l2cap_chans[MYNEWT_VAL(BLE_BREDR_L2CAP_MAX_CHANS)];

static SLIST_HEAD(, ble_br_l2cap_server)
ble_br_l2cap_servers;

struct os_mbuf *
ble_br_l2cap_alloc(void)
{
    return ble_hs_mbuf_l2cap_pkt();
}

static int
ble_br_l2cap_tx(struct ble_br_conn *conn, uint16_t cid, struct os_mbuf *om)
{
    om = ble_l2cap_prepend_hdr(om, cid, OS_MBUF_PKTLEN(om));
    if (om == NULL) {
        return BLE_HS_ENOMEM;
    }

    return ble_br_hs_acl_tx(conn, om);
}

static int
ble_br_l2cap_sig_tx(struct ble_br_conn *conn, uint8_t code, uint8_t ident,
                    const void *data, uint16_t len)
{
    struct ble_br_l2cap_sig_hdr hdr;
    struct os_mbuf *om;

    om = ble_br_l2cap_alloc();
    if (om == NULL) {
        return BLE_HS_ENOMEM;
    }

    hdr.code = code;
    hdr.ident = ident;
    hdr.len = htole16(len);

    if (os_mbuf_append(om, &hdr, sizeof(hdr)) != 0 ||
        os_mbuf_append(om, data, len) != 0) {
        os_mbuf_free_chain(om);
        return BLE_HS_ENOMEM;
    }

    return ble_br_l2cap_tx(conn, BLE_BR_L2CAP_CID_SIG, om);
}

static uint8_t
ble_br_l2cap_next_ident(struct ble_br_conn *conn)
{
    if (++conn->sig_ident == 0) {
        conn->sig_ident = 1;
    }

    return conn->sig_ident;
}

static struct ble_br_l2cap_chan *
ble_br_l2cap_chan_alloc(struct ble_br_conn *conn, uint16_t psm)
{
    struct ble_br_l2cap_chan *chan;
    int i;

    for (i = 0; i < MYNEWT_VAL(BLE_BREDR_L2CAP_MAX_CHANS); i++) {
        chan = &ble_br_l2cap_chans[i];
        if (chan->conn == NULL) {
            memset(chan, 0, sizeof(*chan));
            chan->conn = conn;
            chan->psm = psm;
            chan->scid = BLE_BR_L2CAP_CID_DYN_MIN + i;
            chan->peer_mtu = BLE_BR_L2CAP_MTU_DEFAULT;
            chan->exp = ble_br_hs_deadline(BLE_BR_L2CAP_RTX_TIMEOUT);
            return chan;
        }
    }

    return NULL;
}

static struct ble_br_l2cap_chan *
ble_br_l2cap_chan_find(struct ble_br_conn *conn, uint16_t scid)
{
    struct ble_br_l2cap_chan *chan;
    int idx;

    idx = scid - BLE_BR_L2CAP_CID_DYN_MIN;
    if (idx < 0 || idx >= MYNEWT_VAL(BLE_BREDR_L2CAP_MAX_CHANS)) {
        return NULL;
    }

    chan = &ble_br_l2cap_chans[idx];

    return chan->conn == conn ? chan : NULL;
}

/* Frees the channel and tells its owner, whichever state it was in */
static void
ble_br_l2cap_chan_close(struct ble_br_l2cap_chan *chan, int status)
{
    const struct ble_br_l2cap_chan_ops *ops = chan->ops;
    void *arg = chan->arg;
    uint8_t state = chan->state;

    chan->conn = NULL;

    if (ops == NULL) {
        return;
    }

    if (state == BLE_BR_L2CAP_CHAN_OPEN) {
        ops->disconnected(chan, arg);
    } else {
        ops->connected(chan, status, arg);
    }
}

static int
ble_br_l2cap_config_tx(struct ble_br_l2cap_chan *chan)
{
    struct {
        struct ble_br_l2cap_config_req req;
        uint8_t opt_type;
        uint8_t opt_len;
        uint16_t mtu;
    } __attribute__((packed)) cmd;

    cmd.req.dcid = htole16(chan->dcid);
    cmd.req.flags = 0;
    cmd.opt_type = BLE_BR_L2CAP_OPT_MTU;
    cmd.opt_len = sizeof(cmd.mtu);
    cmd.mtu = htole16(MYNEWT_VAL(BLE_BREDR_L2CAP_MTU));

    chan->state = BLE_BR_L2CAP_CHAN_CONFIG;
    chan->exp = ble_br_hs_deadline(BLE_BR_L2CAP_RTX_TIMEOUT);
    chan->ident = ble_br_l2cap_next_ident(chan->conn);

    return ble_br_l2cap_sig_tx(chan->conn, BLE_BR_L2CAP_SIG_CONFIG_REQ,
                               chan->ident, &cmd, sizeof(cmd));
}

static void
ble_br_l2cap_chan_check_open(struct ble_br_l2cap_chan *chan)
{
    const uint8_t done = BLE_BR_L2CAP_CHAN_F_LOCAL_CFG | BLE_BR_L2CAP_CHAN_F_PEER_CFG;

    if (chan->state == BLE_BR_L2CAP_CHAN_CONFIG && (chan->flags & done) == done) {
        chan->state = BLE_BR_L2CAP_CHAN_OPEN;
        chan->ops->connected(chan, 0, chan->arg);
    }
}

static void
ble_br_l2cap_conn_rsp_tx(struct ble_br_conn *conn, uint8_t ident, uint16_t dcid,
                         uint16_t scid, uint16_t result, uint16_t status)
{
    struct ble_br_l2cap_conn_rsp rsp;

    rsp.dcid = htole16(dcid);
    rsp.scid = htole16(scid);
    rsp.result = htole16(result);
    rsp.status = htole16(status);

    ble_br_l2cap_sig_tx(conn, BLE_BR_L2CAP_SIG_CONN_RSP, ident, &rsp, sizeof(rsp));
}

static void
ble_br_l2cap_rx_conn_req(struct ble_br_conn *conn, uint8_t ident,
                         const uint8_t *data, uint16_t len)
{
    const struct ble_br_l2cap_conn_req *req = (const void *)data;
    struct ble_br_l2cap_server *server;
    struct ble_br_l2cap_chan *chan;
    uint16_t result;
    uint16_t scid;
    uint16_t psm;

    if (len != sizeof(*req)) {
        return;
    }

    psm = le16toh(req->psm);
    scid = le16toh(req->scid);
    chan = NULL;

    SLIST_FOREACH(server, &ble_br_l2cap_servers, next) {
        if (server->psm == psm) {
            break;
        }
    }

    if (server == NULL) {
        result = BLE_BR_L2CAP_CONN_PSM_NOT_SUPP;
    } else {
        result = BLE_BR_L2CAP_CONN_NO_RESOURCES;
        chan = ble_br_l2cap_chan_alloc(conn, psm);
        if (chan != NULL) {
            if (server->accept(conn->handle, &chan->ops, &chan->arg) == 0) {
                result = BLE_BR_L2CAP_CONN_SUCCESS;
            } else {
                chan->conn = NULL;
                chan = NULL;
            }
        }
    }

    if (chan == NULL) {
        ble_br_l2cap_conn_rsp_tx(conn, ident, 0, scid, result, 0);
        return;
    }

    chan->dcid = scid;

    /* The request may get here ahead of the event that tells the link is
     * encrypted: the answer waits for it.
     */
    if ((server->flags & BLE_BR_L2CAP_SERVER_F_ENCRYPT) &&
        !(conn->flags & BLE_BR_CONN_F_ENCRYPTED)) {
        chan->state = BLE_BR_L2CAP_CHAN_WAIT_ENC;
        chan->exp = ble_br_hs_deadline(BLE_BR_L2CAP_ERTX_TIMEOUT);
        chan->ident = ident;
        ble_br_l2cap_conn_rsp_tx(conn, ident, chan->scid, scid,
                                 BLE_BR_L2CAP_CONN_PENDING,
                                 BLE_BR_L2CAP_CONN_STATUS_AUTHEN);
        return;
    }

    ble_br_l2cap_conn_rsp_tx(conn, ident, chan->scid, scid, result, 0);
    ble_br_l2cap_config_tx(chan);
}

void
ble_br_l2cap_enc_change(struct ble_br_conn *conn)
{
    struct ble_br_l2cap_chan *chan;
    int i;

    for (i = 0; i < MYNEWT_VAL(BLE_BREDR_L2CAP_MAX_CHANS); i++) {
        chan = &ble_br_l2cap_chans[i];
        if (chan->conn != conn || chan->state != BLE_BR_L2CAP_CHAN_WAIT_ENC) {
            continue;
        }

        if (conn->flags & BLE_BR_CONN_F_ENCRYPTED) {
            ble_br_l2cap_conn_rsp_tx(conn, chan->ident, chan->scid, chan->dcid,
                                     BLE_BR_L2CAP_CONN_SUCCESS, 0);
            ble_br_l2cap_config_tx(chan);
        } else {
            ble_br_l2cap_conn_rsp_tx(conn, chan->ident, 0, chan->dcid,
                                     BLE_BR_L2CAP_CONN_SECURITY_BLOCK, 0);
            ble_br_l2cap_chan_close(chan, BLE_HS_EAUTHEN);
        }
    }
}

static void
ble_br_l2cap_rx_conn_rsp(struct ble_br_conn *conn, uint8_t ident,
                         const uint8_t *data, uint16_t len)
{
    const struct ble_br_l2cap_conn_rsp *rsp = (const void *)data;
    struct ble_br_l2cap_chan *chan;

    if (len != sizeof(*rsp)) {
        return;
    }

    chan = ble_br_l2cap_chan_find(conn, le16toh(rsp->scid));
    if (chan == NULL || chan->state != BLE_BR_L2CAP_CHAN_CONNECTING ||
        chan->ident != ident) {
        return;
    }

    switch (le16toh(rsp->result)) {
    case BLE_BR_L2CAP_CONN_SUCCESS:
        chan->dcid = le16toh(rsp->dcid);
        ble_br_l2cap_config_tx(chan);
        break;

    case BLE_BR_L2CAP_CONN_PENDING:
        chan->exp = ble_br_hs_deadline(BLE_BR_L2CAP_ERTX_TIMEOUT);
        break;

    case BLE_BR_L2CAP_CONN_SECURITY_BLOCK:
        ble_br_l2cap_chan_close(chan, BLE_HS_EAUTHEN);
        break;

    default:
        ble_br_l2cap_chan_close(chan, BLE_HS_EREJECT);
        break;
    }
}

static void
ble_br_l2cap_rx_config_req(struct ble_br_conn *conn, uint8_t ident,
                           const uint8_t *data, uint16_t len)
{
    const struct ble_br_l2cap_config_req *req = (const void *)data;
    struct {
        struct ble_br_l2cap_config_rsp rsp;
        uint8_t opts[2 + BLE_BR_L2CAP_OPT_RFC_LEN];
    } __attribute__((packed)) cmd;
    struct ble_br_l2cap_chan *chan;
    uint16_t result;
    uint16_t flags;
    uint8_t opts_len;
    uint8_t opt_type;
    uint8_t opt_len;
    uint16_t off;

    if (len < sizeof(*req)) {
        return;
    }

    chan = ble_br_l2cap_chan_find(conn, le16toh(req->dcid));
    if (chan == NULL || chan->state == BLE_BR_L2CAP_CHAN_CONNECTING ||
        chan->state == BLE_BR_L2CAP_CHAN_WAIT_ENC) {
        return;
    }

    flags = le16toh(req->flags);
    result = BLE_BR_L2CAP_CONFIG_SUCCESS;
    opts_len = 0;

    for (off = sizeof(*req); off < len; off += 2 + opt_len) {
        if (off + 2 > len || off + 2 + data[off + 1] > len) {
            result = BLE_BR_L2CAP_CONFIG_REJECTED;
            break;
        }

        opt_type = data[off];
        opt_len = data[off + 1];

        switch (opt_type & ~BLE_BR_L2CAP_OPT_HINT) {
        case BLE_BR_L2CAP_OPT_MTU:
            if (opt_len != 2) {
                result = BLE_BR_L2CAP_CONFIG_REJECTED;
            } else if (get_le16(&data[off + 2]) < BLE_BR_L2CAP_MTU_MIN) {
                result = BLE_BR_L2CAP_CONFIG_UNACCEPTABLE;
                cmd.opts[0] = BLE_BR_L2CAP_OPT_MTU;
                cmd.opts[1] = 2;
                put_le16(&cmd.opts[2], BLE_BR_L2CAP_MTU_MIN);
                opts_len = 4;
            } else {
                chan->peer_mtu = get_le16(&data[off + 2]);
            }
            break;

        case BLE_BR_L2CAP_OPT_RFC:
            if (opt_len != BLE_BR_L2CAP_OPT_RFC_LEN) {
                result = BLE_BR_L2CAP_CONFIG_REJECTED;
            } else if (data[off + 2] != BLE_BR_L2CAP_RFC_MODE_BASIC) {
                result = BLE_BR_L2CAP_CONFIG_UNACCEPTABLE;
                memset(cmd.opts, 0, sizeof(cmd.opts));
                cmd.opts[0] = BLE_BR_L2CAP_OPT_RFC;
                cmd.opts[1] = BLE_BR_L2CAP_OPT_RFC_LEN;
                cmd.opts[2] = BLE_BR_L2CAP_RFC_MODE_BASIC;
                opts_len = sizeof(cmd.opts);
            }
            break;

        case BLE_BR_L2CAP_OPT_FLUSH_TIMEOUT:
        case BLE_BR_L2CAP_OPT_QOS:
        case BLE_BR_L2CAP_OPT_FCS:
            break;

        default:
            if (!(opt_type & BLE_BR_L2CAP_OPT_HINT)) {
                result = BLE_BR_L2CAP_CONFIG_UNKNOWN_OPTS;
                cmd.opts[0] = opt_type;
                cmd.opts[1] = 0;
                opts_len = 2;
            }
            break;
        }

        if (result != BLE_BR_L2CAP_CONFIG_SUCCESS) {
            break;
        }
    }

    cmd.rsp.scid = htole16(chan->dcid);
    cmd.rsp.flags = htole16(flags & BLE_BR_L2CAP_CONFIG_F_CONTINUATION);
    cmd.rsp.result = htole16(result);

    ble_br_l2cap_sig_tx(conn, BLE_BR_L2CAP_SIG_CONFIG_RSP, ident, &cmd,
                        sizeof(cmd.rsp) + opts_len);

    if (result == BLE_BR_L2CAP_CONFIG_SUCCESS &&
        !(flags & BLE_BR_L2CAP_CONFIG_F_CONTINUATION)) {
        chan->flags |= BLE_BR_L2CAP_CHAN_F_PEER_CFG;
        ble_br_l2cap_chan_check_open(chan);
    }
}

static void
ble_br_l2cap_rx_config_rsp(struct ble_br_conn *conn, uint8_t ident,
                           const uint8_t *data, uint16_t len)
{
    const struct ble_br_l2cap_config_rsp *rsp = (const void *)data;
    struct ble_br_l2cap_chan *chan;

    if (len < sizeof(*rsp)) {
        return;
    }

    chan = ble_br_l2cap_chan_find(conn, le16toh(rsp->scid));
    if (chan == NULL || chan->state != BLE_BR_L2CAP_CHAN_CONFIG ||
        chan->ident != ident) {
        return;
    }

    if (le16toh(rsp->result) != BLE_BR_L2CAP_CONFIG_SUCCESS) {
        ble_br_l2cap_disconnect(chan);
        ble_br_l2cap_chan_close(chan, BLE_HS_EREJECT);
        return;
    }

    if (!(le16toh(rsp->flags) & BLE_BR_L2CAP_CONFIG_F_CONTINUATION)) {
        chan->flags |= BLE_BR_L2CAP_CHAN_F_LOCAL_CFG;
        ble_br_l2cap_chan_check_open(chan);
    }
}

static void
ble_br_l2cap_rx_disconn_req(struct ble_br_conn *conn, uint8_t ident,
                            const uint8_t *data, uint16_t len)
{
    const struct ble_br_l2cap_disconn *req = (const void *)data;
    struct ble_br_l2cap_chan *chan;

    if (len != sizeof(*req)) {
        return;
    }

    chan = ble_br_l2cap_chan_find(conn, le16toh(req->dcid));
    if (chan == NULL || chan->dcid != le16toh(req->scid)) {
        return;
    }

    ble_br_l2cap_sig_tx(conn, BLE_BR_L2CAP_SIG_DISCONN_RSP, ident, req, sizeof(*req));
    ble_br_l2cap_chan_close(chan, BLE_HS_EDONE);
}

static void
ble_br_l2cap_rx_info_req(struct ble_br_conn *conn, uint8_t ident,
                         const uint8_t *data, uint16_t len)
{
    struct {
        uint16_t type;
        uint16_t result;
        uint8_t data[8];
    } __attribute__((packed)) rsp;
    uint16_t rsp_len;

    if (len != sizeof(rsp.type)) {
        return;
    }

    memset(&rsp, 0, sizeof(rsp));
    memcpy(&rsp.type, data, sizeof(rsp.type));
    rsp.result = htole16(BLE_BR_L2CAP_INFO_SUCCESS);
    rsp_len = sizeof(rsp.type) + sizeof(rsp.result);

    switch (le16toh(rsp.type)) {
    case BLE_BR_L2CAP_INFO_EXT_FEATURES:
        put_le32(rsp.data, BLE_BR_L2CAP_FEAT_FIXED_CHANS);
        rsp_len += 4;
        break;

    case BLE_BR_L2CAP_INFO_FIXED_CHANS:
        rsp.data[0] = BLE_BR_L2CAP_FIXED_CHAN_SIG;
        rsp_len += 8;
        break;

    default:
        rsp.result = htole16(BLE_BR_L2CAP_INFO_NOT_SUPP);
        break;
    }

    ble_br_l2cap_sig_tx(conn, BLE_BR_L2CAP_SIG_INFO_RSP, ident, &rsp, rsp_len);
}

static void
ble_br_l2cap_rx_sig(struct ble_br_conn *conn, struct os_mbuf *om)
{
    const struct ble_br_l2cap_sig_hdr *hdr;
    uint8_t buf[BLE_BR_L2CAP_SIG_MTU];
    uint16_t reason;
    uint16_t total;
    uint16_t len;
    uint16_t off;

    total = OS_MBUF_PKTLEN(om);
    if (total > sizeof(buf)) {
        return;
    }
    os_mbuf_copydata(om, 0, total, buf);

    for (off = 0; off + sizeof(*hdr) <= total; off += sizeof(*hdr) + len) {
        hdr = (const void *)&buf[off];
        len = le16toh(hdr->len);
        if (hdr->ident == 0 || off + sizeof(*hdr) + len > total) {
            return;
        }

        switch (hdr->code) {
        case BLE_BR_L2CAP_SIG_CONN_REQ:
            ble_br_l2cap_rx_conn_req(conn, hdr->ident, (const void *)(hdr + 1), len);
            break;

        case BLE_BR_L2CAP_SIG_CONN_RSP:
            ble_br_l2cap_rx_conn_rsp(conn, hdr->ident, (const void *)(hdr + 1), len);
            break;

        case BLE_BR_L2CAP_SIG_CONFIG_REQ:
            ble_br_l2cap_rx_config_req(conn, hdr->ident, (const void *)(hdr + 1), len);
            break;

        case BLE_BR_L2CAP_SIG_CONFIG_RSP:
            ble_br_l2cap_rx_config_rsp(conn, hdr->ident, (const void *)(hdr + 1), len);
            break;

        case BLE_BR_L2CAP_SIG_DISCONN_REQ:
            ble_br_l2cap_rx_disconn_req(conn, hdr->ident,
                                        (const void *)(hdr + 1), len);
            break;

        case BLE_BR_L2CAP_SIG_ECHO_REQ:
            ble_br_l2cap_sig_tx(conn, BLE_BR_L2CAP_SIG_ECHO_RSP, hdr->ident,
                                hdr + 1, len);
            break;

        case BLE_BR_L2CAP_SIG_INFO_REQ:
            ble_br_l2cap_rx_info_req(conn, hdr->ident, (const void *)(hdr + 1), len);
            break;

        case BLE_BR_L2CAP_SIG_CMD_REJECT:
        case BLE_BR_L2CAP_SIG_DISCONN_RSP:
        case BLE_BR_L2CAP_SIG_ECHO_RSP:
        case BLE_BR_L2CAP_SIG_INFO_RSP:
            break;

        default:
            reason = htole16(BLE_BR_L2CAP_REJECT_NOT_UNDERSTOOD);
            ble_br_l2cap_sig_tx(conn, BLE_BR_L2CAP_SIG_CMD_REJECT, hdr->ident,
                                &reason, sizeof(reason));
            break;
        }

        /* A handler may have ended the connection */
        if (!(conn->flags & BLE_BR_CONN_F_CONNECTED)) {
            return;
        }
    }
}

void
ble_br_l2cap_rx(struct ble_br_conn *conn, uint8_t pb, struct os_mbuf *om)
{
    struct ble_br_l2cap_chan *chan;
    struct ble_l2cap_hdr hdr;
    uint16_t cid;

    if (pb == BLE_HCI_PB_MIDDLE) {
        if (conn->rx_om == NULL) {
            os_mbuf_free_chain(om);
            return;
        }
        os_mbuf_concat(conn->rx_om, om);
    } else {
        os_mbuf_free_chain(conn->rx_om);
        conn->rx_om = om;

        if (os_mbuf_copydata(om, 0, sizeof(hdr), &hdr) != 0) {
            goto drop;
        }
        conn->rx_len = sizeof(hdr) + le16toh(hdr.len);
        if (conn->rx_len > sizeof(hdr) + MYNEWT_VAL(BLE_BREDR_L2CAP_MTU) &&
            conn->rx_len > sizeof(hdr) + BLE_BR_L2CAP_SIG_MTU) {
            goto drop;
        }
    }

    if (OS_MBUF_PKTLEN(conn->rx_om) < conn->rx_len) {
        return;
    }
    if (OS_MBUF_PKTLEN(conn->rx_om) > conn->rx_len) {
        goto drop;
    }

    om = conn->rx_om;
    conn->rx_om = NULL;

    os_mbuf_copydata(om, 0, sizeof(hdr), &hdr);
    os_mbuf_adj(om, sizeof(hdr));
    cid = le16toh(hdr.cid);

    if (cid == BLE_BR_L2CAP_CID_SIG) {
        ble_br_l2cap_rx_sig(conn, om);
    } else {
        chan = ble_br_l2cap_chan_find(conn, cid);
        if (chan != NULL && chan->state == BLE_BR_L2CAP_CHAN_OPEN) {
            chan->ops->recv(chan, om, chan->arg);
        }
    }

    os_mbuf_free_chain(om);
    return;

drop:
    os_mbuf_free_chain(conn->rx_om);
    conn->rx_om = NULL;
}

/* Gives up on the channels that did not open in time */
void
ble_br_l2cap_timer(ble_npl_time_t now, int32_t *ticks)
{
    struct ble_br_l2cap_chan *chan;
    int i;

    for (i = 0; i < MYNEWT_VAL(BLE_BREDR_L2CAP_MAX_CHANS); i++) {
        chan = &ble_br_l2cap_chans[i];
        if (chan->conn == NULL || chan->state == BLE_BR_L2CAP_CHAN_OPEN ||
            !ble_br_hs_expired(chan->exp, now, ticks)) {
            continue;
        }

        /* The peer still waits for the final answer to its request */
        if (chan->state == BLE_BR_L2CAP_CHAN_WAIT_ENC) {
            ble_br_l2cap_conn_rsp_tx(chan->conn, chan->ident, 0, chan->dcid,
                                     BLE_BR_L2CAP_CONN_SECURITY_BLOCK, 0);
        }

        ble_br_l2cap_disconnect(chan);
        ble_br_l2cap_chan_close(chan, BLE_HS_ETIMEOUT);
    }
}

void
ble_br_l2cap_conn_broken(struct ble_br_conn *conn)
{
    struct ble_br_l2cap_chan *chan;
    int i;

    for (i = 0; i < MYNEWT_VAL(BLE_BREDR_L2CAP_MAX_CHANS); i++) {
        chan = &ble_br_l2cap_chans[i];
        if (chan->conn == conn) {
            ble_br_l2cap_chan_close(chan, BLE_HS_ENOTCONN);
        }
    }
}

int
ble_br_l2cap_server_register(struct ble_br_l2cap_server *server)
{
    SLIST_INSERT_HEAD(&ble_br_l2cap_servers, server, next);

    return 0;
}

int
ble_br_l2cap_connect(uint16_t conn_handle, uint16_t psm,
                     const struct ble_br_l2cap_chan_ops *ops, void *arg,
                     struct ble_br_l2cap_chan **out_chan)
{
    struct ble_br_l2cap_conn_req req;
    struct ble_br_l2cap_chan *chan;
    struct ble_br_conn *conn;
    int rc;

    conn = ble_br_conn_find(conn_handle);
    if (conn == NULL) {
        return BLE_HS_ENOTCONN;
    }

    chan = ble_br_l2cap_chan_alloc(conn, psm);
    if (chan == NULL) {
        return BLE_HS_ENOMEM;
    }

    chan->ops = ops;
    chan->arg = arg;
    chan->state = BLE_BR_L2CAP_CHAN_CONNECTING;
    chan->ident = ble_br_l2cap_next_ident(conn);

    req.psm = htole16(psm);
    req.scid = htole16(chan->scid);

    rc = ble_br_l2cap_sig_tx(conn, BLE_BR_L2CAP_SIG_CONN_REQ, chan->ident,
                             &req, sizeof(req));
    if (rc != 0) {
        chan->conn = NULL;
        return rc;
    }

    if (out_chan != NULL) {
        *out_chan = chan;
    }

    return 0;
}

/* The owner is not told: it asked */
int
ble_br_l2cap_disconnect(struct ble_br_l2cap_chan *chan)
{
    struct ble_br_l2cap_disconn req;
    struct ble_br_conn *conn = chan->conn;

    if (conn == NULL) {
        return BLE_HS_ENOTCONN;
    }

    chan->conn = NULL;

    if (chan->state == BLE_BR_L2CAP_CHAN_CONNECTING ||
        chan->state == BLE_BR_L2CAP_CHAN_WAIT_ENC) {
        return 0;
    }

    req.dcid = htole16(chan->dcid);
    req.scid = htole16(chan->scid);

    return ble_br_l2cap_sig_tx(conn, BLE_BR_L2CAP_SIG_DISCONN_REQ,
                               ble_br_l2cap_next_ident(conn), &req, sizeof(req));
}

int
ble_br_l2cap_send(struct ble_br_l2cap_chan *chan, struct os_mbuf *om)
{
    if (chan->conn == NULL || chan->state != BLE_BR_L2CAP_CHAN_OPEN ||
        OS_MBUF_PKTLEN(om) > chan->peer_mtu) {
        os_mbuf_free_chain(om);
        return BLE_HS_EINVAL;
    }

    return ble_br_l2cap_tx(chan->conn, chan->dcid, om);
}

uint16_t
ble_br_l2cap_chan_conn_handle(const struct ble_br_l2cap_chan *chan)
{
    return chan->conn != NULL ? chan->conn->handle : BLE_HS_CONN_HANDLE_NONE;
}

uint16_t
ble_br_l2cap_chan_peer_mtu(const struct ble_br_l2cap_chan *chan)
{
    return chan->peer_mtu;
}

void
ble_br_l2cap_init(void)
{
    memset(ble_br_l2cap_chans, 0, sizeof(ble_br_l2cap_chans));
    SLIST_INIT(&ble_br_l2cap_servers);
}

#endif
