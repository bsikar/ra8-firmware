//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The real ThreadX entry points, for an image that links the kernel.
//!
//! These are the error-checking `_txe_` services. The firmware builds
//! ThreadX without `TX_DISABLE_ERROR_CHECKING`, so they are what
//! `tx_queue_send` and its siblings already expand to in every C file of
//! the resident image, and this binding calls the same ones. They check the
//! queue pointer and its id before the kernel proper touches it.

const tx = @import("api.zig");

extern fn _txe_queue_send(queue: tx.Handle, source: *anyopaque, wait: tx.Ulong) tx.Uint;
extern fn _txe_queue_receive(queue: tx.Handle, destination: *anyopaque, wait: tx.Ulong) tx.Uint;
extern fn _txe_queue_info_get(
    queue: tx.Handle,
    name: ?*?[*:0]u8,
    enqueued: ?*tx.Ulong,
    available_storage: ?*tx.Ulong,
    first_suspended: ?*?*anyopaque,
    suspended_count: ?*tx.Ulong,
    next_queue: ?*?*anyopaque,
) tx.Uint;

pub const api: tx.Api = .{
    .send = &_txe_queue_send,
    .receive = &_txe_queue_receive,
    .info_get = &_txe_queue_info_get,
};
