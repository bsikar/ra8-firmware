//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Root of the `ra8_fs` archive: every ported unit, referenced so its C ABI
//! exports are emitted.

pub const lock = @import("ra8_fs_lock_abi.zig");

comptime {
    _ = lock;
}
