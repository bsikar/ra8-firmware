//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Archive root for `ra8_cache_store`. The runtime ABI and mount modules meet
//! through ordinary Zig calls; only the public init entry point is C-exported.

comptime {
    _ = @import("ra8_cache_store_abi.zig");
    _ = @import("cache_store_backend");
    _ = @import("cache_store_init");
}
