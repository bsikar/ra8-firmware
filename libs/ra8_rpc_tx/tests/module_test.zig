//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The module-side services: what each one hands the dispatcher, and that
//! a `TxQueue` over them behaves as it does over the kernel's.

const std = @import("std");
const testing = std.testing;

const rpc = @import("ra8_rpc");
const rpc_tx = @import("ra8_rpc_tx");
const tx = rpc_tx.api;
const module = rpc_tx.module;
const fake = @import("fake_threadx.zig");
const dispatcher = @import("fake_dispatcher.zig");
const FakeQueue = fake.FakeQueue;
const TxQueue = rpc_tx.TxQueue(module.api);

comptime {
    _ = @import("fake_dispatcher.zig");
    _ = @import("fake_threadx.zig");
}

/// Two-word messages, four of them.
const words = 2;
const message_bytes = words * 4;
const depth = 4;

/// A fake ThreadX queue, the module's reference to it, and the binding.
const Bench = struct {
    storage: [depth * words]u32 = undefined,
    kernel: FakeQueue = undefined,
    ref: module.QueueRef = undefined,
    bound: TxQueue = undefined,

    fn init(self: *Bench) !void {
        dispatcher.clear();
        self.kernel = FakeQueue.init(&self.storage, words);
        self.ref = .{ .dispatcher = dispatcher.dispatch, .queue = self.kernel.handle() };
        self.bound = try TxQueue.init(self.ref.handle(), message_bytes);
    }

    /// The `TX_QUEUE *` as the word the dispatcher should be given.
    fn queueWord(self: *Bench) tx.Ulong {
        return @intCast(@intFromPtr(self.kernel.handle()));
    }
};

fn address(pointer: anytype) tx.Ulong {
    return @intCast(@intFromPtr(pointer));
}

test "the request numbers are the ones txm_module.h gives the three services" {
    try testing.expectEqual(@as(tx.Ulong, 38), module.Request.queue_info_get);
    try testing.expectEqual(@as(tx.Ulong, 42), module.Request.queue_receive);
    try testing.expectEqual(@as(tx.Ulong, 43), module.Request.queue_send);
}

test "the handle is the reference, and the reference names the queue" {
    var bench: Bench = .{};
    try bench.init();
    try testing.expectEqual(@intFromPtr(&bench.ref), @intFromPtr(bench.ref.handle()));
    try testing.expectEqual(@intFromPtr(&bench.kernel), @intFromPtr(bench.ref.queue));
}

test "send is request 43 with the queue, the message and the wait option" {
    var bench: Bench = .{};
    try bench.init();
    var message: [words]u32 align(4) = .{ 0x11111111, 0x22222222 };
    // A wait of seven ticks, to see it arrive. The fake service refuses to
    // wait, and its refusal is what comes back.
    const status = module.api.send(bench.ref.handle(), &message, 7);
    try testing.expectEqual(fake.Status.wait_error, status);

    const call = dispatcher.last();
    try testing.expectEqual(@as(usize, 1), dispatcher.calls().len);
    try testing.expectEqual(module.Request.queue_send, call.request);
    try testing.expectEqual(bench.queueWord(), call.params[0]);
    try testing.expectEqual(address(&message), call.params[1]);
    try testing.expectEqual(@as(tx.Ulong, 7), call.params[2]);
}

test "receive is request 42 with the queue, the destination and the wait option" {
    var bench: Bench = .{};
    try bench.init();
    var message: [words]u32 align(4) = undefined;
    const status = module.api.receive(bench.ref.handle(), &message, tx.Wait.none);
    try testing.expectEqual(tx.Status.queue_empty, status);

    const call = dispatcher.last();
    try testing.expectEqual(@as(usize, 1), dispatcher.calls().len);
    try testing.expectEqual(module.Request.queue_receive, call.request);
    try testing.expectEqual(bench.queueWord(), call.params[0]);
    try testing.expectEqual(address(&message), call.params[1]);
    try testing.expectEqual(tx.Wait.none, call.params[2]);
}

test "info_get is request 38 with the queue, the name and five more in an array" {
    var bench: Bench = .{};
    try bench.init();
    var name: ?[*:0]u8 = null;
    var enqueued: tx.Ulong = 99;
    var available: tx.Ulong = 99;
    var suspended: tx.Ulong = 99;
    var first: ?*anyopaque = null;
    var next: ?*anyopaque = null;
    // The fake service takes only the two counts, so it is told to answer
    // without looking; this is about where each pointer lands on the way.
    bench.kernel.force_info = tx.Status.success;
    _ = module.api.info_get(
        bench.ref.handle(),
        &name,
        &enqueued,
        &available,
        &first,
        &suspended,
        &next,
    );

    const call = dispatcher.last();
    try testing.expectEqual(module.Request.queue_info_get, call.request);
    try testing.expectEqual(bench.queueWord(), call.params[0]);
    try testing.expectEqual(address(&name), call.params[1]);
    try testing.expect(call.params[2] != 0);
    const extra = dispatcher.last_extra;
    try testing.expectEqual(address(&enqueued), extra[0]);
    try testing.expectEqual(address(&available), extra[1]);
    try testing.expectEqual(address(&first), extra[2]);
    try testing.expectEqual(address(&suspended), extra[3]);
    try testing.expectEqual(address(&next), extra[4]);
}

test "an output nobody asked for goes to the dispatcher as zero" {
    var bench: Bench = .{};
    try bench.init();
    var enqueued: tx.Ulong = 99;
    var available: tx.Ulong = 99;
    const handle = bench.ref.handle();
    const status = module.api.info_get(handle, null, &enqueued, &available, null, null, null);
    try testing.expectEqual(tx.Status.success, status);
    try testing.expectEqual(@as(tx.Ulong, 0), enqueued);
    try testing.expectEqual(@as(tx.Ulong, depth), available);

    try testing.expectEqual(@as(tx.Ulong, 0), dispatcher.last().params[1]);
    const extra = dispatcher.last_extra;
    try testing.expectEqual(address(&enqueued), extra[0]);
    try testing.expectEqual(address(&available), extra[1]);
    for (extra[2..]) |word| try testing.expectEqual(@as(tx.Ulong, 0), word);
}

test "the status that comes back is the one the dispatcher returned" {
    var bench: Bench = .{};
    try bench.init();
    var message: [words]u32 align(4) = undefined;
    const handle = bench.ref.handle();
    bench.kernel.force_send = fake.Status.deleted;
    try testing.expectEqual(fake.Status.deleted, module.api.send(handle, &message, 0));
    bench.kernel.force_receive = fake.Status.queue_error;
    try testing.expectEqual(fake.Status.queue_error, module.api.receive(handle, &message, 0));
}

test "a bound queue sends and receives through the dispatcher, without waiting" {
    var bench: Bench = .{};
    try bench.init();
    const queue = bench.bound.queue();
    try queue.send("abcdefgh");
    try testing.expectEqualSlices(u8, "abcdefgh", std.mem.sliceAsBytes(bench.kernel.message(0)));
    try testing.expectEqual(@as(usize, 1), queue.waiting());
    try testing.expectEqual(@as(usize, depth - 1), queue.free());

    var got: [message_bytes]u8 = undefined;
    try testing.expect(try queue.receive(&got));
    try testing.expectEqualSlices(u8, "abcdefgh", &got);

    // Nothing reached the fake kernel except through the dispatcher, and
    // every send and receive carried TX_NO_WAIT.
    const requests = [_]tx.Ulong{
        module.Request.queue_send,
        module.Request.queue_info_get,
        module.Request.queue_info_get,
        module.Request.queue_receive,
    };
    try testing.expectEqual(requests.len, dispatcher.calls().len);
    for (requests, dispatcher.calls()) |request, call| {
        try testing.expectEqual(request, call.request);
        try testing.expectEqual(bench.queueWord(), call.params[0]);
    }
    try testing.expectEqual(tx.Wait.none, dispatcher.calls()[0].params[2]);
    try testing.expectEqual(tx.Wait.none, dispatcher.calls()[3].params[2]);
}

test "a full queue is QueueFull and an empty one is no message, as on the resident side" {
    var bench: Bench = .{};
    try bench.init();
    const queue = bench.bound.queue();
    var got: [message_bytes]u8 = @splat(0x7E);
    try testing.expect(!try queue.receive(&got));
    for (got) |byte| try testing.expectEqual(@as(u8, 0x7E), byte);

    for (0..depth) |_| try queue.send("abcdefgh");
    try testing.expectError(error.QueueFull, queue.send("12345678"));
    try testing.expectEqual(@as(usize, depth), bench.kernel.count);
    try testing.expectEqual(null, bench.bound.failure);
}

test "any other status takes the queue down and the dispatcher is not asked again" {
    var bench: Bench = .{};
    try bench.init();
    const queue = bench.bound.queue();
    bench.kernel.force_send = fake.Status.deleted;
    try testing.expectError(error.QueueDown, queue.send("abcdefgh"));
    try testing.expectEqual(fake.Status.deleted, bench.bound.failure.?);

    bench.kernel.force_send = null;
    const asked = dispatcher.calls().len;
    var got: [message_bytes]u8 = undefined;
    try testing.expectError(error.QueueDown, queue.send("abcdefgh"));
    try testing.expectError(error.QueueDown, queue.receive(&got));
    try testing.expect(queue.waiting() != 0);
    try testing.expectEqual(asked, dispatcher.calls().len);

    bench.bound.clear();
    try queue.send("abcdefgh");
}

test "a request the manager does not serve takes the queue down too" {
    var bench: Bench = .{};
    try bench.init();
    // A dispatcher that serves nothing: every request is TX_NOT_AVAILABLE.
    const Deaf = struct {
        fn dispatch(_: tx.Ulong, _: tx.Ulong, _: tx.Ulong, _: tx.Ulong) callconv(.c) tx.Ulong {
            return dispatcher.not_available;
        }
    };
    bench.ref.dispatcher = Deaf.dispatch;
    try testing.expectError(error.QueueDown, bench.bound.queue().send("abcdefgh"));
    try testing.expectEqual(dispatcher.not_available, bench.bound.failure.?);
}
