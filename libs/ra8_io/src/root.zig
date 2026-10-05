//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Root of the `ra8_io` archive: every ported unit, referenced so its C ABI
//! exports are emitted.

pub const log = @import("ra8_io_log_abi.zig");

comptime {
    _ = log;
}
