//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The message packing under random traffic, in both directions.
//!
//! Going out: whatever is written comes back byte for byte, in the fewest
//! messages that hold it, however the reader's buffers cut it up.
//!
//! Coming in: messages of any content are either unpacked into exactly the
//! bytes their length fields claim, or the stream stops at the first bad
//! one. Nothing past a message's used length is ever handed on, and nothing
//! is handed on after a failure.
//!
//! The seeded loops run on every `zig build test`; the `std.testing.fuzz`
//! entry drives the incoming property under `zig build test --fuzz`.

const std = @import("std");
const testing = std.testing;

const rpc = @import("ra8_rpc");
const MockQueue = @import("mock_queue.zig").MockQueue;

comptime {
    _ = @import("messages.zig");
    _ = @import("mock_queue.zig");
    _ = @import("service.zig");
}

const Limits = struct {
    const used = 2;
    const max_message = 24;
    const depth = 16;
    const storage = max_message * depth;
    const rounds = 20_000;
};

/// A transport whose two queues are the same queue, so what it sends it
/// also receives.
const Echo = struct {
    store: [Limits.storage]u8 = undefined,
    queue: MockQueue = undefined,
    scratch: [2][Limits.max_message]u8 = undefined,
    link: rpc.QueueTransport = undefined,

    fn init(self: *Echo, message_bytes: usize) !void {
        self.queue = MockQueue.init(self.store[0 .. message_bytes * Limits.depth], message_bytes);
        const queue = self.queue.queue();
        self.link = try rpc.QueueTransport.init(queue, queue, &self.scratch[0], &self.scratch[1]);
    }
};

/// Read everything waiting into `into`, `chunk` bytes at a time, and return
/// how much arrived and the error that stopped it, if one did.
fn drain(wire: rpc.Transport, into: []u8, chunk: usize) struct { usize, ?rpc.Transport.Error } {
    var done: usize = 0;
    while (done < into.len) {
        const want = @min(chunk, into.len - done);
        const count = wire.receive(into[done..][0..want]) catch |err| return .{ done, err };
        if (count == 0) break;
        done += count;
    }
    return .{ done, null };
}

/// The incoming property, for `raw` cut into messages of `message_bytes`.
/// Returns true if the stream was stopped by a bad message.
fn unpacks(message_bytes: usize, raw: []const u8, chunk: usize) !bool {
    var echo: Echo = .{};
    try echo.init(message_bytes);
    const each = message_bytes - Limits.used;

    var want: [Limits.storage]u8 = undefined;
    var want_len: usize = 0;
    var bad = false;
    var rest = raw;
    while (rest.len >= message_bytes and echo.queue.count < Limits.depth) {
        const message = rest[0..message_bytes];
        rest = rest[message_bytes..];
        try echo.queue.inject(message);
        if (bad) continue;
        const length = std.mem.readInt(u16, message[0..Limits.used], .little);
        if (length == 0 or length > each) {
            bad = true;
            continue;
        }
        @memcpy(want[want_len..][0..length], message[Limits.used..][0..length]);
        want_len += length;
    }

    const wire = echo.link.transport();
    var got: [Limits.storage + 1]u8 = undefined;
    const count, const stopped = drain(wire, &got, chunk);
    try testing.expectEqualSlices(u8, want[0..want_len], got[0..count]);
    try testing.expectEqual(bad, stopped != null);
    if (bad) {
        try testing.expectEqual(error.BadMessage, stopped.?);
        try testing.expectError(error.BadMessage, wire.receive(&got));
        try testing.expectError(error.BadMessage, wire.send("x"));
        try testing.expect(wire.poll() != 0);
    } else {
        try testing.expectEqual(@as(usize, 0), wire.poll());
    }
    return bad;
}

test "what is written comes back the same, in the fewest messages" {
    var prng = std.Random.DefaultPrng.init(0x5241_3846_5735_3238);
    const random = prng.random();
    var refused: usize = 0;
    for (0..Limits.rounds) |_| {
        const message_bytes = random.intRangeAtMost(usize, Limits.used + 1, Limits.max_message);
        const each = message_bytes - Limits.used;
        var echo: Echo = .{};
        try echo.init(message_bytes);
        const wire = echo.link.transport();

        var sent: [Limits.storage]u8 = undefined;
        const bytes = sent[0..random.uintAtMost(usize, sent.len)];
        random.bytes(bytes);
        const need = bytes.len / each + @intFromBool(bytes.len % each != 0);

        wire.send(bytes) catch |err| {
            try testing.expectEqual(error.LinkFull, err);
            try testing.expect(need > Limits.depth);
            try testing.expectEqual(@as(usize, 0), echo.queue.count);
            refused += 1;
            continue;
        };
        try testing.expectEqual(need, echo.queue.count);

        var got: [Limits.storage + 1]u8 = undefined;
        const chunk = random.intRangeAtMost(usize, 1, 2 * message_bytes);
        const count, const stopped = drain(wire, &got, chunk);
        try testing.expectEqual(null, stopped);
        try testing.expectEqualSlices(u8, bytes, got[0..count]);
    }
    try testing.expect(refused > 0 and refused < Limits.rounds);
}

test "random messages unpack to exactly their used bytes or stop the stream" {
    var prng = std.Random.DefaultPrng.init(0x0528);
    const random = prng.random();
    var stopped: usize = 0;
    for (0..Limits.rounds) |_| {
        const message_bytes = random.intRangeAtMost(usize, Limits.used + 1, Limits.max_message);
        var raw: [Limits.storage]u8 = undefined;
        const bytes = raw[0 .. random.uintAtMost(usize, Limits.depth) * message_bytes];
        random.bytes(bytes);
        // Random length fields are nearly always too big. Make most of them
        // ones a sender could have written, so both outcomes are reached.
        var at: usize = 0;
        while (at < bytes.len) : (at += message_bytes) {
            if (random.uintLessThan(u8, 8) == 0) continue;
            const length = random.intRangeAtMost(u16, 1, @intCast(message_bytes - Limits.used));
            std.mem.writeInt(u16, bytes[at..][0..Limits.used], length, .little);
        }
        const chunk = random.intRangeAtMost(usize, 1, 2 * message_bytes);
        stopped += @intFromBool(try unpacks(message_bytes, bytes, chunk));
    }
    try testing.expect(stopped > 0 and stopped < Limits.rounds);
}

test "fuzz: queue messages unpack to their used bytes or stop the stream" {
    const one = struct {
        fn one(_: void, input: []const u8) anyerror!void {
            if (input.len < 2) return;
            const span = Limits.max_message - Limits.used;
            const message_bytes = Limits.used + 1 + input[0] % span;
            _ = try unpacks(message_bytes, input[2..], 1 + input[1] % (2 * message_bytes));
        }
    }.one;
    const corpus = [_][]const u8{
        &.{ 3, 4, 4, 0, 'a', 'b', 'c', 'd', 1, 0, 'e', 0, 0, 0 },
        &.{ 3, 9, 5, 0, 'a', 'b', 'c', 'd' },
        &.{ 3, 1, 0, 0, 0, 0, 0, 0 },
    };
    try testing.fuzz({}, one, .{ .corpus = &corpus });
}
