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

#ifndef H_BLE_BR_PRIV_
#define H_BLE_BR_PRIV_

#include <stdbool.h>
#include <stdint.h>
#include "os/os.h"
#include "nimble/hci_common.h"
#include "host/ble_br_gap.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_BR_MIN(a, b) ((a) < (b) ? (a) : (b))

struct ble_hci_ev;
struct ble_hci_ev_disconn_cmp;
struct ble_hci_ev_enrypt_chg;

/*
 * All BR/EDR state belongs to the host task: events are handled there, and
 * the API hops to it with ble_br_hs_call().
 */

/* Host integration (ble_br_hs.c) */

void ble_br_hs_init(void);
void ble_br_hs_reset(void);
void ble_br_hs_set_features(uint64_t features);
uint64_t ble_br_hs_event_mask(void);
int ble_br_hs_set_buf_sz(uint16_t pktlen, uint16_t max_pkts, bool shared);
int ble_br_hs_startup(void);
int ble_br_hs_rx_evt(const struct ble_hci_ev *ev);
int ble_br_hs_rx_disconn_cmp(const struct ble_hci_ev_disconn_cmp *ev);
int ble_br_hs_rx_enc_change(const struct ble_hci_ev_enrypt_chg *ev);
void ble_br_hs_rx_num_completed_pkts(uint16_t handle, uint16_t num_pkts);
int ble_br_hs_rx_acl(uint16_t handle, uint8_t pb, struct os_mbuf *om);

int32_t ble_br_hs_timer(void);

/* A deadline some milliseconds from now, for the timer function of a module */
ble_npl_time_t ble_br_hs_deadline(uint32_t ms);
/* Whether a deadline has passed; if not, ticks takes the time left if sooner */
bool ble_br_hs_expired(ble_npl_time_t exp, ble_npl_time_t now, int32_t *ticks);

typedef int ble_br_hs_call_fn(void *arg);
int ble_br_hs_call(ble_br_hs_call_fn *fn, void *arg);

/* Connections (ble_br_hs.c) */

#define BLE_BR_CONN_F_INCOMING     0x01
#define BLE_BR_CONN_F_CONNECTED    0x02
#define BLE_BR_CONN_F_ENCRYPTED    0x04
#define BLE_BR_CONN_F_AUTH_PENDING 0x08
#define BLE_BR_CONN_F_TX_CONT      0x10

struct ble_br_conn {
    bool in_use;
    uint8_t flags;
    uint16_t handle;
    uint8_t addr[BLE_DEV_ADDR_LEN];

    /* ACL packets the controller holds */
    uint16_t outstanding_pkts;
    STAILQ_HEAD(, os_mbuf_pkthdr) txq;

    /* L2CAP PDU being reassembled */
    struct os_mbuf *rx_om;
    uint16_t rx_len;

    uint8_t sig_ident;
};

struct ble_br_conn *ble_br_conn_alloc(const uint8_t *addr);
void ble_br_conn_free(struct ble_br_conn *conn);
struct ble_br_conn *ble_br_conn_find(uint16_t handle);
struct ble_br_conn *ble_br_conn_find_by_addr(const uint8_t *addr);

/* Queues an L2CAP PDU, which is consumed in any case */
int ble_br_hs_acl_tx(struct ble_br_conn *conn, struct os_mbuf *om);

int ble_br_hs_cmd(uint8_t ogf, uint16_t ocf, const void *cmd, uint8_t cmd_len,
                  void *rsp, uint8_t rsp_len);

/* GAP (ble_br_gap.c) */

void ble_br_gap_init(void);
int ble_br_gap_startup(void);
void ble_br_gap_reset(void);
int ble_br_gap_rx_evt(uint8_t code, const void *data, unsigned int len);
void ble_br_gap_rx_disconn_cmp(struct ble_br_conn *conn, uint8_t reason);
void ble_br_gap_rx_enc_change(struct ble_br_conn *conn, uint8_t status, bool enabled);
int ble_br_gap_security_initiate_conn(struct ble_br_conn *conn);
uint64_t ble_br_gap_local_features(void);

/* Takes the link key derived from the LTK of an LE pairing with the peer */
void ble_br_gap_derived_link_key(const uint8_t *addr, const uint8_t *key,
                                 bool authenticated);

/* Adds a 16-bit service class to the extended inquiry response */
int ble_br_gap_eir_add_uuid16(uint16_t uuid16);

/* L2CAP (ble_br_l2cap.c) */

#define BLE_BR_L2CAP_PSM_SDP    0x0001
#define BLE_BR_L2CAP_PSM_RFCOMM 0x0003

struct ble_br_l2cap_chan;

struct ble_br_l2cap_chan_ops {
    /* The channel is open, or failed to open and is gone */
    void (*connected)(struct ble_br_l2cap_chan *chan, int status, void *arg);
    void (*disconnected)(struct ble_br_l2cap_chan *chan, void *arg);
    /* The SDU is only valid during the call */
    void (*recv)(struct ble_br_l2cap_chan *chan, struct os_mbuf *om, void *arg);
};

#define BLE_BR_L2CAP_SERVER_F_ENCRYPT 0x01

struct ble_br_l2cap_server {
    SLIST_ENTRY(ble_br_l2cap_server) next;
    uint16_t psm;
    uint8_t flags;
    /* Accepts the channel by giving its callbacks */
    int (*accept)(uint16_t conn_handle,
                  const struct ble_br_l2cap_chan_ops **ops, void **arg);
};

void ble_br_l2cap_init(void);
int ble_br_l2cap_server_register(struct ble_br_l2cap_server *server);
int ble_br_l2cap_connect(uint16_t conn_handle, uint16_t psm,
                         const struct ble_br_l2cap_chan_ops *ops, void *arg,
                         struct ble_br_l2cap_chan **out_chan);
int ble_br_l2cap_disconnect(struct ble_br_l2cap_chan *chan);
/* Sends an SDU, which is consumed in any case */
int ble_br_l2cap_send(struct ble_br_l2cap_chan *chan, struct os_mbuf *om);
uint16_t ble_br_l2cap_chan_conn_handle(const struct ble_br_l2cap_chan *chan);
uint16_t ble_br_l2cap_chan_peer_mtu(const struct ble_br_l2cap_chan *chan);
struct os_mbuf *ble_br_l2cap_alloc(void);
void ble_br_l2cap_rx(struct ble_br_conn *conn, uint8_t pb, struct os_mbuf *om);
void ble_br_l2cap_conn_broken(struct ble_br_conn *conn);
void ble_br_l2cap_enc_change(struct ble_br_conn *conn);
void ble_br_l2cap_timer(ble_npl_time_t now, int32_t *ticks);

/* SDP (ble_br_sdp.c) */

struct ble_br_sdp_attr {
    uint16_t id;
    /* The value, as a data element */
    const uint8_t *value;
    uint8_t value_len;
};

struct ble_br_sdp_record {
    const struct ble_br_sdp_attr *attrs;
    uint8_t num_attrs;
    /* UUIDs a search matches the record with */
    const uint16_t *uuids;
    uint8_t num_uuids;
};

/* channel is 0 if the peer has no such service */
typedef void ble_br_sdp_rfcomm_fn(uint16_t conn_handle, int status,
                                  uint8_t channel, void *arg);

void ble_br_sdp_init(void);
void ble_br_sdp_timer(ble_npl_time_t now, int32_t *ticks);
int ble_br_sdp_register(const struct ble_br_sdp_record *record);
int ble_br_sdp_find_rfcomm(uint16_t conn_handle, uint16_t uuid16,
                           ble_br_sdp_rfcomm_fn *cb, void *cb_arg);

/* RFCOMM (ble_br_rfcomm.c) */

struct ble_br_rfcomm_dlc;

struct ble_br_rfcomm_dlc_ops {
    /* The link is open, or failed to open and is gone */
    void (*connected)(struct ble_br_rfcomm_dlc *dlc, int status, void *arg);
    void (*disconnected)(struct ble_br_rfcomm_dlc *dlc, void *arg);
    void (*recv)(struct ble_br_rfcomm_dlc *dlc, const uint8_t *data,
                 uint16_t len, void *arg);
};

struct ble_br_rfcomm_server {
    SLIST_ENTRY(ble_br_rfcomm_server) next;
    uint8_t channel;
    int (*accept)(uint16_t conn_handle,
                  const struct ble_br_rfcomm_dlc_ops **ops, void **arg);
};

void ble_br_rfcomm_init(void);
void ble_br_rfcomm_timer(ble_npl_time_t now, int32_t *ticks);
int ble_br_rfcomm_server_register(struct ble_br_rfcomm_server *server);
int ble_br_rfcomm_connect(uint16_t conn_handle, uint8_t channel,
                          const struct ble_br_rfcomm_dlc_ops *ops, void *arg,
                          struct ble_br_rfcomm_dlc **out_dlc);
int ble_br_rfcomm_disconnect(struct ble_br_rfcomm_dlc *dlc);
int ble_br_rfcomm_send(struct ble_br_rfcomm_dlc *dlc, const uint8_t *data,
                       uint16_t len);
uint16_t ble_br_rfcomm_dlc_conn_handle(const struct ble_br_rfcomm_dlc *dlc);

/* Synchronous connections (ble_br_sco.c) */

struct ble_br_sco_params {
    uint16_t max_latency;
    uint8_t retrans_effort;
    uint16_t pkt_type;
};

struct ble_br_sco_ops {
    /* Whether to accept a connection, and how */
    int (*accept)(const uint8_t *addr, uint8_t link_type,
                  struct ble_br_sco_params *params);
    void (*connected)(const uint8_t *addr, int status, uint16_t sco_handle);
    void (*disconnected)(uint16_t sco_handle, int reason);
};

void ble_br_sco_reset(void);
void ble_br_sco_set_ops(const struct ble_br_sco_ops *ops);
int ble_br_sco_connect(uint16_t conn_handle, const struct ble_br_sco_params *params);
int ble_br_sco_disconnect(uint16_t sco_handle);
void ble_br_sco_rx_conn_request(const struct ble_hci_ev_conn_request *ev);
int ble_br_sco_rx_evt(uint8_t code, const void *data, unsigned int len);
int ble_br_sco_rx_disconn_cmp(uint16_t handle, uint8_t reason);
void ble_br_sco_conn_broken(const uint8_t *addr);

/* Hands-Free (ble_br_hfp_hf.c) */

void ble_br_hfp_hf_reset(void);
void ble_br_hfp_hf_conn_broken(uint16_t conn_handle);
void ble_br_hfp_hf_enc_change(uint16_t conn_handle, int status);
void ble_br_hfp_hf_timer(ble_npl_time_t now, int32_t *ticks);

#ifdef __cplusplus
}
#endif

#endif
