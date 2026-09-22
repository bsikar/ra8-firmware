//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host-safe options for the test-zig gate's standalone production-source
//! compile. Real library and test builds receive generated options from
//! build.zig instead.

pub const bench_vcom_mv: ?u16 = null;
pub const production_build: bool = false;
