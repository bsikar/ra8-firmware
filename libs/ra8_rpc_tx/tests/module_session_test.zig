//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! A client inside a module and a server in the resident image, over two
//! ThreadX queues: the client's end goes through the dispatcher and the
//! server's calls the services directly.

const std = @import("std");
const testing = std.testing;

const rpc = @import("ra8_rpc");
const rpc_tx = @import("ra8_rpc_tx");
const module = rpc_tx.module;
const fake = @import("fake_threadx.zig");
const dispatcher = @import("fake_dispatcher.zig");
const FakeQueue = fake.FakeQueue;
/// The module's end, and the resident image's.
const ModuleQueue = rpc_tx.TxQueue(module.api);
const ResidentQueue = rpc_tx.TxQueue(fake.api);

comptime {
    _ = @import("fake_dispatcher.zig");
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

/// Two-word messages: a two-byte length and six bytes of the stream, so a
/// 16-byte hello takes three and a 24-byte request four.
const words = 2;
const message_bytes = words * 4;
const hello_messages = 3;
const add_messages = 4;

const up = 0;
const down = 1;

/// Set up in place: the ends hold pointers into it.
fn Rig(comptime depth: usize) type {
    return struct {
        const Self = @This();

        storage: [2][depth * words]u32 = undefined,
        kernel: [2]FakeQueue = undefined,
        /// The module's references to the two queues.
        refs: [2]module.QueueRef = undefined,
        client_queues: [2]ModuleQueue = undefined,
        server_queues: [2]ResidentQueue = undefined,
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
            dispatcher.clear();
            for (&self.kernel, &self.storage, &self.refs) |*kernel, *storage, *ref| {
                kernel.* = FakeQueue.init(storage, words);
                ref.* = .{ .dispatcher = dispatcher.dispatch, .queue = kernel.handle() };
            }
            self.client_queues = .{
                try ModuleQueue.init(self.refs[up].handle(), message_bytes),
                try ModuleQueue.init(self.refs[down].handle(), message_bytes),
            };
            self.server_queues = .{
                try ResidentQueue.init(self.kernel[down].handle(), message_bytes),
                try ResidentQueue.init(self.kernel[up].handle(), message_bytes),
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
            self.client = Client.init(self.links[0].transport(), &self.client_rx, 1);
            self.server = Server.init(self.links[1].transport(), &self.server_rx, &self.board, 2);
        }

        fn open(self: *Self) !void {
            try self.client.greet(&self.tx);
            try testing.expectEqual(rpc.Step.greeted, try self.server.poll(&self.tx));
            try testing.expectEqual(@as(u32, 2), (try self.client.poll(&self.tx)).?.ready);
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

/// How many of the requests so far were `request`.
fn requests(request: rpc_tx.api.Ulong) usize {
    var total: usize = 0;
    for (dispatcher.calls()) |call| total += @intFromBool(call.request == request);
    return total;
}

test "a module's call reaches the resident server and its answer comes back" {
    var rig: Rig(16) = .{};
    try rig.init();
    try rig.open();
    _ = try rig.call(40, 2);
    try testing.expectEqual(rpc.Step.answered, try rig.server.poll(&rig.tx));
    try testing.expectEqual(@as(u32, 42), try rig.sum());
    try testing.expectEqual(@as(u32, 1), rig.board.calls);
}

test "a frame longer than a queue message goes through the dispatcher once per message" {
    var rig: Rig(16) = .{};
    try rig.init();
    try rig.client.greet(&rig.tx);
    try testing.expectEqual(@as(usize, hello_messages), rig.kernel[up].count);
    try testing.expectEqual(@as(usize, hello_messages), requests(module.Request.queue_send));

    _ = try rig.server.poll(&rig.tx);
    _ = try rig.client.poll(&rig.tx);
    _ = try rig.call(7, 8);
    try testing.expectEqual(@as(usize, add_messages), rig.kernel[up].count);
    try testing.expectEqual(
        @as(usize, hello_messages + add_messages),
        requests(module.Request.queue_send),
    );
    _ = try rig.server.poll(&rig.tx);
    try testing.expectEqual(@as(u32, 15), try rig.sum());
    try testing.expectEqual(@as(usize, 0), rig.kernel[up].count);
    try testing.expectEqual(@as(usize, 0), rig.kernel[down].count);
}

test "every request the module made named one of its two queues" {
    var rig: Rig(16) = .{};
    try rig.init();
    try rig.open();
    _ = try rig.call(1, 2);
    _ = try rig.server.poll(&rig.tx);
    _ = try rig.sum();

    const to_server: usize = @intFromPtr(rig.kernel[up].handle());
    const to_client: usize = @intFromPtr(rig.kernel[down].handle());
    try testing.expect(dispatcher.calls().len > 0);
    for (dispatcher.calls()) |made| {
        const queue: usize = @intCast(made.params[0]);
        switch (made.request) {
            // The module only ever sends up and receives down.
            module.Request.queue_send => try testing.expectEqual(to_server, queue),
            module.Request.queue_receive => try testing.expectEqual(to_client, queue),
            module.Request.queue_info_get => try testing.expect(
                queue == to_server or queue == to_client,
            ),
            else => return error.TestUnexpectedResult,
        }
    }
}

test "a queue without room for the module's whole frame takes none of it" {
    var rig: Rig(7) = .{};
    try rig.init();
    try rig.open();
    _ = try rig.call(1, 1);
    const sent = requests(module.Request.queue_send);

    try testing.expectError(error.LinkFull, rig.call(2, 2));
    try testing.expectEqual(@as(usize, add_messages), rig.kernel[up].count);
    try testing.expectEqual(sent, requests(module.Request.queue_send));
    try testing.expectEqual(@as(usize, 1), rig.client.pending.count());
}

test "an unexpected status from the dispatcher is an error to the module's session" {
    var rig: Rig(16) = .{};
    try rig.init();
    try rig.open();
    rig.kernel[up].force_send = fake.Status.deleted;
    try testing.expectError(error.LinkDown, rig.call(1, 2));
    try testing.expectEqual(fake.Status.deleted, rig.client_queues[0].failure.?);

    rig.kernel[up].force_send = null;
    const asked = dispatcher.calls().len;
    try testing.expectError(error.LinkDown, rig.call(1, 2));
    try testing.expectEqual(asked, dispatcher.calls().len);
}
