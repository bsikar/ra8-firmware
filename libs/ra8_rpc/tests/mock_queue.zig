//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! A fixed-size message queue over storage the test owns, with the two ways
//! a real queue can let its user down available on request.

const std = @import("std");
const rpc = @import("ra8_rpc");
const Queue = rpc.Queue;

pub const MockQueue = struct {
    storage: []u8,
    message_bytes: usize,
    head: usize = 0,
    count: usize = 0,
    /// When set, the queue refuses every send after this many more, however
    /// much room it has: a second sender taking the space.
    sends_left: ?usize = null,
    /// When set, every send and receive reports the queue gone.
    down: bool = false,

    pub fn init(storage: []u8, message_bytes: usize) MockQueue {
        return .{ .storage = storage, .message_bytes = message_bytes };
    }

    /// The queue points at this `MockQueue`, so it must not move.
    pub fn queue(self: *MockQueue) Queue {
        return .{ .ctx = self, .vtable = &vtable, .message_bytes = self.message_bytes };
    }

    pub fn depth(self: *const MockQueue) usize {
        return self.storage.len / self.message_bytes;
    }

    /// Queue `bytes` as one message exactly as given, zero-filled to length:
    /// what a peer that does not follow the packing would put there.
    pub fn inject(self: *MockQueue, bytes: []const u8) !void {
        try std.testing.expect(self.count < self.depth());
        const message = self.slot(self.count);
        @memset(message, 0);
        @memcpy(message[0..bytes.len], bytes);
        self.count += 1;
    }

    fn slot(self: *MockQueue, offset: usize) []u8 {
        const index = (self.head + offset) % self.depth();
        return self.storage[index * self.message_bytes ..][0..self.message_bytes];
    }

    const vtable: Queue.VTable = .{
        .send = send,
        .receive = receive,
        .waiting = waiting,
        .free = free,
    };

    fn from(ctx: *anyopaque) *MockQueue {
        return @ptrCast(@alignCast(ctx));
    }

    fn send(ctx: *anyopaque, message: []const u8) Queue.Error!void {
        const self = from(ctx);
        if (self.down) return error.QueueDown;
        if (self.sends_left) |*left| {
            if (left.* == 0) return error.QueueFull;
            left.* -= 1;
        }
        if (self.count == self.depth()) return error.QueueFull;
        @memcpy(self.slot(self.count), message);
        self.count += 1;
    }

    fn receive(ctx: *anyopaque, into: []u8) Queue.Error!bool {
        const self = from(ctx);
        if (self.down) return error.QueueDown;
        if (self.count == 0) return false;
        @memcpy(into, self.slot(0));
        self.head = (self.head + 1) % self.depth();
        self.count -= 1;
        return true;
    }

    fn waiting(ctx: *anyopaque) usize {
        return from(ctx).count;
    }

    fn free(ctx: *anyopaque) usize {
        const self = from(ctx);
        return self.depth() - self.count;
    }
};
