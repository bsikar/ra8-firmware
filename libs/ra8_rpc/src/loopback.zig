//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Two transports joined back to back in memory, for tests.
//!
//! What end `a` sends, end `b` receives, and the other way round. Each
//! direction is a byte ring over storage the caller owns.

const Transport = @import("transport.zig").Transport;

pub const Loopback = struct {
    /// `pipes[0]` carries a to b, `pipes[1]` carries b to a.
    pipes: [2]Pipe,

    pub fn init(a_to_b: []u8, b_to_a: []u8) Loopback {
        return .{ .pipes = .{ .{ .buf = a_to_b }, .{ .buf = b_to_a } } };
    }

    /// The transports point at this `Loopback`, so it must not move.
    pub fn a(self: *Loopback) Transport {
        return .{ .ctx = self, .vtable = &End(0).vtable };
    }

    pub fn b(self: *Loopback) Transport {
        return .{ .ctx = self, .vtable = &End(1).vtable };
    }
};

/// The vtable of the end that writes `pipes[out]` and reads the other one.
fn End(comptime out: u1) type {
    return struct {
        const vtable: Transport.VTable = .{ .send = send, .receive = receive, .poll = poll };

        fn pipe(ctx: *anyopaque, which: u1) *Pipe {
            const self: *Loopback = @ptrCast(@alignCast(ctx));
            return &self.pipes[which];
        }

        fn send(ctx: *anyopaque, bytes: []const u8) Transport.Error!void {
            return pipe(ctx, out).write(bytes);
        }

        fn receive(ctx: *anyopaque, into: []u8) Transport.Error!usize {
            return pipe(ctx, ~out).read(into);
        }

        fn poll(ctx: *anyopaque) usize {
            return pipe(ctx, ~out).len;
        }
    };
}

const Pipe = struct {
    buf: []u8,
    head: usize = 0,
    len: usize = 0,

    fn write(self: *Pipe, bytes: []const u8) Transport.Error!void {
        if (bytes.len > self.buf.len - self.len) return error.LinkFull;
        for (bytes, self.len..) |byte, i| self.buf[(self.head + i) % self.buf.len] = byte;
        self.len += bytes.len;
    }

    fn read(self: *Pipe, into: []u8) usize {
        const count = @min(into.len, self.len);
        for (into[0..count], 0..) |*byte, i| byte.* = self.buf[(self.head + i) % self.buf.len];
        if (count != 0) self.head = (self.head + count) % self.buf.len;
        self.len -= count;
        return count;
    }
};
