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

#ifndef H_BLE_BR_GAP_
#define H_BLE_BR_GAP_

/**
 * @brief Bluetooth Host BR/EDR Generic Access Profile (GAP)
 * @defgroup bt_host_br_gap Bluetooth Host BR/EDR Generic Access Profile (GAP)
 * @ingroup bt_host
 * @{
 */

#include <stdbool.h>
#include <stdint.h>
#include "nimble/ble.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Length of a BR/EDR link key, in bytes. */
#define BLE_BR_GAP_LINK_KEY_LEN 16

/** Longest device name, in bytes. */
#define BLE_BR_GAP_NAME_MAX_LEN 31

/**
 * @defgroup ble_br_gap_scan_modes BR/EDR scan modes
 * @{
 */
/** Neither discoverable nor connectable. */
#define BLE_BR_GAP_SCAN_NONE 0x00
/** Discoverable: answers inquiries. */
#define BLE_BR_GAP_SCAN_INQUIRY 0x01
/** Connectable: answers pages. */
#define BLE_BR_GAP_SCAN_PAGE 0x02
/** @} */

/**
 * @defgroup ble_br_gap_events BR/EDR GAP events
 * @{
 */
/** An ACL connection attempt, incoming or outgoing, completed. */
#define BLE_BR_GAP_EVENT_CONNECT 0
/** An ACL connection ended. */
#define BLE_BR_GAP_EVENT_DISCONNECT 1
/** Encryption of a connection changed. */
#define BLE_BR_GAP_EVENT_ENC_CHANGE 2
/** The peer asks to confirm a numeric value. */
#define BLE_BR_GAP_EVENT_USER_CONFIRM 3
/** Pairing completed. */
#define BLE_BR_GAP_EVENT_PAIRING_COMPLETE 4
/** A link key was created or replaced. */
#define BLE_BR_GAP_EVENT_LINK_KEY 5
/** A connection entered or left sniff mode. */
#define BLE_BR_GAP_EVENT_MODE_CHANGE 6
/** @} */

/** A BR/EDR GAP event. */
struct ble_br_gap_event {
    /** Which event, a BLE_BR_GAP_EVENT code. */
    uint8_t type;

    union {
        /** BLE_BR_GAP_EVENT_CONNECT. */
        struct {
            /** 0 on success, a host error code otherwise. */
            int status;
            /** Connection handle, valid on success. */
            uint16_t conn_handle;
            /** Address of the peer. */
            uint8_t addr[BLE_DEV_ADDR_LEN];
        } connect;

        /** BLE_BR_GAP_EVENT_DISCONNECT. */
        struct {
            /** Connection handle. */
            uint16_t conn_handle;
            /** Why, as a host error code. */
            int reason;
            /** Address of the peer. */
            uint8_t addr[BLE_DEV_ADDR_LEN];
        } disconnect;

        /** BLE_BR_GAP_EVENT_ENC_CHANGE. */
        struct {
            /** 0 on success, a host error code otherwise. */
            int status;
            /** Connection handle. */
            uint16_t conn_handle;
            /** Whether the connection is now encrypted. */
            bool encrypted;
        } enc_change;

        /**
         * BLE_BR_GAP_EVENT_USER_CONFIRM. Answer with
         * ble_br_gap_user_confirm().
         */
        struct {
            /** Address of the peer. */
            uint8_t addr[BLE_DEV_ADDR_LEN];
            /** The six digit value both devices show. */
            uint32_t numeric_value;
        } user_confirm;

        /** BLE_BR_GAP_EVENT_PAIRING_COMPLETE. */
        struct {
            /** 0 on success, a host error code otherwise. */
            int status;
            /** Address of the peer. */
            uint8_t addr[BLE_DEV_ADDR_LEN];
        } pairing_complete;

        /** BLE_BR_GAP_EVENT_LINK_KEY. */
        struct {
            /** Address of the peer. */
            uint8_t addr[BLE_DEV_ADDR_LEN];
            /** The key. */
            uint8_t key[BLE_BR_GAP_LINK_KEY_LEN];
            /** Its type, as the controller reports it. */
            uint8_t key_type;
        } link_key;

        /** BLE_BR_GAP_EVENT_MODE_CHANGE. */
        struct {
            /** 0 on success, a host error code otherwise. */
            int status;
            /** Connection handle. */
            uint16_t conn_handle;
            /** Whether the connection is now in sniff mode. */
            bool sniff;
            /** The sniff interval, in units of 0.625 ms. */
            uint16_t interval;
        } mode_change;
    };
};

/**
 * BR/EDR GAP event callback, called from the host task.
 *
 * @param event                 The event.
 * @param arg                   Argument given on registration.
 */
typedef void ble_br_gap_event_fn(const struct ble_br_gap_event *event, void *arg);

/**
 * Set the callback that receives BR/EDR GAP events.
 *
 * @param cb                    The callback, or NULL for none.
 * @param cb_arg                Its argument.
 */
void ble_br_gap_set_event_cb(ble_br_gap_event_fn *cb, void *cb_arg);

/**
 * Tell whether the controller supports BR/EDR. Valid once the host is
 * synchronized.
 *
 * @return                      true if it does.
 */
bool ble_br_gap_supported(void);

/**
 * Set the device name, given to peers on request and in the inquiry
 * response. It may be set before the host is synchronized.
 *
 * @param name                  The name, at most BLE_BR_GAP_NAME_MAX_LEN
 *                              bytes long.
 *
 * @return                      0 on success, a host error code otherwise.
 */
int ble_br_gap_set_name(const char *name);

/**
 * Set the class of device. It may be set before the host is synchronized.
 *
 * @param cod                   The class of device.
 *
 * @return                      0 on success, a host error code otherwise.
 */
int ble_br_gap_set_class_of_device(uint32_t cod);

/**
 * Set whether the device is discoverable and connectable. It may be set
 * before the host is synchronized.
 *
 * @param mode                  A combination of BLE_BR_GAP_SCAN flags.
 *
 * @return                      0 on success, a host error code otherwise.
 */
int ble_br_gap_set_scan_mode(uint8_t mode);

/**
 * Connect to a peer. The result comes with BLE_BR_GAP_EVENT_CONNECT.
 *
 * @param addr                  Address of the peer.
 *
 * @return                      0 on success, a host error code otherwise.
 */
int ble_br_gap_connect(const uint8_t *addr);

/**
 * Terminate a connection.
 *
 * @param conn_handle           The connection.
 * @param hci_reason            The HCI error code given to the peer.
 *
 * @return                      0 on success, a host error code otherwise.
 */
int ble_br_gap_terminate(uint16_t conn_handle, uint8_t hci_reason);

/**
 * Put a connection in sniff mode, in which the devices only meet every
 * sniff interval, to save power while it carries little. The peer may ask
 * for the same: it is accepted. The result comes with
 * BLE_BR_GAP_EVENT_MODE_CHANGE.
 *
 * @param conn_handle           The connection.
 * @param min_interval          Shortest sniff interval, in units of 0.625 ms,
 *                              an even number.
 * @param max_interval          Longest sniff interval, likewise.
 * @param attempt               Number of receive slots at each interval.
 * @param timeout               Number of receive slots the devices stay for
 *                              after one that carried data.
 *
 * @return                      0 on success, a host error code otherwise.
 */
int ble_br_gap_sniff(uint16_t conn_handle, uint16_t min_interval,
                     uint16_t max_interval, uint16_t attempt, uint16_t timeout);

/**
 * Take a connection out of sniff mode. The result comes with
 * BLE_BR_GAP_EVENT_MODE_CHANGE.
 *
 * @param conn_handle           The connection.
 *
 * @return                      0 on success, a host error code otherwise.
 */
int ble_br_gap_exit_sniff(uint16_t conn_handle);

/**
 * Set what the device accepts of sniff subrating, with which the devices
 * skip sniff intervals while the connection carries nothing.
 *
 * @param conn_handle           The connection.
 * @param max_latency           Longest time between two intervals the
 *                              devices meet at, in units of 0.625 ms.
 * @param min_remote_timeout    Shortest time the peer has to keep meeting at
 *                              each interval after data, in units of
 *                              0.625 ms.
 * @param min_local_timeout     The same for the device itself.
 *
 * @return                      0 on success, a host error code otherwise.
 */
int ble_br_gap_sniff_subrating(uint16_t conn_handle, uint16_t max_latency,
                               uint16_t min_remote_timeout,
                               uint16_t min_local_timeout);

/**
 * Authenticate and encrypt a connection, pairing if there is no link key
 * for the peer. The result comes with BLE_BR_GAP_EVENT_ENC_CHANGE.
 *
 * @param conn_handle           The connection.
 *
 * @return                      0 on success, a host error code otherwise.
 */
int ble_br_gap_security_initiate(uint16_t conn_handle);

/**
 * Answer BLE_BR_GAP_EVENT_USER_CONFIRM.
 *
 * @param addr                  Address of the peer.
 * @param accept                Whether the values match.
 *
 * @return                      0 on success, a host error code otherwise.
 */
int ble_br_gap_user_confirm(const uint8_t *addr, bool accept);

/**
 * Give the host the link key of a peer, e.g. one persisted from
 * BLE_BR_GAP_EVENT_LINK_KEY.
 *
 * @param addr                  Address of the peer.
 * @param key                   The key.
 * @param key_type              Its type.
 *
 * @return                      0 on success, a host error code otherwise.
 */
int ble_br_gap_set_link_key(const uint8_t *addr, const uint8_t *key, uint8_t key_type);

/**
 * Forget the link key of a peer.
 *
 * @param addr                  Address of the peer.
 *
 * @return                      0 on success, BLE_HS_ENOENT if there is none.
 */
int ble_br_gap_delete_link_key(const uint8_t *addr);

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif
