//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Link-time backend for the runtime ABI tests. It keeps the ABI module's
//! private storage hooks out of the production archive while delegating to the
//! test file's RAM-medium implementation.

const common = @import("cache_store_types");
const Store = common.Store;
const RawErr = common.RawErr;

extern fn cache_store_test_mock_crc32(data: ?[*]const u8, len: u32) u32;
extern fn cache_store_test_mock_sector_read(store: ?*const Store, sector: u32, out512: ?[*]u8) RawErr;
extern fn cache_store_test_mock_sector_write(store: ?*Store, sector: u32, in512: ?[*]const u8) RawErr;
extern fn cache_store_test_mock_sector_release(store: ?*Store, sector: u32) RawErr;
extern fn cache_store_test_mock_index_find(store: ?*const Store, key: u32) i32;
extern fn cache_store_test_mock_index_add(
    store: ?*Store,
    key: u32,
    start_sector: u32,
    sector_count: u16,
    byte_len: u32,
    pinned: bool,
) i32;
extern fn cache_store_test_mock_super_write(store: ?*Store, clean: u32) RawErr;
extern fn cache_store_test_mock_dir_save(store: ?*Store, out_entry_count: ?*u32) RawErr;

pub fn priv_cache_store_crc32(data: ?[*]const u8, len: u32) u32 {
    return cache_store_test_mock_crc32(data, len);
}

pub fn priv_cache_store_sector_read(store: ?*const Store, sector: u32, out512: ?[*]u8) RawErr {
    return cache_store_test_mock_sector_read(store, sector, out512);
}

pub fn priv_cache_store_sector_write(store: ?*Store, sector: u32, in512: ?[*]const u8) RawErr {
    return cache_store_test_mock_sector_write(store, sector, in512);
}

pub fn priv_cache_store_sector_release(store: ?*Store, sector: u32) RawErr {
    return cache_store_test_mock_sector_release(store, sector);
}

pub fn priv_cache_store_index_find(store: ?*const Store, key: u32) i32 {
    return cache_store_test_mock_index_find(store, key);
}

pub fn priv_cache_store_index_add(
    store: ?*Store,
    key: u32,
    start_sector: u32,
    sector_count: u16,
    byte_len: u32,
    pinned: bool,
) i32 {
    return cache_store_test_mock_index_add(store, key, start_sector, sector_count, byte_len, pinned);
}

pub fn priv_cache_store_super_write(store: ?*Store, clean: u32) RawErr {
    return cache_store_test_mock_super_write(store, clean);
}

pub fn priv_cache_store_dir_save(store: ?*Store, out_entry_count: ?*u32) RawErr {
    return cache_store_test_mock_dir_save(store, out_entry_count);
}
