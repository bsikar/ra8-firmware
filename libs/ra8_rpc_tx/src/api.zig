//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The part of ThreadX a queue binding needs: three entry points and the
//! handful of values they speak in.
//!
//! The entry points are a comptime value rather than `extern` names, so a
//! host test hands in fakes and only an ARM image binds the kernel. The
//! numbers are ThreadX's own, from `tx_api.h`.

/// ThreadX `UINT` and `ULONG`, as the C ABI of the target spells them.
pub const Uint = c_uint;
pub const Ulong = c_ulong;

/// A `TX_QUEUE *`. The control block's layout is ThreadX's business.
pub const Handle = *anyopaque;

pub const Api = struct {
    /// `tx_queue_send`.
    send: *const fn (queue: Handle, source: *anyopaque, wait: Ulong) callconv(.c) Uint,
    /// `tx_queue_receive`.
    receive: *const fn (queue: Handle, destination: *anyopaque, wait: Ulong) callconv(.c) Uint,
    /// `tx_queue_info_get`. Every output but `enqueued` and
    /// `available_storage` is passed as null.
    info_get: *const fn (
        queue: Handle,
        name: ?*?[*:0]u8,
        enqueued: ?*Ulong,
        available_storage: ?*Ulong,
        first_suspended: ?*?*anyopaque,
        suspended_count: ?*Ulong,
        next_queue: ?*?*anyopaque,
    ) callconv(.c) Uint,
};

/// The return values this binding tells apart. Every other one is a failure.
pub const Status = struct {
    pub const success: Uint = 0x00;
    pub const queue_empty: Uint = 0x0A;
    pub const queue_full: Uint = 0x0B;
};

pub const Wait = struct {
    /// `TX_NO_WAIT`: return at once instead of suspending the caller.
    pub const none: Ulong = 0;
};

/// A ThreadX message is one to sixteen 32-bit words.
pub const Message = struct {
    pub const word_bytes = 4;
    pub const min_words = 1;
    pub const max_words = 16;
    pub const max_bytes = max_words * word_bytes;
};
