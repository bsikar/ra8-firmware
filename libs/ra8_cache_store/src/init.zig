//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Public C ABI entry point for mounting the cache store. Kept separate from
//! the internal mount helpers so test-only helper archives do not duplicate
//! this product symbol.

const mount = @import("cache_store_backend");
const Store = @import("cache_store_types").Store;
const Config = @import("cache_store_types").Config;
const RawErr = @import("cache_store_types").RawErr;

pub export fn ra8_cache_store_init(store: ?*Store, config: ?*const Config) RawErr {
    return mount.initStore(store, config);
}
