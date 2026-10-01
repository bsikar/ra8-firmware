//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `fw_clock_ra8_resolve`.
//!
//! This file owns what the pure table in `internal/clock_map.zig` refuses to
//! know about: an out-parameter that may be null, and the two `has_` flags the
//! C row carries in place of Zig's optionals. It makes no mapping decision of
//! its own.

const std = @import("std");
const map = @import("clock_map");

/// `ra8_err.h` codes this membrane can return.
pub const Err = struct {
    pub const ok: u32 = 0;
    pub const invalid_arg: u32 = 0x103;
    pub const not_found: u32 = 0x106;
    pub const not_supported: u32 = 0x107;
};

/// `fw_clock_module_t`.
pub const Module = extern struct {
    kind: u8,
    index: u8,
};

/// `fw_clock_ra8_row_t`.
pub const Row = extern struct {
    domain: u8,
    gate: u16,
    has_domain: bool,
    has_gate: bool,
};

// The C header's placeholders for a row that carries no domain and no gate.
// A caller reading `domain` without checking `has_domain` gets these, exactly
// as it did from the C.
const no_domain: u8 = @intFromEnum(map.Domain.cpuclk0);
const no_gate: u16 = map.Mstp.sram0;

comptime {
    std.debug.assert(@sizeOf(bool) == 1);
    std.debug.assert(@offsetOf(Row, "domain") == 0);
    std.debug.assert(@offsetOf(Row, "gate") == 2);
    std.debug.assert(@offsetOf(Row, "has_domain") == 4);
    std.debug.assert(@offsetOf(Row, "has_gate") == 5);
}

/// Fill a C row from the table's answer, or leave it blank and say why.
pub fn fill(module: Module, out_row: ?*Row) u32 {
    const row = out_row orelse return Err.invalid_arg;

    row.* = .{
        .domain = no_domain,
        .gate = no_gate,
        .has_domain = false,
        .has_gate = false,
    };

    const resolved = map.resolve(module.kind, module.index) catch |fault| return switch (fault) {
        error.BadKind => Err.invalid_arg,
        error.NotFound => Err.not_found,
    };

    if (resolved.domain) |domain| {
        row.domain = @intFromEnum(domain);
        row.has_domain = true;
    }
    if (resolved.gate) |gate| {
        row.gate = gate;
        row.has_gate = true;
    }

    return Err.ok;
}

pub export fn fw_clock_ra8_resolve(module: Module, out_row: ?*Row) callconv(.c) u32 {
    return fill(module, out_row);
}
