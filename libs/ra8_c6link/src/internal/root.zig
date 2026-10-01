//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Module root for the Zig implementation of `ra8_c6link`, so the internal
//! tests reach the envelope codec and its vocabulary by one import.

pub const vocab = @import("vocab.zig");
pub const tlv = @import("tlv.zig");
