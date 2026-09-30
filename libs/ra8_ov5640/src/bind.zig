//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! One module root gathering the driver membrane and the binder, so a test
//! binary that needs both compiles the ABI membrane exactly once and the
//! types on both sides of a call are the same types.

pub const driver = @import("ra8_ov5640_abi.zig");
pub const binder = @import("ra8_ov5640_bind_abi.zig");
pub const framing = @import("internal/i2c_bind.zig");
