//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The answering side of a session: a comptime table from method to handler.
//!
//! A handler is `fn (*Context, Args) Outcome(Reply)`, where `Args` and
//! `Reply` are codec messages. The server decodes the arguments, calls the
//! handler and encodes what it returns; the handler never sees bytes.

const std = @import("std");
const codec = @import("codec.zig");
const envelope = @import("envelope.zig");
const link = @import("link.zig");
const Transport = @import("transport.zig").Transport;
const Kind = envelope.Kind;

pub const Error = link.Error;

/// What one `poll` did.
pub const Step = enum { idle, greeted, answered };

/// A server over `routes`, a tuple of `.{ method, handler }` pairs, with
/// bodies of at most `max_body` bytes.
pub fn Server(comptime Context: type, comptime max_body: usize, comptime routes: anytype) type {
    return struct {
        const Self = @This();
        pub const Env = envelope.Envelope(max_body);

        wire: Transport,
        inbox: link.Inbox,
        context: *Context,
        caps: u32,
        /// The client's capabilities, once its hello has been accepted.
        peer_caps: ?u32 = null,

        /// `rx` holds frames as they arrive and must outlive the server.
        pub fn init(wire: Transport, rx: []u8, context: *Context, caps: u32) Self {
            return .{ .wire = wire, .inbox = .{ .buf = rx }, .context = context, .caps = caps };
        }

        /// Take one frame off the transport and deal with it.
        pub fn poll(self: *Self, tx: []u8) Error!Step {
            const got = try self.inbox.next(self.wire) orelse return .idle;
            switch (got.kind) {
                Kind.hello => {
                    self.peer_caps = try link.admit(self.wire, got.payload, tx);
                    const hello: envelope.Hello = .{ .caps = self.caps };
                    try link.post(self.wire, envelope.Hello, Kind.hello, hello, tx);
                    return .greeted;
                },
                Kind.request => {
                    try self.serve(got.payload, tx);
                    return .answered;
                },
                Kind.fault => return link.refusal(got.payload),
                else => return error.Unexpected,
            }
        }

        /// Push the message `payload` to the client under `topic`.
        pub fn emit(
            self: *Self,
            comptime T: type,
            topic: u16,
            payload: T,
            tx: []u8,
        ) Error!void {
            if (self.peer_caps == null) return error.NotReady;
            var body: [codec.maxSize(T)]u8 = undefined;
            const event: Env.Event = .{
                .topic = topic,
                .payload = try codec.encode(T, payload, &body),
            };
            try link.post(self.wire, Env.Event, Kind.event, event, tx);
        }

        fn serve(self: *Self, payload: []const u8, tx: []u8) Error!void {
            if (self.peer_caps == null) {
                const fault: envelope.Fault = .{ .code = .not_ready };
                try link.post(self.wire, envelope.Fault, Kind.fault, fault, tx);
                return error.NotReady;
            }
            const request = try codec.decode(Env.Request, payload);
            inline for (routes) |route| {
                if (request.method == route[0]) return self.run(route[1], request, tx);
            }
            return self.respond(request.id, .{ .err = .unknown_method }, tx);
        }

        fn run(self: *Self, comptime handler: anytype, request: Env.Request, tx: []u8) Error!void {
            const signature = @typeInfo(@TypeOf(handler)).@"fn";
            const Args = signature.params[1].type.?;
            const Reply = @FieldType(signature.return_type.?, "ok");

            const args = codec.decode(Args, request.args) catch
                return self.respond(request.id, .{ .err = .bad_args }, tx);
            const reply = switch (handler(self.context, args)) {
                .ok => |reply| reply,
                .err => |code| return self.respond(request.id, .{ .err = code }, tx),
            };
            // A reply that breaks its own bounds is the handler's failure, and
            // the caller is told so rather than left waiting.
            var body: [codec.maxSize(Reply)]u8 = undefined;
            const bytes = codec.encode(Reply, reply, &body) catch
                return self.respond(request.id, .{ .err = .failed }, tx);
            return self.respond(request.id, .{ .ok = bytes }, tx);
        }

        fn respond(self: *Self, id: u32, result: Env.Result, tx: []u8) Error!void {
            const response: Env.Response = .{ .id = id, .result = result };
            try link.post(self.wire, Env.Response, Kind.response, response, tx);
        }
    };
}
