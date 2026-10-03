//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! A client and a server talking through two rings in one shared buffer,
//! and the way back after a ring has been damaged.

const std = @import("std");
const testing = std.testing;

const rpc = @import("ra8_rpc");
const messages = @import("messages.zig");
const service = @import("service.zig");
const MockSignal = @import("mock_signal.zig").MockSignal;
const Layout = rpc.ring.Layout;
const Env = service.Env;
const Method = service.Method;

comptime {
    _ = @import("messages.zig");
    _ = @import("mock_queue.zig");
    _ = @import("mock_signal.zig");
    _ = @import("service.zig");
}

const header = rpc.frame.Header.bytes;
/// An `add` request on the wire: the header, an id, a method, a length and
/// the two arguments.
const add_frame = header + 4 + 2 + 4 + 8;

/// Which ring carries which direction.
const up = 0;
const down = 1;

/// Both ends over two rings of `capacity` data bytes, side by side in one
/// buffer. Set up in place: the ends hold pointers into it.
fn Rig(comptime capacity: usize) type {
    return struct {
        const Self = @This();
        const ring_bytes = Layout.header_bytes + std.mem.alignForward(usize, capacity, 4);

        /// What the two cores share: the ring up to the server, then the
        /// ring back down to the client.
        shared: [2 * ring_bytes]u8 align(4) = undefined,
        /// The client's signal and the server's.
        signals: [2]MockSignal = .{ .{}, .{} },
        links: [2]rpc.RingTransport = undefined,
        board: service.Board = .{},
        client_rx: [Env.max_frame]u8 = undefined,
        server_rx: [Env.max_frame]u8 = undefined,
        tx: [Env.max_frame]u8 = undefined,
        client: service.Client = undefined,
        server: service.Server = undefined,

        fn mem(self: *Self, which: usize) []align(4) u8 {
            const offset = which * ring_bytes;
            return @alignCast(self.shared[offset..][0 .. Layout.header_bytes + capacity]);
        }

        fn ring(self: *Self, which: usize) rpc.Ring {
            return rpc.Ring.init(self.mem(which)) catch unreachable;
        }

        fn init(self: *Self) void {
            self.* = .{};
            self.ring(up).writeHeader();
            self.ring(down).writeHeader();
            const Link = rpc.RingTransport;
            self.links[0] = Link.init(self.ring(up), self.ring(down), self.signals[0].signal());
            self.links[1] = Link.init(self.ring(down), self.ring(up), self.signals[1].signal());
            self.start();
        }

        fn start(self: *Self) void {
            const Client = service.Client;
            const Server = service.Server;
            const caps = service.caps;
            self.client = Client.init(self.links[0].transport(), &self.client_rx, caps.client);
            self.server = Server.init(
                self.links[1].transport(),
                &self.server_rx,
                &self.board,
                caps.server,
            );
        }

        fn open(self: *Self) !void {
            try self.client.greet(&self.tx);
            try testing.expectEqual(rpc.Step.greeted, try self.server.poll(&self.tx));
            try testing.expectEqual(service.caps.server, (try self.client.poll(&self.tx)).?.ready);
        }

        /// The reset the README describes: with both cores stopped, one
        /// formats both rings, each clears its transport, and both sessions
        /// start again from the handshake.
        fn reset(self: *Self) !void {
            self.ring(up).writeHeader();
            self.ring(down).writeHeader();
            for (&self.links) |*link| link.reset();
            self.start();
            try self.open();
        }

        fn word(self: *Self, which: usize, offset: usize) u32 {
            return std.mem.readInt(u32, self.mem(which)[offset..][0..4], .little);
        }

        fn setWord(self: *Self, which: usize, offset: usize, value: u32) void {
            std.mem.writeInt(u32, self.mem(which)[offset..][0..4], value, .little);
        }

        fn add(self: *Self, a: u32, b: u32, waiter: usize) !u32 {
            const args: service.Add = .{ .a = a, .b = b };
            return self.client.call(service.Add, Method.add, args, waiter, &self.tx);
        }

        /// Poll the client for a response and return the sum it carries.
        fn sum(self: *Self) !u32 {
            const response = (try self.client.poll(&self.tx)).?.response;
            return (try rpc.codec.decode(service.Sum, response.result.ok)).sum;
        }

        /// One whole call: out, answered, back.
        fn roundTrip(self: *Self, a: u32, b: u32) !u32 {
            _ = try self.add(a, b, 0);
            try testing.expectEqual(rpc.Step.answered, try self.server.poll(&self.tx));
            return self.sum();
        }
    };
}

test "a call crosses one ring and its answer comes back on the other" {
    var rig: Rig(64) = .{};
    rig.init();
    try rig.open();
    try testing.expectEqual(@as(u32, 42), try rig.roundTrip(40, 2));

    // Both rings moved, and each sits wholly inside the one shared buffer.
    try testing.expect(rig.word(up, Layout.At.head) != 0);
    try testing.expect(rig.word(down, Layout.At.head) != 0);
    const first = @intFromPtr(rig.mem(up).ptr);
    const second = @intFromPtr(rig.mem(down).ptr);
    try testing.expect(first >= @intFromPtr(&rig.shared));
    try testing.expect(first + rig.mem(up).len <= second);
    try testing.expect(second + rig.mem(down).len <= @intFromPtr(&rig.shared) + rig.shared.len);
}

test "calls keep working as both rings wrap round again and again" {
    var rig: Rig(40) = .{};
    rig.init();
    try rig.open();

    var wraps = [_]usize{ 0, 0 };
    var heads = [_]u32{ rig.word(up, Layout.At.head), rig.word(down, Layout.At.head) };
    for (0..50) |i| {
        const n: u32 = @intCast(i);
        try testing.expectEqual(n + n * 3, try rig.roundTrip(n, n * 3));
        for (&wraps, &heads, 0..) |*count, *before, which| {
            const now = rig.word(which, Layout.At.head);
            count.* += @intFromBool(now < before.*);
            before.* = now;
        }
    }
    try testing.expect(wraps[up] > 10);
    try testing.expect(wraps[down] > 10);
}

test "a frame that exactly fills the ring goes through" {
    var rig: Rig(add_frame + 1) = .{};
    rig.init();
    try rig.open();
    _ = try rig.add(7, 8, 0);
    // Every byte the ring can hold is in use, and it took the frame whole.
    try testing.expectEqual(@as(usize, add_frame), rig.links[1].transport().poll());
    try testing.expectError(error.LinkFull, rig.links[0].transport().send("x"));

    try testing.expectEqual(rpc.Step.answered, try rig.server.poll(&rig.tx));
    try testing.expectEqual(@as(u32, 15), try rig.sum());
}

test "a frame one byte too long for the ring is refused" {
    var rig: Rig(add_frame) = .{};
    rig.init();
    try rig.open();
    try testing.expectError(error.LinkFull, rig.add(7, 8, 0));
    try testing.expectEqual(@as(usize, 0), rig.client.pending.count());
}

test "a full ring refuses the whole frame and is left exactly as it was" {
    var rig: Rig(40) = .{};
    rig.init();
    try rig.open();
    _ = try rig.add(1, 1, 1);

    var before: [Layout.header_bytes + 40]u8 = undefined;
    @memcpy(&before, rig.mem(up));
    try testing.expectError(error.LinkFull, rig.add(2, 2, 2));
    try testing.expectEqualSlices(u8, &before, rig.mem(up));
    try testing.expectEqual(@as(usize, 1), rig.client.pending.count());

    _ = try rig.server.poll(&rig.tx);
    try testing.expectEqual(@as(u32, 2), try rig.sum());
    try testing.expectEqual(@as(u32, 4), try rig.roundTrip(2, 2));
}

test "responses and events keep their order across the ring" {
    var rig: Rig(96) = .{};
    rig.init();
    try rig.open();
    _ = try rig.add(2, 3, 5);

    try rig.server.emit(service.Text, 9, .{ .text = "before" }, &rig.tx);
    _ = try rig.server.poll(&rig.tx);
    try rig.server.emit(service.Text, 9, .{ .text = "after" }, &rig.tx);

    const before = (try rig.client.poll(&rig.tx)).?.event;
    const first = try rpc.codec.decode(service.Text, before.payload);
    try testing.expectEqualSlices(u8, "before", first.text);
    try testing.expectEqual(@as(u32, 5), try rig.sum());
    const after = (try rig.client.poll(&rig.tx)).?.event;
    const last = try rpc.codec.decode(service.Text, after.payload);
    try testing.expectEqualSlices(u8, "after", last.text);
    try testing.expectEqual(null, try rig.client.poll(&rig.tx));
}

test "a corrupted header is an error to the session, and a reset brings it back" {
    for ([_]usize{ Layout.At.magic, Layout.At.version, Layout.At.capacity }) |field| {
        var rig: Rig(64) = .{};
        rig.init();
        try rig.open();
        try testing.expectEqual(@as(u32, 3), try rig.roundTrip(1, 2));

        rig.setWord(down, field, rig.word(down, field) ^ 0x0100);
        try testing.expectError(error.BadMessage, rig.client.poll(&rig.tx));
        try testing.expectError(error.BadMessage, rig.client.poll(&rig.tx));
        // The ring to the server is sound, but this end has failed as a whole.
        try testing.expectError(error.BadMessage, rig.add(1, 2, 0));
        try testing.expectEqual(@as(usize, 0), rig.client.pending.count());

        try rig.reset();
        try testing.expectEqual(@as(u32, 42), try rig.roundTrip(20, 22));
    }
}

test "an index outside the ring is an error on the side that reads it" {
    const capacity = 64;
    for ([_]u32{ capacity, std.math.maxInt(u32) }) |index| {
        var rig: Rig(capacity) = .{};
        rig.init();
        try rig.open();

        // The server's head, as the client reads it.
        rig.setWord(down, Layout.At.head, index);
        try testing.expectError(error.BadMessage, rig.client.poll(&rig.tx));
        try rig.reset();

        // The server's tail, as the client reads it before writing.
        rig.setWord(up, Layout.At.tail, index);
        try testing.expectError(error.BadMessage, rig.add(1, 2, 0));
        try testing.expectEqual(@as(usize, 0), rig.client.pending.count());
        // And the same damage, as the server finds it.
        try testing.expectError(error.BadMessage, rig.server.poll(&rig.tx));

        try rig.reset();
        try testing.expectEqual(@as(u32, 3), try rig.roundTrip(1, 2));
    }
}

test "the doorbell rings once for every frame sent and never for one refused" {
    var rig: Rig(40) = .{};
    rig.init();
    const client = &rig.signals[0];
    const server = &rig.signals[1];

    try rig.client.greet(&rig.tx);
    try testing.expectEqual(@as(usize, 1), client.notifies);
    _ = try rig.server.poll(&rig.tx);
    try testing.expectEqual(@as(usize, 1), server.notifies);
    _ = try rig.client.poll(&rig.tx);

    _ = try rig.add(1, 1, 0);
    try testing.expectEqual(@as(usize, 2), client.notifies);
    try testing.expectError(error.LinkFull, rig.add(2, 2, 0));
    try testing.expectEqual(@as(usize, 2), client.notifies);

    _ = try rig.server.poll(&rig.tx);
    try testing.expectEqual(@as(usize, 2), server.notifies);
    try rig.server.emit(service.Sum, 9, .{ .sum = 1 }, &rig.tx);
    try testing.expectEqual(@as(usize, 3), server.notifies);

    // Reading rings nothing, on either side.
    while (try rig.client.poll(&rig.tx)) |_| {}
    try testing.expectEqual(@as(usize, 2), client.notifies);
    try testing.expectEqual(@as(usize, 3), server.notifies);
}
