//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The ThreadX queue services as code inside a module reaches them: not by
//! calling the kernel, but through the module's kernel-call dispatcher.
//!
//! This does in Zig what the module library's own wrappers do
//! (`txm_queue_send.c`, `txm_queue_receive.c`, `txm_queue_info_get.c`): one
//! call to the dispatcher with the service's request number and its
//! arguments as words. The numbers are ThreadX's, from `txm_module.h`.
//!
//! The C wrappers find the dispatcher in a global of the module,
//! `_txm_module_kernel_call_dispatcher`. This file reads no global. The
//! dispatcher arrives with the queue, in a `QueueRef` the caller owns, and
//! the handle a `TxQueue` is bound to on this side is a pointer to that.

const tx = @import("api.zig");

/// The module's kernel-call dispatcher: a request number and three words in,
/// one word out. `ALIGN_TYPE` is `ULONG` on this port.
pub const Dispatcher = *const fn (
    request: tx.Ulong,
    param_1: tx.Ulong,
    param_2: tx.Ulong,
    param_3: tx.Ulong,
) callconv(.c) tx.Ulong;

/// The request numbers of the three services.
pub const Request = struct {
    pub const queue_info_get: tx.Ulong = 38;
    pub const queue_receive: tx.Ulong = 42;
    pub const queue_send: tx.Ulong = 43;
};

/// Where `info_get` puts each of the five outputs that do not fit in the
/// dispatcher's three parameters.
pub const Extra = struct {
    pub const enqueued = 0;
    pub const available_storage = 1;
    pub const first_suspended = 2;
    pub const suspended_count = 3;
    pub const next_queue = 4;
    pub const words = 5;
};

/// One queue as module code refers to it: the dispatcher to go through, and
/// the `TX_QUEUE *` to name when it gets there. Caller-owned; a `TxQueue` on
/// the module side is bound to a pointer to one of these.
pub const QueueRef = struct {
    dispatcher: Dispatcher,
    queue: *anyopaque,

    /// The handle to bind a `TxQueue` to. This `QueueRef` must not move.
    pub fn handle(self: *QueueRef) tx.Handle {
        return self;
    }
};

pub const api: tx.Api = .{ .send = send, .receive = receive, .info_get = infoGet };

fn from(handle: tx.Handle) *const QueueRef {
    return @ptrCast(@alignCast(handle));
}

/// A pointer of any kind as the word the dispatcher carries it in. Null is
/// zero.
fn word(pointer: anytype) tx.Ulong {
    return @intCast(@intFromPtr(pointer));
}

fn send(handle: tx.Handle, source: *anyopaque, wait: tx.Ulong) callconv(.c) tx.Uint {
    const ref = from(handle);
    return @truncate(ref.dispatcher(Request.queue_send, word(ref.queue), word(source), wait));
}

fn receive(handle: tx.Handle, destination: *anyopaque, wait: tx.Ulong) callconv(.c) tx.Uint {
    const ref = from(handle);
    const request = Request.queue_receive;
    return @truncate(ref.dispatcher(request, word(ref.queue), word(destination), wait));
}

fn infoGet(
    handle: tx.Handle,
    name: ?*?[*:0]u8,
    enqueued: ?*tx.Ulong,
    available_storage: ?*tx.Ulong,
    first_suspended: ?*?*anyopaque,
    suspended_count: ?*tx.Ulong,
    next_queue: ?*?*anyopaque,
) callconv(.c) tx.Uint {
    const ref = from(handle);
    var extra: [Extra.words]tx.Ulong = undefined;
    extra[Extra.enqueued] = word(enqueued);
    extra[Extra.available_storage] = word(available_storage);
    extra[Extra.first_suspended] = word(first_suspended);
    extra[Extra.suspended_count] = word(suspended_count);
    extra[Extra.next_queue] = word(next_queue);
    const request = Request.queue_info_get;
    return @truncate(ref.dispatcher(request, word(ref.queue), word(name), word(&extra)));
}
