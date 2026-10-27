//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `fw_clock_stub_resolve`.
//!
//! This file owns what the pure table in `internal/clock_stub.zig` refuses to
//! know about: an out-parameter that may be null. It makes no mapping decision
//! of its own.

const stub = @import("clock_stub");

/// `ra8_err.h` codes this membrane can return.
pub const Err = struct {
    pub const ok: u32 = 0;
    pub const invalid_arg: u32 = 0x103;
    pub const not_found: u32 = 0x106;
};

/// `fw_clock_module_t`.
pub const Module = extern struct {
    kind: u8,
    index: u8,
};

/// `fw_clock_stub_row_t`.
pub const Row = extern struct {
    rate_hz: u32,
    has_rate: bool,
};

/// Fill the row for one neutral module. An absent module zeroes the row and
/// returns `not_found`, exactly as the C header documents.
pub fn fill(module: Module, out: ?*Row) u32 {
    const row = out orelse return Err.invalid_arg;

    const rate = stub.resolve(module.kind, module.index) orelse {
        row.* = .{ .rate_hz = 0, .has_rate = false };
        return Err.not_found;
    };

    row.* = .{ .rate_hz = rate, .has_rate = true };
    return Err.ok;
}
