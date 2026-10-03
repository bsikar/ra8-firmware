//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! A small service and a client wired to it over loopback, for the session
//! tests to drive.

const std = @import("std");
const rpc = @import("ra8_rpc");
const messages = @import("messages.zig");

pub const Env = messages.Env;

pub const Method = struct {
    pub const add: u16 = 1;
    pub const echo: u16 = 2;
    pub const refuse: u16 = 3;
    pub const overrun: u16 = 4;
    /// A method no route answers.
    pub const missing: u16 = 0x7FFF;
};

pub const Add = struct { a: u32, b: u32 };
pub const Sum = struct { sum: u32 };

pub const Text = struct {
    text: []const u8,

    pub const max_len = .{ .text = 8 };
};

/// The code `refuse` answers with, from the application's own range.
pub const busy: rpc.Code = @enumFromInt(rpc.Code.first_app + 1);

/// What the handlers act on.
pub const Board = struct { calls: u32 = 0 };

fn add(board: *Board, args: Add) rpc.Outcome(Sum) {
    board.calls += 1;
    return .{ .ok = .{ .sum = args.a +% args.b } };
}

fn echo(board: *Board, args: Text) rpc.Outcome(Text) {
    board.calls += 1;
    return .{ .ok = args };
}

fn refuse(board: *Board, _: messages.Empty) rpc.Outcome(messages.Empty) {
    board.calls += 1;
    return .{ .err = busy };
}

/// A handler bug: a reply longer than its own message allows.
fn overrun(_: *Board, _: messages.Empty) rpc.Outcome(Text) {
    return .{ .ok = .{ .text = "far too long" } };
}

pub const routes = .{
    .{ Method.add, add },
    .{ Method.echo, echo },
    .{ Method.refuse, refuse },
    .{ Method.overrun, overrun },
};

pub const capacity = 2;
pub const caps = struct {
    pub const client: u32 = 0x0000_0003;
    pub const server: u32 = 0x0000_0006;
};

pub const Client = rpc.Client(capacity, Env.Request.max_len.args);
pub const Server = rpc.Server(Board, Env.Request.max_len.args, routes);

/// Both ends and everything they need. Set up in place: the two ends hold
/// pointers into it.
pub const Rig = struct {
    wires: [2][4 * Env.max_frame]u8 = undefined,
    loop: rpc.Loopback = undefined,
    board: Board = .{},
    client_rx: [Env.max_frame]u8 = undefined,
    server_rx: [Env.max_frame]u8 = undefined,
    tx: [Env.max_frame]u8 = undefined,
    client: Client = undefined,
    server: Server = undefined,

    pub fn init(self: *Rig) void {
        self.* = .{};
        self.loop = rpc.Loopback.init(&self.wires[0], &self.wires[1]);
        self.client = Client.init(self.loop.a(), &self.client_rx, caps.client);
        self.server = Server.init(self.loop.b(), &self.server_rx, &self.board, caps.server);
    }

    /// Run the handshake to the point where calls are accepted.
    pub fn open(self: *Rig) !void {
        try self.client.greet(&self.tx);
        try std.testing.expectEqual(rpc.Step.greeted, try self.server.poll(&self.tx));
        const ready = (try self.client.poll(&self.tx)).?.ready;
        try std.testing.expectEqual(caps.server, ready);
    }

    /// Put one frame on the wire towards the client, as a server would.
    pub fn toClient(self: *Rig, comptime T: type, kind: u16, value: T) !void {
        try rpc.link.post(self.loop.b(), T, kind, value, &self.tx);
    }

    /// Put one frame on the wire towards the server, as a client would.
    pub fn toServer(self: *Rig, comptime T: type, kind: u16, value: T) !void {
        try rpc.link.post(self.loop.a(), T, kind, value, &self.tx);
    }
};
