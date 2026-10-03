//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! One ThreadX queue as an `rpc.Queue`.
//!
//! Nothing here waits: every call into ThreadX is made with `TX_NO_WAIT`, so
//! it is safe from a thread, from a timer and from an interrupt handler.
//!
//! ThreadX has three answers this binding acts on. Success is success, a
//! full queue on send is `QueueFull`, and an empty queue on receive is "no
//! message". Any other status is a failure of the queue itself. It is
//! `QueueDown`, the status is kept for whoever investigates, and the binding
//! stays down until `clear`: nothing is retried and nothing is dropped
//! quietly.
//!
//! A queue must have one sender. `rpc.QueueTransport` counts the free
//! messages and then sends, and a second sender between the two would break
//! that. Give each direction of each link a queue of its own.

const std = @import("std");
const rpc = @import("ra8_rpc");
const tx = @import("api.zig");
const Queue = rpc.Queue;

/// A queue binding over the ThreadX entry points in `api`.
pub fn TxQueue(comptime api: tx.Api) type {
    return struct {
        const Self = @This();

        handle: tx.Handle,
        /// Bytes in every message: what the queue was created with.
        message_bytes: usize,
        /// The ThreadX status that took this queue down, if one has.
        failure: ?tx.Uint = null,

        pub const InitError = error{
            /// Not a whole number of 32-bit words between one and sixteen.
            BadMessageSize,
        };

        /// Bind a queue that already exists. `message_bytes` must be the
        /// size it was created with, in bytes: ThreadX copies that much on
        /// every send and receive and has no way to be told otherwise.
        pub fn init(handle: tx.Handle, message_bytes: usize) InitError!Self {
            if (message_bytes % tx.Message.word_bytes != 0) return error.BadMessageSize;
            const words = message_bytes / tx.Message.word_bytes;
            if (words < tx.Message.min_words) return error.BadMessageSize;
            if (words > tx.Message.max_words) return error.BadMessageSize;
            return .{ .handle = handle, .message_bytes = message_bytes };
        }

        /// The queue points at this `TxQueue`, so it must not move.
        pub fn queue(self: *Self) Queue {
            return .{ .ctx = self, .vtable = &vtable, .message_bytes = self.message_bytes };
        }

        /// Forget a failure. The ThreadX queue is not touched: flush or
        /// recreate it first, as the README describes.
        pub fn clear(self: *Self) void {
            self.failure = null;
        }

        const vtable: Queue.VTable = .{
            .send = send,
            .receive = receive,
            .waiting = waiting,
            .free = free,
        };

        /// A message as ThreadX wants it: whole words, word-aligned. The
        /// transport's own buffers are bytes at any alignment.
        const Words = [tx.Message.max_words]u32;

        fn from(ctx: *anyopaque) *Self {
            return @ptrCast(@alignCast(ctx));
        }

        fn fail(self: *Self, status: tx.Uint) Queue.Error {
            self.failure = status;
            return error.QueueDown;
        }

        fn send(ctx: *anyopaque, message: []const u8) Queue.Error!void {
            const self = from(ctx);
            if (self.failure != null) return error.QueueDown;

            var words: Words = undefined;
            @memcpy(@as([*]u8, @ptrCast(&words))[0..self.message_bytes], message);
            switch (api.send(self.handle, &words, tx.Wait.none)) {
                tx.Status.success => {},
                tx.Status.queue_full => return error.QueueFull,
                else => |status| return self.fail(status),
            }
        }

        fn receive(ctx: *anyopaque, into: []u8) Queue.Error!bool {
            const self = from(ctx);
            if (self.failure != null) return error.QueueDown;

            var words: Words = undefined;
            switch (api.receive(self.handle, &words, tx.Wait.none)) {
                tx.Status.success => {},
                tx.Status.queue_empty => return false,
                else => |status| return self.fail(status),
            }
            @memcpy(into, @as([*]const u8, @ptrCast(&words))[0..self.message_bytes]);
            return true;
        }

        const Counts = struct { waiting: tx.Ulong, free: tx.Ulong };

        /// Ask ThreadX how full the queue is. Null, with the failure kept,
        /// if it will not say.
        fn counts(self: *Self) ?Counts {
            if (self.failure != null) return null;
            var got: Counts = .{ .waiting = 0, .free = 0 };
            const queue_ptr = self.handle;
            const status = api.info_get(queue_ptr, null, &got.waiting, &got.free, null, null, null);
            if (status == tx.Status.success) return got;
            self.failure = status;
            return null;
        }

        /// One, not zero, when the queue is down: the caller then tries to
        /// receive and is told `QueueDown`, where a zero would read as an
        /// empty queue for ever.
        fn waiting(ctx: *anyopaque) usize {
            return if (from(ctx).counts()) |got| got.waiting else 1;
        }

        /// Likewise for a send: room is claimed, so the caller tries and is
        /// told `QueueDown`, where a zero would read as a full queue.
        fn free(ctx: *anyopaque) usize {
            return if (from(ctx).counts()) |got| got.free else std.math.maxInt(usize);
        }
    };
}
