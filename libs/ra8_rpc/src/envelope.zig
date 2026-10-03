//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! What a frame's kind means: the handshake, and the three kinds of traffic.
//!
//! Every number here is on the wire and is pinned. A new kind or code takes a
//! new number; an existing one never changes meaning.

const std = @import("std");
const codec = @import("codec.zig");
const frame = @import("frame.zig");

pub const Protocol = struct {
    /// The bytes `RA8R` at the start of every hello.
    pub const magic: u32 = 0x52384152;
    pub const version: u16 = 1;
};

/// The `kind` in the frame header. Zero is never sent.
pub const Kind = struct {
    pub const hello: u16 = 1;
    pub const request: u16 = 2;
    pub const response: u16 = 3;
    pub const event: u16 = 4;
    pub const fault: u16 = 5;
};

/// Why a call or a connection was refused.
///
/// Values below `first_app` belong to this library. A handler reports its own
/// failures with values from `first_app` up.
pub const Code = enum(u16) {
    unknown_method = 1,
    bad_args = 2,
    failed = 3,
    version_mismatch = 4,
    bad_magic = 5,
    not_ready = 6,
    _,

    pub const first_app: u16 = 0x0100;
};

/// The first frame each side sends.
pub const Hello = struct {
    magic: u32 = Protocol.magic,
    version: u16 = Protocol.version,
    /// Capability bits. Their meaning belongs to the message set in use.
    caps: u32,

    pub fn check(self: Hello) error{ BadMagic, VersionMismatch }!void {
        if (self.magic != Protocol.magic) return error.BadMagic;
        if (self.version != Protocol.version) return error.VersionMismatch;
    }
};

/// A refusal that belongs to the connection rather than to one call.
pub const Fault = struct {
    code: Code,
    /// The version the sender speaks, so a mismatch names both sides.
    version: u16 = Protocol.version,
};

/// What a handler hands back: its reply, or the code to refuse with.
pub fn Outcome(comptime Reply: type) type {
    return union(enum) { ok: Reply, err: Code };
}

pub const ResultTag = enum(u8) { ok = 0, err = 1 };

/// The traffic messages, for bodies of at most `max_body` bytes.
///
/// `args`, `ok` and `payload` are a message already encoded by `codec`; the
/// envelope carries it as bytes and does not look inside.
pub fn Envelope(comptime max_body: usize) type {
    return struct {
        pub const Request = struct {
            id: u32,
            method: u16,
            args: []const u8,

            pub const max_len = .{ .args = max_body };
        };

        pub const Result = union(ResultTag) {
            ok: []const u8,
            err: Code,

            pub const max_len = .{ .ok = max_body };
        };

        pub const Response = struct { id: u32, result: Result };

        pub const Event = struct {
            topic: u16,
            payload: []const u8,

            pub const max_len = .{ .payload = max_body };
        };

        /// A buffer this long holds any frame either side can send.
        pub const max_frame = @max(
            frame.maxSize(Request),
            frame.maxSize(Response),
            frame.maxSize(Event),
            frame.maxSize(Hello),
            frame.maxSize(Fault),
        );
    };
}
