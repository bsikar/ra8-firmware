//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Archive root for `ra8_cache_store`. The library is two halves that meet at
//! the `priv_cache_store_*` symbols: the runtime path in
//! `ra8_cache_store_abi.zig` declares them `extern`, the mount path in
//! `mount.zig` exports them. Both have to reach the archive, and an exported
//! function is only emitted when its file is part of the compilation, hence
//! this root.

comptime {
    _ = @import("ra8_cache_store_abi.zig");
    _ = @import("mount.zig");
}
