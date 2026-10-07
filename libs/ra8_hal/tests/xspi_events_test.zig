//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/xspi_events.zig (RA8FW-865).

const std = @import("std");
const ev = @import("xspi_events");

const Fake = struct {
    ints: u32 = 0,
    intc: ?u32 = null,

    pub fn read(self: *Fake, off: usize) u32 {
        std.debug.assert(off == ev.off_ints);
        return self.ints;
    }
    pub fn write(self: *Fake, off: usize, v: u32) void {
        std.debug.assert(off == ev.off_intc);
        self.intc = v;
    }
};

var seen: u32 = 0;
var seen_ctx: ?*anyopaque = null;

fn record(ctx: ?*anyopaque, mask: u32) callconv(.c) void {
    seen = mask;
    seen_ctx = ctx;
}

test "instance bases and range" {
    try std.testing.expectEqual(@as(?usize, 0x4026_8000), ev.instanceBase(0));
    try std.testing.expectEqual(@as(?usize, 0x4026_8400), ev.instanceBase(1));
    try std.testing.expectEqual(@as(?usize, null), ev.instanceBase(2));
    try std.testing.expect(!ev.inRange(2));
}

test "State matches the C two-pointer layout" {
    try std.testing.expectEqual(2 * @sizeOf(usize), @sizeOf(ev.State));
    try std.testing.expectEqual(@sizeOf(usize), @offsetOf(ev.State, "ctx"));
}

test "dispatch snapshots INTS, clears all, then calls the handler" {
    var f = Fake{ .ints = 0x11 };
    var st = ev.State{};
    var token: u8 = 0;
    ev.attach(&st, record, &token);
    ev.dispatch(&f, st);
    try std.testing.expectEqual(@as(?u32, 0xFFFF_FFFF), f.intc);
    try std.testing.expectEqual(@as(u32, 0x11), seen);
    try std.testing.expectEqual(@as(?*anyopaque, &token), seen_ctx);
}

test "dispatch without a handler still clears the flags" {
    var f = Fake{ .ints = 0x2 };
    seen = 0;
    ev.dispatch(&f, .{});
    try std.testing.expectEqual(@as(?u32, 0xFFFF_FFFF), f.intc);
    try std.testing.expectEqual(@as(u32, 0), seen);
}
