//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The protobuf wire primitives the media-download requests are built from:
//! a base-128 varint, a field tag, a proto3 scalar and a length-delimited
//! string, each left out when it holds its default. Only what the request
//! encoders use, nothing more.

/// Protobuf wire types this file writes.
pub const Wire = struct {
    pub const varint: u3 = 0;
    pub const len: u3 = 2;
};

pub const Error = error{NoSpace};

/// Appends encoded bytes to a caller-owned buffer, refusing to overrun it.
pub const Writer = struct {
    buf: []u8,
    pos: usize = 0,

    pub fn init(buf: []u8) Writer {
        return .{ .buf = buf };
    }

    pub fn written(self: *const Writer) []const u8 {
        return self.buf[0..self.pos];
    }

    fn byte(self: *Writer, value: u8) Error!void {
        if (self.pos >= self.buf.len) return error.NoSpace;
        self.buf[self.pos] = value;
        self.pos += 1;
    }

    /// Base-128, least significant group first, high bit set on all but last.
    pub fn varint(self: *Writer, value: u64) Error!void {
        var rest = value;
        while (rest >= 0x80) : (rest >>= 7) {
            try self.byte(@as(u8, @truncate(rest)) | 0x80);
        }
        try self.byte(@truncate(rest));
    }

    pub fn tag(self: *Writer, field: u32, wire: u3) Error!void {
        try self.varint((@as(u64, field) << 3) | wire);
    }

    /// A proto3 unsigned scalar: zero is the default and is not written,
    /// matching both protobuf-c and the reference encoder.
    pub fn uint(self: *Writer, field: u32, value: u64) Error!void {
        if (value == 0) return;
        try self.tag(field, Wire.varint);
        try self.varint(value);
    }

    /// A proto3 string or bytes field: empty is the default and is not
    /// written. The payload is refused whole rather than truncated.
    pub fn bytes(self: *Writer, field: u32, data: []const u8) Error!void {
        if (data.len == 0) return;
        try self.tag(field, Wire.len);
        try self.varint(data.len);
        if (data.len > self.buf.len - self.pos) return error.NoSpace;
        @memcpy(self.buf[self.pos..][0..data.len], data);
        self.pos += data.len;
    }
};
