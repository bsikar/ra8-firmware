//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The frame header: what gets written, and what gets believed.

const std = @import("std");
const testing = std.testing;

const rpc = @import("ra8_rpc");
const frame = rpc.frame;
const Header = frame.Header;
const messages = @import("messages.zig");
const Blob = messages.Blob;

comptime {
    _ = @import("messages.zig");
    _ = @import("mock_queue.zig");
    _ = @import("mock_signal.zig");
    _ = @import("service.zig");
}

const blob: Blob = .{ .addr = 0x11223344, .data = "abc" };

test "the header is six bytes, the length first" {
    try testing.expectEqual(@as(usize, 6), Header.bytes);
    try testing.expectEqual(@as(usize, 0), Header.At.length);
    try testing.expectEqual(@as(usize, 4), Header.At.kind);
}

test "both header fields are little-endian" {
    var bytes: [Header.bytes]u8 = undefined;
    (Header{ .length = 0x04030201, .kind = 0x0605 }).write(&bytes);
    try testing.expectEqualSlices(u8, &.{ 1, 2, 3, 4, 5, 6 }, &bytes);

    const back = Header.read(&bytes);
    try testing.expectEqual(@as(u32, 0x04030201), back.length);
    try testing.expectEqual(@as(u16, 0x0605), back.kind);
}

test "the largest frame is the header plus the largest payload" {
    try testing.expectEqual(Header.bytes + rpc.codec.maxSize(Blob), frame.maxSize(Blob));
}

test "the length counts the payload and not the header" {
    var out: [frame.maxSize(Blob)]u8 = undefined;
    const bytes = try frame.encode(Blob, 9, blob, &out);
    const header = Header.read(bytes[0..Header.bytes]);
    try testing.expectEqual(bytes.len - Header.bytes, header.length);
    try testing.expectEqual(@as(u16, 9), header.kind);
}

test "a buffer too small for the header or the payload is refused and left alone" {
    var out: [32]u8 = @splat(0x7E);
    const need = Header.bytes + try rpc.codec.size(Blob, blob);
    for (0..need) |len| {
        try testing.expectError(error.NoSpace, frame.encode(Blob, 9, blob, out[0..len]));
    }
    for (out) |byte| try testing.expectEqual(@as(u8, 0x7E), byte);
    _ = try frame.encode(Blob, 9, blob, out[0..need]);
}

test "anything shorter than a header is truncated" {
    const bytes: [Header.bytes]u8 = @splat(0);
    for (0..Header.bytes) |len| {
        try testing.expectError(error.Truncated, frame.split(bytes[0..len], 0));
    }
    _ = try frame.split(&bytes, 0);
}

test "a length past the caller's limit is oversize before the body is looked at" {
    var bytes: [Header.bytes]u8 = undefined;
    (Header{ .length = 100, .kind = 1 }).write(&bytes);
    try testing.expectError(error.Oversize, frame.split(&bytes, 99));
    try testing.expectError(error.Truncated, frame.split(&bytes, 100));
}

test "a body shorter than the header claims is truncated" {
    var out: [frame.maxSize(Blob)]u8 = undefined;
    const bytes = try frame.encode(Blob, 9, blob, &out);
    for (Header.bytes..bytes.len) |len| {
        try testing.expectError(error.Truncated, frame.split(bytes[0..len], bytes.len));
    }
}

test "a length of all ones is refused, not wrapped" {
    var bytes: [Header.bytes + 4]u8 = undefined;
    (Header{ .length = 0xFFFF_FFFF, .kind = 1 }).write(bytes[0..Header.bytes]);
    try testing.expectError(error.Oversize, frame.split(&bytes, 64));
    try testing.expectError(error.Truncated, frame.split(&bytes, std.math.maxInt(usize)));
}

test "two frames back to back come off one at a time" {
    var out: [2 * frame.maxSize(Blob)]u8 = undefined;
    const first = try frame.encode(Blob, 1, blob, &out);
    const second = try frame.encode(Blob, 2, .{ .addr = 5, .data = "" }, out[first.len..]);
    const both = out[0 .. first.len + second.len];

    const one = try frame.split(both, rpc.codec.maxSize(Blob));
    try testing.expectEqual(@as(u16, 1), one.kind);
    try testing.expectEqualDeep(blob, try rpc.codec.decode(Blob, one.payload));

    const two = try frame.split(one.rest, rpc.codec.maxSize(Blob));
    try testing.expectEqual(@as(u16, 2), two.kind);
    try testing.expectEqual(@as(usize, 0), two.rest.len);
    try testing.expectError(error.Truncated, frame.split(two.rest, 0));
}
