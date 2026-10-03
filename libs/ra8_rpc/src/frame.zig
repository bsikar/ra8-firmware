//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The six-byte frame header, and a message framed behind it.
//!
//! A frame is a `u32` length, a `u16` kind and then `length` payload bytes,
//! all little-endian. The length counts the payload only, never the header.
//!
//! The kind is an opaque number here. What kinds exist and what each one
//! means belongs to the layer above.

const std = @import("std");
const codec = @import("codec.zig");

pub const Error = codec.Error;

pub const Header = struct {
    /// Payload bytes that follow the header.
    length: u32,
    kind: u16,

    pub const bytes = At.kind + @sizeOf(u16);

    /// Byte offsets of the fields.
    pub const At = struct {
        pub const length = 0;
        pub const kind = @sizeOf(u32);
    };

    pub fn write(self: Header, out: *[bytes]u8) void {
        std.mem.writeInt(u32, out[At.length..][0..4], self.length, .little);
        std.mem.writeInt(u16, out[At.kind..][0..2], self.kind, .little);
    }

    pub fn read(in: *const [bytes]u8) Header {
        return .{
            .length = std.mem.readInt(u32, in[At.length..][0..4], .little),
            .kind = std.mem.readInt(u16, in[At.kind..][0..2], .little),
        };
    }
};

/// One frame found at the front of a buffer. Both slices point into it.
pub const Frame = struct {
    kind: u16,
    payload: []const u8,
    /// Whatever followed the frame, for a buffer holding more than one.
    rest: []const u8,
};

/// The largest frame a message of type `T` can make.
pub fn maxSize(comptime T: type) comptime_int {
    return Header.bytes + codec.maxSize(T);
}

/// Encode `value` as one frame at the front of `out` and return it.
///
/// `out` is untouched on error.
pub fn encode(comptime T: type, kind: u16, value: T, out: []u8) Error![]u8 {
    comptime std.debug.assert(codec.maxSize(T) <= std.math.maxInt(u32));
    if (out.len < Header.bytes) return error.NoSpace;

    const payload = try codec.encode(T, value, out[Header.bytes..]);
    const header: Header = .{ .length = @intCast(payload.len), .kind = kind };
    header.write(out[0..Header.bytes]);
    return out[0 .. Header.bytes + payload.len];
}

/// Take one frame off the front of `in`.
///
/// `max_payload` is the most the caller will accept. A header claiming more is
/// `Oversize` whether or not those bytes are present, so a reader can refuse a
/// frame before it has waited for the body.
pub fn split(in: []const u8, max_payload: usize) Error!Frame {
    if (in.len < Header.bytes) return error.Truncated;
    const header = Header.read(in[0..Header.bytes]);
    const body = in[Header.bytes..];

    if (header.length > max_payload) return error.Oversize;
    if (header.length > body.len) return error.Truncated;
    return .{
        .kind = header.kind,
        .payload = body[0..header.length],
        .rest = body[header.length..],
    };
}
