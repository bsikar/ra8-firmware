//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Stand-ins for the three ThreadX queue services, over storage the test
//! owns. They behave as the error-checking services do, and can be told to
//! misbehave.

const std = @import("std");
const tx = @import("ra8_rpc_tx").api;

/// ThreadX statuses the fake hands out beyond the three the binding knows.
pub const Status = struct {
    pub const deleted: tx.Uint = 0x01;
    pub const ptr_error: tx.Uint = 0x03;
    pub const wait_error: tx.Uint = 0x04;
    pub const queue_error: tx.Uint = 0x09;
};

const valid_id = 0x5155_4555;

/// What a `TX_QUEUE` is, as far as the fake services go.
pub const FakeQueue = struct {
    id: u32 = valid_id,
    storage: []u32,
    /// Words in every message.
    words: usize,
    head: usize = 0,
    count: usize = 0,
    /// When set, every call of that service returns this and does nothing.
    force_send: ?tx.Uint = null,
    force_receive: ?tx.Uint = null,
    force_info: ?tx.Uint = null,
    /// How many times each service was entered.
    sends: usize = 0,
    receives: usize = 0,
    infos: usize = 0,

    pub fn init(storage: []u32, words: usize) FakeQueue {
        return .{ .storage = storage, .words = words };
    }

    pub fn handle(self: *FakeQueue) tx.Handle {
        return self;
    }

    pub fn depth(self: *const FakeQueue) usize {
        return self.storage.len / self.words;
    }

    /// The message `offset` places behind the oldest one.
    pub fn message(self: *FakeQueue, offset: usize) []u32 {
        const index = (self.head + offset) % self.depth();
        return self.storage[index * self.words ..][0..self.words];
    }

    /// What `tx_queue_delete` leaves behind: a control block with no id.
    pub fn delete(self: *FakeQueue) void {
        self.id = 0;
    }
};

pub const api: tx.Api = .{ .send = send, .receive = receive, .info_get = infoGet };

fn from(queue: tx.Handle) *FakeQueue {
    return @ptrCast(@alignCast(queue));
}

/// The checks the `_txe_` services make before the kernel proper runs, plus
/// one ThreadX leaves to the caller: the message must be word-aligned.
fn check(self: *FakeQueue, message: *anyopaque, wait: tx.Ulong) tx.Uint {
    if (self.id != valid_id) return Status.queue_error;
    if (@intFromPtr(message) % @alignOf(u32) != 0) return Status.ptr_error;
    if (wait != tx.Wait.none) return Status.wait_error;
    return tx.Status.success;
}

fn send(queue: tx.Handle, source: *anyopaque, wait: tx.Ulong) callconv(.c) tx.Uint {
    const self = from(queue);
    self.sends += 1;
    if (self.force_send) |status| return status;
    const checked = check(self, source, wait);
    if (checked != tx.Status.success) return checked;
    if (self.count == self.depth()) return tx.Status.queue_full;

    const words: [*]const u32 = @ptrCast(@alignCast(source));
    @memcpy(self.message(self.count), words[0..self.words]);
    self.count += 1;
    return tx.Status.success;
}

fn receive(queue: tx.Handle, destination: *anyopaque, wait: tx.Ulong) callconv(.c) tx.Uint {
    const self = from(queue);
    self.receives += 1;
    if (self.force_receive) |status| return status;
    const checked = check(self, destination, wait);
    if (checked != tx.Status.success) return checked;
    if (self.count == 0) return tx.Status.queue_empty;

    const words: [*]u32 = @ptrCast(@alignCast(destination));
    @memcpy(words[0..self.words], self.message(0));
    self.head = (self.head + 1) % self.depth();
    self.count -= 1;
    return tx.Status.success;
}

fn infoGet(
    queue: tx.Handle,
    name: ?*?[*:0]u8,
    enqueued: ?*tx.Ulong,
    available_storage: ?*tx.Ulong,
    first_suspended: ?*?*anyopaque,
    suspended_count: ?*tx.Ulong,
    next_queue: ?*?*anyopaque,
) callconv(.c) tx.Uint {
    const self = from(queue);
    self.infos += 1;
    if (self.force_info) |status| return status;
    if (self.id != valid_id) return Status.queue_error;
    // The binding asks for the two counts and nothing else.
    std.debug.assert(name == null and first_suspended == null);
    std.debug.assert(suspended_count == null and next_queue == null);
    enqueued.?.* = @intCast(self.count);
    available_storage.?.* = @intCast(self.depth() - self.count);
    return tx.Status.success;
}
