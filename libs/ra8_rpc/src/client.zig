//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The calling side of a session: send requests, match responses to them.
//!
//! Nothing blocks. `call` sends and returns; `poll` hands back whatever has
//! arrived, one frame at a time, in whatever order the server answered.

const codec = @import("codec.zig");
const envelope = @import("envelope.zig");
const link = @import("link.zig");
const Pending = @import("pending.zig").Pending;
const Transport = @import("transport.zig").Transport;
const Kind = envelope.Kind;

pub const Error = link.Error;

/// A client with room for `capacity` calls in flight, each body at most
/// `max_body` bytes.
pub fn Client(comptime capacity: usize, comptime max_body: usize) type {
    return struct {
        const Self = @This();
        pub const Env = envelope.Envelope(max_body);

        /// One thing `poll` found. Slices point into the receive buffer and
        /// are valid until the next `poll`.
        pub const Incoming = union(enum) {
            /// The handshake finished; the server's capabilities.
            ready: u32,
            response: struct { id: u32, waiter: usize, result: Env.Result },
            event: Env.Event,
        };

        wire: Transport,
        inbox: link.Inbox,
        caps: u32,
        /// The server's capabilities, once its hello has been accepted.
        peer_caps: ?u32 = null,
        pending: Pending(capacity) = .{},

        /// `rx` holds frames as they arrive and must outlive the client.
        pub fn init(wire: Transport, rx: []u8, caps: u32) Self {
            return .{ .wire = wire, .inbox = .{ .buf = rx }, .caps = caps };
        }

        /// Open the session. The server's answer arrives through `poll`.
        pub fn greet(self: *Self, tx: []u8) Error!void {
            try link.post(self.wire, envelope.Hello, Kind.hello, .{ .caps = self.caps }, tx);
        }

        /// Send a request whose arguments are the message `args`.
        pub fn call(
            self: *Self,
            comptime Args: type,
            method: u16,
            args: Args,
            waiter: usize,
            tx: []u8,
        ) Error!u32 {
            var body: [codec.maxSize(Args)]u8 = undefined;
            return self.callBytes(method, try codec.encode(Args, args, &body), waiter, tx);
        }

        /// Send a request whose arguments are already encoded, and return its
        /// id. `waiter` comes back with the response.
        ///
        /// A request that could not be sent holds no slot.
        pub fn callBytes(
            self: *Self,
            method: u16,
            args: []const u8,
            waiter: usize,
            tx: []u8,
        ) Error!u32 {
            if (self.peer_caps == null) return error.NotReady;
            const id = try self.pending.add(waiter);
            errdefer _ = self.pending.take(id) catch unreachable;

            const request: Env.Request = .{ .id = id, .method = method, .args = args };
            try link.post(self.wire, Env.Request, Kind.request, request, tx);
            return id;
        }

        /// Take one frame off the transport, or null if none is complete.
        ///
        /// `tx` is used only to tell a server its hello was not acceptable.
        pub fn poll(self: *Self, tx: []u8) Error!?Incoming {
            const got = try self.inbox.next(self.wire) orelse return null;
            switch (got.kind) {
                Kind.hello => {
                    self.peer_caps = try link.admit(self.wire, got.payload, tx);
                    return .{ .ready = self.peer_caps.? };
                },
                Kind.response => {
                    const response = try codec.decode(Env.Response, got.payload);
                    const waiter = try self.pending.take(response.id);
                    return .{ .response = .{
                        .id = response.id,
                        .waiter = waiter,
                        .result = response.result,
                    } };
                },
                Kind.event => return .{ .event = try codec.decode(Env.Event, got.payload) },
                Kind.fault => return link.refusal(got.payload),
                else => return error.Unexpected,
            }
        }
    };
}
