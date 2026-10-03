//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Frames over a transport: one out, one in, and the handshake checks both
//! ends share.

const std = @import("std");
const codec = @import("codec.zig");
const frame = @import("frame.zig");
const envelope = @import("envelope.zig");
const Transport = @import("transport.zig").Transport;
const Kind = envelope.Kind;

pub const Error = codec.Error || Transport.Error || error{
    /// Every pending slot holds a call still waiting for its response.
    TableFull,
    /// A response names a request this side is not waiting on.
    UnknownId,
    /// The peer speaks another protocol version.
    VersionMismatch,
    /// The peer's hello does not start with the protocol's magic.
    BadMagic,
    /// The peer refused the connection for some other reason.
    Rejected,
    /// A frame of a kind this side has no use for.
    Unexpected,
    /// Traffic before the handshake finished.
    NotReady,
};

/// Encode `value` as one frame in `tx` and send it whole.
pub fn post(
    link: Transport,
    comptime T: type,
    kind: u16,
    value: T,
    tx: []u8,
) Error!void {
    try link.send(try frame.encode(T, kind, value, tx));
}

/// Check a peer's hello and return its capabilities. A hello this side
/// cannot accept is answered with a fault frame before the error is returned.
pub fn admit(link: Transport, payload: []const u8, tx: []u8) Error!u32 {
    const hello = try codec.decode(envelope.Hello, payload);
    hello.check() catch |err| {
        const code: envelope.Code = switch (err) {
            error.BadMagic => .bad_magic,
            error.VersionMismatch => .version_mismatch,
        };
        try post(link, envelope.Fault, Kind.fault, .{ .code = code }, tx);
        return err;
    };
    return hello.caps;
}

/// The error a peer's fault frame stands for.
pub fn refusal(payload: []const u8) Error {
    const fault = codec.decode(envelope.Fault, payload) catch |err| return err;
    return switch (fault.code) {
        .version_mismatch => error.VersionMismatch,
        .bad_magic => error.BadMagic,
        .not_ready => error.NotReady,
        else => error.Rejected,
    };
}

/// Reassembles frames from whatever pieces a transport delivers.
pub const Inbox = struct {
    /// Caller-owned. The longest frame this inbox accepts is `buf.len`.
    buf: []u8,
    /// Bytes of `buf` holding received data.
    have: usize = 0,
    /// Bytes at the front that belong to the frame last returned.
    used: usize = 0,

    /// The next whole frame, or null if it has not all arrived yet.
    ///
    /// The frame points into `buf` and is valid until the next call. A frame
    /// too long for `buf` is `Oversize`, and stays so: the stream cannot be
    /// read past a frame that was never taken in.
    pub fn next(self: *Inbox, from: Transport) Error!?frame.Frame {
        std.mem.copyForwards(u8, self.buf, self.buf[self.used..self.have]);
        self.have -= self.used;
        self.used = 0;

        if (from.poll() != 0) self.have += try from.receive(self.buf[self.have..]);

        const room = self.buf.len -| frame.Header.bytes;
        const got = frame.split(self.buf[0..self.have], room) catch |err| switch (err) {
            error.Truncated => return null,
            else => return err,
        };
        self.used = self.have - got.rest.len;
        return got;
    }
};
