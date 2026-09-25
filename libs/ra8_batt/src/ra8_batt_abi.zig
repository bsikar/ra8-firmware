//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_batt/inc/ra8_batt.h`. The policy itself is in
//! `internal/root.zig`; this file owns the exported symbols, the argument
//! guards in their original order, the `ra8_err_t` mapping, and the one
//! diagnostic line the C implementation emitted through `RA8_CHECK_NULL_PTR`.
//!
//! No HAL header is reachable from here. The library reads no hardware: a
//! caller hands it a percent and a charge flag, which is what keeps every
//! branch host-testable.

const std = @import("std");
const implementation = @import("internal/root.zig");

/// Nag state carried across calls (`ra8_batt_monitor_t`).
pub const Monitor = extern struct {
    low_raised: u8,
    critical_raised: u8,
};
/// Warning a single update raises (`ra8_batt_nag_t`).
pub const Nag = implementation.Nag;

/// Subset of `ra8_err_t` this library returns.
pub const BattError = enum(u16) {
    ok = 0,
    invalid_arg = 0x103,
    null_ptr = 0x504,
};

/// Component tag on the library's log lines, matching the C's `s_tag`.
const tag: [*:0]const u8 = "ra8_batt";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

/// Reset a nag monitor to the un-nagged, fully-armed state.
///
/// Mirrors `ra8_batt_monitor_init`: one struct clear, no hardware access.
pub export fn ra8_batt_monitor_init(mon: ?*Monitor) callconv(.c) u16 {
    const monitor = mon orelse {
        ra8_log_emit_error(tag, "mon must not be nullptr");
        return @intFromEnum(BattError.null_ptr);
    };
    monitor.* = .{ .low_raised = 0, .critical_raised = 0 };
    return @intFromEnum(BattError.ok);
}

/// Fold one SOC reading into the monitor and report the nag to raise.
///
/// Guard order is the contract: `mon` is rejected before `out_nag`, and both
/// before any band decision, so the host suite's null-guard case can tell the
/// two rejections apart by which call it made.
pub export fn ra8_batt_update(
    mon: ?*Monitor,
    soc_pct: u8,
    charging: u8,
    out_nag: ?*Nag,
) callconv(.c) u16 {
    const monitor = mon orelse {
        ra8_log_emit_error(tag, "mon must not be nullptr");
        return @intFromEnum(BattError.null_ptr);
    };
    const out = out_nag orelse {
        ra8_log_emit_error(tag, "out_nag must not be nullptr");
        return @intFromEnum(BattError.null_ptr);
    };

    if (charging > 1 or monitor.low_raised > 1 or monitor.critical_raised > 1) {
        return @intFromEnum(BattError.invalid_arg);
    }

    var state = implementation.Monitor{
        .low_raised = monitor.low_raised != 0,
        .critical_raised = monitor.critical_raised != 0,
    };
    const nag = implementation.step(&state, soc_pct, charging == 1);
    monitor.low_raised = @intFromBool(state.low_raised);
    monitor.critical_raised = @intFromBool(state.critical_raised);
    out.* = nag;
    return @intFromEnum(BattError.ok);
}

/// Map a nag level to a short, stable upper-case label.
///
/// The parameter is the raw enumerator byte: `ra8_batt_nag_t` is
/// `uint8_t`-backed and the documented contract answers `"?"` for a value
/// outside the enum, which a Zig `Nag` parameter could not represent.
pub export fn ra8_batt_nag_str(nag: u8) callconv(.c) [*:0]const u8 {
    return implementation.label(nag);
}

comptime {
    // `ra8_err_t` is 16-bit across the repo; these two are the only codes the
    // library can return.
    std.debug.assert(@intFromEnum(BattError.ok) == 0);
    std.debug.assert(@intFromEnum(BattError.invalid_arg) == 0x103);
    std.debug.assert(@intFromEnum(BattError.null_ptr) == 0x504);
    // The nag enumerators are public API: consumers switch on the numbers.
    std.debug.assert(@intFromEnum(Nag.none) == 0);
    std.debug.assert(@intFromEnum(Nag.low) == 1);
    std.debug.assert(@intFromEnum(Nag.critical) == 2);
    std.debug.assert(@sizeOf(Nag) == 1);
    std.debug.assert(@sizeOf(Monitor) == 2);
    std.debug.assert(@alignOf(Monitor) == 1);
    std.debug.assert(@offsetOf(Monitor, "low_raised") == 0);
    std.debug.assert(@offsetOf(Monitor, "critical_raised") == 1);
}
