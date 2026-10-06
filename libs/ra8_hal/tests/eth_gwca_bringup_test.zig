//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/eth_gwca_bringup.zig (RA8FW-850).

const std = @import("std");
const b = @import("eth_gwca_bringup");
const q = b.q;

const Log = struct {
    count: *u32,
    pub fn logError(self: Log, _: [*:0]const u8) void {
        self.count.* += 1;
    }
};

/// Records every call; `fail_at` is the 1-based call that returns 0x203.
const Ops = struct {
    trace: *std.ArrayList(u32),
    calls: *u32,
    fail_at: u32,
    last_step: *u32,
    fn next(self: Ops, tag: u32) u16 {
        self.calls.* += 1;
        self.trace.append(tag) catch unreachable;
        return if (self.calls.* == self.fail_at) 0x203 else 0;
    }
    pub fn setMode(self: Ops, mode: u32) u16 {
        return self.next(mode);
    }
    pub fn axiInit(self: Ops) u16 {
        return self.next(0xA);
    }
    pub fn installLinkfix(self: Ops, _: ?[*]volatile q.Desc, _: u32) u16 {
        return self.next(0xF);
    }
    pub fn step(self: Ops, v: u32) void {
        self.last_step.* = v;
    }
};

fn run(fail_at: u32, step: *u32, trace: *std.ArrayList(u32)) u16 {
    var calls: u32 = 0;
    var table = [_]q.Desc{.{}} ** 2;
    return b.bringUp(Ops{ .trace = trace, .calls = &calls, .fail_at = fail_at, .last_step = step }, &table, 2);
}

test "installLinkfix marks entries LEMPTY and splits the address" {
    var errs: u32 = 0;
    var table = [_]q.Desc{.{ .ds_l = 9, .ptr_l = 7 }} ** 3;
    var hi: u32 = 0xDEAD;
    var lo: u32 = 0;
    try std.testing.expectEqual(b.ok, b.installLinkfix(Log{ .count = &errs }, &table, 3, &hi, &lo));
    for (table) |d| {
        try std.testing.expectEqual(@as(u8, 0), d.ds_l);
        try std.testing.expectEqual(@as(u8, b.dt_lempty << 4), d.b2);
        try std.testing.expectEqual(@as(u32, 0), d.ptr_l);
    }
    const addr: u64 = @intFromPtr(&table);
    try std.testing.expectEqual(@as(u32, @truncate(addr)), lo);
    try std.testing.expectEqual(@as(u32, @truncate((addr >> 32) & 0xFF)), hi);
}

test "installLinkfix rejects a null table and bad counts" {
    var errs: u32 = 0;
    var hi: u32 = 0;
    var lo: u32 = 0;
    var table = [_]q.Desc{.{}} ** 1;
    try std.testing.expectEqual(b.null_ptr, b.installLinkfix(Log{ .count = &errs }, null, 1, &hi, &lo));
    try std.testing.expectEqual(b.invalid_arg, b.installLinkfix(Log{ .count = &errs }, &table, 0, &hi, &lo));
    try std.testing.expectEqual(b.invalid_arg, b.installLinkfix(Log{ .count = &errs }, &table, 33, &hi, &lo));
    try std.testing.expectEqual(@as(u32, 3), errs);
}

test "bringUp runs the full sequence and ends on step 6" {
    var trace = std.ArrayList(u32).init(std.testing.allocator);
    defer trace.deinit();
    var step: u32 = 0xFF;
    try std.testing.expectEqual(b.ok, run(0, &step, &trace));
    try std.testing.expectEqualSlices(u32, &.{ 1, 2, 0xA, 0xF, 1, 3 }, trace.items);
    try std.testing.expectEqual(@as(u32, 6), step);
}

test "bringUp failures record the step and fall back to DISABLE" {
    var trace = std.ArrayList(u32).init(std.testing.allocator);
    defer trace.deinit();
    var step: u32 = 0;
    try std.testing.expectEqual(@as(u16, 0x203), run(1, &step, &trace));
    try std.testing.expectEqual(@as(u32, 0x11), step);
    try std.testing.expectEqualSlices(u32, &.{1}, trace.items);
    trace.clearRetainingCapacity();
    try std.testing.expectEqual(@as(u16, 0x203), run(3, &step, &trace));
    try std.testing.expectEqual(@as(u32, 0x13), step);
    try std.testing.expectEqualSlices(u32, &.{ 1, 2, 0xA, 1 }, trace.items);
    trace.clearRetainingCapacity();
    try std.testing.expectEqual(@as(u16, 0x203), run(6, &step, &trace));
    try std.testing.expectEqual(@as(u32, 0x16), step);
}
