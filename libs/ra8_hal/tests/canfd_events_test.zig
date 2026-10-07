//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/canfd_events.zig (RA8FW-860).

const std = @import("std");
const ev = @import("canfd_events");

const Seen = struct { channel: u8 = 0xFF, mask: u32 = 0, calls: u32 = 0 };

fn record(ctx: ?*anyopaque, channel: u8, mask: u32) callconv(.c) void {
    const seen: *Seen = @ptrCast(@alignCast(ctx.?));
    seen.* = .{ .channel = channel, .mask = mask, .calls = seen.calls + 1 };
}

test "status reads STS and clear keeps flags outside the mask" {
    var c = ev.Chan{ .sts = 0x1234, .erfl = 0b1111 };
    try std.testing.expectEqual(@as(u32, 0x1234), ev.status(&c));
    ev.clear(&c, 0b0101);
    try std.testing.expectEqual(@as(u32, 0b1010), c.erfl);
}

test "dispatch acks ERFL and passes the snapshot to the handler" {
    var c = ev.Chan{ .erfl = 0x80 };
    var seen = Seen{};
    ev.dispatch(&c, 1, .{ .func = record, .ctx = &seen });
    try std.testing.expectEqual(@as(u32, 0), c.erfl);
    try std.testing.expectEqual(Seen{ .channel = 1, .mask = 0x80, .calls = 1 }, seen);
}

test "dispatch without a handler still acks" {
    var c = ev.Chan{ .erfl = 0x3 };
    ev.dispatch(&c, 0, .{});
    try std.testing.expectEqual(@as(u32, 0), c.erfl);
}
