//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host-safe options for the test-zig gate's standalone production-source
//! compile. Real library and test builds receive generated options from
//! build.zig instead.

/// Default Root of Trust toggle for isolated standalone compilation.
pub const root_of_trust: bool = false;
/// Capture register accesses into the software record during standalone checks.
pub const host_capture: bool = true;
