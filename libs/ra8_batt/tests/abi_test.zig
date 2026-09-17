//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the exported C ABI: return codes, guard order, the
//! out-parameter write, and the label pointer. The log sink the membrane
//! calls on a rejected pointer is exported here, the same link-time
//! substitution the firmware build performs with `ra8_log.c`, so the cases
//! can also assert that a rejection actually logged.

const std = @import("std");
const abi = @import("abi");

const ok = @intFromEnum(abi.BattError.ok);
const null_ptr = @intFromEnum(abi.BattError.null_ptr);

var log_calls: usize = 0;
var last_message: [*:0]const u8 = "";

export fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void {
    _ = tag;
    log_calls += 1;
    last_message = message;
}

fn resetLog() void {
    log_calls = 0;
    last_message = "";
}

fn update(mon: *abi.Monitor, soc: u8, charging: bool) abi.Nag {
    var nag: abi.Nag = .none;
    std.testing.expectEqual(ok, abi.ra8_batt_update(mon, soc, charging, &nag)) catch unreachable;
    return nag;
}

test "init returns ok and arms both bands" {
    resetLog();
    var mon: abi.Monitor = .{ .low_raised = true, .critical_raised = true };
    try std.testing.expectEqual(ok, abi.ra8_batt_monitor_init(&mon));
    try std.testing.expect(!mon.low_raised);
    try std.testing.expect(!mon.critical_raised);
    try std.testing.expectEqual(@as(usize, 0), log_calls);
}

test "init rejects a null monitor and logs" {
    resetLog();
    try std.testing.expectEqual(null_ptr, abi.ra8_batt_monitor_init(null));
    try std.testing.expectEqual(@as(usize, 1), log_calls);
    try std.testing.expectEqualStrings("mon must not be nullptr", std.mem.span(last_message));
}

test "update rejects a null monitor before touching the out pointer" {
    resetLog();
    try std.testing.expectEqual(null_ptr, abi.ra8_batt_update(null, 5, false, null));
    try std.testing.expectEqual(@as(usize, 1), log_calls);
    try std.testing.expectEqualStrings("mon must not be nullptr", std.mem.span(last_message));
}

test "update rejects a null out pointer with the monitor untouched" {
    resetLog();
    var mon: abi.Monitor = undefined;
    try std.testing.expectEqual(ok, abi.ra8_batt_monitor_init(&mon));
    _ = update(&mon, 20, false);
    try std.testing.expectEqual(null_ptr, abi.ra8_batt_update(&mon, 5, false, null));
    try std.testing.expect(mon.low_raised);
    try std.testing.expectEqualStrings("out_nag must not be nullptr", std.mem.span(last_message));
}

test "a rejected update leaves the caller's nag slot alone" {
    resetLog();
    var nag: abi.Nag = .critical;
    try std.testing.expectEqual(null_ptr, abi.ra8_batt_update(null, 5, false, &nag));
    try std.testing.expectEqual(abi.Nag.critical, nag);
}

test "the monitor stays usable after a rejected call" {
    resetLog();
    var mon: abi.Monitor = undefined;
    try std.testing.expectEqual(ok, abi.ra8_batt_monitor_init(&mon));
    try std.testing.expectEqual(null_ptr, abi.ra8_batt_update(&mon, 5, false, null));
    try std.testing.expectEqual(abi.Nag.low, update(&mon, 20, false));
}

test "the basic descent across the ABI matches the C suite" {
    var mon: abi.Monitor = undefined;
    try std.testing.expectEqual(ok, abi.ra8_batt_monitor_init(&mon));
    try std.testing.expectEqual(abi.Nag.none, update(&mon, 72, false));
    try std.testing.expectEqual(abi.Nag.low, update(&mon, 20, false));
    try std.testing.expectEqual(abi.Nag.none, update(&mon, 18, false));
    try std.testing.expectEqual(abi.Nag.critical, update(&mon, 10, false));
    try std.testing.expectEqual(abi.Nag.none, update(&mon, 8, false));
}

test "re-arm on rise across the ABI matches the C suite" {
    var mon: abi.Monitor = undefined;
    try std.testing.expectEqual(ok, abi.ra8_batt_monitor_init(&mon));
    try std.testing.expectEqual(abi.Nag.low, update(&mon, 20, false));
    try std.testing.expectEqual(abi.Nag.none, update(&mon, 22, false));
    try std.testing.expectEqual(abi.Nag.none, update(&mon, 20, false));
    try std.testing.expectEqual(abi.Nag.none, update(&mon, 24, false));
    try std.testing.expectEqual(abi.Nag.low, update(&mon, 20, false));
}

test "charging across the ABI matches the C suite" {
    var mon: abi.Monitor = undefined;
    try std.testing.expectEqual(ok, abi.ra8_batt_monitor_init(&mon));
    try std.testing.expectEqual(abi.Nag.low, update(&mon, 20, false));
    try std.testing.expectEqual(abi.Nag.none, update(&mon, 5, true));
    try std.testing.expectEqual(abi.Nag.critical, update(&mon, 5, false));
}

test "the clamp arm across the ABI matches the C suite" {
    var mon: abi.Monitor = undefined;
    try std.testing.expectEqual(ok, abi.ra8_batt_monitor_init(&mon));
    try std.testing.expectEqual(abi.Nag.none, update(&mon, 200, false));
    try std.testing.expectEqual(ok, abi.ra8_batt_monitor_init(&mon));
    try std.testing.expectEqual(abi.Nag.critical, update(&mon, 0, false));
}

test "raise-low MC/DC vectors across the ABI" {
    var mon: abi.Monitor = undefined;
    try std.testing.expectEqual(ok, abi.ra8_batt_monitor_init(&mon));
    try std.testing.expectEqual(abi.Nag.low, update(&mon, 20, false));
    try std.testing.expectEqual(ok, abi.ra8_batt_monitor_init(&mon));
    try std.testing.expectEqual(abi.Nag.none, update(&mon, 21, false));
    try std.testing.expectEqual(ok, abi.ra8_batt_monitor_init(&mon));
    _ = update(&mon, 20, false);
    try std.testing.expectEqual(abi.Nag.none, update(&mon, 20, false));
}

test "raise-critical MC/DC vectors across the ABI" {
    var mon: abi.Monitor = undefined;
    try std.testing.expectEqual(ok, abi.ra8_batt_monitor_init(&mon));
    try std.testing.expectEqual(abi.Nag.critical, update(&mon, 10, false));
    try std.testing.expectEqual(ok, abi.ra8_batt_monitor_init(&mon));
    try std.testing.expectEqual(abi.Nag.low, update(&mon, 11, false));
    try std.testing.expectEqual(ok, abi.ra8_batt_monitor_init(&mon));
    _ = update(&mon, 10, false);
    try std.testing.expectEqual(abi.Nag.none, update(&mon, 10, false));
}

test "re-arm-low MC/DC vectors across the ABI" {
    var mon: abi.Monitor = undefined;
    try std.testing.expectEqual(ok, abi.ra8_batt_monitor_init(&mon));
    _ = update(&mon, 20, false);
    _ = update(&mon, 22, true);
    try std.testing.expectEqual(abi.Nag.low, update(&mon, 20, false));
    try std.testing.expectEqual(ok, abi.ra8_batt_monitor_init(&mon));
    _ = update(&mon, 20, false);
    _ = update(&mon, 24, false);
    try std.testing.expectEqual(abi.Nag.low, update(&mon, 20, false));
    try std.testing.expectEqual(ok, abi.ra8_batt_monitor_init(&mon));
    _ = update(&mon, 20, false);
    _ = update(&mon, 22, false);
    try std.testing.expectEqual(abi.Nag.none, update(&mon, 20, false));
}

test "re-arm-critical MC/DC vectors across the ABI" {
    var mon: abi.Monitor = undefined;
    try std.testing.expectEqual(ok, abi.ra8_batt_monitor_init(&mon));
    _ = update(&mon, 10, false);
    _ = update(&mon, 12, true);
    try std.testing.expectEqual(abi.Nag.critical, update(&mon, 10, false));
    try std.testing.expectEqual(ok, abi.ra8_batt_monitor_init(&mon));
    _ = update(&mon, 10, false);
    _ = update(&mon, 14, false);
    try std.testing.expectEqual(abi.Nag.critical, update(&mon, 10, false));
    try std.testing.expectEqual(ok, abi.ra8_batt_monitor_init(&mon));
    _ = update(&mon, 10, false);
    _ = update(&mon, 12, false);
    try std.testing.expectEqual(abi.Nag.none, update(&mon, 10, false));
}

test "nag_str returns the documented labels" {
    try std.testing.expectEqualStrings("OK", std.mem.span(abi.ra8_batt_nag_str(0)));
    try std.testing.expectEqualStrings("LOW", std.mem.span(abi.ra8_batt_nag_str(1)));
    try std.testing.expectEqualStrings("CRITICAL", std.mem.span(abi.ra8_batt_nag_str(2)));
    try std.testing.expectEqualStrings("?", std.mem.span(abi.ra8_batt_nag_str(200)));
}

test "nag_str is stable across calls" {
    const first = abi.ra8_batt_nag_str(2);
    const second = abi.ra8_batt_nag_str(2);
    try std.testing.expectEqual(first, second);
}

test "a happy-path update never logs" {
    resetLog();
    var mon: abi.Monitor = undefined;
    try std.testing.expectEqual(ok, abi.ra8_batt_monitor_init(&mon));
    _ = update(&mon, 20, false);
    _ = update(&mon, 5, false);
    _ = update(&mon, 90, true);
    try std.testing.expectEqual(@as(usize, 0), log_calls);
}

test "a sweep across the ABI stays in the documented enum" {
    var mon: abi.Monitor = undefined;
    var soc: u9 = 0;
    while (soc <= 255) : (soc += 1) {
        try std.testing.expectEqual(ok, abi.ra8_batt_monitor_init(&mon));
        var nag: abi.Nag = .none;
        try std.testing.expectEqual(ok, abi.ra8_batt_update(&mon, @intCast(soc), false, &nag));
        try std.testing.expect(@intFromEnum(nag) <= 2);
    }
}
