//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! A client and a server over two ThreadX queues, through the same
//! `QueueTransport` the mocked-queue tests in `ra8_rpc` use.

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

const max_body = 32;
const Env = rpc.Envelope(max_body);
const Add = struct { a: u32, b: u32 };
const Sum = struct { sum: u32 };
const Board = struct { calls: u32 = 0 };
const method_add = 1;

fn add(board: *Board, args: Add) rpc.Outcome(Sum) {
    board.calls += 1;
    return .{ .ok = .{ .sum = args.a +% args.b } };
}

const Client = rpc.Client(2, max_body);
const Server = rpc.Server(Board, max_body, .{.{ method_add, add }});

/// Two-word messages: a two-byte length and six bytes of the stream.
const words = 2;
const message_bytes = words * 4;
const per_message = message_bytes - 2;
/// A hello is 16 bytes and an add request 24.
const hello_messages = 3;
const add_messages = 4;

/// Which queue carries which direction. Each has one sender.
const up = 0;
const down = 1;

/// Both ends over two fake ThreadX queues of `depth` messages. Set up in
/// place: the ends hold pointers into it.
fn Rig(comptime depth: usize) type {
    return struct {
        const Self = @This();

        storage: [2][depth * words]u32 = undefined,
        kernel: [2]FakeQueue = undefined,
        /// Each end binds both queues: the one it sends on, then the one it
        /// receives from.
        client_queues: [2]TxQueue = undefined,
        server_queues: [2]TxQueue = undefined,
        scratch: [4][message_bytes]u8 = undefined,
        links: [2]rpc.QueueTransport = undefined,
        board: Board = .{},
        client_rx: [Env.max_frame]u8 = undefined,
        server_rx: [Env.max_frame]u8 = undefined,
        tx: [Env.max_frame]u8 = undefined,
        client: Client = undefined,
        server: Server = undefined,

        fn init(self: *Self) !void {
            self.* = .{};
            for (&self.kernel, &self.storage) |*kernel, *storage| {
                kernel.* = FakeQueue.init(storage, words);
            }
            const up_handle = self.kernel[up].handle();
            const down_handle = self.kernel[down].handle();
            self.client_queues = .{
                try TxQueue.init(up_handle, message_bytes),
                try TxQueue.init(down_handle, message_bytes),
            };
            self.server_queues = .{
                try TxQueue.init(down_handle, message_bytes),
                try TxQueue.init(up_handle, message_bytes),
            };
            const Link = rpc.QueueTransport;
            self.links[0] = try Link.init(
                self.client_queues[0].queue(),
                self.client_queues[1].queue(),
                &self.scratch[0],
                &self.scratch[1],
            );
            self.links[1] = try Link.init(
                self.server_queues[0].queue(),
                self.server_queues[1].queue(),
                &self.scratch[2],
                &self.scratch[3],
            );
            self.start();
        }

        fn start(self: *Self) void {
            self.client = Client.init(self.links[0].transport(), &self.client_rx, 1);
            self.server = Server.init(self.links[1].transport(), &self.server_rx, &self.board, 2);
        }

        fn open(self: *Self) !void {
            try self.client.greet(&self.tx);
            try testing.expectEqual(rpc.Step.greeted, try self.server.poll(&self.tx));
            try testing.expectEqual(@as(u32, 2), (try self.client.poll(&self.tx)).?.ready);
        }

        /// The reset the README describes: empty both ThreadX queues, clear
        /// every binding and transport, and start both sessions again.
        fn reset(self: *Self) !void {
            for (&self.kernel) |*kernel| {
                kernel.head = 0;
                kernel.count = 0;
            }
            for (&self.client_queues, &self.server_queues) |*near, *far| {
                near.clear();
                far.clear();
            }
            for (&self.links) |*link| link.reset();
            self.start();
            try self.open();
        }

        fn call(self: *Self, a: u32, b: u32) !u32 {
            return self.client.call(Add, method_add, .{ .a = a, .b = b }, 0, &self.tx);
        }

        fn sum(self: *Self) !u32 {
            const response = (try self.client.poll(&self.tx)).?.response;
            return (try rpc.codec.decode(Sum, response.result.ok)).sum;
        }
    };
}

test "a call goes out on one ThreadX queue and its answer back on the other" {
    var rig: Rig(16) = .{};
    try rig.init();
    try rig.open();
    _ = try rig.call(40, 2);
    try testing.expectEqual(rpc.Step.answered, try rig.server.poll(&rig.tx));
    try testing.expectEqual(@as(u32, 42), try rig.sum());
    try testing.expectEqual(@as(u32, 1), rig.board.calls);
}

test "a frame longer than a queue message crosses in several and arrives whole" {
    var rig: Rig(16) = .{};
    try rig.init();
    try rig.client.greet(&rig.tx);
    try testing.expectEqual(@as(usize, hello_messages), rig.kernel[up].count);
    // Full messages carry six bytes; the last carries what is left.
    const first = std.mem.sliceAsBytes(rig.kernel[up].message(0));
    const last = std.mem.sliceAsBytes(rig.kernel[up].message(hello_messages - 1));
    const full = std.mem.readInt(u16, first[0..2], .little);
    const rest = std.mem.readInt(u16, last[0..2], .little);
    try testing.expectEqual(@as(u16, per_message), full);
    try testing.expectEqual(@as(u16, 16 - 2 * per_message), rest);

    _ = try rig.server.poll(&rig.tx);
    _ = try rig.client.poll(&rig.tx);
    _ = try rig.call(7, 8);
    try testing.expectEqual(@as(usize, add_messages), rig.kernel[up].count);
    _ = try rig.server.poll(&rig.tx);
    try testing.expectEqual(@as(u32, 15), try rig.sum());
    try testing.expectEqual(@as(usize, 0), rig.kernel[up].count);
    try testing.expectEqual(@as(usize, 0), rig.kernel[down].count);
}

test "a ThreadX queue without room for the whole frame takes none of it" {
    // Seven messages deep: one request leaves three free, and the next
    // needs four.
    var rig: Rig(7) = .{};
    try rig.init();
    try rig.open();
    _ = try rig.call(1, 1);
    const sends = rig.kernel[up].sends;

    try testing.expectError(error.LinkFull, rig.call(2, 2));
    try testing.expectEqual(@as(usize, add_messages), rig.kernel[up].count);
    try testing.expectEqual(sends, rig.kernel[up].sends);
    try testing.expectEqual(@as(usize, 1), rig.client.pending.count());

    _ = try rig.server.poll(&rig.tx);
    try testing.expectEqual(@as(u32, 2), try rig.sum());
    _ = try rig.call(2, 2);
    _ = try rig.server.poll(&rig.tx);
    try testing.expectEqual(@as(u32, 4), try rig.sum());
}

test "responses and events keep their order across the queue" {
    var rig: Rig(32) = .{};
    try rig.init();
    try rig.open();
    _ = try rig.call(2, 3);
    try rig.server.emit(Sum, 9, .{ .sum = 100 }, &rig.tx);
    _ = try rig.server.poll(&rig.tx);
    try rig.server.emit(Sum, 9, .{ .sum = 200 }, &rig.tx);

    const before = (try rig.client.poll(&rig.tx)).?.event;
    try testing.expectEqual(@as(u32, 100), (try rpc.codec.decode(Sum, before.payload)).sum);
    try testing.expectEqual(@as(u32, 5), try rig.sum());
    const after = (try rig.client.poll(&rig.tx)).?.event;
    try testing.expectEqual(@as(u32, 200), (try rpc.codec.decode(Sum, after.payload)).sum);
    try testing.expectEqual(null, try rig.client.poll(&rig.tx));
}

test "an unexpected ThreadX status is an error to the session, and a reset brings it back" {
    var rig: Rig(16) = .{};
    try rig.init();
    try rig.open();

    rig.kernel[up].force_send = fake.Status.deleted;
    try testing.expectError(error.LinkDown, rig.call(1, 2));
    try testing.expectEqual(@as(usize, 0), rig.client.pending.count());
    try testing.expectEqual(fake.Status.deleted, rig.client_queues[0].failure.?);

    // The queue would work again, but nothing is retried behind the caller.
    rig.kernel[up].force_send = null;
    const sends = rig.kernel[up].sends;
    try testing.expectError(error.LinkDown, rig.call(1, 2));
    try testing.expectError(error.LinkDown, rig.client.poll(&rig.tx));
    try testing.expectEqual(sends, rig.kernel[up].sends);

    try rig.reset();
    _ = try rig.call(20, 22);
    _ = try rig.server.poll(&rig.tx);
    try testing.expectEqual(@as(u32, 42), try rig.sum());
}

test "a queue deleted under the server is an error on the server's side" {
    var rig: Rig(16) = .{};
    try rig.init();
    try rig.open();
    _ = try rig.call(1, 2);
    rig.kernel[up].delete();
    try testing.expectError(error.LinkDown, rig.server.poll(&rig.tx));
    try testing.expectError(error.LinkDown, rig.server.poll(&rig.tx));
    try testing.expectEqual(@as(u32, 0), rig.board.calls);
}
