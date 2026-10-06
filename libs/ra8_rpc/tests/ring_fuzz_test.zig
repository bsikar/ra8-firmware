//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The ring under random traffic and under a random header.
//!
//! Sound header: sends and receives in any order and of any size behave as
//! a plain queue of `capacity - 1` bytes does. A send is refused exactly
//! when it does not fit, and the doorbell rings exactly once for each send
//! that carried anything.
//!
//! Random header: the transport either refuses it, and goes on refusing, or
//! the header was in fact a valid one. Either way nothing outside the ring's
//! own memory is written and nothing is handed to the caller that the
//! indices did not cover.
//!
//! The seeded loops run on every `zig build test`; the `std.testing.fuzz`
//! entry drives the random-header property under `zig build test --fuzz`.

const std = @import("std");
const testing = std.testing;

const rpc = @import("ra8_rpc");
const Layout = rpc.ring.Layout;
const MockSignal = @import("mock_signal.zig").MockSignal;

comptime {
    _ = @import("messages.zig");
    _ = @import("mock_queue.zig");
    _ = @import("mock_signal.zig");
    _ = @import("service.zig");
}

const Limits = struct {
    const max_capacity = 48;
    /// Bytes on each side of the ring that nothing may touch.
    const guard = 32;
    const guard_byte = 0xC5;
    const rounds = 4_000;
    const ops_per_round = 48;
};

/// A ring with guard bytes on both sides and a transport that writes and
/// reads it.
const Bench = struct {
    buf: [Limits.guard + Layout.header_bytes + Limits.max_capacity + Limits.guard]u8 align(4) =
        @splat(Limits.guard_byte),
    capacity: u32 = undefined,
    signal: MockSignal = .{},
    link: rpc.RingTransport = undefined,

    fn init(self: *Bench, capacity: u32) !void {
        self.capacity = capacity;
        const ring = try rpc.Ring.init(self.mem());
        ring.writeHeader();
        self.link = rpc.RingTransport.init(ring, ring, self.signal.signal());
    }

    fn mem(self: *Bench) []align(4) u8 {
        return @alignCast(self.buf[Limits.guard..][0 .. Layout.header_bytes + self.capacity]);
    }

    fn word(self: *Bench, offset: usize) u32 {
        return std.mem.readInt(u32, self.mem()[offset..][0..4], .little);
    }

    fn setWord(self: *Bench, offset: usize, value: u32) void {
        std.mem.writeInt(u32, self.mem()[offset..][0..4], value, .little);
    }

    /// Nothing before the ring or after it has been written.
    fn guardsHold(self: *Bench) !void {
        const end = Limits.guard + Layout.header_bytes + self.capacity;
        for (self.buf[0..Limits.guard]) |byte| try testing.expectEqual(Limits.guard_byte, byte);
        for (self.buf[end..]) |byte| try testing.expectEqual(Limits.guard_byte, byte);
    }

    /// Whether the header, read independently of the library, is one it
    /// should accept.
    fn sound(self: *Bench) bool {
        return self.word(Layout.At.magic) == Layout.magic and
            self.word(Layout.At.version) == Layout.version and
            self.word(Layout.At.capacity) == self.capacity and
            self.word(Layout.At.head) < self.capacity and
            self.word(Layout.At.tail) < self.capacity;
    }

    fn used(self: *Bench) u32 {
        const head = self.word(Layout.At.head);
        const tail = self.word(Layout.At.tail);
        return if (head >= tail) head - tail else self.capacity - tail + head;
    }
};

/// The random-header property. `ops` is one byte per step: its low bit
/// picks send or receive and the rest is the size. True if the header was
/// refused.
fn holds(bench: *Bench, ops: []const u8) !bool {
    const wire = bench.link.transport();
    const refused = !bench.sound();
    var scratch: [Limits.max_capacity + 2]u8 = @splat(0x3C);

    for (ops) |op| {
        const size = (op >> 1) % (bench.capacity + 2);
        const before = if (refused) 0 else bench.used();
        if (op & 1 == 0) {
            const result = wire.send(scratch[0..size]);
            if (refused) {
                try testing.expectError(error.BadMessage, result);
            } else if (size > bench.capacity - 1 - before) {
                try testing.expectError(error.LinkFull, result);
                try testing.expectEqual(before, bench.used());
            } else {
                try result;
                try testing.expectEqual(before + size, bench.used());
            }
        } else {
            const result = wire.receive(scratch[0..size]);
            if (refused) {
                try testing.expectError(error.BadMessage, result);
            } else {
                try testing.expectEqual(@min(size, before), try result);
                try testing.expectEqual(before - @min(size, before), bench.used());
            }
        }
        if (refused) try testing.expect(wire.poll() != 0);
        if (!refused) try testing.expect(bench.sound());
        try bench.guardsHold();
    }
    return refused;
}

test "random sends and receives agree with a plain queue" {
    var prng = std.Random.DefaultPrng.init(0x5241_3846_5735_3239);
    const random = prng.random();
    var refusals: usize = 0;
    var wraps: usize = 0;
    for (0..Limits.rounds) |_| {
        var bench: Bench = .{};
        try bench.init(random.intRangeAtMost(u32, Layout.min_capacity, Limits.max_capacity));
        // Start anywhere in the ring, so the end of the data is crossed early.
        const start = random.uintLessThan(u32, bench.capacity);
        bench.setWord(Layout.At.head, start);
        bench.setWord(Layout.At.tail, start);
        const wire = bench.link.transport();

        var queue: [Limits.max_capacity]u8 = undefined;
        var queued: usize = 0;
        var sends: usize = 0;
        for (0..Limits.ops_per_round) |_| {
            var bytes: [Limits.max_capacity + 2]u8 = undefined;
            const size = random.uintAtMost(usize, bench.capacity + 1);
            if (random.boolean()) {
                random.bytes(bytes[0..size]);
                const head = bench.word(Layout.At.head);
                wire.send(bytes[0..size]) catch |err| {
                    try testing.expectEqual(error.LinkFull, err);
                    try testing.expect(size > bench.capacity - 1 - queued);
                    try testing.expectEqual(head, bench.word(Layout.At.head));
                    refusals += 1;
                    continue;
                };
                try testing.expect(size <= bench.capacity - 1 - queued);
                @memcpy(queue[queued..][0..size], bytes[0..size]);
                queued += size;
                sends += @intFromBool(size != 0);
                wraps += @intFromBool(bench.word(Layout.At.head) < head);
            } else {
                const count = try wire.receive(bytes[0..size]);
                try testing.expectEqual(@min(size, queued), count);
                try testing.expectEqualSlices(u8, queue[0..count], bytes[0..count]);
                std.mem.copyForwards(u8, &queue, queue[count..queued]);
                queued -= count;
            }
            try testing.expectEqual(queued, wire.poll());
            try testing.expectEqual(sends, bench.signal.notifies);
            try testing.expect(bench.sound());
            try bench.guardsHold();
        }
    }
    try testing.expect(refusals > 0);
    try testing.expect(wraps > 0);
}

test "a header of random words is refused for good, or is a sound one" {
    var prng = std.Random.DefaultPrng.init(0x0529);
    const random = prng.random();
    var refused: usize = 0;
    for (0..Limits.rounds) |_| {
        var bench: Bench = .{};
        try bench.init(random.intRangeAtMost(u32, Layout.min_capacity, Limits.max_capacity));
        random.bytes(bench.mem()[Layout.header_bytes..]);
        // Damage one field in most rounds and leave the rest alone, so both
        // a refusal and a sound header are reached. Indices land in range
        // about half the time.
        const fields = [_]usize{
            Layout.At.magic, Layout.At.version, Layout.At.capacity,
            Layout.At.head,  Layout.At.tail,
        };
        for (fields) |field| {
            if (random.uintLessThan(u8, 3) != 0) continue;
            const near = random.uintLessThan(u32, 2 * bench.capacity);
            bench.setWord(field, if (random.boolean()) near else random.int(u32));
        }
        var ops: [Limits.ops_per_round]u8 = undefined;
        random.bytes(&ops);
        refused += @intFromBool(try holds(&bench, &ops));
    }
    try testing.expect(refused > 0 and refused < Limits.rounds);
}

test "fuzz: a ring header of any bytes is refused for good, or is a sound one" {
    const one = struct {
        fn one(_: void, input: []const u8) anyerror!void {
            if (input.len < 1 + Layout.header_bytes) return;
            const span = Limits.max_capacity - Layout.min_capacity + 1;
            var bench: Bench = .{};
            try bench.init(Layout.min_capacity + input[0] % span);
            @memcpy(bench.mem()[0..Layout.header_bytes], input[1..][0..Layout.header_bytes]);
            const ops = input[1 + Layout.header_bytes ..];
            _ = try holds(&bench, ops[0..@min(ops.len, Limits.ops_per_round)]);
        }
    }.one;
    // A sound header for an eight-byte ring, then one with its head past the
    // end, each followed by a few steps.
    const sound = [_]u8{6} ++ "RA8B".* ++ [_]u8{ 1, 0, 0, 0, 8, 0, 0, 0 } ++ @as([84]u8, @splat(0));
    var past = sound;
    past[1 + Layout.At.head] = 8;
    const steps = [_]u8{ 6, 3, 14, 9, 2 };
    try testing.fuzz({}, one, .{ .corpus = &.{ &(sound ++ steps), &(past ++ steps) } });
}
