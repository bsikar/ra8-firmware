//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/wdt_subs.zig (RA8FW-890).

const std = @import("std");
const subs = @import("wdt_subs");

var hits: [8]u16 = undefined;
var hit_count: usize = 0;

fn record(ctx: ?*anyopaque, mask: u16) callconv(.c) void {
    const tagged: u16 = if (ctx) |c| @as(*u16, @ptrCast(@alignCast(c))).* else 0;
    hits[hit_count] = tagged | mask;
    hit_count += 1;
}

test "subscribe skips the legacy slot and fills 1..5" {
    var t: subs.Table = .{};
    for (1..subs.max_subs) |want| try std.testing.expectEqual(@as(u8, @intCast(want)), try t.subscribe(record, null));
    try std.testing.expectError(error.Full, t.subscribe(record, null));
    try std.testing.expectEqual(@as(u8, 5), t.count());
}

test "attach owns slot 0 and null clears it" {
    var t: subs.Table = .{};
    t.attach(record, null);
    try std.testing.expectEqual(@as(u8, 1), t.count());
    try std.testing.expectEqual(@as(u8, 1), try t.subscribe(record, null));
    t.attach(null, null);
    try std.testing.expectEqual(@as(u8, 1), t.count());
}

test "unsubscribe rejects bad and empty slots" {
    var t: subs.Table = .{};
    try std.testing.expectError(error.BadSlot, t.unsubscribe(6));
    try std.testing.expectError(error.Empty, t.unsubscribe(2));
    const s = try t.subscribe(record, null);
    try t.unsubscribe(s);
    try std.testing.expectEqual(@as(u8, 0), t.count());
}

test "notify calls slots in order with their context, clear empties" {
    var t: subs.Table = .{};
    var a: u16 = 1;
    var b: u16 = 2;
    _ = try t.subscribe(record, &b);
    t.attach(record, &a);
    hit_count = 0;
    t.notify(0x4000);
    try std.testing.expectEqual(@as(usize, 2), hit_count);
    try std.testing.expectEqual(@as(u16, 0x4001), hits[0]);
    try std.testing.expectEqual(@as(u16, 0x4002), hits[1]);
    t.clear();
    try std.testing.expectEqual(@as(u8, 0), t.count());
}
