//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Vectors for the NextRequest and CancelRequest encoders. Every expected
//! byte string was produced by the reference protobuf encoder (Python
//! google.protobuf over `proto/ra8_media_download.proto`), so these pin wire
//! compatibility with the C6 side, not just self-consistency.

const std = @import("std");
const implementation = @import("implementation");

const encode = implementation.mdl_encode;
const wire = implementation.mdl_wire;

fn hex(comptime text: []const u8) [text.len / 2]u8 {
    var out: [text.len / 2]u8 = undefined;
    _ = std.fmt.hexToBytes(&out, text) catch unreachable;
    return out;
}

test "next request matches the reference encoder" {
    var buf: [32]u8 = undefined;
    try std.testing.expectEqualSlices(u8, &hex("08031007208008"), try encode.next(&buf, 7, 0, 1024));
}

test "next request with wide job id and a 41-bit offset" {
    var buf: [32]u8 = undefined;
    const got = try encode.next(&buf, 0xFFFF_FFFF, (1 << 40) + 5, 65535);
    try std.testing.expectEqualSlices(u8, &hex("080310ffffffff0f1885808080802020ffff03"), got);
}

test "cancel request matches the reference encoder" {
    var buf: [16]u8 = undefined;
    try std.testing.expectEqualSlices(u8, &hex("080310ac02"), try encode.cancel(&buf, 300));
}

test "a zero field is left out, as proto3 requires" {
    var buf: [16]u8 = undefined;
    try std.testing.expectEqualSlices(u8, &hex("0803"), try encode.cancel(&buf, 0));
}

test "an encode that does not fit is refused, not truncated" {
    var buf: [4]u8 = undefined;
    try std.testing.expectError(error.NoSpace, encode.next(&buf, 7, 0, 1024));
    var empty: [0]u8 = .{};
    try std.testing.expectError(error.NoSpace, encode.cancel(&empty, 1));
}

test "varint boundaries" {
    var buf: [10]u8 = undefined;
    var w = wire.Writer.init(&buf);
    try w.varint(127);
    try std.testing.expectEqualSlices(u8, &.{0x7F}, w.written());
    w = wire.Writer.init(&buf);
    try w.varint(128);
    try std.testing.expectEqualSlices(u8, &.{ 0x80, 0x01 }, w.written());
    w = wire.Writer.init(&buf);
    try w.varint(std.math.maxInt(u64));
    try std.testing.expectEqual(@as(usize, 10), w.written().len);
    try std.testing.expectEqual(@as(u8, 0x01), w.written()[9]);
}

test "a tag packs the field number above the wire type" {
    var buf: [4]u8 = undefined;
    var w = wire.Writer.init(&buf);
    try w.tag(16, wire.Wire.len);
    try std.testing.expectEqualSlices(u8, &.{ 0x82, 0x01 }, w.written());
}
