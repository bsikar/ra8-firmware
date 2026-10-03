//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! A client and a server over loopback: the handshake, calls, events, and
//! each way a session refuses.

const std = @import("std");
const testing = std.testing;

const rpc = @import("ra8_rpc");
const messages = @import("messages.zig");
const service = @import("service.zig");
const Env = service.Env;
const Method = service.Method;
const Rig = service.Rig;

comptime {
    _ = @import("messages.zig");
    _ = @import("mock_queue.zig");
    _ = @import("mock_signal.zig");
    _ = @import("service.zig");
}

const version_two: rpc.Hello = .{ .version = rpc.Protocol.version + 1, .caps = 0 };

/// The next frame the server put on the wire, read as the client would.
fn fromServer(rig: *Rig, buf: []u8) !rpc.frame.Frame {
    var inbox: rpc.link.Inbox = .{ .buf = buf };
    return (try inbox.next(rig.loop.a())).?;
}

test "the handshake gives each side the other's capabilities" {
    var rig: Rig = undefined;
    rig.init();
    try testing.expectEqual(null, rig.client.peer_caps);
    try rig.open();
    try testing.expectEqual(service.caps.server, rig.client.peer_caps.?);
    try testing.expectEqual(service.caps.client, rig.server.peer_caps.?);
}

test "a call before the handshake is refused on both sides" {
    var rig: Rig = undefined;
    rig.init();
    const args: service.Add = .{ .a = 1, .b = 2 };
    try testing.expectError(
        error.NotReady,
        rig.client.call(service.Add, Method.add, args, 0, &rig.tx),
    );
    try testing.expectEqual(@as(usize, 0), rig.client.pending.count());
    const sum: service.Sum = .{ .sum = 0 };
    try testing.expectError(error.NotReady, rig.server.emit(service.Sum, 1, sum, &rig.tx));

    try rig.toServer(Env.Request, rpc.Kind.request, .{ .id = 1, .method = Method.add, .args = "" });
    try testing.expectError(error.NotReady, rig.server.poll(&rig.tx));
    try testing.expectEqual(@as(u32, 0), rig.board.calls);
    try testing.expectError(error.NotReady, rig.client.poll(&rig.tx));
}

test "a call reaches its handler and the reply comes back to its waiter" {
    var rig: Rig = undefined;
    rig.init();
    try rig.open();

    const args: service.Add = .{ .a = 40, .b = 2 };
    const id = try rig.client.call(service.Add, Method.add, args, 77, &rig.tx);
    try testing.expectEqual(rpc.Step.answered, try rig.server.poll(&rig.tx));

    const response = (try rig.client.poll(&rig.tx)).?.response;
    try testing.expectEqual(id, response.id);
    try testing.expectEqual(@as(usize, 77), response.waiter);
    const sum = try rpc.codec.decode(service.Sum, response.result.ok);
    try testing.expectEqual(@as(u32, 42), sum.sum);
    try testing.expectEqual(@as(usize, 0), rig.client.pending.count());
    try testing.expectEqual(null, try rig.client.poll(&rig.tx));
}

test "a slice goes out and comes back whole" {
    var rig: Rig = undefined;
    rig.init();
    try rig.open();
    _ = try rig.client.call(service.Text, Method.echo, .{ .text = "ra8 rpc!" }, 0, &rig.tx);
    _ = try rig.server.poll(&rig.tx);
    const response = (try rig.client.poll(&rig.tx)).?.response;
    const text = try rpc.codec.decode(service.Text, response.result.ok);
    try testing.expectEqualSlices(u8, "ra8 rpc!", text.text);
}

test "responses in the other order still find their own waiters" {
    var rig: Rig = undefined;
    rig.init();
    try rig.open();
    const args: service.Add = .{ .a = 1, .b = 1 };
    const first = try rig.client.call(service.Add, Method.add, args, 100, &rig.tx);
    const second = try rig.client.call(service.Add, Method.add, args, 200, &rig.tx);

    try rig.toClient(Env.Response, rpc.Kind.response, .{ .id = second, .result = .{ .ok = "b" } });
    try rig.toClient(Env.Response, rpc.Kind.response, .{ .id = first, .result = .{ .ok = "a" } });

    const early = (try rig.client.poll(&rig.tx)).?.response;
    try testing.expectEqual(@as(usize, 200), early.waiter);
    try testing.expectEqualSlices(u8, "b", early.result.ok);
    try testing.expectEqual(@as(usize, 1), rig.client.pending.count());

    const late = (try rig.client.poll(&rig.tx)).?.response;
    try testing.expectEqual(@as(usize, 100), late.waiter);
    try testing.expectEqualSlices(u8, "a", late.result.ok);
}

test "events arrive between responses, in the order they were sent" {
    var rig: Rig = undefined;
    rig.init();
    try rig.open();
    _ = try rig.client.call(service.Add, Method.add, .{ .a = 2, .b = 3 }, 5, &rig.tx);

    try rig.server.emit(service.Text, 9, .{ .text = "before" }, &rig.tx);
    _ = try rig.server.poll(&rig.tx);
    try rig.server.emit(service.Text, 9, .{ .text = "after" }, &rig.tx);

    const before = (try rig.client.poll(&rig.tx)).?.event;
    try testing.expectEqual(@as(u16, 9), before.topic);
    const text = try rpc.codec.decode(service.Text, before.payload);
    try testing.expectEqualSlices(u8, "before", text.text);

    try testing.expectEqual(@as(usize, 5), (try rig.client.poll(&rig.tx)).?.response.waiter);

    const after = (try rig.client.poll(&rig.tx)).?.event;
    const last = try rpc.codec.decode(service.Text, after.payload);
    try testing.expectEqualSlices(u8, "after", last.text);
    try testing.expectEqual(@as(usize, 0), rig.client.pending.count());
}

test "a full pending table refuses the call and sends nothing" {
    var rig: Rig = undefined;
    rig.init();
    try rig.open();
    const args: service.Add = .{ .a = 1, .b = 1 };
    for (0..service.capacity) |waiter| {
        _ = try rig.client.call(service.Add, Method.add, args, waiter, &rig.tx);
    }
    const queued = rig.loop.b().poll();
    try testing.expectError(
        error.TableFull,
        rig.client.call(service.Add, Method.add, args, 9, &rig.tx),
    );
    try testing.expectEqual(queued, rig.loop.b().poll());

    _ = try rig.server.poll(&rig.tx);
    _ = try rig.client.poll(&rig.tx);
    _ = try rig.client.call(service.Add, Method.add, args, 9, &rig.tx);
}

test "a request the wire cannot take holds no slot" {
    var rig: Rig = undefined;
    rig.init();
    try rig.open();
    const junk = [_]u8{0} ** 32;
    while (rig.loop.a().send(&junk)) |_| {} else |_| {}

    const args: service.Add = .{ .a = 1, .b = 1 };
    try testing.expectError(
        error.LinkFull,
        rig.client.call(service.Add, Method.add, args, 0, &rig.tx),
    );
    try testing.expectEqual(@as(usize, 0), rig.client.pending.count());
}

test "a response to an id nobody is waiting on is an error, not ignored" {
    var rig: Rig = undefined;
    rig.init();
    try rig.open();
    const args: service.Add = .{ .a = 1, .b = 1 };
    const id = try rig.client.call(service.Add, Method.add, args, 0, &rig.tx);

    try rig.toClient(Env.Response, rpc.Kind.response, .{ .id = id + 1, .result = .{ .ok = "" } });
    try testing.expectError(error.UnknownId, rig.client.poll(&rig.tx));
    try testing.expectEqual(@as(usize, 1), rig.client.pending.count());

    _ = try rig.server.poll(&rig.tx);
    try testing.expectEqual(id, (try rig.client.poll(&rig.tx)).?.response.id);
    try rig.toClient(Env.Response, rpc.Kind.response, .{ .id = id, .result = .{ .ok = "" } });
    try testing.expectError(error.UnknownId, rig.client.poll(&rig.tx));
}

test "an unknown method gets an error response and the server carries on" {
    var rig: Rig = undefined;
    rig.init();
    try rig.open();
    _ = try rig.client.call(messages.Empty, Method.missing, .{}, 1, &rig.tx);
    try testing.expectEqual(rpc.Step.answered, try rig.server.poll(&rig.tx));
    const missing = (try rig.client.poll(&rig.tx)).?.response;
    try testing.expectEqual(rpc.Code.unknown_method, missing.result.err);
    try testing.expectEqual(@as(u32, 0), rig.board.calls);

    _ = try rig.client.call(service.Add, Method.add, .{ .a = 1, .b = 2 }, 2, &rig.tx);
    _ = try rig.server.poll(&rig.tx);
    try testing.expectEqual(@as(usize, 2), (try rig.client.poll(&rig.tx)).?.response.waiter);
}

test "arguments that do not decode never reach the handler" {
    var rig: Rig = undefined;
    rig.init();
    try rig.open();
    _ = try rig.client.callBytes(Method.add, "\x01\x02\x03", 0, &rig.tx);
    _ = try rig.server.poll(&rig.tx);
    const response = (try rig.client.poll(&rig.tx)).?.response;
    try testing.expectEqual(rpc.Code.bad_args, response.result.err);
    try testing.expectEqual(@as(u32, 0), rig.board.calls);
}

test "a handler's own refusal and a handler's broken reply both come back as codes" {
    var rig: Rig = undefined;
    rig.init();
    try rig.open();
    _ = try rig.client.call(messages.Empty, Method.refuse, .{}, 0, &rig.tx);
    _ = try rig.server.poll(&rig.tx);
    const refused = (try rig.client.poll(&rig.tx)).?.response;
    try testing.expectEqual(service.busy, refused.result.err);

    _ = try rig.client.call(messages.Empty, Method.overrun, .{}, 0, &rig.tx);
    _ = try rig.server.poll(&rig.tx);
    const broken = (try rig.client.poll(&rig.tx)).?.response;
    try testing.expectEqual(rpc.Code.failed, broken.result.err);
}

test "a server refuses a client of another version, and the client hears why" {
    var rig: Rig = undefined;
    rig.init();
    try rig.toServer(rpc.Hello, rpc.Kind.hello, version_two);
    try testing.expectError(error.VersionMismatch, rig.server.poll(&rig.tx));
    try testing.expectEqual(null, rig.server.peer_caps);

    try testing.expectError(error.VersionMismatch, rig.client.poll(&rig.tx));
    try testing.expectEqual(null, rig.client.peer_caps);
}

test "the refusal is a fault frame naming the version the server speaks" {
    var rig: Rig = undefined;
    rig.init();
    try rig.toServer(rpc.Hello, rpc.Kind.hello, version_two);
    try testing.expectError(error.VersionMismatch, rig.server.poll(&rig.tx));

    var buf: [Env.max_frame]u8 = undefined;
    const got = try fromServer(&rig, &buf);
    try testing.expectEqual(rpc.Kind.fault, got.kind);
    const fault = try rpc.codec.decode(rpc.Fault, got.payload);
    try testing.expectEqual(rpc.Code.version_mismatch, fault.code);
    try testing.expectEqual(rpc.Protocol.version, fault.version);
}

test "a client refuses a server of another version, and the server hears why" {
    var rig: Rig = undefined;
    rig.init();
    try rig.toClient(rpc.Hello, rpc.Kind.hello, version_two);
    try testing.expectError(error.VersionMismatch, rig.client.poll(&rig.tx));
    try testing.expectEqual(null, rig.client.peer_caps);
    try testing.expectError(error.VersionMismatch, rig.server.poll(&rig.tx));
}

test "a hello with the wrong magic is refused as such" {
    var rig: Rig = undefined;
    rig.init();
    try rig.toServer(rpc.Hello, rpc.Kind.hello, .{ .magic = 0, .caps = 0 });
    try testing.expectError(error.BadMagic, rig.server.poll(&rig.tx));
    try testing.expectError(error.BadMagic, rig.client.poll(&rig.tx));
}

test "a frame of a kind a side has no use for is an error" {
    var rig: Rig = undefined;
    rig.init();
    try rig.open();
    try rig.toServer(Env.Event, rpc.Kind.event, .{ .topic = 1, .payload = "" });
    try testing.expectError(error.Unexpected, rig.server.poll(&rig.tx));
    try rig.toClient(Env.Request, rpc.Kind.request, .{ .id = 1, .method = 1, .args = "" });
    try testing.expectError(error.Unexpected, rig.client.poll(&rig.tx));
    try rig.toClient(messages.Empty, 0, .{});
    try testing.expectError(error.Unexpected, rig.client.poll(&rig.tx));
}
