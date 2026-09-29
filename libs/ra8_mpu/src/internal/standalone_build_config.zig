//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host-safe options for the test-zig gate's standalone production-source
//! compile. Real library and test builds receive generated options from
//! build.zig instead.

/// Use the host barrier stand-ins while the ABI source compiles in isolation.
pub const off_target: bool = true;
