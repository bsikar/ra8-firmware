//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The protobuf wire reader the media-download response decoders walk: one
//! field at a time, tag then value. The scan rules are protobuf-c's own, so a
//! message the generated unpack refused is refused here too: a zero field
//! number, a tag longer than five bytes, a varint longer than ten, a group
//! wire type, or a value that runs past the end of the buffer.

const std = @import("std");

/// Protobuf wire types this reader accepts.
pub const Wire = struct {
    pub const varint: u3 = 0;
    pub const fixed64: u3 = 1;
    pub const len: u3 = 2;
    pub const fixed32: u3 = 5;
};

/// Scan limits, taken from protobuf-c's unpack.
pub const Limit = struct {
    pub const tag_bytes: usize = 5;
    pub const varint_bytes: usize = 10;
    pub const len_prefix_bytes: usize = 5;
    pub const len_max: u64 = std.math.maxInt(i32);
};

pub const Error = error{Malformed};

/// One field's value as it sat on the wire.
pub const Value = union(enum) {
    varint: u64,
    fixed64: u64,
    fixed32: u32,
    len: []const u8,
};

pub const Field = struct {
    number: u32,
    value: Value,

    /// A uint32 or enum field: the low 32 bits of a varint, as protobuf-c
    /// reads it. Any other wire type on a known field is a malformed message.
    pub fn uint32(self: Field) Error!u32 {
        return switch (self.value) {
            .varint => |v| @truncate(v),
            else => error.Malformed,
        };
    }

    /// An int32 field: the same low 32 bits, reinterpreted as signed.
    pub fn int32(self: Field) Error!i32 {
        return @bitCast(try self.uint32());
    }

    /// A uint64 field: the whole varint.
    pub fn uint64(self: Field) Error!u64 {
        return switch (self.value) {
            .varint => |v| v,
            else => error.Malformed,
        };
    }

    /// A bytes or string field: the span, borrowed from the buffer.
    pub fn bytes(self: Field) Error![]const u8 {
        return switch (self.value) {
            .len => |span| span,
            else => error.Malformed,
        };
    }
};

/// Walks a caller-owned buffer; never reads past it.
pub const Reader = struct {
    buf: []const u8,
    pos: usize = 0,

    pub fn init(buf: []const u8) Reader {
        return .{ .buf = buf };
    }

    /// The next field, or null once the buffer is exactly consumed.
    pub fn next(self: *Reader) Error!?Field {
        if (self.pos == self.buf.len) return null;
        if (self.buf[self.pos] & 0xF8 == 0) return error.Malformed;
        const key = try self.varint(Limit.tag_bytes);
        const number: u32 = @truncate(key >> 3);
        return .{ .number = number, .value = try self.value(@truncate(key)) };
    }

    fn value(self: *Reader, wire: u3) Error!Value {
        return switch (wire) {
            Wire.varint => .{ .varint = try self.varint(Limit.varint_bytes) },
            Wire.fixed64 => .{ .fixed64 = std.mem.readInt(u64, (try self.take(8))[0..8], .little) },
            Wire.len => .{ .len = try self.take(try self.lenPrefix()) },
            Wire.fixed32 => .{ .fixed32 = std.mem.readInt(u32, (try self.take(4))[0..4], .little) },
            else => error.Malformed,
        };
    }

    /// Base-128 of at most `max_bytes`; bits past 64 are dropped.
    fn varint(self: *Reader, max_bytes: usize) Error!u64 {
        var result: u64 = 0;
        var index: usize = 0;
        while (index < max_bytes) : (index += 1) {
            if (self.pos >= self.buf.len) return error.Malformed;
            const byte = self.buf[self.pos];
            self.pos += 1;
            const shift: u6 = @intCast(@min(index * 7, 63));
            result |= @as(u64, byte & 0x7F) << shift;
            if (byte & 0x80 == 0) return result;
        }
        return error.Malformed;
    }

    fn lenPrefix(self: *Reader) Error!usize {
        const length = try self.varint(Limit.len_prefix_bytes);
        if (length > Limit.len_max) return error.Malformed;
        return @intCast(length);
    }

    fn take(self: *Reader, count: usize) Error![]const u8 {
        if (count > self.buf.len - self.pos) return error.Malformed;
        defer self.pos += count;
        return self.buf[self.pos..][0..count];
    }
};
