//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! One module root gathering the facade and every ported backend, so a test
//! binary that needs more than one of them compiles the ABI membrane exactly
//! once and the types on both sides of a call are the same types.

pub const abi = @import("ra8_camera_abi.zig");
pub const memory = @import("source_memory.zig");
pub const passthrough = @import("codec_passthrough.zig");
pub const jpeg_sw = @import("codec_jpeg_sw.zig");
