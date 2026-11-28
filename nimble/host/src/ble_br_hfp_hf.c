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

#if MYNEWT_VAL(BLE_BREDR_HFP_HF)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os/os.h"
#include "host/ble_hs_log.h"
#include "host/ble_br_hfp_hf.h"
#include "ble_hs_priv.h"
#include "ble_br_priv.h"
#include "ble_br_sdp_priv.h"

#define BLE_BR_HFP_HF_CHANNEL MYNEWT_VAL(BLE_BREDR_HFP_HF_RFCOMM_CHANNEL)

/* Version 1.7 of the profile */
#define BLE_BR_HFP_VERSION 0x0107

/* Features given in the service record */
#define BLE_BR_HFP_HF_SDP_FEAT_CLI    0x0004
#define BLE_BR_HFP_HF_SDP_FEAT_VOLUME 0x0010
#define BLE_BR_HFP_HF_SDP_FEATURES                                            \
    (BLE_BR_HFP_HF_SDP_FEAT_CLI | BLE_BR_HFP_HF_SDP_FEAT_VOLUME)

/* Features exchanged with AT+BRSF */
#define BLE_BR_HFP_HF_FEAT_CLI     0x0004
#define BLE_BR_HFP_HF_FEAT_VOLUME  0x0010
#define BLE_BR_HFP_HF_FEAT_ESCO_S4 0x0200
#define BLE_BR_HFP_AG_FEAT_ESCO_S4 0x0800

/* Settings of the audio connection: S4, S3, S1 and D1 of the profile */
#define BLE_BR_HFP_ESCO_S4_LATENCY 12
#define BLE_BR_HFP_ESCO_S4_PKT_TYPE                                           \
    (BLE_HCI_SYNCH_PKT_NO_3EV3 | BLE_HCI_SYNCH_PKT_NO_2EV5 | BLE_HCI_SYNCH_PKT_NO_3EV5)
#define BLE_BR_HFP_ESCO_S3_LATENCY 10
#define BLE_BR_HFP_ESCO_S3_PKT_TYPE                                           \
    (BLE_HCI_SYNCH_PKT_EV3 | BLE_HCI_SYNCH_PKT_NO_3EV3 |                      \
     BLE_HCI_SYNCH_PKT_NO_2EV5 | BLE_HCI_SYNCH_PKT_NO_3EV5)
#define BLE_BR_HFP_ESCO_S1_LATENCY 7
#define BLE_BR_HFP_ESCO_S1_PKT_TYPE                                                  \
    (BLE_HCI_SYNCH_PKT_EV3 | BLE_HCI_SYNCH_PKT_NO_2EV3 | BLE_HCI_SYNCH_PKT_NO_3EV3 | \
     BLE_HCI_SYNCH_PKT_NO_2EV5 | BLE_HCI_SYNCH_PKT_NO_3EV5)
#define BLE_BR_HFP_SCO_D1_PKT_TYPE                                                   \
    (BLE_HCI_SYNCH_PKT_HV3 | BLE_HCI_SYNCH_PKT_NO_2EV3 | BLE_HCI_SYNCH_PKT_NO_3EV3 | \
     BLE_HCI_SYNCH_PKT_NO_2EV5 | BLE_HCI_SYNCH_PKT_NO_3EV5)

/* How long the audio gateway takes to answer a command at most */
#define BLE_BR_HFP_HF_CMD_TIMEOUT 10000

#define BLE_BR_HFP_HF_LINE_MAX_LEN   255
#define BLE_BR_HFP_HF_CMD_MAX_LEN    (BLE_BR_HFP_HF_NUMBER_MAX_LEN + 8)
#define BLE_BR_HFP_HF_CMD_QUEUE_LEN  4
#define BLE_BR_HFP_HF_MAX_INDICATORS 20

#define BLE_BR_HFP_HF_STATE_IDLE        0
#define BLE_BR_HFP_HF_STATE_SECURING    1
#define BLE_BR_HFP_HF_STATE_DISCOVERING 2
#define BLE_BR_HFP_HF_STATE_CONNECTING  3
#define BLE_BR_HFP_HF_STATE_SLC         4
#define BLE_BR_HFP_HF_STATE_CONNECTED   5

/* Commands of the service level connection setup, in order */
#define BLE_BR_HFP_HF_SLC_BRSF      0
#define BLE_BR_HFP_HF_SLC_CIND_TEST 1
#define BLE_BR_HFP_HF_SLC_CIND_READ 2
#define BLE_BR_HFP_HF_SLC_CMER      3
#define BLE_BR_HFP_HF_SLC_DONE      4

static const uint8_t ble_br_hfp_hf_sdp_classes[] = {
    BLE_BR_SDP_DE_SEQ8,   6,
    BLE_BR_SDP_DE_UUID16, BLE_BR_SDP_BE16(BLE_BR_SDP_UUID_HANDSFREE),
    BLE_BR_SDP_DE_UUID16, BLE_BR_SDP_BE16(BLE_BR_SDP_UUID_GENERIC_AUDIO),
};

static const uint8_t ble_br_hfp_hf_sdp_protocols[] = {
    BLE_BR_SDP_DE_SEQ8,   12,
    BLE_BR_SDP_DE_SEQ8,   3,
    BLE_BR_SDP_DE_UUID16, BLE_BR_SDP_BE16(BLE_BR_SDP_UUID_L2CAP),
    BLE_BR_SDP_DE_SEQ8,   5,
    BLE_BR_SDP_DE_UUID16, BLE_BR_SDP_BE16(BLE_BR_SDP_UUID_RFCOMM),
    BLE_BR_SDP_DE_UINT8,  BLE_BR_HFP_HF_CHANNEL,
};

static const uint8_t ble_br_hfp_hf_sdp_browse[] = {
    BLE_BR_SDP_DE_SEQ8,
    3,
    BLE_BR_SDP_DE_UUID16,
    BLE_BR_SDP_BE16(BLE_BR_SDP_UUID_PUBLIC_BROWSE_ROOT),
};

static const uint8_t ble_br_hfp_hf_sdp_profiles[] = {
    BLE_BR_SDP_DE_SEQ8,   8,
    BLE_BR_SDP_DE_SEQ8,   6,
    BLE_BR_SDP_DE_UUID16, BLE_BR_SDP_BE16(BLE_BR_SDP_UUID_HANDSFREE),
    BLE_BR_SDP_DE_UINT16, BLE_BR_SDP_BE16(BLE_BR_HFP_VERSION),
};

static const uint8_t ble_br_hfp_hf_sdp_name[] = {
    BLE_BR_SDP_DE_TEXT8, 10, 'H', 'a', 'n', 'd', 's', '-', 'F', 'r', 'e', 'e',
};

static const uint8_t ble_br_hfp_hf_sdp_features[] = {
    BLE_BR_SDP_DE_UINT16,
    BLE_BR_SDP_BE16(BLE_BR_HFP_HF_SDP_FEATURES),
};

static const struct ble_br_sdp_attr ble_br_hfp_hf_sdp_attrs[] = {
    { BLE_BR_SDP_ATTR_SERVICE_CLASS_LIST, ble_br_hfp_hf_sdp_classes,
     sizeof(ble_br_hfp_hf_sdp_classes)   },
    { BLE_BR_SDP_ATTR_PROTOCOL_DESC_LIST, ble_br_hfp_hf_sdp_protocols,
     sizeof(ble_br_hfp_hf_sdp_protocols) },
    { BLE_BR_SDP_ATTR_BROWSE_GROUP_LIST,  ble_br_hfp_hf_sdp_browse,
     sizeof(ble_br_hfp_hf_sdp_browse)    },
    { BLE_BR_SDP_ATTR_PROFILE_DESC_LIST,  ble_br_hfp_hf_sdp_profiles,
     sizeof(ble_br_hfp_hf_sdp_profiles)  },
    { BLE_BR_SDP_ATTR_SERVICE_NAME,       ble_br_hfp_hf_sdp_name,
     sizeof(ble_br_hfp_hf_sdp_name)      },
    { BLE_BR_SDP_ATTR_SUPPORTED_FEATURES, ble_br_hfp_hf_sdp_features,
     sizeof(ble_br_hfp_hf_sdp_features)  },
};

static const uint16_t ble_br_hfp_hf_sdp_uuids[] = {
    BLE_BR_SDP_UUID_HANDSFREE,
    BLE_BR_SDP_UUID_GENERIC_AUDIO,
    BLE_BR_SDP_UUID_L2CAP,
    BLE_BR_SDP_UUID_RFCOMM,
    BLE_BR_SDP_UUID_PUBLIC_BROWSE_ROOT,
};

static const struct ble_br_sdp_record ble_br_hfp_hf_sdp_record = {
    .attrs = ble_br_hfp_hf_sdp_attrs,
    .num_attrs = sizeof(ble_br_hfp_hf_sdp_attrs) / sizeof(ble_br_hfp_hf_sdp_attrs[0]),
    .uuids = ble_br_hfp_hf_sdp_uuids,
    .num_uuids = sizeof(ble_br_hfp_hf_sdp_uuids) / sizeof(ble_br_hfp_hf_sdp_uuids[0]),
};

static const char *const ble_br_hfp_hf_ind_names[BLE_BR_HFP_HF_IND_COUNT] = {
    [BLE_BR_HFP_HF_IND_SERVICE] = "service",
    [BLE_BR_HFP_HF_IND_CALL] = "call",
    [BLE_BR_HFP_HF_IND_CALLSETUP] = "callsetup",
    [BLE_BR_HFP_HF_IND_CALLHELD] = "callheld",
    [BLE_BR_HFP_HF_IND_SIGNAL] = "signal",
    [BLE_BR_HFP_HF_IND_ROAM] = "roam",
    [BLE_BR_HFP_HF_IND_BATTCHG] = "battchg",
};

static struct {
    ble_br_hfp_hf_event_fn *cb;
    void *cb_arg;

    uint8_t state;
    uint16_t conn_handle;
    struct ble_br_rfcomm_dlc *dlc;
    uint16_t sco_handle;

    uint8_t slc_step;
    uint32_t ag_features;
    /* Indicator of each position the audio gateway reports, plus one */
    uint8_t ind_map[BLE_BR_HFP_HF_MAX_INDICATORS];

    /* One command at a time: the rest waits for its answer */
    bool cmd_pending;
    ble_npl_time_t cmd_exp;
    bool cmd_internal;
    char cmd_queue[BLE_BR_HFP_HF_CMD_QUEUE_LEN][BLE_BR_HFP_HF_CMD_MAX_LEN + 1];
    uint8_t cmd_head;
    uint8_t cmd_count;

    char line[BLE_BR_HFP_HF_LINE_MAX_LEN + 1];
    uint16_t line_len;
} ble_br_hfp_hf = {
    .conn_handle = BLE_HS_CONN_HANDLE_NONE,
    .sco_handle = BLE_HS_CONN_HANDLE_NONE,
};

static const struct ble_br_rfcomm_dlc_ops ble_br_hfp_hf_dlc_ops;

static void
ble_br_hfp_hf_event(struct ble_br_hfp_hf_event *event)
{
    event->conn_handle = ble_br_hfp_hf.conn_handle;
    ble_br_hfp_hf.cb(event, ble_br_hfp_hf.cb_arg);
}

/* Forgets the audio gateway and tells the application */
static void
ble_br_hfp_hf_close(int status)
{
    struct ble_br_hfp_hf_event event;
    uint8_t state = ble_br_hfp_hf.state;
    uint16_t sco_handle = ble_br_hfp_hf.sco_handle;

    if (state == BLE_BR_HFP_HF_STATE_IDLE) {
        return;
    }

    /* The audio goes with the service level connection */
    if (sco_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_br_sco_disconnect(sco_handle);

        memset(&event, 0, sizeof(event));
        event.type = BLE_BR_HFP_HF_EVENT_AUDIO_DISCONNECT;
        event.conn_handle = ble_br_hfp_hf.conn_handle;
        event.audio_disconnect.sco_handle = sco_handle;
        ble_br_hfp_hf.sco_handle = BLE_HS_CONN_HANDLE_NONE;
        ble_br_hfp_hf.cb(&event, ble_br_hfp_hf.cb_arg);
    }

    if (ble_br_hfp_hf.dlc != NULL) {
        ble_br_rfcomm_disconnect(ble_br_hfp_hf.dlc);
        ble_br_hfp_hf.dlc = NULL;
    }

    memset(&event, 0, sizeof(event));
    if (state == BLE_BR_HFP_HF_STATE_CONNECTED) {
        event.type = BLE_BR_HFP_HF_EVENT_DISCONNECT;
    } else {
        event.type = BLE_BR_HFP_HF_EVENT_CONNECT;
        event.connect.status = status;
    }
    event.conn_handle = ble_br_hfp_hf.conn_handle;

    ble_br_hfp_hf.state = BLE_BR_HFP_HF_STATE_IDLE;
    ble_br_hfp_hf.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    ble_br_hfp_hf.cmd_pending = false;
    ble_br_hfp_hf.cmd_count = 0;
    ble_br_hfp_hf.line_len = 0;

    ble_br_hfp_hf.cb(&event, ble_br_hfp_hf.cb_arg);
}

static int
ble_br_hfp_hf_cmd_tx(const char *cmd, bool internal)
{
    int rc;

    BLE_HS_LOG(DEBUG, "hfp tx: %s\n", cmd);

    rc = ble_br_rfcomm_send(ble_br_hfp_hf.dlc, (const uint8_t *)cmd, strlen(cmd));
    if (rc == 0) {
        ble_br_hfp_hf.cmd_pending = true;
        ble_br_hfp_hf.cmd_exp = ble_br_hs_deadline(BLE_BR_HFP_HF_CMD_TIMEOUT);
        ble_br_hfp_hf.cmd_internal = internal;
    }

    return rc;
}

/* Sends a command, once the ones before it are answered */
static int
ble_br_hfp_hf_cmd(const char *cmd)
{
    uint8_t idx;

    if (strlen(cmd) > BLE_BR_HFP_HF_CMD_MAX_LEN) {
        return BLE_HS_EINVAL;
    }

    if (!ble_br_hfp_hf.cmd_pending) {
        return ble_br_hfp_hf_cmd_tx(cmd, false);
    }

    if (ble_br_hfp_hf.cmd_count == BLE_BR_HFP_HF_CMD_QUEUE_LEN) {
        return BLE_HS_EBUSY;
    }

    idx = (ble_br_hfp_hf.cmd_head + ble_br_hfp_hf.cmd_count) % BLE_BR_HFP_HF_CMD_QUEUE_LEN;
    strcpy(ble_br_hfp_hf.cmd_queue[idx], cmd);
    ble_br_hfp_hf.cmd_count++;

    return 0;
}

static void
ble_br_hfp_hf_cmd_next(void)
{
    const char *cmd;

    if (ble_br_hfp_hf.cmd_count == 0) {
        return;
    }

    cmd = ble_br_hfp_hf.cmd_queue[ble_br_hfp_hf.cmd_head];
    ble_br_hfp_hf.cmd_head = (ble_br_hfp_hf.cmd_head + 1) % BLE_BR_HFP_HF_CMD_QUEUE_LEN;
    ble_br_hfp_hf.cmd_count--;

    ble_br_hfp_hf_cmd_tx(cmd, false);
}

static void
ble_br_hfp_hf_slc_next(void)
{
    struct ble_br_hfp_hf_event event;
    char cmd[BLE_BR_HFP_HF_CMD_MAX_LEN + 1];
    uint16_t features;
    int rc;

    switch (ble_br_hfp_hf.slc_step) {
    case BLE_BR_HFP_HF_SLC_BRSF:
        features = BLE_BR_HFP_HF_FEAT_CLI | BLE_BR_HFP_HF_FEAT_VOLUME;
        if (ble_br_gap_local_features() & BLE_HCI_LMP_FEAT_EDR_ESCO_2M) {
            features |= BLE_BR_HFP_HF_FEAT_ESCO_S4;
        }
        snprintf(cmd, sizeof(cmd), "AT+BRSF=%u\r", features);
        rc = ble_br_hfp_hf_cmd_tx(cmd, true);
        break;

    case BLE_BR_HFP_HF_SLC_CIND_TEST:
        rc = ble_br_hfp_hf_cmd_tx("AT+CIND=?\r", true);
        break;

    case BLE_BR_HFP_HF_SLC_CIND_READ:
        rc = ble_br_hfp_hf_cmd_tx("AT+CIND?\r", true);
        break;

    case BLE_BR_HFP_HF_SLC_CMER:
        rc = ble_br_hfp_hf_cmd_tx("AT+CMER=3,0,0,1\r", true);
        break;

    default:
        ble_br_hfp_hf.state = BLE_BR_HFP_HF_STATE_CONNECTED;

        /* Have the number of the caller sent with each ring */
        ble_br_hfp_hf_cmd_tx("AT+CLIP=1\r", true);

        memset(&event, 0, sizeof(event));
        event.type = BLE_BR_HFP_HF_EVENT_CONNECT;
        ble_br_hfp_hf_event(&event);
        return;
    }

    if (rc != 0) {
        ble_br_hfp_hf_close(rc);
    }
}

static void
ble_br_hfp_hf_rx_result(int status)
{
    struct ble_br_hfp_hf_event event;
    bool internal;

    if (!ble_br_hfp_hf.cmd_pending) {
        return;
    }

    ble_br_hfp_hf.cmd_pending = false;
    internal = ble_br_hfp_hf.cmd_internal;

    if (ble_br_hfp_hf.state == BLE_BR_HFP_HF_STATE_SLC) {
        /* Gateways older than the feature exchange do not know AT+BRSF */
        if (status != 0 && ble_br_hfp_hf.slc_step != BLE_BR_HFP_HF_SLC_BRSF) {
            ble_br_hfp_hf_close(status);
            return;
        }
        ble_br_hfp_hf.slc_step++;
        ble_br_hfp_hf_slc_next();
        return;
    }

    if (!internal) {
        memset(&event, 0, sizeof(event));
        event.type = BLE_BR_HFP_HF_EVENT_CMD_COMPLETE;
        event.cmd_complete.status = status;
        ble_br_hfp_hf_event(&event);
    }

    if (ble_br_hfp_hf.state == BLE_BR_HFP_HF_STATE_CONNECTED &&
        !ble_br_hfp_hf.cmd_pending) {
        ble_br_hfp_hf_cmd_next();
    }
}

static void
ble_br_hfp_hf_indicator(uint8_t pos, uint8_t value)
{
    struct ble_br_hfp_hf_event event;

    if (pos == 0 || pos > BLE_BR_HFP_HF_MAX_INDICATORS ||
        ble_br_hfp_hf.ind_map[pos - 1] == 0) {
        return;
    }

    memset(&event, 0, sizeof(event));
    event.type = BLE_BR_HFP_HF_EVENT_INDICATOR;
    event.indicator.id = ble_br_hfp_hf.ind_map[pos - 1] - 1;
    event.indicator.value = value;
    ble_br_hfp_hf_event(&event);
}

/* +CIND: ("call",(0,1)),("callsetup",(0-3)),... */
static void
ble_br_hfp_hf_rx_cind_test(const char *args)
{
    const char *end;
    uint8_t pos;
    int i;

    memset(ble_br_hfp_hf.ind_map, 0, sizeof(ble_br_hfp_hf.ind_map));

    for (pos = 0; pos < BLE_BR_HFP_HF_MAX_INDICATORS; pos++) {
        args = strchr(args, '"');
        if (args == NULL) {
            return;
        }
        args++;
        end = strchr(args, '"');
        if (end == NULL) {
            return;
        }

        for (i = 0; i < BLE_BR_HFP_HF_IND_COUNT; i++) {
            if (strlen(ble_br_hfp_hf_ind_names[i]) == (size_t)(end - args) &&
                strncmp(ble_br_hfp_hf_ind_names[i], args, end - args) == 0) {
                ble_br_hfp_hf.ind_map[pos] = i + 1;
            }
        }

        /* Past the name and the values, which are quoted nowhere */
        args = end + 1;
    }
}

/* +CIND: 1,0,0,... */
static void
ble_br_hfp_hf_rx_cind_read(const char *args)
{
    uint8_t pos;
    char *end;
    long value;

    for (pos = 1; pos <= BLE_BR_HFP_HF_MAX_INDICATORS; pos++) {
        value = strtol(args, &end, 10);
        if (end == args) {
            return;
        }
        ble_br_hfp_hf_indicator(pos, value);

        args = strchr(end, ',');
        if (args == NULL) {
            return;
        }
        args++;
    }
}

static void
ble_br_hfp_hf_rx_clip(const char *args)
{
    struct ble_br_hfp_hf_event event;
    const char *end;
    size_t len;

    memset(&event, 0, sizeof(event));
    event.type = BLE_BR_HFP_HF_EVENT_CLIP;

    args = strchr(args, '"');
    if (args != NULL) {
        args++;
        end = strchr(args, '"');
        if (end != NULL) {
            len = BLE_BR_MIN((size_t)(end - args), BLE_BR_HFP_HF_NUMBER_MAX_LEN);
            memcpy(event.clip.number, args, len);
        }
    }

    ble_br_hfp_hf_event(&event);
}

static void
ble_br_hfp_hf_rx_line(const char *line)
{
    struct ble_br_hfp_hf_event event;
    char *end;
    long value;

    memset(&event, 0, sizeof(event));

    BLE_HS_LOG(DEBUG, "hfp rx: %s\n", line);

    if (strcmp(line, "OK") == 0) {
        ble_br_hfp_hf_rx_result(0);
    } else if (strcmp(line, "ERROR") == 0 || strncmp(line, "+CME ERROR", 10) == 0) {
        ble_br_hfp_hf_rx_result(BLE_HS_EREJECT);
    } else if (strcmp(line, "RING") == 0) {
        event.type = BLE_BR_HFP_HF_EVENT_RING;
        ble_br_hfp_hf_event(&event);
    } else if (strncmp(line, "+BRSF:", 6) == 0) {
        ble_br_hfp_hf.ag_features = strtoul(line + 6, NULL, 10);
    } else if (strncmp(line, "+CIND:", 6) == 0) {
        if (ble_br_hfp_hf.slc_step == BLE_BR_HFP_HF_SLC_CIND_TEST) {
            ble_br_hfp_hf_rx_cind_test(line + 6);
        } else {
            ble_br_hfp_hf_rx_cind_read(line + 6);
        }
    } else if (strncmp(line, "+CIEV:", 6) == 0) {
        value = strtol(line + 6, &end, 10);
        if (end != line + 6 && *end == ',') {
            ble_br_hfp_hf_indicator(value, strtol(end + 1, NULL, 10));
        }
    } else if (strncmp(line, "+CLIP:", 6) == 0) {
        ble_br_hfp_hf_rx_clip(line + 6);
    } else if (strncmp(line, "+VGS", 4) == 0 && (line[4] == ':' || line[4] == '=')) {
        value = strtol(line + 5, NULL, 10);
        if (value >= 0 && value <= BLE_BR_HFP_HF_VOLUME_MAX) {
            event.type = BLE_BR_HFP_HF_EVENT_VOLUME;
            event.volume.gain = value;
            ble_br_hfp_hf_event(&event);
        }
    }
}

static void
ble_br_hfp_hf_dlc_connected(struct ble_br_rfcomm_dlc *dlc, int status, void *arg)
{
    if (status != 0) {
        ble_br_hfp_hf.dlc = NULL;
        ble_br_hfp_hf_close(status);
        return;
    }

    ble_br_hfp_hf.dlc = dlc;
    ble_br_hfp_hf.state = BLE_BR_HFP_HF_STATE_SLC;
    ble_br_hfp_hf.slc_step = BLE_BR_HFP_HF_SLC_BRSF;
    ble_br_hfp_hf.ag_features = 0;
    ble_br_hfp_hf.cmd_pending = false;
    ble_br_hfp_hf.cmd_count = 0;
    ble_br_hfp_hf.line_len = 0;

    ble_br_hfp_hf_slc_next();
}

static void
ble_br_hfp_hf_dlc_disconnected(struct ble_br_rfcomm_dlc *dlc, void *arg)
{
    ble_br_hfp_hf.dlc = NULL;
    ble_br_hfp_hf_close(BLE_HS_ENOTCONN);
}

static void
ble_br_hfp_hf_dlc_recv(struct ble_br_rfcomm_dlc *dlc, const uint8_t *data,
                       uint16_t len, void *arg)
{
    uint16_t i;
    char c;

    for (i = 0; i < len; i++) {
        c = data[i];

        if (c != '\r' && c != '\n') {
            if (ble_br_hfp_hf.line_len < BLE_BR_HFP_HF_LINE_MAX_LEN) {
                ble_br_hfp_hf.line[ble_br_hfp_hf.line_len++] = c;
            }
            continue;
        }

        if (ble_br_hfp_hf.line_len == 0) {
            continue;
        }

        ble_br_hfp_hf.line[ble_br_hfp_hf.line_len] = '\0';
        ble_br_hfp_hf.line_len = 0;
        ble_br_hfp_hf_rx_line(ble_br_hfp_hf.line);

        /* A line may have ended the connection */
        if (ble_br_hfp_hf.dlc != dlc) {
            return;
        }
    }
}

static const struct ble_br_rfcomm_dlc_ops ble_br_hfp_hf_dlc_ops = {
    .connected = ble_br_hfp_hf_dlc_connected,
    .disconnected = ble_br_hfp_hf_dlc_disconnected,
    .recv = ble_br_hfp_hf_dlc_recv,
};

static int
ble_br_hfp_hf_rfcomm_accept(uint16_t conn_handle,
                            const struct ble_br_rfcomm_dlc_ops **ops, void **arg)
{
    if (ble_br_hfp_hf.state != BLE_BR_HFP_HF_STATE_IDLE) {
        return BLE_HS_EBUSY;
    }

    ble_br_hfp_hf.state = BLE_BR_HFP_HF_STATE_CONNECTING;
    ble_br_hfp_hf.conn_handle = conn_handle;
    *ops = &ble_br_hfp_hf_dlc_ops;
    *arg = NULL;

    return 0;
}

static struct ble_br_rfcomm_server ble_br_hfp_hf_rfcomm_server = {
    .channel = BLE_BR_HFP_HF_CHANNEL,
    .accept = ble_br_hfp_hf_rfcomm_accept,
};

static void
ble_br_hfp_hf_sdp_done(uint16_t conn_handle, int status, uint8_t channel, void *arg)
{
    if (ble_br_hfp_hf.state != BLE_BR_HFP_HF_STATE_DISCOVERING ||
        ble_br_hfp_hf.conn_handle != conn_handle) {
        return;
    }

    if (status == 0 && channel == 0) {
        status = BLE_HS_ENOENT;
    }
    if (status == 0) {
        ble_br_hfp_hf.state = BLE_BR_HFP_HF_STATE_CONNECTING;
        status = ble_br_rfcomm_connect(conn_handle, channel,
                                       &ble_br_hfp_hf_dlc_ops, NULL, NULL);
    }
    if (status != 0) {
        ble_br_hfp_hf_close(status);
    }
}

static int
ble_br_hfp_hf_discover(void)
{
    ble_br_hfp_hf.state = BLE_BR_HFP_HF_STATE_DISCOVERING;

    return ble_br_sdp_find_rfcomm(ble_br_hfp_hf.conn_handle,
                                  BLE_BR_SDP_UUID_HANDSFREE_AG,
                                  ble_br_hfp_hf_sdp_done, NULL);
}

void
ble_br_hfp_hf_enc_change(uint16_t conn_handle, int status)
{
    if (ble_br_hfp_hf.state != BLE_BR_HFP_HF_STATE_SECURING ||
        ble_br_hfp_hf.conn_handle != conn_handle) {
        return;
    }

    if (status == 0) {
        status = ble_br_hfp_hf_discover();
    }
    if (status != 0) {
        ble_br_hfp_hf_close(status);
    }
}

/* An audio gateway that stopped answering is let go */
void
ble_br_hfp_hf_timer(ble_npl_time_t now, int32_t *ticks)
{
    if (ble_br_hfp_hf.state != BLE_BR_HFP_HF_STATE_IDLE && ble_br_hfp_hf.cmd_pending &&
        ble_br_hs_expired(ble_br_hfp_hf.cmd_exp, now, ticks)) {
        ble_br_hfp_hf_close(BLE_HS_ETIMEOUT);
    }
}

void
ble_br_hfp_hf_conn_broken(uint16_t conn_handle)
{
    if (ble_br_hfp_hf.conn_handle == conn_handle) {
        ble_br_hfp_hf.dlc = NULL;
        ble_br_hfp_hf_close(BLE_HS_ENOTCONN);
    }
}

static void
ble_br_hfp_hf_sco_params(uint8_t link_type, struct ble_br_sco_params *params)
{
    bool edr;

    edr = (ble_br_gap_local_features() & BLE_HCI_LMP_FEAT_EDR_ESCO_2M) != 0;

    if (link_type != BLE_HCI_LINK_TYPE_ESCO) {
        params->max_latency = BLE_HCI_SYNCH_LATENCY_DONT_CARE;
        params->retrans_effort = BLE_HCI_SYNCH_RETRANS_NONE;
        params->pkt_type = BLE_BR_HFP_SCO_D1_PKT_TYPE;
    } else if (edr && (ble_br_hfp_hf.ag_features & BLE_BR_HFP_AG_FEAT_ESCO_S4)) {
        params->max_latency = BLE_BR_HFP_ESCO_S4_LATENCY;
        params->retrans_effort = BLE_HCI_SYNCH_RETRANS_QUALITY;
        params->pkt_type = BLE_BR_HFP_ESCO_S4_PKT_TYPE;
    } else if (edr) {
        /* Up to S3, which the settings below it fit in as well */
        params->max_latency = BLE_BR_HFP_ESCO_S3_LATENCY;
        params->retrans_effort = BLE_HCI_SYNCH_RETRANS_POWER;
        params->pkt_type = BLE_BR_HFP_ESCO_S3_PKT_TYPE;
    } else {
        params->max_latency = BLE_BR_HFP_ESCO_S1_LATENCY;
        params->retrans_effort = BLE_HCI_SYNCH_RETRANS_POWER;
        params->pkt_type = BLE_BR_HFP_ESCO_S1_PKT_TYPE;
    }
}

static int
ble_br_hfp_hf_sco_accept(const uint8_t *addr, uint8_t link_type,
                         struct ble_br_sco_params *params)
{
    struct ble_br_conn *conn;

    if (ble_br_hfp_hf.state != BLE_BR_HFP_HF_STATE_CONNECTED) {
        return BLE_HS_EREJECT;
    }

    conn = ble_br_conn_find(ble_br_hfp_hf.conn_handle);
    if (conn == NULL || memcmp(conn->addr, addr, BLE_DEV_ADDR_LEN) != 0) {
        return BLE_HS_EREJECT;
    }

    ble_br_hfp_hf_sco_params(link_type, params);

    return 0;
}

static void
ble_br_hfp_hf_sco_connected(const uint8_t *addr, int status, uint16_t sco_handle)
{
    struct ble_br_hfp_hf_event event;

    memset(&event, 0, sizeof(event));
    event.type = BLE_BR_HFP_HF_EVENT_AUDIO_CONNECT;
    event.audio_connect.status = status;
    event.audio_connect.sco_handle = sco_handle;

    if (status == 0) {
        ble_br_hfp_hf.sco_handle = sco_handle;
    }

    ble_br_hfp_hf_event(&event);
}

static void
ble_br_hfp_hf_sco_disconnected(uint16_t sco_handle, int reason)
{
    struct ble_br_hfp_hf_event event;

    /* Already told, when the service level connection ended first */
    if (ble_br_hfp_hf.sco_handle != sco_handle) {
        return;
    }

    memset(&event, 0, sizeof(event));
    event.type = BLE_BR_HFP_HF_EVENT_AUDIO_DISCONNECT;
    event.audio_disconnect.sco_handle = sco_handle;

    ble_br_hfp_hf.sco_handle = BLE_HS_CONN_HANDLE_NONE;

    ble_br_hfp_hf_event(&event);
}

static const struct ble_br_sco_ops ble_br_hfp_hf_sco_ops = {
    .accept = ble_br_hfp_hf_sco_accept,
    .connected = ble_br_hfp_hf_sco_connected,
    .disconnected = ble_br_hfp_hf_sco_disconnected,
};

static int
ble_br_hfp_hf_connect_call(void *arg)
{
    struct ble_br_conn *conn;
    int rc;

    if (ble_br_hfp_hf.state != BLE_BR_HFP_HF_STATE_IDLE) {
        return BLE_HS_EALREADY;
    }

    conn = ble_br_conn_find(*(uint16_t *)arg);
    if (conn == NULL) {
        return BLE_HS_ENOTCONN;
    }

    ble_br_hfp_hf.conn_handle = conn->handle;

    if (conn->flags & BLE_BR_CONN_F_ENCRYPTED) {
        rc = ble_br_hfp_hf_discover();
    } else {
        ble_br_hfp_hf.state = BLE_BR_HFP_HF_STATE_SECURING;
        rc = ble_br_gap_security_initiate_conn(conn);
    }

    if (rc != 0) {
        ble_br_hfp_hf.state = BLE_BR_HFP_HF_STATE_IDLE;
        ble_br_hfp_hf.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }

    return rc;
}

int
ble_br_hfp_hf_connect(uint16_t conn_handle)
{
    return ble_br_hs_call(ble_br_hfp_hf_connect_call, &conn_handle);
}

static int
ble_br_hfp_hf_disconnect_call(void *arg)
{
    if (ble_br_hfp_hf.state == BLE_BR_HFP_HF_STATE_IDLE ||
        ble_br_hfp_hf.conn_handle != *(uint16_t *)arg) {
        return BLE_HS_ENOTCONN;
    }

    ble_br_hfp_hf_close(BLE_HS_EDONE);

    return 0;
}

int
ble_br_hfp_hf_disconnect(uint16_t conn_handle)
{
    return ble_br_hs_call(ble_br_hfp_hf_disconnect_call, &conn_handle);
}

struct ble_br_hfp_hf_cmd_arg {
    uint16_t conn_handle;
    const char *cmd;
};

static int
ble_br_hfp_hf_cmd_call(void *arg)
{
    struct ble_br_hfp_hf_cmd_arg *args = arg;

    if (ble_br_hfp_hf.state != BLE_BR_HFP_HF_STATE_CONNECTED ||
        ble_br_hfp_hf.conn_handle != args->conn_handle) {
        return BLE_HS_ENOTCONN;
    }

    return ble_br_hfp_hf_cmd(args->cmd);
}

static int
ble_br_hfp_hf_cmd_api(uint16_t conn_handle, const char *cmd)
{
    struct ble_br_hfp_hf_cmd_arg args = {
        .conn_handle = conn_handle,
        .cmd = cmd,
    };

    return ble_br_hs_call(ble_br_hfp_hf_cmd_call, &args);
}

int
ble_br_hfp_hf_answer(uint16_t conn_handle)
{
    return ble_br_hfp_hf_cmd_api(conn_handle, "ATA\r");
}

int
ble_br_hfp_hf_hangup(uint16_t conn_handle)
{
    return ble_br_hfp_hf_cmd_api(conn_handle, "AT+CHUP\r");
}

int
ble_br_hfp_hf_dial(uint16_t conn_handle, const char *number)
{
    char cmd[BLE_BR_HFP_HF_CMD_MAX_LEN + 1];
    size_t len;
    size_t i;

    len = strlen(number);
    if (len == 0 || len > BLE_BR_HFP_HF_NUMBER_MAX_LEN) {
        return BLE_HS_EINVAL;
    }

    for (i = 0; i < len; i++) {
        if (strchr("0123456789*#+ABCD", number[i]) == NULL) {
            return BLE_HS_EINVAL;
        }
    }

    snprintf(cmd, sizeof(cmd), "ATD%s;\r", number);

    return ble_br_hfp_hf_cmd_api(conn_handle, cmd);
}

int
ble_br_hfp_hf_set_volume(uint16_t conn_handle, uint8_t gain)
{
    char cmd[BLE_BR_HFP_HF_CMD_MAX_LEN + 1];

    if (gain > BLE_BR_HFP_HF_VOLUME_MAX) {
        return BLE_HS_EINVAL;
    }

    snprintf(cmd, sizeof(cmd), "AT+VGS=%u\r", gain);

    return ble_br_hfp_hf_cmd_api(conn_handle, cmd);
}

static int
ble_br_hfp_hf_audio_connect_call(void *arg)
{
    struct ble_br_sco_params params;

    if (ble_br_hfp_hf.state != BLE_BR_HFP_HF_STATE_CONNECTED ||
        ble_br_hfp_hf.conn_handle != *(uint16_t *)arg) {
        return BLE_HS_ENOTCONN;
    }

    ble_br_hfp_hf_sco_params(BLE_HCI_LINK_TYPE_ESCO, &params);

    return ble_br_sco_connect(ble_br_hfp_hf.conn_handle, &params);
}

int
ble_br_hfp_hf_audio_connect(uint16_t conn_handle)
{
    return ble_br_hs_call(ble_br_hfp_hf_audio_connect_call, &conn_handle);
}

static int
ble_br_hfp_hf_audio_disconnect_call(void *arg)
{
    if (ble_br_hfp_hf.conn_handle != *(uint16_t *)arg) {
        return BLE_HS_ENOTCONN;
    }

    return ble_br_sco_disconnect(ble_br_hfp_hf.sco_handle);
}

int
ble_br_hfp_hf_audio_disconnect(uint16_t conn_handle)
{
    return ble_br_hs_call(ble_br_hfp_hf_audio_disconnect_call, &conn_handle);
}

void
ble_br_hfp_hf_reset(void)
{
    memset(&ble_br_hfp_hf, 0, sizeof(ble_br_hfp_hf));
    ble_br_hfp_hf.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    ble_br_hfp_hf.sco_handle = BLE_HS_CONN_HANDLE_NONE;
}

int
ble_br_hfp_hf_init(ble_br_hfp_hf_event_fn *cb, void *cb_arg)
{
    int rc;

    if (cb == NULL) {
        return BLE_HS_EINVAL;
    }

    ble_br_hfp_hf_reset();
    ble_br_hfp_hf.cb = cb;
    ble_br_hfp_hf.cb_arg = cb_arg;

    rc = ble_br_sdp_register(&ble_br_hfp_hf_sdp_record);
    if (rc != 0) {
        return rc;
    }

    rc = ble_br_rfcomm_server_register(&ble_br_hfp_hf_rfcomm_server);
    if (rc != 0) {
        return rc;
    }

    ble_br_sco_set_ops(&ble_br_hfp_hf_sco_ops);

    return ble_br_gap_eir_add_uuid16(BLE_BR_SDP_UUID_HANDSFREE);
}

#endif
