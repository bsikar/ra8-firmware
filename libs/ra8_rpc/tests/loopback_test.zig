//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The loopback transport: two byte rings, and a write that is whole or not
//! at all.

const std = @import("std");
const testing = std.testing;

const rpc = @import("ra8_rpc");

comptime {
    _ = @import("messages.zig");
    _ = @import("mock_queue.zig");
    _ = @import("service.zig");
}

test "what one end sends the other receives, in each direction" {
    var wires: [2][8]u8 = undefined;
    var loop = rpc.Loopback.init(&wires[0], &wires[1]);
    const a = loop.a();
    const b = loop.b();

    try a.send("abc");
    try b.send("xy");
    try testing.expectEqual(@as(usize, 3), b.poll());
    try testing.expectEqual(@as(usize, 2), a.poll());

    var got: [8]u8 = undefined;
    try testing.expectEqualSlices(u8, "abc", got[0..try b.receive(&got)]);
    try testing.expectEqualSlices(u8, "xy", got[0..try a.receive(&got)]);
    try testing.expectEqual(@as(usize, 0), a.poll());
    try testing.expectEqual(@as(usize, 0), try a.receive(&got));
}

test "a receive takes no more than it has room for and keeps the rest" {
    var wires: [2][8]u8 = undefined;
    var loop = rpc.Loopback.init(&wires[0], &wires[1]);
    try loop.a().send("abcde");

    var got: [2]u8 = undefined;
    try testing.expectEqualSlices(u8, "ab", got[0..try loop.b().receive(&got)]);
    try testing.expectEqualSlices(u8, "cd", got[0..try loop.b().receive(&got)]);
    try testing.expectEqualSlices(u8, "e", got[0..try loop.b().receive(&got)]);
}

test "a write that does not fit sends nothing" {
    var wires: [2][4]u8 = undefined;
    var loop = rpc.Loopback.init(&wires[0], &wires[1]);
    try loop.a().send("abc");
    try testing.expectError(error.LinkFull, loop.a().send("de"));
    try testing.expectEqual(@as(usize, 3), loop.b().poll());
    try loop.a().send("d");

    var got: [4]u8 = undefined;
    try testing.expectEqualSlices(u8, "abcd", got[0..try loop.b().receive(&got)]);
}

test "bytes keep their order across the end of the ring" {
    var wires: [2][4]u8 = undefined;
    var loop = rpc.Loopback.init(&wires[0], &wires[1]);
    var got: [4]u8 = undefined;
    var next: u8 = 0;
    var want: u8 = 0;
    for (0..20) |_| {
        for (0..3) |_| {
            try loop.a().send(&.{next});
            next += 1;
        }
        for (got[0..try loop.b().receive(&got)]) |byte| {
            try testing.expectEqual(want, byte);
            want += 1;
        }
    }
    try testing.expectEqual(next, want);
}
