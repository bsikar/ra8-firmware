//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The struct codec: sizes, and every way an encode or a decode is refused.

const std = @import("std");
const testing = std.testing;

const rpc = @import("ra8_rpc");
const codec = rpc.codec;
const messages = @import("messages.zig");
const Blob = messages.Blob;
const Mixed = messages.Mixed;

comptime {
    _ = @import("messages.zig");
    _ = @import("mock_queue.zig");
    _ = @import("mock_signal.zig");
    _ = @import("service.zig");
}

const header_bytes = rpc.frame.Header.bytes;

/// A length prefix, as the wire carries it.
fn prefix(len: u32) [4]u8 {
    var bytes: [4]u8 = undefined;
    std.mem.writeInt(u32, &bytes, len, .little);
    return bytes;
}

test "the comptime maximum is the widest encoding of each message" {
    try testing.expectEqual(@as(usize, 0), codec.maxSize(messages.Empty));
    try testing.expectEqual(@as(usize, 30), codec.maxSize(messages.Ints));
    try testing.expectEqual(@as(usize, 3), codec.maxSize(messages.Tagged));
    try testing.expectEqual(@as(usize, 4 + 4 + 16), codec.maxSize(Blob));
    try testing.expectEqual(@as(usize, 1 + 4 + 8 + 2 + 4 + 32), codec.maxSize(Mixed));
}

test "size is the exact length of the encoding" {
    inline for (messages.cases) |case| {
        const T = @TypeOf(case.value);
        const payload = case.golden[header_bytes..];
        try testing.expectEqual(payload.len, try codec.size(T, case.value));
    }
}

test "a value at its bounds fills a maximum buffer exactly" {
    const value: Blob = .{ .addr = 1, .data = &@as([Blob.max_len.data]u8, @splat(0xAB)) };
    var out: [codec.maxSize(Blob)]u8 = undefined;
    try testing.expectEqual(out.len, (try codec.encode(Blob, value, &out)).len);
}

test "a buffer one byte short is refused and left alone" {
    inline for (messages.cases) |case| {
        const T = @TypeOf(case.value);
        if (codec.maxSize(T) == 0) continue;
        const need = try codec.size(T, case.value);

        var out: [64]u8 = @splat(0x7E);
        try testing.expectError(error.NoSpace, codec.encode(T, case.value, out[0 .. need - 1]));
        for (out) |byte| try testing.expectEqual(@as(u8, 0x7E), byte);
    }
}

test "a slice past its bound is refused on encode and the buffer left alone" {
    const value: Blob = .{ .addr = 1, .data = &@as([Blob.max_len.data + 1]u8, @splat(0xAB)) };
    var out: [64]u8 = @splat(0x7E);
    try testing.expectError(error.Oversize, codec.encode(Blob, value, &out));
    try testing.expectError(error.Oversize, codec.size(Blob, value));
    for (out) |byte| try testing.expectEqual(@as(u8, 0x7E), byte);
}

test "a length past the bound is oversize even when the bytes are all there" {
    const len = Blob.max_len.data + 1;
    const in = prefix(0) ++ prefix(len) ++ @as([len]u8, @splat(0xAB));
    try testing.expectError(error.Oversize, codec.decode(Blob, &in));
}

test "a length past the end of the input is truncated" {
    const in = prefix(0) ++ prefix(5) ++ @as([4]u8, @splat(0xAB));
    try testing.expectError(error.Truncated, codec.decode(Blob, &in));
}

test "a length of all ones is oversize, not an overflow" {
    const in = prefix(0) ++ prefix(0xFFFF_FFFF) ++ @as([4]u8, @splat(0xAB));
    try testing.expectError(error.Oversize, codec.decode(Blob, &in));
}

test "every strict prefix of a valid payload is truncated" {
    inline for (messages.cases) |case| {
        const payload = case.golden[header_bytes..];
        for (0..payload.len) |len| {
            const got = codec.decode(@TypeOf(case.value), payload[0..len]);
            try testing.expectError(error.Truncated, got);
        }
    }
}

test "a byte after the last field is trailing" {
    inline for (messages.cases) |case| {
        var in: [64]u8 = undefined;
        const payload = case.golden[header_bytes..];
        @memcpy(in[0..payload.len], payload);
        in[payload.len] = 0;
        const got = codec.decode(@TypeOf(case.value), in[0 .. payload.len + 1]);
        try testing.expectError(error.Trailing, got);
    }
}

test "an enum tag the type does not name is refused, at either width" {
    const Tagged = messages.Tagged;
    _ = try codec.decode(Tagged, &.{ 0x07, 0x5A, 0xA5 });
    try testing.expectError(error.BadTag, codec.decode(Tagged, &.{ 0x02, 0x5A, 0xA5 }));
    try testing.expectError(error.BadTag, codec.decode(Tagged, &.{ 0x07, 0xA5, 0x5A }));
}

test "a union is as wide as its tag and its widest field" {
    try testing.expectEqual(@as(usize, 2 + 1 + 4 + 8), codec.maxSize(messages.Reply));
}

test "a union tag the union does not name is refused" {
    const Reply = messages.Reply;
    _ = try codec.decode(Reply, &.{ 1, 0, 0x00 });
    try testing.expectError(error.BadTag, codec.decode(Reply, &.{ 1, 0, 0x02 }));
    try testing.expectError(error.BadTag, codec.decode(Reply, &.{ 1, 0, 0xFF }));
}

test "a union field's own rules still hold behind the tag" {
    const Reply = messages.Reply;
    const long = prefix(9) ++ @as([9]u8, @splat(0xAB));
    try testing.expectError(error.Oversize, codec.decode(Reply, &([_]u8{ 1, 0, 4 } ++ long)));
    try testing.expectError(error.Truncated, codec.decode(Reply, &.{ 1, 0, 1, 0xEF }));
    try testing.expectError(error.Trailing, codec.decode(Reply, &.{ 1, 0, 0, 0 }));

    const value: Reply = .{ .seq = 1, .body = .{ .text = "nine long" } };
    var out: [32]u8 = @splat(0x7E);
    try testing.expectError(error.Oversize, codec.encode(Reply, value, &out));
    for (out) |byte| try testing.expectEqual(@as(u8, 0x7E), byte);
}

test "decoded slices point into the input rather than a copy" {
    const in = prefix(9) ++ prefix(3) ++ [_]u8{ 1, 2, 3 };
    const value = try codec.decode(Blob, &in);
    try testing.expectEqual(@as(u32, 9), value.addr);
    try testing.expectEqual(@intFromPtr(&in[8]), @intFromPtr(value.data.ptr));
    try testing.expectEqual(@as(usize, 3), value.data.len);
}
