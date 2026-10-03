//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Bytes packed into fixed-size messages and unpacked again.

const std = @import("std");
const testing = std.testing;

const rpc = @import("ra8_rpc");
const MockQueue = @import("mock_queue.zig").MockQueue;

comptime {
    _ = @import("messages.zig");
    _ = @import("mock_queue.zig");
    _ = @import("service.zig");
}

/// Six-byte messages: a two-byte length and room for four.
const message_bytes = 6;
const depth = 4;

/// One end of a link, with the queue it writes and the queue it reads.
const End = struct {
    store: [2][depth * message_bytes]u8 = undefined,
    out: MockQueue = undefined,
    in: MockQueue = undefined,
    scratch: [2][message_bytes]u8 = undefined,
    link: rpc.QueueTransport = undefined,

    fn init(self: *End) !void {
        self.out = MockQueue.init(&self.store[0], message_bytes);
        self.in = MockQueue.init(&self.store[1], message_bytes);
        self.link = try rpc.QueueTransport.init(
            self.out.queue(),
            self.in.queue(),
            &self.scratch[0],
            &self.scratch[1],
        );
    }

    /// The oldest message this end has sent.
    fn sent(self: *End) ![message_bytes]u8 {
        var message: [message_bytes]u8 = undefined;
        try testing.expect(try self.out.queue().receive(&message));
        return message;
    }
};

test "a write is cut into full messages, each with its used length in front" {
    var end: End = .{};
    try end.init();
    try end.link.transport().send("abcdefgh");
    try testing.expectEqual(@as(usize, 2), end.out.count);
    try testing.expectEqualSlices(u8, &.{ 4, 0, 'a', 'b', 'c', 'd' }, &try end.sent());
    try testing.expectEqualSlices(u8, &.{ 4, 0, 'e', 'f', 'g', 'h' }, &try end.sent());
}

test "the last message carries what is left and zeroes after it" {
    var end: End = .{};
    try end.init();
    @memset(&end.scratch[0], 0xAA);
    try end.link.transport().send("abcde");
    try testing.expectEqualSlices(u8, &.{ 4, 0, 'a', 'b', 'c', 'd' }, &try end.sent());
    try testing.expectEqualSlices(u8, &.{ 1, 0, 'e', 0, 0, 0 }, &try end.sent());
}

test "a write of nothing sends no message" {
    var end: End = .{};
    try end.init();
    try end.link.transport().send("");
    try testing.expectEqual(@as(usize, 0), end.out.count);
}

test "a write that needs more messages than are free sends none of them" {
    var end: End = .{};
    try end.init();
    const wire = end.link.transport();
    try wire.send("abcdefgh");
    try testing.expectError(error.LinkFull, wire.send("123456789"));
    try testing.expectEqual(@as(usize, 2), end.out.count);

    try wire.send("12345678");
    try testing.expectEqual(@as(usize, depth), end.out.count);
    try testing.expectError(error.LinkFull, wire.send("x"));
}

test "messages come back as the bytes that went in, padding left behind" {
    var end: End = .{};
    try end.init();
    try end.in.inject(&.{ 4, 0, 'a', 'b', 'c', 'd' });
    try end.in.inject(&.{ 1, 0, 'e', 0, 0, 0 });

    var got: [16]u8 = undefined;
    const wire = end.link.transport();
    try testing.expectEqualSlices(u8, "abcde", got[0..try wire.receive(&got)]);
    try testing.expectEqual(@as(usize, 0), try wire.receive(&got));
    try testing.expectEqual(@as(usize, 0), wire.poll());
}

test "a receive too small for a message keeps the rest for the next one" {
    var end: End = .{};
    try end.init();
    try end.in.inject(&.{ 4, 0, 'a', 'b', 'c', 'd' });
    try end.in.inject(&.{ 2, 0, 'e', 'f', 0, 0 });

    var got: [3]u8 = undefined;
    const wire = end.link.transport();
    try testing.expect(wire.poll() != 0);
    try testing.expectEqualSlices(u8, "abc", got[0..try wire.receive(&got)]);
    try testing.expectEqualSlices(u8, "def", got[0..try wire.receive(&got)]);
    try testing.expectEqual(@as(usize, 0), try wire.receive(&got));
}

test "a length longer than the message is an error and nothing of it is read" {
    var end: End = .{};
    try end.init();
    try end.in.inject(&.{ 5, 0, 'a', 'b', 'c', 'd' });
    var got = [_]u8{0x7E} ** 8;
    try testing.expectError(error.BadMessage, end.link.transport().receive(&got));
    for (got) |byte| try testing.expectEqual(@as(u8, 0x7E), byte);
}

test "good bytes ahead of a bad message are delivered before the error" {
    var end: End = .{};
    try end.init();
    try end.in.inject(&.{ 2, 0, 'o', 'k', 0, 0 });
    try end.in.inject(&.{ 5, 0, 'a', 'b', 'c', 'd' });
    var got: [8]u8 = undefined;
    const wire = end.link.transport();
    try testing.expectEqualSlices(u8, "ok", got[0..try wire.receive(&got)]);
    try testing.expectError(error.BadMessage, wire.receive(&got));
}

test "a length of zero is an error too: no sender makes one" {
    var end: End = .{};
    try end.init();
    try end.in.inject(&.{ 0, 0, 0, 0, 0, 0 });
    var got: [8]u8 = undefined;
    try testing.expectError(error.BadMessage, end.link.transport().receive(&got));
}

test "after a bad message every call fails until the link is reset" {
    var end: End = .{};
    try end.init();
    const wire = end.link.transport();
    try end.in.inject(&.{ 0xFF, 0xFF, 0, 0, 0, 0 });
    try end.in.inject(&.{ 1, 0, 'a', 0, 0, 0 });

    var got: [8]u8 = undefined;
    try testing.expectError(error.BadMessage, wire.receive(&got));
    try testing.expectError(error.BadMessage, wire.receive(&got));
    try testing.expectError(error.BadMessage, wire.send("x"));
    try testing.expect(wire.poll() != 0);
    try testing.expectEqual(@as(usize, 0), end.out.count);

    end.link.reset();
    try testing.expectEqual(@as(usize, 0), end.in.count);
    try testing.expectEqual(@as(usize, 0), wire.poll());
    try end.in.inject(&.{ 1, 0, 'b', 0, 0, 0 });
    try testing.expectEqualSlices(u8, "b", got[0..try wire.receive(&got)]);
    try wire.send("x");
}

test "a queue that refuses part of a write ends the stream" {
    var end: End = .{};
    try end.init();
    const wire = end.link.transport();
    end.out.sends_left = 1;
    try testing.expectError(error.LinkDown, wire.send("abcdefgh"));
    try testing.expectEqual(@as(usize, 1), end.out.count);
    end.out.sends_left = null;
    try testing.expectError(error.LinkDown, wire.send("x"));
}

test "a queue that is gone is a link that is down" {
    var end: End = .{};
    try end.init();
    end.in.down = true;
    try end.in.inject(&.{ 1, 0, 'a', 0, 0, 0 });
    var got: [8]u8 = undefined;
    try testing.expectError(error.LinkDown, end.link.transport().receive(&got));
}

test "a queue whose messages cannot be packed is refused at init" {
    var store: [64]u8 = undefined;
    var tiny = MockQueue.init(&store, 2);
    var fine = MockQueue.init(&store, message_bytes);
    var scratch: [2][message_bytes]u8 = undefined;
    const Link = rpc.QueueTransport;
    try testing.expectError(
        error.MessageTooSmall,
        Link.init(tiny.queue(), fine.queue(), &scratch[0], &scratch[1]),
    );
    try testing.expectError(
        error.BufferTooSmall,
        Link.init(fine.queue(), fine.queue(), scratch[0][0..5], &scratch[1]),
    );
    try testing.expectError(
        error.BufferTooSmall,
        Link.init(fine.queue(), fine.queue(), &scratch[0], scratch[1][0..5]),
    );
}
