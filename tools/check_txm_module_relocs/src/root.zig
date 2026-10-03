//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Module root for the checker, so the tests and `main.zig` reach its parts
//! by one import.

pub const arm = @import("arm.zig");
pub const elf32 = @import("elf32.zig");
pub const check = @import("check.zig");
pub const coverage = @import("coverage.zig");
pub const layout = @import("layout.zig");
pub const records = @import("records.zig");
pub const table = @import("table.zig");
pub const report = @import("report.zig");
