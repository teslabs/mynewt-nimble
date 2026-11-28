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
#include "host/ble_hs.h"
#include "ble_hs_priv.h"
#include "ble_br_priv.h"

#define BLE_BR_GAP_EIR_MAX_UUIDS 4

static ble_br_gap_event_fn *ble_br_gap_cb;
static void *ble_br_gap_cb_arg;

static char ble_br_gap_name[BLE_BR_GAP_NAME_MAX_LEN + 1] = "nimble";
static uint32_t ble_br_gap_cod;
static uint8_t ble_br_gap_scan_mode;
static uint16_t ble_br_gap_eir_uuids[BLE_BR_GAP_EIR_MAX_UUIDS];
static uint8_t ble_br_gap_eir_num_uuids;
static uint64_t ble_br_gap_features;

static void
ble_br_gap_event(const struct ble_br_gap_event *event)
{
    if (ble_br_gap_cb != NULL) {
        ble_br_gap_cb(event, ble_br_gap_cb_arg);
    }
}

void
ble_br_gap_set_event_cb(ble_br_gap_event_fn *cb, void *cb_arg)
{
    ble_br_gap_cb = cb;
    ble_br_gap_cb_arg = cb_arg;
}

uint64_t
ble_br_gap_local_features(void)
{
    return ble_br_gap_features;
}

static void
ble_br_gap_link_key_addr(ble_addr_t *out_addr, const uint8_t *addr)
{
    out_addr->type = BLE_ADDR_PUBLIC;
    memcpy(out_addr->val, addr, BLE_DEV_ADDR_LEN);
}

static int
ble_br_gap_link_key_find(const uint8_t *addr,
                         struct ble_store_value_br_link_key *out_value)
{
    struct ble_store_key_br_link_key key;

    memset(&key, 0, sizeof(key));
    ble_br_gap_link_key_addr(&key.peer_addr, addr);

    return ble_store_read_br_link_key(&key, out_value);
}

/* The keys are kept by the store, next to the LE ones of the peer */
static int
ble_br_gap_link_key_store(const uint8_t *addr, const uint8_t *key, uint8_t key_type)
{
    struct ble_store_value_br_link_key value;

    memset(&value, 0, sizeof(value));
    ble_br_gap_link_key_addr(&value.peer_addr, addr);
    memcpy(value.key, key, sizeof(value.key));
    value.key_type = key_type;

    return ble_store_write_br_link_key(&value);
}

static int
ble_br_gap_wr_name(void)
{
    uint8_t name[BLE_HCI_LOCAL_NAME_LEN];

    memset(name, 0, sizeof(name));
    memcpy(name, ble_br_gap_name, strlen(ble_br_gap_name));

    return ble_br_hs_cmd(BLE_HCI_OGF_CTLR_BASEBAND, BLE_HCI_OCF_CB_WR_LOCAL_NAME,
                         name, sizeof(name), NULL, 0);
}

static int
ble_br_gap_wr_eir(void)
{
    struct ble_hci_cb_wr_eir_cp cmd;
    uint8_t name_len;
    uint8_t *p;
    int i;

    memset(&cmd, 0, sizeof(cmd));
    p = cmd.eir;

    name_len = strlen(ble_br_gap_name);
    *p++ = name_len + 1;
    *p++ = BLE_HCI_EIR_TYPE_NAME_COMPLETE;
    memcpy(p, ble_br_gap_name, name_len);
    p += name_len;

    if (ble_br_gap_eir_num_uuids > 0) {
        *p++ = 1 + 2 * ble_br_gap_eir_num_uuids;
        *p++ = BLE_HCI_EIR_TYPE_UUID16_COMPLETE;
        for (i = 0; i < ble_br_gap_eir_num_uuids; i++) {
            put_le16(p, ble_br_gap_eir_uuids[i]);
            p += 2;
        }
    }

    return ble_br_hs_cmd(BLE_HCI_OGF_CTLR_BASEBAND, BLE_HCI_OCF_CB_WR_EIR,
                         &cmd, sizeof(cmd), NULL, 0);
}

static int
ble_br_gap_wr_cod(void)
{
    uint8_t cod[BLE_HCI_CLASS_OF_DEVICE_LEN];

    put_le24(cod, ble_br_gap_cod);

    return ble_br_hs_cmd(BLE_HCI_OGF_CTLR_BASEBAND, BLE_HCI_OCF_CB_WR_CLASS_OF_DEVICE,
                         cod, sizeof(cod), NULL, 0);
}

static int
ble_br_gap_wr_scan_enable(void)
{
    return ble_br_hs_cmd(BLE_HCI_OGF_CTLR_BASEBAND,
                         BLE_HCI_OCF_CB_WR_SCAN_ENABLE, &ble_br_gap_scan_mode,
                         sizeof(ble_br_gap_scan_mode), NULL, 0);
}

int
ble_br_gap_startup(void)
{
    struct ble_hci_lp_wr_default_link_policy_cp policy;
    struct ble_hci_ip_rd_loc_supp_feat_rp feat;
    uint8_t ssp_mode;
    int rc;

    rc = ble_br_hs_cmd(BLE_HCI_OGF_INFO_PARAMS, BLE_HCI_OCF_IP_RD_LOC_SUPP_FEAT,
                       NULL, 0, &feat, sizeof(feat));
    if (rc != 0) {
        return rc;
    }
    ble_br_gap_features = le64toh(feat.features);

    ssp_mode = 1;
    rc = ble_br_hs_cmd(BLE_HCI_OGF_CTLR_BASEBAND, BLE_HCI_OCF_CB_WR_SSP_MODE,
                       &ssp_mode, sizeof(ssp_mode), NULL, 0);
    if (rc != 0) {
        return rc;
    }

    /*
     * Lets a phone that is central of the LE link be the one here too, and
     * either side put an idle connection in sniff mode
     */
    policy.settings =
        htole16(BLE_HCI_LINK_POLICY_ROLE_SWITCH | BLE_HCI_LINK_POLICY_SNIFF);
    rc = ble_br_hs_cmd(BLE_HCI_OGF_LINK_POLICY, BLE_HCI_OCF_LP_WR_DEFAULT_LINK_POLICY,
                       &policy, sizeof(policy), NULL, 0);
    if (rc != 0 && rc != BLE_HS_HCI_ERR(BLE_ERR_UNKNOWN_HCI_CMD) &&
        rc != BLE_HS_HCI_ERR(BLE_ERR_UNSUPPORTED)) {
        return rc;
    }

    rc = ble_br_gap_wr_cod();
    if (rc != 0) {
        return rc;
    }

    rc = ble_br_gap_wr_name();
    if (rc != 0) {
        return rc;
    }

    rc = ble_br_gap_wr_eir();
    if (rc != 0) {
        return rc;
    }

    return ble_br_gap_wr_scan_enable();
}

void
ble_br_gap_reset(void)
{
    ble_br_gap_features = 0;
}

void
ble_br_gap_init(void)
{
    ble_br_gap_cb = NULL;
    ble_br_gap_cb_arg = NULL;
    strcpy(ble_br_gap_name, "nimble");
    ble_br_gap_cod = 0;
    ble_br_gap_scan_mode = BLE_BR_GAP_SCAN_NONE;
    ble_br_gap_eir_num_uuids = 0;
    ble_br_gap_features = 0;
}

static int
ble_br_gap_set_name_call(void *arg)
{
    const char *name = arg;
    int rc;

    if (strlen(name) > BLE_BR_GAP_NAME_MAX_LEN) {
        return BLE_HS_EINVAL;
    }

    strcpy(ble_br_gap_name, name);

    if (!ble_hs_synced() || !ble_br_gap_supported()) {
        return 0;
    }

    rc = ble_br_gap_wr_name();
    if (rc != 0) {
        return rc;
    }

    return ble_br_gap_wr_eir();
}

int
ble_br_gap_set_name(const char *name)
{
    return ble_br_hs_call(ble_br_gap_set_name_call, (void *)name);
}

static int
ble_br_gap_set_cod_call(void *arg)
{
    ble_br_gap_cod = *(uint32_t *)arg;

    if (!ble_hs_synced() || !ble_br_gap_supported()) {
        return 0;
    }

    return ble_br_gap_wr_cod();
}

int
ble_br_gap_set_class_of_device(uint32_t cod)
{
    return ble_br_hs_call(ble_br_gap_set_cod_call, &cod);
}

static int
ble_br_gap_set_scan_mode_call(void *arg)
{
    ble_br_gap_scan_mode = *(uint8_t *)arg;

    if (!ble_hs_synced() || !ble_br_gap_supported()) {
        return 0;
    }

    return ble_br_gap_wr_scan_enable();
}

int
ble_br_gap_set_scan_mode(uint8_t mode)
{
    if (mode & ~(BLE_BR_GAP_SCAN_INQUIRY | BLE_BR_GAP_SCAN_PAGE)) {
        return BLE_HS_EINVAL;
    }

    return ble_br_hs_call(ble_br_gap_set_scan_mode_call, &mode);
}

int
ble_br_gap_eir_add_uuid16(uint16_t uuid16)
{
    if (ble_br_gap_eir_num_uuids == BLE_BR_GAP_EIR_MAX_UUIDS) {
        return BLE_HS_ENOMEM;
    }

    ble_br_gap_eir_uuids[ble_br_gap_eir_num_uuids++] = uuid16;

    if (!ble_hs_synced() || !ble_br_gap_supported()) {
        return 0;
    }

    return ble_br_gap_wr_eir();
}

static int
ble_br_gap_connect_call(void *arg)
{
    struct ble_hci_lc_create_conn_cp cmd;
    struct ble_br_conn *conn;
    const uint8_t *addr = arg;
    int rc;

    if (!ble_hs_synced() || !ble_br_gap_supported()) {
        return BLE_HS_ENOTSUP;
    }

    if (ble_br_conn_find_by_addr(addr) != NULL) {
        return BLE_HS_EALREADY;
    }

    conn = ble_br_conn_alloc(addr);
    if (conn == NULL) {
        return BLE_HS_ENOMEM;
    }

    memset(&cmd, 0, sizeof(cmd));
    memcpy(cmd.bd_addr, addr, BLE_DEV_ADDR_LEN);
    cmd.pkt_type = htole16(BLE_HCI_ACL_PKT_TYPES);
    cmd.psr_mode = BLE_HCI_PSR_MODE_R1;
    cmd.allow_role_switch = 1;

    rc = ble_br_hs_cmd(BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_CREATE_CONN, &cmd,
                       sizeof(cmd), NULL, 0);
    if (rc != 0) {
        ble_br_conn_free(conn);
    }

    return rc;
}

int
ble_br_gap_connect(const uint8_t *addr)
{
    return ble_br_hs_call(ble_br_gap_connect_call, (void *)addr);
}

struct ble_br_gap_sniff_arg {
    uint16_t conn_handle;
    uint16_t min_interval;
    uint16_t max_interval;
    uint16_t attempt;
    uint16_t timeout;
};

static int
ble_br_gap_sniff_call(void *arg)
{
    struct ble_br_gap_sniff_arg *args = arg;
    struct ble_hci_lp_sniff_mode_cp cmd;

    if (ble_br_conn_find(args->conn_handle) == NULL) {
        return BLE_HS_ENOTCONN;
    }

    cmd.conn_handle = htole16(args->conn_handle);
    cmd.max_interval = htole16(args->max_interval);
    cmd.min_interval = htole16(args->min_interval);
    cmd.attempt = htole16(args->attempt);
    cmd.timeout = htole16(args->timeout);

    return ble_br_hs_cmd(BLE_HCI_OGF_LINK_POLICY, BLE_HCI_OCF_LP_SNIFF_MODE,
                         &cmd, sizeof(cmd), NULL, 0);
}

int
ble_br_gap_sniff(uint16_t conn_handle, uint16_t min_interval,
                 uint16_t max_interval, uint16_t attempt, uint16_t timeout)
{
    struct ble_br_gap_sniff_arg args = {
        .conn_handle = conn_handle,
        .min_interval = min_interval,
        .max_interval = max_interval,
        .attempt = attempt,
        .timeout = timeout,
    };

    return ble_br_hs_call(ble_br_gap_sniff_call, &args);
}

static int
ble_br_gap_exit_sniff_call(void *arg)
{
    struct ble_hci_lp_exit_sniff_mode_cp cmd;
    uint16_t conn_handle = *(uint16_t *)arg;

    if (ble_br_conn_find(conn_handle) == NULL) {
        return BLE_HS_ENOTCONN;
    }

    cmd.conn_handle = htole16(conn_handle);

    return ble_br_hs_cmd(BLE_HCI_OGF_LINK_POLICY, BLE_HCI_OCF_LP_EXIT_SNIFF_MODE,
                         &cmd, sizeof(cmd), NULL, 0);
}

int
ble_br_gap_exit_sniff(uint16_t conn_handle)
{
    return ble_br_hs_call(ble_br_gap_exit_sniff_call, &conn_handle);
}

struct ble_br_gap_sniff_subrating_arg {
    uint16_t conn_handle;
    uint16_t max_latency;
    uint16_t min_remote_timeout;
    uint16_t min_local_timeout;
};

static int
ble_br_gap_sniff_subrating_call(void *arg)
{
    struct ble_br_gap_sniff_subrating_arg *args = arg;
    struct ble_hci_lp_sniff_subrating_cp cmd;
    struct ble_hci_lp_sniff_subrating_rp rsp;

    if (ble_br_conn_find(args->conn_handle) == NULL) {
        return BLE_HS_ENOTCONN;
    }

    cmd.conn_handle = htole16(args->conn_handle);
    cmd.max_latency = htole16(args->max_latency);
    cmd.min_remote_timeout = htole16(args->min_remote_timeout);
    cmd.min_local_timeout = htole16(args->min_local_timeout);

    return ble_br_hs_cmd(BLE_HCI_OGF_LINK_POLICY, BLE_HCI_OCF_LP_SNIFF_SUBRATING,
                         &cmd, sizeof(cmd), &rsp, sizeof(rsp));
}

int
ble_br_gap_sniff_subrating(uint16_t conn_handle, uint16_t max_latency,
                           uint16_t min_remote_timeout, uint16_t min_local_timeout)
{
    struct ble_br_gap_sniff_subrating_arg args = {
        .conn_handle = conn_handle,
        .max_latency = max_latency,
        .min_remote_timeout = min_remote_timeout,
        .min_local_timeout = min_local_timeout,
    };

    return ble_br_hs_call(ble_br_gap_sniff_subrating_call, &args);
}

struct ble_br_gap_terminate_arg {
    uint16_t conn_handle;
    uint8_t hci_reason;
};

static int
ble_br_gap_terminate_call(void *arg)
{
    struct ble_br_gap_terminate_arg *args = arg;
    struct ble_hci_lc_disconnect_cp cmd;

    if (ble_br_conn_find(args->conn_handle) == NULL) {
        return BLE_HS_ENOTCONN;
    }

    cmd.conn_handle = htole16(args->conn_handle);
    cmd.reason = args->hci_reason;

    return ble_br_hs_cmd(BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_DISCONNECT_CMD,
                         &cmd, sizeof(cmd), NULL, 0);
}

int
ble_br_gap_terminate(uint16_t conn_handle, uint8_t hci_reason)
{
    struct ble_br_gap_terminate_arg args = {
        .conn_handle = conn_handle,
        .hci_reason = hci_reason,
    };

    return ble_br_hs_call(ble_br_gap_terminate_call, &args);
}

static int
ble_br_gap_set_encrypt(struct ble_br_conn *conn)
{
    struct ble_hci_lc_set_conn_encrypt_cp cmd;

    cmd.conn_handle = htole16(conn->handle);
    cmd.enable = 1;

    return ble_br_hs_cmd(BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_SET_CONN_ENCRYPT,
                         &cmd, sizeof(cmd), NULL, 0);
}

int
ble_br_gap_security_initiate_conn(struct ble_br_conn *conn)
{
    struct ble_hci_lc_auth_requested_cp cmd;
    int rc;

    if (conn->flags & BLE_BR_CONN_F_ENCRYPTED) {
        return BLE_HS_EALREADY;
    }

    if (conn->flags & BLE_BR_CONN_F_AUTH_PENDING) {
        return 0;
    }

    cmd.conn_handle = htole16(conn->handle);

    rc = ble_br_hs_cmd(BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_AUTH_REQUESTED, &cmd,
                       sizeof(cmd), NULL, 0);
    if (rc == 0) {
        conn->flags |= BLE_BR_CONN_F_AUTH_PENDING;
    }

    return rc;
}

static int
ble_br_gap_security_initiate_call(void *arg)
{
    struct ble_br_conn *conn;

    conn = ble_br_conn_find(*(uint16_t *)arg);
    if (conn == NULL) {
        return BLE_HS_ENOTCONN;
    }

    return ble_br_gap_security_initiate_conn(conn);
}

int
ble_br_gap_security_initiate(uint16_t conn_handle)
{
    return ble_br_hs_call(ble_br_gap_security_initiate_call, &conn_handle);
}

static int
ble_br_gap_addr_cmd(uint16_t ocf, const uint8_t *addr)
{
    struct ble_hci_lc_bd_addr_cp cmd;
    struct ble_hci_lc_bd_addr_rp rsp;

    memcpy(cmd.bd_addr, addr, BLE_DEV_ADDR_LEN);

    return ble_br_hs_cmd(BLE_HCI_OGF_LINK_CTRL, ocf, &cmd, sizeof(cmd), &rsp,
                         sizeof(rsp));
}

struct ble_br_gap_user_confirm_arg {
    const uint8_t *addr;
    bool accept;
};

static int
ble_br_gap_user_confirm_call(void *arg)
{
    struct ble_br_gap_user_confirm_arg *args = arg;

    return ble_br_gap_addr_cmd(args->accept ? BLE_HCI_OCF_USER_CONFIRM_REPLY
                                            : BLE_HCI_OCF_USER_CONFIRM_NEG_REPLY,
                               args->addr);
}

int
ble_br_gap_user_confirm(const uint8_t *addr, bool accept)
{
    struct ble_br_gap_user_confirm_arg args = {
        .addr = addr,
        .accept = accept,
    };

    return ble_br_hs_call(ble_br_gap_user_confirm_call, &args);
}

struct ble_br_gap_link_key_arg {
    const uint8_t *addr;
    const uint8_t *key;
    uint8_t key_type;
};

static int
ble_br_gap_set_link_key_call(void *arg)
{
    struct ble_br_gap_link_key_arg *args = arg;

    return ble_br_gap_link_key_store(args->addr, args->key, args->key_type);
}

int
ble_br_gap_set_link_key(const uint8_t *addr, const uint8_t *key, uint8_t key_type)
{
    struct ble_br_gap_link_key_arg args = {
        .addr = addr,
        .key = key,
        .key_type = key_type,
    };

    return ble_br_hs_call(ble_br_gap_set_link_key_call, &args);
}

static int
ble_br_gap_delete_link_key_call(void *arg)
{
    struct ble_store_key_br_link_key key;

    memset(&key, 0, sizeof(key));
    ble_br_gap_link_key_addr(&key.peer_addr, arg);

    return ble_store_delete_br_link_key(&key);
}

int
ble_br_gap_delete_link_key(const uint8_t *addr)
{
    return ble_br_hs_call(ble_br_gap_delete_link_key_call, (void *)addr);
}

static uint8_t
ble_br_gap_io_cap(void)
{
    switch (ble_hs_cfg.sm_io_cap) {
    case BLE_HS_IO_KEYBOARD_DISPLAY:
        return BLE_HCI_IO_CAP_DISPLAY_YESNO;
    default:
        /* The other values are the same on both transports */
        return ble_hs_cfg.sm_io_cap;
    }
}

static void
ble_br_gap_rx_conn_request(const struct ble_hci_ev_conn_request *ev)
{
    struct ble_hci_lc_accept_conn_req_cp accept;
    struct ble_hci_lc_reject_conn_req_cp reject;
    struct ble_br_conn *conn;
    int rc;

    if (ev->link_type != BLE_HCI_LINK_TYPE_ACL) {
#if MYNEWT_VAL(BLE_BREDR_SCO)
        ble_br_sco_rx_conn_request(ev);
#else
        memcpy(reject.bd_addr, ev->bd_addr, BLE_DEV_ADDR_LEN);
        reject.reason = BLE_ERR_CONN_REJ_RESOURCES;
        ble_br_hs_cmd(BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_REJECT_SYNCH_CONN_REQ,
                      &reject, sizeof(reject), NULL, 0);
#endif
        return;
    }

    conn = NULL;
    if (ble_br_conn_find_by_addr(ev->bd_addr) == NULL) {
        conn = ble_br_conn_alloc(ev->bd_addr);
    }
    if (conn == NULL) {
        memcpy(reject.bd_addr, ev->bd_addr, BLE_DEV_ADDR_LEN);
        reject.reason = BLE_ERR_CONN_REJ_RESOURCES;
        ble_br_hs_cmd(BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_REJECT_CONN_REQ,
                      &reject, sizeof(reject), NULL, 0);
        return;
    }

    conn->flags |= BLE_BR_CONN_F_INCOMING;

    memcpy(accept.bd_addr, ev->bd_addr, BLE_DEV_ADDR_LEN);
    accept.role = BLE_HCI_ROLE_PERIPHERAL;
    rc = ble_br_hs_cmd(BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_ACCEPT_CONN_REQ,
                       &accept, sizeof(accept), NULL, 0);
    if (rc != 0) {
        ble_br_conn_free(conn);
    }
}

static void
ble_br_gap_rx_conn_cmp(const struct ble_hci_ev_conn_done *ev)
{
    struct ble_br_gap_event event;
    struct ble_br_conn *conn;

    conn = ble_br_conn_find_by_addr(ev->bd_addr);
    if (conn == NULL || (conn->flags & BLE_BR_CONN_F_CONNECTED)) {
        return;
    }

    memset(&event, 0, sizeof(event));
    event.type = BLE_BR_GAP_EVENT_CONNECT;
    memcpy(event.connect.addr, ev->bd_addr, BLE_DEV_ADDR_LEN);

    if (ev->status != 0) {
        ble_br_conn_free(conn);
        event.connect.status = BLE_HS_HCI_ERR(ev->status);
    } else {
        conn->handle = le16toh(ev->conn_handle);
        conn->flags |= BLE_BR_CONN_F_CONNECTED;
        if (ev->encryption_enabled) {
            conn->flags |= BLE_BR_CONN_F_ENCRYPTED;
        }
        event.connect.conn_handle = conn->handle;
    }

    ble_br_gap_event(&event);
}

void
ble_br_gap_rx_disconn_cmp(struct ble_br_conn *conn, uint8_t reason)
{
    struct ble_br_gap_event event;

    memset(&event, 0, sizeof(event));
    event.type = BLE_BR_GAP_EVENT_DISCONNECT;
    event.disconnect.conn_handle = conn->handle;
    event.disconnect.reason = BLE_HS_HCI_ERR(reason);
    memcpy(event.disconnect.addr, conn->addr, BLE_DEV_ADDR_LEN);

    conn->flags &= ~BLE_BR_CONN_F_CONNECTED;

    /* The audio first: its end is told while its profile is still up */
#if MYNEWT_VAL(BLE_BREDR_SCO)
    ble_br_sco_conn_broken(conn->addr);
#endif
    ble_br_l2cap_conn_broken(conn);
    ble_br_conn_free(conn);

    ble_br_gap_event(&event);
}

void
ble_br_gap_rx_enc_change(struct ble_br_conn *conn, uint8_t status, bool enabled)
{
    struct ble_br_gap_event event;

    if (status == 0) {
        if (enabled) {
            conn->flags |= BLE_BR_CONN_F_ENCRYPTED;
        } else {
            conn->flags &= ~BLE_BR_CONN_F_ENCRYPTED;
        }
    }

    memset(&event, 0, sizeof(event));
    event.type = BLE_BR_GAP_EVENT_ENC_CHANGE;
    event.enc_change.status = status == 0 ? 0 : BLE_HS_HCI_ERR(status);
    event.enc_change.conn_handle = conn->handle;
    event.enc_change.encrypted = (conn->flags & BLE_BR_CONN_F_ENCRYPTED) != 0;

    ble_br_l2cap_enc_change(conn);

    ble_br_gap_event(&event);
}

static void
ble_br_gap_rx_mode_change(const struct ble_hci_ev_mode_change *ev)
{
    struct ble_br_gap_event event;

    if (ble_br_conn_find(le16toh(ev->conn_handle)) == NULL) {
        return;
    }

    memset(&event, 0, sizeof(event));
    event.type = BLE_BR_GAP_EVENT_MODE_CHANGE;
    event.mode_change.status = ev->status == 0 ? 0 : BLE_HS_HCI_ERR(ev->status);
    event.mode_change.conn_handle = le16toh(ev->conn_handle);
    event.mode_change.sniff = ev->mode == BLE_HCI_MODE_SNIFF;
    event.mode_change.interval = le16toh(ev->interval);

    ble_br_gap_event(&event);
}

static void
ble_br_gap_rx_auth_cmp(const struct ble_hci_ev_auth_cmp *ev)
{
    struct ble_br_conn *conn;
    uint8_t status;

    conn = ble_br_conn_find(le16toh(ev->conn_handle));
    if (conn == NULL) {
        return;
    }

    conn->flags &= ~BLE_BR_CONN_F_AUTH_PENDING;

    status = ev->status;
    if (status == BLE_ERR_PINKEY_MISSING) {
        /* The peer no longer has the key: pair again next time */
        ble_br_gap_delete_link_key_call(conn->addr);
    }

    if (status == 0) {
        if (conn->flags & BLE_BR_CONN_F_ENCRYPTED) {
            return;
        }
        if (ble_br_gap_set_encrypt(conn) == 0) {
            return;
        }
        status = BLE_ERR_UNSPECIFIED;
    }

    ble_br_gap_rx_enc_change(conn, status, false);
}

static void
ble_br_gap_rx_link_key_req(const struct ble_hci_ev_bd_addr *ev)
{
    struct ble_hci_lc_link_key_req_reply_cp cmd;
    struct ble_hci_lc_bd_addr_rp rsp;
    struct ble_store_value_br_link_key value;

    if (ble_br_gap_link_key_find(ev->bd_addr, &value) != 0) {
        ble_br_gap_addr_cmd(BLE_HCI_OCF_LINK_KEY_REQ_NEG_REPLY, ev->bd_addr);
        return;
    }

    memcpy(cmd.bd_addr, ev->bd_addr, BLE_DEV_ADDR_LEN);
    memcpy(cmd.key, value.key, sizeof(cmd.key));
    ble_br_hs_cmd(BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_LINK_KEY_REQ_REPLY, &cmd,
                  sizeof(cmd), &rsp, sizeof(rsp));
}

static void
ble_br_gap_rx_link_key_notify(const struct ble_hci_ev_link_key_notify *ev)
{
    struct ble_br_gap_event event;

    ble_br_gap_link_key_store(ev->bd_addr, ev->key, ev->key_type);

    memset(&event, 0, sizeof(event));
    event.type = BLE_BR_GAP_EVENT_LINK_KEY;
    memcpy(event.link_key.addr, ev->bd_addr, BLE_DEV_ADDR_LEN);
    memcpy(event.link_key.key, ev->key, BLE_BR_GAP_LINK_KEY_LEN);
    event.link_key.key_type = ev->key_type;

    ble_br_gap_event(&event);
}

static void
ble_br_gap_rx_io_cap_req(const struct ble_hci_ev_bd_addr *ev)
{
    struct ble_hci_lc_io_cap_req_reply_cp cmd;
    struct ble_hci_lc_bd_addr_rp rsp;

    memcpy(cmd.bd_addr, ev->bd_addr, BLE_DEV_ADDR_LEN);
    cmd.io_cap = ble_br_gap_io_cap();
    cmd.oob_present = 0;
    cmd.auth_req = ble_hs_cfg.sm_bonding ? BLE_HCI_AUTH_REQ_GENERAL_BONDING
                                         : BLE_HCI_AUTH_REQ_NO_BONDING;
    if (ble_hs_cfg.sm_mitm && cmd.io_cap != BLE_HCI_IO_CAP_NO_INPUT_OUTPUT) {
        cmd.auth_req |= BLE_HCI_AUTH_REQ_MITM;
    }

    ble_br_hs_cmd(BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_IO_CAP_REQ_REPLY, &cmd,
                  sizeof(cmd), &rsp, sizeof(rsp));
}

static void
ble_br_gap_rx_user_confirm_req(const struct ble_hci_ev_user_confirm_req *ev)
{
    struct ble_br_gap_event event;

    /* Nothing to compare the value with: the pairing is not authenticated */
    if (ble_br_gap_cb == NULL || ble_br_gap_io_cap() == BLE_HCI_IO_CAP_NO_INPUT_OUTPUT) {
        ble_br_gap_addr_cmd(BLE_HCI_OCF_USER_CONFIRM_REPLY, ev->bd_addr);
        return;
    }

    memset(&event, 0, sizeof(event));
    event.type = BLE_BR_GAP_EVENT_USER_CONFIRM;
    memcpy(event.user_confirm.addr, ev->bd_addr, BLE_DEV_ADDR_LEN);
    event.user_confirm.numeric_value = le32toh(ev->numeric_value);

    ble_br_gap_event(&event);
}

static void
ble_br_gap_rx_simple_pair_cmp(const struct ble_hci_ev_simple_pair_comp *ev)
{
    struct ble_br_gap_event event;

    memset(&event, 0, sizeof(event));
    event.type = BLE_BR_GAP_EVENT_PAIRING_COMPLETE;
    event.pairing_complete.status = ev->status == 0 ? 0 : BLE_HS_HCI_ERR(ev->status);
    memcpy(event.pairing_complete.addr, ev->bd_addr, BLE_DEV_ADDR_LEN);

    ble_br_gap_event(&event);
}

int
ble_br_gap_rx_evt(uint8_t code, const void *data, unsigned int len)
{
    switch (code) {
    case BLE_HCI_EVCODE_CONN_REQUEST:
        if (len != sizeof(struct ble_hci_ev_conn_request)) {
            return BLE_HS_ECONTROLLER;
        }
        ble_br_gap_rx_conn_request(data);
        return 0;

    case BLE_HCI_EVCODE_CONN_DONE:
        if (len != sizeof(struct ble_hci_ev_conn_done)) {
            return BLE_HS_ECONTROLLER;
        }
        ble_br_gap_rx_conn_cmp(data);
        return 0;

    case BLE_HCI_EVCODE_AUTH_CMP:
        if (len != sizeof(struct ble_hci_ev_auth_cmp)) {
            return BLE_HS_ECONTROLLER;
        }
        ble_br_gap_rx_auth_cmp(data);
        return 0;

    case BLE_HCI_EVCODE_MODE_CHANGE:
        if (len != sizeof(struct ble_hci_ev_mode_change)) {
            return BLE_HS_ECONTROLLER;
        }
        ble_br_gap_rx_mode_change(data);
        return 0;

    case BLE_HCI_EVCODE_LINK_KEY_REQ:
        if (len != sizeof(struct ble_hci_ev_bd_addr)) {
            return BLE_HS_ECONTROLLER;
        }
        ble_br_gap_rx_link_key_req(data);
        return 0;

    case BLE_HCI_EVCODE_LINK_KEY_NOTIFY:
        if (len != sizeof(struct ble_hci_ev_link_key_notify)) {
            return BLE_HS_ECONTROLLER;
        }
        ble_br_gap_rx_link_key_notify(data);
        return 0;

    case BLE_HCI_EVCODE_PIN_CODE_REQ:
        if (len != sizeof(struct ble_hci_ev_bd_addr)) {
            return BLE_HS_ECONTROLLER;
        }
        /* Legacy pairing is not supported */
        ble_br_gap_addr_cmd(BLE_HCI_OCF_PIN_CODE_REQ_NEG_REPLY,
                            ((const struct ble_hci_ev_bd_addr *)data)->bd_addr);
        return 0;

    case BLE_HCI_EVOCDE_IO_CAP_REQ:
        if (len != sizeof(struct ble_hci_ev_bd_addr)) {
            return BLE_HS_ECONTROLLER;
        }
        ble_br_gap_rx_io_cap_req(data);
        return 0;

    case BLE_HCI_EVCODE_IO_CAP_RSP:
        return 0;

    case BLE_HCI_EVCODE_USER_CONFIRM_REQ:
        if (len != sizeof(struct ble_hci_ev_user_confirm_req)) {
            return BLE_HS_ECONTROLLER;
        }
        ble_br_gap_rx_user_confirm_req(data);
        return 0;

    case BLE_HCI_EVCODE_SIMPLE_PAIR_COMP:
        if (len != sizeof(struct ble_hci_ev_simple_pair_comp)) {
            return BLE_HS_ECONTROLLER;
        }
        ble_br_gap_rx_simple_pair_cmp(data);
        return 0;

    default:
        return BLE_HS_ENOTSUP;
    }
}

#endif
