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
#include "ble_br_sdp_priv.h"

#define BLE_BR_SDP_PDU_ERROR_RSP       0x01
#define BLE_BR_SDP_PDU_SEARCH_REQ      0x02
#define BLE_BR_SDP_PDU_SEARCH_RSP      0x03
#define BLE_BR_SDP_PDU_ATTR_REQ        0x04
#define BLE_BR_SDP_PDU_ATTR_RSP        0x05
#define BLE_BR_SDP_PDU_SEARCH_ATTR_REQ 0x06
#define BLE_BR_SDP_PDU_SEARCH_ATTR_RSP 0x07

#define BLE_BR_SDP_ERR_INVALID_HANDLE 0x0002
#define BLE_BR_SDP_ERR_INVALID_SYNTAX 0x0003
#define BLE_BR_SDP_ERR_INVALID_CONT   0x0005
#define BLE_BR_SDP_ERR_NO_RESOURCES   0x0006

#define BLE_BR_SDP_HDR_SZ 5

/* Record handles the server gives out start here */
#define BLE_BR_SDP_HANDLE_BASE 0x00010000

/* The server continues a response from an offset, which it hands out */
#define BLE_BR_SDP_CONT_LEN     2
#define BLE_BR_SDP_CONT_MAX_LEN 16

#define BLE_BR_SDP_SERVER_BUF_SZ     192
#define BLE_BR_SDP_CLIENT_BUF_SZ     160
#define BLE_BR_SDP_CLIENT_TIMEOUT    30000
#define BLE_BR_SDP_CLIENT_MAX_ROUNDS 8

struct ble_br_sdp_elem {
    uint8_t type;
    const uint8_t *data;
    uint16_t len;
    /* With its header */
    uint16_t total;
};

/* Header of a sequence, whose length is filled in once it is known */
static const uint8_t ble_br_sdp_empty_seq[] = { BLE_BR_SDP_DE_SEQ8, 0 };

static const struct ble_br_sdp_record *ble_br_sdp_records[MYNEWT_VAL(BLE_BREDR_SDP_MAX_RECORDS)];
static uint8_t ble_br_sdp_num_records;

static struct {
    struct ble_br_l2cap_chan *chan;
    uint16_t conn_handle;
    uint16_t uuid16;
    ble_br_sdp_rfcomm_fn *cb;
    void *cb_arg;
    uint16_t tid;
    uint8_t rounds;
    ble_npl_time_t exp;
    uint8_t buf[BLE_BR_SDP_CLIENT_BUF_SZ];
    uint16_t buf_len;
} ble_br_sdp_client;

static int
ble_br_sdp_elem_parse(const uint8_t *data, uint16_t len, struct ble_br_sdp_elem *elem)
{
    uint32_t size;
    uint8_t size_len;
    uint8_t hdr;
    int i;

    if (len < 1) {
        return BLE_HS_EBADDATA;
    }

    elem->type = data[0] & BLE_BR_SDP_DE_TYPE_MASK;

    hdr = 1;
    if ((data[0] & BLE_BR_SDP_DE_SIZE_MASK) < BLE_BR_SDP_DE_SIZE_NEXT1) {
        size = elem->type == BLE_BR_SDP_DE_NIL
                   ? 0
                   : 1U << (data[0] & BLE_BR_SDP_DE_SIZE_MASK);
    } else {
        size_len =
            1U << ((data[0] & BLE_BR_SDP_DE_SIZE_MASK) - BLE_BR_SDP_DE_SIZE_NEXT1);
        if (len < 1 + size_len) {
            return BLE_HS_EBADDATA;
        }
        size = 0;
        for (i = 0; i < size_len; i++) {
            size = (size << 8) | data[1 + i];
        }
        hdr += size_len;
    }

    if (size > (uint32_t)(len - hdr)) {
        return BLE_HS_EBADDATA;
    }

    elem->data = data + hdr;
    elem->len = size;
    elem->total = hdr + size;

    return 0;
}

/* Takes the next element out of a sequence */
static int
ble_br_sdp_elem_next(struct ble_br_sdp_elem *seq, struct ble_br_sdp_elem *elem)
{
    int rc;

    rc = ble_br_sdp_elem_parse(seq->data, seq->len, elem);
    if (rc != 0) {
        return rc;
    }

    seq->data += elem->total;
    seq->len -= elem->total;

    return 0;
}

/* The 16-bit form of a UUID, 0 if it has none */
static uint16_t
ble_br_sdp_elem_uuid16(const struct ble_br_sdp_elem *elem)
{
    static const uint8_t base[] = {
        0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb,
    };

    if (elem->type != BLE_BR_SDP_DE_UUID) {
        return 0;
    }

    switch (elem->len) {
    case 2:
        return get_be16(elem->data);

    case 4:
        return get_be16(elem->data) == 0 ? get_be16(elem->data + 2) : 0;

    case 16:
        if (get_be16(elem->data) != 0 ||
            memcmp(elem->data + 4, base, sizeof(base)) != 0) {
            return 0;
        }
        return get_be16(elem->data + 2);

    default:
        return 0;
    }
}

static bool
ble_br_sdp_record_matches(const struct ble_br_sdp_record *record,
                          const struct ble_br_sdp_elem *pattern)
{
    struct ble_br_sdp_elem seq = *pattern;
    struct ble_br_sdp_elem elem;
    uint16_t uuid16;
    bool found;
    int i;

    if (seq.len == 0) {
        return false;
    }

    while (seq.len > 0) {
        if (ble_br_sdp_elem_next(&seq, &elem) != 0) {
            return false;
        }

        uuid16 = ble_br_sdp_elem_uuid16(&elem);
        found = false;
        for (i = 0; uuid16 != 0 && i < record->num_uuids; i++) {
            found = found || record->uuids[i] == uuid16;
        }
        if (!found) {
            return false;
        }
    }

    return true;
}

static bool
ble_br_sdp_attr_requested(const struct ble_br_sdp_elem *id_list, uint16_t id)
{
    struct ble_br_sdp_elem seq = *id_list;
    struct ble_br_sdp_elem elem;

    while (seq.len > 0) {
        if (ble_br_sdp_elem_next(&seq, &elem) != 0 || elem.type != BLE_BR_SDP_DE_UINT) {
            return false;
        }

        if (elem.len == 2 && get_be16(elem.data) == id) {
            return true;
        }
        if (elem.len == 4 && get_be16(elem.data) <= id &&
            id <= get_be16(elem.data + 2)) {
            return true;
        }
    }

    return false;
}

static int
ble_br_sdp_put(uint8_t *buf, uint16_t buf_sz, uint16_t *off,
               const uint8_t *data, uint16_t len)
{
    if (*off + len > buf_sz) {
        return BLE_HS_ENOMEM;
    }

    memcpy(buf + *off, data, len);
    *off += len;

    return 0;
}

/* Appends the requested attributes of a record, as a sequence */
static int
ble_br_sdp_put_record(uint8_t *buf, uint16_t buf_sz, uint16_t *off,
                      uint8_t idx, const struct ble_br_sdp_elem *id_list)
{
    const struct ble_br_sdp_record *record = ble_br_sdp_records[idx];
    const struct ble_br_sdp_attr *attr;
    uint8_t handle[1 + 4];
    uint8_t id[1 + 2];
    uint16_t start;
    int rc;
    int i;

    start = *off;
    rc = ble_br_sdp_put(buf, buf_sz, off, ble_br_sdp_empty_seq,
                        sizeof(ble_br_sdp_empty_seq));
    if (rc != 0) {
        return rc;
    }

    id[0] = BLE_BR_SDP_DE_UINT16;

    if (ble_br_sdp_attr_requested(id_list, BLE_BR_SDP_ATTR_RECORD_HANDLE)) {
        put_be16(&id[1], BLE_BR_SDP_ATTR_RECORD_HANDLE);
        handle[0] = BLE_BR_SDP_DE_UINT32;
        put_be32(&handle[1], BLE_BR_SDP_HANDLE_BASE + idx);
        rc = ble_br_sdp_put(buf, buf_sz, off, id, sizeof(id));
        if (rc == 0) {
            rc = ble_br_sdp_put(buf, buf_sz, off, handle, sizeof(handle));
        }
        if (rc != 0) {
            return rc;
        }
    }

    for (i = 0; i < record->num_attrs; i++) {
        attr = &record->attrs[i];
        if (!ble_br_sdp_attr_requested(id_list, attr->id)) {
            continue;
        }

        put_be16(&id[1], attr->id);
        rc = ble_br_sdp_put(buf, buf_sz, off, id, sizeof(id));
        if (rc == 0) {
            rc = ble_br_sdp_put(buf, buf_sz, off, attr->value, attr->value_len);
        }
        if (rc != 0) {
            return rc;
        }
    }

    buf[start + 1] = *off - start - 2;

    return 0;
}

static void
ble_br_sdp_rsp_tx(struct ble_br_l2cap_chan *chan, uint8_t pdu, uint16_t tid,
                  const uint8_t *params, uint16_t len)
{
    uint8_t hdr[BLE_BR_SDP_HDR_SZ];
    struct os_mbuf *om;

    om = ble_br_l2cap_alloc();
    if (om == NULL) {
        return;
    }

    hdr[0] = pdu;
    put_be16(&hdr[1], tid);
    put_be16(&hdr[3], len);

    if (os_mbuf_append(om, hdr, sizeof(hdr)) != 0 ||
        os_mbuf_append(om, params, len) != 0) {
        os_mbuf_free_chain(om);
        return;
    }

    ble_br_l2cap_send(chan, om);
}

static void
ble_br_sdp_error_tx(struct ble_br_l2cap_chan *chan, uint16_t tid, uint16_t code)
{
    uint8_t params[2];

    put_be16(params, code);
    ble_br_sdp_rsp_tx(chan, BLE_BR_SDP_PDU_ERROR_RSP, tid, params, sizeof(params));
}

static void
ble_br_sdp_server_search(struct ble_br_l2cap_chan *chan, uint16_t tid,
                         const uint8_t *data, uint16_t len)
{
    uint8_t rsp[4 + 4 * MYNEWT_VAL(BLE_BREDR_SDP_MAX_RECORDS) + 1];
    struct ble_br_sdp_elem pattern;
    uint16_t count;
    uint16_t max;
    int i;

    if (ble_br_sdp_elem_parse(data, len, &pattern) != 0 ||
        pattern.type != BLE_BR_SDP_DE_SEQ || len != pattern.total + 2 + 1 ||
        data[pattern.total + 2] != 0) {
        ble_br_sdp_error_tx(chan, tid, BLE_BR_SDP_ERR_INVALID_SYNTAX);
        return;
    }

    max = get_be16(data + pattern.total);
    count = 0;
    for (i = 0; i < ble_br_sdp_num_records && count < max; i++) {
        if (ble_br_sdp_record_matches(ble_br_sdp_records[i], &pattern)) {
            put_be32(&rsp[4 + 4 * count], BLE_BR_SDP_HANDLE_BASE + i);
            count++;
        }
    }

    put_be16(&rsp[0], count);
    put_be16(&rsp[2], count);
    rsp[4 + 4 * count] = 0;

    ble_br_sdp_rsp_tx(chan, BLE_BR_SDP_PDU_SEARCH_RSP, tid, rsp, 4 + 4 * count + 1);
}

/* Answers the two requests that return attributes */
static void
ble_br_sdp_server_attr(struct ble_br_l2cap_chan *chan, uint8_t pdu,
                       uint16_t tid, const uint8_t *data, uint16_t len)
{
    uint8_t rsp[2 + BLE_BR_SDP_SERVER_BUF_SZ + 1 + BLE_BR_SDP_CONT_LEN];
    uint8_t list[BLE_BR_SDP_SERVER_BUF_SZ];
    struct ble_br_sdp_elem pattern;
    struct ble_br_sdp_elem id_list;
    uint16_t list_len;
    uint16_t rsp_len;
    uint16_t offset;
    uint16_t chunk;
    uint16_t max;
    uint32_t idx;
    int rc;
    int i;

    idx = 0;
    if (pdu == BLE_BR_SDP_PDU_ATTR_REQ) {
        if (len < 4) {
            goto syntax;
        }
        idx = get_be32(data) - BLE_BR_SDP_HANDLE_BASE;
        if (idx >= ble_br_sdp_num_records) {
            ble_br_sdp_error_tx(chan, tid, BLE_BR_SDP_ERR_INVALID_HANDLE);
            return;
        }
        data += 4;
        len -= 4;
    } else {
        if (ble_br_sdp_elem_parse(data, len, &pattern) != 0 ||
            pattern.type != BLE_BR_SDP_DE_SEQ) {
            goto syntax;
        }
        data += pattern.total;
        len -= pattern.total;
    }

    if (len < 2) {
        goto syntax;
    }
    max = get_be16(data);
    data += 2;
    len -= 2;

    if (ble_br_sdp_elem_parse(data, len, &id_list) != 0 ||
        id_list.type != BLE_BR_SDP_DE_SEQ) {
        goto syntax;
    }
    data += id_list.total;
    len -= id_list.total;

    offset = 0;
    if (len == 1 + BLE_BR_SDP_CONT_LEN && data[0] == BLE_BR_SDP_CONT_LEN) {
        offset = get_be16(&data[1]);
    } else if (len != 1 || data[0] != 0) {
        ble_br_sdp_error_tx(chan, tid, BLE_BR_SDP_ERR_INVALID_CONT);
        return;
    }

    list_len = 0;
    if (pdu == BLE_BR_SDP_PDU_ATTR_REQ) {
        rc = ble_br_sdp_put_record(list, sizeof(list), &list_len, idx, &id_list);
    } else {
        rc = ble_br_sdp_put(list, sizeof(list), &list_len,
                            ble_br_sdp_empty_seq, sizeof(ble_br_sdp_empty_seq));
        for (i = 0; rc == 0 && i < ble_br_sdp_num_records; i++) {
            if (ble_br_sdp_record_matches(ble_br_sdp_records[i], &pattern)) {
                rc = ble_br_sdp_put_record(list, sizeof(list), &list_len, i, &id_list);
            }
        }
        list[1] = list_len - 2;
    }
    if (rc != 0) {
        ble_br_sdp_error_tx(chan, tid, BLE_BR_SDP_ERR_NO_RESOURCES);
        return;
    }

    if (offset >= list_len) {
        ble_br_sdp_error_tx(chan, tid, BLE_BR_SDP_ERR_INVALID_CONT);
        return;
    }

    chunk = BLE_BR_MIN(list_len - offset, max);
    chunk = BLE_BR_MIN(chunk, ble_br_l2cap_chan_peer_mtu(chan) - BLE_BR_SDP_HDR_SZ -
                                  2 - 1 - BLE_BR_SDP_CONT_LEN);

    put_be16(&rsp[0], chunk);
    memcpy(&rsp[2], &list[offset], chunk);
    rsp_len = 2 + chunk;
    if (offset + chunk < list_len) {
        rsp[rsp_len++] = BLE_BR_SDP_CONT_LEN;
        put_be16(&rsp[rsp_len], offset + chunk);
        rsp_len += BLE_BR_SDP_CONT_LEN;
    } else {
        rsp[rsp_len++] = 0;
    }

    ble_br_sdp_rsp_tx(chan, pdu + 1, tid, rsp, rsp_len);
    return;

syntax:
    ble_br_sdp_error_tx(chan, tid, BLE_BR_SDP_ERR_INVALID_SYNTAX);
}

static void
ble_br_sdp_server_connected(struct ble_br_l2cap_chan *chan, int status, void *arg)
{
}

static void
ble_br_sdp_server_disconnected(struct ble_br_l2cap_chan *chan, void *arg)
{
}

static void
ble_br_sdp_server_recv(struct ble_br_l2cap_chan *chan, struct os_mbuf *om, void *arg)
{
    uint8_t buf[BLE_BR_SDP_SERVER_BUF_SZ];
    uint16_t len;
    uint16_t tid;

    len = OS_MBUF_PKTLEN(om);
    if (len < BLE_BR_SDP_HDR_SZ || len > sizeof(buf)) {
        return;
    }
    os_mbuf_copydata(om, 0, len, buf);

    tid = get_be16(&buf[1]);
    if (get_be16(&buf[3]) != len - BLE_BR_SDP_HDR_SZ) {
        ble_br_sdp_error_tx(chan, tid, BLE_BR_SDP_ERR_INVALID_SYNTAX);
        return;
    }

    switch (buf[0]) {
    case BLE_BR_SDP_PDU_SEARCH_REQ:
        ble_br_sdp_server_search(chan, tid, &buf[BLE_BR_SDP_HDR_SZ],
                                 len - BLE_BR_SDP_HDR_SZ);
        break;

    case BLE_BR_SDP_PDU_ATTR_REQ:
    case BLE_BR_SDP_PDU_SEARCH_ATTR_REQ:
        ble_br_sdp_server_attr(chan, buf[0], tid, &buf[BLE_BR_SDP_HDR_SZ],
                               len - BLE_BR_SDP_HDR_SZ);
        break;

    default:
        ble_br_sdp_error_tx(chan, tid, BLE_BR_SDP_ERR_INVALID_SYNTAX);
        break;
    }
}

static const struct ble_br_l2cap_chan_ops ble_br_sdp_server_ops = {
    .connected = ble_br_sdp_server_connected,
    .disconnected = ble_br_sdp_server_disconnected,
    .recv = ble_br_sdp_server_recv,
};

static int
ble_br_sdp_server_accept(uint16_t conn_handle,
                         const struct ble_br_l2cap_chan_ops **ops, void **arg)
{
    *ops = &ble_br_sdp_server_ops;
    *arg = NULL;

    return 0;
}

static struct ble_br_l2cap_server ble_br_sdp_l2cap_server = {
    .psm = BLE_BR_L2CAP_PSM_SDP,
    .accept = ble_br_sdp_server_accept,
};

int
ble_br_sdp_register(const struct ble_br_sdp_record *record)
{
    if (ble_br_sdp_num_records == MYNEWT_VAL(BLE_BREDR_SDP_MAX_RECORDS)) {
        return BLE_HS_ENOMEM;
    }

    ble_br_sdp_records[ble_br_sdp_num_records++] = record;

    return 0;
}

static void
ble_br_sdp_client_done(int status, uint8_t channel)
{
    ble_br_sdp_rfcomm_fn *cb = ble_br_sdp_client.cb;

    if (ble_br_sdp_client.chan != NULL) {
        ble_br_l2cap_disconnect(ble_br_sdp_client.chan);
        ble_br_sdp_client.chan = NULL;
    }

    ble_br_sdp_client.cb = NULL;
    cb(ble_br_sdp_client.conn_handle, status, channel, ble_br_sdp_client.cb_arg);
}

static int
ble_br_sdp_client_req_tx(const uint8_t *cont, uint8_t cont_len)
{
    uint8_t req[BLE_BR_SDP_HDR_SZ + 5 + 2 + 5 + 1 + BLE_BR_SDP_CONT_MAX_LEN];
    struct os_mbuf *om;
    uint16_t max;
    uint8_t *p;

    if (++ble_br_sdp_client.rounds > BLE_BR_SDP_CLIENT_MAX_ROUNDS) {
        return BLE_HS_EBADDATA;
    }

    max = sizeof(ble_br_sdp_client.buf) - ble_br_sdp_client.buf_len;
    max = BLE_BR_MIN(max, MYNEWT_VAL(BLE_BREDR_L2CAP_MTU) - BLE_BR_SDP_HDR_SZ -
                              2 - 1 - BLE_BR_SDP_CONT_MAX_LEN);

    p = req;
    *p++ = BLE_BR_SDP_PDU_SEARCH_ATTR_REQ;
    put_be16(p, ++ble_br_sdp_client.tid);
    p += 2;
    put_be16(p, 5 + 2 + 5 + 1 + cont_len);
    p += 2;

    /* Records of the service */
    *p++ = BLE_BR_SDP_DE_SEQ8;
    *p++ = 3;
    *p++ = BLE_BR_SDP_DE_UUID16;
    put_be16(p, ble_br_sdp_client.uuid16);
    p += 2;

    put_be16(p, max);
    p += 2;

    /* How to reach it */
    *p++ = BLE_BR_SDP_DE_SEQ8;
    *p++ = 3;
    *p++ = BLE_BR_SDP_DE_UINT16;
    put_be16(p, BLE_BR_SDP_ATTR_PROTOCOL_DESC_LIST);
    p += 2;

    *p++ = cont_len;
    memcpy(p, cont, cont_len);
    p += cont_len;

    om = ble_br_l2cap_alloc();
    if (om == NULL) {
        return BLE_HS_ENOMEM;
    }
    if (os_mbuf_append(om, req, p - req) != 0) {
        os_mbuf_free_chain(om);
        return BLE_HS_ENOMEM;
    }

    return ble_br_l2cap_send(ble_br_sdp_client.chan, om);
}

/* Finds the RFCOMM channel in the attribute lists of the matching records */
static uint8_t
ble_br_sdp_client_parse(const uint8_t *data, uint16_t len)
{
    struct ble_br_sdp_elem records;
    struct ble_br_sdp_elem record;
    struct ble_br_sdp_elem protocols;
    struct ble_br_sdp_elem protocol;
    struct ble_br_sdp_elem elem;
    struct ble_br_sdp_elem id;

    if (ble_br_sdp_elem_parse(data, len, &records) != 0 ||
        records.type != BLE_BR_SDP_DE_SEQ) {
        return 0;
    }

    while (records.len > 0) {
        if (ble_br_sdp_elem_next(&records, &record) != 0 ||
            record.type != BLE_BR_SDP_DE_SEQ) {
            return 0;
        }

        while (record.len > 0) {
            if (ble_br_sdp_elem_next(&record, &id) != 0 || id.type != BLE_BR_SDP_DE_UINT ||
                id.len != 2 || ble_br_sdp_elem_next(&record, &protocols) != 0) {
                return 0;
            }

            if (get_be16(id.data) != BLE_BR_SDP_ATTR_PROTOCOL_DESC_LIST ||
                protocols.type != BLE_BR_SDP_DE_SEQ) {
                continue;
            }

            while (protocols.len > 0) {
                if (ble_br_sdp_elem_next(&protocols, &protocol) != 0 ||
                    protocol.type != BLE_BR_SDP_DE_SEQ ||
                    ble_br_sdp_elem_next(&protocol, &elem) != 0) {
                    return 0;
                }

                if (ble_br_sdp_elem_uuid16(&elem) != BLE_BR_SDP_UUID_RFCOMM) {
                    continue;
                }

                if (ble_br_sdp_elem_next(&protocol, &elem) == 0 &&
                    elem.type == BLE_BR_SDP_DE_UINT && elem.len == 1) {
                    return elem.data[0];
                }
            }
        }
    }

    return 0;
}

static void
ble_br_sdp_client_connected(struct ble_br_l2cap_chan *chan, int status, void *arg)
{
    if (status != 0) {
        ble_br_sdp_client.chan = NULL;
        ble_br_sdp_client_done(status, 0);
        return;
    }

    status = ble_br_sdp_client_req_tx(NULL, 0);
    if (status != 0) {
        ble_br_sdp_client_done(status, 0);
    }
}

static void
ble_br_sdp_client_disconnected(struct ble_br_l2cap_chan *chan, void *arg)
{
    ble_br_sdp_client.chan = NULL;
    ble_br_sdp_client_done(BLE_HS_ENOTCONN, 0);
}

static void
ble_br_sdp_client_recv(struct ble_br_l2cap_chan *chan, struct os_mbuf *om, void *arg)
{
    uint8_t buf[BLE_BR_SDP_HDR_SZ + 2 + BLE_BR_SDP_CLIENT_BUF_SZ + 1 + BLE_BR_SDP_CONT_MAX_LEN];
    uint16_t count;
    uint16_t len;
    uint8_t cont_len;
    int rc;

    len = OS_MBUF_PKTLEN(om);
    if (len < BLE_BR_SDP_HDR_SZ || len > sizeof(buf)) {
        goto err;
    }
    os_mbuf_copydata(om, 0, len, buf);

    if (get_be16(&buf[1]) != ble_br_sdp_client.tid) {
        return;
    }

    if (buf[0] != BLE_BR_SDP_PDU_SEARCH_ATTR_RSP ||
        get_be16(&buf[3]) != len - BLE_BR_SDP_HDR_SZ ||
        len < BLE_BR_SDP_HDR_SZ + 2 + 1) {
        goto err;
    }

    count = get_be16(&buf[BLE_BR_SDP_HDR_SZ]);
    if (count > len - BLE_BR_SDP_HDR_SZ - 2 - 1 ||
        count > sizeof(ble_br_sdp_client.buf) - ble_br_sdp_client.buf_len) {
        goto err;
    }

    cont_len = buf[BLE_BR_SDP_HDR_SZ + 2 + count];
    if (cont_len > BLE_BR_SDP_CONT_MAX_LEN ||
        len != BLE_BR_SDP_HDR_SZ + 2 + count + 1 + cont_len) {
        goto err;
    }

    memcpy(&ble_br_sdp_client.buf[ble_br_sdp_client.buf_len],
           &buf[BLE_BR_SDP_HDR_SZ + 2], count);
    ble_br_sdp_client.buf_len += count;

    if (cont_len > 0) {
        rc = ble_br_sdp_client_req_tx(&buf[BLE_BR_SDP_HDR_SZ + 2 + count + 1], cont_len);
        if (rc != 0) {
            goto err;
        }
        return;
    }

    ble_br_sdp_client_done(0, ble_br_sdp_client_parse(ble_br_sdp_client.buf,
                                                      ble_br_sdp_client.buf_len));
    return;

err:
    ble_br_sdp_client_done(BLE_HS_EBADDATA, 0);
}

static const struct ble_br_l2cap_chan_ops ble_br_sdp_client_ops = {
    .connected = ble_br_sdp_client_connected,
    .disconnected = ble_br_sdp_client_disconnected,
    .recv = ble_br_sdp_client_recv,
};

int
ble_br_sdp_find_rfcomm(uint16_t conn_handle, uint16_t uuid16,
                       ble_br_sdp_rfcomm_fn *cb, void *cb_arg)
{
    int rc;

    if (ble_br_sdp_client.cb != NULL) {
        return BLE_HS_EBUSY;
    }

    ble_br_sdp_client.conn_handle = conn_handle;
    ble_br_sdp_client.uuid16 = uuid16;
    ble_br_sdp_client.cb = cb;
    ble_br_sdp_client.cb_arg = cb_arg;
    ble_br_sdp_client.rounds = 0;
    ble_br_sdp_client.exp = ble_br_hs_deadline(BLE_BR_SDP_CLIENT_TIMEOUT);
    ble_br_sdp_client.buf_len = 0;

    rc = ble_br_l2cap_connect(conn_handle, BLE_BR_L2CAP_PSM_SDP,
                              &ble_br_sdp_client_ops, NULL, &ble_br_sdp_client.chan);
    if (rc != 0) {
        ble_br_sdp_client.cb = NULL;
    }

    return rc;
}

void
ble_br_sdp_timer(ble_npl_time_t now, int32_t *ticks)
{
    if (ble_br_sdp_client.cb != NULL &&
        ble_br_hs_expired(ble_br_sdp_client.exp, now, ticks)) {
        ble_br_sdp_client_done(BLE_HS_ETIMEOUT, 0);
    }
}

void
ble_br_sdp_init(void)
{
    ble_br_sdp_num_records = 0;
    memset(&ble_br_sdp_client, 0, sizeof(ble_br_sdp_client));

    ble_br_l2cap_server_register(&ble_br_sdp_l2cap_server);
}

#endif
