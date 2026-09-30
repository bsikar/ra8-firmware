//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Export surface of the `ra8_jpeg` archive.
//!
//! The library has two C ABI membranes and a Zig static library has one root,
//! so this file exists only to pull both in. Nothing else belongs here.

comptime {
    _ = @import("jpeg_imgdec_abi.zig");
    _ = @import("jpeg_encode_abi.zig");
}
