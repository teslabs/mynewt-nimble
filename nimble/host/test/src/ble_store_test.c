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

#include "testutil/testutil.h"
#include "ble_hs_test.h"
#include "ble_hs_test_util.h"

static struct ble_store_status_event ble_store_test_status_event;

static void
ble_store_test_util_verify_peer_deleted(const ble_addr_t *addr)
{
    union ble_store_value value;
    union ble_store_key key;
    ble_addr_t addrs[64];
    int num_addrs;
    int rc;
    int i;

    memset(&key, 0, sizeof key);
    key.sec.peer_addr = *addr;
    rc = ble_store_read(BLE_STORE_OBJ_TYPE_OUR_SEC, &key, &value);
    TEST_ASSERT(rc == BLE_HS_ENOENT);
    rc = ble_store_read(BLE_STORE_OBJ_TYPE_PEER_SEC, &key, &value);
    TEST_ASSERT(rc == BLE_HS_ENOENT);

    memset(&key, 0, sizeof key);
    key.cccd.peer_addr = *addr;
    rc = ble_store_read(BLE_STORE_OBJ_TYPE_CCCD, &key, &value);
    TEST_ASSERT(rc == BLE_HS_ENOENT);

    rc = ble_store_util_bonded_peers(addrs, &num_addrs,
                                     sizeof addrs / sizeof addrs[0]);
    TEST_ASSERT_FATAL(rc == 0);
    for (i = 0; i < num_addrs; i++) {
        TEST_ASSERT(ble_addr_cmp(addr, addrs + i) != 0);
    }
}

static int
ble_store_test_util_status_overflow(struct ble_store_status_event *event,
                                    void *arg)
{
    int *status;

    status = arg;

    ble_store_test_status_event = *event;
    return *status;
}

static void
ble_store_test_util_overflow_sec(int is_our_sec)
{
    union ble_store_value val;
    int obj_type;
    int status;
    int rc;
    int i;

    ble_hs_test_util_init();

    ble_hs_cfg.store_status_cb = ble_store_test_util_status_overflow;
    ble_hs_cfg.store_status_arg = &status;

    if (is_our_sec) {
        obj_type = BLE_STORE_OBJ_TYPE_OUR_SEC;
    } else {
        obj_type = BLE_STORE_OBJ_TYPE_PEER_SEC;
    }

    memset(&ble_store_test_status_event, 0,
           sizeof ble_store_test_status_event);
    memset(&val, 0, sizeof val);

    val.sec.peer_addr =
        (ble_addr_t){ BLE_ADDR_PUBLIC, { 1, 2, 3, 4, 5, 6 } };
    val.sec.ltk_present = 1,

    status = BLE_HS_ESTORE_CAP;
    for (i = 0; ; i++) {
        rc = ble_store_write(obj_type, &val);
        if (i < MYNEWT_VAL(BLE_STORE_MAX_BONDS)) {
            TEST_ASSERT_FATAL(rc == 0);
        } else {
            /* This record should have caused an overflow. */
            TEST_ASSERT(rc == BLE_HS_ESTORE_CAP);
            TEST_ASSERT(ble_store_test_status_event.event_code ==
                        BLE_STORE_EVENT_OVERFLOW);
            TEST_ASSERT(ble_store_test_status_event.overflow.obj_type ==
                        obj_type);
            TEST_ASSERT(ble_store_test_status_event.overflow.value == &val);
            break;
        }

        val.sec.peer_addr.val[0]++;
    }
}

static int
ble_store_test_util_count(int obj_type)
{
    int count;
    int rc;

    rc = ble_store_util_count(obj_type, &count);
    TEST_ASSERT_FATAL(rc == 0);

    return count;
}

TEST_CASE_SELF(ble_store_test_peers)
{
    struct ble_store_value_sec secs[3] = {
        {
            .peer_addr = { BLE_ADDR_PUBLIC,     { 1, 2, 3, 4, 5, 6 } },
            .ltk_present = 1,
        },
        {
            /* Address value is a duplicate of above, but type differs. */
            .peer_addr = { BLE_ADDR_RANDOM,     { 1, 2, 3, 4, 5, 6 } },
            .ltk_present = 1,
        },
        {
            .peer_addr = { BLE_ADDR_PUBLIC,     { 2, 3, 4, 5, 6, 7 } },
            .ltk_present = 1,
        },
    };
    ble_addr_t peer_addrs[3];
    int num_addrs;
    int rc;
    int i;

    ble_hs_test_util_init();

    for (i = 0; i < sizeof secs / sizeof secs[0]; i++) {
        rc = ble_store_write_our_sec(secs + i);
        TEST_ASSERT_FATAL(rc == 0);
        rc = ble_store_write_peer_sec(secs + i);
        TEST_ASSERT_FATAL(rc == 0);
    }

    rc = ble_store_util_bonded_peers(peer_addrs, &num_addrs,
                                     sizeof peer_addrs / sizeof peer_addrs[0]);
    TEST_ASSERT_FATAL(rc == 0);

    TEST_ASSERT(num_addrs == sizeof secs / sizeof secs[0]);
    for (i = 0; i < num_addrs; i++) {
        TEST_ASSERT(ble_addr_cmp(&peer_addrs[i], &secs[i].peer_addr) == 0);
    }

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_store_test_delete_peer)
{
    struct ble_store_value_sec secs[2] = {
        {
            .peer_addr = { BLE_ADDR_PUBLIC,     { 1, 2, 3, 4, 5, 6 } },
            .ltk_present = 1,
        },
        {
            /* Address value is a duplicate of above, but type differs. */
            .peer_addr = { BLE_ADDR_RANDOM,     { 1, 2, 3, 4, 5, 6 } },
            .ltk_present = 1,
        },
    };
    struct ble_store_value_cccd cccds[3] = {
        /* First two belong to first peer. */
        {
            .peer_addr = secs[0].peer_addr,
            .chr_val_handle = 5,
        },
        {
            .peer_addr = secs[0].peer_addr,
            .chr_val_handle = 8,
        },

        /* Last belongs to second peer. */
        {
            .peer_addr = secs[1].peer_addr,
            .chr_val_handle = 5,
        },
    };
    union ble_store_value value;
    union ble_store_key key;
    int count;
    int rc;
    int i;

    ble_hs_test_util_init();

    for (i = 0; i < sizeof secs / sizeof secs[0]; i++) {
        rc = ble_store_write_our_sec(secs + i);
        TEST_ASSERT_FATAL(rc == 0);
        rc = ble_store_write_peer_sec(secs + i);
        TEST_ASSERT_FATAL(rc == 0);
    }

    for (i = 0; i < sizeof cccds / sizeof cccds[0]; i++) {
        rc = ble_store_write_cccd(cccds + i);
        TEST_ASSERT_FATAL(rc == 0);
    }

    /* Delete first peer. */
    rc = ble_store_util_delete_peer(&secs[0].peer_addr);
    TEST_ASSERT_FATAL(rc == 0);

    /* Ensure all traces of first peer have been removed. */
    ble_store_test_util_verify_peer_deleted(&secs[0].peer_addr);

    /* Ensure second peer data is still intact. */
    ble_store_key_from_value_sec(&key.sec, secs + 1);

    rc = ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &count);
    TEST_ASSERT_FATAL(rc == 0);
    TEST_ASSERT(count == 1);

    rc = ble_store_read_our_sec(&key.sec, &value.sec);
    TEST_ASSERT_FATAL(rc == 0);
    TEST_ASSERT(memcmp(&value.sec, secs + 1, sizeof value.sec) == 0);

    rc = ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &count);
    TEST_ASSERT_FATAL(rc == 0);
    TEST_ASSERT(count == 1);

    rc = ble_store_read_peer_sec(&key.sec, &value.sec);
    TEST_ASSERT_FATAL(rc == 0);
    TEST_ASSERT(memcmp(&value.sec, secs + 1, sizeof value.sec) == 0);

    ble_store_key_from_value_cccd(&key.cccd, cccds + 2);

    rc = ble_store_util_count(BLE_STORE_OBJ_TYPE_CCCD, &count);
    TEST_ASSERT_FATAL(rc == 0);
    TEST_ASSERT(count == 1);

    rc = ble_store_read_cccd(&key.cccd, &value.cccd);
    TEST_ASSERT_FATAL(rc == 0);
    TEST_ASSERT(memcmp(&value.cccd, cccds + 2, sizeof value.cccd) == 0);

    /* Delete second peer. */
    rc = ble_store_util_delete_peer(&secs[1].peer_addr);
    TEST_ASSERT_FATAL(rc == 0);

    /* Ensure all traces of first peer have been removed. */
    ble_store_test_util_verify_peer_deleted(&secs[1].peer_addr);

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_store_test_count)
{
    struct ble_store_value_sec secs[4] = {
        {
            .peer_addr = { BLE_ADDR_PUBLIC,     { 1, 2, 3, 4, 5, 6 } },
            .ltk_present = 1,
        },
        {
            .peer_addr = { BLE_ADDR_RANDOM,     { 1, 2, 3, 4, 5, 6 } },
            .ltk_present = 1,
        },
        {
            .peer_addr = { BLE_ADDR_PUBLIC,     { 2, 3, 4, 5, 6, 7 } },
            .ltk_present = 1,
        },
        {
            .peer_addr = { BLE_ADDR_RANDOM,     { 3, 4, 5, 6, 7, 8 } },
            .ltk_present = 1,
        },
    };
    struct ble_store_value_cccd cccds[2] = {
        {
            .peer_addr = secs[0].peer_addr,
            .chr_val_handle = 5,
        },
        {
            .peer_addr = secs[0].peer_addr,
            .chr_val_handle = 8,
        },
    };
    int count;
    int rc;
    int i;

    ble_hs_test_util_init();

    /*** Verify initial counts are 0. */

    rc = ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &count);
    TEST_ASSERT_FATAL(rc == 0);
    TEST_ASSERT(count == 0);

    rc = ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &count);
    TEST_ASSERT_FATAL(rc == 0);
    TEST_ASSERT(count == 0);

    rc = ble_store_util_count(BLE_STORE_OBJ_TYPE_CCCD, &count);
    TEST_ASSERT_FATAL(rc == 0);
    TEST_ASSERT(count == 0);

    /* Write some test data. */

    for (i = 0; i < 3; i++) {
        rc = ble_store_write_our_sec(secs + i);
        TEST_ASSERT_FATAL(rc == 0);
    }
    for (i = 0; i < 2; i++) {
        rc = ble_store_write_peer_sec(secs + i);
        TEST_ASSERT_FATAL(rc == 0);
    }
    for (i = 0; i < 1; i++) {
        rc = ble_store_write_cccd(cccds + i);
        TEST_ASSERT_FATAL(rc == 0);
    }

    /*** Verify counts after populating store. */
    rc = ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &count);
    TEST_ASSERT_FATAL(rc == 0);
    TEST_ASSERT(count == 3);

    rc = ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &count);
    TEST_ASSERT_FATAL(rc == 0);
    TEST_ASSERT(count == 2);

    rc = ble_store_util_count(BLE_STORE_OBJ_TYPE_CCCD, &count);
    TEST_ASSERT_FATAL(rc == 0);
    TEST_ASSERT(count == 1);

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_store_test_overflow)
{
    ble_store_test_util_overflow_sec(0);
    ble_store_test_util_overflow_sec(1);

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_store_test_clear)
{
    const struct ble_store_value_sec secs[2] = {
        {
            .peer_addr = { BLE_ADDR_PUBLIC,     { 1, 2, 3, 4, 5, 6 } },
            .ltk_present = 1,
        },
        {
            /* Address value is a duplicate of above, but type differs. */
            .peer_addr = { BLE_ADDR_RANDOM,     { 1, 2, 3, 4, 5, 6 } },
            .ltk_present = 1,
        },
    };
    const struct ble_store_value_cccd cccds[3] = {
        /* First two belong to first peer. */
        {
            .peer_addr = secs[0].peer_addr,
            .chr_val_handle = 5,
        },
        {
            .peer_addr = secs[0].peer_addr,
            .chr_val_handle = 8,
        },

        /* Last belongs to second peer. */
        {
            .peer_addr = secs[1].peer_addr,
            .chr_val_handle = 5,
        },
    };
    int rc;
    int i;

    ble_hs_test_util_init();

    for (i = 0; i < sizeof secs / sizeof secs[0]; i++) {
        rc = ble_store_write_our_sec(secs + i);
        TEST_ASSERT_FATAL(rc == 0);
        rc = ble_store_write_peer_sec(secs + i);
        TEST_ASSERT_FATAL(rc == 0);
    }

    for (i = 0; i < sizeof cccds / sizeof cccds[0]; i++) {
        rc = ble_store_write_cccd(cccds + i);
        TEST_ASSERT_FATAL(rc == 0);
    }

    /* Sanity check. */
    TEST_ASSERT_FATAL(
        ble_store_test_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC) == 2);
    TEST_ASSERT_FATAL(
        ble_store_test_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC) == 2);
    TEST_ASSERT_FATAL(
        ble_store_test_util_count(BLE_STORE_OBJ_TYPE_CCCD) == 3);

    /* Ensure store is empty after clear gets called. */
    rc = ble_store_clear();
    TEST_ASSERT_FATAL(rc == 0);
    TEST_ASSERT(ble_store_test_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC) == 0);
    TEST_ASSERT(ble_store_test_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC) == 0);
    TEST_ASSERT(ble_store_test_util_count(BLE_STORE_OBJ_TYPE_CCCD) == 0);

    /* Ensure second clear succeeds with no effect. */
    rc = ble_store_clear();
    TEST_ASSERT_FATAL(rc == 0);
    TEST_ASSERT(ble_store_test_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC) == 0);
    TEST_ASSERT(ble_store_test_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC) == 0);
    TEST_ASSERT(ble_store_test_util_count(BLE_STORE_OBJ_TYPE_CCCD) == 0);

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

#if MYNEWT_VAL(BLE_BREDR)
/**
 * Makes up a link key for the peer whose public address starts with the
 * specified byte.
 */
static void
ble_store_test_util_br_link_key(struct ble_store_value_br_link_key *value,
                                uint8_t addr0, uint8_t key_type)
{
    int i;

    memset(value, 0, sizeof *value);

    value->peer_addr.type = BLE_ADDR_PUBLIC;
    value->peer_addr.val[0] = addr0;
    for (i = 0; i < sizeof value->key; i++) {
        value->key[i] = addr0 + i;
    }
    value->key_type = key_type;
}

static void
ble_store_test_util_verify_br_link_key(const struct ble_store_value_br_link_key *exp)
{
    struct ble_store_value_br_link_key value;
    struct ble_store_key_br_link_key key;
    int rc;

    ble_store_key_from_value_br_link_key(&key, exp);

    rc = ble_store_read_br_link_key(&key, &value);
    TEST_ASSERT_FATAL(rc == 0);
    TEST_ASSERT(ble_addr_cmp(&value.peer_addr, &exp->peer_addr) == 0);
    TEST_ASSERT(memcmp(value.key, exp->key, sizeof value.key) == 0);
    TEST_ASSERT(value.key_type == exp->key_type);
}

static void
ble_store_test_util_verify_no_br_link_key(const ble_addr_t *addr)
{
    struct ble_store_value_br_link_key value;
    struct ble_store_key_br_link_key key;
    int rc;

    memset(&key, 0, sizeof key);
    key.peer_addr = *addr;

    rc = ble_store_read_br_link_key(&key, &value);
    TEST_ASSERT(rc == BLE_HS_ENOENT);
}

TEST_CASE_SELF(ble_store_test_br_link_key)
{
    struct ble_store_value_br_link_key link_keys[2];
    struct ble_store_value_br_link_key value;
    struct ble_store_key_br_link_key key;
    int rc;
    int i;

    ble_hs_test_util_init();

    ble_store_test_util_br_link_key(link_keys + 0, 1, 7);
    ble_store_test_util_br_link_key(link_keys + 1, 2, 8);

    TEST_ASSERT(ble_store_test_util_count(BLE_STORE_OBJ_TYPE_BR_LINK_KEY) == 0);
    ble_store_test_util_verify_no_br_link_key(&link_keys[0].peer_addr);

    /*** Write. */
    for (i = 0; i < sizeof link_keys / sizeof link_keys[0]; i++) {
        rc = ble_store_write_br_link_key(link_keys + i);
        TEST_ASSERT_FATAL(rc == 0);
    }
    TEST_ASSERT(ble_store_test_util_count(BLE_STORE_OBJ_TYPE_BR_LINK_KEY) == 2);

    /*** Read, by peer and by index. */
    for (i = 0; i < sizeof link_keys / sizeof link_keys[0]; i++) {
        ble_store_test_util_verify_br_link_key(link_keys + i);

        memset(&key, 0, sizeof key);
        key.idx = i;
        rc = ble_store_read_br_link_key(&key, &value);
        TEST_ASSERT_FATAL(rc == 0);
        TEST_ASSERT(ble_addr_cmp(&value.peer_addr, &link_keys[i].peer_addr) == 0);
        TEST_ASSERT(memcmp(value.key, link_keys[i].key, sizeof value.key) == 0);
    }

    memset(&key, 0, sizeof key);
    key.idx = 2;
    rc = ble_store_read_br_link_key(&key, &value);
    TEST_ASSERT(rc == BLE_HS_ENOENT);

    /* Only the first match of a peer exists. */
    ble_store_key_from_value_br_link_key(&key, link_keys + 0);
    key.idx = 1;
    rc = ble_store_read_br_link_key(&key, &value);
    TEST_ASSERT(rc == BLE_HS_ENOENT);

    /*** A new key of a peer replaces its old one. */
    link_keys[0].key[0] = 0xaa;
    link_keys[0].key_type = 8;
    rc = ble_store_write_br_link_key(link_keys + 0);
    TEST_ASSERT_FATAL(rc == 0);

    TEST_ASSERT(ble_store_test_util_count(BLE_STORE_OBJ_TYPE_BR_LINK_KEY) == 2);
    ble_store_test_util_verify_br_link_key(link_keys + 0);
    ble_store_test_util_verify_br_link_key(link_keys + 1);

    /*** Delete. */
    ble_store_key_from_value_br_link_key(&key, link_keys + 0);
    rc = ble_store_delete_br_link_key(&key);
    TEST_ASSERT(rc == 0);

    TEST_ASSERT(ble_store_test_util_count(BLE_STORE_OBJ_TYPE_BR_LINK_KEY) == 1);
    ble_store_test_util_verify_no_br_link_key(&link_keys[0].peer_addr);
    ble_store_test_util_verify_br_link_key(link_keys + 1);

    rc = ble_store_delete_br_link_key(&key);
    TEST_ASSERT(rc == BLE_HS_ENOENT);

    /* The other object types are not affected. */
    TEST_ASSERT(ble_store_test_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC) == 0);
    TEST_ASSERT(ble_store_test_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC) == 0);
    TEST_ASSERT(ble_store_test_util_count(BLE_STORE_OBJ_TYPE_CCCD) == 0);

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_store_test_delete_peer_br_link_key)
{
    struct ble_store_value_br_link_key link_keys[2];
    struct ble_store_value_sec secs[2];
    int rc;
    int i;

    ble_hs_test_util_init();

    /* Two link keys; the address of the second one is the one of the second
     * LE peer, which is a random one and has no meaning on BR/EDR.
     */
    ble_store_test_util_br_link_key(link_keys + 0, 1, 7);
    ble_store_test_util_br_link_key(link_keys + 1, 2, 7);

    memset(secs, 0, sizeof secs);
    secs[0].peer_addr = link_keys[0].peer_addr;
    secs[0].ltk_present = 1;
    secs[1].peer_addr = link_keys[1].peer_addr;
    secs[1].peer_addr.type = BLE_ADDR_RANDOM;
    secs[1].ltk_present = 1;

    for (i = 0; i < sizeof secs / sizeof secs[0]; i++) {
        rc = ble_store_write_our_sec(secs + i);
        TEST_ASSERT_FATAL(rc == 0);
        rc = ble_store_write_peer_sec(secs + i);
        TEST_ASSERT_FATAL(rc == 0);
    }

    for (i = 0; i < sizeof link_keys / sizeof link_keys[0]; i++) {
        rc = ble_store_write_br_link_key(link_keys + i);
        TEST_ASSERT_FATAL(rc == 0);
    }

    /* Delete the peer with the random address; no link key is its. */
    rc = ble_store_util_delete_peer(&secs[1].peer_addr);
    TEST_ASSERT_FATAL(rc == 0);

    ble_store_test_util_verify_peer_deleted(&secs[1].peer_addr);
    TEST_ASSERT(ble_store_test_util_count(BLE_STORE_OBJ_TYPE_BR_LINK_KEY) == 2);
    ble_store_test_util_verify_br_link_key(link_keys + 0);
    ble_store_test_util_verify_br_link_key(link_keys + 1);

    /* Delete the peer with the public address; its link key goes too. */
    rc = ble_store_util_delete_peer(&secs[0].peer_addr);
    TEST_ASSERT_FATAL(rc == 0);

    ble_store_test_util_verify_peer_deleted(&secs[0].peer_addr);
    ble_store_test_util_verify_no_br_link_key(&link_keys[0].peer_addr);
    TEST_ASSERT(ble_store_test_util_count(BLE_STORE_OBJ_TYPE_BR_LINK_KEY) == 1);
    ble_store_test_util_verify_br_link_key(link_keys + 1);

    /* Delete a peer that only has a link key. */
    rc = ble_store_util_delete_peer(&link_keys[1].peer_addr);
    TEST_ASSERT_FATAL(rc == 0);

    TEST_ASSERT(ble_store_test_util_count(BLE_STORE_OBJ_TYPE_BR_LINK_KEY) == 0);

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_store_test_overflow_br_link_key)
{
    struct ble_store_value_br_link_key link_keys[MYNEWT_VAL(BLE_BREDR_MAX_LINK_KEYS) + 2];
    ble_store_status_fn *status_cb;
    void *status_arg;
    int status;
    int rc;
    int i;

    ble_hs_test_util_init();

    status_cb = ble_hs_cfg.store_status_cb;
    status_arg = ble_hs_cfg.store_status_arg;

    for (i = 0; i < sizeof link_keys / sizeof link_keys[0]; i++) {
        ble_store_test_util_br_link_key(link_keys + i, 1 + i, 7);
    }

    /*** The application makes no room: the write fails. */
    ble_hs_cfg.store_status_cb = ble_store_test_util_status_overflow;
    ble_hs_cfg.store_status_arg = &status;
    status = BLE_HS_ESTORE_CAP;

    memset(&ble_store_test_status_event, 0, sizeof ble_store_test_status_event);

    for (i = 0; i < MYNEWT_VAL(BLE_BREDR_MAX_LINK_KEYS); i++) {
        rc = ble_store_write_br_link_key(link_keys + i);
        TEST_ASSERT_FATAL(rc == 0);
    }
    TEST_ASSERT(ble_store_test_status_event.event_code == 0);

    /* This record should have caused an overflow. */
    rc = ble_store_write_br_link_key(link_keys + i);
    TEST_ASSERT(rc == BLE_HS_ESTORE_CAP);
    TEST_ASSERT(ble_store_test_status_event.event_code == BLE_STORE_EVENT_OVERFLOW);
    TEST_ASSERT(ble_store_test_status_event.overflow.obj_type ==
                BLE_STORE_OBJ_TYPE_BR_LINK_KEY);
    TEST_ASSERT(ble_store_test_status_event.overflow.value == (void *)(link_keys + i));

    TEST_ASSERT(ble_store_test_util_count(BLE_STORE_OBJ_TYPE_BR_LINK_KEY) ==
                MYNEWT_VAL(BLE_BREDR_MAX_LINK_KEYS));
    ble_store_test_util_verify_no_br_link_key(&link_keys[i].peer_addr);

    /* A new key for a peer that has one needs no room. */
    link_keys[0].key[1] = 0xaa;
    rc = ble_store_write_br_link_key(link_keys + 0);
    TEST_ASSERT(rc == 0);
    ble_store_test_util_verify_br_link_key(link_keys + 0);

    /*** The round-robin handler drops the link key stored first. */
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.store_status_arg = NULL;

    rc = ble_store_write_br_link_key(link_keys + i);
    TEST_ASSERT_FATAL(rc == 0);

    TEST_ASSERT(ble_store_test_util_count(BLE_STORE_OBJ_TYPE_BR_LINK_KEY) ==
                MYNEWT_VAL(BLE_BREDR_MAX_LINK_KEYS));
    ble_store_test_util_verify_no_br_link_key(&link_keys[0].peer_addr);
    for (i = 1; i <= MYNEWT_VAL(BLE_BREDR_MAX_LINK_KEYS); i++) {
        ble_store_test_util_verify_br_link_key(link_keys + i);
    }

    /* And the next one stored after it. */
    rc = ble_store_write_br_link_key(link_keys + i);
    TEST_ASSERT_FATAL(rc == 0);

    TEST_ASSERT(ble_store_test_util_count(BLE_STORE_OBJ_TYPE_BR_LINK_KEY) ==
                MYNEWT_VAL(BLE_BREDR_MAX_LINK_KEYS));
    ble_store_test_util_verify_no_br_link_key(&link_keys[1].peer_addr);
    for (i = 2; i <= MYNEWT_VAL(BLE_BREDR_MAX_LINK_KEYS) + 1; i++) {
        ble_store_test_util_verify_br_link_key(link_keys + i);
    }

    ble_hs_cfg.store_status_cb = status_cb;
    ble_hs_cfg.store_status_arg = status_arg;

    ble_hs_test_util_assert_mbufs_freed(NULL);
}

TEST_CASE_SELF(ble_store_test_clear_br_link_key)
{
    struct ble_store_value_br_link_key link_keys[2];
    struct ble_store_value_sec sec;
    int rc;
    int i;

    ble_hs_test_util_init();

    ble_store_test_util_br_link_key(link_keys + 0, 1, 7);
    ble_store_test_util_br_link_key(link_keys + 1, 2, 8);

    memset(&sec, 0, sizeof sec);
    sec.peer_addr = link_keys[0].peer_addr;
    sec.ltk_present = 1;

    rc = ble_store_write_our_sec(&sec);
    TEST_ASSERT_FATAL(rc == 0);

    for (i = 0; i < sizeof link_keys / sizeof link_keys[0]; i++) {
        rc = ble_store_write_br_link_key(link_keys + i);
        TEST_ASSERT_FATAL(rc == 0);
    }

    /* Sanity check. */
    TEST_ASSERT_FATAL(ble_store_test_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC) == 1);
    TEST_ASSERT_FATAL(ble_store_test_util_count(BLE_STORE_OBJ_TYPE_BR_LINK_KEY) == 2);

    /* Ensure store is empty after clear gets called. */
    rc = ble_store_clear();
    TEST_ASSERT_FATAL(rc == 0);
    TEST_ASSERT(ble_store_test_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC) == 0);
    TEST_ASSERT(ble_store_test_util_count(BLE_STORE_OBJ_TYPE_BR_LINK_KEY) == 0);

    /* Ensure second clear succeeds with no effect. */
    rc = ble_store_clear();
    TEST_ASSERT_FATAL(rc == 0);
    TEST_ASSERT(ble_store_test_util_count(BLE_STORE_OBJ_TYPE_BR_LINK_KEY) == 0);

    ble_hs_test_util_assert_mbufs_freed(NULL);
}
#endif

TEST_SUITE(ble_store_suite)
{
    ble_store_test_peers();
    ble_store_test_delete_peer();
    ble_store_test_count();
    ble_store_test_overflow();
    ble_store_test_clear();
#if MYNEWT_VAL(BLE_BREDR)
    ble_store_test_br_link_key();
    ble_store_test_delete_peer_br_link_key();
    ble_store_test_overflow_br_link_key();
    ble_store_test_clear_br_link_key();
#endif
}
