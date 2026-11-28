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

#ifndef H_BLE_BR_SDP_PRIV_
#define H_BLE_BR_SDP_PRIV_

#ifdef __cplusplus
extern "C" {
#endif

/* Data element header: type in the upper bits, size index in the lower */
#define BLE_BR_SDP_DE_TYPE_MASK  0xf8
#define BLE_BR_SDP_DE_SIZE_MASK  0x07
#define BLE_BR_SDP_DE_SIZE_NEXT1 0x05

#define BLE_BR_SDP_DE_NIL  0x00
#define BLE_BR_SDP_DE_UINT 0x08
#define BLE_BR_SDP_DE_UUID 0x18
#define BLE_BR_SDP_DE_TEXT 0x20
#define BLE_BR_SDP_DE_SEQ  0x30

/* Headers of the elements used here */
#define BLE_BR_SDP_DE_UINT8  (BLE_BR_SDP_DE_UINT | 0)
#define BLE_BR_SDP_DE_UINT16 (BLE_BR_SDP_DE_UINT | 1)
#define BLE_BR_SDP_DE_UINT32 (BLE_BR_SDP_DE_UINT | 2)
#define BLE_BR_SDP_DE_UUID16 (BLE_BR_SDP_DE_UUID | 1)
/* Followed by a length byte */
#define BLE_BR_SDP_DE_TEXT8 (BLE_BR_SDP_DE_TEXT | 5)
#define BLE_BR_SDP_DE_SEQ8  (BLE_BR_SDP_DE_SEQ | 5)

#define BLE_BR_SDP_ATTR_RECORD_HANDLE      0x0000
#define BLE_BR_SDP_ATTR_SERVICE_CLASS_LIST 0x0001
#define BLE_BR_SDP_ATTR_PROTOCOL_DESC_LIST 0x0004
#define BLE_BR_SDP_ATTR_BROWSE_GROUP_LIST  0x0005
#define BLE_BR_SDP_ATTR_PROFILE_DESC_LIST  0x0009
#define BLE_BR_SDP_ATTR_SERVICE_NAME       0x0100
#define BLE_BR_SDP_ATTR_SUPPORTED_FEATURES 0x0311

#define BLE_BR_SDP_UUID_RFCOMM             0x0003
#define BLE_BR_SDP_UUID_L2CAP              0x0100
#define BLE_BR_SDP_UUID_PUBLIC_BROWSE_ROOT 0x1002
#define BLE_BR_SDP_UUID_HANDSFREE          0x111e
#define BLE_BR_SDP_UUID_HANDSFREE_AG       0x111f
#define BLE_BR_SDP_UUID_GENERIC_AUDIO      0x1203

/* Splits a 16-bit value in the two bytes it takes in a record */
#define BLE_BR_SDP_BE16(val) ((val) >> 8) & 0xff, (val) & 0xff

#ifdef __cplusplus
}
#endif

#endif
