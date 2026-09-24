//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C-test-only exports for the cache-store internal contract tests. This file
//! is built into a separate archive linked only by `test_ra8_cache_store`; the
//! production `ra8_cache_store` archive contains no `priv_cache_store_*` C ABI
//! symbols.

const mount = @import("cache_store_mount");
const Store = mount.Store;
const RawErr = mount.RawErr;

export fn priv_cache_store_crc32(data: ?[*]const u8, len: u32) u32 {
    return mount.priv_cache_store_crc32(data, len);
}

export fn priv_cache_store_sector_read(store: ?*const Store, sector: u32, out512: ?[*]u8) RawErr {
    return mount.priv_cache_store_sector_read(store, sector, out512);
}

export fn priv_cache_store_sector_write(store: ?*Store, sector: u32, in512: ?[*]const u8) RawErr {
    return mount.priv_cache_store_sector_write(store, sector, in512);
}

export fn priv_cache_store_sector_release(store: ?*Store, sector: u32) RawErr {
    return mount.priv_cache_store_sector_release(store, sector);
}

export fn priv_cache_store_index_find(store: ?*const Store, key: u32) i32 {
    return mount.priv_cache_store_index_find(store, key);
}

export fn priv_cache_store_index_add(
    store: ?*Store,
    key: u32,
    start_sector: u32,
    sector_count: u16,
    byte_len: u32,
    pinned: bool,
) i32 {
    return mount.priv_cache_store_index_add(store, key, start_sector, sector_count, byte_len, pinned);
}

export fn priv_cache_store_super_write(store: ?*Store, clean: u32) RawErr {
    return mount.priv_cache_store_super_write(store, clean);
}

export fn priv_cache_store_dir_save(store: ?*Store, out_entry_count: ?*u32) RawErr {
    return mount.priv_cache_store_dir_save(store, out_entry_count);
}
