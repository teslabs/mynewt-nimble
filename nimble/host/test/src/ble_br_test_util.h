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

#ifndef H_BLE_BR_TEST_UTIL_
#define H_BLE_BR_TEST_UTIL_

#include <inttypes.h>
#include "host/ble_br_gap.h"
#include "ble_hs_test_util.h"
#include "ble_br_priv.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ACL buffers of the controller; small ones, to have PDUs fragmented. */
#define BLE_BR_TEST_UTIL_ACL_LEN  64
#define BLE_BR_TEST_UTIL_ACL_PKTS 200

#define BLE_BR_TEST_UTIL_LC_OPCODE(ocf)                                       \
    ble_hs_hci_util_opcode_join(BLE_HCI_OGF_LINK_CTRL, (ocf))

/* L2CAP signaling. */
#define BLE_BR_TEST_UTIL_CID_SIG 0x0001

#define BLE_BR_TEST_UTIL_SIG_CMD_REJECT  0x01
#define BLE_BR_TEST_UTIL_SIG_CONN_REQ    0x02
#define BLE_BR_TEST_UTIL_SIG_CONN_RSP    0x03
#define BLE_BR_TEST_UTIL_SIG_CONFIG_REQ  0x04
#define BLE_BR_TEST_UTIL_SIG_CONFIG_RSP  0x05
#define BLE_BR_TEST_UTIL_SIG_DISCONN_REQ 0x06
#define BLE_BR_TEST_UTIL_SIG_DISCONN_RSP 0x07
#define BLE_BR_TEST_UTIL_SIG_ECHO_REQ    0x08
#define BLE_BR_TEST_UTIL_SIG_ECHO_RSP    0x09
#define BLE_BR_TEST_UTIL_SIG_INFO_REQ    0x0a
#define BLE_BR_TEST_UTIL_SIG_INFO_RSP    0x0b

/* RFCOMM frames. */
#define BLE_BR_TEST_UTIL_RFCOMM_SABM 0x2f
#define BLE_BR_TEST_UTIL_RFCOMM_UA   0x63
#define BLE_BR_TEST_UTIL_RFCOMM_DM   0x0f
#define BLE_BR_TEST_UTIL_RFCOMM_DISC 0x43
#define BLE_BR_TEST_UTIL_RFCOMM_UIH  0xef
#define BLE_BR_TEST_UTIL_RFCOMM_PF   0x10

/* Address field of a frame: EA bit, C/R bit and DLCI. */
#define BLE_BR_TEST_UTIL_RFCOMM_ADDR(dlci, cr)                                \
    (0x01 | ((cr) << 1) | ((dlci) << 2))

/* Type field of a multiplexer command or response. */
#define BLE_BR_TEST_UTIL_RFCOMM_MCC(type, cr)                                 \
    (0x01 | ((cr) << 1) | ((type) << 2))

#define BLE_BR_TEST_UTIL_RFCOMM_MCC_NSC 0x04
#define BLE_BR_TEST_UTIL_RFCOMM_MCC_PN  0x20
#define BLE_BR_TEST_UTIL_RFCOMM_MCC_RPN 0x24
#define BLE_BR_TEST_UTIL_RFCOMM_MCC_MSC 0x38

/* Credits the host gives for a link, and when it gives them again. */
#define BLE_BR_TEST_UTIL_RFCOMM_CREDITS     7
#define BLE_BR_TEST_UTIL_RFCOMM_CREDITS_LOW 2

/* An L2CAP channel, as the peer sees it. */
struct ble_br_test_util_chan {
    uint16_t conn_handle;
    uint16_t our_cid;
    uint16_t peer_cid;
};

void ble_br_test_util_init(void);
void ble_br_test_util_init_buf(uint16_t acl_len, uint16_t acl_pkts, int shared);

/* $gap events */
const struct ble_br_gap_event *ble_br_test_util_gap_event_next(void);
int ble_br_test_util_gap_event_cnt(void);

/* $hci */
void ble_br_test_util_rx_evt(uint8_t code, const void *params, uint8_t len);
void ble_br_test_util_ack_set(uint16_t ocf, uint8_t status);
void ble_br_test_util_ack_set_addr(uint16_t ocf, const uint8_t *addr);
void ble_br_test_util_verify_tx_addr(uint16_t ocf, const uint8_t *addr);
void ble_br_test_util_rx_conn_request(const uint8_t *addr, uint8_t link_type);
void ble_br_test_util_rx_conn_cmp(uint8_t status, uint16_t conn_handle,
                                  const uint8_t *addr, uint8_t link_type,
                                  uint8_t encrypted);
void ble_br_test_util_rx_enc_change(uint16_t conn_handle, uint8_t status,
                                    uint8_t enabled);
void ble_br_test_util_create_conn(uint16_t conn_handle, const uint8_t *addr,
                                  int encrypted);
void ble_br_test_util_rx_num_completed_pkts(uint16_t conn_handle, uint16_t num_pkts);

/* $l2cap */
int ble_br_test_util_rx_acl(uint16_t conn_handle, uint8_t pb, const void *data,
                            uint16_t len);
void ble_br_test_util_rx_l2cap(uint16_t conn_handle, uint16_t cid,
                               const void *data, uint16_t len);
void ble_br_test_util_rx_sig(uint16_t conn_handle, uint8_t code, uint8_t ident,
                             const void *data, uint16_t len);
uint8_t ble_br_test_util_next_ident(void);
const uint8_t *ble_br_test_util_tx_dequeue(uint16_t *out_conn_handle,
                                           uint16_t *out_cid, uint16_t *out_len);
void ble_br_test_util_verify_tx_l2cap(uint16_t conn_handle, uint16_t cid,
                                      const void *data, uint16_t len);
const uint8_t *ble_br_test_util_verify_tx_sig(uint16_t conn_handle, uint8_t code,
                                              uint8_t *out_ident, uint16_t len);
void ble_br_test_util_tx_queue_clear(void);
void ble_br_test_util_verify_tx_none(void);
void ble_br_test_util_l2cap_config(const struct ble_br_test_util_chan *chan);
void ble_br_test_util_l2cap_rx_connect(struct ble_br_test_util_chan *chan,
                                       uint16_t conn_handle, uint16_t psm,
                                       uint16_t peer_cid);
void ble_br_test_util_l2cap_rx_connect_rsp(struct ble_br_test_util_chan *chan,
                                           uint16_t conn_handle, uint16_t psm,
                                           uint16_t peer_cid);
void ble_br_test_util_l2cap_verify_tx_disconnect(const struct ble_br_test_util_chan *chan);

/* $rfcomm */
uint8_t ble_br_test_util_rfcomm_fcs(const uint8_t *data, int len);
void ble_br_test_util_rfcomm_rx(const struct ble_br_test_util_chan *chan,
                                uint8_t addr, uint8_t ctrl, int credits,
                                const void *data, uint16_t len);
const uint8_t *ble_br_test_util_rfcomm_verify_tx(const struct ble_br_test_util_chan *chan,
                                                 uint8_t addr, uint8_t ctrl,
                                                 int credits, uint16_t *out_len);
void ble_br_test_util_rfcomm_rx_mcc(const struct ble_br_test_util_chan *chan,
                                    uint8_t addr, uint8_t type,
                                    const void *data, uint8_t len);
void ble_br_test_util_rfcomm_verify_tx_mcc(const struct ble_br_test_util_chan *chan,
                                           uint8_t addr, uint8_t type,
                                           const void *data, uint8_t len);
void ble_br_test_util_rfcomm_rx_session(struct ble_br_test_util_chan *chan,
                                        uint16_t conn_handle);
void ble_br_test_util_rfcomm_rx_open(const struct ble_br_test_util_chan *chan,
                                     uint8_t dlci, uint8_t credits);
void ble_br_test_util_rfcomm_rx_accept(struct ble_br_test_util_chan *chan,
                                       uint16_t conn_handle, uint8_t dlci,
                                       uint8_t credits);

#if MYNEWT_VAL(BLE_BREDR_SCO)
/* $sco */
void ble_br_test_util_rx_synch_conn_cmp(uint8_t status, uint16_t sco_handle,
                                        const uint8_t *addr, uint8_t link_type);
void ble_br_test_util_verify_tx_accept_synch_conn(const uint8_t *addr,
                                                  const struct ble_br_sco_params *params);
int ble_br_test_util_sco_tx_cnt(void);
const uint8_t *ble_br_test_util_sco_tx_last(uint16_t *out_len);
#endif

/* $time */
int32_t ble_br_test_util_time_advance(int32_t ticks);

#ifdef __cplusplus
}
#endif

#endif
