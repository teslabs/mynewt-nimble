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

#ifndef H_BLE_BR_SCO_
#define H_BLE_BR_SCO_

/**
 * @brief Bluetooth Host BR/EDR synchronous data
 * @defgroup bt_host_br_sco Bluetooth Host BR/EDR synchronous data
 * @ingroup bt_host
 *
 * Audio of a synchronous connection, which a profile sets up, as 16-bit
 * signed samples at 8 kHz.
 * @{
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Largest payload of a synchronous data packet, in bytes. */
#define BLE_BR_SCO_MAX_DATA_LEN 255

/**
 * @defgroup ble_br_sco_status Status of received synchronous data
 * @{
 */
/** The data is good. */
#define BLE_BR_SCO_STATUS_GOOD 0
/** The data may be invalid. */
#define BLE_BR_SCO_STATUS_INVALID 1
/** Nothing was received. */
#define BLE_BR_SCO_STATUS_NONE 2
/** Part of the data was lost. */
#define BLE_BR_SCO_STATUS_PARTIAL 3
/** @} */

/**
 * Callback for received synchronous data, called from the context of the
 * HCI transport.
 *
 * @param sco_handle            The synchronous connection.
 * @param data                  The data, only valid during the call.
 * @param len                   Its length, in bytes.
 * @param status                A BLE_BR_SCO_STATUS code.
 * @param arg                   Argument given on registration.
 */
typedef void ble_br_sco_rx_fn(uint16_t sco_handle, const uint8_t *data,
                              uint16_t len, uint8_t status, void *arg);

/**
 * Set the callback that receives synchronous data.
 *
 * @param cb                    The callback, or NULL for none.
 * @param cb_arg                Its argument.
 */
void ble_br_sco_set_rx_cb(ble_br_sco_rx_fn *cb, void *cb_arg);

/**
 * Send synchronous data.
 *
 * @param sco_handle            The synchronous connection.
 * @param data                  The data.
 * @param len                   Its length, at most BLE_BR_SCO_MAX_DATA_LEN.
 *
 * @return                      0 on success, a host error code otherwise.
 */
int ble_br_sco_tx(uint16_t sco_handle, const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif
