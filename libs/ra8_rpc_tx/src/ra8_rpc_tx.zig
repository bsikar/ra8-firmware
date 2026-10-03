//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Module root for `ra8_rpc_tx`: ThreadX queues behind the queue interface
//! `ra8_rpc`'s `QueueTransport` takes.
//!
//! Freestanding, no heap and no libc. This is the only place the RPC stack
//! names ThreadX; `ra8_rpc` itself stays free of it.

pub const api = @import("api.zig");
pub const Api = api.Api;
pub const Status = api.Status;
pub const Wait = api.Wait;
pub const TxQueue = @import("queue.zig").TxQueue;

/// The real kernel's entry points. Referring to this is what makes an image
/// need the ThreadX kernel at link time, so host tests never do.
pub const kernel = @import("kernel.zig");

/// The same three services as code inside a ThreadX module reaches them:
/// through the module's kernel-call dispatcher. It needs no symbol at all.
pub const module = @import("module.zig");
