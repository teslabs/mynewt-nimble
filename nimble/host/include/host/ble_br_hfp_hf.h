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

#ifndef H_BLE_BR_HFP_HF_
#define H_BLE_BR_HFP_HF_

/**
 * @brief Bluetooth Host Hands-Free Profile, Hands-Free role
 * @defgroup bt_host_br_hfp_hf Bluetooth Host Hands-Free Profile, Hands-Free role
 * @ingroup bt_host
 *
 * Call control with one audio gateway, a phone, and the audio connection of
 * its calls, with the CVSD codec. The audio itself goes through the
 * synchronous data API.
 * @{
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Longest phone number, in characters. */
#define BLE_BR_HFP_HF_NUMBER_MAX_LEN 32

/** Largest speaker gain. */
#define BLE_BR_HFP_HF_VOLUME_MAX 15

/**
 * @defgroup ble_br_hfp_hf_indicators Indicators of the audio gateway
 * @{
 */
/** Whether there is phone service: 0 or 1. */
#define BLE_BR_HFP_HF_IND_SERVICE 0
/** Whether a call is active: 0 or 1. */
#define BLE_BR_HFP_HF_IND_CALL 1
/** Call being set up: 0 none, 1 incoming, 2 outgoing, 3 remote alerted. */
#define BLE_BR_HFP_HF_IND_CALLSETUP 2
/** Held calls: 0 none, 1 held and active, 2 held only. */
#define BLE_BR_HFP_HF_IND_CALLHELD 3
/** Signal strength: 0 to 5. */
#define BLE_BR_HFP_HF_IND_SIGNAL 4
/** Whether the phone roams: 0 or 1. */
#define BLE_BR_HFP_HF_IND_ROAM 5
/** Battery charge of the phone: 0 to 5. */
#define BLE_BR_HFP_HF_IND_BATTCHG 6
/** Number of indicators. */
#define BLE_BR_HFP_HF_IND_COUNT 7
/** @} */

/**
 * @defgroup ble_br_hfp_hf_events Hands-Free events
 * @{
 */
/** The service level connection is up, or could not be set up. */
#define BLE_BR_HFP_HF_EVENT_CONNECT 0
/** The service level connection ended. */
#define BLE_BR_HFP_HF_EVENT_DISCONNECT 1
/** An indicator of the audio gateway changed. */
#define BLE_BR_HFP_HF_EVENT_INDICATOR 2
/** An incoming call rings. */
#define BLE_BR_HFP_HF_EVENT_RING 3
/** Number of the calling party. */
#define BLE_BR_HFP_HF_EVENT_CLIP 4
/** The audio connection is up, or could not be set up. */
#define BLE_BR_HFP_HF_EVENT_AUDIO_CONNECT 5
/** The audio connection ended. */
#define BLE_BR_HFP_HF_EVENT_AUDIO_DISCONNECT 6
/** The audio gateway set the speaker gain. */
#define BLE_BR_HFP_HF_EVENT_VOLUME 7
/** The audio gateway answered a command. */
#define BLE_BR_HFP_HF_EVENT_CMD_COMPLETE 8
/** @} */

/** A Hands-Free event. */
struct ble_br_hfp_hf_event {
    /** Which event, a BLE_BR_HFP_HF_EVENT code. */
    uint8_t type;
    /** ACL connection of the audio gateway. */
    uint16_t conn_handle;

    union {
        /** BLE_BR_HFP_HF_EVENT_CONNECT. */
        struct {
            /** 0 on success, a host error code otherwise. */
            int status;
        } connect;

        /** BLE_BR_HFP_HF_EVENT_INDICATOR. */
        struct {
            /** Which indicator, a BLE_BR_HFP_HF_IND code. */
            uint8_t id;
            /** Its value. */
            uint8_t value;
        } indicator;

        /** BLE_BR_HFP_HF_EVENT_CLIP. */
        struct {
            /** The number, empty if withheld. */
            char number[BLE_BR_HFP_HF_NUMBER_MAX_LEN + 1];
        } clip;

        /** BLE_BR_HFP_HF_EVENT_AUDIO_CONNECT. */
        struct {
            /** 0 on success, a host error code otherwise. */
            int status;
            /** Synchronous connection handle, valid on success. */
            uint16_t sco_handle;
        } audio_connect;

        /** BLE_BR_HFP_HF_EVENT_AUDIO_DISCONNECT. */
        struct {
            /** Synchronous connection handle. */
            uint16_t sco_handle;
        } audio_disconnect;

        /** BLE_BR_HFP_HF_EVENT_VOLUME. */
        struct {
            /** Speaker gain, 0 to BLE_BR_HFP_HF_VOLUME_MAX. */
            uint8_t gain;
        } volume;

        /** BLE_BR_HFP_HF_EVENT_CMD_COMPLETE. */
        struct {
            /** 0 if the audio gateway accepted the command. */
            int status;
        } cmd_complete;
    };
};

/**
 * Hands-Free event callback, called from the host task.
 *
 * @param event                 The event.
 * @param arg                   Argument given on registration.
 */
typedef void ble_br_hfp_hf_event_fn(const struct ble_br_hfp_hf_event *event,
                                    void *arg);

/**
 * Register the Hands-Free service, which audio gateways can then connect
 * to. Call it once, before the host starts.
 *
 * @param cb                    The callback that receives the events.
 * @param cb_arg                Its argument.
 *
 * @return                      0 on success, a host error code otherwise.
 */
int ble_br_hfp_hf_init(ble_br_hfp_hf_event_fn *cb, void *cb_arg);

/**
 * Connect to the audio gateway of a peer, securing the connection first if
 * needed. The result comes with BLE_BR_HFP_HF_EVENT_CONNECT.
 *
 * @param conn_handle           ACL connection to the peer.
 *
 * @return                      0 on success, a host error code otherwise.
 */
int ble_br_hfp_hf_connect(uint16_t conn_handle);

/**
 * End the service level connection.
 *
 * @param conn_handle           ACL connection of the audio gateway.
 *
 * @return                      0 on success, a host error code otherwise.
 */
int ble_br_hfp_hf_disconnect(uint16_t conn_handle);

/**
 * Answer the incoming call.
 *
 * @param conn_handle           ACL connection of the audio gateway.
 *
 * @return                      0 on success, a host error code otherwise.
 */
int ble_br_hfp_hf_answer(uint16_t conn_handle);

/**
 * Reject the incoming call, or end the one set up or active.
 *
 * @param conn_handle           ACL connection of the audio gateway.
 *
 * @return                      0 on success, a host error code otherwise.
 */
int ble_br_hfp_hf_hangup(uint16_t conn_handle);

/**
 * Place a call.
 *
 * @param conn_handle           ACL connection of the audio gateway.
 * @param number                The number to call, at most
 *                              BLE_BR_HFP_HF_NUMBER_MAX_LEN characters.
 *
 * @return                      0 on success, a host error code otherwise.
 */
int ble_br_hfp_hf_dial(uint16_t conn_handle, const char *number);

/**
 * Tell the audio gateway the speaker gain.
 *
 * @param conn_handle           ACL connection of the audio gateway.
 * @param gain                  The gain, 0 to BLE_BR_HFP_HF_VOLUME_MAX.
 *
 * @return                      0 on success, a host error code otherwise.
 */
int ble_br_hfp_hf_set_volume(uint16_t conn_handle, uint8_t gain);

/**
 * Take the audio of the call. The result comes with
 * BLE_BR_HFP_HF_EVENT_AUDIO_CONNECT.
 *
 * @param conn_handle           ACL connection of the audio gateway.
 *
 * @return                      0 on success, a host error code otherwise.
 */
int ble_br_hfp_hf_audio_connect(uint16_t conn_handle);

/**
 * Give the audio of the call back to the audio gateway.
 *
 * @param conn_handle           ACL connection of the audio gateway.
 *
 * @return                      0 on success, a host error code otherwise.
 */
int ble_br_hfp_hf_audio_disconnect(uint16_t conn_handle);

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif
