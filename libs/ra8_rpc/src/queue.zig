//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! A queue of fixed-size messages, injected as a context and a vtable.
//!
//! This is the shape of an RTOS message queue with the RTOS left out: every
//! message is the same number of bytes, one goes in or comes out at a time,
//! and nothing here waits. Whoever owns the real queue supplies the vtable.

pub const Queue = struct {
    ctx: *anyopaque,
    vtable: *const VTable,
    /// Bytes in every message of this queue.
    message_bytes: usize,

    pub const Error = error{
        /// No free message. Nothing was queued.
        QueueFull,
        /// The queue is gone or was deleted.
        QueueDown,
    };

    pub const VTable = struct {
        /// Queue one message of exactly `message_bytes`, without waiting.
        send: *const fn (ctx: *anyopaque, message: []const u8) Error!void,
        /// Take the oldest message into `into`, which is `message_bytes`
        /// long. False, with `into` untouched, if the queue is empty.
        receive: *const fn (ctx: *anyopaque, into: []u8) Error!bool,
        /// Messages waiting to be received.
        waiting: *const fn (ctx: *anyopaque) usize,
        /// Messages that can still be sent before the queue is full.
        free: *const fn (ctx: *anyopaque) usize,
    };

    pub fn send(self: Queue, message: []const u8) Error!void {
        return self.vtable.send(self.ctx, message);
    }

    pub fn receive(self: Queue, into: []u8) Error!bool {
        return self.vtable.receive(self.ctx, into);
    }

    pub fn waiting(self: Queue) usize {
        return self.vtable.waiting(self.ctx);
    }

    pub fn free(self: Queue) usize {
        return self.vtable.free(self.ctx);
    }
};
