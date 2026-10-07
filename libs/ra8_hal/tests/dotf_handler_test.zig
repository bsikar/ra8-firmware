//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/dotf_handler.zig.

const std = @import("std");
const handler = @import("dotf_handler");

const Seen = struct { calls: u32 = 0, channel: u8 = 0xFF };

fn record(ctx: ?*anyopaque, channel: u8) callconv(.c) void {
    const s: *Seen = @ptrCast(@alignCast(ctx.?));
    s.calls += 1;
    s.channel = channel;
}

test "dispatch calls the handler with its context and channel" {
    var s = Seen{};
    handler.dispatch(record, &s, 1);
    try std.testing.expectEqual(@as(u32, 1), s.calls);
    try std.testing.expectEqual(@as(u8, 1), s.channel);
}

test "dispatch ignores an out-of-range channel" {
    var s = Seen{};
    handler.dispatch(record, &s, 2);
    try std.testing.expectEqual(@as(u32, 0), s.calls);
}

test "dispatch with no handler does nothing" {
    handler.dispatch(null, null, 0);
}
