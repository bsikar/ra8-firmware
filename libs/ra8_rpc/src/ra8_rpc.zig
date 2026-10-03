//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Module root for `ra8_rpc`: the frame header, the struct codec under it,
//! and the session built on both.
//!
//! Freestanding, no heap and no libc. Every buffer belongs to the caller.

pub const codec = @import("codec.zig");
pub const frame = @import("frame.zig");
pub const envelope = @import("envelope.zig");
pub const link = @import("link.zig");

pub const Error = link.Error;

pub const Kind = envelope.Kind;
pub const Code = envelope.Code;
pub const Protocol = envelope.Protocol;
pub const Hello = envelope.Hello;
pub const Fault = envelope.Fault;
pub const Outcome = envelope.Outcome;
pub const Envelope = envelope.Envelope;

pub const Transport = @import("transport.zig").Transport;
pub const Loopback = @import("loopback.zig").Loopback;
pub const Queue = @import("queue.zig").Queue;
pub const QueueTransport = @import("queue_transport.zig").QueueTransport;
pub const ring = @import("ring.zig");
pub const Ring = ring.Ring;
pub const Signal = @import("signal.zig").Signal;
pub const RingTransport = @import("ring_transport.zig").RingTransport;
pub const Pending = @import("pending.zig").Pending;
pub const Client = @import("client.zig").Client;
pub const Server = @import("server.zig").Server;
pub const Step = @import("server.zig").Step;
