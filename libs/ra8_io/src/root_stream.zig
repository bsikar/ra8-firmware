//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Narrow ra8_io root for C consumers that reach only the stream ABI.
//! root.zig's comptime block keeps the whole ABI surface (and every unit's
//! extern driver deps) in the archive, where --gc-sections cannot drop the
//! unreachable facades; a consumer of this root emits only the stream units.

pub const stream = @import("ra8_io_stream_abi.zig");
pub const stream_ram = @import("ra8_io_stream_ram_abi.zig");

comptime {
    _ = stream;
    _ = stream_ram;
}
