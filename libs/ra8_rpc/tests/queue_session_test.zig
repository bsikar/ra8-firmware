//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! A client and a server talking through two message queues, and the way
//! back after the link has failed.

const std = @import("std");
const testing = std.testing;

const rpc = @import("ra8_rpc");
const messages = @import("messages.zig");
const service = @import("service.zig");
const MockQueue = @import("mock_queue.zig").MockQueue;
const Env = service.Env;
const Method = service.Method;

comptime {
    _ = @import("messages.zig");
    _ = @import("mock_queue.zig");
    _ = @import("service.zig");
}

const header = rpc.frame.Header.bytes;
const used = 2;

/// An `add` request on the wire: the header, an id, a method, a length and
/// the two arguments.
const add_frame = header + 4 + 2 + 4 + 8;
/// A hello on the wire.
const hello_frame = header + rpc.codec.maxSize(rpc.Hello);

/// Both ends over two queues of `depth` messages of `message_bytes` each.
/// Set up in place: the ends hold pointers into it.
fn Rig(comptime message_bytes: usize, comptime depth: usize) type {
    return struct {
        const Self = @This();

        store: [2][depth * message_bytes]u8 = undefined,
        /// `queues[0]` carries client to server, `queues[1]` the other way.
        queues: [2]MockQueue = undefined,
        scratch: [4][message_bytes]u8 = undefined,
        links: [2]rpc.QueueTransport = undefined,
        board: service.Board = .{},
        client_rx: [Env.max_frame]u8 = undefined,
        server_rx: [Env.max_frame]u8 = undefined,
        tx: [Env.max_frame]u8 = undefined,
        client: service.Client = undefined,
        server: service.Server = undefined,

        fn init(self: *Self) !void {
            self.* = .{};
            for (&self.queues, &self.store) |*queue, *store| {
                queue.* = MockQueue.init(store, message_bytes);
            }
            const up = self.queues[0].queue();
            const down = self.queues[1].queue();
            const Link = rpc.QueueTransport;
            self.links[0] = try Link.init(up, down, &self.scratch[0], &self.scratch[1]);
            self.links[1] = try Link.init(down, up, &self.scratch[2], &self.scratch[3]);
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

        /// The reset the README describes: both ends stop, both transports
        /// are reset, and both sessions start again from the handshake.
        fn reset(self: *Self) !void {
            for (&self.links) |*link| link.reset();
            self.start();
            try self.open();
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
    };
}

test "a frame longer than a message crosses in several and arrives whole" {
    var rig: Rig(8, 16) = .{};
    try rig.init();
    try rig.client.greet(&rig.tx);
    try testing.expectEqual(@as(usize, 3), rig.queues[0].count);
    try testing.expectEqual(rpc.Step.greeted, try rig.server.poll(&rig.tx));
    try testing.expectEqual(service.caps.server, (try rig.client.poll(&rig.tx)).?.ready);

    _ = try rig.add(40, 2, 0);
    try testing.expectEqual(@as(usize, 4), rig.queues[0].count);
    try testing.expectEqual(rpc.Step.answered, try rig.server.poll(&rig.tx));
    try testing.expectEqual(@as(u32, 42), try rig.sum());
}

test "a frame is not acted on until its last message has arrived" {
    var rig: Rig(8, 16) = .{};
    try rig.init();
    try rig.open();

    var args: [8]u8 = undefined;
    _ = try rpc.codec.encode(service.Add, .{ .a = 1, .b = 2 }, &args);
    const request: Env.Request = .{ .id = 1, .method = Method.add, .args = &args };
    const bytes = try rpc.frame.encode(Env.Request, rpc.Kind.request, request, &rig.tx);

    // The sender gets the first three messages out and is then held up.
    const early = 3 * (8 - used);
    const wire = rig.links[0].transport();
    try wire.send(bytes[0..early]);
    try testing.expectEqual(rpc.Step.idle, try rig.server.poll(&rig.tx));
    try testing.expectEqual(@as(u32, 0), rig.board.calls);

    try wire.send(bytes[early..]);
    try testing.expectEqual(rpc.Step.answered, try rig.server.poll(&rig.tx));
    try testing.expectEqual(@as(u32, 1), rig.board.calls);
}

test "a frame exactly one message long takes one message and no padding" {
    var rig: Rig(add_frame + used, 4) = .{};
    try rig.init();
    try rig.open();
    _ = try rig.add(7, 8, 0);
    try testing.expectEqual(@as(usize, 1), rig.queues[0].count);
    const at = rig.queues[0].head * (add_frame + used);
    const length = std.mem.readInt(u16, rig.store[0][at..][0..used], .little);
    try testing.expectEqual(@as(u16, add_frame), length);

    try testing.expectEqual(rpc.Step.answered, try rig.server.poll(&rig.tx));
    try testing.expectEqual(@as(u32, 15), try rig.sum());
}

test "a frame one byte longer than a message takes two" {
    var rig: Rig(add_frame + used - 1, 4) = .{};
    try rig.init();
    try rig.open();
    _ = try rig.add(7, 8, 0);
    try testing.expectEqual(@as(usize, 2), rig.queues[0].count);
    _ = try rig.server.poll(&rig.tx);
    try testing.expectEqual(@as(u32, 15), try rig.sum());
}

test "a queue without room for the whole frame takes none of it" {
    // Three messages a hello, four a request: after the hello is answered
    // and one request is queued, a second does not fit in what is left.
    var rig: Rig(8, 7) = .{};
    try rig.init();
    try rig.open();
    _ = try rig.add(1, 1, 1);
    try testing.expectEqual(@as(usize, 4), rig.queues[0].count);

    try testing.expectError(error.LinkFull, rig.add(2, 2, 2));
    try testing.expectEqual(@as(usize, 4), rig.queues[0].count);
    try testing.expectEqual(@as(usize, 1), rig.client.pending.count());

    _ = try rig.server.poll(&rig.tx);
    try testing.expectEqual(@as(u32, 2), try rig.sum());
    _ = try rig.add(2, 2, 2);
    _ = try rig.server.poll(&rig.tx);
    try testing.expectEqual(@as(u32, 4), try rig.sum());
}

test "a server that cannot queue its whole answer queues none of it" {
    var rig: Rig(8, 16) = .{};
    try rig.init();
    try rig.open();
    _ = try rig.add(1, 1, 0);
    // Fill the way back until there is one free message.
    const junk = [_]u8{ 1, 0, 0xEE, 0, 0, 0, 0, 0 };
    while (rig.queues[1].count < rig.queues[1].depth() - 1) try rig.queues[1].inject(&junk);

    try testing.expectError(error.LinkFull, rig.server.poll(&rig.tx));
    try testing.expectEqual(rig.queues[1].depth() - 1, rig.queues[1].count);
}

test "responses and events keep their order across the queue" {
    var rig: Rig(8, 32) = .{};
    try rig.init();
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

test "a bad length field is an error to the session, and a reset brings it back" {
    var rig: Rig(8, 16) = .{};
    try rig.init();
    try rig.open();
    _ = try rig.add(1, 2, 0);
    _ = try rig.server.poll(&rig.tx);

    // A message that claims seven bytes and has room for six, queued behind
    // a good response. The response still arrives; the stream ends after it.
    try rig.queues[1].inject(&.{ 7, 0, 1, 2, 3, 4, 5, 6 });
    try testing.expectEqual(@as(u32, 3), try rig.sum());
    try testing.expectError(error.BadMessage, rig.client.poll(&rig.tx));
    try testing.expectError(error.BadMessage, rig.client.poll(&rig.tx));
    try testing.expectError(error.BadMessage, rig.add(1, 2, 0));

    try rig.reset();
    try testing.expectEqual(@as(usize, 0), rig.client.pending.count());
    _ = try rig.add(20, 22, 0);
    _ = try rig.server.poll(&rig.tx);
    try testing.expectEqual(@as(u32, 42), try rig.sum());
}

test "a frame too long for the receive buffer stays refused until a reset" {
    var rig: Rig(8, 16) = .{};
    try rig.init();
    try rig.open();

    var oversize: [header]u8 = undefined;
    (rpc.frame.Header{ .length = Env.max_frame, .kind = rpc.Kind.event }).write(&oversize);
    try rig.links[1].transport().send(&oversize);
    try testing.expectError(error.Oversize, rig.client.poll(&rig.tx));
    try testing.expectError(error.Oversize, rig.client.poll(&rig.tx));

    try rig.reset();
    _ = try rig.add(1, 1, 0);
    _ = try rig.server.poll(&rig.tx);
    try testing.expectEqual(@as(u32, 2), try rig.sum());
}
