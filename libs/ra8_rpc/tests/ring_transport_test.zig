//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! One ring: where its fields are, the order things happen in, and what it
//! refuses.

const std = @import("std");
const testing = std.testing;

const rpc = @import("ra8_rpc");
const Layout = rpc.ring.Layout;
const MockSignal = @import("mock_signal.zig").MockSignal;
const Entry = MockSignal.Entry;

comptime {
    _ = @import("messages.zig");
    _ = @import("mock_queue.zig");
    _ = @import("mock_signal.zig");
    _ = @import("service.zig");
}

const capacity = 8;

/// A transport that writes and reads the same ring, with a signal that
/// watches it.
const Loop = struct {
    mem: [Layout.header_bytes + capacity]u8 align(4) = undefined,
    signal: MockSignal = .{},
    link: rpc.RingTransport = undefined,

    fn init(self: *Loop) !void {
        const ring = try rpc.Ring.init(&self.mem);
        ring.writeHeader();
        self.signal.watch = &self.mem;
        self.link = rpc.RingTransport.init(ring, ring, self.signal.signal());
    }

    fn word(self: *Loop, offset: usize) u32 {
        return std.mem.readInt(u32, self.mem[offset..][0..4], .little);
    }

    fn setWord(self: *Loop, offset: usize, value: u32) void {
        std.mem.writeInt(u32, self.mem[offset..][0..4], value, .little);
    }

    fn data(self: *Loop) []u8 {
        return self.mem[Layout.header_bytes..];
    }
};

test "the layout keeps its numbers" {
    try testing.expectEqual(@as(usize, 32), Layout.line);
    try testing.expectEqual(@as(usize, 0), Layout.At.magic);
    try testing.expectEqual(@as(usize, 4), Layout.At.version);
    try testing.expectEqual(@as(usize, 8), Layout.At.capacity);
    try testing.expectEqual(@as(usize, 32), Layout.At.head);
    try testing.expectEqual(@as(usize, 64), Layout.At.tail);
    try testing.expectEqual(@as(usize, 96), Layout.header_bytes);
    try testing.expectEqual(@as(u32, 1), Layout.version);
}

test "a formatted ring starts with RA8B and is empty, at its memory's capacity" {
    var loop: Loop = .{};
    @memset(&loop.mem, 0xAA);
    try loop.init();
    try testing.expectEqualSlices(u8, "RA8B", loop.mem[0..4]);
    try testing.expectEqual(Layout.version, loop.word(Layout.At.version));
    try testing.expectEqual(@as(u32, capacity), loop.word(Layout.At.capacity));
    try testing.expectEqual(@as(u32, 0), loop.word(Layout.At.head));
    try testing.expectEqual(@as(u32, 0), loop.word(Layout.At.tail));
    for (loop.mem[12..Layout.header_bytes]) |byte| try testing.expectEqual(@as(u8, 0), byte);
    try testing.expectEqual(@as(usize, 0), loop.link.transport().poll());
}

test "memory without room for a header and two bytes is not a ring" {
    var mem: [Layout.header_bytes + 2]u8 align(4) = undefined;
    try testing.expectError(error.RingTooSmall, rpc.Ring.init(mem[0 .. mem.len - 1]));
    try testing.expectError(error.RingTooSmall, rpc.Ring.init(mem[0..0]));
    _ = try rpc.Ring.init(&mem);
}

test "writing: the data, a barrier, the head, and then the doorbell" {
    var loop: Loop = .{};
    try loop.init();
    try loop.link.transport().send("abc");

    // The data was in place when the barrier ran, and the head was not yet
    // moved; by the time the doorbell rang, it was.
    try testing.expectEqualSlices(Entry, &.{
        .{ .kind = .barrier, .head = 0, .tail = 0 },
        .{ .kind = .notify, .head = 3, .tail = 0 },
    }, loop.signal.entries());
    try testing.expectEqualSlices(u8, "abc", loop.data()[0..3]);
}

test "reading: the head, a barrier, the data, a barrier, and then the tail" {
    var loop: Loop = .{};
    try loop.init();
    const wire = loop.link.transport();
    try wire.send("abc");
    loop.signal.clear();

    var got: [8]u8 = undefined;
    try testing.expectEqualSlices(u8, "abc", got[0..try wire.receive(&got)]);
    // Both barriers ran with the tail where it was; it moved only after the
    // second, once the bytes were out.
    try testing.expectEqualSlices(Entry, &.{
        .{ .kind = .barrier, .head = 3, .tail = 0 },
        .{ .kind = .barrier, .head = 3, .tail = 0 },
    }, loop.signal.entries());
    try testing.expectEqual(@as(u32, 3), loop.word(Layout.At.tail));
    // The one ring so far was the send's. Reading rings nothing.
    try testing.expectEqual(@as(usize, 1), loop.signal.notifies);
}

test "reading an empty ring moves nothing" {
    var loop: Loop = .{};
    try loop.init();
    var got: [8]u8 = undefined;
    try testing.expectEqual(@as(usize, 0), try loop.link.transport().receive(&got));
    try testing.expectEqual(@as(u32, 0), loop.word(Layout.At.tail));
    try testing.expectEqual(@as(usize, 1), loop.signal.barriers);
}

test "bytes keep their order across the end of the data" {
    var loop: Loop = .{};
    try loop.init();
    const wire = loop.link.transport();
    var got: [8]u8 = undefined;
    try wire.send("abcdef");
    try testing.expectEqualSlices(u8, "abcdef", got[0..try wire.receive(&got)]);

    try wire.send("12345");
    try testing.expectEqual(@as(u32, 3), loop.word(Layout.At.head));
    try testing.expectEqualSlices(u8, "345", loop.data()[0..3]);
    try testing.expectEqualSlices(u8, "12", loop.data()[6..8]);
    try testing.expectEqual(@as(usize, 5), wire.poll());
    try testing.expectEqualSlices(u8, "12345", got[0..try wire.receive(&got)]);
    try testing.expectEqual(@as(u32, 3), loop.word(Layout.At.tail));
}

test "a receive too small for what is waiting keeps the rest" {
    var loop: Loop = .{};
    try loop.init();
    const wire = loop.link.transport();
    try wire.send("abcde");
    var got: [2]u8 = undefined;
    try testing.expectEqualSlices(u8, "ab", got[0..try wire.receive(&got)]);
    try testing.expectEqualSlices(u8, "cd", got[0..try wire.receive(&got)]);
    try testing.expectEqualSlices(u8, "e", got[0..try wire.receive(&got)]);
    try testing.expectEqual(@as(usize, 0), try wire.receive(&got));
}

test "a ring holds one byte less than its capacity" {
    var loop: Loop = .{};
    try loop.init();
    const wire = loop.link.transport();
    try testing.expectError(error.LinkFull, wire.send("12345678"));
    try wire.send("1234567");
    try testing.expectEqual(@as(usize, capacity - 1), wire.poll());
    try testing.expectError(error.LinkFull, wire.send("x"));
}

test "a send that does not fit writes nothing and rings nothing" {
    var loop: Loop = .{};
    try loop.init();
    const wire = loop.link.transport();
    try wire.send("abcd");
    const before = loop.mem;
    const rung = loop.signal.notifies;
    const barriers = loop.signal.barriers;

    try testing.expectError(error.LinkFull, wire.send("1234"));
    try testing.expectEqualSlices(u8, &before, &loop.mem);
    try testing.expectEqual(rung, loop.signal.notifies);
    try testing.expectEqual(barriers, loop.signal.barriers);
    try wire.send("123");
}

test "the doorbell rings once per send, wrapped or not, and never for nothing" {
    var loop: Loop = .{};
    try loop.init();
    const wire = loop.link.transport();
    var got: [8]u8 = undefined;
    try wire.send("abcdef");
    try testing.expectEqual(@as(usize, 1), loop.signal.notifies);
    _ = try wire.receive(&got);
    try wire.send("12345");
    try testing.expectEqual(@as(usize, 2), loop.signal.notifies);
    try wire.send("");
    try testing.expectEqual(@as(usize, 2), loop.signal.notifies);
}

test "a header that is not this layout is refused on every path" {
    for ([_]usize{ Layout.At.magic, Layout.At.version, Layout.At.capacity }) |field| {
        var loop: Loop = .{};
        try loop.init();
        const wire = loop.link.transport();
        try wire.send("abc");
        loop.setWord(field, loop.word(field) + 1);

        var got: [8]u8 = @splat(0x7E);
        try testing.expect(wire.poll() != 0);
        try testing.expectError(error.BadMessage, wire.receive(&got));
        for (got) |byte| try testing.expectEqual(@as(u8, 0x7E), byte);
        try testing.expectError(error.BadMessage, wire.send("x"));
    }
}

test "an index at or past the capacity is refused, whichever index it is" {
    for ([_]usize{ Layout.At.head, Layout.At.tail }) |field| {
        for ([_]u32{ capacity, capacity + 1, std.math.maxInt(u32) }) |index| {
            var loop: Loop = .{};
            try loop.init();
            const wire = loop.link.transport();
            loop.setWord(field, index);
            const before = loop.mem;

            var got: [8]u8 = @splat(0x7E);
            try testing.expectError(error.BadMessage, wire.receive(&got));
            for (got) |byte| try testing.expectEqual(@as(u8, 0x7E), byte);
            loop.link.reset();
            try testing.expectError(error.BadMessage, wire.send("x"));
            try testing.expectEqualSlices(u8, &before, &loop.mem);
        }
    }
}

test "a failure stays until the header is rewritten and the transport reset" {
    var loop: Loop = .{};
    try loop.init();
    const wire = loop.link.transport();
    loop.setWord(Layout.At.head, capacity);
    var got: [8]u8 = undefined;
    try testing.expectError(error.BadMessage, wire.receive(&got));

    // Mending the header is not enough: the failure is the transport's now.
    loop.setWord(Layout.At.head, 0);
    try testing.expectError(error.BadMessage, wire.receive(&got));
    try testing.expectError(error.BadMessage, wire.send("x"));
    try testing.expect(wire.poll() != 0);
    try testing.expectEqual(@as(usize, 0), loop.signal.notifies);

    (try rpc.Ring.init(&loop.mem)).writeHeader();
    loop.link.reset();
    try testing.expectEqual(@as(usize, 0), wire.poll());
    try wire.send("ok");
    try testing.expectEqualSlices(u8, "ok", got[0..try wire.receive(&got)]);
}
