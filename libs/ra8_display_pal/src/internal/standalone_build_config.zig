//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host-safe defaults for the test-zig gate's standalone production-source
//! compile. Real library and test builds receive generated options from
//! build.zig instead.

/// Use host synchronization stubs while compiling in isolation.
pub const off_target: bool = true;
/// Keep D-cache cleaning disabled under host standalone checks.
pub const boot_enable_cache_mpu: bool = false;
