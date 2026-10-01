//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Vectors for the StartRequest, NextRequest and CancelRequest encoders. Every expected
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

test "minimal start request matches the reference encoder" {
    var buf: [64]u8 = undefined;
    const got = try encode.start(&buf, .{ .url = "https://a.b/c" });
    try std.testing.expectEqualSlices(u8, &hex("0803120d68747470733a2f2f612e622f63"), got);
}

test "start request with every field matches the reference encoder" {
    var buf: [256]u8 = undefined;
    const got = try encode.start(&buf, .{
        .url = "https://example.com/x.cbz",
        .format = 1,
        .user_agent = "ra8/1",
        .referer = "https://r/",
        .if_none_match = "\"e1\"",
        .if_modified_since = "Wed, 21 Oct 2015 07:28:00 GMT",
        .timeout_ms = 60000,
    });
    const want = hex("0803121968747470733a2f2f6578616d706c652e636f6d2f782e63627a" ++
        "180122057261382f312a0a68747470733a2f2f722f3204226531223a1d" ++
        "5765642c203231204f637420323031352030373a32383a303020474d5440e0d403");
    try std.testing.expectEqualSlices(u8, &want, got);
}

test "the last format and a tiny timeout match the reference encoder" {
    var buf: [32]u8 = undefined;
    const got = try encode.start(&buf, .{ .url = "https://h/", .format = 8, .timeout_ms = 1 });
    try std.testing.expectEqualSlices(u8, &hex("0803120a68747470733a2f2f682f18084001"), got);
}

test "a 200-byte url takes a two-byte length prefix" {
    var buf: [256]u8 = undefined;
    const url = "https://" ++ "a" ** 192;
    const got = try encode.start(&buf, .{ .url = url });
    try std.testing.expectEqualSlices(u8, &hex("080312c801"), got[0..5]);
    try std.testing.expectEqualStrings(url, got[5..]);
    try std.testing.expectEqual(@as(usize, 205), got.len);
}

test "a string that does not fit is refused whole" {
    var buf: [12]u8 = undefined;
    try std.testing.expectError(error.NoSpace, encode.start(&buf, .{ .url = "https://a.b/c" }));
}

test "the worst-case start request fits the request buffer" {
    var buf: [implementation.mdl_issue.Bound.request_bytes_max]u8 = undefined;
    const got = try encode.start(&buf, .{
        .url = "u" ** 511,
        .format = 8,
        .user_agent = "a" ** 255,
        .referer = "r" ** 511,
        .if_none_match = "e" ** 127,
        .if_modified_since = "d" ** 63,
        .timeout_ms = 60000,
    });
    try std.testing.expect(got.len <= buf.len);
}
