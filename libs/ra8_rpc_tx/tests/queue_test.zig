//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! One ThreadX queue behind the queue interface: what each ThreadX answer
//! becomes, and what the binding refuses to be built over.

const std = @import("std");
const testing = std.testing;

const rpc = @import("ra8_rpc");
const rpc_tx = @import("ra8_rpc_tx");
const fake = @import("fake_threadx.zig");
const FakeQueue = fake.FakeQueue;
const TxQueue = rpc_tx.TxQueue(fake.api);

comptime {
    _ = @import("fake_threadx.zig");
}

/// Two-word messages, four of them.
const words = 2;
const message_bytes = words * 4;
const depth = 4;

/// A fake ThreadX queue and the binding over it.
const Bench = struct {
    storage: [depth * words]u32 = undefined,
    kernel: FakeQueue = undefined,
    bound: TxQueue = undefined,

    fn init(self: *Bench) !void {
        self.kernel = FakeQueue.init(&self.storage, words);
        self.bound = try TxQueue.init(self.kernel.handle(), message_bytes);
    }
};

test "a message size ThreadX cannot have is refused when the binding is built" {
    var storage: [16]u32 = undefined;
    var kernel = FakeQueue.init(&storage, 1);
    for ([_]usize{ 0, 1, 2, 3, 5, 6, 7, 62, 63, 65, 68, 128 }) |bytes| {
        try testing.expectError(error.BadMessageSize, TxQueue.init(kernel.handle(), bytes));
    }
    var bytes: usize = 4;
    while (bytes <= 64) : (bytes += 4) {
        const bound = try TxQueue.init(kernel.handle(), bytes);
        try testing.expectEqual(bytes, bound.message_bytes);
    }
}

test "the queue reports the message size it was bound with" {
    var bench: Bench = .{};
    try bench.init();
    try testing.expectEqual(@as(usize, message_bytes), bench.bound.queue().message_bytes);
}

test "a send puts the message on the ThreadX queue, byte for byte" {
    var bench: Bench = .{};
    try bench.init();
    try bench.bound.queue().send("abcdefgh");
    try testing.expectEqual(@as(usize, 1), bench.kernel.count);
    try testing.expectEqualSlices(u8, "abcdefgh", std.mem.sliceAsBytes(bench.kernel.message(0)));
}

test "a receive takes the oldest message, byte for byte" {
    var bench: Bench = .{};
    try bench.init();
    const queue = bench.bound.queue();
    try queue.send("first!!!");
    try queue.send("second!!");

    var got: [message_bytes]u8 = undefined;
    try testing.expect(try queue.receive(&got));
    try testing.expectEqualSlices(u8, "first!!!", &got);
    try testing.expect(try queue.receive(&got));
    try testing.expectEqualSlices(u8, "second!!", &got);
}

test "the counts are ThreadX's own" {
    var bench: Bench = .{};
    try bench.init();
    const queue = bench.bound.queue();
    try testing.expectEqual(@as(usize, 0), queue.waiting());
    try testing.expectEqual(@as(usize, depth), queue.free());
    try queue.send("abcdefgh");
    try testing.expectEqual(@as(usize, 1), queue.waiting());
    try testing.expectEqual(@as(usize, depth - 1), queue.free());
}

test "a full queue is QueueFull: nothing is lost and the queue is not down" {
    var bench: Bench = .{};
    try bench.init();
    const queue = bench.bound.queue();
    for (0..depth) |_| try queue.send("abcdefgh");
    try testing.expectError(error.QueueFull, queue.send("12345678"));
    try testing.expectEqual(@as(usize, depth), bench.kernel.count);
    try testing.expectEqual(null, bench.bound.failure);

    var got: [message_bytes]u8 = undefined;
    try testing.expect(try queue.receive(&got));
    try queue.send("12345678");
}

test "an empty queue is no message, and the buffer is left alone" {
    var bench: Bench = .{};
    try bench.init();
    var got = [_]u8{0x7E} ** message_bytes;
    try testing.expect(!try bench.bound.queue().receive(&got));
    for (got) |byte| try testing.expectEqual(@as(u8, 0x7E), byte);
    try testing.expectEqual(null, bench.bound.failure);
}

test "any other status on send takes the queue down and is not retried" {
    var bench: Bench = .{};
    try bench.init();
    const queue = bench.bound.queue();
    bench.kernel.force_send = fake.Status.deleted;

    try testing.expectError(error.QueueDown, queue.send("abcdefgh"));
    try testing.expectEqual(fake.Status.deleted, bench.bound.failure.?);
    try testing.expectEqual(@as(usize, 1), bench.kernel.sends);
    try testing.expectEqual(@as(usize, 0), bench.kernel.count);

    // ThreadX would take the next one. The binding does not ask.
    bench.kernel.force_send = null;
    var got: [message_bytes]u8 = undefined;
    try testing.expectError(error.QueueDown, queue.send("abcdefgh"));
    try testing.expectError(error.QueueDown, queue.receive(&got));
    try testing.expectEqual(@as(usize, 1), bench.kernel.sends);
    try testing.expectEqual(@as(usize, 0), bench.kernel.receives);
}

test "any other status on receive takes the queue down and hands nothing over" {
    var bench: Bench = .{};
    try bench.init();
    const queue = bench.bound.queue();
    try queue.send("abcdefgh");
    bench.kernel.force_receive = fake.Status.queue_error;

    var got = [_]u8{0x7E} ** message_bytes;
    try testing.expectError(error.QueueDown, queue.receive(&got));
    for (got) |byte| try testing.expectEqual(@as(u8, 0x7E), byte);
    try testing.expectEqual(fake.Status.queue_error, bench.bound.failure.?);
    try testing.expectEqual(@as(usize, 1), bench.kernel.count);
}

test "a deleted queue is down from whichever call finds it first" {
    var bench: Bench = .{};
    try bench.init();
    bench.kernel.delete();
    try testing.expectError(error.QueueDown, bench.bound.queue().send("abcdefgh"));
    try testing.expectEqual(fake.Status.queue_error, bench.bound.failure.?);
}

test "a count ThreadX refuses to give is reported by the next send or receive" {
    var bench: Bench = .{};
    try bench.init();
    const queue = bench.bound.queue();
    bench.kernel.force_info = fake.Status.queue_error;

    // Neither count reads as "nothing to do", so the caller goes on to
    // the call that can tell it what happened.
    try testing.expect(queue.free() != 0);
    try testing.expect(queue.waiting() != 0);
    try testing.expectEqual(@as(usize, 1), bench.kernel.infos);
    var got: [message_bytes]u8 = undefined;
    try testing.expectError(error.QueueDown, queue.send("abcdefgh"));
    try testing.expectError(error.QueueDown, queue.receive(&got));
    try testing.expectEqual(@as(usize, 0), bench.kernel.sends);
}

test "clear forgets the failure once the queue has been dealt with" {
    var bench: Bench = .{};
    try bench.init();
    const queue = bench.bound.queue();
    bench.kernel.force_send = fake.Status.deleted;
    try testing.expectError(error.QueueDown, queue.send("abcdefgh"));

    bench.kernel.force_send = null;
    bench.bound.clear();
    try testing.expectEqual(null, bench.bound.failure);
    try queue.send("abcdefgh");
    try testing.expectEqual(@as(usize, 1), queue.waiting());
}

test "every call into ThreadX is made without waiting" {
    // The fake answers a wait option other than TX_NO_WAIT with
    // TX_WAIT_ERROR, which the binding would turn into QueueDown.
    var bench: Bench = .{};
    try bench.init();
    const queue = bench.bound.queue();
    var got: [message_bytes]u8 = undefined;
    try queue.send("abcdefgh");
    try testing.expect(try queue.receive(&got));
    try testing.expect(!try queue.receive(&got));
    try testing.expectEqual(null, bench.bound.failure);
}

test "ThreadX is handed whole aligned words, whatever the caller's buffer" {
    var bench: Bench = .{};
    try bench.init();
    const queue = bench.bound.queue();
    var odd: [message_bytes + 1]u8 align(4) = undefined;
    @memcpy(odd[1..], "abcdefgh");

    try queue.send(odd[1..]);
    @memset(&odd, 0);
    try testing.expect(try queue.receive(odd[1..]));
    try testing.expectEqualSlices(u8, "abcdefgh", odd[1..]);
    try testing.expectEqual(null, bench.bound.failure);
}

test "the largest ThreadX message goes through whole" {
    var storage: [2 * 16]u32 = undefined;
    var kernel = FakeQueue.init(&storage, 16);
    var bound = try TxQueue.init(kernel.handle(), 64);
    var sent: [64]u8 = undefined;
    for (&sent, 0..) |*byte, i| byte.* = @intCast(i);

    try bound.queue().send(&sent);
    var got: [64]u8 = undefined;
    try testing.expect(try bound.queue().receive(&got));
    try testing.expectEqualSlices(u8, &sent, &got);
}
