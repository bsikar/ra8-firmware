//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Frames coming off a transport that delivers them in pieces.

const std = @import("std");
const testing = std.testing;

const rpc = @import("ra8_rpc");
const messages = @import("messages.zig");
const Blob = messages.Blob;

comptime {
    _ = @import("messages.zig");
    _ = @import("mock_queue.zig");
    _ = @import("mock_signal.zig");
    _ = @import("service.zig");
}

const max_frame = rpc.frame.maxSize(Blob);

test "nothing on the wire is no frame" {
    var wires: [2][64]u8 = undefined;
    var loop = rpc.Loopback.init(&wires[0], &wires[1]);
    var buf: [max_frame]u8 = undefined;
    var inbox: rpc.link.Inbox = .{ .buf = &buf };
    try testing.expectEqual(null, try inbox.next(loop.b()));
}

test "a frame arriving a byte at a time is held until it is whole" {
    var wires: [2][64]u8 = undefined;
    var loop = rpc.Loopback.init(&wires[0], &wires[1]);
    var buf: [max_frame]u8 = undefined;
    var inbox: rpc.link.Inbox = .{ .buf = &buf };

    var out: [max_frame]u8 = undefined;
    const value: Blob = .{ .addr = 0x11223344, .data = "abc" };
    const bytes = try rpc.frame.encode(Blob, 9, value, &out);
    for (bytes[0 .. bytes.len - 1]) |byte| {
        try loop.a().send(&.{byte});
        try testing.expectEqual(null, try inbox.next(loop.b()));
    }
    try loop.a().send(bytes[bytes.len - 1 ..]);

    const got = (try inbox.next(loop.b())).?;
    try testing.expectEqual(@as(u16, 9), got.kind);
    try testing.expectEqualDeep(value, try rpc.codec.decode(Blob, got.payload));
    try testing.expectEqual(null, try inbox.next(loop.b()));
}

test "frames that arrive together come out one at a time, in order" {
    var wires: [2][128]u8 = undefined;
    var loop = rpc.Loopback.init(&wires[0], &wires[1]);
    var buf: [2 * max_frame]u8 = undefined;
    var inbox: rpc.link.Inbox = .{ .buf = &buf };

    var out: [max_frame]u8 = undefined;
    for (1..4) |kind| {
        const value: Blob = .{ .addr = @intCast(kind), .data = "abcdefgh"[0..kind] };
        try rpc.link.post(loop.a(), Blob, @intCast(kind), value, &out);
    }
    for (1..4) |kind| {
        const got = (try inbox.next(loop.b())).?;
        try testing.expectEqual(@as(u16, @intCast(kind)), got.kind);
        try testing.expectEqual(kind, (try rpc.codec.decode(Blob, got.payload)).data.len);
    }
    try testing.expectEqual(null, try inbox.next(loop.b()));
}

test "a frame longer than the buffer is refused from its header, and stays refused" {
    var wires: [2][64]u8 = undefined;
    var loop = rpc.Loopback.init(&wires[0], &wires[1]);
    var buf: [rpc.frame.Header.bytes + 4]u8 = undefined;
    var inbox: rpc.link.Inbox = .{ .buf = &buf };

    var header: [rpc.frame.Header.bytes]u8 = undefined;
    (rpc.frame.Header{ .length = 5, .kind = 1 }).write(&header);
    try loop.a().send(&header);
    try testing.expectError(error.Oversize, inbox.next(loop.b()));
    try testing.expectError(error.Oversize, inbox.next(loop.b()));
}

test "a frame that does not fit the wire is not sent in part" {
    var wires: [2][8]u8 = undefined;
    var loop = rpc.Loopback.init(&wires[0], &wires[1]);
    var out: [max_frame]u8 = undefined;
    const value: Blob = .{ .addr = 1, .data = "abc" };
    try testing.expectError(error.LinkFull, rpc.link.post(loop.a(), Blob, 9, value, &out));
    try testing.expectEqual(@as(usize, 0), loop.b().poll());
}
