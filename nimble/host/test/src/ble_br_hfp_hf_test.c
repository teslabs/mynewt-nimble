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
#include "host/ble_br_hfp_hf.h"
#include "ble_hs_test.h"
#include "ble_hs_test_util.h"
#include "ble_br_test_util.h"
#include "ble_br_sdp_priv.h"

#if MYNEWT_VAL(BLE_BREDR_HFP_HF)

#define BLE_BR_HFP_HF_TEST_EVENT_MAX 64

/* The link to the server of the host, when the peer starts the session. */
#define BLE_BR_HFP_HF_TEST_DLCI                                               \
    (MYNEWT_VAL(BLE_BREDR_HFP_HF_RFCOMM_CHANNEL) << 1)

/* Where the service of the peer is, and the link to it. */
#define BLE_BR_HFP_HF_TEST_AG_CHANNEL 3
#define BLE_BR_HFP_HF_TEST_AG_DLCI    (BLE_BR_HFP_HF_TEST_AG_CHANNEL << 1)

/* Features of the audio gateway: three-way calling and eSCO S4 settings,
 * or the former only.
 */
#define BLE_BR_HFP_HF_TEST_AG_FEAT_S4    "2049"
#define BLE_BR_HFP_HF_TEST_AG_FEAT_NO_S4 "1"

/* How long the audio gateway takes to answer a command at most. */
#define BLE_BR_HFP_HF_TEST_CMD_TIMEOUT_MS 10000

#define BLE_BR_HFP_HF_TEST_SCO_HANDLE 0x0101

static const uint8_t ble_br_hfp_hf_test_addr[BLE_DEV_ADDR_LEN] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
};

/* The indicators of the audio gateway, which take more than a frame. */
static const char ble_br_hfp_hf_test_cind1[] =
    "\r\n+CIND: (\"service\",(0,1)),(\"call\",(0,1)),(\"callsetup\",(0-3)),"
    "(\"callheld\",(0-2)),(\"sig";
static const char ble_br_hfp_hf_test_cind2[] =
    "nal\",(0-5)),(\"roam\",(0,1)),(\"battchg\",(0-5))\r\n";

static struct ble_br_hfp_hf_event ble_br_hfp_hf_test_events[BLE_BR_HFP_HF_TEST_EVENT_MAX];
static int ble_br_hfp_hf_test_event_head;
static int ble_br_hfp_hf_test_event_tail;
static int ble_br_hfp_hf_test_cb_arg;

static struct ble_br_test_util_chan ble_br_hfp_hf_test_chan;
static uint8_t ble_br_hfp_hf_test_dlci;
/* C/R bit of the frames the host sends on the link. */
static uint8_t ble_br_hfp_hf_test_cr;
static int ble_br_hfp_hf_test_rx_cnt;
static uint16_t ble_br_hfp_hf_test_sdp_tid;

static void
ble_br_hfp_hf_test_cb(const struct ble_br_hfp_hf_event *event, void *arg)
{
    TEST_ASSERT(arg == &ble_br_hfp_hf_test_cb_arg);
    TEST_ASSERT_FATAL(ble_br_hfp_hf_test_event_tail < BLE_BR_HFP_HF_TEST_EVENT_MAX);

    ble_br_hfp_hf_test_events[ble_br_hfp_hf_test_event_tail++] = *event;
}

/**
 * Retrieves the oldest event the application has not looked at yet; all of
 * them are expected to be for connection 2.
 */
static const struct ble_br_hfp_hf_event *
ble_br_hfp_hf_test_util_event_next(void)
{
    const struct ble_br_hfp_hf_event *event;

    TEST_ASSERT_FATAL(ble_br_hfp_hf_test_event_head < ble_br_hfp_hf_test_event_tail);

    event = &ble_br_hfp_hf_test_events[ble_br_hfp_hf_test_event_head++];
    TEST_ASSERT(event->conn_handle == 2);

    return event;
}

static void
ble_br_hfp_hf_test_util_verify_no_event(void)
{
    TEST_ASSERT(ble_br_hfp_hf_test_event_head == ble_br_hfp_hf_test_event_tail);
}

static void
ble_br_hfp_hf_test_util_verify_event(uint8_t type)
{
    const struct ble_br_hfp_hf_event *event;

    event = ble_br_hfp_hf_test_util_event_next();
    TEST_ASSERT(event->type == type);
}

static void
ble_br_hfp_hf_test_util_verify_connect(int status)
{
    const struct ble_br_hfp_hf_event *event;

    event = ble_br_hfp_hf_test_util_event_next();
    TEST_ASSERT(event->type == BLE_BR_HFP_HF_EVENT_CONNECT);
    TEST_ASSERT(event->connect.status == status);
}

static void
ble_br_hfp_hf_test_util_verify_indicator(uint8_t id, uint8_t value)
{
    const struct ble_br_hfp_hf_event *event;

    event = ble_br_hfp_hf_test_util_event_next();
    TEST_ASSERT(event->type == BLE_BR_HFP_HF_EVENT_INDICATOR);
    TEST_ASSERT(event->indicator.id == id);
    TEST_ASSERT(event->indicator.value == value);
}

static void
ble_br_hfp_hf_test_util_verify_cmd_complete(int status)
{
    const struct ble_br_hfp_hf_event *event;

    event = ble_br_hfp_hf_test_util_event_next();
    TEST_ASSERT(event->type == BLE_BR_HFP_HF_EVENT_CMD_COMPLETE);
    TEST_ASSERT(event->cmd_complete.status == status);
}

static void
ble_br_hfp_hf_test_util_init(int encrypted)
{
    int rc;

    ble_br_test_util_init();

    ble_br_hfp_hf_test_event_head = 0;
    ble_br_hfp_hf_test_event_tail = 0;
    ble_br_hfp_hf_test_rx_cnt = 0;
    ble_br_hfp_hf_test_sdp_tid = 0;

    /* The service is added to the extended inquiry response. */
    ble_hs_test_util_hci_ack_set(
        ble_hs_hci_util_opcode_join(BLE_HCI_OGF_CTLR_BASEBAND, BLE_HCI_OCF_CB_WR_EIR),
        0);
    rc = ble_br_hfp_hf_init(ble_br_hfp_hf_test_cb, &ble_br_hfp_hf_test_cb_arg);
    TEST_ASSERT_FATAL(rc == 0);
    ble_hs_test_util_hci_out_clear();

    ble_br_test_util_create_conn(2, ble_br_hfp_hf_test_addr, encrypted);
}

/**
 * Receives data from the audio gateway.  The host gives the credits that
 * got used back when two are left.
 */
static void
ble_br_hfp_hf_test_util_rx(const char *str)
{
    uint16_t len;

    ble_br_test_util_rfcomm_rx(
        &ble_br_hfp_hf_test_chan,
        BLE_BR_TEST_UTIL_RFCOMM_ADDR(ble_br_hfp_hf_test_dlci, !ble_br_hfp_hf_test_cr),
        BLE_BR_TEST_UTIL_RFCOMM_UIH, -1, str, strlen(str));

    ble_br_hfp_hf_test_rx_cnt++;
    if (ble_br_hfp_hf_test_rx_cnt % (BLE_BR_TEST_UTIL_RFCOMM_CREDITS -
                                     BLE_BR_TEST_UTIL_RFCOMM_CREDITS_LOW) ==
        0) {
        ble_br_test_util_rfcomm_verify_tx(
            &ble_br_hfp_hf_test_chan,
            BLE_BR_TEST_UTIL_RFCOMM_ADDR(ble_br_hfp_hf_test_dlci, ble_br_hfp_hf_test_cr),
            BLE_BR_TEST_UTIL_RFCOMM_UIH | BLE_BR_TEST_UTIL_RFCOMM_PF,
            BLE_BR_TEST_UTIL_RFCOMM_CREDITS - BLE_BR_TEST_UTIL_RFCOMM_CREDITS_LOW,
            &len);
        TEST_ASSERT(len == 0);
    }
}

/**
 * Verifies the host sent a command to the audio gateway.
 */
static void
ble_br_hfp_hf_test_util_verify_tx(const char *cmd)
{
    const uint8_t *info;
    uint16_t len;

    info = ble_br_test_util_rfcomm_verify_tx(
        &ble_br_hfp_hf_test_chan,
        BLE_BR_TEST_UTIL_RFCOMM_ADDR(ble_br_hfp_hf_test_dlci, ble_br_hfp_hf_test_cr),
        BLE_BR_TEST_UTIL_RFCOMM_UIH, -1, &len);
    TEST_ASSERT_FATAL(len == strlen(cmd));
    TEST_ASSERT(memcmp(info, cmd, len) == 0);
}

/**
 * Has the audio gateway give the host credits to send its commands with.
 */
static void
ble_br_hfp_hf_test_util_rx_credits(uint8_t credits)
{
    ble_br_test_util_rfcomm_rx(
        &ble_br_hfp_hf_test_chan,
        BLE_BR_TEST_UTIL_RFCOMM_ADDR(ble_br_hfp_hf_test_dlci, !ble_br_hfp_hf_test_cr),
        BLE_BR_TEST_UTIL_RFCOMM_UIH | BLE_BR_TEST_UTIL_RFCOMM_PF, credits, NULL, 0);
}

/**
 * Has the audio gateway open a link to the service of the host, which
 * starts to set the service level connection up.
 */
static void
ble_br_hfp_hf_test_util_rx_open(void)
{
    ble_br_hfp_hf_test_dlci = BLE_BR_HFP_HF_TEST_DLCI;
    ble_br_hfp_hf_test_cr = 0;
    ble_br_hfp_hf_test_rx_cnt = 0;

    ble_br_test_util_rfcomm_rx_session(&ble_br_hfp_hf_test_chan, 2);
    ble_br_test_util_rfcomm_rx_open(&ble_br_hfp_hf_test_chan,
                                    ble_br_hfp_hf_test_dlci, 7);
    ble_br_hfp_hf_test_util_rx_credits(100);
}

/**
 * Runs the exchange that sets the service level connection up.
 *
 * @param ag_features           The features of the audio gateway.
 */
static void
ble_br_hfp_hf_test_util_slc(const char *ag_features)
{
    char line[32];

    /* Features: CLI presentation, remote volume control and eSCO S4
     * settings.
     */
    ble_br_hfp_hf_test_util_verify_tx("AT+BRSF=532\r");
    snprintf(line, sizeof line, "\r\n+BRSF: %s\r\n", ag_features);
    ble_br_hfp_hf_test_util_rx(line);
    ble_br_hfp_hf_test_util_rx("\r\nOK\r\n");

    /* The indicators of the audio gateway. */
    ble_br_hfp_hf_test_util_verify_tx("AT+CIND=?\r");
    ble_br_hfp_hf_test_util_rx(ble_br_hfp_hf_test_cind1);
    ble_br_hfp_hf_test_util_rx(ble_br_hfp_hf_test_cind2);
    ble_br_hfp_hf_test_util_rx("\r\nOK\r\n");

    /* Their values. */
    ble_br_hfp_hf_test_util_verify_tx("AT+CIND?\r");
    ble_br_hfp_hf_test_util_rx("\r\n+CIND: 1,0,0,0,4,0,5\r\n");
    ble_br_hfp_hf_test_util_rx("\r\nOK\r\n");

    ble_br_hfp_hf_test_util_verify_indicator(BLE_BR_HFP_HF_IND_SERVICE, 1);
    ble_br_hfp_hf_test_util_verify_indicator(BLE_BR_HFP_HF_IND_CALL, 0);
    ble_br_hfp_hf_test_util_verify_indicator(BLE_BR_HFP_HF_IND_CALLSETUP, 0);
    ble_br_hfp_hf_test_util_verify_indicator(BLE_BR_HFP_HF_IND_CALLHELD, 0);
    ble_br_hfp_hf_test_util_verify_indicator(BLE_BR_HFP_HF_IND_SIGNAL, 4);
    ble_br_hfp_hf_test_util_verify_indicator(BLE_BR_HFP_HF_IND_ROAM, 0);
    ble_br_hfp_hf_test_util_verify_indicator(BLE_BR_HFP_HF_IND_BATTCHG, 5);

    /* Have the changes of the indicators reported. */
    ble_br_hfp_hf_test_util_verify_tx("AT+CMER=3,0,0,1\r");
    ble_br_hfp_hf_test_util_verify_no_event();
    ble_br_hfp_hf_test_util_rx("\r\nOK\r\n");

    /* The connection is up; the number of the caller is asked for. */
    ble_br_hfp_hf_test_util_verify_connect(0);
    ble_br_hfp_hf_test_util_verify_tx("AT+CLIP=1\r");
    ble_br_hfp_hf_test_util_rx("\r\nOK\r\n");

    ble_br_hfp_hf_test_util_verify_no_event();
    ble_br_test_util_verify_tx_none();
}

/**
 * Has the audio gateway connect to the host.
 */
static void
ble_br_hfp_hf_test_util_connect(const char *ag_features)
{
    ble_br_hfp_hf_test_util_rx_open();
    ble_br_hfp_hf_test_util_slc(ag_features);
}

/**
 * Verifies the host closed the link to the audio gateway, and the session
 * with it.
 */
static void
ble_br_hfp_hf_test_util_verify_tx_close(void)
{
    uint16_t len;

    ble_br_test_util_rfcomm_verify_tx(
        &ble_br_hfp_hf_test_chan,
        BLE_BR_TEST_UTIL_RFCOMM_ADDR(ble_br_hfp_hf_test_dlci, ble_br_hfp_hf_test_cr),
        BLE_BR_TEST_UTIL_RFCOMM_DISC | BLE_BR_TEST_UTIL_RFCOMM_PF, -1, &len);
    TEST_ASSERT(len == 0);

    ble_br_test_util_l2cap_verify_tx_disconnect(&ble_br_hfp_hf_test_chan);
    ble_br_test_util_verify_tx_none();
}

TEST_CASE_SELF(ble_br_hfp_hf_test_slc)
{
    int rc;

    ble_br_hfp_hf_test_util_init(1);

    /* No connection yet. */
    rc = ble_br_hfp_hf_answer(2);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);
    rc = ble_br_hfp_hf_disconnect(2);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);

    ble_br_hfp_hf_test_util_rx_open();

    /* The commands of the application wait for the connection to be up. */
    rc = ble_br_hfp_hf_answer(2);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);

    ble_br_hfp_hf_test_util_slc(BLE_BR_HFP_HF_TEST_AG_FEAT_S4);

    /* One audio gateway at a time. */
    rc = ble_br_hfp_hf_connect(2);
    TEST_ASSERT(rc == BLE_HS_EALREADY);

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_hfp_hf_test_slc_fail)
{
    ble_br_hfp_hf_test_util_init(1);

    /*** An audio gateway that does not know the feature exchange. */
    ble_br_hfp_hf_test_util_rx_open();

    ble_br_hfp_hf_test_util_verify_tx("AT+BRSF=532\r");
    ble_br_hfp_hf_test_util_rx("\r\nERROR\r\n");

    ble_br_hfp_hf_test_util_verify_tx("AT+CIND=?\r");
    ble_br_hfp_hf_test_util_verify_no_event();

    /*** An audio gateway that refuses a command that is required. */
    ble_br_hfp_hf_test_util_rx("\r\nERROR\r\n");

    ble_br_hfp_hf_test_util_verify_connect(BLE_HS_EREJECT);
    ble_br_hfp_hf_test_util_verify_tx_close();

    /*** The audio gateway closes the link before the connection is up. */
    ble_br_hfp_hf_test_util_rx_open();
    ble_br_hfp_hf_test_util_verify_tx("AT+BRSF=532\r");

    ble_br_test_util_rfcomm_rx(
        &ble_br_hfp_hf_test_chan,
        BLE_BR_TEST_UTIL_RFCOMM_ADDR(ble_br_hfp_hf_test_dlci, 1),
        BLE_BR_TEST_UTIL_RFCOMM_DISC | BLE_BR_TEST_UTIL_RFCOMM_PF, -1, NULL, 0);
    ble_br_test_util_tx_queue_clear();

    ble_br_hfp_hf_test_util_verify_connect(BLE_HS_ENOTCONN);
    ble_br_hfp_hf_test_util_verify_no_event();

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_hfp_hf_test_indicators)
{
    ble_br_hfp_hf_test_util_init(1);
    ble_br_hfp_hf_test_util_rx_open();

    ble_br_hfp_hf_test_util_verify_tx("AT+BRSF=532\r");
    ble_br_hfp_hf_test_util_rx("\r\n+BRSF: 1\r\n\r\nOK\r\n");

    /* The audio gateway has the indicators in an order of its own, one of
     * them unknown.
     */
    ble_br_hfp_hf_test_util_verify_tx("AT+CIND=?\r");
    ble_br_hfp_hf_test_util_rx("\r\n+CIND: (\"battchg\",(0-5)),"
                               "(\"message\",(0,1)),(\"call\",(0,1))\r\n"
                               "\r\nOK\r\n");

    ble_br_hfp_hf_test_util_verify_tx("AT+CIND?\r");
    ble_br_hfp_hf_test_util_rx("\r\n+CIND: 3,1,0\r\n\r\nOK\r\n");

    ble_br_hfp_hf_test_util_verify_indicator(BLE_BR_HFP_HF_IND_BATTCHG, 3);
    ble_br_hfp_hf_test_util_verify_indicator(BLE_BR_HFP_HF_IND_CALL, 0);
    ble_br_hfp_hf_test_util_verify_no_event();

    ble_br_hfp_hf_test_util_verify_tx("AT+CMER=3,0,0,1\r");
    ble_br_hfp_hf_test_util_rx("\r\nOK\r\n");
    ble_br_hfp_hf_test_util_verify_connect(0);
    ble_br_hfp_hf_test_util_verify_tx("AT+CLIP=1\r");
    ble_br_hfp_hf_test_util_rx("\r\nOK\r\n");

    /*** Changes of the indicators. */
    ble_br_hfp_hf_test_util_rx("\r\n+CIEV: 3,1\r\n");
    ble_br_hfp_hf_test_util_verify_indicator(BLE_BR_HFP_HF_IND_CALL, 1);

    ble_br_hfp_hf_test_util_rx("\r\n+CIEV: 1,5\r\n");
    ble_br_hfp_hf_test_util_verify_indicator(BLE_BR_HFP_HF_IND_BATTCHG, 5);

    ble_br_hfp_hf_test_util_rx("\r\n+CIEV:3,0\r\n");
    ble_br_hfp_hf_test_util_verify_indicator(BLE_BR_HFP_HF_IND_CALL, 0);

    /*** Indicators the host does not know, or the audio gateway does not
     * have.
     */
    ble_br_hfp_hf_test_util_rx("\r\n+CIEV: 2,1\r\n");
    ble_br_hfp_hf_test_util_rx("\r\n+CIEV: 4,1\r\n");
    ble_br_hfp_hf_test_util_rx("\r\n+CIEV: 0,1\r\n");
    ble_br_hfp_hf_test_util_rx("\r\n+CIEV: 21,1\r\n");
    ble_br_hfp_hf_test_util_rx("\r\n+CIEV: 300,1\r\n");

    /*** Malformed reports. */
    ble_br_hfp_hf_test_util_rx("\r\n+CIEV: \r\n");
    ble_br_hfp_hf_test_util_rx("\r\n+CIEV: 3\r\n");
    ble_br_hfp_hf_test_util_rx("\r\n+CIEV: x,1\r\n");

    ble_br_hfp_hf_test_util_verify_no_event();
    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_hfp_hf_test_ring_clip)
{
    const struct ble_br_hfp_hf_event *event;

    ble_br_hfp_hf_test_util_init(1);
    ble_br_hfp_hf_test_util_connect(BLE_BR_HFP_HF_TEST_AG_FEAT_S4);

    /*** An incoming call. */
    ble_br_hfp_hf_test_util_rx("\r\n+CIEV: 3,1\r\n");
    ble_br_hfp_hf_test_util_verify_indicator(BLE_BR_HFP_HF_IND_CALLSETUP, 1);

    ble_br_hfp_hf_test_util_rx("\r\nRING\r\n");
    ble_br_hfp_hf_test_util_verify_event(BLE_BR_HFP_HF_EVENT_RING);

    ble_br_hfp_hf_test_util_rx("\r\n+CLIP: \"+15551234567\",145\r\n");
    event = ble_br_hfp_hf_test_util_event_next();
    TEST_ASSERT(event->type == BLE_BR_HFP_HF_EVENT_CLIP);
    TEST_ASSERT(strcmp(event->clip.number, "+15551234567") == 0);

    /* Both in a frame. */
    ble_br_hfp_hf_test_util_rx("\r\nRING\r\n\r\n+CLIP: \"5551234\",129\r\n");
    ble_br_hfp_hf_test_util_verify_event(BLE_BR_HFP_HF_EVENT_RING);
    event = ble_br_hfp_hf_test_util_event_next();
    TEST_ASSERT(event->type == BLE_BR_HFP_HF_EVENT_CLIP);
    TEST_ASSERT(strcmp(event->clip.number, "5551234") == 0);

    /*** A number that is longer than the host keeps. */
    ble_br_hfp_hf_test_util_rx(
        "\r\n+CLIP: \"0123456789012345678901234567890123456789\",129\r\n");
    event = ble_br_hfp_hf_test_util_event_next();
    TEST_ASSERT(event->type == BLE_BR_HFP_HF_EVENT_CLIP);
    TEST_ASSERT(strlen(event->clip.number) == BLE_BR_HFP_HF_NUMBER_MAX_LEN);
    TEST_ASSERT(strcmp(event->clip.number, "01234567890123456789012345678901") == 0);

    /*** A number that is withheld or malformed. */
    ble_br_hfp_hf_test_util_rx("\r\n+CLIP: \"\",128\r\n");
    event = ble_br_hfp_hf_test_util_event_next();
    TEST_ASSERT(event->type == BLE_BR_HFP_HF_EVENT_CLIP);
    TEST_ASSERT(strcmp(event->clip.number, "") == 0);

    ble_br_hfp_hf_test_util_rx("\r\n+CLIP: \"5551234\r\n");
    event = ble_br_hfp_hf_test_util_event_next();
    TEST_ASSERT(event->type == BLE_BR_HFP_HF_EVENT_CLIP);
    TEST_ASSERT(strcmp(event->clip.number, "") == 0);

    /*** The audio gateway sets the volume of the speaker. */
    ble_br_hfp_hf_test_util_rx("\r\n+VGS: 9\r\n");
    event = ble_br_hfp_hf_test_util_event_next();
    TEST_ASSERT(event->type == BLE_BR_HFP_HF_EVENT_VOLUME);
    TEST_ASSERT(event->volume.gain == 9);

    ble_br_hfp_hf_test_util_rx("\r\n+VGS=15\r\n");
    event = ble_br_hfp_hf_test_util_event_next();
    TEST_ASSERT(event->type == BLE_BR_HFP_HF_EVENT_VOLUME);
    TEST_ASSERT(event->volume.gain == 15);

    /* A gain out of range. */
    ble_br_hfp_hf_test_util_rx("\r\n+VGS: 16\r\n");

    /*** Results the host does not know. */
    ble_br_hfp_hf_test_util_rx("\r\n+CCWA: \"5551234\",129\r\n");
    ble_br_hfp_hf_test_util_rx("\r\n+BSIR: 0\r\n");

    ble_br_hfp_hf_test_util_verify_no_event();
    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_hfp_hf_test_cmds)
{
    int rc;
    int i;

    ble_br_hfp_hf_test_util_init(1);
    ble_br_hfp_hf_test_util_connect(BLE_BR_HFP_HF_TEST_AG_FEAT_S4);

    /*** Answer; the audio gateway accepts. */
    rc = ble_br_hfp_hf_answer(2);
    TEST_ASSERT(rc == 0);
    ble_br_hfp_hf_test_util_verify_tx("ATA\r");
    ble_br_hfp_hf_test_util_verify_no_event();

    ble_br_hfp_hf_test_util_rx("\r\nOK\r\n");
    ble_br_hfp_hf_test_util_verify_cmd_complete(0);

    /*** Hang up; the audio gateway refuses. */
    rc = ble_br_hfp_hf_hangup(2);
    TEST_ASSERT(rc == 0);
    ble_br_hfp_hf_test_util_verify_tx("AT+CHUP\r");

    ble_br_hfp_hf_test_util_rx("\r\nERROR\r\n");
    ble_br_hfp_hf_test_util_verify_cmd_complete(BLE_HS_EREJECT);

    /*** Dial; the audio gateway refuses, with a reason. */
    rc = ble_br_hfp_hf_dial(2, "+15551234567");
    TEST_ASSERT(rc == 0);
    ble_br_hfp_hf_test_util_verify_tx("ATD+15551234567;\r");

    ble_br_hfp_hf_test_util_rx("\r\n+CME ERROR: 30\r\n");
    ble_br_hfp_hf_test_util_verify_cmd_complete(BLE_HS_EREJECT);

    /*** Volume of the speaker. */
    rc = ble_br_hfp_hf_set_volume(2, 10);
    TEST_ASSERT(rc == 0);
    ble_br_hfp_hf_test_util_verify_tx("AT+VGS=10\r");

    ble_br_hfp_hf_test_util_rx("\r\nOK\r\n");
    ble_br_hfp_hf_test_util_verify_cmd_complete(0);

    /*** Commands that are not sent. */
    rc = ble_br_hfp_hf_answer(3);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);
    rc = ble_br_hfp_hf_dial(2, "");
    TEST_ASSERT(rc == BLE_HS_EINVAL);
    rc = ble_br_hfp_hf_dial(2, "555 1234");
    TEST_ASSERT(rc == BLE_HS_EINVAL);
    rc = ble_br_hfp_hf_dial(2, "5551234;\rAT+CHUP");
    TEST_ASSERT(rc == BLE_HS_EINVAL);
    rc = ble_br_hfp_hf_dial(2, "012345678901234567890123456789012");
    TEST_ASSERT(rc == BLE_HS_EINVAL);
    rc = ble_br_hfp_hf_set_volume(2, BLE_BR_HFP_HF_VOLUME_MAX + 1);
    TEST_ASSERT(rc == BLE_HS_EINVAL);
    ble_br_test_util_verify_tx_none();

    /* The longest number. */
    rc = ble_br_hfp_hf_dial(2, "01234567890123456789012345678901");
    TEST_ASSERT(rc == 0);
    ble_br_hfp_hf_test_util_verify_tx("ATD01234567890123456789012345678901;\r");
    ble_br_hfp_hf_test_util_rx("\r\nOK\r\n");
    ble_br_hfp_hf_test_util_verify_cmd_complete(0);

    /*** A result nothing waits for is ignored. */
    ble_br_hfp_hf_test_util_rx("\r\nOK\r\n");
    ble_br_hfp_hf_test_util_rx("\r\nERROR\r\n");
    ble_br_hfp_hf_test_util_verify_no_event();

    /*** One command at a time: the others wait for their turn. */
    rc = ble_br_hfp_hf_answer(2);
    TEST_ASSERT(rc == 0);
    for (i = 0; i < 4; i++) {
        rc = ble_br_hfp_hf_set_volume(2, i);
        TEST_ASSERT(rc == 0);
    }
    rc = ble_br_hfp_hf_hangup(2);
    TEST_ASSERT(rc == BLE_HS_EBUSY);

    ble_br_hfp_hf_test_util_verify_tx("ATA\r");
    ble_br_test_util_verify_tx_none();

    /* Unsolicited results do not complete a command. */
    ble_br_hfp_hf_test_util_rx("\r\n+CIEV: 2,1\r\n");
    ble_br_hfp_hf_test_util_verify_indicator(BLE_BR_HFP_HF_IND_CALL, 1);
    ble_br_test_util_verify_tx_none();

    ble_br_hfp_hf_test_util_rx("\r\nOK\r\n");
    ble_br_hfp_hf_test_util_verify_cmd_complete(0);
    ble_br_hfp_hf_test_util_verify_tx("AT+VGS=0\r");
    ble_br_test_util_verify_tx_none();

    ble_br_hfp_hf_test_util_rx("\r\nERROR\r\n");
    ble_br_hfp_hf_test_util_verify_cmd_complete(BLE_HS_EREJECT);
    ble_br_hfp_hf_test_util_verify_tx("AT+VGS=1\r");

    /* There is room for a command again. */
    rc = ble_br_hfp_hf_hangup(2);
    TEST_ASSERT(rc == 0);

    ble_br_hfp_hf_test_util_rx("\r\nOK\r\n");
    ble_br_hfp_hf_test_util_verify_cmd_complete(0);
    ble_br_hfp_hf_test_util_verify_tx("AT+VGS=2\r");

    ble_br_hfp_hf_test_util_rx("\r\nOK\r\n");
    ble_br_hfp_hf_test_util_verify_cmd_complete(0);
    ble_br_hfp_hf_test_util_verify_tx("AT+VGS=3\r");

    ble_br_hfp_hf_test_util_rx("\r\nOK\r\n");
    ble_br_hfp_hf_test_util_verify_cmd_complete(0);
    ble_br_hfp_hf_test_util_verify_tx("AT+CHUP\r");

    ble_br_hfp_hf_test_util_rx("\r\nOK\r\n");
    ble_br_hfp_hf_test_util_verify_cmd_complete(0);

    ble_br_hfp_hf_test_util_verify_no_event();
    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_hfp_hf_test_cmd_timeout)
{
    int32_t ticks_from_now;
    int32_t timeout_ticks;
    int rc;

    ble_br_hfp_hf_test_util_init(1);
    ble_br_hfp_hf_test_util_connect(BLE_BR_HFP_HF_TEST_AG_FEAT_S4);

    timeout_ticks = ble_npl_time_ms_to_ticks32(BLE_BR_HFP_HF_TEST_CMD_TIMEOUT_MS);

    /* Ensure timer is not set. */
    ticks_from_now = ble_br_test_util_time_advance(0);
    TEST_ASSERT(ticks_from_now == BLE_HS_FOREVER);

    /*** The audio gateway answers just in time. */
    rc = ble_br_hfp_hf_answer(2);
    TEST_ASSERT(rc == 0);
    ble_br_hfp_hf_test_util_verify_tx("ATA\r");

    /* Ensure timer will expire in 10 seconds. */
    ticks_from_now = ble_br_test_util_time_advance(0);
    TEST_ASSERT(ticks_from_now == timeout_ticks);

    /* Almost let the timer expire. */
    ticks_from_now = ble_br_test_util_time_advance(timeout_ticks - 1);
    TEST_ASSERT(ticks_from_now == 1);

    ble_br_hfp_hf_test_util_rx("\r\nOK\r\n");
    ble_br_hfp_hf_test_util_verify_cmd_complete(0);

    /* Ensure timer has been stopped. */
    ticks_from_now = ble_br_test_util_time_advance(1);
    TEST_ASSERT(ticks_from_now == BLE_HS_FOREVER);
    ble_br_hfp_hf_test_util_verify_no_event();

    /*** The audio gateway does not answer: it is let go. */
    rc = ble_br_hfp_hf_hangup(2);
    TEST_ASSERT(rc == 0);
    ble_br_hfp_hf_test_util_verify_tx("AT+CHUP\r");

    /* Unsolicited results do not restart the timer. */
    ticks_from_now = ble_br_test_util_time_advance(timeout_ticks - 1);
    TEST_ASSERT(ticks_from_now == 1);
    ble_br_hfp_hf_test_util_rx("\r\nRING\r\n");
    ble_br_hfp_hf_test_util_verify_event(BLE_BR_HFP_HF_EVENT_RING);

    /* Allow the timer to expire. */
    ticks_from_now = ble_br_test_util_time_advance(1);
    TEST_ASSERT(ticks_from_now == BLE_HS_FOREVER);

    ble_br_hfp_hf_test_util_verify_event(BLE_BR_HFP_HF_EVENT_DISCONNECT);
    ble_br_hfp_hf_test_util_verify_no_event();
    ble_br_hfp_hf_test_util_verify_tx_close();

    rc = ble_br_hfp_hf_hangup(2);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);

    /*** The audio gateway stops to answer as the connection is set up. */
    ble_br_hfp_hf_test_util_rx_open();
    ble_br_hfp_hf_test_util_verify_tx("AT+BRSF=532\r");
    ble_br_hfp_hf_test_util_rx("\r\n+BRSF: 1\r\n\r\nOK\r\n");
    ble_br_hfp_hf_test_util_verify_tx("AT+CIND=?\r");

    ticks_from_now = ble_br_test_util_time_advance(0);
    TEST_ASSERT(ticks_from_now == timeout_ticks);

    ticks_from_now = ble_br_test_util_time_advance(timeout_ticks);
    TEST_ASSERT(ticks_from_now == BLE_HS_FOREVER);

    ble_br_hfp_hf_test_util_verify_connect(BLE_HS_ETIMEOUT);
    ble_br_hfp_hf_test_util_verify_no_event();
    ble_br_hfp_hf_test_util_verify_tx_close();

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_hfp_hf_test_fragmented)
{
    char filler[101];
    int rc;
    int i;

    ble_br_hfp_hf_test_util_init(1);

    /*** The responses of the connection setup, cut at odd places. */
    ble_br_hfp_hf_test_util_rx_open();

    ble_br_hfp_hf_test_util_verify_tx("AT+BRSF=532\r");
    ble_br_hfp_hf_test_util_rx("\r\n+BR");
    ble_br_hfp_hf_test_util_rx("SF: 20");
    ble_br_hfp_hf_test_util_rx("49\r\n\r");
    ble_br_test_util_verify_tx_none();
    ble_br_hfp_hf_test_util_rx("\nO");
    ble_br_test_util_verify_tx_none();
    ble_br_hfp_hf_test_util_rx("K\r");

    ble_br_hfp_hf_test_util_verify_tx("AT+CIND=?\r");
    ble_br_hfp_hf_test_util_rx("\n\r\n+CIND: (\"service\",(0,1)),(\"ca");
    ble_br_hfp_hf_test_util_rx("ll\",(0,1)),(\"callsetup\",(0-3))");
    ble_br_hfp_hf_test_util_rx("\r\n\r\nOK\r\n");

    ble_br_hfp_hf_test_util_verify_tx("AT+CIND?\r");
    ble_br_hfp_hf_test_util_rx("\r\n+CIND: 1,");
    ble_br_hfp_hf_test_util_verify_no_event();
    ble_br_hfp_hf_test_util_rx("1,2\r\n\r\nOK\r\n");

    ble_br_hfp_hf_test_util_verify_indicator(BLE_BR_HFP_HF_IND_SERVICE, 1);
    ble_br_hfp_hf_test_util_verify_indicator(BLE_BR_HFP_HF_IND_CALL, 1);
    ble_br_hfp_hf_test_util_verify_indicator(BLE_BR_HFP_HF_IND_CALLSETUP, 2);

    ble_br_hfp_hf_test_util_verify_tx("AT+CMER=3,0,0,1\r");
    ble_br_hfp_hf_test_util_rx("\r\nOK");
    ble_br_hfp_hf_test_util_verify_no_event();
    ble_br_hfp_hf_test_util_rx("\r\n");

    ble_br_hfp_hf_test_util_verify_connect(0);
    ble_br_hfp_hf_test_util_verify_tx("AT+CLIP=1\r");
    ble_br_hfp_hf_test_util_rx("\r\nOK\r\n");

    /*** An unsolicited result in two frames. */
    ble_br_hfp_hf_test_util_rx("\r\n+CIEV: ");
    ble_br_hfp_hf_test_util_verify_no_event();
    ble_br_hfp_hf_test_util_rx("2,0\r\n");
    ble_br_hfp_hf_test_util_verify_indicator(BLE_BR_HFP_HF_IND_CALL, 0);

    /*** The result of a command in two frames. */
    rc = ble_br_hfp_hf_answer(2);
    TEST_ASSERT(rc == 0);
    ble_br_hfp_hf_test_util_verify_tx("ATA\r");

    ble_br_hfp_hf_test_util_rx("\r\nERR");
    ble_br_hfp_hf_test_util_verify_no_event();
    ble_br_hfp_hf_test_util_rx("OR\r\n");
    ble_br_hfp_hf_test_util_verify_cmd_complete(BLE_HS_EREJECT);

    /*** Three results in a frame, the last one unfinished. */
    ble_br_hfp_hf_test_util_rx("\r\nRING\r\n\r\n+CIEV: 2,1\r\n\r\n+VGS");
    ble_br_hfp_hf_test_util_verify_event(BLE_BR_HFP_HF_EVENT_RING);
    ble_br_hfp_hf_test_util_verify_indicator(BLE_BR_HFP_HF_IND_CALL, 1);
    ble_br_hfp_hf_test_util_verify_no_event();

    ble_br_hfp_hf_test_util_rx(": 7\r\n");
    ble_br_hfp_hf_test_util_verify_event(BLE_BR_HFP_HF_EVENT_VOLUME);

    /*** A line that is longer than the host keeps. */
    memset(filler, 'A', sizeof filler - 1);
    filler[sizeof filler - 1] = '\0';

    ble_br_hfp_hf_test_util_rx("\r\n");
    for (i = 0; i < 4; i++) {
        ble_br_hfp_hf_test_util_rx(filler);
    }
    ble_br_hfp_hf_test_util_rx("\r\n\r\nRING\r\n");
    ble_br_hfp_hf_test_util_verify_event(BLE_BR_HFP_HF_EVENT_RING);

    ble_br_hfp_hf_test_util_verify_no_event();
    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_hfp_hf_test_disconnect)
{
    int rc;

    ble_br_hfp_hf_test_util_init(1);

    /*** We disconnect. */
    ble_br_hfp_hf_test_util_connect(BLE_BR_HFP_HF_TEST_AG_FEAT_S4);

    rc = ble_br_hfp_hf_disconnect(3);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);

    rc = ble_br_hfp_hf_disconnect(2);
    TEST_ASSERT(rc == 0);
    ble_br_hfp_hf_test_util_verify_event(BLE_BR_HFP_HF_EVENT_DISCONNECT);
    ble_br_hfp_hf_test_util_verify_tx_close();

    rc = ble_br_hfp_hf_disconnect(2);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);

    /*** The audio gateway closes the link; a command waits for its
     * result.
     */
    ble_br_hfp_hf_test_util_connect(BLE_BR_HFP_HF_TEST_AG_FEAT_S4);

    rc = ble_br_hfp_hf_answer(2);
    TEST_ASSERT(rc == 0);
    ble_br_hfp_hf_test_util_verify_tx("ATA\r");

    ble_br_test_util_rfcomm_rx(
        &ble_br_hfp_hf_test_chan,
        BLE_BR_TEST_UTIL_RFCOMM_ADDR(ble_br_hfp_hf_test_dlci, 1),
        BLE_BR_TEST_UTIL_RFCOMM_DISC | BLE_BR_TEST_UTIL_RFCOMM_PF, -1, NULL, 0);
    ble_br_test_util_tx_queue_clear();

    ble_br_hfp_hf_test_util_verify_event(BLE_BR_HFP_HF_EVENT_DISCONNECT);
    ble_br_hfp_hf_test_util_verify_no_event();

    /* Nothing is left to time out. */
    TEST_ASSERT(ble_br_test_util_time_advance(ble_npl_time_ms_to_ticks32(
                    BLE_BR_HFP_HF_TEST_CMD_TIMEOUT_MS)) == BLE_HS_FOREVER);
    ble_br_hfp_hf_test_util_verify_no_event();

    /*** The connection is lost. */
    ble_hs_test_util_hci_rx_disconn_complete_event(2, 0, BLE_ERR_CONN_SPVN_TMO);
    ble_br_test_util_tx_queue_clear();
    ble_br_test_util_gap_event_next();
    ble_br_test_util_create_conn(2, ble_br_hfp_hf_test_addr, 1);

    ble_br_hfp_hf_test_util_connect(BLE_BR_HFP_HF_TEST_AG_FEAT_S4);

    ble_hs_test_util_hci_rx_disconn_complete_event(2, 0, BLE_ERR_CONN_SPVN_TMO);
    ble_br_hfp_hf_test_util_verify_event(BLE_BR_HFP_HF_EVENT_DISCONNECT);
    ble_br_hfp_hf_test_util_verify_no_event();

    rc = ble_br_hfp_hf_answer(2);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

/**
 * Has the server of the peer tell where its audio gateway service is.
 *
 * @param channel               The RFCOMM channel; 0 for no such service.
 */
static void
ble_br_hfp_hf_test_util_sdp(uint8_t channel)
{
    /* A search for the protocols of the audio gateway service. */
    uint8_t exp_req[] = { 0x06, 0x00, 0x01, 0x00, 0x0d, 0x35, 0x03, 0x19, 0x11,
                          0x1f, 0x00, 0xa0, 0x35, 0x03, 0x09, 0x00, 0x04, 0x00 };
    /* L2CAP, and RFCOMM with its server channel, which is filled in. */
    uint8_t rsp[] = {
        0x07, 0x00, 0x01, 0x00, 0x18, 0x00, 0x15, 0x35, 0x13, 0x35,
        0x11, 0x09, 0x00, 0x04, 0x35, 0x0c, 0x35, 0x03, 0x19, 0x01,
        0x00, 0x35, 0x05, 0x19, 0x00, 0x03, 0x08, 0x00, 0x00,
    };
    uint8_t rsp_none[] = {
        0x07, 0x00, 0x01, 0x00, 0x05, 0x00, 0x02, 0x35, 0x00, 0x00,
    };
    struct ble_br_test_util_chan chan;

    /* Each request of the client has a transaction ID of its own. */
    ble_br_hfp_hf_test_sdp_tid++;
    put_be16(exp_req + 1, ble_br_hfp_hf_test_sdp_tid);
    put_be16(rsp + 1, ble_br_hfp_hf_test_sdp_tid);
    rsp[sizeof rsp - 2] = channel;
    put_be16(rsp_none + 1, ble_br_hfp_hf_test_sdp_tid);

    ble_br_test_util_l2cap_rx_connect_rsp(&chan, 2, BLE_BR_L2CAP_PSM_SDP, 0x0070);
    ble_br_test_util_verify_tx_l2cap(2, chan.peer_cid, exp_req, sizeof exp_req);

    if (channel != 0) {
        ble_br_test_util_rx_l2cap(2, chan.our_cid, rsp, sizeof rsp);
    } else {
        ble_br_test_util_rx_l2cap(2, chan.our_cid, rsp_none, sizeof rsp_none);
    }

    ble_br_test_util_l2cap_verify_tx_disconnect(&chan);
}

TEST_CASE_SELF(ble_br_hfp_hf_test_outgoing)
{
    const struct ble_br_gap_event *gap_event;
    struct ble_hci_ev_auth_cmp auth_cmp;
    uint8_t param_len;
    uint8_t *param;
    int rc;

    /* A connection that is not encrypted. */
    ble_br_hfp_hf_test_util_init(0);

    rc = ble_br_hfp_hf_connect(3);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);

    /*** The link gets authenticated and encrypted first. */
    ble_br_test_util_ack_set(BLE_HCI_OCF_AUTH_REQUESTED, 0);
    rc = ble_br_hfp_hf_connect(2);
    TEST_ASSERT_FATAL(rc == 0);

    param = ble_hs_test_util_hci_verify_tx(BLE_HCI_OGF_LINK_CTRL,
                                           BLE_HCI_OCF_AUTH_REQUESTED, &param_len);
    TEST_ASSERT(param_len == 2);
    TEST_ASSERT(get_le16(param) == 2);
    ble_br_test_util_verify_tx_none();

    rc = ble_br_hfp_hf_connect(2);
    TEST_ASSERT(rc == BLE_HS_EALREADY);

    auth_cmp.status = 0;
    auth_cmp.conn_handle = htole16(2);
    ble_br_test_util_ack_set(BLE_HCI_OCF_SET_CONN_ENCRYPT, 0);
    ble_br_test_util_rx_evt(BLE_HCI_EVCODE_AUTH_CMP, &auth_cmp, sizeof auth_cmp);
    ble_hs_test_util_hci_verify_tx(BLE_HCI_OGF_LINK_CTRL,
                                   BLE_HCI_OCF_SET_CONN_ENCRYPT, &param_len);
    ble_br_test_util_verify_tx_none();

    ble_br_test_util_rx_enc_change(2, 0, 1);

    gap_event = ble_br_test_util_gap_event_next();
    TEST_ASSERT_FATAL(gap_event != NULL);
    TEST_ASSERT(gap_event->type == BLE_BR_GAP_EVENT_ENC_CHANGE);
    TEST_ASSERT(gap_event->enc_change.encrypted);

    /*** The service of the audio gateway is searched for. */
    ble_br_hfp_hf_test_util_sdp(BLE_BR_HFP_HF_TEST_AG_CHANNEL);

    /*** The host starts a session and opens a link to the service. */
    ble_br_hfp_hf_test_dlci = BLE_BR_HFP_HF_TEST_AG_DLCI;
    ble_br_hfp_hf_test_cr = 1;
    ble_br_hfp_hf_test_rx_cnt = 0;

    ble_br_test_util_rfcomm_rx_accept(&ble_br_hfp_hf_test_chan, 2,
                                      BLE_BR_HFP_HF_TEST_AG_DLCI, 7);
    ble_br_hfp_hf_test_util_verify_no_event();

    /*** The service level connection is set up. */
    ble_br_hfp_hf_test_util_rx_credits(100);
    ble_br_hfp_hf_test_util_slc(BLE_BR_HFP_HF_TEST_AG_FEAT_S4);

    rc = ble_br_hfp_hf_answer(2);
    TEST_ASSERT(rc == 0);
    ble_br_hfp_hf_test_util_verify_tx("ATA\r");
    ble_br_hfp_hf_test_util_rx("\r\nOK\r\n");
    ble_br_hfp_hf_test_util_verify_cmd_complete(0);

    /*** We disconnect, and connect again: the link is encrypted. */
    rc = ble_br_hfp_hf_disconnect(2);
    TEST_ASSERT(rc == 0);
    ble_br_hfp_hf_test_util_verify_event(BLE_BR_HFP_HF_EVENT_DISCONNECT);
    ble_br_hfp_hf_test_util_verify_tx_close();

    rc = ble_br_hfp_hf_connect(2);
    TEST_ASSERT_FATAL(rc == 0);
    TEST_ASSERT(ble_hs_test_util_hci_out_first() == NULL);

    /*** The peer has no audio gateway service. */
    ble_br_hfp_hf_test_util_sdp(0);

    ble_br_hfp_hf_test_util_verify_connect(BLE_HS_ENOENT);
    ble_br_hfp_hf_test_util_verify_no_event();
    ble_br_test_util_verify_tx_none();

    /*** The peer refuses the link to its service. */
    rc = ble_br_hfp_hf_connect(2);
    TEST_ASSERT_FATAL(rc == 0);
    ble_br_hfp_hf_test_util_sdp(BLE_BR_HFP_HF_TEST_AG_CHANNEL);

    ble_br_test_util_l2cap_rx_connect_rsp(&ble_br_hfp_hf_test_chan, 2,
                                          BLE_BR_L2CAP_PSM_RFCOMM, 0x0060);
    ble_br_test_util_tx_queue_clear();
    ble_br_test_util_rfcomm_rx(
        &ble_br_hfp_hf_test_chan, BLE_BR_TEST_UTIL_RFCOMM_ADDR(0, 1),
        BLE_BR_TEST_UTIL_RFCOMM_DM | BLE_BR_TEST_UTIL_RFCOMM_PF, -1, NULL, 0);

    ble_br_hfp_hf_test_util_verify_connect(BLE_HS_EREJECT);
    ble_br_hfp_hf_test_util_verify_no_event();
    ble_br_test_util_l2cap_verify_tx_disconnect(&ble_br_hfp_hf_test_chan);

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_hfp_hf_test_outgoing_fail)
{
    const struct ble_br_gap_event *gap_event;
    struct ble_hci_ev_auth_cmp auth_cmp;
    uint8_t param_len;
    int rc;

    ble_br_hfp_hf_test_util_init(0);

    /*** The controller refuses to authenticate the link. */
    ble_br_test_util_ack_set(BLE_HCI_OCF_AUTH_REQUESTED, BLE_ERR_CMD_DISALLOWED);
    rc = ble_br_hfp_hf_connect(2);
    TEST_ASSERT(rc == BLE_HS_HCI_ERR(BLE_ERR_CMD_DISALLOWED));
    ble_hs_test_util_hci_out_clear();
    ble_br_hfp_hf_test_util_verify_no_event();

    /*** The authentication fails. */
    ble_br_test_util_ack_set(BLE_HCI_OCF_AUTH_REQUESTED, 0);
    rc = ble_br_hfp_hf_connect(2);
    TEST_ASSERT_FATAL(rc == 0);
    ble_hs_test_util_hci_verify_tx(BLE_HCI_OGF_LINK_CTRL,
                                   BLE_HCI_OCF_AUTH_REQUESTED, &param_len);

    auth_cmp.status = BLE_ERR_AUTH_FAIL;
    auth_cmp.conn_handle = htole16(2);
    ble_br_test_util_rx_evt(BLE_HCI_EVCODE_AUTH_CMP, &auth_cmp, sizeof auth_cmp);

    gap_event = ble_br_test_util_gap_event_next();
    TEST_ASSERT_FATAL(gap_event != NULL);
    TEST_ASSERT(gap_event->type == BLE_BR_GAP_EVENT_ENC_CHANGE);
    TEST_ASSERT(!gap_event->enc_change.encrypted);

    ble_br_hfp_hf_test_util_verify_connect(BLE_HS_HCI_ERR(BLE_ERR_AUTH_FAIL));
    ble_br_hfp_hf_test_util_verify_no_event();

    /*** The connection is lost as the link gets authenticated. */
    ble_br_test_util_ack_set(BLE_HCI_OCF_AUTH_REQUESTED, 0);
    rc = ble_br_hfp_hf_connect(2);
    TEST_ASSERT_FATAL(rc == 0);
    ble_hs_test_util_hci_verify_tx(BLE_HCI_OGF_LINK_CTRL,
                                   BLE_HCI_OCF_AUTH_REQUESTED, &param_len);

    ble_hs_test_util_hci_rx_disconn_complete_event(2, 0, BLE_ERR_CONN_SPVN_TMO);

    ble_br_hfp_hf_test_util_verify_connect(BLE_HS_ENOTCONN);
    ble_br_hfp_hf_test_util_verify_no_event();

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

/**
 * Has the audio gateway ask for an audio connection, and verifies the host
 * accepts it with the specified settings.
 */
static void
ble_br_hfp_hf_test_util_rx_audio(uint8_t link_type,
                                 const struct ble_br_sco_params *params)
{
    const struct ble_br_hfp_hf_event *event;

    ble_br_test_util_ack_set(BLE_HCI_OCF_ACCEPT_SYNCH_CONN_REQ, 0);
    ble_br_test_util_rx_conn_request(ble_br_hfp_hf_test_addr, link_type);
    ble_br_test_util_verify_tx_accept_synch_conn(ble_br_hfp_hf_test_addr, params);
    ble_br_hfp_hf_test_util_verify_no_event();

    ble_br_test_util_rx_synch_conn_cmp(0, BLE_BR_HFP_HF_TEST_SCO_HANDLE,
                                       ble_br_hfp_hf_test_addr, link_type);

    event = ble_br_hfp_hf_test_util_event_next();
    TEST_ASSERT(event->type == BLE_BR_HFP_HF_EVENT_AUDIO_CONNECT);
    TEST_ASSERT(event->audio_connect.status == 0);
    TEST_ASSERT(event->audio_connect.sco_handle == BLE_BR_HFP_HF_TEST_SCO_HANDLE);
}

/**
 * Has the audio gateway end the audio connection.
 */
static void
ble_br_hfp_hf_test_util_rx_audio_disconnect(void)
{
    const struct ble_br_hfp_hf_event *event;

    ble_hs_test_util_hci_rx_disconn_complete_event(BLE_BR_HFP_HF_TEST_SCO_HANDLE,
                                                   0, BLE_ERR_REM_USER_CONN_TERM);

    event = ble_br_hfp_hf_test_util_event_next();
    TEST_ASSERT(event->type == BLE_BR_HFP_HF_EVENT_AUDIO_DISCONNECT);
    TEST_ASSERT(event->audio_disconnect.sco_handle == BLE_BR_HFP_HF_TEST_SCO_HANDLE);
}

TEST_CASE_SELF(ble_br_hfp_hf_test_audio)
{
    /* The settings of the profile, with the retransmission effort for power
     * consumption or for link quality.
     */
    static const struct ble_br_sco_params params_s4 = {
        .max_latency = 12,
        .retrans_effort = BLE_HCI_SYNCH_RETRANS_QUALITY,
        .pkt_type = BLE_HCI_SYNCH_PKT_NO_3EV3 | BLE_HCI_SYNCH_PKT_NO_2EV5 |
                    BLE_HCI_SYNCH_PKT_NO_3EV5,
    };
    static const struct ble_br_sco_params params_s3 = {
        .max_latency = 10,
        .retrans_effort = BLE_HCI_SYNCH_RETRANS_POWER,
        .pkt_type = BLE_HCI_SYNCH_PKT_EV3 | BLE_HCI_SYNCH_PKT_NO_3EV3 |
                    BLE_HCI_SYNCH_PKT_NO_2EV5 | BLE_HCI_SYNCH_PKT_NO_3EV5,
    };
    static const struct ble_br_sco_params params_d1 = {
        .max_latency = BLE_HCI_SYNCH_LATENCY_DONT_CARE,
        .retrans_effort = BLE_HCI_SYNCH_RETRANS_NONE,
        .pkt_type = BLE_HCI_SYNCH_PKT_HV3 | BLE_HCI_SYNCH_PKT_NO_2EV3 |
                    BLE_HCI_SYNCH_PKT_NO_3EV3 | BLE_HCI_SYNCH_PKT_NO_2EV5 |
                    BLE_HCI_SYNCH_PKT_NO_3EV5,
    };
    static const uint8_t other_addr[BLE_DEV_ADDR_LEN] = {
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
    };
    const struct ble_br_hfp_hf_event *event;
    uint8_t param_len;
    uint8_t *param;
    int rc;

    ble_br_hfp_hf_test_util_init(1);

    /*** No audio without a service level connection. */
    ble_br_test_util_ack_set(BLE_HCI_OCF_REJECT_SYNCH_CONN_REQ, 0);
    ble_br_test_util_rx_conn_request(ble_br_hfp_hf_test_addr, BLE_HCI_LINK_TYPE_ESCO);
    param = ble_hs_test_util_hci_verify_tx(
        BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_REJECT_SYNCH_CONN_REQ, &param_len);
    TEST_ASSERT(memcmp(param, ble_br_hfp_hf_test_addr, BLE_DEV_ADDR_LEN) == 0);

    rc = ble_br_hfp_hf_audio_connect(2);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);

    /*** An audio gateway with the eSCO S4 settings. */
    ble_br_hfp_hf_test_util_connect(BLE_BR_HFP_HF_TEST_AG_FEAT_S4);

    ble_br_hfp_hf_test_util_rx_audio(BLE_HCI_LINK_TYPE_ESCO, &params_s4);
    ble_br_hfp_hf_test_util_rx_audio_disconnect();

    /* A SCO connection. */
    ble_br_hfp_hf_test_util_rx_audio(BLE_HCI_LINK_TYPE_SCO, &params_d1);
    ble_br_hfp_hf_test_util_rx_audio_disconnect();

    /* Only the audio gateway gets audio. */
    ble_br_test_util_ack_set(BLE_HCI_OCF_REJECT_SYNCH_CONN_REQ, 0);
    ble_br_test_util_rx_conn_request(other_addr, BLE_HCI_LINK_TYPE_ESCO);
    param = ble_hs_test_util_hci_verify_tx(
        BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_REJECT_SYNCH_CONN_REQ, &param_len);
    TEST_ASSERT(memcmp(param, other_addr, BLE_DEV_ADDR_LEN) == 0);

    /*** The audio connection fails to get set up. */
    ble_br_test_util_ack_set(BLE_HCI_OCF_ACCEPT_SYNCH_CONN_REQ, 0);
    ble_br_test_util_rx_conn_request(ble_br_hfp_hf_test_addr, BLE_HCI_LINK_TYPE_ESCO);
    ble_br_test_util_verify_tx_accept_synch_conn(ble_br_hfp_hf_test_addr, &params_s4);
    ble_br_test_util_rx_synch_conn_cmp(BLE_ERR_CONN_ACCEPT_TMO, 0,
                                       ble_br_hfp_hf_test_addr,
                                       BLE_HCI_LINK_TYPE_ESCO);

    event = ble_br_hfp_hf_test_util_event_next();
    TEST_ASSERT(event->type == BLE_BR_HFP_HF_EVENT_AUDIO_CONNECT);
    TEST_ASSERT(event->audio_connect.status == BLE_HS_HCI_ERR(BLE_ERR_CONN_ACCEPT_TMO));

    /*** We set the audio connection up, and end it. */
    rc = ble_br_hfp_hf_audio_connect(3);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);

    ble_br_test_util_ack_set(BLE_HCI_OCF_SETUP_SYNCH_CONN, 0);
    rc = ble_br_hfp_hf_audio_connect(2);
    TEST_ASSERT_FATAL(rc == 0);

    param = ble_hs_test_util_hci_verify_tx(
        BLE_HCI_OGF_LINK_CTRL, BLE_HCI_OCF_SETUP_SYNCH_CONN, &param_len);
    TEST_ASSERT(param_len == sizeof(struct ble_hci_lc_setup_synch_conn_cp));
    TEST_ASSERT(get_le16(param + 0) == 2);
    TEST_ASSERT(get_le16(param + 10) == params_s4.max_latency);
    TEST_ASSERT(param[14] == params_s4.retrans_effort);
    TEST_ASSERT(get_le16(param + 15) == params_s4.pkt_type);

    rc = ble_br_hfp_hf_audio_connect(2);
    TEST_ASSERT(rc == BLE_HS_EALREADY);

    ble_br_test_util_rx_synch_conn_cmp(0, BLE_BR_HFP_HF_TEST_SCO_HANDLE,
                                       ble_br_hfp_hf_test_addr,
                                       BLE_HCI_LINK_TYPE_ESCO);
    event = ble_br_hfp_hf_test_util_event_next();
    TEST_ASSERT(event->type == BLE_BR_HFP_HF_EVENT_AUDIO_CONNECT);
    TEST_ASSERT(event->audio_connect.status == 0);
    TEST_ASSERT(event->audio_connect.sco_handle == BLE_BR_HFP_HF_TEST_SCO_HANDLE);

    rc = ble_br_hfp_hf_audio_disconnect(3);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);

    ble_hs_test_util_hci_ack_set_disconnect(0);
    rc = ble_br_hfp_hf_audio_disconnect(2);
    TEST_ASSERT(rc == 0);
    ble_hs_test_util_hci_verify_tx_disconnect(BLE_BR_HFP_HF_TEST_SCO_HANDLE,
                                              BLE_ERR_REM_USER_CONN_TERM);

    ble_br_hfp_hf_test_util_rx_audio_disconnect();

    rc = ble_br_hfp_hf_audio_disconnect(2);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);

    /*** An audio gateway without the eSCO S4 settings. */
    ble_hs_test_util_hci_rx_disconn_complete_event(2, 0, BLE_ERR_CONN_SPVN_TMO);
    ble_br_hfp_hf_test_util_verify_event(BLE_BR_HFP_HF_EVENT_DISCONNECT);
    ble_br_hfp_hf_test_util_verify_no_event();

    ble_br_test_util_tx_queue_clear();
    ble_br_test_util_gap_event_next();
    ble_br_test_util_create_conn(2, ble_br_hfp_hf_test_addr, 1);
    ble_br_hfp_hf_test_util_connect(BLE_BR_HFP_HF_TEST_AG_FEAT_NO_S4);

    ble_br_hfp_hf_test_util_rx_audio(BLE_HCI_LINK_TYPE_ESCO, &params_s3);
    ble_br_hfp_hf_test_util_rx_audio_disconnect();

    ble_br_hfp_hf_test_util_verify_no_event();
    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_br_hfp_hf_test_audio_close)
{
    static const struct ble_br_sco_params params = {
        .max_latency = 12,
        .retrans_effort = BLE_HCI_SYNCH_RETRANS_QUALITY,
        .pkt_type = BLE_HCI_SYNCH_PKT_NO_3EV3 | BLE_HCI_SYNCH_PKT_NO_2EV5 |
                    BLE_HCI_SYNCH_PKT_NO_3EV5,
    };
    const struct ble_br_hfp_hf_event *event;
    int32_t timeout_ticks;
    int rc;

    ble_br_hfp_hf_test_util_init(1);

    timeout_ticks = ble_npl_time_ms_to_ticks32(BLE_BR_HFP_HF_TEST_CMD_TIMEOUT_MS);

    /*** We disconnect while audio is up: the audio ends first. */
    ble_br_hfp_hf_test_util_connect(BLE_BR_HFP_HF_TEST_AG_FEAT_S4);
    ble_br_hfp_hf_test_util_rx_audio(BLE_HCI_LINK_TYPE_ESCO, &params);

    ble_hs_test_util_hci_ack_set_disconnect(0);
    rc = ble_br_hfp_hf_disconnect(2);
    TEST_ASSERT(rc == 0);

    ble_hs_test_util_hci_verify_tx_disconnect(BLE_BR_HFP_HF_TEST_SCO_HANDLE,
                                              BLE_ERR_REM_USER_CONN_TERM);
    TEST_ASSERT(ble_hs_test_util_hci_out_first() == NULL);

    event = ble_br_hfp_hf_test_util_event_next();
    TEST_ASSERT(event->type == BLE_BR_HFP_HF_EVENT_AUDIO_DISCONNECT);
    TEST_ASSERT(event->audio_disconnect.sco_handle == BLE_BR_HFP_HF_TEST_SCO_HANDLE);
    ble_br_hfp_hf_test_util_verify_event(BLE_BR_HFP_HF_EVENT_DISCONNECT);
    ble_br_hfp_hf_test_util_verify_no_event();
    ble_br_hfp_hf_test_util_verify_tx_close();

    /* The application was told already as the controller confirms. */
    ble_hs_test_util_hci_rx_disconn_complete_event(BLE_BR_HFP_HF_TEST_SCO_HANDLE,
                                                   0, BLE_ERR_CONN_TERM_LOCAL);
    ble_br_hfp_hf_test_util_verify_no_event();

    rc = ble_br_hfp_hf_audio_disconnect(2);
    TEST_ASSERT(rc == BLE_HS_ENOTCONN);

    /*** The audio gateway stops to answer while audio is up. */
    ble_br_hfp_hf_test_util_connect(BLE_BR_HFP_HF_TEST_AG_FEAT_S4);
    ble_br_hfp_hf_test_util_rx_audio(BLE_HCI_LINK_TYPE_ESCO, &params);

    rc = ble_br_hfp_hf_hangup(2);
    TEST_ASSERT(rc == 0);
    ble_br_hfp_hf_test_util_verify_tx("AT+CHUP\r");

    ble_hs_test_util_hci_ack_set_disconnect(0);
    TEST_ASSERT(ble_br_test_util_time_advance(timeout_ticks) == BLE_HS_FOREVER);

    ble_hs_test_util_hci_verify_tx_disconnect(BLE_BR_HFP_HF_TEST_SCO_HANDLE,
                                              BLE_ERR_REM_USER_CONN_TERM);

    event = ble_br_hfp_hf_test_util_event_next();
    TEST_ASSERT(event->type == BLE_BR_HFP_HF_EVENT_AUDIO_DISCONNECT);
    TEST_ASSERT(event->audio_disconnect.sco_handle == BLE_BR_HFP_HF_TEST_SCO_HANDLE);
    ble_br_hfp_hf_test_util_verify_event(BLE_BR_HFP_HF_EVENT_DISCONNECT);
    ble_br_hfp_hf_test_util_verify_no_event();
    ble_br_hfp_hf_test_util_verify_tx_close();

    ble_hs_test_util_hci_rx_disconn_complete_event(BLE_BR_HFP_HF_TEST_SCO_HANDLE,
                                                   0, BLE_ERR_CONN_TERM_LOCAL);
    ble_br_hfp_hf_test_util_verify_no_event();

    /*** The connection is lost while audio is up. */
    ble_br_hfp_hf_test_util_connect(BLE_BR_HFP_HF_TEST_AG_FEAT_S4);
    ble_br_hfp_hf_test_util_rx_audio(BLE_HCI_LINK_TYPE_ESCO, &params);

    ble_hs_test_util_hci_rx_disconn_complete_event(2, 0, BLE_ERR_CONN_SPVN_TMO);

    /* Nothing is asked of the controller: the links are gone. */
    TEST_ASSERT(ble_hs_test_util_hci_out_first() == NULL);

    event = ble_br_hfp_hf_test_util_event_next();
    TEST_ASSERT(event->type == BLE_BR_HFP_HF_EVENT_AUDIO_DISCONNECT);
    TEST_ASSERT(event->audio_disconnect.sco_handle == BLE_BR_HFP_HF_TEST_SCO_HANDLE);
    ble_br_hfp_hf_test_util_verify_event(BLE_BR_HFP_HF_EVENT_DISCONNECT);
    ble_br_hfp_hf_test_util_verify_no_event();

    ble_br_test_util_verify_tx_none();
    ble_hs_test_util_assert_mbufs_freed(NULL);
}

#endif

TEST_SUITE(ble_br_hfp_hf_test_suite)
{
#if MYNEWT_VAL(BLE_BREDR_HFP_HF)
    ble_br_hfp_hf_test_slc();
    ble_br_hfp_hf_test_slc_fail();
    ble_br_hfp_hf_test_indicators();
    ble_br_hfp_hf_test_ring_clip();
    ble_br_hfp_hf_test_cmds();
    ble_br_hfp_hf_test_cmd_timeout();
    ble_br_hfp_hf_test_fragmented();
    ble_br_hfp_hf_test_disconnect();
    ble_br_hfp_hf_test_outgoing();
    ble_br_hfp_hf_test_outgoing_fail();
    ble_br_hfp_hf_test_audio();
    ble_br_hfp_hf_test_audio_close();
#endif
}
