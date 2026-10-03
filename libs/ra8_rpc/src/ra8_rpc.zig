//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Module root for `ra8_rpc`: the frame header and the struct codec under it.
//!
//! Freestanding, no heap and no libc. Every buffer belongs to the caller.

pub const codec = @import("codec.zig");
pub const frame = @import("frame.zig");

pub const Error = codec.Error;
